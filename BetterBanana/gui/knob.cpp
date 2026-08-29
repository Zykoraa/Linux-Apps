#include "knob.h"
#include "theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>

static constexpr double kStartAngle = 225.0;   // degrees, Qt convention
static constexpr double kSweep      = 270.0;

Knob::Knob(int lo, int hi, int def, bool bipolar, QString suffix, QWidget* parent)
    : QWidget(parent), m_lo(lo), m_hi(hi), m_def(def), m_value(def),
      m_bipolar(bipolar), m_suffix(std::move(suffix))
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::WheelFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void Knob::setValue(int v)
{
    v = qBound(m_lo, v, m_hi);
    if (v == m_value) return;
    m_value = v;
    update();
    emit valueChanged(v);
}

void Knob::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int textH = 11;
    const int side  = qMin(width(), height() - textH);
    const QRectF box(( width() - side) / 2.0 + 3.0, 1.0, side - 6.0, side - 6.0);

    const double frac = (m_hi == m_lo) ? 0.0 : double(m_value - m_lo) / double(m_hi - m_lo);

    // Track
    QPen track(t.panelAlt, 4.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(track);
    p.drawArc(box, int(kStartAngle * 16), int(-kSweep * 16));

    // Value arc: from centre when bipolar, from the low end otherwise.
    QPen fill(t.accent, 4.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(fill);
    if (m_bipolar) {
        const double from = kStartAngle - kSweep * 0.5;
        const double span = -kSweep * (frac - 0.5);
        if (qAbs(span) > 0.6) p.drawArc(box, int(from * 16), int(span * 16));
    } else if (frac > 0.004) {
        p.drawArc(box, int(kStartAngle * 16), int(-kSweep * frac * 16));
    }

    // Body
    const QPointF c = box.center();
    const double r = box.width() / 2.0 - 4.0;
    QRadialGradient g(c.x(), c.y() - r * 0.3, r * 1.6);
    g.setColorAt(0.0, t.header.lighter(t.dark ? 130 : 104));
    g.setColorAt(1.0, t.header);
    p.setBrush(g);
    p.setPen(QPen(t.border, 1.0));
    p.drawEllipse(c, r, r);

    // Pointer
    const double ang = qDegreesToRadians(kStartAngle - kSweep * frac);
    const QPointF tip(c.x() + qCos(ang) * r * 0.78, c.y() - qSin(ang) * r * 0.78);
    const QPointF base(c.x() + qCos(ang) * r * 0.24, c.y() - qSin(ang) * r * 0.24);
    p.setPen(QPen(m_value == m_def ? t.text : t.accent, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(base, tip);

    // Value readout. Shrink to fit rather than clip: a truncated "-34.0 dB"
    // reads as "34.0 dl", which hides the minus sign on a negative value.
    p.setPen(m_value == m_def ? t.textDim : t.accent);
    const QString shown = m_fmt ? m_fmt(m_value)
                                : QString::number(m_value * m_scale, 'f', m_decimals) + m_suffix;
    QFont f = p.font();
    f.setBold(m_value != m_def);
    double pt = 7.5;
    f.setPointSizeF(pt);
    while (pt > 5.0 && QFontMetricsF(f).horizontalAdvance(shown) > width() - 1.0) {
        pt -= 0.5;
        f.setPointSizeF(pt);
    }
    p.setFont(f);
    p.drawText(QRectF(0, height() - textH - 1, width(), textH), Qt::AlignCenter, shown);
}

void Knob::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStartY = e->position().toPoint().y();
    m_dragStartVal = m_value;
}

void Knob::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) return;
    const int dy = m_dragStartY - e->position().toPoint().y();
    // Fine control with Ctrl held, as in most mixers.
    const double perPixel = (m_hi - m_lo) / 150.0 * ((e->modifiers() & Qt::ControlModifier) ? 0.25 : 1.0);
    setValue(m_dragStartVal + int(qRound(dy * perPixel)));
}

void Knob::mouseReleaseEvent(QMouseEvent*) { m_dragging = false; }

void Knob::mouseDoubleClickEvent(QMouseEvent*) { setValue(m_def); }

void Knob::wheelEvent(QWheelEvent* e)
{
    const int steps = e->angleDelta().y() / 120;
    const int unit = qMax(1, (m_hi - m_lo) / 100);
    setValue(m_value + steps * unit * ((e->modifiers() & Qt::ControlModifier) ? 1 : 5));
    e->accept();
}

// ---------------------------------------------------------------------------
XYPad::XYPad(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void XYPad::setValues(int x, int y)
{
    x = qBound(-100, x, 100); y = qBound(-100, y, 100);
    if (x == m_x && y == m_y) return;
    m_x = x; m_y = y;
    update();
    emit valuesChanged(m_x, m_y);
}

void XYPad::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(1, 1, -1, -1);

    p.setPen(Qt::NoPen);
    p.setBrush(t.panelAlt);
    p.drawRoundedRect(r, 3, 3);

    p.setPen(QPen(t.border, 1.0, Qt::DotLine));
    p.drawLine(QPointF(r.center().x(), r.top() + 2), QPointF(r.center().x(), r.bottom() - 2));
    p.drawLine(QPointF(r.left() + 2, r.center().y()), QPointF(r.right() - 2, r.center().y()));

    const double px = r.left() + (m_x + 100) / 200.0 * r.width();
    const double py = r.top()  + (100 - m_y) / 200.0 * r.height();
    const bool centred = (m_x == 0 && m_y == 0);

    p.setPen(QPen(centred ? t.textDim : t.accent, 1.0));
    p.drawLine(QPointF(px, r.top() + 2), QPointF(px, r.bottom() - 2));
    p.setBrush(centred ? t.textDim : t.accent);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(px, py), 3.2, 3.2);

    p.setPen(QPen(t.border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 3, 3);
}

void XYPad::applyPos(const QPoint& pt)
{
    const QRectF r = rect().adjusted(1, 1, -1, -1);
    if (r.width() < 2 || r.height() < 2) return;
    setValues(int(qRound((pt.x() - r.left()) / r.width()  * 200.0 - 100.0)),
              int(qRound(100.0 - (pt.y() - r.top())  / r.height() * 200.0)));
}

void XYPad::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    applyPos(e->position().toPoint());
}
void XYPad::mouseMoveEvent(QMouseEvent* e) { if (m_dragging) applyPos(e->position().toPoint()); }
void XYPad::mouseReleaseEvent(QMouseEvent*) { m_dragging = false; }
void XYPad::mouseDoubleClickEvent(QMouseEvent*) { setValues(0, 0); }

// ---------------------------------------------------------------------------
// Fader
// ---------------------------------------------------------------------------
Fader::Fader(int lo, int hi, int def, QWidget* parent)
    : QWidget(parent), m_lo(lo), m_hi(hi), m_def(def), m_value(def)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void Fader::setValue(int v)
{
    v = qBound(m_lo, v, m_hi);
    if (v == m_value) return;
    m_value = v;
    update();
    emit valueChanged(v);
}

static constexpr double kCapH = 13.0;

QRectF Fader::grooveRect() const
{
    const double x = width() * 0.30;
    return QRectF(x - 3.0, kCapH / 2 + 2, 6.0, height() - kCapH - 4);
}

double Fader::valueToY(int v) const
{
    const QRectF g = grooveRect();
    const double f = double(v - m_lo) / double(m_hi - m_lo);
    return g.bottom() - f * g.height();
}

int Fader::yToValue(double y) const
{
    const QRectF g = grooveRect();
    const double f = qBound(0.0, (g.bottom() - y) / g.height(), 1.0);
    return int(qRound(m_lo + f * (m_hi - m_lo)));
}

void Fader::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF g = grooveRect();

    // dB scale, drawn to the right of the groove.
    static const int kTicks[] = { 120, 0, -60, -120, -200, -300, -400, -600 };
    p.setFont(QFont(font().family(), 6));
    for (int tick : kTicks) {
        if (tick > m_hi || tick < m_lo) continue;
        const double y = valueToY(tick);
        const bool unity = (tick == 0);
        p.setPen(QPen(unity ? t.accent : t.border, unity ? 1.4 : 1.0));
        p.drawLine(QPointF(g.right() + 2, y), QPointF(g.right() + (unity ? 8 : 5), y));
        if (unity || tick == m_lo) {
            // Right-align inside whatever room is left, so "-60" is never clipped.
            p.setPen(unity ? t.accent : t.textDim);
            const double lx = g.right() + 9;
            p.drawText(QRectF(lx, y - 5, width() - lx - 1, 10),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       unity ? "0" : QString::number(tick / 10));
        }
    }

    // Groove, with the travel below the cap tinted so the level reads at a glance.
    p.setPen(Qt::NoPen);
    p.setBrush(t.panelAlt);
    p.drawRoundedRect(g, 3, 3);
    const double y = valueToY(m_value);
    QColor fill = t.accent;
    fill.setAlpha(m_value == m_def ? 90 : 150);
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(g.left(), y, g.width(), g.bottom() - y), 3, 3);

    // Cap
    const QRectF cap(g.center().x() - 11, y - kCapH / 2, 22, kCapH);
    QLinearGradient lg(cap.topLeft(), cap.bottomLeft());
    lg.setColorAt(0.0, t.header.lighter(t.dark ? 150 : 102));
    lg.setColorAt(0.5, t.header.lighter(t.dark ? 125 : 100));
    lg.setColorAt(1.0, t.header);
    p.setBrush(lg);
    p.setPen(QPen(hasFocus() || m_dragging ? t.accent : t.border, 1.0));
    p.drawRoundedRect(cap, 3, 3);
    p.setPen(QPen(m_value == m_def ? t.accent : t.text, 1.4));
    p.drawLine(QPointF(cap.left() + 3, cap.center().y()), QPointF(cap.right() - 3, cap.center().y()));
}

void Fader::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    setFocus(Qt::MouseFocusReason);
    setValue(yToValue(e->position().y()));
}

void Fader::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) return;
    // Ctrl gives fine control by scaling the movement around the current value.
    if (e->modifiers() & Qt::ControlModifier) {
        const int target = yToValue(e->position().y());
        setValue(m_value + (target - m_value) / 4);
    } else {
        setValue(yToValue(e->position().y()));
    }
}

void Fader::mouseReleaseEvent(QMouseEvent*) { m_dragging = false; update(); }

// Double-click snaps back to unity.
void Fader::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = false;
    setValue(m_def);
}

void Fader::wheelEvent(QWheelEvent* e)
{
    // Only once deliberately focused, so a stray scroll cannot move the mix.
    if (!hasFocus()) { e->ignore(); return; }
    const int steps = e->angleDelta().y() / 120;
    setValue(m_value + steps * ((e->modifiers() & Qt::ControlModifier) ? 1 : 10));
    e->accept();
}
