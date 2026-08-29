#include "widgets.h"
#include "theme.h"
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <cmath>

ReductionBar::ReductionBar(Kind kind, QWidget* parent)
    : QWidget(parent), m_kind(kind)
{
    setFixedHeight(4);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ReductionBar::setAmount(float amount)
{
    amount = qBound(0.0f, amount, 1.0f);
    if (qAbs(amount - m_amount) < 0.01f) return;
    m_amount = amount;
    update();
}

void ReductionBar::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.fillRect(rect(), t.panelAlt);
    if (m_amount <= 0.005f) return;
    // Reduction grows from the right, the way a gain-reduction meter reads.
    const QColor col = m_kind == Gate ? t.mono : (m_kind == Comp ? t.solo : t.eqOn);
    const int w = int(width() * m_amount);
    p.fillRect(width() - w, 0, w, height(), col);
}

LevelMeter::LevelMeter(int channels, QWidget* parent)
    : QWidget(parent), m_channels(channels)
{
    setChannels(channels);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void LevelMeter::setChannels(int n)
{
    m_channels = qMax(1, n);
    m_db.fill(kMinDb, m_channels);
    m_hold.fill(kMinDb, m_channels);
    m_holdAge.fill(0, m_channels);
    updateGeometry();
}

float LevelMeter::dbToFrac(float db)
{
    if (db <= kMinDb) return 0.0f;
    if (db >= kMaxDb) return 1.0f;
    return (db - kMinDb) / (kMaxDb - kMinDb);
}

void LevelMeter::setLevels(const float* linear, int n)
{
    n = qMin(n, m_channels);
    bool dirty = false;
    for (int c = 0; c < n; ++c) {
        const float db = linear[c] <= 1e-7f ? kMinDb : 20.0f * std::log10(linear[c]);
        if (std::fabs(db - m_db[c]) > 0.1f) dirty = true;
        m_db[c] = db;
        if (db >= m_hold[c]) { m_hold[c] = db; m_holdAge[c] = 0; dirty = true; }
        else if (++m_holdAge[c] > 45) {          // ~1.5 s at 30 fps
            m_hold[c] = qMax(kMinDb, m_hold[c] - 0.9f);
            dirty = true;
        }
    }
    if (dirty) update();
}

void LevelMeter::setClipped(bool c)
{
    if (c == m_clipped) return;
    m_clipped = c;
    update();
}

void LevelMeter::mousePressEvent(QMouseEvent*)
{
    if (m_onClick) m_onClick();
}

void LevelMeter::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    const int w = width(), h = height();
    const int gap = 1;
    const int bw = qMax(3, (w - gap * (m_channels - 1)) / m_channels);

    for (int c = 0; c < m_channels; ++c) {
        const int x = c * (bw + gap);
        p.fillRect(x, 0, bw, h, t.panelAlt);

        const int lit = int(dbToFrac(m_db[c]) * h);
        if (lit > 0) {
            // Green below -18, yellow to -6, red above. Gradient is laid out in
            // widget coordinates so the colour boundaries sit at fixed dB.
            QLinearGradient g(0, h, 0, 0);
            g.setColorAt(0.0,              t.meterLow);
            g.setColorAt(dbToFrac(-18.0f), t.meterLow);
            g.setColorAt(dbToFrac(-6.0f),  t.meterMid);
            g.setColorAt(dbToFrac(-1.0f),  t.meterHigh);
            g.setColorAt(1.0,              t.meterPeak);
            p.fillRect(x, h - lit, bw, lit, g);
        }

        if (m_hold[c] > kMinDb) {
            const int y = h - int(dbToFrac(m_hold[c]) * h);
            p.fillRect(x, qBound(0, y - 1, h - 2), bw, 2,
                       m_hold[c] >= -0.5f ? t.meterPeak : t.meterHold);
        }
    }

    // dB ruler: faint lines at the usual reference points, brighter at unity.
    QColor grid = t.text; grid.setAlpha(28);
    p.setPen(grid);
    for (float db : { -6.0f, -12.0f, -20.0f, -30.0f, -40.0f }) {
        const int y = h - int(dbToFrac(db) * h);
        p.drawLine(0, y, w, y);
    }
    QColor ref = t.text; ref.setAlpha(85);
    p.setPen(ref);
    const int y0 = h - int(dbToFrac(0.0f) * h);
    p.drawLine(0, y0, w, y0);

    // Latched clip indicator across the top; click the meter to clear it.
    if (m_clipped) p.fillRect(0, 0, w, 3, t.meterPeak);

    p.setPen(t.border);
    p.drawRect(0, 0, w - 1, h - 1);
}
