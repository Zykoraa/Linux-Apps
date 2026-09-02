#include "knob.h"
#include "color.h"
#include "metrics.h"
#include "theme.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>

static constexpr double kStartAngle = 225.0;   // degrees, Qt convention
static constexpr double kSweep      = 270.0;

// ---------------------------------------------------------------------------
// Knob
// ---------------------------------------------------------------------------
Knob::Knob(int lo, int hi, int def, bool bipolar, QString suffix, QWidget* parent)
    : QWidget(parent), m_lo(lo), m_hi(hi), m_def(qBound(lo, def, hi)),
      m_value(qBound(lo, def, hi)), m_bipolar(bipolar), m_suffix(std::move(suffix))
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize Knob::sizeHint() const        { return QSize(bbui::px(42), bbui::px(52)); }
QSize Knob::minimumSizeHint() const { return QSize(bbui::px(34), bbui::px(44)); }

void Knob::setValue(int v)
{
    v = qBound(m_lo, v, m_hi);
    if (v == m_value) return;
    m_value = v;
    update();
    emit valueChanged(v);
}

void Knob::setReduction(float r)
{
    r = qBound(0.0f, r, 1.0f);
    if (qAbs(r - m_reduction) < 0.01f) return;
    m_reduction = r;
    update();
}

QString Knob::shownText() const
{
    return m_fmt ? m_fmt(m_value)
                 : QString::number(m_value * m_scale, 'f', m_decimals) + m_suffix;
}

int Knob::stepFor(bool fine) const
{
    const int unit = qMax(1, (m_hi - m_lo) / 100);
    return fine ? unit : unit * 5;
}

void Knob::showValueTip()
{
    // The two-argument overload only: the three-argument form hides the moment
    // the pointer leaves the widget rect, which a vertical drag off a 42x52
    // knob does immediately.
    QToolTip::showText(mapToGlobal(QPoint(width() + 6, 0)), shownText());
}

void Knob::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int textH = bbui::px(12);
    const int side  = qMin(width(), height() - textH);
    const QRectF box((width() - side) / 2.0 + 3.0, 1.0, side - 6.0, side - 6.0);

    const double frac  = (m_hi == m_lo) ? 0.0 : double(m_value - m_lo) / double(m_hi - m_lo);
    const double dfrac = (m_hi == m_lo) ? 0.0 : double(m_def   - m_lo) / double(m_hi - m_lo);
    const bool   atDef = (m_value == m_def);
    const double arcW  = qMax(3.0, box.width() * 0.115);

    // --- track ------------------------------------------------------------
    //
    // Was panelAlt, which measures 1.19:1 against a card and 1.02:1 against the
    // bare window - and since the card never painted, every knob in the app
    // read as a featureless coin with a tally mark on it.
    QPen track(bbcolor::ensureContrast(t.border, t.panel, 1.9), arcW,
               Qt::SolidLine, Qt::RoundCap);
    p.setPen(track);
    p.drawArc(box, int(kStartAngle * 16), int(-kSweep * 16));

    // --- value arc --------------------------------------------------------
    //
    // Always drawn, including at the default: the old guards (`frac > 0.004`
    // and `|span| > 0.6`) both failed at rest, so the app's resting state was
    // its worst-looking one. A bipolar arc grows from the *default*, not the
    // range midpoint - the recorder's -60..+12 knob defaults to 0 dB and used
    // to draw a filled arc across the whole -24..0 region as if boosted.
    const QColor arcCol = atDef ? bbcolor::mix(t.accent, t.panel, 0.55) : t.accent;
    QPen fill(arcCol, arcW, Qt::SolidLine, Qt::RoundCap);
    p.setPen(fill);
    if (m_bipolar) {
        const double from = kStartAngle - kSweep * dfrac;
        const double span = -kSweep * (frac - dfrac);
        p.drawArc(box, int(from * 16), int(qAbs(span) < 0.8 ? (span < 0 ? -13 : 13)
                                                            : span * 16));
    } else {
        const double span = -kSweep * qMax(frac, 0.008);
        p.drawArc(box, int(kStartAngle * 16), int(span * 16));
    }

    // Gain reduction, as an outer arc. Drawn after the body would hide it, so
    // it goes here - before the body ellipse covers the middle.
    if (m_reduction > 0.005f) {
        QPen red(t.solo, qMax(2.0, arcW * 0.45), Qt::SolidLine, Qt::RoundCap);
        p.setPen(red);
        const QRectF outer = box.adjusted(-arcW * 0.75, -arcW * 0.75, arcW * 0.75, arcW * 0.75);
        p.drawArc(outer, int(kStartAngle * 16), int(-kSweep * m_reduction * 16));
    }

    // --- body: shadow, bezel, face, specular ------------------------------
    const QPointF c = box.center();
    const double r = box.width() / 2.0 - arcW * 0.9;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, t.dark ? 90 : 34));
    p.drawEllipse(c + QPointF(0, r * 0.10), r * 1.03, r * 1.03);

    p.setBrush(bbcolor::nudge(t.header, t.dark ? 10.0 : -6.0));
    p.drawEllipse(c, r, r);

    QRadialGradient g(c.x(), c.y() - r * 0.42, r * 1.75);
    g.setColorAt(0.0, bbcolor::nudge(t.header, t.dark ? 16.0 : 5.0));
    g.setColorAt(0.55, t.header);
    g.setColorAt(1.0, bbcolor::nudge(t.header, t.dark ? -9.0 : -7.0));
    p.setBrush(g);
    p.drawEllipse(c, r * 0.90, r * 0.90);

    // A thin highlight along the top edge, which is what makes it read as a
    // physical cap rather than a filled circle.
    QPen spec(QColor(255, 255, 255, t.dark ? 34 : 120), qMax(1.0, r * 0.07));
    p.setPen(spec);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c.x() - r * 0.80, c.y() - r * 0.80, r * 1.60, r * 1.60),
              int(35 * 16), int(110 * 16));

    if (m_hover || hasFocus()) {
        p.setPen(QPen(t.accent, 1.0));
        p.drawEllipse(c, r * 0.95, r * 0.95);
    }

    // --- pointer ----------------------------------------------------------
    //
    // A tapered wedge out to 90% of the radius. The old 2px stub covered 54%
    // and read as a tally mark.
    const double ang = qDegreesToRadians(kStartAngle - kSweep * frac);
    const QPointF dir(qCos(ang), -qSin(ang));
    const QPointF nrm(-dir.y(), dir.x());
    const double halfBase = qMax(1.1, r * 0.15);
    QPolygonF wedge;
    wedge << c + dir * (r * 0.90)
          << c + dir * (r * 0.22) + nrm * halfBase
          << c + dir * (r * 0.22) - nrm * halfBase;
    p.setPen(Qt::NoPen);
    p.setBrush(atDef ? bbcolor::ensureContrast(t.text, t.header, 3.0) : t.accent);
    p.drawPolygon(wedge);

    // --- readout ----------------------------------------------------------
    //
    // Fixed size: the old shrink-to-fit loop changed the font as the value
    // changed, so a knob's own label wobbled between 7.5pt and 5pt while you
    // dragged it. Elide instead - and keep the sign, which truncation ate.
    const QString shown = shownText();
    QFont f = p.font();
    f.setPixelSize(qMax(8, bbui::fs(9)));
    f.setBold(!atDef);
    bbui::makeTabular(f);
    p.setFont(f);
    const QFontMetricsF fm(f);
    p.setPen(atDef ? bbcolor::ensureContrast(t.textDim, t.panel, bbcolor::kTextFloor)
                   : bbcolor::ensureContrast(t.accent, t.panel, 3.0));
    p.drawText(QRectF(0, height() - textH - 1, width(), textH), Qt::AlignCenter,
               fm.elidedText(shown, Qt::ElideRight, width() - 1.0));
}

void Knob::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_fine = e->modifiers() & Qt::ControlModifier;
    setFocus(Qt::MouseFocusReason);
    m_dragStartY = e->position().toPoint().y();
    m_dragStartVal = m_value;
    showValueTip();
}

void Knob::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) return;
    // Fine control with Ctrl held, as in most mixers - re-anchored the moment
    // the modifier changes, or the accumulated offset is re-scaled under you.
    const bool fine = e->modifiers() & Qt::ControlModifier;
    if (fine != m_fine) {
        m_fine = fine;
        m_dragStartY = e->position().toPoint().y();
        m_dragStartVal = m_value;
    }
    const int dy = m_dragStartY - e->position().toPoint().y();
    const double perPixel = (m_hi - m_lo) / 150.0 * (fine ? 0.25 : 1.0);
    setValue(m_dragStartVal + int(qRound(dy * perPixel)));
    showValueTip();
}

void Knob::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
    QToolTip::hideText();
}

void Knob::mouseDoubleClickEvent(QMouseEvent*) { setValue(m_def); }

void Knob::wheelEvent(QWheelEvent* e)
{
    const int steps = e->angleDelta().y() / 120;
    setValue(m_value + steps * stepFor(e->modifiers() & Qt::ControlModifier));
    e->accept();
}

void Knob::keyPressEvent(QKeyEvent* e)
{
    const int step = stepFor(e->modifiers() & Qt::ControlModifier);
    switch (e->key()) {
    case Qt::Key_Up:   case Qt::Key_Right: setValue(m_value + step); break;
    case Qt::Key_Down: case Qt::Key_Left:  setValue(m_value - step); break;
    case Qt::Key_PageUp:   setValue(m_value + step * 5); break;
    case Qt::Key_PageDown: setValue(m_value - step * 5); break;
    case Qt::Key_Home: setValue(m_def); break;
    default: QWidget::keyPressEvent(e); return;
    }
    showValueTip();
    e->accept();
}

void Knob::enterEvent(QEnterEvent*) { m_hover = true;  update(); }
void Knob::leaveEvent(QEvent*)      { m_hover = false; update(); }

// ---------------------------------------------------------------------------
// XYPad
// ---------------------------------------------------------------------------
XYPad::XYPad(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::SizeHorCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize XYPad::sizeHint() const        { return QSize(bbui::px(64), bbui::px(34)); }
QSize XYPad::minimumSizeHint() const { return QSize(bbui::px(48), bbui::px(28)); }

void XYPad::setValues(int x, int y)
{
    x = qBound(-100, x, 100);
    y = 0;      // the engine has never read pan_y; see the header
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
    const bool centred = (m_x == 0);

    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());

    // Centre detent and the two ends, so the travel is readable at rest.
    p.setPen(QPen(bbcolor::mix(t.well, t.text, 0.30), 1.0));
    p.drawLine(QPointF(r.center().x(), r.top() + 3), QPointF(r.center().x(), r.bottom() - 3));
    p.setPen(QPen(bbcolor::mix(t.well, t.text, 0.18), 1.0));
    p.drawLine(QPointF(r.left() + 4, r.center().y()), QPointF(r.right() - 4, r.center().y()));

    const double px_ = r.left() + (m_x + 100) / 200.0 * r.width();

    // A filled bar from centre to the handle: pan is a signed quantity and this
    // is the only thing that showed which way and how far.
    if (!centred) {
        QColor fill = t.accent; fill.setAlpha(120);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        const double x0 = qMin(r.center().x(), px_), x1 = qMax(r.center().x(), px_);
        p.drawRect(QRectF(x0, r.center().y() - 2, x1 - x0, 4));
    }

    p.setPen(QPen(centred ? bbcolor::mix(t.well, t.text, 0.5) : t.accent, 1.4));
    p.drawLine(QPointF(px_, r.top() + 3), QPointF(px_, r.bottom() - 3));
    p.setBrush(centred ? bbcolor::mix(t.well, t.text, 0.5) : t.accent);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(px_, r.center().y()), 3.4, 3.4);

    // L / C / R, so the control says what it is.
    QFont f = p.font();
    f.setPixelSize(qMax(7, bbui::fs(8)));
    bbui::makeTabular(f);
    p.setFont(f);
    p.setPen(bbcolor::ensureContrast(t.textDim, t.well, 3.2));
    const QString lbl = centred ? QStringLiteral("C")
                                : QString::asprintf("%c %d", m_x < 0 ? 'L' : 'R', qAbs(m_x));
    p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
               centred ? QStringLiteral("L") : QString());
    p.drawText(r.adjusted(4, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter,
               centred ? QStringLiteral("R") : QString());
    if (!centred) p.drawText(r, Qt::AlignCenter, lbl);
    else          p.drawText(r, Qt::AlignCenter, QStringLiteral("C"));

    p.setPen(QPen(m_hover || hasFocus() ? t.accent : t.border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());
}

void XYPad::applyPos(const QPoint& pt)
{
    const QRectF r = rect().adjusted(1, 1, -1, -1);
    if (r.width() < 2) return;
    setValues(int(qRound((pt.x() - r.left()) / r.width() * 200.0 - 100.0)), 0);
}

void XYPad::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    setFocus(Qt::MouseFocusReason);
    applyPos(e->position().toPoint());
}
void XYPad::mouseMoveEvent(QMouseEvent* e) { if (m_dragging) applyPos(e->position().toPoint()); }
void XYPad::mouseReleaseEvent(QMouseEvent*) { m_dragging = false; }
void XYPad::mouseDoubleClickEvent(QMouseEvent*) { setValues(0, 0); }

void XYPad::keyPressEvent(QKeyEvent* e)
{
    const int step = (e->modifiers() & Qt::ControlModifier) ? 1 : 5;
    switch (e->key()) {
    case Qt::Key_Left:  setValues(m_x - step, 0); break;
    case Qt::Key_Right: setValues(m_x + step, 0); break;
    case Qt::Key_Home:  setValues(0, 0); break;
    default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void XYPad::enterEvent(QEnterEvent*) { m_hover = true;  update(); }
void XYPad::leaveEvent(QEvent*)      { m_hover = false; update(); }

// ---------------------------------------------------------------------------
// Fader
// ---------------------------------------------------------------------------
Fader::Fader(int lo, int hi, int def, QWidget* parent)
    : QWidget(parent), m_lo(lo), m_hi(hi), m_def(qBound(lo, def, hi)),
      m_value(qBound(lo, def, hi))
{
    setCursor(Qt::SizeVerCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

QSize Fader::sizeHint() const        { return QSize(bbui::px(44), bbui::px(200)); }
QSize Fader::minimumSizeHint() const { return QSize(bbui::px(42), bbui::px(90));  }

void Fader::setValue(int v)
{
    v = qBound(m_lo, v, m_hi);
    if (v == m_value) return;
    m_value = v;
    update();
    emit valueChanged(v);
}

QRectF Fader::grooveRect() const
{
    const double x = width() * 0.30;
    const double inset = bbui::travelInset();
    return QRectF(x - 3.5, inset, 7.0, qMax(6.0, height() - inset * 2));
}

QRectF Fader::capRect() const
{
    const QRectF g = grooveRect();
    const double half = bbui::capHalf();
    return QRectF(g.center().x() - bbui::px(13), valueToY(m_value) - half,
                  bbui::px(26), half * 2);
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

void Fader::showValueTip()
{
    QToolTip::showText(mapToGlobal(QPoint(width() + 6, 0)),
                       QString::asprintf("%+.1f dB", m_value / 10.0));
}

void Fader::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF g = grooveRect();

    // --- dB scale ---------------------------------------------------------
    //
    // Five labelled stops rather than two: seven of nine ticks used to be bare
    // 3px dashes floating in the groove with nothing to read them against.
    static const int kTicks[]   = { 120, 60, 0, -60, -120, -200, -300, -400, -600 };
    static const int kLabelled[] = { 120, 0, -120, -300, -600 };
    QFont f = p.font();
    f.setPixelSize(qMax(7, bbui::fs(8)));
    bbui::makeTabular(f);
    p.setFont(f);
    const QFontMetricsF fm(f);
    const QColor tickCol  = bbcolor::mix(t.panel, t.text, 0.34);
    const QColor labelCol = bbcolor::ensureContrast(t.textDim, t.panel, bbcolor::kTextFloor);

    for (int tick : kTicks) {
        if (tick > m_hi || tick < m_lo) continue;
        const double y = valueToY(tick);
        const bool unity = (tick == 0);
        bool labelled = false;
        for (int l : kLabelled) labelled = labelled || (l == tick);

        p.setPen(QPen(unity ? t.accent : tickCol, unity ? 1.5 : 1.0));
        p.drawLine(QPointF(g.right() + 2, y), QPointF(g.right() + (labelled ? 7 : 4), y));
        if (!labelled) continue;
        p.setPen(unity ? t.accent : labelCol);
        const QString s = unity ? QStringLiteral("0") : QString::number(tick / 10);
        // Right-aligned against the fader's own edge, so the column of numbers
        // lines up down the whole console.
        p.drawText(QRectF(g.right() + 8, y - 6, width() - g.right() - 9, 12),
                   Qt::AlignRight | Qt::AlignVCenter, s);
    }

    // --- groove -----------------------------------------------------------
    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(g, bbui::radWell(), bbui::radWell());

    // Travel indication as a narrow centred stripe, not a filled groove: a
    // full-width saturated column beside a level meter reads as a second meter,
    // and in Everforest `accent` and `meterLow` are byte-identical.
    const double y = valueToY(m_value);
    QRectF stripe(g.center().x() - 1.5, y, 3.0, g.bottom() - y);
    p.setBrush(m_value == m_def ? bbcolor::mix(t.accent, t.well, 0.45) : t.accent);
    p.drawRoundedRect(stripe, 1.5, 1.5);

    // --- cap --------------------------------------------------------------
    const QRectF cap = capRect();
    const double rad = bbui::px(3);

    p.setBrush(QColor(0, 0, 0, t.dark ? 96 : 40));
    p.drawRoundedRect(cap.translated(0, 1.5), rad, rad);

    QLinearGradient lg(cap.topLeft(), cap.bottomLeft());
    lg.setColorAt(0.0,  bbcolor::nudge(t.header, t.dark ? 18.0 : 7.0));
    lg.setColorAt(0.46, bbcolor::nudge(t.header, t.dark ? 8.0  : 2.0));
    lg.setColorAt(0.54, bbcolor::nudge(t.header, t.dark ? -4.0 : -3.0));
    lg.setColorAt(1.0,  bbcolor::nudge(t.header, t.dark ? -10.0 : -8.0));
    p.setBrush(lg);
    p.setPen(QPen(hasFocus() || m_dragging || m_hover ? t.accent : t.border, 1.0));
    p.drawRoundedRect(cap, rad, rad);

    // Grip seam, then the read line: the seam gives it a body, the read line is
    // what you actually align against the scale.
    p.setPen(QPen(QColor(0, 0, 0, t.dark ? 70 : 30), 1.0));
    for (double o : { -3.0, 3.0 })
        p.drawLine(QPointF(cap.left() + 4, cap.center().y() + o),
                   QPointF(cap.right() - 4, cap.center().y() + o));
    p.setPen(QPen(m_value == m_def ? t.accent
                                   : bbcolor::ensureContrast(t.text, t.header, 3.0), 1.6));
    p.drawLine(QPointF(cap.left() + 3, cap.center().y()),
               QPointF(cap.right() - 3, cap.center().y()));
}

void Fader::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    // Middle-click keeps the old absolute jump. A left press used to slam the
    // level to wherever it landed, so a mis-click 2px from the bottom was an
    // instant -60 dB on a live mix.
    if (e->button() == Qt::MiddleButton) {
        setValue(yToValue(e->position().y()));
        return;
    }
    if (e->button() != Qt::LeftButton) return;

    const QRectF cap = capRect();
    m_dragging = true;
    m_fine = (e->modifiers() & Qt::ControlModifier);
    if (!cap.adjusted(-4, -4, 4, 4).contains(e->position())) {
        // Off the cap: page toward the click rather than jumping to it.
        const int page = qMax(1, (m_hi - m_lo) / 20);
        setValue(m_value + (e->position().y() < cap.center().y() ? page : -page));
    }
    m_dragStartY = int(e->position().y());
    m_dragStartVal = m_value;
    showValueTip();
}

void Fader::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) return;
    // Anchored, like Knob. The old form was `m_value + (target - m_value) / 4`
    // in integers, so inside three raw units the quotient was 0 and fine drag
    // stopped moving altogether - the opposite of fine control.
    //
    // Re-anchor when Ctrl goes down or up part-way through a drag: the offset
    // is measured from the anchor, so changing the scale without moving the
    // anchor re-scales everything accumulated so far and jumps a live fader.
    const bool fine = e->modifiers() & Qt::ControlModifier;
    if (fine != m_fine) {
        m_fine = fine;
        m_dragStartY = int(e->position().y());
        m_dragStartVal = m_value;
    }
    const double dy = m_dragStartY - e->position().y();
    const double perPixel = double(m_hi - m_lo) / qMax(1.0, grooveRect().height())
                          * (fine ? 0.25 : 1.0);
    setValue(m_dragStartVal + int(qRound(dy * perPixel)));
    showValueTip();
}

void Fader::mouseReleaseEvent(QMouseEvent*)
{
    m_dragging = false;
    QToolTip::hideText();
    update();
}

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

void Fader::keyPressEvent(QKeyEvent* e)
{
    // The same increment the wheel uses: 1.0 dB, or 0.1 dB with Ctrl.
    const int step = (e->modifiers() & Qt::ControlModifier) ? 1 : 10;
    switch (e->key()) {
    case Qt::Key_Up:       setValue(m_value + step); break;
    case Qt::Key_Down:     setValue(m_value - step); break;
    case Qt::Key_PageUp:   setValue(m_value + step * 5); break;
    case Qt::Key_PageDown: setValue(m_value - step * 5); break;
    case Qt::Key_Home:     setValue(m_def); break;
    case Qt::Key_End:      setValue(m_lo); break;
    default: QWidget::keyPressEvent(e); return;
    }
    showValueTip();
    e->accept();
}

void Fader::enterEvent(QEnterEvent*) { m_hover = true;  update(); }
void Fader::leaveEvent(QEvent*)      { m_hover = false; update(); }
