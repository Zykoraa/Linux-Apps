#include "fxdialog.h"
#include "color.h"
#include "dialogbits.h"
#include "knob.h"
#include "metrics.h"
#include "theme.h"

#include "calibdialog.h"

#include "../common/fxpreset.h"
#include "../engine/autotune.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

using namespace bb;

static QLabel* cap(const QString& text)
{
    auto* l = new QLabel(text);
    l->setProperty("role", "caption");
    l->setAlignment(Qt::AlignHCenter);
    return l;
}

// Explanatory text. Every paragraph in this dialog used to be a 9px caption
// carrying its own '\n' at whatever column the author's font happened to break
// on, so it could neither reflow nor be right at any other width or type scale.
// role="prose" is body-sized and the wrap does the line breaking.
static QLabel* prose(const QString& text)
{
    auto* l = new QLabel(text);
    l->setProperty("role", "prose");
    l->setWordWrap(true);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // A floor for the wrap, so a paragraph sharing a grid row with a knob does
    // not get squeezed into a column of single words.
    l->setMinimumWidth(bbui::px(190));
    return l;
}

// A section that is doing nothing looked exactly like one that is working:
// same border, same title colour, whether or not a single knob in it was off
// its factory position.
//
// The state is a stylesheet on the group box itself rather than a dynamic
// property, because the property route needs a matching selector inside
// buildStyleSheet() and this dialog does not own gui/theme.cpp. A widget's own
// sheet out-ranks the application's for the properties it names and inherits
// everything else, so border width, radius, margin-top and the title's
// geometry all still come from there - only the two colours move. The
// `engaged` property is memory rather than a selector: rebuilding and
// re-parsing a sheet on every step of a knob drag is exactly the cost the
// mixer window already avoids on its status line.
//
// Reading theme() once at the moment the state changes is safe here because
// the dialog is modal - the theme menu cannot be reached while it is open, so
// there is no stale sheet to re-apply.
static void setEngaged(QGroupBox* box, bool on)
{
    if (box->property("engaged").toBool() == on) return;
    box->setProperty("engaged", on);
    if (!on) { box->setStyleSheet(QString()); return; }

    const Theme& t = theme();
    auto c = [](const QColor& x) { return x.name(QColor::HexRgb); };
    box->setStyleSheet(
        QString("QGroupBox{border:1px solid %1;}QGroupBox::title{color:%2;}")
            .arg(c(bbcolor::ensureContrast(t.accent, t.bg, bbcolor::kBoundFloor)),
                 c(bbcolor::ensureContrast(t.accent, t.bg, bbcolor::kTextFloor))));
}

Knob* VoiceFxDialog::addKnob(QGridLayout* g, int row, int col, const QString& text,
                             int lo, int hi, int def, double scale, int decimals,
                             const QString& suffix, bool bipolar)
{
    auto* k = new Knob(lo, hi, def, bipolar, suffix);
    k->setScale(scale);
    k->setDecimals(decimals);
    // Slightly under the old uniform 54px. Everything in here except PITCH and
    // FORMANT is a garnish, and nineteen knobs at one size said otherwise.
    k->setFixedSize(bbui::px(46), bbui::px(54));
    g->addWidget(cap(text), row * 2, col);
    g->addWidget(k, row * 2 + 1, col, Qt::AlignHCenter);
    connect(k, &Knob::valueChanged, this, [this] {
        if (m_updating) return;
        push();
        refreshState();
    });
    return k;
}

VoiceFxDialog::VoiceFxDialog(Shared* shm, int strip, const QString& title, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_strip(strip), m_status(this)
{
    // VoiceCalibDialog recovers the strip's name by splitting on " - ", so the
    // separator is load-bearing.
    setWindowTitle(title + " - voice changer");

    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header(
        title, "Voice changer - sits after this strip's EQ and before its fader, so the EQ "
               "cleans the voice going in rather than the artefacts coming out. Presets are "
               "grouped: VOICE changes who you sound like, SINGING makes you sound good, "
               "CHARACTER and FUN are effects."));

    // --- preset bar --------------------------------------------------------
    auto* bar = new QHBoxLayout;
    bar->setSpacing(bbui::gapM());
    m_on = new QPushButton("FX ON");
    m_on->setCheckable(true);
    m_on->setFixedHeight(bbui::rowH());
    m_on->setProperty("role", "eq");
    connect(m_on, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.on.store(b ? 1 : 0);
    });
    bar->addWidget(m_on);
    bar->addWidget(cap("PRESET"));
    m_preset = new QComboBox;
    m_preset->setMinimumWidth(bbui::px(160));
    m_preset->addItem("(custom)", -1);
    QString group;
    for (int i = 0; i < (int)fx_presets().size(); ++i) {
        if (group != fx_presets()[i].group) {
            group = fx_presets()[i].group;
            m_preset->insertSeparator(m_preset->count());
        }
        m_preset->addItem(fx_presets()[i].name, i);
    }
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this] {
        if (m_updating) return;
        const int idx = m_preset->currentData().toInt();
        if (idx < 0) return;
        VoiceFx& p = m_shm->strip[m_strip].fx;
        fx_apply(p, fx_presets()[idx].v);
        // "Off" is how you clear it, so it must not also switch the block on.
        p.on.store(QString(fx_presets()[idx].name) == "Off" ? 0 : 1);
        pull();
        m_status.say(QString("applied \"%1\"").arg(fx_presets()[idx].name));
    });
    bar->addWidget(m_preset, 1);

    m_calib = new QPushButton("Calibrate to my voice...");
    m_calib->setToolTip("Record a few seconds, measure where your voice actually "
                        "sits,\nand work the shift out from there. A preset has to "
                        "guess.");
    connect(m_calib, &QPushButton::clicked, this, [this] {
        // The strip's own capture device, so the measurement is of the real
        // voice rather than of whatever the mix happens to be doing to it.
        QString dev;
        if (m_strip < kHwStrips) {
            char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
            uint32_t seq = 0;
            for (int t = 0; t < 16; ++t)
                if (routing_read(m_shm->routing, seq, hw, out)) {
                    dev = QString::fromUtf8(hw[m_strip]);
                    break;
                }
            if (dev.startsWith(kCablePrefix)) dev.clear();   // a cable is not a mic
        }
        VoiceCalibDialog dlg(m_shm, m_strip, dev, windowTitle().section(" - ", 0, 0), this);
        if (dlg.exec() == QDialog::Accepted && dlg.applied()) {
            pull();
            m_status.say("calibrated to your voice");
        }
    });
    bar->addWidget(m_calib);
    root->addLayout(bar);

    // The five sections used to stack straight into the dialog, which gave it a
    // layout minimum of 605x791 - so its own resize(660, 440) was clamped away
    // and it opened taller than a 1366x768 laptop screen, with OUTPUT off the
    // bottom and no way to reach it. Header, preset bar and buttons stay
    // pinned; only the sections scroll.
    auto* body = new QWidget;
    auto* col  = new QVBoxLayout(body);
    col->setContentsMargins(0, 0, bbui::gapXS(), 0);   // clear of the scrollbar
    col->setSpacing(bbui::gapM());

    // Knob defaults below are the factory values from fx_set_defaults() in
    // common/protocol.h, in raw knob units - the same scaling pull() applies.
    // Passing 0 for all of them is what made HOLD, ROOM, DAMPING, SPEED and
    // AMOUNT render accent-bold with a filled arc on a block where nothing was
    // switched on.

    // --- the voice itself: pitch and formants, the reason this dialog exists -
    //
    // The knobs and the paragraph sit in separate layouts rather than sharing a
    // grid: spanning the text across the knob rows made the tallest of them the
    // paragraph's, so the two knobs sank to the bottom of a 150px cell and the
    // captions naming them floated 90px above.
    m_voiceBox = new QGroupBox("VOICE");
    auto* voiceRow = new QHBoxLayout(m_voiceBox);
    voiceRow->setSpacing(bbui::gapL());
    auto* pg = new QGridLayout;
    pg->setHorizontalSpacing(bbui::gapM());
    pg->setVerticalSpacing(bbui::gapS());
    voiceRow->addLayout(pg);
    m_pitch = addKnob(pg, 0, 0, "PITCH", -120, 120, 0, 0.1, 1, " st", true);
    m_pitch->setToolTip("Zero is a true bypass.");

    m_formant = addKnob(pg, 0, 1, "FORMANT", -120, 120, 0, 0.1, 1, " st", true);
    m_formant->setToolTip("Where your vocal tract resonates - what a listener "
                          "hears as body size.\nThis is the NET shift: it already "
                          "accounts for what pitch is doing.");
    // These two are the dialog's subject and they were the same 54px as ECHO
    // MIX. Knob paints from width()/height(), so a fixed size is the whole
    // change.
    for (Knob* k : { m_pitch, m_formant }) k->setFixedSize(bbui::px(86), bbui::px(96));

    m_fmtOn = new QPushButton("SEPARATE");
    m_fmtOn->setCheckable(true);
    m_fmtOn->setFixedHeight(bbui::rowH());
    m_fmtOn->setProperty("role", "eq");
    m_fmtOn->setToolTip("Off: formants ride along with pitch - a chipmunk, a "
                        "giant.\nOn: they move on their own, which is what makes "
                        "a voice sound\nlike a different person rather than a "
                        "different size.");
    connect(m_fmtOn, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.formant_on.store(b ? 1 : 0);
        m_formant->setEnabled(b);
        refreshState();
    });
    pg->addWidget(m_fmtOn, 2, 1, Qt::AlignHCenter);
    // The paragraph beside these knobs is the taller of the two columns below
    // about 640px wide, and QGridLayout hands surplus height to every row
    // equally - so at the dialog's own minimum width the captions ended up
    // 55px above the knobs they name. An empty stretch row takes the surplus.
    pg->setRowStretch(3, 1);

    auto* notes = new QVBoxLayout;
    notes->setSpacing(bbui::gapS());
    notes->addWidget(prose("Pitch alone is tape speed: it moves the formants too, so a big "
                           "shift sounds like a smaller person rather than a different one. "
                           "Turn on SEPARATE and set both."));
    notes->addWidget(prose("Costs about 20 ms of delay, and another 20 with SEPARATE on. "
                           "Your listeners will not notice; you will, so monitor through "
                           "your interface rather than through this."));
    notes->addStretch(1);
    voiceRow->addLayout(notes, 1);
    col->addWidget(m_voiceBox);

    // --- character ---------------------------------------------------------
    m_charBox = new QGroupBox("CHARACTER");
    auto* cg = new QGridLayout(m_charBox);
    cg->setVerticalSpacing(bbui::gapS());
    m_drive   = addKnob(cg, 0, 0, "DRIVE",    0, 100,  0, 0.1, 1, "");
    m_ringHz  = addKnob(cg, 0, 1, "RING",     0, 2000, 0, 1.0, 0, " Hz");
    m_ringMix = addKnob(cg, 0, 2, "RING MIX", 0, 100,  0, 1.0, 0, " %");
    m_bits    = addKnob(cg, 0, 3, "BITS",     0, 15,   0, 1.0, 0, "");
    m_down    = addKnob(cg, 0, 4, "HOLD",     1, 32,   1, 1.0, 0, "x");
    m_ringHz->setToolTip("Ring modulator - the robot voice. 0 Hz is off.");
    m_bits->setToolTip("Bit-crush depth. 0 is off; 4-8 is where it gets gritty.");
    m_down->setToolTip("Sample-and-hold factor: the whine of an old sampler. 1 is off.");
    col->addWidget(m_charBox);

    // --- space -------------------------------------------------------------
    m_spaceBox = new QGroupBox("SPACE");
    auto* sg = new QGridLayout(m_spaceBox);
    sg->setVerticalSpacing(bbui::gapS());
    m_chMs   = addKnob(sg, 0, 0, "CHORUS",   0, 120,  0,  0.1,  1, " ms");
    m_chHz   = addKnob(sg, 0, 1, "CH RATE",  0, 800,  0,  0.01, 2, " Hz");
    m_chMix  = addKnob(sg, 0, 2, "CH MIX",   0, 100,  0,  1.0,  0, " %");
    m_echoMs = addKnob(sg, 0, 3, "ECHO",     0, 1000, 0,  1.0,  0, " ms");
    m_echoFb = addKnob(sg, 0, 4, "FEEDBACK", 0, 95,   0,  1.0,  0, " %");
    m_echoMix= addKnob(sg, 0, 5, "ECHO MIX", 0, 100,  0,  1.0,  0, " %");
    m_rvSize = addKnob(sg, 1, 0, "ROOM",     0, 100,  50, 1.0,  0, " %");
    m_rvDamp = addKnob(sg, 1, 1, "DAMPING",  0, 100,  50, 1.0,  0, " %");
    m_rvMix  = addKnob(sg, 1, 2, "REVERB",   0, 100,  0,  1.0,  0, " %");
    m_rvSize->setToolTip("How big the room is: about 0.7 s of tail at 20%, "
                         "2.6 s at 85%.");
    m_rvDamp->setToolTip("How fast the tail loses its highs, the way a room "
                         "full of soft things does.");
    m_rvMix->setToolTip("0% is off entirely. Reverb is the effect that does most "
                        "of the work\nof making a voice sound good rather than "
                        "sound like someone else.");
    col->addWidget(m_spaceBox);

    // --- pitch correction ---------------------------------------------------
    m_tuneBox = new QGroupBox("PITCH CORRECTION");
    auto* tg = new QGridLayout(m_tuneBox);
    tg->setVerticalSpacing(bbui::gapS());
    m_tuneOn = new QPushButton("TUNE");
    m_tuneOn->setCheckable(true);
    m_tuneOn->setFixedHeight(bbui::rowH());
    m_tuneOn->setProperty("role", "eq");
    m_tuneOn->setToolTip("Snaps what you sing to the nearest note. Uses the same "
                         "pitch tracker\nthe shifter already runs, so it costs "
                         "almost nothing extra.");
    connect(m_tuneOn, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.tune_on.store(b ? 1 : 0);
        refreshState();
    });
    tg->addWidget(m_tuneOn, 1, 0);

    m_tuneSpeed  = addKnob(tg, 0, 1, "SPEED",  0, 400, 40,  1.0, 0, " ms");
    m_tuneAmount = addKnob(tg, 0, 2, "AMOUNT", 0, 100, 100, 1.0, 0, " %");
    m_tuneSpeed->setToolTip("How fast it pulls you onto the note. 0 ms is the "
                            "instant snap\neveryone recognises; 100-200 ms is "
                            "correction you do not hear.");
    m_tuneAmount->setToolTip("How much of the error to take out. Less than 100% "
                             "leaves some\nof your own intonation in.");

    tg->addWidget(cap("KEY"), 0, 3);
    m_key = new QComboBox;
    for (int i = 0; i < 12; ++i) m_key->addItem(tune_note_name(i));
    connect(m_key, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.tune_key.store(i);
        refreshState();
    });
    tg->addWidget(m_key, 1, 3);

    tg->addWidget(cap("SCALE"), 0, 4);
    m_scale = new QComboBox;
    m_scale->addItem("Chromatic");
    m_scale->addItem("Major");
    m_scale->addItem("Minor");
    m_scale->setToolTip("Chromatic snaps to any semitone. A key and scale refuse "
                        "the notes\nthat are not in it, which is what keeps a "
                        "correction musical.");
    connect(m_scale, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.tune_scale.store(i);
        refreshState();
    });
    tg->addWidget(m_scale, 1, 4);
    tg->setColumnStretch(5, 1);
    col->addWidget(m_tuneBox);

    // --- output ------------------------------------------------------------
    m_outBox = new QGroupBox("OUTPUT");
    auto* outRow = new QHBoxLayout(m_outBox);
    outRow->setSpacing(bbui::gapL());
    auto* og = new QGridLayout;
    og->setVerticalSpacing(bbui::gapS());
    outRow->addLayout(og);
    m_gain = addKnob(og, 0, 0, "GAIN", -240, 240, 0, 0.1, 1, " dB", true);
    og->setRowStretch(2, 1);         // same as VOICE: surplus below, not between
    m_gain->setToolTip("Makeup: most of these change the level as well as the sound.");
    // Deliberate: a telephone or megaphone voice lives in the EQ's profile list
    // so that applying an FX preset here can never rewrite a curve the user
    // tuned. The pointer to it stays.
    outRow->addWidget(prose("A telephone, radio, megaphone or walkie-talkie voice is EQ "
                            "rather than an effect. Open this strip's EQ and pick one from "
                            "its profile list - they live there so that a preset here never "
                            "rewrites a curve you tuned."),
                      1);
    col->addWidget(m_outBox);
    col->addStretch(1);

    auto* sc = new QScrollArea;
    sc->setWidget(body);
    sc->setWidgetResizable(true);
    sc->setFrameShape(QFrame::NoFrame);
    root->addWidget(sc, 1);

    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* foot = bbdlg::buttonRow(nullptr, close);
    foot->insertWidget(0, m_status.widget(), 1);
    root->addLayout(foot);

    pull();
    // Close asked to be the default button, so Return dismissed the dialog from
    // anywhere in it - including out of a combo box you were arrowing through.
    // Escape already closes a QDialog, which is the right affordance here.
    bbdlg::tameDefaults(this);

    // Now that the sections scroll, resize() is no longer clamped up to a
    // layout minimum, so this is the size the dialog actually opens at. It has
    // to fit a 768px-tall laptop with a panel on it.
    const QScreen* s = QGuiApplication::primaryScreen();
    const int avail = s ? s->availableGeometry().height() : 700;
    resize(bbui::px(720), std::min(bbui::px(880), avail - bbui::px(80)));
}

void VoiceFxDialog::pull()
{
    m_updating = true;
    const VoiceFx& p = m_shm->strip[m_strip].fx;
    m_on->setChecked(p.on.load() != 0);
    m_pitch->setValue(int(std::lround(p.pitch.load() * 10)));
    m_formant->setValue(int(std::lround(p.formant.load() * 10)));
    const bool fon = p.formant_on.load() != 0;
    m_fmtOn->setChecked(fon);
    m_formant->setEnabled(fon);
    m_drive->setValue(int(std::lround(p.drive.load() * 10)));
    m_ringHz->setValue(int(std::lround(p.ring_hz.load())));
    m_ringMix->setValue(int(std::lround(p.ring_mix.load() * 100)));
    m_bits->setValue(p.bits.load());
    m_down->setValue(p.downsample.load());
    m_chMs->setValue(int(std::lround(p.chorus_ms.load() * 10)));
    m_chHz->setValue(int(std::lround(p.chorus_hz.load() * 100)));
    m_chMix->setValue(int(std::lround(p.chorus_mix.load() * 100)));
    m_echoMs->setValue(int(std::lround(p.echo_ms.load())));
    m_echoFb->setValue(int(std::lround(p.echo_fb.load() * 100)));
    m_echoMix->setValue(int(std::lround(p.echo_mix.load() * 100)));
    m_rvSize->setValue(int(std::lround(p.reverb_size.load() * 100)));
    m_rvDamp->setValue(int(std::lround(p.reverb_damp.load() * 100)));
    m_rvMix->setValue(int(std::lround(p.reverb_mix.load() * 100)));
    m_tuneOn->setChecked(p.tune_on.load() != 0);
    m_tuneSpeed->setValue(int(std::lround(p.tune_speed_ms.load())));
    m_tuneAmount->setValue(int(std::lround(p.tune_amount.load() * 100)));
    m_key->setCurrentIndex(std::clamp<int>(p.tune_key.load(), 0, 11));
    m_scale->setCurrentIndex(std::clamp<int>(p.tune_scale.load(), 0, 2));
    m_gain->setValue(int(std::lround(p.gain_db.load() * 10)));
    m_updating = false;
    refreshState();
}

void VoiceFxDialog::push()
{
    FxValues v;
    v.pitch      = m_pitch->value() / 10.0f;
    v.formant_on = m_fmtOn->isChecked();
    v.formant    = m_formant->value() / 10.0f;
    v.drive      = m_drive->value() / 10.0f;
    v.ring_hz    = float(m_ringHz->value());
    v.ring_mix   = m_ringMix->value() / 100.0f;
    v.bits       = m_bits->value();
    v.downsample = m_down->value();
    v.chorus_ms  = m_chMs->value() / 10.0f;
    v.chorus_hz  = m_chHz->value() / 100.0f;
    v.chorus_mix = m_chMix->value() / 100.0f;
    v.echo_ms    = float(m_echoMs->value());
    v.echo_fb    = m_echoFb->value() / 100.0f;
    v.echo_mix   = m_echoMix->value() / 100.0f;
    v.reverb_size = m_rvSize->value() / 100.0f;
    v.reverb_damp = m_rvDamp->value() / 100.0f;
    v.reverb_mix  = m_rvMix->value() / 100.0f;
    v.tune_on     = m_tuneOn->isChecked();
    v.tune_speed_ms = float(m_tuneSpeed->value());
    v.tune_amount = m_tuneAmount->value() / 100.0f;
    v.tune_key    = m_key->currentIndex();
    v.tune_scale  = m_scale->currentIndex();
    v.gain_db    = m_gain->value() / 10.0f;
    fx_apply(m_shm->strip[m_strip].fx, v);
}

// The two readouts that describe the block as a whole rather than one control.
void VoiceFxDialog::refreshState()
{
    refreshPresetCombo();
    refreshEngaged();
}

// Shows which preset the knobs currently amount to, or "(custom)" once they
// have been moved off one.
void VoiceFxDialog::refreshPresetCombo()
{
    const QSignalBlocker block(m_preset);
    const int idx = fx_preset_index(fx_capture(m_shm->strip[m_strip].fx));
    const int at = m_preset->findData(idx);
    m_preset->setCurrentIndex(at >= 0 ? at : 0);
}

// Which sections are audibly doing something. Read off the values rather than
// off Knob's own "moved from default" flag: engaged is not the same as moved.
// ROOM and DAMPING sit at 50% in a block with the reverb entirely bypassed, and
// a ring modulator at 800 Hz with its mix at zero is silent - each of these
// effects has one control that gates it, and that is the one to test.
void VoiceFxDialog::refreshEngaged()
{
    const VoiceFx& p = m_shm->strip[m_strip].fx;
    setEngaged(m_voiceBox, p.pitch.load() != 0.0f
                           || (p.formant_on.load() != 0 && p.formant.load() != 0.0f));
    setEngaged(m_charBox,  p.drive.load() > 0.0f || p.ring_mix.load() > 0.0f
                           || p.bits.load() > 0 || p.downsample.load() > 1);
    setEngaged(m_spaceBox, p.chorus_mix.load() > 0.0f || p.echo_mix.load() > 0.0f
                           || p.reverb_mix.load() > 0.0f);
    setEngaged(m_tuneBox,  p.tune_on.load() != 0);
    setEngaged(m_outBox,   p.gain_db.load() != 0.0f);
}
