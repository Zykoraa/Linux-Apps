#include "calibdialog.h"
#include "theme.h"

#include "../common/fxpreset.h"
#include "../engine/pitchtrack.h"
#include "../engine/voicefx.h"

#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <memory>

using namespace bb;

static constexpr int kSeconds = 8;
static constexpr int kRate    = 48000;

static const char* kPhrase =
    "\"The rainbow is a division of white light into many beautiful colours.\n"
    " We were away a year ago, and the early bird catches the worm.\"";

static QLabel* cap(const QString& t, Qt::Alignment a = Qt::AlignLeft)
{
    auto* l = new QLabel(t);
    l->setProperty("role", "caption");
    l->setAlignment(a);
    return l;
}

static QString scratchDir()
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
              + "/betterbanana-calib";
    QDir().mkpath(d);
    return d;
}

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

    root->addWidget(new QLabel(
        "The right shift depends entirely on where your voice already sits, so "
        "this measures it\nrather than guessing. Nothing is recorded until you "
        "press the button, and the recording\nstays on this machine."));

    // --- record ------------------------------------------------------------
    auto* recBox = new QGroupBox("1.  READ THIS ALOUD, AT YOUR NORMAL SPEAKING VOLUME");
    auto* rg = new QVBoxLayout(recBox);
    m_phrase = new QLabel(kPhrase);
    QFont pf = m_phrase->font();
    pf.setPointSizeF(pf.pointSizeF() + 1.5);
    m_phrase->setFont(pf);
    rg->addWidget(m_phrase);

    auto* rr = new QHBoxLayout;
    m_record = new QPushButton(QString("Record %1 seconds").arg(kSeconds));
    m_record->setFixedHeight(26);
    connect(m_record, &QPushButton::clicked, this, &VoiceCalibDialog::startRecording);
    rr->addWidget(m_record);
    m_status = cap("");
    rr->addWidget(m_status, 1);
    rg->addLayout(rr);
    if (m_device.isEmpty()) {
        m_record->setEnabled(false);
        m_status->setText("This strip has no capture device assigned.");
    } else {
        m_status->setText("Recording from " + m_device);
    }
    root->addWidget(recBox);

    // --- result ------------------------------------------------------------
    auto* resBox = new QGroupBox("2.  WHAT TO AIM FOR");
    auto* g = new QGridLayout(resBox);
    m_result = cap("Nothing measured yet.");
    g->addWidget(m_result, 0, 0, 1, 4);

    g->addWidget(cap("TARGET", Qt::AlignRight), 1, 0);
    m_target = new QDoubleSpinBox;
    m_target->setRange(120.0, 300.0);
    m_target->setDecimals(0);
    m_target->setSuffix(" Hz");
    m_target->setValue(195.0);
    m_target->setToolTip("Where you want to land. Adult female speech typically "
                         "sits around 190-220 Hz;\nnudging this is usually better "
                         "than fighting the semitone number directly.");
    connect(m_target, &QDoubleSpinBox::valueChanged, this, [this] {
        if (!m_updating) retarget();
    });
    g->addWidget(m_target, 1, 1);

    g->addWidget(cap("PITCH", Qt::AlignRight), 1, 2);
    m_pitch = new QDoubleSpinBox;
    m_pitch->setRange(-12.0, 12.0);
    m_pitch->setDecimals(1);
    m_pitch->setSingleStep(0.5);
    m_pitch->setSuffix(" st");
    g->addWidget(m_pitch, 1, 3);

    g->addWidget(cap("FORMANT", Qt::AlignRight), 2, 2);
    m_formant = new QDoubleSpinBox;
    m_formant->setRange(-12.0, 12.0);
    m_formant->setDecimals(1);
    m_formant->setSingleStep(0.5);
    m_formant->setSuffix(" st");
    m_formant->setValue(3.0);
    m_formant->setToolTip("Vocal tract size. Pitch can be measured; this one "
                          "cannot be, reliably,\nso it starts at a sensible +3 "
                          "and wants adjusting by ear.");
    g->addWidget(m_formant, 2, 3);
    g->addWidget(cap("Formants are the part you have to judge by ear - a "
                     "measurement cannot pick them\nreliably off a live "
                     "microphone. Try ±1 semitone either side.",
                     Qt::AlignLeft), 2, 0, 1, 2);
    g->setColumnStretch(0, 1);
    root->addWidget(resBox);

    // --- listen ------------------------------------------------------------
    auto* playBox = new QGroupBox("3.  LISTEN, ADJUST, REPEAT");
    auto* pl = new QHBoxLayout(playBox);
    m_playOrig = new QPushButton("Play what I said");
    m_playRes  = new QPushButton("Play it shifted");
    m_playRes->setDefault(true);
    connect(m_playOrig, &QPushButton::clicked, this, &VoiceCalibDialog::playOriginal);
    connect(m_playRes,  &QPushButton::clicked, this, &VoiceCalibDialog::playResult);
    pl->addWidget(m_playOrig);
    pl->addWidget(m_playRes);
    pl->addStretch();
    root->addWidget(playBox);
    root->addStretch();          // keep the three steps compact, not spread out

    auto* btns = new QHBoxLayout;
    m_apply = new QPushButton("Apply to this strip");
    connect(m_apply, &QPushButton::clicked, this, &VoiceCalibDialog::applyToStrip);
    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    btns->addStretch();
    btns->addWidget(m_apply);
    btns->addWidget(close);
    root->addLayout(btns);

    m_playOrig->setEnabled(false);
    m_playRes->setEnabled(false);
    m_apply->setEnabled(false);
    resize(640, 520);
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

void VoiceCalibDialog::setBusy(const QString& what, bool busy)
{
    m_status->setText(what);
    m_record->setEnabled(!busy && !m_device.isEmpty());
    const bool have = !m_raw.isEmpty();
    m_playOrig->setEnabled(!busy && have);
    m_playRes->setEnabled(!busy && have);
    m_apply->setEnabled(!busy && have);
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
        setBusy("Could not start pw-record.", false);
        m_rec->deleteLater();
        m_rec = nullptr;
        return;
    }

    m_left = kSeconds;
    setBusy(QString("Recording... %1").arg(m_left), true);
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
        setBusy("Nothing was captured.", false);
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
        setBusy("That was too short to measure.", false);
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
        setBusy("Measured nothing usable.", false);
        m_apply->setEnabled(false);
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
    setBusy("Measured. Now listen and adjust.", false);
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
