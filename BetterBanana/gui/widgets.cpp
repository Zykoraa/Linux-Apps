#include "widgets.h"
#include "color.h"
#include "metrics.h"
#include "theme.h"
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QToolTip>
#include <cmath>

// ---------------------------------------------------------------------------
// ReductionBar
// ---------------------------------------------------------------------------
ReductionBar::ReductionBar(Kind kind, QWidget* parent)
    : QWidget(parent), m_kind(kind)
{
    setFixedHeight(bbui::px(5));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize ReductionBar::sizeHint() const        { return QSize(bbui::px(30), bbui::px(5)); }
QSize ReductionBar::minimumSizeHint() const { return QSize(bbui::px(12), bbui::px(5)); }

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
    p.setRenderHint(QPainter::Antialiasing);

    // On a card, `well` is a visible recess; it used to be panelAlt on bare
    // window colour at 1.02:1, which made three live engine measurements
    // legible nowhere.
    const QRectF r = rect().adjusted(0, 0, 0, 0);
    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());

    if (m_amount <= 0.005f) return;
    // Reduction grows from the right, the way a gain-reduction meter reads.
    const QColor col = m_kind == Gate ? t.mono : (m_kind == Comp ? t.solo : t.eqOn);
    const double w = r.width() * m_amount;
    p.setBrush(col);
    p.drawRoundedRect(QRectF(r.right() - w, r.top(), w, r.height()),
                      bbui::radWell(), bbui::radWell());
}

// ---------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------
LevelMeter::LevelMeter(int channels, QWidget* parent)
    : QWidget(parent), m_channels(channels)
{
    setChannels(channels);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_Hover, true);
    m_clock.start();
    m_lastMs = m_clock.elapsed();
}

QSize LevelMeter::sizeHint() const
{
    return QSize(bbui::px(10) * m_channels + bbui::px(2) * (m_channels - 1) + 2, bbui::px(180));
}

QSize LevelMeter::minimumSizeHint() const
{
    return QSize(bbui::px(7) * m_channels + m_channels + 2, bbui::px(60));
}

void LevelMeter::setChannels(int n)
{
    m_channels = qMax(1, n);
    m_db.fill(kMinDb, m_channels);
    m_slow.fill(kMinDb, m_channels);
    m_hold.fill(kMinDb, m_channels);
    m_holdAt.fill(0, m_channels);
    updateGeometry();
}

float LevelMeter::dbToFrac(float db)
{
    if (db <= kMinDb) return 0.0f;
    if (db >= kMaxDb) return 1.0f;
    return (db - kMinDb) / (kMaxDb - kMinDb);
}

float LevelMeter::heldPeak() const
{
    float m = kMinDb;
    for (float h : m_hold) m = qMax(m, h);
    return m;
}

void LevelMeter::setLevels(const float* linear, int n)
{
    n = qMin(n, m_channels);
    // dt is hoisted out of the channel loop deliberately: computing it inside
    // means channel 0 consumes the whole interval and channel 1 never decays.
    const qint64 now = m_clock.elapsed();
    const double dt  = qBound(0.0, double(now - m_lastMs) / 1000.0, 0.25);
    m_lastMs = now;

    bool dirty = m_stale;
    m_stale = false;

    // Slow bar: instant attack, 26 dB/s release. Peak hold: 1.4 s flat, then
    // 24 dB/s. Both in seconds rather than frames, so the meter reads the same
    // whether the GUI is keeping up or not.
    constexpr double kRelease = 26.0, kHoldFall = 24.0, kHoldMs = 1400.0;

    for (int c = 0; c < n; ++c) {
        const float db = linear[c] <= 1e-7f ? kMinDb : 20.0f * std::log10(linear[c]);
        if (std::fabs(db - m_db[c]) > 0.1f) dirty = true;
        m_db[c] = db;

        if (db >= m_slow[c]) { m_slow[c] = db; }
        else {
            const float fell = m_slow[c] - float(kRelease * dt);
            m_slow[c] = qMax(qMax(db, kMinDb), fell);
            dirty = true;
        }

        if (db >= m_hold[c]) { m_hold[c] = db; m_holdAt[c] = now; dirty = true; }
        else if (now - m_holdAt[c] > qint64(kHoldMs)) {
            m_hold[c] = qMax(kMinDb, m_hold[c] - float(kHoldFall * dt));
            dirty = true;
        }
    }
    // A latched clip has to keep repainting or its pulse never advances.
    if (m_clipped) dirty = true;
    if (dirty) update();
}

void LevelMeter::setClipped(bool c)
{
    if (c == m_clipped) return;
    m_clipped = c;
    update();
}

void LevelMeter::setStale(bool s)
{
    if (s == m_stale) return;
    m_stale = s;
    if (s) {
        m_db.fill(kMinDb);
        m_slow.fill(kMinDb);
        m_hold.fill(kMinDb);
    }
    update();
}

void LevelMeter::mousePressEvent(QMouseEvent*)
{
    if (m_onClick) m_onClick();
}

void LevelMeter::enterEvent(QEnterEvent*)
{
    m_hover = true;
    const float h = heldPeak();
    setToolTip(h <= kMinDb ? QStringLiteral("No signal\nClick to clear the clip indicator")
                           : QString::asprintf("Peak %+.1f dB\nClick to clear the clip indicator", h));
    update();
}

void LevelMeter::leaveEvent(QEvent*) { m_hover = false; update(); }

void LevelMeter::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    const int w = width(), h = height();
    // The bars live in the same band the fader's groove does, so the two
    // scales beside each other read the same dB at the same y.
    const int inset = bbui::travelInset();
    const int top = inset, bot = qMax(top + 4, h - inset), span = bot - top;
    const int gap = bbui::px(2);
    const int bw  = qMax(3, (w - gap * (m_channels - 1)) / m_channels);
    const int used = bw * m_channels + gap * (m_channels - 1);
    const int x0 = (w - used) / 2;

    // The rung ladder, in dB rather than pixels, so a segment is worth the same
    // on a short strip meter and a tall bus meter.
    const int rungs = int((kMaxDb - kMinDb) / kRungDb);
    const QColor cut = t.dark ? QColor(0, 0, 0, 150) : QColor(0, 0, 0, 60);
    const QColor ghost = bbcolor::mix(t.well, t.text, t.dark ? 0.10 : 0.14);

    // --- pass 1: troughs, and the ladder ghosted over them ----------------
    for (int c = 0; c < m_channels; ++c) {
        const int x = x0 + c * (bw + gap);
        p.fillRect(x, top, bw, span, t.well);
        p.setPen(Qt::NoPen);
        p.setBrush(ghost);
        for (int i = 1; i < rungs; ++i) {
            const int y = bot - int(double(i) / rungs * span);
            p.fillRect(x, y, bw, 1, ghost);
        }
    }

    // --- pass 2: the dB ruler, across the whole width ---------------------
    //
    // Drawn between the troughs and the lit bars: inside the per-channel loop
    // it ended up underneath the next channel's trough, and it used to be alpha
    // 28 - between 1.17:1 and 1.38:1, which is to say invisible in every theme.
    if (!m_stale) {
        QColor grid = bbcolor::mix(t.well, t.text, 0.26);
        for (float db : { -6.0f, -12.0f, -20.0f, -30.0f, -40.0f }) {
            const int y = bot - int(dbToFrac(db) * span);
            p.setPen(grid);
            for (int c = 0; c < m_channels; ++c) {
                const int x = x0 + c * (bw + gap);
                p.drawLine(x, y, x + bw - 1, y);   // stop at each channel
            }
        }
        QColor ref = bbcolor::mix(t.well, t.text, 0.55);
        p.setPen(ref);
        const int y0 = bot - int(dbToFrac(0.0f) * span);
        for (int c = 0; c < m_channels; ++c) {
            const int x = x0 + c * (bw + gap);
            p.drawLine(x, y0, x + bw - 1, y0);
        }
    }

    // --- pass 3: the lit bars ---------------------------------------------
    if (!m_stale) {
        for (int c = 0; c < m_channels; ++c) {
            const int x = x0 + c * (bw + gap);

            const int lit = int(dbToFrac(m_slow[c]) * span);
            if (lit > 0) {
                // Colour boundaries sit at fixed dB, laid out in widget
                // coordinates rather than in the bar's own.
                QLinearGradient g(0, bot, 0, top);
                g.setColorAt(0.0,              t.meterLow);
                g.setColorAt(dbToFrac(-18.0f), t.meterLow);
                g.setColorAt(dbToFrac(-6.0f),  t.meterMid);
                g.setColorAt(dbToFrac(-1.0f),  t.meterHigh);
                g.setColorAt(1.0,              t.meterPeak);
                p.fillRect(x, bot - lit, bw, lit, g);

                // Punch the rungs back out of the lit column. Segmentation is
                // what makes a level countable rather than a smear of colour.
                p.setPen(Qt::NoPen);
                for (int i = 1; i < rungs; ++i) {
                    const int y = bot - int(double(i) / rungs * span);
                    if (y >= bot - lit) p.fillRect(x, y, bw, 1, cut);
                }
            }

            // The engine's own fast value, over the eye-paced bar.
            if (m_db[c] > kMinDb) {
                const int y = bot - int(dbToFrac(m_db[c]) * span);
                p.fillRect(x, qBound(top, y - 1, bot - 2), bw, 2,
                           bbcolor::mix(t.text, t.meterHigh, 0.35));
            }

            // Peak hold.
            if (m_hold[c] > kMinDb) {
                const int y = bot - int(dbToFrac(m_hold[c]) * span);
                p.fillRect(x, qBound(top, y - 1, bot - 2), bw, 2,
                           m_hold[c] >= -0.5f ? t.meterPeak : t.meterHold);
            }
        }
    }

    // --- latched clip -----------------------------------------------------
    if (m_clipped) {
        const int ch = bbui::px(5);
        p.fillRect(0, top, w, ch, t.meterPeak);
        p.fillRect(0, top + ch, w, 1, bbcolor::onColor(t.meterPeak));
    }

    // --- held peak, overlaid ----------------------------------------------
    //
    // No reserved strip: the bars have to keep the same dB-per-pixel as the
    // fader beside them, so the number sits on top with a halo instead.
    if (!m_stale && h >= bbui::px(60) && w >= bbui::px(18)) {
        const float hp = heldPeak();
        if (hp > kMinDb) {
            QFont f = p.font();
            f.setPixelSize(qMax(7, bbui::fs(8)));
            f.setBold(true);
            bbui::makeTabular(f);
            p.setFont(f);
            const QString txt = QString::number(int(std::lround(hp)));
            const QRect box(0, top + (m_clipped ? bbui::px(7) : 1), w, bbui::px(11));
            // The halo is the trough colour, not black: a black outline on a
            // light theme reads as a smudge rather than a number.
            QColor halo = t.well; halo.setAlpha(220);
            p.setPen(halo);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if (dx || dy) p.drawText(box.translated(dx, dy), Qt::AlignCenter, txt);
            p.setPen(hp >= -0.5f ? t.meterPeak : bbcolor::mix(t.text, t.well, 0.15));
            p.drawText(box, Qt::AlignCenter, txt);
        }
    }

    // --- frame -------------------------------------------------------------
    p.setPen(m_hover ? t.accent : t.border);
    p.setBrush(Qt::NoBrush);
    p.drawRect(0, top, w - 1, span - 1);
}

// ---------------------------------------------------------------------------
// EqThumb
// ---------------------------------------------------------------------------
EqThumb::EqThumb(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setToolTip("This bus's parametric EQ. Click to edit.");
}

QSize EqThumb::sizeHint() const        { return QSize(bbui::px(100), bbui::px(64)); }
QSize EqThumb::minimumSizeHint() const { return QSize(bbui::px(40),  bbui::px(28)); }

void EqThumb::setCurve(const QVector<float>& db, bool on)
{
    if (on == m_on && db == m_db) return;
    m_db = db;
    m_on = on;
    update();
}

void EqThumb::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(1, 1, -1, -1);

    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());

    // +/-15 dB, with a unity line: the same range the editor opens at.
    constexpr double kSpan = 15.0;
    auto yFor = [&](double db) {
        return r.center().y() - qBound(-kSpan, db, kSpan) / kSpan * (r.height() / 2 - 2);
    };
    p.setPen(QPen(bbcolor::mix(t.well, t.text, 0.22), 1.0));
    p.drawLine(QPointF(r.left() + 2, yFor(0)), QPointF(r.right() - 2, yFor(0)));

    if (m_db.size() >= 2) {
        QPainterPath path;
        for (int i = 0; i < m_db.size(); ++i) {
            const double x = r.left() + r.width() * double(i) / (m_db.size() - 1);
            const double y = yFor(m_db.at(i));
            if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        // A bypassed block still shows its shape, so you can see what turning it
        // on would do - just not as though it were doing it.
        p.setPen(QPen(m_on ? t.eqOn : bbcolor::mix(t.eqOn, t.well, 0.62), 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    p.setPen(QPen(m_hover ? t.accent : t.border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());

    if (!m_on) {
        QFont f = p.font();
        f.setPixelSize(qMax(7, bbui::fs(8)));
        p.setFont(f);
        p.setPen(bbcolor::ensureContrast(t.textDim, t.well, 3.0));
        p.drawText(r.adjusted(4, 2, -4, 0), Qt::AlignLeft | Qt::AlignTop, "EQ OFF");
    }
}

void EqThumb::mousePressEvent(QMouseEvent*) { if (m_onClick) m_onClick(); }
void EqThumb::enterEvent(QEnterEvent*) { m_hover = true;  update(); }
void EqThumb::leaveEvent(QEvent*)      { m_hover = false; update(); }

// ---------------------------------------------------------------------------
// ElidedLabel
// ---------------------------------------------------------------------------
ElidedLabel::ElidedLabel(const QString& text, QWidget* parent)
    : QLabel(parent), m_full(text)
{
    QLabel::setText(text);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
}

void ElidedLabel::setText(const QString& t)
{
    m_full = t;
    QLabel::setText(t);
    setToolTip(QString());
    update();
}

QSize ElidedLabel::minimumSizeHint() const
{
    QSize s = QLabel::minimumSizeHint();
    s.setWidth(bbui::px(24));
    return s;
}

void ElidedLabel::paintEvent(QPaintEvent* e)
{
    // Paint the plate through the stylesheet, then the shortened text over it.
    QString keep = QLabel::text();
    const QString shown = fontMetrics().elidedText(m_full, Qt::ElideRight,
                                                   qMax(0, contentsRect().width() - 2));
    if (shown != keep) QLabel::setText(shown);
    if (shown != m_full && toolTip().isEmpty()) setToolTip(m_full);
    QLabel::paintEvent(e);
}

// ---------------------------------------------------------------------------
// DeviceCombo
// ---------------------------------------------------------------------------
void DeviceCombo::paintEvent(QPaintEvent*)
{
    QStylePainter p(this);
    QStyleOptionComboBox opt;
    initStyleOption(&opt);
    p.drawComplexControl(QStyle::CC_ComboBox, opt);

    QRect r = style()->subControlRect(QStyle::CC_ComboBox, &opt,
                                      QStyle::SC_ComboBoxEditField, this);
    // QCommonStyle adjusts the label rect by a further pixel on each side, so
    // eliding to the sub-control rect alone still clips the ellipsis away.
    r = r.adjusted(1, 0, -2, 0);
    opt.currentText = fontMetrics().elidedText(currentText(), Qt::ElideRight,
                                               qMax(0, r.width()));
    p.drawControl(QStyle::CE_ComboBoxLabel, opt);
}
