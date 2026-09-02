#include "eqdialog.h"
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
#include <QGridLayout>
#include <QGroupBox>
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
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

using namespace bb;


static const char* kAutoEqBase =
    "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/";

static QPushButton* makeToggle(const QString& text, const char* role, int h = 22)
{
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setFixedHeight(h);
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
static constexpr double kFLo = 20.0, kFHi = 20000.0, kMaxDb = 18.0;

// The spectrum shares the plot area but not its scale: 90 dB of level across
// the same height as 36 dB of EQ. That ratio is chosen so every EQ gridline
// lands on a round level - +18 dB is 0 dBFS, 0 dB is -45, -18 dB is -90 - and
// the two scales can share one set of horizontal lines.
static constexpr double kSpecTop = 0.0, kSpecBot = -90.0;

EqCurve::EqCurve(Shared* shm, EqParams* eq, int specSource, QWidget* parent)
    : QWidget(parent), m_shm(shm), m_eq(eq), m_spec(specSource)
{
    setMinimumHeight(200);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
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

QRectF EqCurve::plotRect() const
{
    // The right margin holds the level scale. It is reserved whether or not the
    // analyser is running, so the plot does not jump the moment it starts.
    return QRectF(rect()).adjusted(30, 5, m_spec == kSpecNone ? -6 : -32, -16);
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
    return r.center().y() - (std::clamp(db, -kMaxDb, kMaxDb) / kMaxDb) * (r.height() / 2);
}

double EqCurve::dbForY(double y) const
{
    const QRectF r = plotRect();
    return std::clamp(-(y - r.center().y()) / (r.height() / 2) * kMaxDb, -kMaxDb, kMaxDb);
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

int EqCurve::bandAt(const QPoint& pt) const
{
    int best = -1;
    double bestD = 11.0 * 11.0;
    for (int k = 0; k < kEqBands; ++k) {
        const QPointF h = handlePos(k);
        const double dx = h.x() - pt.x(), dy = h.y() - pt.y();
        const double d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = k; }
    }
    return best;
}

// The engine's own analysis of whatever this EQ sits in, drawn behind the
// curve so a boom or a whistle can be seen rather than guessed at.
void EqCurve::drawSpectrum(QPainter& p, const QRectF& r) const
{
    if (!spectrumLive()) return;
    const Theme& t = theme();
    const double lo = m_shm->spec.f_lo.load(std::memory_order_relaxed);
    const double hi = m_shm->spec.f_hi.load(std::memory_order_relaxed);
    if (!(hi > lo)) return;
    const double ratio = std::pow(hi / lo, 1.0 / kSpecBins);

    QPainterPath top;
    for (int b = 0; b < kSpecBins; ++b) {
        // A band's geometric centre is its midpoint on a log axis.
        const double f = lo * std::pow(ratio, b + 0.5);
        const double x = std::clamp(xForFreq(f), r.left(), r.right());
        const QPointF pt(x, yForSpec(m_shm->spec.bin_db[b].load(std::memory_order_relaxed)));
        if (b == 0) top.moveTo(r.left(), pt.y());
        top.lineTo(pt);
    }
    top.lineTo(r.right(), top.currentPosition().y());

    QPainterPath area = top;
    area.lineTo(r.right(), r.bottom());
    area.lineTo(r.left(), r.bottom());
    area.closeSubpath();

    QColor fill = t.meterLow; fill.setAlpha(56);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPath(area);

    QColor edge = t.meterLow; edge.setAlpha(160);
    p.setPen(QPen(edge, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(top);
}

void EqCurve::paintEvent(QPaintEvent*)
{
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    p.fillRect(rect(), t.panelAlt);

    const float sr = m_shm->samplerate.load();
    const bool on = m_eq->on.load() != 0;

    QFont small = p.font();
    small.setPointSizeF(std::max(6.5, small.pointSizeF() - 2.0));
    p.setFont(small);

    // Spectrum first: it is the floor everything else is read against.
    p.save();
    p.setClipRect(r);
    drawSpectrum(p, r);
    p.restore();

    // Grid.
    p.setPen(QPen(t.border, 1.0, Qt::DotLine));
    for (double db : { -12.0, -6.0, 6.0, 12.0 })
        p.drawLine(QPointF(r.left(), yForDb(db)), QPointF(r.right(), yForDb(db)));
    static const double kFreqTicks[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    for (double f : kFreqTicks) {
        const double x = xForFreq(f);
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    p.setPen(QPen(t.textDim, 1.0));
    p.drawLine(QPointF(r.left(), yForDb(0)), QPointF(r.right(), yForDb(0)));

    // Scales. EQ gain on the left, and - when the analyser is running - the
    // level the spectrum is drawn on, on the right.
    for (double db : { -12.0, -6.0, 0.0, 6.0, 12.0 }) {
        const QString s = db > 0 ? QString("+%1").arg(int(db)) : QString::number(int(db));
        p.drawText(QRectF(0, yForDb(db) - 7, 26, 14), Qt::AlignRight | Qt::AlignVCenter, s);
    }
    if (spectrumLive()) {
        QColor lab = t.meterLow; lab.setAlpha(190);
        p.setPen(QPen(lab, 1.0));
        for (double db : { 18.0, 12.0, 6.0, 0.0, -6.0, -12.0, -18.0 }) {
            const double dbfs = kSpecBot + (db + kMaxDb) / (2 * kMaxDb) * (kSpecTop - kSpecBot);
            p.drawText(QRectF(r.right() + 3, yForDb(db) - 7, 28, 14),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(int(dbfs)));
        }
        p.setPen(QPen(t.textDim, 1.0));
    }
    // The end labels sit on the plot's edges, so centring them on the tick would
    // run half of each off the widget - and on the right, straight through the
    // level scale, where "20k" and "-90" would print on top of each other.
    const double xmax = m_spec == kSpecNone ? double(width()) : r.right();
    for (double f : { 20.0, 100.0, 1000.0, 10000.0, 20000.0 }) {
        QRectF box(xForFreq(f) - 24, r.bottom() + 1, 48, 14);
        int flags = Qt::AlignHCenter | Qt::AlignTop;
        if (box.left() < 0)      { box.moveLeft(0);      flags = Qt::AlignLeft  | Qt::AlignTop; }
        if (box.right() > xmax)  { box.moveRight(xmax);  flags = Qt::AlignRight | Qt::AlignTop; }
        p.drawText(box, flags, fmtHz(f));
    }

    // Per-band responses, faint, then the sum on top.
    Biquad band[kEqBands];
    bool live[kEqBands];
    for (int k = 0; k < kEqBands; ++k) {
        live[k] = m_eq->band_on[k].load() != 0;
        design_band(band[k], m_eq->type[k].load(), sr,
                    m_eq->freq[k].load(), m_eq->q[k].load(), m_eq->gain[k].load());
    }
    const double preamp = m_eq->preamp_db.load();
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

    QColor faint = t.accent;
    faint.setAlpha(on ? 70 : 40);
    for (int k = 0; k < kEqBands; ++k) {
        if (!live[k]) continue;
        const int type = m_eq->type[k].load();
        if (eq_type_uses_gain(type) && std::fabs(m_eq->gain[k].load()) < 0.05f) continue;
        p.setPen(QPen(k == m_sel ? t.solo : faint, k == m_sel ? 1.4 : 1.0));
        p.drawPath(sweep(k));
    }

    p.setPen(QPen(on ? t.accent : t.textDim, 2.0));
    p.drawPath(sweep(-1));

    // Handles.
    for (int k = 0; k < kEqBands; ++k) {
        const QPointF h = handlePos(k);
        if (!r.adjusted(-6, -6, 6, 6).contains(h)) continue;
        const bool sel = k == m_sel;
        const bool hot = k == m_hover || k == m_drag;
        const double rad = sel ? 8.0 : 6.5;
        QColor fill = live[k] ? (sel ? t.solo : t.accent) : t.panel;
        if (hot) fill = fill.lighter(125);
        p.setBrush(fill);
        p.setPen(QPen(live[k] ? t.text : t.textDim, sel ? 1.6 : 1.0));
        p.drawEllipse(h, rad, rad);
        p.setPen(QPen(t.bg, 1.0));
        p.drawText(QRectF(h.x() - rad, h.y() - rad, rad * 2, rad * 2),
                   Qt::AlignCenter, QString::number(k + 1));
    }

    // Read-out for whatever is under the pointer, or the selected band.
    const int show = m_drag >= 0 ? m_drag : (m_hover >= 0 ? m_hover : m_sel);
    if (show >= 0 && show < kEqBands) {
        const int type = m_eq->type[show].load();
        QString s = QString("%1  %2  %3")
                        .arg(show + 1)
                        .arg(eq_type_name(type))
                        .arg(fmtHz(m_eq->freq[show].load()) + " Hz");
        if (eq_type_uses_gain(type))
            s += QString("  %1 dB").arg(m_eq->gain[show].load(), 0, 'f', 1);
        s += QString("  Q %1").arg(m_eq->q[show].load(), 0, 'f', 2);
        if (!live[show]) s += "  (bypassed)";
        p.setPen(QPen(t.text, 1.0));
        p.drawText(QRectF(r.left() + 4, r.top() + 2, r.width() - 8, 14),
                   Qt::AlignLeft | Qt::AlignTop, s);
    }
    if (std::fabs(preamp) > 0.05) {
        p.setPen(QPen(t.textDim, 1.0));
        p.drawText(QRectF(r.left() + 4, r.top() + 2, r.width() - 8, 14),
                   Qt::AlignRight | Qt::AlignTop,
                   QString("preamp %1 dB").arg(preamp, 0, 'f', 1));
    }

    p.setPen(QPen(t.border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
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
    m_drag = k;
    setSelected(k);
    emit bandSelected(k);
    update();
}

void EqCurve::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag < 0) {
        const int h = bandAt(e->pos());
        if (h != m_hover) { m_hover = h; update(); }
        return;
    }
    m_eq->freq[m_drag].store(float(std::clamp(freqForX(e->position().x()), 10.0, 24000.0)));
    if (eq_type_uses_gain(m_eq->type[m_drag].load())) {
        const double g = dbForY(e->position().y()) - m_eq->preamp_db.load();
        m_eq->gain[m_drag].store(float(std::clamp(g, -24.0, 24.0)));
    }
    emit bandEdited(m_drag);
    update();
}

void EqCurve::mouseReleaseEvent(QMouseEvent*)
{
    if (m_drag < 0) return;
    m_drag = -1;
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
    if (m_hover != -1) { m_hover = -1; update(); }
}

// ---------------------------------------------------------------------------
// EqEditorDialog
// ---------------------------------------------------------------------------
EqEditorDialog::EqEditorDialog(Shared* shm, EqParams* eq, int specSource,
                               const QString& title, int bus, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_eq(eq), m_bus(bus), m_spec(specSource), m_title(title)
{
    setWindowTitle(title + " - parametric EQ");

    // Ask the engine to analyse this signal for as long as the dialog is up.
    // Only one analysis runs at a time, which is all anyone can look at.
    if (m_spec != kSpecNone) m_shm->spec.source.store(m_spec);

    auto* root = new QVBoxLayout(this);

    // --- profile bar -------------------------------------------------------
    auto* bar = new QHBoxLayout;
    bar->addWidget(makeLabel("PROFILE", "caption", Qt::AlignRight));
    m_profile = new QComboBox;
    m_profile->setMinimumWidth(220);
    bar->addWidget(m_profile, 1);

    auto* save = new QPushButton("Save as...");
    m_delete   = new QPushButton("Delete");
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
    root->addWidget(m_curve, 1);

    // --- band table --------------------------------------------------------
    auto* tableHost = new QWidget;
    auto* grid = new QGridLayout(tableHost);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(2);

    grid->addWidget(makeLabel("BAND", "caption"),  0, 0);
    grid->addWidget(makeLabel("ON",   "caption"),  0, 1);
    grid->addWidget(makeLabel("TYPE", "caption"),  0, 2);
    grid->addWidget(makeLabel("FREQ", "caption"),  0, 3);
    grid->addWidget(makeLabel("GAIN", "caption"),  0, 4);
    grid->addWidget(makeLabel("Q",    "caption"),  0, 5);

    m_rows.resize(kEqBands);
    for (int k = 0; k < kEqBands; ++k) {
        BandRow& row = m_rows[k];
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

        grid->addWidget(row.num,  k + 1, 0);
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
            highlight(k);
            m_curve->setSelected(k);
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

    auto* scroll = new QScrollArea;
    scroll->setWidget(tableHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(170);
    root->addWidget(scroll, 1);

    // --- bottom bar --------------------------------------------------------
    auto* btns = new QHBoxLayout;
    m_eqOn = makeToggle("EQ ON", "eq", 24);
    m_eqOn->setChecked(m_eq->on.load() != 0);
    connect(m_eqOn, &QPushButton::toggled, this, [this](bool b) {
        if (m_updating) return;
        m_eq->on.store(b ? 1 : 0);
        m_curve->update();
    });
    btns->addWidget(m_eqOn);

    btns->addSpacing(8);
    btns->addWidget(makeLabel("PREAMP", "caption", Qt::AlignRight));
    m_preamp = new QDoubleSpinBox;
    m_preamp->setRange(-24.0, 12.0);
    m_preamp->setDecimals(1);
    m_preamp->setSingleStep(0.5);
    m_preamp->setSuffix(" dB");
    m_preamp->setKeyboardTracking(false);
    m_preamp->setToolTip("Overall level trim applied before the bands, so a "
                         "boosted curve does not clip.");
    connect(m_preamp, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (m_updating) return;
        snapshot(-1);                       // one control, so one gesture
        m_eq->preamp_db.store(float(v));
        highlight(-1);
        m_curve->update();
    });
    btns->addWidget(m_preamp);
    auto* autoPre = new QPushButton("Auto");
    autoPre->setToolTip("Set the preamp to just clear the curve's highest peak");
    connect(autoPre, &QPushButton::clicked, this, &EqEditorDialog::autoPreamp);
    btns->addWidget(autoPre);

    btns->addSpacing(8);
    m_undoBtn = new QPushButton("Undo");
    m_undoBtn->setShortcut(QKeySequence::Undo);
    m_undoBtn->setToolTip("Step back through the edits made in this dialog (Ctrl+Z)");
    m_undoBtn->setEnabled(false);
    connect(m_undoBtn, &QPushButton::clicked, this, &EqEditorDialog::undo);
    btns->addWidget(m_undoBtn);

    auto* flat = new QPushButton("Flatten");
    connect(flat, &QPushButton::clicked, this, &EqEditorDialog::flatten);
    btns->addWidget(flat);

    m_note = makeLabel("", "caption", Qt::AlignLeft);
    btns->addWidget(m_note, 1);

    auto* close = new QPushButton("Close");
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    btns->addWidget(close);
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
    resize(760, 700);
}

EqEditorDialog::~EqEditorDialog()
{
    // Stop the engine analysing the moment nobody is looking.
    if (m_spec != kSpecNone && m_shm->spec.source.load() == m_spec)
        m_shm->spec.source.store(kSpecNone);
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
    m_note->setText(m_undo.isEmpty() ? "undone (nothing further to undo)" : "undone");
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
        m_note->setText(QString("applied \"%1\"").arg(label));
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

// Marks the row matching the band selected on the curve, so the two halves of
// the dialog stay visibly connected.
void EqEditorDialog::highlight(int band)
{
    for (int k = 0; k < kEqBands; ++k) {
        QFont f = m_rows[k].num->font();
        f.setBold(k == band);
        m_rows[k].num->setFont(f);
    }
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
    m_note->setText(QString("preamp set to %1 dB").arg(pa, 0, 'f', 1));
}

void EqEditorDialog::flatten()
{
    snapshot(-5);
    EqProfile flat;
    eq_apply(*m_eq, flat);
    pullFromShm();
    const QSignalBlocker block(m_profile);
    m_profile->setCurrentIndex(0);
    m_note->setText("flattened");
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
    m_note->setText(QString("saved \"%1\"").arg(name));
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
    m_note->setText(QString("deleted \"%1\"").arg(name));
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
        m_note->setText(QString("applied \"%1\" - %2 of its %3 filters "
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
    m_note->setText("exported " + QFileInfo(path).fileName());
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
        m_note->setText(QString("applied \"%1\"").arg(dlg.applied()));
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
    root->addWidget(makeLabel(
        "Measured corrections for thousands of headphones and IEMs, from the "
        "AutoEq project. Pick yours, and the matching parametric EQ is applied "
        "to this bus.", "caption", Qt::AlignLeft));

    auto* top = new QHBoxLayout;
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
        m_remember->setText(QString("Re-apply automatically whenever this bus is set to %1")
                                .arg(m_device));
        m_remember->setChecked(
            QSettings("betterbanana", "gui").contains("eqdevice/" + deviceKey(m_device)));
    }
    root->addWidget(m_remember);

    auto* btns = new QHBoxLayout;
    m_status = makeLabel("", "caption", Qt::AlignLeft);
    btns->addWidget(m_status, 1);
    m_apply = new QPushButton("Apply");
    m_apply->setDefault(true);
    m_apply->setEnabled(false);
    auto* close = new QPushButton("Close");
    btns->addWidget(m_apply);
    btns->addWidget(close);
    root->addLayout(btns);

    connect(close,    &QPushButton::clicked, this, &QDialog::accept);
    connect(m_apply,  &QPushButton::clicked, this, &AutoEqDialog::applySelected);
    connect(m_refresh, &QPushButton::clicked, this, &AutoEqDialog::refreshIndex);
    connect(m_search, &QLineEdit::textChanged, this, &AutoEqDialog::applyFilter);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int r) {
        m_apply->setEnabled(r >= 0 && m_list->item(r)->data(Qt::UserRole).isValid());
    });
    connect(m_list, &QListWidget::itemActivated, this, &AutoEqDialog::applySelected);

    resize(680, 520);

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
    setCursor(busy ? Qt::WaitCursor : Qt::ArrowCursor);
    m_apply->setEnabled(!busy && m_list->currentRow() >= 0);
    m_refresh->setEnabled(!busy);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
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
    setStatus("Downloading the headphone list...", true);

    QNetworkRequest req{ QUrl(QString(kAutoEqBase) + "INDEX.md") };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "BetterBanana");
    QNetworkReply* reply = m_net->get(req);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        setStatus("Timed out fetching the list - check the network and try again.");
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = reply->errorString();
        reply->deleteLater();
        setStatus("Could not download the list: " + msg);
        return;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();

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

    setStatus("Downloading " + name + "...", true);
    QNetworkRequest req{ url };
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "BetterBanana");
    QNetworkReply* reply = m_net->get(req);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        setStatus("Timed out downloading " + name + ".");
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = reply->errorString();
        reply->deleteLater();
        setStatus("Could not download " + name + ": " + msg);
        return;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();

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
