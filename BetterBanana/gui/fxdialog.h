// betterbanana GUI - the voice changer.
//
// One dialog per input strip, editing that strip's VoiceFx block directly; the
// engine picks changes up on its next block. Presets come from
// common/fxpreset.h so this and bb-ctl always offer the same list.
#pragma once

#include "../common/protocol.h"

#include <QDialog>
#include <QVector>

class QComboBox;
class QLabel;
class QPushButton;
class Knob;

class VoiceFxDialog : public QDialog {
    Q_OBJECT
public:
    VoiceFxDialog(bb::Shared* shm, int strip, const QString& title,
                  QWidget* parent = nullptr);

private:
    Knob* addKnob(class QGridLayout* g, int row, int col, const QString& cap,
                  int lo, int hi, double scale, int decimals,
                  const QString& suffix, bool bipolar = false);
    void pull();                    // shm -> knobs, without re-entering them
    void push();                    // knobs -> shm
    void refreshPresetCombo();

    bb::Shared* m_shm;
    int         m_strip;
    bool        m_updating = false;

    QComboBox*   m_preset = nullptr;
    QPushButton* m_on     = nullptr;
    QLabel*      m_note   = nullptr;

    QPushButton* m_fmtOn = nullptr;
    QPushButton* m_calib = nullptr;
    Knob* m_pitch = nullptr;
    Knob* m_formant = nullptr;
    Knob* m_drive = nullptr;
    Knob* m_ringHz = nullptr;
    Knob* m_ringMix = nullptr;
    Knob* m_bits = nullptr;
    Knob* m_down = nullptr;
    Knob* m_chMs = nullptr;
    Knob* m_chHz = nullptr;
    Knob* m_chMix = nullptr;
    Knob* m_echoMs = nullptr;
    Knob* m_echoFb = nullptr;
    Knob* m_echoMix = nullptr;
    Knob* m_rvSize = nullptr;
    Knob* m_rvDamp = nullptr;
    Knob* m_rvMix = nullptr;
    Knob* m_gain = nullptr;
};
