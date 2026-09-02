#include "calibdialog.h"
#include "color.h"
#include "dialogbits.h"
#include "metrics.h"
#include "theme.h"

#include "../common/fxpreset.h"
#include "../engine/pitchtrack.h"
#include "../engine/voicefx.h"

#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>
#include <memory>

using namespace bb;

static constexpr int kSeconds = 8;
static constexpr int kRate    = 48000;

// One sentence with no baked line breaks. It used to carry a "\n" and a leading
// space to line the second line up, which lines up at exactly one font size.
static const char* kPhrase =
    "\"The rainbow is a division of white light into many beautiful colours. "
    "We were away a year ago, and the early bird catches the worm.\"";

static QLabel* cap(const QString& t, Qt::Alignment a = Qt::AlignLeft)
{
    auto* l = new QLabel(t);
    l->setProperty("role", "caption");
    l->setAlignment(a);
    return l;
}

static QLabel* prose(const QString& t)
{
    auto* l = new QLabel(t);
    l->setProperty("role", "prose");
    l->setWordWrap(true);
    return l;
}

// A group-box title is bold 11px sitting in the break in the border, which
// reads as a heading right up until it is also a 48-character sentence in
// capitals. Number and name, sentence case, and the separator becomes a tick
// once the step is behind you - so "done" needs no second widget to keep in
// step with the state.
static QString stepTitle(int n, const QString& what, bool done)
{
    return QString("STEP %1   %2   %3")
        .arg(n).arg(done ? QString::fromUtf8("✓") : QString::fromUtf8("·"), what);
}

static const char* kStep1 = "Read this aloud, at your normal speaking volume";
static const char* kStep2 = "What to aim for";
static const char* kStep3 = "Listen, adjust, repeat";

// Re-apply a dynamic property so the stylesheet picks it up. Qt does not
// re-evaluate selectors on a property change by itself.
static void restyle(QWidget* w, const char* name, const QVariant& v)
{
    if (w->property(name) == v) return;
    w->setProperty(name, v);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// Lock or unlock a step.
//
// setEnabled alone leaves the heading behind: the sheet writes an explicit
// colour into QGroupBox::title, and a subcontrol rule out-specifies the
// widget's own :disabled rule, so a locked step kept a fully lit title over
// greyed-out contents - plain to see on Catppuccin Latte. The ink is the same
// mix theme.cpp gives every other disabled thing, and a widget stylesheet is
// safe here where it would not be in the console: the dialog is modal, so no
// theme switch can happen underneath it.
static void gateStep(QGroupBox* box, bool live)
{
    box->setEnabled(live);
    const Theme& t = theme();
    box->setStyleSheet(live ? QString()
                            : QString("QGroupBox::title{color:%1;}")
                                  .arg(bbcolor::mix(dimOn(t, t.panel), t.panel, 0.45)
                                           .name(QColor::HexRgb)));
}

static QString scratchDir()
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
              + "/betterbanana-calib";
    QDir().mkpath(d);
    return d;
}

// ---------------------------------------------------------------------------
// The record row's input meter.
//
// LevelMeter is the console's instrument and it is vertical by construction:
// its ladder is punched every 1.5 dB, which across the forty-odd pixels a
// dialog row can spare comes out as a hatch rather than a scale. This is the
// same colour vocabulary laid on its side, with the held peak as a number,
// because the question being asked here is only "is the microphone working,
// and am I loud enough" - and it could not honestly answer more than that
// anyway: pw-record captures the device directly while this reads the engine's
// view of the strip, so the two need not even be the same signal.
//
// It runs from the moment the dialog opens. Reading a paragraph aloud for
// eight seconds and only then being told nothing came through is the failure
// this exists to prevent, and a meter that is already moving before the button
// is pressed prevents it earlier still.
// ---------------------------------------------------------------------------
class CalibLevelBar : public QWidget {
public:
    explicit CalibLevelBar(QWidget* parent = nullptr) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(bbui::px(14));
        setToolTip("This strip's input level, as the engine sees it.\n"
                   "The recording is taken from the device directly, so treat "
                   "this as a check that your voice is arriving.");
        m_clock.start();
        m_lastMs = m_clock.elapsed();
    }

    // `linear` is a peak in linear units; negative means the engine has stopped
    // publishing one, in which case the bar empties rather than freezing at
    // whatever it last showed.
    void setLevel(float linear)
    {
        const qint64 now = m_clock.elapsed();
        const double dt  = qBound(0.0, double(now - m_lastMs) / 1000.0, 0.25);
        m_lastMs = now;

        if (linear < 0.0f) {
            if (!m_idle) { m_idle = true; m_slow = m_hold = kMinDb; update(); }
            return;
        }

        bool dirty = m_idle;
        m_idle = false;

        // The console's ballistics, so a level that looks healthy here looks
        // the same on the strip meter: 26 dB/s release, a 1.4 s flat hold and
        // then 24 dB/s. In seconds rather than in ticks, so the poll rate is
        // free to differ from the main window's.
        constexpr double kRelease = 26.0, kHoldFall = 24.0, kHoldMs = 1400.0;

        const float db = linear <= 1e-7f ? kMinDb : 20.0f * std::log10(linear);
        if (db >= m_slow) { if (db > m_slow + 0.1f) dirty = true; m_slow = db; }
        else {
            m_slow = qMax(qMax(db, kMinDb), m_slow - float(kRelease * dt));
            dirty = true;
        }
        if (db >= m_hold) { m_hold = db; m_holdAt = now; dirty = true; }
        else if (now - m_holdAt > qint64(kHoldMs)) {
            m_hold = qMax(kMinDb, m_hold - float(kHoldFall * dt));
            dirty = true;
        }
        if (dirty) update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const Theme& t = theme();
        QPainter p(this);

        // Set here rather than in the constructor: qApp->setStyleSheet() drops
        // OpenType feature tags, so a tabular font applied once would stop
        // being tabular at the first theme switch and the number would start
        // shifting sideways as it counted.
        QFont f = font();
        f.setPixelSize(bbui::fsCaption());
        bbui::makeTabular(f);
        p.setFont(f);
        const QFontMetrics fm(f);

        // Wide enough for the longest thing the readout can say, so the bar
        // does not change length underneath the number.
        const int numW = qMax(fm.horizontalAdvance("-88.8 dBFS"),
                              fm.horizontalAdvance("engine idle")) + bbui::gapM();
        const QRect bar(0, 0, qMax(bbui::px(60), width() - numW), height());
        const int x0 = bar.left() + 1, span = qMax(1, bar.width() - 2);
        // The insets go through px() like every other measurement in the app.
        // The bar's height already scales; a 2px lit band inside the 28px bar
        // an interface scale of 2 gives would not have.
        const int inset = bbui::px(2), rung = bbui::px(3), tick = bbui::px(2);

        p.setPen(Qt::NoPen);
        p.setBrush(t.well);
        p.drawRoundedRect(bar, bbui::radWell(), bbui::radWell());

        // The same reference points the console's meter rules, minus the ones
        // that would land inside a rounded corner at this width.
        p.setPen(bbcolor::mix(t.well, t.text, 0.26));
        for (float db : { -40.0f, -30.0f, -20.0f, -12.0f, -6.0f }) {
            const int x = x0 + int(frac(db) * span);
            p.drawLine(x, bar.top() + rung, x, bar.bottom() - rung);
        }

        if (!m_idle) {
            const int lit = int(frac(m_slow) * span);
            if (lit > 0) {
                // Laid out across the whole bar, not across the lit part, so a
                // colour boundary always sits at the same dB.
                QLinearGradient g(x0, 0, x0 + span, 0);
                g.setColorAt(0.0,             t.meterLow);
                g.setColorAt(frac(-18.0f),    t.meterLow);
                g.setColorAt(frac(-6.0f),     t.meterMid);
                g.setColorAt(frac(-1.0f),     t.meterHigh);
                g.setColorAt(1.0,             t.meterPeak);
                p.fillRect(QRect(x0, bar.top() + inset, lit, bar.height() - 2 * inset), g);
            }
            if (m_hold > kMinDb) {
                const int x = x0 + qBound(0, int(frac(m_hold) * span), span - tick);
                p.fillRect(x, bar.top() + inset, tick, bar.height() - 2 * inset,
                           m_hold >= -0.5f ? t.meterPeak : t.meterHold);
            }
        }

        p.setPen(dimOn(t, t.bg));
        p.drawText(QRect(bar.right() + bbui::gapM(), 0,
                         width() - bar.right() - bbui::gapM(), height()),
                   Qt::AlignRight | Qt::AlignVCenter, readout());
    }

private:
    static constexpr float kMinDb = -60.0f, kMaxDb = 0.0f;

    static float frac(float db)
    {
        return qBound(0.0f, (db - kMinDb) / (kMaxDb - kMinDb), 1.0f);
    }

    QString readout() const
    {
        if (m_idle) return QStringLiteral("engine idle");
        if (m_hold <= kMinDb) return QStringLiteral("no signal");
        return QString::asprintf("%.1f dBFS", double(m_hold));
    }

    bool  m_idle = true;
    float m_slow = kMinDb, m_hold = kMinDb;
    QElapsedTimer m_clock;
    qint64 m_lastMs = 0, m_holdAt = 0;
};

// A 16-bit WAV, written by hand. pw-play goes through libsndfile and will not
// take a headerless stream, and one 44-byte header is a smaller price than a
// dependency in the GUI.
static bool write_wav(const QString& path, const QVector<float>& x, int rate)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const uint32_t data = uint32_t(x.size()) * 2;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + data); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1);
    u32(uint32_t(rate)); u32(uint32_t(rate) * 2); u16(2); u16(16);
    f.write("data", 4); u32(data);
    QByteArray pcm;
    pcm.resize(int(data));
    auto* p = reinterpret_cast<int16_t*>(pcm.data());
    for (int i = 0; i < x.size(); ++i) {
        const float v = std::clamp(x[i], -1.0f, 1.0f);
        p[i] = int16_t(std::lround(v * 32767.0f));
    }
    f.write(pcm);
    f.close();
    return true;
}

VoiceCalibDialog::VoiceCalibDialog(Shared* shm, int strip, const QString& device,
                                   const QString& title, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_strip(strip), m_device(device), m_title(title)
{
    setWindowTitle(title + " - calibrate the voice changer");
    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);

    root->addWidget(bbdlg::header(title,
        "Calibrate the voice changer. The right shift depends entirely on where "
        "your voice already sits, so this measures it rather than guessing."));

    // --- record ------------------------------------------------------------
    m_recBox = new QGroupBox(stepTitle(1, kStep1, false));
    auto* rg = new QVBoxLayout(m_recBox);
    rg->setSpacing(bbui::gapM());

    // The phrase is the one thing the user has to actually read, so it gets a
    // plate of its own and the display step. It used to be body prose set 1.5
    // points up, indistinguishable from the paragraph explaining it.
    auto* card = new QFrame;
    card->setProperty("role", "card");
    // Without this the rule paints nothing: Qt only sets WA_StyledBackground by
    // itself when metaObject() is QWidget's own, which QFrame's is not.
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* cv = new QVBoxLayout(card);
    cv->setContentsMargins(bbui::gapM(), bbui::gapM(), bbui::gapM(), bbui::gapM());
    m_phrase = new QLabel(kPhrase);
    m_phrase->setProperty("role", "display");
    m_phrase->setWordWrap(true);
    cv->addWidget(m_phrase);
    rg->addWidget(card);

    auto* rr = new QHBoxLayout;
    rr->setSpacing(bbui::gapM());
    m_record = new QPushButton(QString("Record %1 seconds").arg(kSeconds));
    connect(m_record, &QPushButton::clicked, this, &VoiceCalibDialog::startRecording);
    rr->addWidget(m_record);
    m_meter = new CalibLevelBar;
    rr->addWidget(m_meter, 1);
    rg->addLayout(rr);

    rg->addWidget(prose("Nothing is recorded until you press the button, and the "
                        "recording stays on this machine."));
    root->addWidget(m_recBox);

    // --- result ------------------------------------------------------------
    m_aimBox = new QGroupBox(stepTitle(2, kStep2, false));
    auto* g = new QGridLayout(m_aimBox);
    g->setHorizontalSpacing(bbui::gapM());
    g->setVerticalSpacing(bbui::gapM());
    m_result = new QLabel("Nothing measured yet.");
    m_result->setWordWrap(true);
    g->addWidget(m_result, 0, 0, 1, 7);

    // All three fields on one row, hard against the left edge under the
    // measurement they came from. They used to be a right-aligned mini-grid
    // with the explanatory prose beside them, which put a 200px gulf between
    // the two halves of one step.
    g->addWidget(cap("TARGET", Qt::AlignRight | Qt::AlignVCenter), 1, 0);
    m_target = new QDoubleSpinBox;
    m_target->setRange(120.0, 300.0);
    m_target->setDecimals(0);
    m_target->setSuffix(" Hz");
    m_target->setValue(195.0);
    // The suffix is not in the platform style's idea of the widget's width, so
    // "195 Hz" arrived with its leading digit cut off by the frame. A width is
    // safe to set: it is a background or a border that costs a spin box its
    // step arrows for good (theme.cpp documents that one).
    m_target->setMinimumWidth(bbui::px(88));
    m_target->setToolTip("Where you want to land. Adult female speech typically "
                         "sits around 190-220 Hz;\nnudging this is usually better "
                         "than fighting the semitone number directly.");
    connect(m_target, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!m_updating) retarget();
    });
    g->addWidget(m_target, 1, 1);

    g->addWidget(cap("PITCH", Qt::AlignRight | Qt::AlignVCenter), 1, 2);
    m_pitch = new QDoubleSpinBox;
    m_pitch->setRange(-12.0, 12.0);
    m_pitch->setDecimals(1);
    m_pitch->setSingleStep(0.5);
    m_pitch->setSuffix(" st");
    m_pitch->setMinimumWidth(bbui::px(84));
    g->addWidget(m_pitch, 1, 3);

    g->addWidget(cap("FORMANT", Qt::AlignRight | Qt::AlignVCenter), 1, 4);
    m_formant = new QDoubleSpinBox;
    m_formant->setRange(-12.0, 12.0);
    m_formant->setDecimals(1);
    m_formant->setSingleStep(0.5);
    m_formant->setSuffix(" st");
    m_formant->setValue(3.0);
    m_formant->setMinimumWidth(bbui::px(84));
    m_formant->setToolTip("Vocal tract size. Pitch can be measured; this one "
                          "cannot be, reliably,\nso it starts at a sensible +3 "
                          "and wants adjusting by ear.");
    g->addWidget(m_formant, 1, 5);
    g->setColumnStretch(6, 1);

    g->addWidget(prose("Formants are the part you have to judge by ear - a "
                       "measurement cannot pick them reliably off a live "
                       "microphone. Try ±1 semitone either side."), 2, 0, 1, 7);
    root->addWidget(m_aimBox);

    // --- listen ------------------------------------------------------------
    m_playBox = new QGroupBox(stepTitle(3, kStep3, false));
    auto* pv = new QVBoxLayout(m_playBox);
    pv->setSpacing(bbui::gapM());
    auto* pl = new QHBoxLayout;
    pl->setSpacing(bbui::gapM());
    m_playOrig = new QPushButton("Play what I said");
    m_playRes  = new QPushButton("Play it shifted");
    connect(m_playOrig, &QPushButton::clicked, this, &VoiceCalibDialog::playOriginal);
    connect(m_playRes,  &QPushButton::clicked, this, &VoiceCalibDialog::playResult);
    pl->addWidget(m_playOrig);
    pl->addWidget(m_playRes);
    pl->addStretch();
    pv->addLayout(pl);
    pv->addWidget(prose("Nudge TARGET above and play it again until it sounds "
                        "like you, then apply."));
    root->addWidget(m_playBox);
    root->addStretch();          // keep the three steps compact, not spread out

    m_status = new QLabel;
    m_status->setProperty("role", "value");
    m_status->setWordWrap(true);
    // Reserved whether or not it is saying anything, so the button row does not
    // jump down the moment it does.
    m_status->setMinimumHeight(bbui::px(14));
    root->addWidget(m_status);

    m_apply = new QPushButton("Apply to this strip");
    connect(m_apply, &QPushButton::clicked, this, &VoiceCalibDialog::applyToStrip);
    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    root->addLayout(bbdlg::buttonRow(m_apply, close));

    restage(m_device.isEmpty() ? "This strip has no capture device assigned."
                               : "Recording from " + m_device, false);

    // Apply is what the dialog is for; playback is a rehearsal. Return used to
    // replay the last take instead of committing it.
    bbdlg::tameDefaults(this, m_apply);

    // Polls two atomics and repaints one 14px bar. Nothing here waits on the
    // engine, and nothing here writes to it.
    m_levelTimer = new QTimer(this);
    connect(m_levelTimer, &QTimer::timeout, this, &VoiceCalibDialog::pollLevel);
    m_levelTimer->start(50);
    pollLevel();   // the bar's own initial state is "engine idle", and the first
                   // tick lands 50 ms after the dialog is already on screen

    // A word-wrapped QLabel reports one line as its minimum height, so a layout
    // full of them will happily let a window be dragged down to where three
    // paragraphs sit on top of each other. The floor is the natural size: this
    // dialog has nothing in it that can usefully be made smaller.
    resize(bbui::px(660), sizeHint().height());
    setMinimumSize(bbui::px(560), sizeHint().height());
    bbdlg::rememberGeometry(this, "calib");   // last, so a remembered size wins
}

VoiceCalibDialog::~VoiceCalibDialog()
{
    if (m_rec && m_rec->state() != QProcess::NotRunning) {
        m_rec->terminate();
        m_rec->waitForFinished(1500);
    }
    // The recording is the user's voice; it does not outlive the dialog.
    QFile::remove(scratchDir() + "/take.raw");
    QFile::remove(scratchDir() + "/preview.wav");
}

// Which steps are live follows from two facts and nothing else: whether there
// is a take, and whether a pitch came out of it. Step 3 opens on the take
// rather than on the measurement, because hearing what was captured is exactly
// what diagnoses a measurement that failed.
void VoiceCalibDialog::restage(const QString& status, bool busy)
{
    m_status->setText(status);

    const bool have     = !m_raw.isEmpty();
    const bool measured = m_measured > 0.0f;

    m_record->setEnabled(!busy && !m_device.isEmpty());
    m_recBox->setTitle(stepTitle(1, kStep1, measured));

    gateStep(m_aimBox, !busy && measured);
    gateStep(m_playBox, !busy && have);
    m_playRes->setEnabled(!busy && measured);

    // QPushButton[cta="primary"] is a lit accent fill and it out-specifies the
    // sheet's plain :disabled rule, so an Apply that can do nothing yet would
    // still be painted as the loudest thing in the dialog. It becomes the call
    // to action at the moment there is something to apply.
    const bool ready = !busy && measured;
    m_apply->setEnabled(ready);
    restyle(m_apply, "cta", ready ? QVariant("primary") : QVariant());
}

void VoiceCalibDialog::pollLevel()
{
    // A dead engine leaves the last level in shared memory for ever, and a bar
    // frozen at -18 dBFS is worse than no bar at all.
    const uint32_t hb = m_shm->engine_heartbeat.load(std::memory_order_relaxed);
    if (hb == m_lastHeartbeat) ++m_stallTicks; else m_stallTicks = 0;
    m_lastHeartbeat = hb;
    if (m_stallTicks > 20) { m_meter->setLevel(-1.0f); return; }   // a second

    // Pre-fader and post-input-gain, so the fader position does not change the
    // reading: closer to what the microphone is producing than the post-fader
    // level the console's own strip meter shows.
    float lin = 0.0f;
    for (int c = 0; c < kChan; ++c)
        lin = qMax(lin, m_shm->meters.strip_pre[m_strip][c].load(std::memory_order_relaxed));
    m_meter->setLevel(lin);
}

void VoiceCalibDialog::startRecording()
{
    const QString path = scratchDir() + "/take.raw";
    QFile::remove(path);

    // Raw float, not a WAV: there is no header to finalise, so stopping the
    // capture partway through still leaves a file that reads cleanly.
    m_rec = new QProcess(this);
    m_rec->start("pw-record", {
        "--target=" + m_device, "--rate=48000", "--channels=1",
        "--format=f32", "--container", "raw", path });
    if (!m_rec->waitForStarted(3000)) {
        restage("Could not start pw-record.", false);
        m_rec->deleteLater();
        m_rec = nullptr;
        return;
    }

    m_left = kSeconds;
    restage(QString("Recording... %1").arg(m_left), true);
    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, [this] {
        if (--m_left > 0) { m_status->setText(QString("Recording... %1").arg(m_left)); return; }
        m_tick->stop();
        finishRecording();
    });
    m_tick->start(1000);
}

void VoiceCalibDialog::finishRecording()
{
    if (!m_rec) return;
    m_rec->terminate();
    m_rec->waitForFinished(2000);
    if (m_rec->state() != QProcess::NotRunning) m_rec->kill();
    m_rec->deleteLater();
    m_rec = nullptr;

    QFile f(scratchDir() + "/take.raw");
    if (!f.open(QIODevice::ReadOnly)) {
        restage("Nothing was captured.", false);
        return;
    }
    const QByteArray blob = f.readAll();
    f.close();
    m_raw.resize(int(blob.size() / sizeof(float)));
    std::memcpy(m_raw.data(), blob.constData(), size_t(m_raw.size()) * sizeof(float));
    analyse();
}

void VoiceCalibDialog::analyse()
{
    if (m_raw.size() < kRate / 2) {
        restage("That was too short to measure.", false);
        return;
    }
    float peak = 0.0f;
    for (float v : m_raw) peak = std::max(peak, std::fabs(v));

    const PitchEstimate e = estimate_pitch(m_raw.constData(), m_raw.size(), float(kRate));
    if (!e.ok()) {
        m_measured = 0.0f;
        m_result->setText(peak < 0.01f
            ? "Almost nothing came through - check the strip's input device and gain, "
              "then try again."
            : "Could not find a steady pitch in that. Try again, speaking "
              "continuously rather than in short bursts.");
        restage("Measured nothing usable.", false);
        return;
    }

    m_measured = e.median_hz;
    m_result->setText(QString(
        "Your voice sits at <b>%1 Hz</b>  (%2-%3 Hz across %4 voiced frames of %5). "
        "Peak level %6 dBFS.")
        .arg(e.median_hz, 0, 'f', 0)
        .arg(e.low_hz, 0, 'f', 0).arg(e.high_hz, 0, 'f', 0)
        .arg(e.voiced).arg(e.frames)
        .arg(20.0 * std::log10(std::max(peak, 1e-5f)), 0, 'f', 1));
    retarget();
    restage("Measured. Now listen and adjust.", false);
}

void VoiceCalibDialog::retarget()
{
    if (m_measured <= 0.0f) return;
    m_updating = true;
    const float st = semitones_between(m_measured, float(m_target->value()));
    m_pitch->setValue(std::clamp(st, -12.0f, 12.0f));
    m_updating = false;
    if (std::fabs(st) > 12.0f)
        m_status->setText(QString("That target needs %1 semitones; the shifter "
                                  "stops at 12.").arg(st, 0, 'f', 1));
}

// Where the user will actually hear it: the device the first assigned physical
// bus drives, which is all but always the monitoring one. Falls back to
// whatever PipeWire calls default.
QString VoiceCalibDialog::playbackSink() const
{
    char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
    uint32_t seq = 0;
    for (int t = 0; t < 16; ++t)
        if (routing_read(m_shm->routing, seq, hw, out)) {
            for (int b = 0; b < kPhysBuses; ++b)
                if (out[b][0]) return QString::fromUtf8(out[b]);
            break;
        }
    return QString();
}

QVector<float> VoiceCalibDialog::processed() const
{
    VoiceFx p;
    fx_set_defaults(p);
    p.on.store(1);
    p.pitch.store(float(m_pitch->value()));
    p.formant_on.store(1);
    p.formant.store(float(m_formant->value()));

    // Heap: the chain carries the echo and formant buffers, far too much for
    // the stack.
    auto chain = std::make_unique<VoiceFxChain>();
    chain->configure(float(kRate));
    chain->update(p, float(kRate));

    QVector<float> out;
    out.reserve(m_raw.size());
    for (float v : m_raw) out.push_back(chain->process(0, v));
    return out;
}

bool VoiceCalibDialog::play(const QVector<float>& samples)
{
    const QString path = scratchDir() + "/preview.wav";
    if (!write_wav(path, samples, kRate)) {
        m_status->setText("Could not write the preview file.");
        return false;
    }
    QStringList args;
    const QString sink = playbackSink();
    if (!sink.isEmpty()) args << ("--target=" + sink);
    args << path;
    if (!QProcess::startDetached("pw-play", args)) {
        m_status->setText("Could not start pw-play.");
        return false;
    }
    m_status->setText(sink.isEmpty() ? "Playing to the default output."
                                     : "Playing to " + sink);
    return true;
}

void VoiceCalibDialog::playOriginal() { play(m_raw); }
void VoiceCalibDialog::playResult()   { play(processed()); }

void VoiceCalibDialog::applyToStrip()
{
    VoiceFx& p = m_shm->strip[m_strip].fx;
    p.pitch.store(float(m_pitch->value()));
    p.formant.store(float(m_formant->value()));
    p.formant_on.store(1);
    p.on.store(1);
    m_applied = true;
    accept();
}
