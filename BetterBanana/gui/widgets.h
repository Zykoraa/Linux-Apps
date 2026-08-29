// betterbanana GUI - custom-painted widgets.
// No Q_OBJECT here (nothing emits), so this header needs no moc pass.
#pragma once

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

    QSize sizeHint() const override        { return QSize(30, 4); }
    QSize minimumSizeHint() const override { return QSize(12, 4); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Kind  m_kind;
    float m_amount = 0.0f;
};

// Vertical segmented level meter with peak hold, scaled -60..+12 dB.
class LevelMeter : public QWidget {
public:
    explicit LevelMeter(int channels = 2, QWidget* parent = nullptr);

    void setLevels(const float* linear, int n);   // linear peak per channel
    void setChannels(int n);
    void setClipped(bool c);
    // Called when the meter is clicked, to clear a latched clip indicator.
    void setClickHandler(std::function<void()> f) { m_onClick = std::move(f); }

    QSize sizeHint() const override        { return QSize(6 * m_channels + 3, 160); }
    QSize minimumSizeHint() const override { return QSize(5 * m_channels + 3, 60);  }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    static constexpr float kMinDb = -60.0f, kMaxDb = 12.0f;
    bool m_clipped = false;
    std::function<void()> m_onClick;
    static float dbToFrac(float db);

    int            m_channels;
    QVector<float> m_db;        // current, dB
    QVector<float> m_hold;      // peak hold, dB
    QVector<int>   m_holdAge;   // frames since hold set
};
