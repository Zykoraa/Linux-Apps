#include "fxdialog.h"
#include "knob.h"
#include "theme.h"

#include "calibdialog.h"

#include "../common/fxpreset.h"
#include "../engine/autotune.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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

Knob* VoiceFxDialog::addKnob(QGridLayout* g, int row, int col, const QString& text,
                             int lo, int hi, double scale, int decimals,
                             const QString& suffix, bool bipolar)
{
    auto* k = new Knob(lo, hi, 0, bipolar, suffix);
    k->setScale(scale);
    k->setDecimals(decimals);
    k->setMinimumWidth(54);
    g->addWidget(cap(text), row * 2, col);
    g->addWidget(k, row * 2 + 1, col, Qt::AlignHCenter);
    connect(k, &Knob::valueChanged, this, [this] {
        if (m_updating) return;
        push();
        refreshPresetCombo();
    });
    return k;
}

VoiceFxDialog::VoiceFxDialog(Shared* shm, int strip, const QString& title, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_strip(strip)
{
    setWindowTitle(title + " - voice changer");
    auto* root = new QVBoxLayout(this);

    root->addWidget(new QLabel(
        "Sits after this strip's EQ and before its fader, so the EQ cleans the "
        "voice going in\nrather than the artefacts coming out.  Presets are "
        "grouped: VOICE changes who you sound\nlike, SINGING makes you sound "
        "good, CHARACTER and FUN are effects."));

    // --- preset bar --------------------------------------------------------
    auto* bar = new QHBoxLayout;
    m_on = new QPushButton("FX ON");
    m_on->setCheckable(true);
    m_on->setFixedHeight(24);
    m_on->setProperty("role", "eq");
    connect(m_on, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.on.store(b ? 1 : 0);
    });
    bar->addWidget(m_on);
    bar->addSpacing(8);
    bar->addWidget(cap("PRESET"));
    m_preset = new QComboBox;
    m_preset->setMinimumWidth(160);
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
        m_note->setText(QString("applied \"%1\"").arg(fx_presets()[idx].name));
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
            m_note->setText("calibrated to your voice");
        }
    });
    bar->addWidget(m_calib);
    root->addLayout(bar);

    // --- the voice itself: pitch and formants, the reason this dialog exists -
    auto* pitchBox = new QGroupBox("VOICE");
    auto* pg = new QGridLayout(pitchBox);
    m_pitch = addKnob(pg, 0, 0, "PITCH", -120, 120, 0.1, 1, " st", true);
    m_pitch->setToolTip("Zero is a true bypass.");

    m_formant = addKnob(pg, 0, 1, "FORMANT", -120, 120, 0.1, 1, " st", true);
    m_formant->setToolTip("Where your vocal tract resonates - what a listener "
                          "hears as body size.\nThis is the NET shift: it already "
                          "accounts for what pitch is doing.");

    m_fmtOn = new QPushButton("SEPARATE");
    m_fmtOn->setCheckable(true);
    m_fmtOn->setFixedHeight(20);
    m_fmtOn->setProperty("role", "eq");
    m_fmtOn->setToolTip("Off: formants ride along with pitch - a chipmunk, a "
                        "giant.\nOn: they move on their own, which is what makes "
                        "a voice sound\nlike a different person rather than a "
                        "different size.");
    connect(m_fmtOn, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.formant_on.store(b ? 1 : 0);
        m_formant->setEnabled(b);
        refreshPresetCombo();
    });
    pg->addWidget(m_fmtOn, 2, 1);

    auto* pn = new QLabel(
        "Pitch alone is tape speed: it moves the formants too, so a big shift "
        "sounds like a\nsmaller person rather than a different one. Turn on "
        "SEPARATE and set both.\n\n"
        "Costs about 20 ms of delay, and another 20 with SEPARATE on. Your "
        "listeners will\nnot notice; you will, so monitor through your "
        "interface rather than through this.");
    pn->setProperty("role", "caption");
    pg->addWidget(pn, 0, 2, 3, 1);
    pg->setColumnStretch(2, 1);
    root->addWidget(pitchBox);

    // --- character ---------------------------------------------------------
    auto* charBox = new QGroupBox("CHARACTER");
    auto* cg = new QGridLayout(charBox);
    m_drive   = addKnob(cg, 0, 0, "DRIVE",   0, 100, 0.1, 1, "");
    m_ringHz  = addKnob(cg, 0, 1, "RING",    0, 2000, 1.0, 0, " Hz");
    m_ringMix = addKnob(cg, 0, 2, "RING MIX", 0, 100, 1.0, 0, " %");
    m_bits    = addKnob(cg, 0, 3, "BITS",    0, 15, 1.0, 0, "");
    m_down    = addKnob(cg, 0, 4, "HOLD",    1, 32, 1.0, 0, "x");
    m_ringHz->setToolTip("Ring modulator - the robot voice. 0 Hz is off.");
    m_bits->setToolTip("Bit-crush depth. 0 is off; 4-8 is where it gets gritty.");
    m_down->setToolTip("Sample-and-hold factor: the whine of an old sampler. 1 is off.");
    root->addWidget(charBox);

    // --- space -------------------------------------------------------------
    auto* spaceBox = new QGroupBox("SPACE");
    auto* sg = new QGridLayout(spaceBox);
    m_chMs   = addKnob(sg, 0, 0, "CHORUS",   0, 120, 0.1, 1, " ms");
    m_chHz   = addKnob(sg, 0, 1, "CH RATE",  0, 800, 0.01, 2, " Hz");
    m_chMix  = addKnob(sg, 0, 2, "CH MIX",   0, 100, 1.0, 0, " %");
    m_echoMs = addKnob(sg, 0, 3, "ECHO",     0, 1000, 1.0, 0, " ms");
    m_echoFb = addKnob(sg, 0, 4, "FEEDBACK", 0, 95, 1.0, 0, " %");
    m_echoMix= addKnob(sg, 0, 5, "ECHO MIX", 0, 100, 1.0, 0, " %");
    m_rvSize = addKnob(sg, 1, 0, "ROOM",     0, 100, 1.0, 0, " %");
    m_rvDamp = addKnob(sg, 1, 1, "DAMPING",  0, 100, 1.0, 0, " %");
    m_rvMix  = addKnob(sg, 1, 2, "REVERB",   0, 100, 1.0, 0, " %");
    m_rvSize->setToolTip("How big the room is: about 0.7 s of tail at 20%, "
                         "2.6 s at 85%.");
    m_rvDamp->setToolTip("How fast the tail loses its highs, the way a room "
                         "full of soft things does.");
    m_rvMix->setToolTip("0% is off entirely. Reverb is the effect that does most "
                        "of the work\nof making a voice sound good rather than "
                        "sound like someone else.");
    root->addWidget(spaceBox);

    // --- pitch correction ---------------------------------------------------
    auto* tuneBox = new QGroupBox("PITCH CORRECTION");
    auto* tg = new QGridLayout(tuneBox);
    m_tuneOn = new QPushButton("TUNE");
    m_tuneOn->setCheckable(true);
    m_tuneOn->setFixedHeight(22);
    m_tuneOn->setProperty("role", "eq");
    m_tuneOn->setToolTip("Snaps what you sing to the nearest note. Uses the same "
                         "pitch tracker\nthe shifter already runs, so it costs "
                         "almost nothing extra.");
    connect(m_tuneOn, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_shm->strip[m_strip].fx.tune_on.store(b ? 1 : 0);
        refreshPresetCombo();
    });
    tg->addWidget(m_tuneOn, 1, 0);

    m_tuneSpeed  = addKnob(tg, 0, 1, "SPEED",  0, 400, 1.0, 0, " ms");
    m_tuneAmount = addKnob(tg, 0, 2, "AMOUNT", 0, 100, 1.0, 0, " %");
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
        refreshPresetCombo();
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
        refreshPresetCombo();
    });
    tg->addWidget(m_scale, 1, 4);
    tg->setColumnStretch(5, 1);
    root->addWidget(tuneBox);

    // --- output ------------------------------------------------------------
    auto* outBox = new QGroupBox("OUTPUT");
    auto* og = new QGridLayout(outBox);
    m_gain = addKnob(og, 0, 0, "GAIN", -240, 240, 0.1, 1, " dB", true);
    m_gain->setToolTip("Makeup: most of these change the level as well as the sound.");
    auto* hint = new QLabel(
        "A telephone, radio, megaphone or walkie-talkie voice is EQ rather than "
        "an effect.\nOpen this strip's EQ and pick one from its profile list - "
        "they live there so\nthat a preset here never rewrites a curve you tuned.");
    hint->setProperty("role", "caption");
    og->addWidget(hint, 0, 1, 2, 1);
    og->setColumnStretch(1, 1);
    root->addWidget(outBox);

    auto* btns = new QHBoxLayout;
    m_note = new QLabel;
    m_note->setProperty("role", "caption");
    btns->addWidget(m_note, 1);
    auto* close = new QPushButton("Close");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    btns->addWidget(close);
    root->addLayout(btns);

    pull();
    resize(660, 440);
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
    refreshPresetCombo();
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

// Shows which preset the knobs currently amount to, or "(custom)" once they
// have been moved off one.
void VoiceFxDialog::refreshPresetCombo()
{
    const QSignalBlocker block(m_preset);
    const int idx = fx_preset_index(fx_capture(m_shm->strip[m_strip].fx));
    const int at = m_preset->findData(idx);
    m_preset->setCurrentIndex(at >= 0 ? at : 0);
}
