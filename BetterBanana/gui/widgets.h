// betterbanana GUI - custom-painted widgets.
// No Q_OBJECT here (nothing emits), so this header needs no moc pass.
#pragma once

#include <QComboBox>
#include <QLabel>
#include <QElapsedTimer>
#include <QWidget>
#include <QVector>
#include <functional>

// Thin horizontal bar showing how much gain something is removing. Used for
// the gate, the compressor and the ducker, whose reduction the engine already
// computes but nothing displayed until now.
class ReductionBar : public QWidget {
public:
    enum Kind { Gate, Comp, Duck };
    explicit ReductionBar(Kind kind, QWidget* parent = nullptr);

    // `amount` is 0 (nothing removed) .. 1 (fully removed).
    void setAmount(float amount);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Kind  m_kind;
    float m_amount = 0.0f;
};

// Vertical segmented level meter with peak hold, scaled -60..+12 dB.
//
// Two bars per channel: a slow one carrying the eye and a 2px fast-peak line
// over it, because the engine's only ballistic is a 300 ms release and a meter
// that tracks the raw poll value looks like it is flickering rather than
// reading. Rungs are punched every 1.5 dB, so a rung is worth the same on a
// 220px strip as on a 340px bus - a pixel pitch would be a texture instead of
// something an eye can count.
class LevelMeter : public QWidget {
public:
    explicit LevelMeter(int channels = 2, QWidget* parent = nullptr);

    void setLevels(const float* linear, int n);   // linear peak per channel
    void setChannels(int n);
    void setClipped(bool c);
    // The engine has stopped writing. Without this the lit bar freezes exactly
    // where it was and ten meters go on showing a steady signal that no longer
    // exists.
    void setStale(bool s);
    // Called when the meter is clicked, to clear a latched clip indicator.
    void setClickHandler(std::function<void()> f) { m_onClick = std::move(f); }

    // Held peak in dB, for a tooltip or a master readout.
    float heldPeak() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    static constexpr float kMinDb = -60.0f, kMaxDb = 12.0f;
    static constexpr float kRungDb = 1.5f;      // one segment
    bool m_clipped = false;
    bool m_stale   = false;
    bool m_hover   = false;
    std::function<void()> m_onClick;
    static float dbToFrac(float db);

    int            m_channels;
    QVector<float> m_db;        // fast: what the engine last published
    QVector<float> m_slow;      // eye-paced fall
    QVector<float> m_hold;      // peak hold, dB
    QElapsedTimer  m_clock;     // ballistics in time, not in frames
    qint64         m_lastMs = 0;
    QVector<qint64> m_holdAt;   // when each hold was set
};

// A label that shortens rather than clipping. Card headers carry user-chosen
// names of any length, and QLabel just cuts them off mid-word.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString& text = QString(), QWidget* parent = nullptr);
    void setText(const QString& t);
    QString fullText() const { return m_full; }
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_full;
};

// A bus's parametric EQ, small. A bus card carries fewer controls than a strip,
// so aligning every meter in the console onto one baseline leaves a gap above
// it; this is the most useful thing that can go there. Click to open the
// editor. Drawn from the same eq_response_db() the editor's own curve uses, so
// what is here is what is being applied.
class EqThumb : public QWidget {
public:
    explicit EqThumb(QWidget* parent = nullptr);

    // `db` is one magnitude sample per pixel column, already in dB; `on` is
    // whether the block is enabled. Recomputed at the GUI's 2 Hz sync rate,
    // not per frame.
    void setCurve(const QVector<float>& db, bool on);
    void setClickHandler(std::function<void()> f) { m_onClick = std::move(f); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QVector<float> m_db;
    bool  m_on = false;
    bool  m_hover = false;
    std::function<void()> m_onClick;
};

// A device picker that elides instead of clipping. QComboBox cuts its text off
// mid-word, which is how "Ryzen HD Audio Controller" reached the screen as
// "Ryzen HD Audio Contro" with no ellipsis to say so.
class DeviceCombo : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent*) override;
};
