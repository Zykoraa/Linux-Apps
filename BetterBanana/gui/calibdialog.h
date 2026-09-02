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
#pragma once

#include "../common/protocol.h"

#include <QDialog>
#include <QString>
#include <QVector>

class QDoubleSpinBox;
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
    void setBusy(const QString& what, bool busy);
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

    QPushButton*    m_record = nullptr;
    QPushButton*    m_playOrig = nullptr;
    QPushButton*    m_playRes = nullptr;
    QPushButton*    m_apply = nullptr;
    QLabel*         m_phrase = nullptr;
    QLabel*         m_result = nullptr;
    QLabel*         m_status = nullptr;
    QDoubleSpinBox* m_target = nullptr;
    QDoubleSpinBox* m_pitch = nullptr;
    QDoubleSpinBox* m_formant = nullptr;
};
