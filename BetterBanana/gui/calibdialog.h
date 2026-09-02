// betterbanana GUI - calibrating the voice changer to the voice using it.
//
// A preset that lifts six semitones lands a 95 Hz voice at 134 Hz and a 140 Hz
// voice at 198 Hz: the same setting, two completely different results. So this
// records a few seconds, measures where the speaker actually sits, and works
// out the shift from there - then plays the recording back through the result
// so it can be judged by ear rather than by number.
//
// Deliberately a measurement, not a model: it runs offline in a second or two,
// needs no network and no training, and everything it decides is visible and
// editable afterwards.
//
// The three steps are staged rather than merely numbered. Everything after the
// recording depends on a pitch nobody has measured yet, so until there is one,
// step 2 is locked and step 3 offers only playback of what was captured -
// which is the one thing worth hearing when a measurement fails.
#pragma once

#include "../common/protocol.h"

#include <QDialog>
#include <QString>
#include <QVector>

class CalibLevelBar;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QProcess;
class QPushButton;
class QTimer;

class VoiceCalibDialog : public QDialog {
    Q_OBJECT
public:
    // `device` is the capture node to record from, empty if the strip has none.
    VoiceCalibDialog(bb::Shared* shm, int strip, const QString& device,
                     const QString& title, QWidget* parent = nullptr);
    ~VoiceCalibDialog() override;

    bool applied() const { return m_applied; }

private slots:
    void startRecording();
    void finishRecording();
    void playOriginal();
    void playResult();
    void applyToStrip();

private:
    void analyse();
    void retarget();                 // target Hz -> pitch semitones
    // Status line and step gating in one place: which steps are live is a
    // function of what has been captured and measured, and nothing else.
    void restage(const QString& status, bool busy);
    void pollLevel();
    QString playbackSink() const;
    bool play(const QVector<float>& samples);
    QVector<float> processed() const;

    bb::Shared* m_shm;
    int         m_strip;
    QString     m_device;
    QString     m_title;
    bool        m_applied = false;
    bool        m_updating = false;

    QVector<float> m_raw;            // what was recorded, 48 kHz mono
    float          m_measured = 0.0f;

    QProcess* m_rec = nullptr;
    QTimer*   m_tick = nullptr;
    int       m_left = 0;

    // The input meter polls the engine's own strip level, so it needs to know
    // when the engine has stopped writing one.
    QTimer*   m_levelTimer = nullptr;
    uint32_t  m_lastHeartbeat = 0;
    int       m_stallTicks = 0;

    QGroupBox*      m_recBox = nullptr;
    QGroupBox*      m_aimBox = nullptr;
    QGroupBox*      m_playBox = nullptr;
    QPushButton*    m_record = nullptr;
    QPushButton*    m_playOrig = nullptr;
    QPushButton*    m_playRes = nullptr;
    QPushButton*    m_apply = nullptr;
    CalibLevelBar*  m_meter = nullptr;
    QLabel*         m_phrase = nullptr;
    QLabel*         m_result = nullptr;
    QLabel*         m_status = nullptr;
    QDoubleSpinBox* m_target = nullptr;
    QDoubleSpinBox* m_pitch = nullptr;
    QDoubleSpinBox* m_formant = nullptr;
};
