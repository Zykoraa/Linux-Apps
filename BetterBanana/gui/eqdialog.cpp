#include "eqdialog.h"
#include "color.h"
#include "dialogbits.h"
#include "metrics.h"
#include "theme.h"

#include "../common/eqprofile.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

using namespace bb;


static const char* kAutoEqBase =
    "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/";

static QPushButton* makeToggle(const QString& text, const char* role)
{
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setFixedHeight(bbui::rowH());
    b->setMinimumWidth(1);
    b->setProperty("role", role);
    return b;
}

static QLabel* makeLabel(const QString& text, const char* role,
                         Qt::Alignment a = Qt::AlignHCenter)
{
    auto* l = new QLabel(text);
    l->setProperty("role", role);
    l->setAlignment(a);
    return l;
}

static QString fmtHz(double f)
{
    return f >= 1000.0 ? QString::number(f / 1000.0, 'g', 3) + "k"
                       : QString::number(f, 'f', 0);
}

// ---------------------------------------------------------------------------
// A spin box whose arrows and wheel step by a ratio rather than a fixed amount,
// so one notch means the same musical distance at 40 Hz and at 12 kHz.
// ---------------------------------------------------------------------------
class LogSpin : public QDoubleSpinBox {
public:
    LogSpin(double ratio, QWidget* parent = nullptr)
        : QDoubleSpinBox(parent), m_ratio(ratio) {}
    void stepBy(int steps) override
    {
        const double v = value() * std::pow(m_ratio, steps);
        setValue(v);
    }
private:
    double m_ratio;
};

// ---------------------------------------------------------------------------
// EqCurve
// ---------------------------------------------------------------------------
static constexpr double kFLo = 20.0, kFHi = 20000.0;

// The spectrum shares the plot area but not its scale: 90 dB of level over the
// full height. The two used to share one set of horizontal lines, which stops
// being possible once the dB axis moves to fit the curve - and a level scale
// that relabels itself every time a band is nudged is worse than one that never
// moves. So the level scale keeps its own round decades down the right margin.
static constexpr double kSpecTop = 0.0, kSpecBot = -90.0;

// The steps the dB axis is allowed to take. Nothing below 12, because a flat
// curve drawn on a +/-3 frame reads as noise rather than as flat; 48 is a -24
// preamp stacked under a +24 band, which is as far as the controls go.
static constexpr int    kDbStepN = 5;
static constexpr double kDbSteps[kDbStepN] = { 12.0, 18.0, 24.0, 36.0, 48.0 };

EqCurve::EqCurve(Shared* shm, EqParams* eq, int specSource, QWidget* parent)
    : QWidget(parent), m_shm(shm), m_eq(eq), m_spec(specSource)
{
    setMinimumHeight(bbui::px(220));
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    for (int b = 0; b < kSpecBins; ++b) m_hold[b] = float(kSpecBot);
    for (int k = 0; k < kEqBands; ++k) m_fan[k] = 0.0;
    m_holdClock.start();
}

void EqCurve::setSelected(int band)
{
    if (band == m_sel) return;
    m_sel = band;
    update();
}

bool EqCurve::spectrumLive() const
{
    return m_spec != kSpecNone
        && m_shm->spec.active.load(std::memory_order_relaxed) == m_spec
        && m_shm->spec.seq.load(std::memory_order_relaxed) > 0;
}

QRectF EqCurve::cardRect() const
{
    // Half a pixel in, so the 1px border lands on whole pixels.
    return QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
}

QRectF EqCurve::plotRect() const
{
    // The top margin is the readout strip, which used to be drawn straight over
    // the curve it describes. The right margin holds the level scale and is
    // reserved whether or not the analyser is running, so the plot does not
    // jump the moment it starts.
    return cardRect().adjusted(bbui::px(34), bbui::px(28),
                               m_spec == kSpecNone ? -bbui::px(10) : -bbui::px(38),
                               -bbui::px(18));
}

QRectF EqCurve::readoutRect() const
{
    const QRectF c = cardRect();
    return QRectF(c.left() + bbui::px(6), c.top() + bbui::px(5),
                  c.width() - bbui::px(12), bbui::px(18));
}

double EqCurve::xForFreq(double f) const
{
    const QRectF r = plotRect();
    const double t = std::log(std::max(f, kFLo) / kFLo) / std::log(kFHi / kFLo);
    return r.left() + r.width() * t;
}

double EqCurve::freqForX(double x) const
{
    const QRectF r = plotRect();
    const double t = (x - r.left()) / std::max(r.width(), 1.0);
    return kFLo * std::pow(kFHi / kFLo, std::clamp(t, 0.0, 1.0));
}

double EqCurve::yForDb(double db) const
{
    const QRectF r = plotRect();
    return r.center().y() - (std::clamp(db, -m_range, m_range) / m_range) * (r.height() / 2);
}

double EqCurve::dbForY(double y) const
{
    const QRectF r = plotRect();
    return std::clamp(-(y - r.center().y()) / (r.height() / 2) * m_range, -m_range, m_range);
}

double EqCurve::yForSpec(double dbfs) const
{
    const QRectF r = plotRect();
    const double t = (dbfs - kSpecBot) / (kSpecTop - kSpecBot);
    return r.bottom() - std::clamp(t, 0.0, 1.0) * r.height();
}

QPointF EqCurve::handlePos(int band) const
{
    const int type = m_eq->type[band].load();
    const double base = m_eq->preamp_db.load();
    const double db = eq_type_uses_gain(type) ? base + m_eq->gain[band].load() : base;
    return QPointF(xForFreq(m_eq->freq[band].load()), yForDb(db));
}

// Handles landing on the same point. The Telephone preset puts two bands on
// 300 Hz and two on 3400 Hz, and the second of each pair was drawn exactly under
// the first, so nothing could ever select it. Offsetting alternately up and down
// is cosmetic - a drag still writes the frequency and gain the pointer is over -
// but it makes both halves of a stacked pair reachable.
void EqCurve::fanHandles() const
{
    const double touch = bbui::px(13);   // closer than this and they overlap
    const double lift  = bbui::px(17);   // one handle diameter, so both show
    for (int k = 0; k < kEqBands; ++k) {
        const QPointF p = handlePos(k);
        double off = 0.0;
        for (int slot = 0; slot < kEqBands; ++slot) {
            bool clear = true;
            for (int j = 0; j < k && clear; ++j) {
                const QPointF q = handlePos(j);
                clear = std::fabs(p.x() - q.x()) >= touch
                     || std::fabs(p.y() + off - q.y() - m_fan[j]) >= touch;
            }
            if (clear) break;
            off = (slot % 2 ? 1 : -1) * (slot / 2 + 1) * lift;
        }
        m_fan[k] = off;
    }
}

QPointF EqCurve::drawPos(int band) const
{
    return handlePos(band) + QPointF(0.0, m_fan[band]);
}

int EqCurve::bandAt(const QPoint& pt) const
{
    // What is picked has to be what is drawn, so the offsets are recomputed
    // here too: a value typed into the table can stack two handles between one
    // paint and the next click.
    fanHandles();
    const double reach = bbui::px(11);
    int best = -1;
    double bestD = reach * reach;
    for (int k = 0; k < kEqBands; ++k) {
        const QPointF h = drawPos(k);
        const double dx = h.x() - pt.x(), dy = h.y() - pt.y();
        const double d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

// A smooth path through a run of points, using quadratics through their
// midpoints. The engine publishes 64 bins across 20 Hz - 20 kHz, about a third
// of an octave each, and joining them with straight segments drew the analyser
// as a visible polygon rather than as a spectrum.
static QPainterPath smoothPath(const QVector<QPointF>& pts)
{
    QPainterPath path;
    if (pts.isEmpty()) return path;
    path.moveTo(pts.first());
    for (int i = 1; i + 1 < pts.size(); ++i) {
        const QPointF mid((pts[i].x() + pts[i + 1].x()) / 2.0,
                          (pts[i].y() + pts[i + 1].y()) / 2.0);
        path.quadTo(pts[i], mid);
    }
    path.lineTo(pts.last());
    return path;
}

// The engine's own analysis of whatever this EQ sits in, drawn behind the
// curve so a boom or a whistle can be seen rather than guessed at.
void EqCurve::drawSpectrum(QPainter& p, const QRectF& r) const
{
    if (m_spec == kSpecNone) return;
    const Theme& t = theme();

    // Direction rather than a fixed alpha. meterLow composited at alpha 56 over
    // the plot ground measured 1.22:1 on Catppuccin Latte: on a light theme an
    // overlay has to go darker than what it lies on, not lighter.
    const QColor spec = bbcolor::ensureContrast(t.meterLow, t.well, bbcolor::kBoundFloor);

    if (!spectrumLive()) {
        // There used to be nothing at all here between opening the dialog and
        // the engine's first publish, which reads as a broken analyser rather
        // than one that has not started yet.
        p.setPen(QPen(dimOn(t, t.well), 1.0));
        p.drawText(QRectF(r.left(), r.bottom() - bbui::px(30), r.width(), bbui::px(16)),
                   Qt::AlignCenter, "waiting for audio");
        return;
    }

    const double lo = m_shm->spec.f_lo.load(std::memory_order_relaxed);
    const double hi = m_shm->spec.f_hi.load(std::memory_order_relaxed);
    if (!(hi > lo)) return;
    const double ratio = std::pow(hi / lo, 1.0 / kSpecBins);

    // The hold falls in dB per second, not per frame: the repaint timer runs at
    // 20 Hz today and the number has to keep meaning the same thing if it ever
    // changes. 12 dB/s is slow enough to read a transient off and fast enough
    // that the trace follows a fader.
    const double dt = std::clamp(m_holdClock.restart() / 1000.0, 0.0, 0.5);

    QVector<QPointF> live, held;
    live.reserve(kSpecBins + 2);
    held.reserve(kSpecBins + 2);
    for (int b = 0; b < kSpecBins; ++b) {
        const float v = m_shm->spec.bin_db[b].load(std::memory_order_relaxed);
        m_hold[b] = v >= m_hold[b] ? v : std::max(v, float(m_hold[b] - 12.0 * dt));
        // A band's geometric centre is its midpoint on a log axis.
        const double f = lo * std::pow(ratio, b + 0.5);
        const double x = std::clamp(xForFreq(f), r.left(), r.right());
        live.push_back(QPointF(x, yForSpec(v)));
        held.push_back(QPointF(x, yForSpec(m_hold[b])));
    }
    live.prepend(QPointF(r.left(), live.first().y()));
    live.append(QPointF(r.right(), live.last().y()));
    held.prepend(QPointF(r.left(), held.first().y()));
    held.append(QPointF(r.right(), held.last().y()));

    const QPainterPath top = smoothPath(live);
    QPainterPath area = top;
    area.lineTo(r.right(), r.bottom());
    area.lineTo(r.left(), r.bottom());
    area.closeSubpath();

    QColor fill = spec;
    fill.setAlpha(t.dark ? 64 : 48);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(area);

    QColor edge = spec;
    edge.setAlpha(t.dark ? 190 : 235);
    p.setPen(QPen(edge, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(top);

    // The held trace borrows the meters' own peak-hold colour, so a held peak
    // means the same thing here as it does on the meter bridge.
    QColor hold = bbcolor::ensureContrast(t.meterHold, t.well, bbcolor::kBoundFloor);
    hold.setAlpha(t.dark ? 125 : 165);
    p.setPen(QPen(hold, 1.0));
    p.drawPath(smoothPath(held));
}

// Two tiers, so the axis can actually be read. Every line used to be drawn in
// one dotted weight - t.border on the plot ground, which is #333a45 on #12151a
// in the default theme - and the eight verticals drawn did not correspond to
// the five labels printed beneath them.
void EqCurve::drawGrid(QPainter& p, const QRectF& r) const
{
    const Theme& t = theme();
    const QColor base = bbcolor::ensureContrast(t.border, t.well, bbcolor::kBoundFloor);
    QColor major = base; major.setAlpha(150);
    QColor minor = base; minor.setAlpha(64);

    auto vline = [&](double f) {
        const double x = std::round(xForFreq(f)) + 0.5;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    };
    p.setPen(QPen(minor, 1.0));
    for (double dec = 10.0; dec <= 10000.0; dec *= 10.0)
        for (int m = 2; m <= 9; ++m) {
            const double f = dec * m;
            if (f <= kFLo || f >= kFHi) continue;      // the ends are the frame
            vline(f);
        }
    p.setPen(QPen(major, 1.0));
    for (double f : { 100.0, 1000.0, 10000.0 }) vline(f);

    // Horizontals follow the adaptive range: a 6 dB ladder on the two tightest
    // frames and a 12 dB one above them, so the count stays between one and
    // three pairs however far the frame has opened up.
    const double step = m_range >= 24.0 ? 12.0 : 6.0;
    for (double db = step; db < m_range - 0.5; db += step)
        for (double s : { db, -db }) {
            const double y = std::round(yForDb(s)) + 0.5;
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }

    // Unity is the axis, not another gridline.
    p.setPen(QPen(dimOn(t, t.well), 1.4));
    p.drawLine(QPointF(r.left(), yForDb(0)), QPointF(r.right(), yForDb(0)));
}

// Whatever is under the pointer, on a plate of its own above the plot. It used
// to share one rectangle with the preamp readout, unplated, in the same faint
// font as the axis labels, directly on top of where the spectrum is drawn.
void EqCurve::drawReadout(QPainter& p) const
{
    const Theme& t = theme();
    const QRectF strip = readoutRect();
    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(strip, bbui::radWell(), bbui::radWell());

    QFont f = p.font();
    f.setPixelSize(bbui::fsControl());
    bbui::makeTabular(f);
    p.setFont(f);
    const QFontMetricsF fm(f);
    const QColor ink = bbcolor::ensureContrast(t.text, t.well, bbcolor::kTextFloor);
    const QColor dim = dimOn(t, t.well);

    // Every field is as wide as its own widest possible value, so nothing in
    // the strip shifts sideways while a handle is being dragged.
    double x = strip.left() + bbui::px(6);
    auto cell = [&](const QString& text, const QString& widest, int align, const QColor& c) {
        const double w = fm.horizontalAdvance(widest);
        p.setPen(QPen(c, 1.0));
        p.drawText(QRectF(x, strip.top(), w, strip.height()), align | Qt::AlignVCenter, text);
        x += w + bbui::px(8);
    };

    const int show = m_drag >= 0 ? m_drag : (m_hover >= 0 ? m_hover : m_sel);
    if (show >= 0 && show < kEqBands) {
        const int type = m_eq->type[show].load();
        const bool live = m_eq->band_on[show].load() != 0;

        // The same chip the handle carries, so the strip names a band you can
        // point at rather than a number you have to go looking for.
        const QColor chip = bbcolor::fitFill(live ? (show == m_sel ? t.solo : t.accent)
                                                  : t.header, bbcolor::kTextFloor);
        const double d = bbui::px(15);
        const QRectF box(x, strip.center().y() - d / 2, d, d);
        p.setBrush(chip);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(box, bbui::radWell(), bbui::radWell());
        p.setPen(QPen(onFill(chip), 1.0));
        p.drawText(box, Qt::AlignCenter, QString::number(show + 1));
        x += d + bbui::px(8);

        cell(eq_type_name(type), "High shelf", Qt::AlignLeft, ink);
        cell(fmtHz(m_eq->freq[show].load()) + " Hz", "20000 Hz", Qt::AlignRight, ink);
        cell(eq_type_uses_gain(type)
                 ? QString("%1 dB").arg(m_eq->gain[show].load(), 0, 'f', 1) : QString(),
             "-24.0 dB", Qt::AlignRight, ink);
        cell(QString("Q %1").arg(m_eq->q[show].load(), 0, 'f', 2), "Q 20.00",
             Qt::AlignRight, ink);
        if (!live) cell("bypassed", "bypassed", Qt::AlignLeft, dim);
    }

    // The preamp keeps the right-hand end to itself: the two readouts used to
    // be drawn into the identical rectangle, one left and one right.
    if (std::fabs(m_eq->preamp_db.load()) > 0.05) {
        p.setPen(QPen(dim, 1.0));
        p.drawText(strip.adjusted(0, 0, -bbui::px(6), 0), Qt::AlignRight | Qt::AlignVCenter,
                   QString("preamp %1 dB").arg(m_eq->preamp_db.load(), 0, 'f', 1));
    }
}

void EqCurve::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QFont small = p.font();
    small.setPixelSize(bbui::fsCaption());
    bbui::makeTabular(small);
    p.setFont(small);

    // The plot is a well sunk into a card, not a slab floating on the dialog:
    // the readout plate and the axis labels need a plane to sit on that is not
    // the plot ground itself.
    const QRectF card = cardRect();
    p.setPen(QPen(t.border, 1.0));
    p.setBrush(t.panel);
    p.drawRoundedRect(card, bbui::radCard(), bbui::radCard());

    const float sr = m_shm->samplerate.load();
    const bool on = m_eq->on.load() != 0;

    // Design first: the range the frame spans is chosen to fit what is about to
    // be drawn on it.
    Biquad band[kEqBands];
    bool live[kEqBands];
    for (int k = 0; k < kEqBands; ++k) {
        live[k] = m_eq->band_on[k].load() != 0;
        design_band(band[k], m_eq->type[k].load(), sr,
                    m_eq->freq[k].load(), m_eq->q[k].load(), m_eq->gain[k].load());
    }
    const double preamp = m_eq->preamp_db.load();

    // Not while a handle is being dragged: the drag reads its gain back off the
    // pointer's y, so a range moving underneath it would fight the gesture.
    if (m_drag < 0) {
        // Only the bands that carry a gain of their own are fitted. A high-pass
        // skirt has no bottom, and a frame stretched to hold one squeezes every
        // peak and shelf in the block into a sliver: fitting the Telephone
        // preset's roll-offs asks for +/-48 to show 4 dB of boost. A roll-off
        // is allowed to leave the picture, as it does in every other EQ.
        double peak = std::fabs(preamp);
        bool skirt = false;
        for (int i = 0; i <= 96; ++i) {
            const double f = kFLo * std::pow(kFHi / kFLo, i / 96.0);
            double db = preamp;
            for (int k = 0; k < kEqBands; ++k)
                if (live[k] && eq_type_uses_gain(m_eq->type[k].load()))
                    db += band[k].magnitude_db(sr, float(f));
            peak = std::max(peak, std::fabs(db));
        }
        // Each band's own trace is drawn too, and a shelf's extreme lies off the
        // end of the sweep rather than on it.
        for (int k = 0; k < kEqBands; ++k) {
            if (!live[k]) continue;
            if (eq_type_uses_gain(m_eq->type[k].load()))
                peak = std::max(peak, std::fabs(preamp + m_eq->gain[k].load()));
            else skirt = true;
        }
        // A roll-off still needs enough frame to be read as a roll-off.
        if (skirt) peak = std::max(peak, 16.0);

        // Grow at once, shrink only with margin. A frame that flips between two
        // steps as a band is nudged is worse than one a step too big.
        auto fit = [&](double margin) {
            for (int i = 0; i < kDbStepN; ++i)
                if (kDbSteps[i] >= peak + margin) return kDbSteps[i];
            return kDbSteps[kDbStepN - 1];
        };
        const double want = fit(2.0);
        if (want > m_range)      m_range = want;
        else if (want < m_range) m_range = std::min(m_range, fit(4.0));
    }
    fanHandles();

    const QRectF r = plotRect();
    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());

    // Spectrum first: it is the floor everything else is read against.
    p.save();
    p.setClipRect(r);
    drawSpectrum(p, r);
    drawGrid(p, r);
    p.restore();

    // Scales, in the margins: EQ gain on the left, and the level the spectrum is
    // drawn on down the right.
    const QColor axis = dimOn(t, t.panel);
    p.setPen(QPen(axis, 1.0));
    const double step = m_range >= 24.0 ? 12.0 : 6.0;
    QVector<double> marks{ 0.0, m_range, -m_range };
    for (double db = step; db < m_range - 0.5; db += step) marks << db << -db;
    for (double db : marks) {
        QRectF box(0, yForDb(db) - bbui::px(7), r.left() - bbui::px(5), bbui::px(14));
        if (box.top() < 0)          box.moveTop(0);
        if (box.bottom() > height()) box.moveBottom(height());
        p.drawText(box, Qt::AlignRight | Qt::AlignVCenter,
                   db > 0 ? QString("+%1").arg(db, 0, 'f', 0) : QString::number(db, 'f', 0));
    }
    if (m_spec != kSpecNone) {
        QColor lab = spectrumLive()
                       ? bbcolor::ensureContrast(t.meterLow, t.panel, bbcolor::kTextFloor)
                       : axis;
        p.setPen(QPen(lab, 1.0));
        for (double dbfs : { 0.0, -20.0, -40.0, -60.0, -80.0 })
            p.drawText(QRectF(r.right() + bbui::px(4), yForSpec(dbfs) - bbui::px(7),
                              bbui::px(32), bbui::px(14)),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(int(dbfs)));
        p.setPen(QPen(axis, 1.0));
    }
    // Labelled on the majors, plus the two ends of the axis. The end labels sit
    // on the frame, so centring them on the tick would run half of each off the
    // widget - and on the right, straight through the level scale, where "20k"
    // and "-90" printed on top of each other.
    const double xmax = m_spec == kSpecNone ? card.right() : r.right();
    for (double f : { 20.0, 100.0, 1000.0, 10000.0, 20000.0 }) {
        QRectF box(xForFreq(f) - bbui::px(24), r.bottom() + bbui::px(2),
                   bbui::px(48), bbui::px(14));
        int flags = Qt::AlignHCenter | Qt::AlignTop;
        if (box.left() < 0)     { box.moveLeft(0);     flags = Qt::AlignLeft  | Qt::AlignTop; }
        if (box.right() > xmax) { box.moveRight(xmax); flags = Qt::AlignRight | Qt::AlignTop; }
        p.drawText(box, flags, fmtHz(f));
    }

    // Per-band responses, faint, then the sum on top.
    const int steps = std::max(2, int(r.width()));
    auto sweep = [&](int only) {
        QPainterPath path;
        for (int i = 0; i <= steps; ++i) {
            const double f = freqForX(r.left() + r.width() * i / steps);
            double db = only < 0 ? preamp : 0.0;
            if (only < 0) {
                for (int k = 0; k < kEqBands; ++k)
                    if (live[k]) db += band[k].magnitude_db(sr, float(f));
            } else {
                db = preamp + band[only].magnitude_db(sr, float(f));
            }
            const QPointF pt(r.left() + r.width() * i / steps, yForDb(db));
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        return path;
    };

    p.save();
    p.setClipRect(r.adjusted(-1, -1, 1, 1));
    // drawPath strokes AND fills, and the brush is still the plot ground from
    // the fillRect above: an unset brush here paints an opaque wedge under
    // every sweep and rubs out the spectrum behind it.
    p.setBrush(Qt::NoBrush);
    QColor faint = t.accent;
    faint.setAlpha(on ? 70 : 40);
    for (int k = 0; k < kEqBands; ++k) {
        if (!live[k]) continue;
        const int type = m_eq->type[k].load();
        if (eq_type_uses_gain(type) && std::fabs(m_eq->gain[k].load()) < 0.05f) continue;
        p.setPen(QPen(k == m_sel ? t.solo : faint, k == m_sel ? 1.4 : 1.0));
        p.drawPath(sweep(k));
    }
    p.setPen(QPen(on ? t.accent : dimOn(t, t.well), 2.0));
    p.drawPath(sweep(-1));
    p.restore();

    // Handles. Drawn in reverse pick order - hovered second last, selected last
    // - so a crowded right-hand edge cannot bury the one being edited under the
    // band that happens to come after it.
    auto drawHandle = [&](int k) {
        const QPointF h = drawPos(k);
        // A handle sitting on the frame is still half inside it, so the cull
        // allows a radius either way - and has to grow with the handle.
        const double slop = bbui::px(12);
        if (!r.adjusted(-slop, -slop, slop, slop).contains(h)) return;
        const bool sel = k == m_sel;
        const bool hot = k == m_hover || k == m_drag;
        const double rad = sel ? bbui::px(9) : bbui::px(8);

        QColor fill = bbcolor::fitFill(live[k] ? (sel ? t.solo : t.accent) : t.header,
                                       bbcolor::kTextFloor);
        // lighter() clamps once a channel is at 255, which left a hot handle in
        // a bright palette looking identical to a cold one.
        if (hot) fill = bbcolor::hoverOf(fill);
        QPen ring(bbcolor::ensureContrast(live[k] ? t.text : t.textDim, t.well,
                                          bbcolor::kBoundFloor),
                  sel ? 1.6 : 1.0);
        // A bypassed band keeps its place on the curve but stops claiming to be
        // part of it.
        if (!live[k]) ring.setStyle(Qt::DashLine);
        p.setBrush(fill);
        p.setPen(ring);
        p.drawEllipse(h, rad, rad);

        // The digit used to be painted in t.bg whatever the handle was filled
        // with: 1.07:1 on an off band in Rose Pine, 1.9:1 on the selected
        // handle in Latte. The ink comes from the fill now.
        QFont hf = small;
        hf.setPixelSize(bbui::fsChip());
        hf.setBold(true);
        p.setFont(hf);
        p.setPen(QPen(onFill(fill), 1.0));
        p.drawText(QRectF(h.x() - rad, h.y() - rad, rad * 2, rad * 2),
                   Qt::AlignCenter, QString::number(k + 1));
        p.setFont(small);
    };
    for (int k = 0; k < kEqBands; ++k)
        if (k != m_sel && k != m_hover) drawHandle(k);
    if (m_hover >= 0 && m_hover != m_sel) drawHandle(m_hover);
    if (m_sel >= 0 && m_sel < kEqBands)   drawHandle(m_sel);

    drawReadout(p);

    p.setPen(QPen(t.border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, bbui::radWell(), bbui::radWell());
}

void EqCurve::mousePressEvent(QMouseEvent* e)
{
    const int k = bandAt(e->pos());
    if (k < 0) return;
    if (e->button() == Qt::RightButton) {
        emit editStarted(k);
        m_eq->band_on[k].store(m_eq->band_on[k].load() ? 0 : 1);
        setSelected(k);
        emit bandToggled(k);
        update();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    emit editStarted(k);
    setCursor(Qt::ClosedHandCursor);
    // Where the pointer sits relative to the handle's TRUE position. A handle
    // that has been fanned aside to clear a coincident one is drawn up to 17px
    // from where its gain actually is, so mapping the raw pointer y straight to
    // a gain snapped the band by exactly that offset the moment you grabbed it.
    m_grabOff = e->position() - handlePos(k);
    m_drag = k;
    setSelected(k);
    emit bandSelected(k);
    update();
}

void EqCurve::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag < 0) {
        const int h = bandAt(e->pos());
        // The cursor never changed over a handle, so a thing you can pick up
        // looked the same as the empty plot beside it.
        setCursor(h >= 0 ? Qt::OpenHandCursor : Qt::CrossCursor);
        if (h != m_hover) { m_hover = h; update(); }
        return;
    }
    const QPointF at = e->position() - m_grabOff;
    m_eq->freq[m_drag].store(float(std::clamp(freqForX(at.x()), 10.0, 24000.0)));
    if (eq_type_uses_gain(m_eq->type[m_drag].load())) {
        const double g = dbForY(at.y()) - m_eq->preamp_db.load();
        m_eq->gain[m_drag].store(float(std::clamp(g, -24.0, 24.0)));
    }
    emit bandEdited(m_drag);
    update();
}

void EqCurve::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_drag < 0) return;
    m_drag = -1;
    setCursor(bandAt(e->pos()) >= 0 ? Qt::OpenHandCursor : Qt::CrossCursor);
    update();
}

void EqCurve::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int k = bandAt(e->pos());
    if (k < 0) return;
    emit editStarted(k);
    m_eq->gain[k].store(0.0f);
    setSelected(k);
    emit bandEdited(k);
    update();
}

void EqCurve::wheelEvent(QWheelEvent* e)
{
    const int k = m_hover >= 0 ? m_hover : m_sel;
    if (k < 0 || k >= kEqBands) { e->ignore(); return; }
    const int notches = e->angleDelta().y() / 120;
    if (!notches) { e->ignore(); return; }
    emit editStarted(k);
    const double q = m_eq->q[k].load() * std::pow(1.12, notches);
    m_eq->q[k].store(float(std::clamp(q, 0.1, 20.0)));
    setSelected(k);
    emit bandEdited(k);
    update();
    e->accept();
}

void EqCurve::leaveEvent(QEvent*)
{
    setCursor(Qt::CrossCursor);
    if (m_hover != -1) { m_hover = -1; update(); }
}

// ---------------------------------------------------------------------------
// EqEditorDialog
// ---------------------------------------------------------------------------
EqEditorDialog::EqEditorDialog(Shared* shm, EqParams* eq, int specSource,
                               const QString& title, int bus, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_eq(eq), m_bus(bus), m_spec(specSource),
      m_title(title), m_note(this)
{
    setWindowTitle(title + " - parametric EQ");

    // Ask the engine to analyse this signal for as long as the dialog is up.
    // Only one analysis runs at a time, which is all anyone can look at.
    if (m_spec != kSpecNone) m_shm->spec.source.store(m_spec);

    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);

    // Which strip or bus this is editing, inside the window. It used to be in
    // the title bar and nowhere else, and a tiling compositor never draws one.
    root->addWidget(bbdlg::header(
        m_title, QString("%1 equaliser - twelve bands. Drag a handle, or type a "
                         "value into its row.").arg(m_bus >= 0 ? "Output" : "Input")));

    // --- profile bar -------------------------------------------------------
    auto* bar = new QHBoxLayout;
    bar->setSpacing(bbui::gapS());
    bar->addWidget(makeLabel("PROFILE", "caption", Qt::AlignRight));
    m_profile = new QComboBox;
    m_profile->setMinimumWidth(bbui::px(220));
    bar->addWidget(m_profile, 1);

    auto* save = new QPushButton("Save as...");
    m_delete   = new QPushButton("Delete");
    // Delete destroys a file on disk and used to look exactly like Export.
    m_delete->setProperty("cta", "danger");
    auto* imp  = new QPushButton("Import...");
    auto* exp  = new QPushButton("Export...");
    for (QPushButton* b : { save, m_delete, imp, exp }) bar->addWidget(b);
    // A measured headphone correction belongs on the bus that drives the
    // headphones; there is nothing to look up for a microphone.
    if (m_bus >= 0) {
        auto* head = new QPushButton("Headphone EQ...");
        head->setToolTip("Search the AutoEq measurement database and apply a "
                         "correction for a specific pair of headphones.");
        connect(head, &QPushButton::clicked, this, &EqEditorDialog::openAutoEq);
        bar->addWidget(head);
    }
    root->addLayout(bar);

    connect(save,     &QPushButton::clicked, this, &EqEditorDialog::saveProfile);
    connect(m_delete, &QPushButton::clicked, this, &EqEditorDialog::deleteProfile);
    connect(imp,      &QPushButton::clicked, this, &EqEditorDialog::importProfile);
    connect(exp,      &QPushButton::clicked, this, &EqEditorDialog::exportProfile);

    // --- curve -------------------------------------------------------------
    m_curve = new EqCurve(m_shm, m_eq, m_spec);
    m_curve->setToolTip("Drag a numbered handle to set frequency and gain.\n"
                        "Wheel over it for Q, right-click to bypass that band,\n"
                        "double-click to zero its gain.\n"
                        "The shaded area behind is the live spectrum, on the\n"
                        "dBFS scale down the right-hand edge.");

    // --- band table --------------------------------------------------------
    auto* tableHost = new QWidget;
    auto* grid = new QGridLayout(tableHost);
    grid->setContentsMargins(bbui::gapS(), bbui::gapS(), bbui::gapS(), bbui::gapS());
    grid->setHorizontalSpacing(bbui::gapM());
    grid->setVerticalSpacing(bbui::gapXS());

    grid->addWidget(makeLabel("BAND", "caption"), 0, 0);
    grid->addWidget(makeLabel("ON",   "caption"), 0, 1);
    grid->addWidget(makeLabel("TYPE", "caption", Qt::AlignLeft | Qt::AlignVCenter), 0, 2);
    // The numeric headings sit over right-aligned fields, so each is inset by
    // the width of the step arrows - otherwise the heading lines up with the
    // frame of the box rather than with the digits inside it.
    const char* numHead[] = { "FREQ", "GAIN", "Q" };
    for (int col = 3; col < 6; ++col) {
        QLabel* h = makeLabel(numHead[col - 3], "caption", Qt::AlignRight | Qt::AlignVCenter);
        h->setContentsMargins(0, 0, bbui::px(20), 0);
        grid->addWidget(h, 0, col);
    }

    m_rows.resize(kEqBands);
    for (int k = 0; k < kEqBands; ++k) {
        BandRow& row = m_rows[k];

        // A plate spanning the whole row, added before the controls so it sits
        // behind them. Twelve rows of numbers with nothing banding them are
        // twelve rows you read one at a time.
        row.holder = new QWidget;
        row.holder->setAttribute(Qt::WA_StyledBackground, true);
        grid->addWidget(row.holder, k + 1, 0, 1, 6);

        row.num = makeLabel(QString::number(k + 1), "caption");
        row.on = new QCheckBox;
        row.on->setToolTip("Bypass this band without losing its settings");

        row.type = new QComboBox;
        for (int t = 0; t < kEqTypeCount; ++t) row.type->addItem(eq_type_name(t));

        row.freq = new LogSpin(std::pow(2.0, 1.0 / 12.0));   // a semitone a notch
        row.freq->setRange(10.0, 24000.0);
        row.freq->setDecimals(0);
        row.freq->setSuffix(" Hz");
        row.freq->setKeyboardTracking(false);

        row.gain = new QDoubleSpinBox;
        row.gain->setRange(-24.0, 24.0);
        row.gain->setDecimals(1);
        row.gain->setSingleStep(0.5);
        row.gain->setSuffix(" dB");
        row.gain->setKeyboardTracking(false);

        row.q = new LogSpin(1.06);
        row.q->setRange(0.1, 20.0);
        row.q->setDecimals(2);
        row.q->setKeyboardTracking(false);

        // Right-aligned and one width, so twelve rows of FREQ, GAIN and Q form
        // three columns you can run an eye down. They were flush left in boxes
        // wide enough to leave a 40px gap before the arrows, which staggered
        // the digits and put the "Hz" in five different places. Alignment and
        // width only: giving a spin box a background or a border would take the
        // widget over and lose its step arrows for good.
        for (QDoubleSpinBox* s : { row.freq, row.gain, row.q }) {
            s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            s->setMinimumWidth(bbui::px(96));
        }

        grid->addWidget(row.num,  k + 1, 0, Qt::AlignCenter);
        grid->addWidget(row.on,   k + 1, 1, Qt::AlignHCenter);
        grid->addWidget(row.type, k + 1, 2);
        grid->addWidget(row.freq, k + 1, 3);
        grid->addWidget(row.gain, k + 1, 4);
        grid->addWidget(row.q,    k + 1, 5);

        auto edited = [this, k] {
            if (m_updating) return;
            snapshot(k);
            pushBand(k);
            refreshRowEnables(k);
            m_curve->setSelected(k);
            highlight(k);
            m_curve->update();
        };
        connect(row.on,   &QCheckBox::toggled, this, edited);
        connect(row.type, &QComboBox::currentIndexChanged, this, edited);
        connect(row.freq, &QDoubleSpinBox::valueChanged, this, edited);
        connect(row.gain, &QDoubleSpinBox::valueChanged, this, edited);
        connect(row.q,    &QDoubleSpinBox::valueChanged, this, edited);
    }
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(3, 1);
    grid->setColumnStretch(4, 1);
    grid->setColumnStretch(5, 1);

    m_table = new QScrollArea;
    m_table->setWidget(tableHost);
    m_table->setWidgetResizable(true);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->setMinimumHeight(bbui::px(120));

    // The plot is the instrument and the table is its keyboard. Both used to be
    // added with stretch 1, so every pixel of a resize was split down the
    // middle and the twelve-row table ended up physically larger than the curve
    // it edits - while still hiding three of its rows behind a scrollbar.
    m_split = new QSplitter(Qt::Vertical);
    m_split->addWidget(m_curve);
    m_split->addWidget(m_table);
    m_split->setChildrenCollapsible(false);
    m_split->setCollapsible(0, false);
    m_split->setStretchFactor(0, 3);
    m_split->setStretchFactor(1, 1);
    root->addWidget(m_split, 1);

    // --- tools -------------------------------------------------------------
    auto* tools = new QHBoxLayout;
    tools->setSpacing(bbui::gapS());
    m_eqOn = makeToggle("EQ ON", "eq");
    m_eqOn->setChecked(m_eq->on.load() != 0);
    connect(m_eqOn, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_eq->on.store(b ? 1 : 0);
        m_curve->update();
    });
    tools->addWidget(m_eqOn);

    tools->addSpacing(bbui::gapM());
    tools->addWidget(makeLabel("PREAMP", "caption", Qt::AlignRight));
    m_preamp = new QDoubleSpinBox;
    m_preamp->setRange(-24.0, 12.0);
    m_preamp->setDecimals(1);
    m_preamp->setSingleStep(0.5);
    m_preamp->setSuffix(" dB");
    m_preamp->setKeyboardTracking(false);
    m_preamp->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_preamp->setMinimumWidth(bbui::px(96));
    m_preamp->setToolTip("Overall level trim applied before the bands, so a "
                         "boosted curve does not clip.");
    connect(m_preamp, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_updating) return;
        snapshot(-1);                       // one control, so one gesture
        m_eq->preamp_db.store(float(v));
        highlight(-1);
        m_curve->update();
    });
    tools->addWidget(m_preamp);
    auto* autoPre = new QPushButton("Auto");
    autoPre->setToolTip("Set the preamp to just clear the curve's highest peak");
    connect(autoPre, &QPushButton::clicked, this, &EqEditorDialog::autoPreamp);
    tools->addWidget(autoPre);

    tools->addSpacing(bbui::gapM());
    m_undoBtn = new QPushButton("Undo");
    m_undoBtn->setShortcut(QKeySequence::Undo);
    m_undoBtn->setToolTip("Step back through the edits made in this dialog (Ctrl+Z)");
    m_undoBtn->setEnabled(false);
    connect(m_undoBtn, &QPushButton::clicked, this, &EqEditorDialog::undo);
    tools->addWidget(m_undoBtn);

    auto* flat = new QPushButton("Flatten");
    connect(flat, &QPushButton::clicked, this, &EqEditorDialog::flatten);
    tools->addWidget(flat);
    tools->addStretch(1);
    root->addLayout(tools);

    // --- status and close --------------------------------------------------
    m_note.widget()->setProperty("role", "value");

    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    QBoxLayout* btns = bbdlg::buttonRow(nullptr, close);
    btns->insertWidget(0, m_note.widget(), 1);
    root->addLayout(btns);

    connect(m_curve, &EqCurve::editStarted,  this, [this](int k) { snapshot(k); });
    connect(m_curve, &EqCurve::bandEdited,   this, [this](int k) { pullFromShm(); highlight(k); });
    connect(m_curve, &EqCurve::bandToggled,  this, [this](int k) { pullFromShm(); highlight(k); });
    connect(m_curve, &EqCurve::bandSelected, this, [this](int k) { highlight(k); });

    buildProfileCombo();
    connect(m_profile, &QComboBox::currentIndexChanged, this, &EqEditorDialog::onProfileChosen);

    // The spectrum comes from the engine, so the curve has to be told to
    // repaint; 20 fps matches the rate the engine publishes at.
    if (m_spec != kSpecNone) {
        m_repaint = new QTimer(this);
        connect(m_repaint, &QTimer::timeout, this, [this] {
            const uint32_t seq = m_shm->spec.seq.load(std::memory_order_relaxed);
            if (seq != m_specSeen) { m_specSeen = seq; m_curve->update(); }
        });
        m_repaint->start(50);
    }

    pullFromShm();
    highlight(0);

    // Big enough to be an instrument, never taller than the screen it opens on:
    // a default that does not fit is simply never honoured, and the dialog then
    // always opens at its layout minimum instead. The wanted size goes through
    // the interface scale, since everything inside it does; the margin left on
    // the screen does not, because a window manager's panels are not ours to
    // scale.
    const QScreen* sc = QGuiApplication::primaryScreen();
    const QRect avail = sc ? sc->availableGeometry() : QRect(0, 0, 1280, 900);
    resize(std::min(bbui::px(980), avail.width() - 80),
           std::min(bbui::px(880), avail.height() - 80));
    bbdlg::rememberGeometry(this, "eq");

    // The table asks for what it needs; the plot gets the rest.
    //
    // A flat 3:1 of the dialog height left the band table 170px, which is 7.4
    // of its twelve rows with the last one sliced through its own widgets - a
    // table that cannot show twelve rows was the thing the splitter was added
    // to fix. Ask the table how tall twelve rows are, cap it at half the
    // dialog so the plot stays the larger half, and round to a whole row.
    {
        const int want = m_table->widget() ? m_table->widget()->sizeHint().height()
                                              : height() / 4;
        const int pitch = qMax(1, m_rows.isEmpty() ? 23 : m_rows.first().holder->sizeHint().height());
        const int chrome = m_table->height() - m_table->viewport()->height();
        int table = qBound(height() / 4, want + chrome, height() / 2);
        table -= (table - chrome) % pitch;          // no half-drawn last row
        m_split->setSizes({ height() - table, table });
    }
    const QByteArray st = QSettings("betterbanana", "gui").value("eqsplit").toByteArray();
    if (!st.isEmpty()) m_split->restoreState(st);

    // Every band spin box is setKeyboardTracking(false), so Return is exactly
    // how a typed frequency is committed - and Close carried the default, so
    // the same keystroke dismissed the editor. Escape already closes a dialog,
    // which is the affordance this one wants.
    bbdlg::tameDefaults(this);
}

void EqEditorDialog::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    // Tabular figures, re-stated after the first polish: a stylesheet font rule
    // is applied when the widget is polished and drops the OpenType feature
    // tags a constructor sets. Safe in this dialog and nowhere long-lived - it
    // is modal, so it cannot outlive a theme switch.
    if (m_tabular) return;
    m_tabular = true;
    auto tabular = [](QWidget* w) {
        QFont f = w->font();
        bbui::makeTabular(f);
        w->setFont(f);
    };
    for (const BandRow& row : m_rows)
        for (QDoubleSpinBox* s : { row.freq, row.gain, row.q }) tabular(s);
    tabular(m_preamp);
}

EqEditorDialog::~EqEditorDialog()
{
    // Stop the engine analysing the moment nobody is looking.
    if (m_spec != kSpecNone && m_shm->spec.source.load() == m_spec)
        m_shm->spec.source.store(kSpecNone);
    // Kept next to the dialog's geometry: someone who wants more table than
    // curve should have to say so once.
    QSettings("betterbanana", "gui").setValue("eqsplit", m_split->saveState());
}

void EqEditorDialog::snapshot(int tag)
{
    // A drag or a wheel spin arrives as dozens of tiny changes but is one
    // gesture; successive touches of the same control inside half a second
    // collapse into the single entry that started it.
    if (tag >= -1 && tag == m_lastTag && m_since.isValid() && m_since.elapsed() < 500) {
        m_since.restart();
        return;
    }
    m_undo.push_back(eq_snapshot(*m_eq));
    if (m_undo.size() > 64) m_undo.removeFirst();
    m_lastTag = tag;
    m_since.restart();
    m_undoBtn->setEnabled(true);
}

void EqEditorDialog::undo()
{
    if (m_undo.isEmpty()) return;
    eq_restore(*m_eq, m_undo.takeLast());
    m_lastTag = -2;                 // the next edit always starts a new entry
    m_undoBtn->setEnabled(!m_undo.isEmpty());
    pullFromShm();
    const QSignalBlocker block(m_profile);
    m_profile->setCurrentIndex(0);
    m_delete->setEnabled(false);
    m_note.say(m_undo.isEmpty() ? "undone (nothing further to undo)" : "undone");
}

// The combo lists the built-ins, then whatever is saved in the EQ directory.
// Index 0 is a placeholder meaning "whatever is on this EQ right now".
void EqEditorDialog::buildProfileCombo(const QString& select)
{
    const QSignalBlocker block(m_profile);
    m_profile->clear();
    m_profile->addItem("(current)", QString());

    m_profile->insertSeparator(m_profile->count());
    for (const EqFactoryPreset& fp : eq_factory_presets())
        m_profile->addItem(fp.name, QString("factory:") + fp.name);

    QDir dir(QString::fromStdString(eq_profile_dir()));
    const QStringList files = dir.entryList({ "*.txt" }, QDir::Files, QDir::Name);
    if (!files.isEmpty()) {
        m_profile->insertSeparator(m_profile->count());
        for (const QString& f : files) {
            const QString name = QFileInfo(f).completeBaseName();
            m_profile->addItem(name, QString("user:") + name);
        }
    }

    int idx = 0;
    if (!select.isEmpty()) {
        const int found = m_profile->findData(select);
        if (found >= 0) idx = found;
    }
    m_profile->setCurrentIndex(idx);
    const QString cur = m_profile->currentData().toString();
    m_delete->setEnabled(cur.startsWith("user:"));
}

void EqEditorDialog::onProfileChosen(int)
{
    const QString key = m_profile->currentData().toString();
    m_delete->setEnabled(key.startsWith("user:"));
    if (m_updating || key.isEmpty()) return;

    EqProfile prof;
    if (key.startsWith("factory:")) {
        if (!eq_factory_profile(key.mid(8).toStdString(), prof)) return;
    } else if (key.startsWith("user:")) {
        const QString path = QString::fromStdString(eq_profile_dir()) + "/" + key.mid(5) + ".txt";
        if (!eq_read_file(prof, path.toUtf8().constData())) {
            QMessageBox::warning(this, "BetterBanana", "Could not read " + path);
            return;
        }
        prof.name = key.mid(5).toStdString();
    } else return;

    applyProfile(prof, QString::fromStdString(prof.name));
}

void EqEditorDialog::applyProfile(const EqProfile& prof, const QString& label)
{
    snapshot(-3);
    eq_apply(*m_eq, prof);
    m_eq->on.store(1);
    pullFromShm();
    if (!label.isEmpty())
        m_note.say(QString("applied \"%1\"").arg(label));
}

void EqEditorDialog::pullFromShm()
{
    m_updating = true;
    for (int k = 0; k < kEqBands; ++k) {
        BandRow& row = m_rows[k];
        row.on->setChecked(m_eq->band_on[k].load() != 0);
        row.type->setCurrentIndex(std::clamp<int>(m_eq->type[k].load(), 0, kEqTypeCount - 1));
        row.freq->setValue(m_eq->freq[k].load());
        row.gain->setValue(m_eq->gain[k].load());
        row.q->setValue(m_eq->q[k].load());
    }
    m_preamp->setValue(m_eq->preamp_db.load());
    m_eqOn->setChecked(m_eq->on.load() != 0);
    m_updating = false;

    for (int k = 0; k < kEqBands; ++k) refreshRowEnables(k);
    restyleRows();
    m_curve->update();
}

void EqEditorDialog::pushBand(int k)
{
    const BandRow& row = m_rows[k];
    m_eq->band_on[k].store(row.on->isChecked() ? 1 : 0);
    m_eq->type[k].store(row.type->currentIndex());
    m_eq->freq[k].store(float(row.freq->value()));
    m_eq->gain[k].store(float(row.gain->value()));
    m_eq->q[k].store(float(row.q->value()));
}

void EqEditorDialog::refreshRowEnables(int k)
{
    const BandRow& row = m_rows[k];
    const bool live = row.on->isChecked();
    const bool gainy = eq_type_uses_gain(row.type->currentIndex());
    row.type->setEnabled(live);
    row.freq->setEnabled(live);
    row.q->setEnabled(live);
    // A pass filter, notch or band-pass has no gain of its own; leave the box
    // showing its stored value but make clear it is not in play.
    row.gain->setEnabled(live && gainy);
}

// The row plates: a zebra so twelve rows of numbers can be scanned across, and
// a selected row carrying the same colour its handle carries on the curve.
void EqEditorDialog::restyleRows()
{
    const Theme& t = theme();
    auto hex = [](const QColor& c) { return c.name(QColor::HexRgb); };
    // A fixed step in L*, not a blend of two palette colours: bg and panel are
    // 1.09:1 apart on Catppuccin Latte, so a mix of them bands nothing.
    const QColor zebra = bbcolor::nudge(t.bg, t.dark ? 5.0 : -5.0);

    for (int k = 0; k < kEqBands; ++k) {
        const bool sel  = k == m_selRow;
        const bool live = m_eq->band_on[k].load() != 0;
        const QString plate = sel
            ? QString("background:%1;border-left:%2px solid %3;border-radius:%4px;")
                  .arg(hex(bbcolor::mix(t.solo, t.bg, 0.78)))
                  .arg(bbui::px(3)).arg(hex(bbcolor::fitFill(t.solo, bbcolor::kTextFloor)))
                  .arg(bbui::radCtl())
            : QString("background:%1;border-radius:%2px;")
                  .arg(hex(k % 2 ? zebra : t.bg)).arg(bbui::radCtl());

        // The band number becomes the handle's own chip. The link between the
        // curve and the table used to be one 9px digit going bold. The chip is
        // wide enough for two digits: 10, 11 and 12 clipped inside a square.
        const QColor chip = live ? bbcolor::fitFill(sel ? t.solo : t.accent,
                                                    bbcolor::kTextFloor)
                                 : t.well;
        const QString chipCss =
            QString("background:%1;color:%2;font-size:%3px;font-weight:bold;"
                    "border-radius:%4px;min-width:%5px;max-width:%5px;"
                    "min-height:%6px;max-height:%6px;")
                .arg(hex(chip), hex(live ? onFill(chip) : dimOn(t, t.well)))
                .arg(bbui::fsChip()).arg(bbui::px(8))
                .arg(bbui::px(22)).arg(bbui::px(16));

        BandRow& row = m_rows[k];
        if (row.plateCss != plate)   { row.plateCss = plate;   row.holder->setStyleSheet(plate); }
        if (row.chipCss  != chipCss) { row.chipCss  = chipCss; row.num->setStyleSheet(chipCss); }
    }
}

// Marks the row matching the band selected on the curve, so the two halves of
// the dialog stay visibly connected.
void EqEditorDialog::highlight(int band)
{
    if (band >= 0 && band < kEqBands) m_selRow = band;
    restyleRows();
    // Dragging handle 11 used to mark a row below the fold.
    if (band >= 0 && band < m_rows.size())
        m_table->ensureWidgetVisible(m_rows[band].num, 0, bbui::px(36));
    // Any hand edit means the curve is no longer exactly the chosen profile.
    if (m_profile->currentIndex() != 0) {
        const QSignalBlocker block(m_profile);
        m_profile->setCurrentIndex(0);
        m_delete->setEnabled(false);
    }
}

void EqEditorDialog::autoPreamp()
{
    snapshot(-4);
    const float pa = eq_suggest_preamp(eq_capture(*m_eq));
    m_eq->preamp_db.store(pa);
    pullFromShm();
    m_note.say(QString("preamp set to %1 dB").arg(pa, 0, 'f', 1));
}

void EqEditorDialog::flatten()
{
    snapshot(-5);
    EqProfile flat;
    eq_apply(*m_eq, flat);
    pullFromShm();
    const QSignalBlocker block(m_profile);
    m_profile->setCurrentIndex(0);
    m_note.say("flattened");
}

void EqEditorDialog::saveProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "Save EQ profile", "Name:", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name.contains('/')) {
        QMessageBox::warning(this, "BetterBanana", "A profile name cannot contain '/'.");
        return;
    }
    QDir().mkpath(QString::fromStdString(eq_profile_dir()));
    const QString path = QString::fromStdString(eq_profile_dir()) + "/" + name + ".txt";
    if (QFile::exists(path)
        && QMessageBox::question(this, "BetterBanana",
                                 QString("Replace the existing profile \"%1\"?").arg(name))
           != QMessageBox::Yes)
        return;

    EqProfile prof = eq_capture(*m_eq, name.toStdString());
    if (!eq_write_file(prof, path.toUtf8().constData())) {
        QMessageBox::warning(this, "BetterBanana", "Could not write " + path);
        return;
    }
    buildProfileCombo("user:" + name);
    m_note.say(QString("saved \"%1\"").arg(name));
}

void EqEditorDialog::deleteProfile()
{
    const QString key = m_profile->currentData().toString();
    if (!key.startsWith("user:")) return;
    const QString name = key.mid(5);
    if (QMessageBox::question(this, "BetterBanana",
                              QString("Delete the saved profile \"%1\"?").arg(name))
        != QMessageBox::Yes) return;
    const QString path = QString::fromStdString(eq_profile_dir()) + "/" + name + ".txt";
    if (!QFile::remove(path)) {
        QMessageBox::warning(this, "BetterBanana", "Could not delete " + path);
        return;
    }
    buildProfileCombo();
    m_note.say(QString("deleted \"%1\"").arg(name));
}

void EqEditorDialog::importProfile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Import an Equalizer APO / AutoEq profile",
        QDir::homePath(), "EQ profiles (*.txt);;All files (*)");
    if (path.isEmpty()) return;
    EqProfile prof;
    if (!eq_read_file(prof, path.toUtf8().constData()) || prof.bands.empty()) {
        QMessageBox::warning(this, "BetterBanana",
                             "No filters found in that file.\n\nIt should be an "
                             "Equalizer APO parametric export - the same format "
                             "Peace and AutoEq use, with lines like\n\n"
                             "  Filter 1: ON PK Fc 105 Hz Gain 6.4 dB Q 0.70");
        return;
    }
    const QString label = QFileInfo(path).completeBaseName();
    applyProfile(prof, label);
    if (int(prof.bands.size()) > kEqBands)
        m_note.say(QString("applied \"%1\" - %2 of its %3 filters "
                                "(the strongest ones fit)")
                            .arg(label).arg(kEqBands).arg(prof.bands.size()));
}

void EqEditorDialog::exportProfile()
{
    QString suggested = m_title;
    suggested.replace('/', '-');
    QString path = QFileDialog::getSaveFileName(
        this, "Export as an Equalizer APO profile",
        QDir::homePath() + "/" + QString("BetterBanana %1.txt").arg(suggested),
        "EQ profiles (*.txt)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".txt", Qt::CaseInsensitive)) path += ".txt";
    EqProfile prof = eq_capture(*m_eq, QFileInfo(path).completeBaseName().toStdString());
    if (!eq_write_file(prof, path.toUtf8().constData())) {
        QMessageBox::warning(this, "BetterBanana", "Could not write " + path);
        return;
    }
    m_note.say("exported " + QFileInfo(path).fileName());
}

void EqEditorDialog::openAutoEq()
{
    if (m_bus < 0) return;
    snapshot(-6);
    AutoEqDialog dlg(m_shm, m_bus, this);
    dlg.exec();
    pullFromShm();
    if (!dlg.applied().isEmpty()) {
        buildProfileCombo("user:" + dlg.applied());
        m_note.say(QString("applied \"%1\"").arg(dlg.applied()));
    }
}

// ---------------------------------------------------------------------------
// AutoEqDialog
// ---------------------------------------------------------------------------
static QString autoeqCachePath()
{
    return QString::fromStdString(preset_dir()) + "/autoeq/INDEX.md";
}

// QSettings treats '/' as a group separator, and node names are free-form, so
// the device is percent-encoded before it is used as a key.
static QString deviceKey(const QString& device)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(device));
}

AutoEqDialog::AutoEqDialog(Shared* shm, int bus, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_bus(bus)
{
    setWindowTitle("Headphone EQ - AutoEq database");
    m_net = new QNetworkAccessManager(this);

    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header(
        "Headphone EQ",
        "Measured corrections for thousands of headphones and IEMs, from the "
        "AutoEq project. Pick yours, and the matching parametric EQ is applied "
        "to this bus."));

    auto* top = new QHBoxLayout;
    top->setSpacing(bbui::gapS());
    m_search = new QLineEdit;
    m_search->setPlaceholderText("Search - e.g.  hd 650   ·   moondrop chu   ·   airpods pro");
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 1);
    m_refresh = new QPushButton("Update list");
    m_refresh->setToolTip("Re-download the index of available headphones");
    top->addWidget(m_refresh);
    root->addLayout(top);

    m_list = new QListWidget;
    m_list->setAlternatingRowColors(true);
    root->addWidget(m_list, 1);

    // Both downloads run in a nested event loop, so the dialog was never
    // actually frozen - it just had no way of saying so. An indeterminate bar
    // under the list is the whole busy state.
    m_busy = new QProgressBar;
    m_busy->setRange(0, 0);
    m_busy->setTextVisible(false);
    m_busy->setFixedHeight(bbui::px(4));
    m_busy->hide();
    root->addWidget(m_busy);

    // Only a physical bus drives a real device worth remembering a profile for.
    if (m_bus < kPhysBuses) {
        char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
        uint32_t seq = 0;
        for (int t = 0; t < 16; ++t)
            if (routing_read(m_shm->routing, seq, hw, out)) { m_device = QString::fromUtf8(out[m_bus]); break; }
    }
    m_remember = new QCheckBox;
    if (m_device.isEmpty()) {
        m_remember->setText("Re-apply automatically for this bus's output device");
        m_remember->setEnabled(false);
        m_remember->setToolTip("Assign an output device to this bus first.");
    } else {
        // A node name is "alsa_output.usb-BEHRINGER_UMC202HD_192k_...Direct__sink",
        // and a check box does not elide its own label - so it was setting the
        // dialog's minimum width, 285px wider than anything else in it.
        m_remember->setText(
            QString("Re-apply automatically whenever this bus is set to %1")
                .arg(QFontMetrics(m_remember->font())
                         .elidedText(m_device, Qt::ElideMiddle, bbui::px(260))));
        m_remember->setToolTip(m_device);
        m_remember->setChecked(
            QSettings("betterbanana", "gui").contains("eqdevice/" + deviceKey(m_device)));
    }
    root->addWidget(m_remember);

    m_status = makeLabel("", "caption", Qt::AlignLeft);
    m_apply = new QPushButton("Apply");
    m_apply->setEnabled(false);
    auto* close = new QPushButton("Close");
    QBoxLayout* btns = bbdlg::buttonRow(m_apply, close);
    btns->insertWidget(0, m_status, 1);
    root->addLayout(btns);

    connect(close,    &QPushButton::clicked, this, &QDialog::accept);
    connect(m_apply,  &QPushButton::clicked, this, &AutoEqDialog::applySelected);
    connect(m_refresh, &QPushButton::clicked, this, &AutoEqDialog::refreshIndex);
    connect(m_search, &QLineEdit::textChanged, this, &AutoEqDialog::applyFilter);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int r) {
        m_apply->setEnabled(r >= 0 && m_list->item(r)->data(Qt::UserRole).isValid());
    });
    connect(m_list, &QListWidget::itemActivated, this, &AutoEqDialog::applySelected);

    resize(bbui::px(700), bbui::px(560));
    bbdlg::rememberGeometry(this, "autoeq");
    // Return in the search box applies the highlighted headphone, which is the
    // only thing this dialog is for.
    bbdlg::tameDefaults(this, m_apply);

    QString err;
    if (!loadIndex(&err)) {
        setStatus("No headphone list yet - press \"Update list\" to download it.");
        QTimer::singleShot(0, this, &AutoEqDialog::refreshIndex);
    } else {
        applyFilter();
    }
    m_search->setFocus();
}

void AutoEqDialog::setStatus(const QString& text, bool busy)
{
    m_status->setText(text);
    m_busy->setVisible(busy);
    // A 30 second timeout with no way out of it is a hang whatever it says on
    // the label, so the button that started the download is the one that stops
    // it. Everything else stands down while it is in flight.
    m_refresh->setText(busy ? "Cancel" : "Update list");
    QListWidgetItem* cur = m_list->currentItem();
    m_apply->setEnabled(!busy && cur && cur->data(Qt::UserRole).isValid());
    m_search->setEnabled(!busy);
    m_list->setEnabled(!busy);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

// One blocking GET, with the busy bar running and Cancel live throughout. A
// nested loop rather than a state machine because this dialog only ever has one
// request outstanding, and both callers need the answer before they continue.
QByteArray AutoEqDialog::fetch(const QUrl& url, const QString& what)
{
    QNetworkRequest req{ url };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "BetterBanana");

    m_reply = m_net->get(req);
    setStatus("Downloading " + what + "...", true);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    QNetworkReply* reply = m_reply;
    m_reply = nullptr;                      // Cancel has nothing left to abort
    QByteArray body;
    if (!reply->isFinished()) {
        reply->abort();
        setStatus("Timed out fetching " + what + " - check the network and try again.");
    } else if (reply->error() == QNetworkReply::OperationCanceledError) {
        setStatus("Cancelled.");
    } else if (reply->error() != QNetworkReply::NoError) {
        setStatus("Could not download " + what + ": " + reply->errorString());
    } else {
        body = reply->readAll();
        setStatus(QString());
    }
    reply->deleteLater();
    return body;
}

// One index line looks like
//   - [Sennheiser HD 650](./oratory1990/over-ear/Sennheiser%20HD%20650) by oratory1990
// with an optional " on <rig>" tail. The path is authoritative for the file
// name, so it is kept percent-encoded exactly as published.
bool AutoEqDialog::loadIndex(QString* err)
{
    QFile f(autoeqCachePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = "no cached index";
        return false;
    }
    m_all.clear();
    m_all.reserve(9000);
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.startsWith("- [")) continue;
        const int nameEnd = line.indexOf("](./");
        if (nameEnd < 0) continue;
        // Nearly a third of the rows have brackets in the model name, and they
        // survive into the path unescaped - "1MORE Aero (ANC Off)". The link's
        // closing paren is therefore the LAST one on the row; taking the first
        // silently drops 2500 headphones. No source or rig name contains one.
        const int pathEnd = line.lastIndexOf(')');
        if (pathEnd < nameEnd + 4) continue;

        Entry e;
        e.name = line.mid(3, nameEnd - 3);
        e.path = line.mid(nameEnd + 4, pathEnd - nameEnd - 4);
        if (e.name.isEmpty() || e.path.isEmpty()) continue;

        QString tail = line.mid(pathEnd + 1).trimmed();
        if (tail.startsWith("by ")) tail = tail.mid(3);
        const int onAt = tail.lastIndexOf(" on ");
        if (onAt > 0) { e.source = tail.left(onAt); e.rig = tail.mid(onAt + 4); }
        else          { e.source = tail; }
        m_all.push_back(e);
    }
    f.close();
    if (m_all.isEmpty()) {
        if (err) *err = "cached index is empty";
        return false;
    }
    return true;
}

void AutoEqDialog::refreshIndex()
{
    // While a download is in flight this button is the Cancel.
    if (m_reply) { m_reply->abort(); return; }

    const QByteArray body = fetch(QUrl(QString(kAutoEqBase) + "INDEX.md"),
                                  "the headphone list");
    if (body.isEmpty()) return;

    QDir().mkpath(QFileInfo(autoeqCachePath()).absolutePath());
    QFile f(autoeqCachePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus("Downloaded, but could not write " + autoeqCachePath());
        return;
    }
    f.write(body);
    f.close();

    QString err;
    if (!loadIndex(&err)) { setStatus("Downloaded list is not usable (" + err + ")"); return; }
    setStatus(QString("%1 headphones available.").arg(m_all.size()));
    applyFilter();
}

// Substring match, except that a bare number must start a word. Every row
// carries its source, and one of the sources is "oratory1990" - so a plain
// search for "990" matches that entire catalogue and buries the DT 990.
// Letters stay loose, so "pods" still finds AirPods.
static bool wordMatches(const QString& hay, const QString& word)
{
    bool numeric = !word.isEmpty();
    for (QChar c : word) if (!c.isDigit()) { numeric = false; break; }
    if (!numeric) return hay.contains(word, Qt::CaseInsensitive);
    for (int at = hay.indexOf(word, 0, Qt::CaseInsensitive); at >= 0;
         at = hay.indexOf(word, at + 1, Qt::CaseInsensitive))
        if (at == 0 || !hay.at(at - 1).isLetterOrNumber()) return true;
    return false;
}

void AutoEqDialog::applyFilter()
{
    const QStringList words = m_search->text().simplified().split(' ', Qt::SkipEmptyParts);
    m_list->clear();

    // Showing nine thousand rows helps nobody and makes the list crawl; the
    // search box is the point.
    const int kMaxRows = 400;
    int shown = 0, matched = 0;
    for (const Entry& e : m_all) {
        const QString hay = e.name + " " + e.source + " " + e.rig;
        bool ok = true;
        for (const QString& w : words)
            if (!wordMatches(hay, w)) { ok = false; break; }
        if (!ok) continue;
        ++matched;
        if (shown >= kMaxRows) continue;
        QString label = e.name + "   -   " + e.source;
        if (!e.rig.isEmpty()) label += " (" + e.rig + ")";
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, e.path);
        item->setData(Qt::UserRole + 1, e.name);
        m_list->addItem(item);
        ++shown;
    }
    if (matched > shown) {
        auto* more = new QListWidgetItem(
            QString("... and %1 more - keep typing to narrow it down").arg(matched - shown));
        more->setFlags(Qt::NoItemFlags);
        m_list->addItem(more);
    }
    if (m_all.isEmpty())      setStatus("No headphone list yet - press \"Update list\".");
    else if (matched == 0)    setStatus("Nothing matches that.");
    else                      setStatus(QString("%1 of %2 headphones").arg(matched).arg(m_all.size()));
    if (shown) m_list->setCurrentRow(0);
}

void AutoEqDialog::applySelected()
{
    QListWidgetItem* item = m_list->currentItem();
    if (!item) return;
    const QVariant pathVar = item->data(Qt::UserRole);
    if (!pathVar.isValid()) return;
    const QString path = pathVar.toString();
    const QString name = item->data(Qt::UserRole + 1).toString();

    // The file is named after the folder it sits in, which is not always the
    // display name, so take the last path segment and un-escape it.
    const QString dir = QUrl::fromPercentEncoding(path.section('/', -1).toUtf8());
    const QString file = QString::fromLatin1(
        QUrl::toPercentEncoding(dir + " ParametricEQ.txt"));
    const QUrl url(QString(kAutoEqBase) + path + "/" + file);

    const QByteArray body = fetch(url, name);
    if (body.isEmpty()) return;

    EqProfile prof = eq_parse_apo(body.toStdString());
    if (prof.bands.empty()) {
        setStatus("That entry has no parametric filters published.");
        return;
    }
    prof.name = name.toStdString();
    eq_apply(m_shm->bus[m_bus].eq, prof);
    m_shm->bus[m_bus].eq.on.store(1);

    // Keep a copy locally: it makes the profile selectable offline afterwards,
    // and gives the remembered-device mapping something stable to point at.
    QDir().mkpath(QString::fromStdString(eq_profile_dir()));
    QString safe = name;
    safe.replace('/', '-');
    const QString saved = QString::fromStdString(eq_profile_dir()) + "/" + safe + ".txt";
    if (eq_write_file(prof, saved.toUtf8().constData())) m_applied = safe;

    QSettings cfg("betterbanana", "gui");
    if (!m_device.isEmpty()) {
        const QString key = "eqdevice/" + deviceKey(m_device);
        if (m_remember->isChecked() && !m_applied.isEmpty()) cfg.setValue(key, m_applied);
        else if (!m_remember->isChecked())                   cfg.remove(key);
    }

    const int fitted = std::min<int>(int(prof.bands.size()), kEqBands);
    QString msg = QString("Applied %1 - %2 bands, preamp %3 dB.")
                      .arg(name).arg(fitted).arg(prof.preamp, 0, 'f', 1);
    if (int(prof.bands.size()) > kEqBands)
        msg += QString(" (%1 published; the strongest %2 fit)")
                   .arg(prof.bands.size()).arg(kEqBands);
    setStatus(msg);
}

QString AutoEqDialog::applyRemembered(Shared* shm, int bus, const QString& device)
{
    if (!shm || device.isEmpty() || bus < 0 || bus >= kBuses) return QString();
    QSettings cfg("betterbanana", "gui");
    const QString name = cfg.value("eqdevice/" + deviceKey(device)).toString();
    if (name.isEmpty()) return QString();

    const QString path = QString::fromStdString(eq_profile_dir()) + "/" + name + ".txt";
    EqProfile prof;
    if (!eq_read_file(prof, path.toUtf8().constData()) || prof.bands.empty()) return QString();
    eq_apply(shm->bus[bus].eq, prof);
    shm->bus[bus].eq.on.store(1);
    return name;
}
