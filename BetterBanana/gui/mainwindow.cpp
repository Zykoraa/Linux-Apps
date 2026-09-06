#include "mainwindow.h"
#include "eqdialog.h"
#include "fxdialog.h"
#include "dialogbits.h"
#include "theme.h"
#include "color.h"
#include "metrics.h"
#include "../common/preset.h"
#include "../engine/dsp.h"
#include "../engine/surround.h"
#include "../engine/delay.h"

#include <QActionGroup>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAbstractItemView>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStyle>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QPixmap>
#include <QIcon>
#include <QProgressBar>
#include <QScreen>
#include <QSpacerItem>
#include <QCloseEvent>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QThread>
#include <algorithm>
#include <functional>
#include <cmath>
#include <csignal>

using namespace bb;

static const char* kBusLabel[kBuses] = { "A1", "A2", "A3", "B1", "B2" };

// The default name of each strip, in one place. There used to be five copies of
// this, in two different spellings; shortening the card titles updated one of
// them, and pullFromShm reverted the header from another twice a second.
static const char* kStripTitle[kStrips] = {
    "HW INPUT 1", "HW INPUT 2", "HW INPUT 3", "VAIO", "AUX"
};

static inline float sliderDb(int v) { return v / 10.0f; }
static inline int   dbSlider(float d) { return int(std::lround(d * 10.0f)); }

// Buttons carry a "role" property; the theme stylesheet colours the checked
// state from it, so switching themes needs no per-widget work.
static QPushButton* makeToggle(const QString& text, const char* role, int h = 0)
{
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setFixedHeight(h > 0 ? h : bbui::rowH());
    b->setMinimumWidth(1);
    b->setProperty("role", role);
    return b;
}

static QLabel* makeLabel(const QString& text, const char* role, Qt::Alignment a = Qt::AlignHCenter)
{
    auto* l = new QLabel(text);
    l->setProperty("role", role);
    l->setAlignment(a);
    return l;
}

// Card headers carry names the user chose, of any length.
static QLabel* makeHeader(const QString& text, const char* role,
                          Qt::Alignment a = Qt::AlignHCenter)
{
    auto* l = new ElidedLabel(text);
    l->setProperty("role", role);
    l->setAlignment(a);
    return l;
}

// --- custom strip / bus names ----------------------------------------------
static QString labelFor(Shared* shm, bool strip, int idx, const QString& fallback)
{
    char ls[kStrips][kLabelLen], lb[kBuses][kLabelLen];
    for (int t = 0; t < 16; ++t)
        if (labels_read(shm->labels, ls, lb)) {
            const char* v = strip ? ls[idx] : lb[idx];
            return v[0] ? QString::fromUtf8(v) : fallback;
        }
    return fallback;
}

static void setLabel(Shared* shm, bool strip, int idx, const QString& text)
{
    shm->labels.seq.fetch_add(1, std::memory_order_acq_rel);
    char* dst = strip ? shm->labels.strip[idx] : shm->labels.bus[idx];
    snprintf(dst, kLabelLen, "%s", text.toUtf8().constData());
    shm->labels.seq.fetch_add(1, std::memory_order_release);
}

// Right-click a title plate to rename it, as in Banana's editable strip labels.
static void installRename(QLabel* header, Shared* shm, bool strip, int idx,
                          const QString& fallback, QWidget* owner)
{
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->setToolTip(QObject::tr("Right-click to rename"));
    QObject::connect(header, &QLabel::customContextMenuRequested, owner,
                     [header, shm, strip, idx, fallback](const QPoint& pos) {
        QMenu m;
        QAction* ren = m.addAction("Rename...");
        QAction* def = m.addAction("Reset to default");
        QAction* got = m.exec(header->mapToGlobal(pos));
        if (got == ren) {
            bool ok = false;
            const QString cur = header->text();
            const QString v = QInputDialog::getText(header, "Rename", "Name:",
                                                    QLineEdit::Normal, cur, &ok);
            if (ok) {
                setLabel(shm, strip, idx, v.trimmed());
                header->setText(v.trimmed().isEmpty() ? fallback : v.trimmed());
            }
        } else if (got == def) {
            setLabel(shm, strip, idx, QString());
            header->setText(fallback);
        }
    });
}

// ---------------------------------------------------------------------------
// StripWidget
// ---------------------------------------------------------------------------
Knob* StripWidget::addKnob(QGridLayout* g, int col, const QString& name,
                           int lo, int hi, int def, bool bipolar)
{
    auto* k = new Knob(lo, hi, def, bipolar, QString());
    // Elided rather than clipped: three five-letter captions share a knob grid
    // barely a hundred pixels wide, and "AUDIB" is already an abbreviation.
    auto* cap = makeHeader(name, "knobcap");
    cap->setToolTip(name == "AUDIB" ? "Audibility: Voicemeeter's intelligibility lift"
                                    : name);
    g->addWidget(cap, 0, col);
    g->addWidget(k, 1, col, Qt::AlignHCenter);
    return k;
}

StripWidget::StripWidget(Shared* shm, int index, bool hardware, const QString& title, QWidget* parent)
    : QWidget(parent), m_shm(shm), m_index(index), m_hardware(hardware)
{
    // Qt only honours a stylesheet background on a plain QWidget when
    // WA_StyledBackground is set: QStyleSheetStyle::polish sets it
    // automatically only when metaObject() *is* QWidget's, which a moc'd
    // subclass never is. Without this the card rule paints nothing at all,
    // which is how every strip, bus and the tape deck spent their whole life
    // floating directly on the window colour.
    setProperty("role", hardware ? "card" : "cardVirtual");
    setAttribute(Qt::WA_StyledBackground, true);
    auto* root = new QVBoxLayout(this);
    m_root = root;
    root->setContentsMargins(bbui::gapS() + 1, bbui::gapS() + 2, bbui::gapS() + 1, bbui::gapS() + 2);
    root->setSpacing(bbui::gapS());
    m_header = makeHeader(labelFor(shm, true, index, title), "header");
    installRename(m_header, shm, true, index, title, this);
    root->addWidget(m_header);

    if (m_hardware) {
        m_device = new DeviceCombo;
        m_device->setMinimumWidth(bbui::px(90));
        m_device->setFixedHeight(bbui::rowH());
        m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_device->addItem("- none -", QString());
        m_device->setToolTip("Right-click to remember this strip's settings for "
                             "whichever device is selected");
        m_device->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_device, &QComboBox::customContextMenuRequested, this,
                [this](const QPoint& pos) { deviceMenu(pos); });
        connect(m_device, &QComboBox::currentIndexChanged, this, [this](int) {
            emit routingChanged(m_index, m_device->currentData().toString());
        });
        root->addWidget(m_device);
    } else {
        auto* vl = makeLabel(index == kHwStrips ? "bb_vaio" : "bb_aux", "caption");
        vl->setFixedHeight(bbui::rowH());   // keeps every meter top on one line
        vl->setToolTip(index == kHwStrips
            ? "Virtual input: whatever plays into BetterBanana Cable 1"
            : "Virtual input: whatever plays into BetterBanana Cable 2");
        root->addWidget(vl);
    }

    StripParams& p = m_shm->strip[m_index];

    // Gate / Comp / Audibility exist only on hardware strips, as in Banana.
    if (m_hardware) {
        auto* g = new QGridLayout;
        g->setSpacing(bbui::gapXS());
        m_gate = addKnob(g, 0, "GATE",  0, 100, 0, false);
        m_comp = addKnob(g, 1, "COMP",  0, 100, 0, false);
        m_aud  = addKnob(g, 2, "AUDIB", 0, 100, 0, false);
        connect(m_gate, &Knob::valueChanged, this, [&p](int v){ p.gate.store(v / 10.0f); });
        connect(m_comp, &Knob::valueChanged, this, [&p](int v){ p.comp.store(v / 10.0f); });
        connect(m_aud,  &Knob::valueChanged, this, [&p](int v){ p.audibility.store(v / 10.0f); });
        // Gain-reduction readouts: the engine already computes these.
        m_gateGr = new ReductionBar(ReductionBar::Gate);
        m_compGr = new ReductionBar(ReductionBar::Comp);
        m_gateGr->setToolTip("Gate attenuation");
        m_compGr->setToolTip("Compressor gain reduction");
        // Side by side across the full knob row: they used to sit under two of
        // three columns, leaving a hairline that read as a rendering slip.
        auto* grRow = new QHBoxLayout;
        grRow->setSpacing(bbui::gapXS());
        grRow->addWidget(m_gateGr, 1);
        grRow->addWidget(m_compGr, 1);
        g->addLayout(grRow, 2, 0, 1, 3);
        root->addLayout(g);
    }

    {   // EQ
        auto* g = new QGridLayout;
        g->setSpacing(bbui::gapXS());
        m_eqLo  = addKnob(g, 0, "LOW",  -120, 120, 0, true);
        m_eqMid = addKnob(g, 1, "MID",  -120, 120, 0, true);
        m_eqHi  = addKnob(g, 2, "HIGH", -120, 120, 0, true);
        connect(m_eqLo,  &Knob::valueChanged, this, [&p](int v){ p.eq_low.store(v / 10.0f); });
        connect(m_eqMid, &Knob::valueChanged, this, [&p](int v){ p.eq_mid.store(v / 10.0f); });
        connect(m_eqHi,  &Knob::valueChanged, this, [&p](int v){ p.eq_high.store(v / 10.0f); });
        root->addLayout(g);

        // The three knobs are a fixed tone control; the twelve-band parametric
        // and the voice changer live behind these two, as the bus EQ does.
        auto* row = new QHBoxLayout;
        row->setSpacing(bbui::gapXS());
        m_eqBtn = makeToggle("EQ", "eq");
        m_eqBtn->setToolTip("Twelve-band parametric EQ, after the tone knobs.\n"
                            "Left-click: enable/bypass.  Right-click: edit.");
        m_eqBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_eqBtn, &QPushButton::customContextMenuRequested, this,
                [this](const QPoint&) { emit eqEditRequested(m_index); });
        connect(m_eqBtn, &QPushButton::toggled, this, [&p](bool b){ p.eq.on.store(b ? 1 : 0); });
        row->addWidget(m_eqBtn);

        m_fxBtn = makeToggle("FX", "solo");
        m_fxBtn->setToolTip("Voice changer: pitch, drive, ring mod, crush, "
                            "chorus, echo.\n"
                            "Left-click: enable/bypass.  Right-click: edit.");
        m_fxBtn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_fxBtn, &QPushButton::customContextMenuRequested, this,
                [this](const QPoint&) { emit fxEditRequested(m_index); });
        connect(m_fxBtn, &QPushButton::toggled, this, [&p](bool b){ p.fx.on.store(b ? 1 : 0); });
        row->addWidget(m_fxBtn);
        root->addLayout(row);
    }

    {   // Mono / Solo, then Mute across the full width.
        //
        // Three four-letter chips side by side is what set this card's minimum
        // width, and it needed 133px per column: ten of those plus chrome asked
        // for a 1370px window, so at any smaller size the labels clipped to
        // "MONC SOLC MUTI". Two rows cost 22px of height - which the console has
        // in quantity - and hand the most-used control the biggest target.
        auto* row = new QHBoxLayout;
        row->setSpacing(bbui::gapXS());
        m_mono = makeToggle("MONO", "mono");
        m_solo = makeToggle("SOLO", "solo");
        m_mute = makeToggle("MUTE", "mute");
        connect(m_mono, &QPushButton::toggled, this, [&p](bool b){ p.mono.store(b ? 1 : 0); });
        connect(m_solo, &QPushButton::toggled, this, [&p](bool b){ p.solo.store(b ? 1 : 0); });
        connect(m_mute, &QPushButton::toggled, this, [&p](bool b){ p.mute.store(b ? 1 : 0); });
        row->addWidget(m_mono); row->addWidget(m_solo);
        root->addLayout(row);
        root->addWidget(m_mute);
    }

    {   // Fader + meter
        // A virtual strip has no gate/comp grid, so without this its meter
        // starts 76px above a hardware strip's and the bridge stops reading
        // across. MainWindow measures the tallest card and pads the rest.
        m_lead = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
        root->addItem(m_lead);

        auto* row = new QHBoxLayout;
        row->setSpacing(0);          // the meter is the fader's companion
        m_fader = new Fader(-600, 120, 0);
        m_meter = new LevelMeter(kChan);
        // One travel for every strip and bus in the window: the four different
        // meter heights used to put the same dBFS 114px apart between columns,
        // and a meter bridge whose scale moves per column is not a bridge.
        m_fader->setFixedHeight(bbui::travel());
        m_meter->setFixedHeight(bbui::travel());
        m_meter->setToolTip("Click to clear this strip's clip indicator");
        m_meter->setClickHandler([this] {
            // Was the global kCmdClearClip, which wiped every strip and every
            // bus - dismissing one red bar destroyed the evidence that another
            // clipped while you were looking away.
            m_shm->meters.strip_clip[m_index].store(0, std::memory_order_relaxed);
        });
        connect(m_fader, &Fader::valueChanged, this, [this, &p](int v) {
            p.gain_db.store(sliderDb(v));
            m_gainLbl->setText(QString::asprintf("%+.1f dB", sliderDb(v)));
        });
        row->addStretch(); row->addWidget(m_fader); row->addWidget(m_meter); row->addStretch();
        root->addLayout(row, 0);
    }

    m_gainLbl = makeLabel("+0.0 dB", "gain");
    root->addWidget(m_gainLbl);
    m_duckGr = new ReductionBar(ReductionBar::Duck);
    m_duckGr->setToolTip("Ducker attenuation");
    root->addWidget(m_duckGr);

    {   // Intellipan. Pan is the last thing the engine does to a strip, so it
        // belongs after the fader rather than five blocks above it.
        //
        // Only the X axis does anything: `pan_y` was written by the GUI, saved
        // into presets and read by no engine file, so the dot used to move
        // vertically to no effect whatever. XYPad pins Y until that changes.
        m_pan = new XYPad;
        m_pan->setFixedHeight(bbui::px(34));
        m_pan->setToolTip("Pan. Double-click to centre.");
        connect(m_pan, &XYPad::valuesChanged, this, [&p](int x, int) {
            p.pan_x.store(x / 100.0f);
        });
        root->addWidget(m_pan);
    }

    {   // Bus assignment: one row of five, as in Banana.
        auto* row = new QHBoxLayout;
        row->setSpacing(bbui::gapXS());
        for (int b = 0; b < kBuses; ++b) {
            auto* btn = makeToggle(kBusLabel[b], b < kPhysBuses ? "busA" : "busB",
                                   bbui::px(19));
            btn->setProperty("bus", b);   // per-bus hue, see buildStyleSheet
            btn->setChecked(p.bus_on[b].load() != 0);
            connect(btn, &QPushButton::toggled, this, [&p, b](bool on){ p.bus_on[b].store(on ? 1 : 0); });
            m_busBtns.push_back(btn);
            row->addWidget(btn);
        }
        root->addLayout(row);
    }
    setMinimumWidth(bbui::px(112));
}

// Re-apply a dynamic property so the stylesheet picks it up. Qt does not
// re-evaluate selectors on a property change by itself.
static void restyle(QWidget* w, const char* name, bool on)
{
    if (w->property(name).toBool() == on) return;
    w->setProperty(name, on);
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

int  StripWidget::leadPad() const  { return m_lead ? m_lead->geometry().height() : 0; }

void StripWidget::setLeadPad(int px)
{
    if (!m_lead || px < 0) return;
    if (m_lead->sizeHint().height() == px) return;
    m_lead->changeSize(0, px, QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_root->invalidate();
}

void StripWidget::setTravel(int px)
{
    if (m_fader->height() == px) return;
    m_fader->setFixedHeight(px);
    m_meter->setFixedHeight(px);
}

void StripWidget::setDimmed(bool d)
{
    if (d == m_dimmed) return;
    m_dimmed = d;
    restyle(this, "dim", d);
}

void StripWidget::setLive(bool live)
{
    m_meter->setStale(!live);
    if (live) return;
    // Reduction bars read from the engine too; a frozen one is a lie.
    for (ReductionBar* b : { m_gateGr, m_compGr, m_duckGr })
        if (b) b->setAmount(0.0f);
}

void StripWidget::setAttached(bool a)
{
    if (a == m_attached) return;
    m_attached = a;
    restyle(m_header, "nodev", !a);
    m_header->setToolTip(a ? QString()
                           : "This strip has a device selected but nothing is "
                             "attached to it right now.");
}

// A bus renamed to "headphones" left every strip carrying a button labelled A2
// with nothing in the window to say what A2 was.
void StripWidget::refreshBusTips(const QStringList& names)
{
    for (int b = 0; b < m_busBtns.size() && b < names.size(); ++b) {
        const QString want = QString("Send this strip to %1  (%2)")
                                 .arg(names.at(b), kBusLabel[b]);
        if (m_busBtns[b]->toolTip() != want) m_busBtns[b]->setToolTip(want);
    }
}

void StripWidget::setDeviceList(const QStringList& ids, const QStringList& labels)
{
    if (!m_device) return;
    const QString keep = m_device->currentData().toString();
    QSignalBlocker block(m_device);
    m_device->clear();
    m_device->addItem("- none -", QString());
    for (int i = 0; i < ids.size(); ++i) m_device->addItem(labels.value(i), ids.at(i));
    const int idx = m_device->findData(keep);
    m_device->setCurrentIndex(idx >= 0 ? idx : 0);
}


// Right-click the device picker to tie this strip's processing to whatever is
// plugged in. Nothing is stored unless you ask for it: the mixer no longer
// saves your session behind your back, and it does not do it here either.
void StripWidget::deviceMenu(const QPoint& pos)
{
    if (!m_device) return;
    const QString dev = deviceValue();
    QMenu m;
    if (dev.isEmpty()) {
        QAction* a = m.addAction("Assign a device first");
        a->setEnabled(false);
        m.exec(m_device->mapToGlobal(pos));
        return;
    }
    const bool known = has_strip_for_device(dev.toStdString());
    QAction* rem = m.addAction(known ? "Update the settings remembered for this device"
                                     : "Remember these settings for this device");
    rem->setToolTip("Gate, compressor, EQ, level and pan - but not the bus "
                    "assignment, which belongs to the mix rather than to the device");
    QAction* fgt = known ? m.addAction("Forget them") : nullptr;
    m.addSeparator();
    QAction* info = m.addAction(dev);
    info->setEnabled(false);

    QAction* got = m.exec(m_device->mapToGlobal(pos));
    if (got == rem) {
        if (save_strip_for_device(m_shm, m_index, dev.toStdString()))
            emit statusMessage("Remembered these settings for " + dev +
                               " - they come back whenever this strip is set to it");
        else
            emit statusMessage("Could not write " +
                QString::fromStdString(device_strip_path(dev.toStdString())));
    } else if (fgt && got == fgt) {
        if (forget_strip_for_device(dev.toStdString()))
            emit statusMessage("Forgot the settings remembered for " + dev);
    }
}

// The engine owns routing; adopt it rather than assuming the GUI started first.
void StripWidget::setDeviceValue(const QString& id)
{
    if (!m_device) return;
    QSignalBlocker block(m_device);
    int idx = m_device->findData(id);
    m_missing = false;
    if (idx < 0 && !id.isEmpty()) {          // assigned device is gone/unplugged
        // The marker leads. It used to be appended to a raw PipeWire node name
        // that needs 437px of advance inside a 94px combo, so the one word that
        // mattered was the only part guaranteed never to be on screen.
        m_device->addItem("missing  -  " + id, id);
        idx = m_device->count() - 1;
    }
    if (idx > 0) m_missing = m_device->itemText(idx).startsWith("missing  -  ");
    // Carry it in the control's own colour too, not just its text. Guarded:
    // readRouting calls this twice a second, and an unguarded unpolish/polish
    // re-runs the stylesheet over the widget every time.
    restyle(m_device, "bad", m_missing);
    m_device->setCurrentIndex(idx >= 0 ? idx : 0);
    m_device->setToolTip(m_device->currentIndex() > 0
        ? m_device->currentText() + "\n" + m_device->currentData().toString()
        : QString("No device assigned"));
}

QString StripWidget::deviceValue() const
{
    return m_device ? m_device->currentData().toString() : QString();
}

void StripWidget::pullFromShm()
{
    StripParams& p = m_shm->strip[m_index];
    auto setK = [](Knob* k, int v) { if (k && !k->isDragging()) k->setValue(v); };
    setK(m_eqLo,  int(p.eq_low.load()  * 10));
    setK(m_eqMid, int(p.eq_mid.load()  * 10));
    setK(m_eqHi,  int(p.eq_high.load() * 10));
    setK(m_gate,  m_gate ? int(p.gate.load() * 10) : 0);
    setK(m_comp,  m_comp ? int(p.comp.load() * 10) : 0);
    setK(m_aud,   m_aud  ? int(p.audibility.load() * 10) : 0);
    if (!m_pan->isDragging())
        // Y is pinned: the engine has never read pan_y, so an old preset
        // carrying one must not put it back on screen.
        m_pan->setValues(int(p.pan_x.load() * 100), 0);
    if (!m_fader->isDragging()) m_fader->setValue(dbSlider(p.gain_db.load()));
    m_mono->setChecked(p.mono.load()); m_solo->setChecked(p.solo.load());
    m_mute->setChecked(p.mute.load());
    if (m_eqBtn) m_eqBtn->setChecked(p.eq.on.load() != 0);
    if (m_fxBtn) m_fxBtn->setChecked(p.fx.on.load() != 0);
    for (int b = 0; b < m_busBtns.size(); ++b) m_busBtns[b]->setChecked(p.bus_on[b].load() != 0);
    m_gainLbl->setText(QString::asprintf("%+.1f dB", p.gain_db.load()));
    m_header->setText(labelFor(m_shm, true, m_index, kStripTitle[m_index]));
}

void StripWidget::refreshMeters()
{
    float v[kChan];
    for (int c = 0; c < kChan; ++c)
        v[c] = m_shm->meters.strip_post[m_index][c].load(std::memory_order_relaxed);
    m_meter->setLevels(v, kChan);
    m_meter->setClipped(m_shm->meters.strip_clip[m_index].load(std::memory_order_relaxed) != 0);

    if (m_gateGr)
        m_gateGr->setAmount(1.0f - m_shm->meters.strip_gate_gain[m_index].load(std::memory_order_relaxed));
    if (m_compGr)   // 20 dB of reduction fills the bar
        m_compGr->setAmount(-m_shm->meters.strip_comp_gr[m_index].load(std::memory_order_relaxed) / 20.0f);
    if (m_duckGr)
        m_duckGr->setAmount(-m_shm->meters.strip_duck_gr[m_index].load(std::memory_order_relaxed) / 20.0f);
}

// ---------------------------------------------------------------------------
// BusWidget
// ---------------------------------------------------------------------------
BusWidget::BusWidget(Shared* shm, int index, bool hardware, const QString& title, QWidget* parent)
    : QWidget(parent), m_shm(shm), m_index(index), m_hardware(hardware)
{
    setProperty("role", hardware ? "card" : "cardVirtual");
    setAttribute(Qt::WA_StyledBackground, true);
    auto* root = new QVBoxLayout(this);
    m_root = root;
    root->setContentsMargins(bbui::gapS() + 1, bbui::gapS() + 2, bbui::gapS() + 1, bbui::gapS() + 2);
    root->setSpacing(bbui::gapS());

    {   // Rename a bus to "headphones" and its A-number used to vanish from the
        // window entirely, while every strip still carried a row of buttons
        // labelled A1..B2 pointing at nothing nameable. The code travels with
        // the card now.
        auto* hrow = new QHBoxLayout;
        hrow->setSpacing(bbui::gapXS());
        auto* tag = makeLabel(kBusLabel[index], "tag");
        tag->setFixedWidth(bbui::px(24));
        tag->setToolTip(hardware ? "Physical output bus" : "Virtual output bus");
        m_header = makeHeader(labelFor(shm, false, index, title),
                              hardware ? "headerA" : "headerB",
                              Qt::AlignLeft | Qt::AlignVCenter);
        installRename(m_header, shm, false, index, title, this);
        hrow->addWidget(tag);
        hrow->addWidget(m_header, 1);
        root->addLayout(hrow);
    }

    if (m_hardware) {
        m_device = new DeviceCombo;
        m_device->setMinimumWidth(bbui::px(90));
        m_device->setFixedHeight(bbui::rowH());
        m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_device->addItem("- none -", QString());
        connect(m_device, &QComboBox::currentIndexChanged, this, [this](int) {
            emit routingChanged(m_index, m_device->currentData().toString());
        });
        root->addWidget(m_device);

        // What the bus publishes. Only the A buses get one: B1 and B2 are what
        // other applications record from, and a recording input that changed
        // channel count under them would be a different kind of surprise.
        m_mode = new DeviceCombo;
        m_mode->setFixedHeight(bbui::rowH());
        m_mode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        for (int m = 0; m < kBusModeCount; ++m) {
            const BusLayout& L = bus_layout(m);
            m_mode->addItem(m == kBusNormal
                                ? QString(L.name)
                                : QString("%1  (%2 ch)").arg(L.name).arg(L.channels), m);
            m_mode->setItemData(m_mode->count() - 1, QString(L.help), Qt::ToolTipRole);
        }
        connect(m_mode, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i < 0) return;
            m_shm->bus[m_index].mode.store(m_mode->itemData(i).toInt());
            emit statusMessage(QString("%1 is now %2")
                                   .arg(m_header->text(),
                                        bus_layout(m_mode->itemData(i).toInt()).name));
        });
        root->addWidget(m_mode);
    } else {
        auto* vl = makeLabel(index == kPhysBuses ? "bb_b1" : "bb_b2", "caption");
        vl->setFixedHeight(bbui::rowH());
        vl->setToolTip("Virtual output: applications can record from this");
        root->addWidget(vl);
    }

    BusParams& p = m_shm->bus[m_index];
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(bbui::gapXS());
        m_eq   = makeToggle("EQ",   "eq");
        m_eq->setToolTip("Left-click: enable/bypass.  Right-click: edit the 12 bands.");
        m_eq->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_eq, &QPushButton::customContextMenuRequested, this,
                [this](const QPoint&) { emit eqEditRequested(m_index); });
        m_mono = makeToggle("MONO", "mono");
        m_mute = makeToggle("MUTE", "mute");
        connect(m_eq,   &QPushButton::toggled, this, [&p](bool b){ p.eq.on.store(b ? 1 : 0); });
        connect(m_mono, &QPushButton::toggled, this, [&p](bool b){ p.mono.store(b ? 1 : 0); });
        connect(m_mute, &QPushButton::toggled, this, [&p](bool b){ p.mute.store(b ? 1 : 0); });
        row->addWidget(m_eq); row->addWidget(m_mono);
        root->addLayout(row);
        root->addWidget(m_mute);      // full width, as on an input strip
    }
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(0);
        m_fader = new Fader(-600, 120, 0);
        m_meter = new LevelMeter(kChan);
        m_fader->setFixedHeight(bbui::travel());
        m_meter->setFixedHeight(bbui::travel());
        m_meter->setToolTip("Click to clear this bus's clip indicator");
        m_meter->setClickHandler([this] {
            m_shm->meters.bus_clip[m_index].store(0, std::memory_order_relaxed);
        });
        connect(m_fader, &Fader::valueChanged, this, [this, &p](int v) {
            p.gain_db.store(sliderDb(v));
            m_gainLbl->setText(QString::asprintf("%+.1f dB", sliderDb(v)));
        });
        // The gap that alignment leaves above a bus's fader, given a tenant.
        m_thumb = new EqThumb;
        m_thumb->setClickHandler([this] { emit eqEditRequested(m_index); });
        root->addWidget(m_thumb, 0);

        // Padded, not floated: a stretch here would put the bus meters wherever
        // the surplus happened to land. MainWindow measures a strip once and
        // sets this so every meter in the window starts on the same line.
        m_lead = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
        root->addItem(m_lead);
        row->addStretch(); row->addWidget(m_fader); row->addWidget(m_meter); row->addStretch();
        root->addLayout(row, 0);
    }
    m_gainLbl = makeLabel("+0.0 dB", "gain");
    root->addWidget(m_gainLbl);

    // Peak says whether it will clip. This says how loud it will sound, which
    // is what a streaming platform measures and what the meter beside it cannot
    // tell you.
    m_lufs = makeLabel("-- LUFS", "caption");
    m_lufs->setAlignment(Qt::AlignHCenter);
    m_lufs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_lufs, &QLabel::customContextMenuRequested, this, [this](const QPoint&) {
        m_shm->cmd.store(kCmdResetLoudness);
        m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
        emit statusMessage("Integrated loudness measurement restarted");
    });
    root->addWidget(m_lufs);
    // A strip carries pan and a bus-assign row under its gain readout. Matching
    // that depth here lines every meter in the console up along one baseline.
    root->addStretch(1);
    setMinimumWidth(bbui::px(100));
}

void BusWidget::setLive(bool live) { m_meter->setStale(!live); }

void BusWidget::setTravel(int px)
{
    if (m_fader->height() == px) return;
    m_fader->setFixedHeight(px);
    m_meter->setFixedHeight(px);
}

int  BusWidget::meterTop() const { return m_meter->mapTo(this, QPoint(0, 0)).y(); }
int  BusWidget::leadPad() const  { return m_lead ? m_lead->geometry().height() : 0; }
int  StripWidget::meterTop() const { return m_meter->mapTo(this, QPoint(0, 0)).y(); }

void BusWidget::setLeadPad(int px)
{
    if (!m_lead || px < 0) return;
    if (m_lead->sizeHint().height() == px) return;
    m_lead->changeSize(0, px, QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_root->invalidate();
}

void BusWidget::setDeviceList(const QStringList& ids, const QStringList& labels)
{
    if (!m_device) return;
    const QString keep = m_device->currentData().toString();
    QSignalBlocker block(m_device);
    m_device->clear();
    m_device->addItem("- none -", QString());
    for (int i = 0; i < ids.size(); ++i) m_device->addItem(labels.value(i), ids.at(i));
    const int idx = m_device->findData(keep);
    m_device->setCurrentIndex(idx >= 0 ? idx : 0);
}


void BusWidget::setDeviceValue(const QString& id)
{
    if (!m_device) return;
    QSignalBlocker block(m_device);
    int idx = m_device->findData(id);
    m_missing = false;
    if (idx < 0 && !id.isEmpty()) {
        m_device->addItem("missing  -  " + id, id);   // marker leads; see StripWidget
        idx = m_device->count() - 1;
    }
    if (idx > 0) m_missing = m_device->itemText(idx).startsWith("missing  -  ");
    restyle(m_device, "bad", m_missing);
    m_device->setCurrentIndex(idx >= 0 ? idx : 0);
    m_device->setToolTip(m_device->currentIndex() > 0
        ? m_device->currentText() + "\n" + m_device->currentData().toString()
        : QString("No device assigned"));
}

QString BusWidget::deviceValue() const
{
    return m_device ? m_device->currentData().toString() : QString();
}

void BusWidget::pullFromShm()
{
    BusParams& p = m_shm->bus[m_index];
    if (!m_fader->isDragging()) m_fader->setValue(dbSlider(p.gain_db.load()));
    m_eq->setChecked(p.eq.on.load()); m_mono->setChecked(p.mono.load());
    m_mute->setChecked(p.mute.load());
    m_gainLbl->setText(QString::asprintf("%+.1f dB", p.gain_db.load()));
    m_header->setText(labelFor(m_shm, false, m_index, kBusLabel[m_index]));

    if (m_mode) {
        int m = p.mode.load();
        if (m < 0 || m >= kBusModeCount) m = kBusNormal;
        if (m_mode->currentIndex() != m) {
            QSignalBlocker block(m_mode);   // a pull is not a change
            m_mode->setCurrentIndex(m);
        }
        const BusLayout& L = bus_layout(m);
        m_mode->setToolTip(QString("%1\n%2\n\nChanging this republishes the bus with "
                                   "%3 channel%4, so the device it drives has to accept "
                                   "that layout.")
                               .arg(L.name, L.help).arg(L.channels)
                               .arg(L.channels == 1 ? "" : "s"));
    }

    if (m_thumb) {
        // Log-spaced, 20 Hz .. 20 kHz, one sample per two pixels. Cheap enough
        // at the 2 Hz sync rate that drives this, and it uses the engine's own
        // response function so the thumbnail cannot drift from the sound.
        const EqProfile prof = eq_capture(p.eq);
        const int n = qMax(2, m_thumb->width() / 2);
        QVector<float> curve(n);
        for (int i = 0; i < n; ++i) {
            const double f = 20.0 * std::pow(1000.0, double(i) / (n - 1));
            curve[i] = eq_response_db(prof, float(f)) + prof.preamp;
        }
        m_thumb->setCurve(curve, p.eq.on.load() != 0);
    }
}

void BusWidget::refreshMeters()
{
    if (m_lufs) {
        const float sh = m_shm->meters.bus_lufs_s[m_index].load(std::memory_order_relaxed);
        const float in = m_shm->meters.bus_lufs_i[m_index].load(std::memory_order_relaxed);
        // Below the floor there is no measurement to report, so say that rather
        // than printing -70.0 as though it were a reading.
        const QString t = sh <= -70.0f ? QString("-- LUFS")
                                       : QString::asprintf("%.1f LUFS", sh);
        if (m_lufs->text() != t) m_lufs->setText(t);
        m_lufs->setToolTip(
            QString("Short-term loudness, the rolling three seconds.\n"
                    "Integrated since the last reset: %1\n\n"
                    "Right-click to start the integrated measurement again.")
                .arg(in <= -70.0f ? QString("not enough audio yet")
                                  : QString::asprintf("%.1f LUFS", in)));
    }

    float v[kChan];
    for (int c = 0; c < kChan; ++c)
        v[c] = m_shm->meters.bus_out[m_index][c].load(std::memory_order_relaxed);
    m_meter->setLevels(v, kChan);
    m_meter->setClipped(m_shm->meters.bus_clip[m_index].load(std::memory_order_relaxed) != 0);
}

// ---------------------------------------------------------------------------
// RecorderWidget
// ---------------------------------------------------------------------------
void RecorderWidget::sendCmd(int cmd)
{
    m_shm->cmd.store(cmd, std::memory_order_relaxed);
    m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
}

void RecorderWidget::writePaths()
{
    m_shm->rec.cfg_seq.fetch_add(1, std::memory_order_acq_rel);
    snprintf(m_shm->rec.rec_path,  kNameLen, "%s", m_recPath->text().toUtf8().constData());
    snprintf(m_shm->rec.play_path, kNameLen, "%s", m_playPath->text().toUtf8().constData());
    m_shm->rec.cfg_seq.fetch_add(1, std::memory_order_release);
}

RecorderWidget::RecorderWidget(Shared* shm, QWidget* parent)
    : QWidget(parent), m_shm(shm)
{
    setProperty("role", "card");
    setAttribute(Qt::WA_StyledBackground, true);
    m_pulse.start();

    // A grid, not two independent rows.
    //
    // The record and play rows used to be separate QHBoxLayouts inside a
    // column, so nothing in one lined up with anything in the other: the two
    // path fields began at different x because "REC FILE" and "PLAY FILE" are
    // different widths, FROM floated in the gap between the rows attached to
    // neither, and PLAYBACK TO sat at the far right of the bar with its own
    // chips three hundred pixels away on the row below. One grid gives every
    // column a single edge.
    //
    //   col:   0        1          2            3     4        5
    //   row 0: RECORDER REC FILE   [path...]    [...] FROM/bus [REC]   ...
    //   row 1:          PLAY FILE  [path...]    [...] [PLAY]   [LOOP]  ...
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(bbui::gapM(), bbui::gapS(), bbui::gapM(), bbui::gapS());
    root->setSpacing(bbui::gapM());

    auto* g = new QGridLayout;
    g->setHorizontalSpacing(bbui::gapS());
    g->setVerticalSpacing(bbui::gapXS());
    root->addLayout(g, 1);

    {   // The card names itself now that it is no longer inside a group box.
        auto* title = makeLabel("RECORDER", "caption", Qt::AlignLeft | Qt::AlignVCenter);
        g->addWidget(title, 0, 0, 2, 1);
    }

    // Both file rows are built the same way, so the columns cannot drift apart.
    auto fileRow = [&](int row, const QString& caption, QLineEdit*& field,
                       const QString& placeholder, std::function<void()> browse) {
        g->addWidget(makeLabel(caption, "caption", Qt::AlignRight | Qt::AlignVCenter), row, 1);
        field = new QLineEdit;
        field->setMinimumWidth(bbui::px(180));
        field->setPlaceholderText(placeholder);
        // Right-aligned: a wide field showing "/home/eve" and hiding the
        // filename is keeping the wrong end of the path.
        field->setAlignment(Qt::AlignRight);
        field->setFixedHeight(bbui::rowH());
        connect(field, &QLineEdit::editingFinished, this, [this] { writePaths(); });
        g->addWidget(field, row, 2);
        auto* b = new QPushButton("...");
        b->setFixedWidth(bbui::px(28));
        b->setFixedHeight(bbui::rowH());
        b->setToolTip("Browse");
        connect(b, &QPushButton::clicked, this, browse);
        g->addWidget(b, row, 3);
    };

    fileRow(0, "REC FILE", m_recPath, "where to record to", [this] {
        const QString f = QFileDialog::getSaveFileName(this, "Record to", m_recPath->text(),
                                                       "WAV audio (*.wav)");
        if (!f.isEmpty()) { m_recPath->setText(f); writePaths(); }
    });
    m_recPath->setText(QDir::homePath() + "/betterbanana-take.wav");

    fileRow(1, "PLAY FILE", m_playPath, "a file to play into the mixer", [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Play file", QDir::homePath(),
                                    "Audio (*.wav *.flac *.ogg *.aiff);;All files (*)");
        if (!f.isEmpty()) { m_playPath->setText(f); writePaths(); }
    });

    {   // Source bus, on the record row, with its label attached to it rather
        // than floating between the two rows.
        auto* from = new QHBoxLayout;
        from->setSpacing(bbui::gapXS());
        from->addWidget(makeLabel("FROM", "caption", Qt::AlignRight | Qt::AlignVCenter));
        m_srcBus = new QComboBox;
        for (int b = 0; b < kBuses; ++b) m_srcBus->addItem(kBusLabel[b]);
        m_srcBus->setCurrentIndex(m_shm->rec.source_bus.load());
        m_srcBus->setFixedHeight(bbui::rowH());
        m_srcBus->setToolTip("Which bus is recorded");
        connect(m_srcBus, &QComboBox::currentIndexChanged, this,
                [this](int i) { m_shm->rec.source_bus.store(i); });
        from->addWidget(m_srcBus, 1);
        g->addLayout(from, 0, 4);
    }

    m_rec = makeToggle("● REC", "rec");
    m_rec->setMinimumWidth(bbui::px(70));
    connect(m_rec, &QPushButton::clicked, this, [this] {
        writePaths();
        sendCmd(m_shm->rec.state.load() == kRecRecording ? kCmdRecStop : kCmdRecStart);
    });
    g->addWidget(m_rec, 0, 5);

    m_play = makeToggle("▶ PLAY", "accent");
    connect(m_play, &QPushButton::clicked, this, [this] {
        writePaths();
        sendCmd(m_shm->rec.state.load() == kRecPlaying ? kCmdPlayStop : kCmdPlayStart);
    });
    g->addWidget(m_play, 1, 4);

    m_loop = makeToggle("LOOP", "accent");
    m_loop->setMinimumWidth(bbui::px(70));
    connect(m_loop, &QPushButton::toggled, this,
            [this](bool b) { m_shm->rec.loop.store(b ? 1 : 0); });
    g->addWidget(m_loop, 1, 5);

    // Only the path fields absorb a wider window.
    g->setColumnStretch(2, 1);

    // --- the right-hand block: where it is going, and what it is doing ------
    {   // Caption over its own chips, in one column, so the two are read
        // together instead of at opposite ends of the bar.
        auto* col = new QVBoxLayout;
        col->setSpacing(bbui::gapXS());
        col->addStretch();
        col->addWidget(makeLabel("PLAYBACK TO", "caption"));
        auto* r = new QHBoxLayout;
        r->setSpacing(bbui::gapXS() - 1);
        for (int b = 0; b < kBuses; ++b) {
            auto* btn = makeToggle(kBusLabel[b], b < kPhysBuses ? "busA" : "busB",
                                   bbui::px(19));
            btn->setProperty("bus", b);
            btn->setChecked(m_shm->rec.bus_on[b].load() != 0);
            connect(btn, &QPushButton::toggled, this,
                    [this, b](bool on) { m_shm->rec.bus_on[b].store(on ? 1 : 0); });
            m_busBtns.push_back(btn);
            r->addWidget(btn);
        }
        col->addLayout(r);
        col->addStretch();
        root->addLayout(col);
    }

    {   // Transport readout: a timecode the widget already formatted but only
        // ever printed into a caption, a position bar, and what it is doing.
        auto* col = new QVBoxLayout;
        col->setSpacing(bbui::gapXS());
        // Centred, so the slack goes above and below rather than being shared
        // out between the three items - which left a gap where the position
        // bar sits when it is hidden.
        col->addStretch();
        m_time = makeLabel("--:--", "gain", Qt::AlignHCenter);
        m_time->setMinimumWidth(bbui::px(112));
        col->addWidget(m_time);
        m_progress = new QProgressBar;
        m_progress->setTextVisible(false);
        m_progress->setFixedHeight(bbui::px(6));
        m_progress->setRange(0, 1000);
        m_progress->setValue(0);
        col->addWidget(m_progress);
        m_status = makeLabel("idle", "caption", Qt::AlignHCenter);
        col->addWidget(m_status);
        col->addStretch();
        root->addLayout(col);
    }

    {
        m_meter = new LevelMeter(kChan);
        m_meter->setFixedHeight(bbui::px(48));
        m_meter->setToolTip("The bus being recorded");
        root->addWidget(m_meter, 0, Qt::AlignVCenter);
    }

    {   // Caption beside the knob, not stacked above it: stacking is what
        // inflated the whole bar by a row for one word.
        auto* gr = new QHBoxLayout;
        gr->setSpacing(bbui::gapXS());
        gr->addWidget(makeLabel("GAIN", "caption", Qt::AlignRight | Qt::AlignVCenter));
        m_gain = new Knob(-600, 120, 0, true, " dB");
        m_gain->setToolTip("Level of the file played into the mixer");
        connect(m_gain, &Knob::valueChanged, this,
                [this](int v) { m_shm->rec.gain_db.store(v / 10.0f); });
        gr->addWidget(m_gain);
        root->addLayout(gr);
    }
}

void RecorderWidget::refresh()
{
    const int st = m_shm->rec.state.load();
    const uint32_t wr = m_shm->rec.frames_written.load();
    const uint32_t pl = m_shm->rec.frames_played.load();
    const uint32_t tot = m_shm->rec.total_frames.load();

    { QSignalBlocker b(m_rec);  m_rec->setChecked(st == kRecRecording); }
    // Pulse the record chip while it is actually recording. Two seconds of a
    // steady red chip and two seconds of an armed one look identical.
    {
        const bool recording = (st == kRecRecording);
        const bool lit = !recording || (m_pulse.elapsed() % 1000) < 620;
        const char* want = lit ? "rec" : "recdim";
        if (m_rec->property("role").toString() != QLatin1String(want)) {
            m_rec->setProperty("role", want);
            m_rec->style()->unpolish(m_rec);
            m_rec->style()->polish(m_rec);
        }
    }
    { QSignalBlocker b(m_play); m_play->setChecked(st == kRecPlaying); }
    { QSignalBlocker b(m_loop); m_loop->setChecked(m_shm->rec.loop.load() != 0); }

    auto mmss = [](uint32_t fr) {
        const uint32_t s = fr / 48000;
        return QString::asprintf("%u:%02u", s / 60, s % 60);
    };
    const int err = m_shm->rec.err.load();

    // The offending field is marked, rather than an error printed 450px away.
    // Guarded: this runs thirty times a second.
    restyle(m_recPath,  "bad", err == 1);
    restyle(m_playPath, "bad", err == 2);

    if (err == 1)      m_status->setText("cannot open the record file");
    else if (err == 2) m_status->setText("cannot open the playback file");
    else if (st == kRecRecording) m_status->setText("recording");
    else if (st == kRecPlaying)   m_status->setText("playing");
    else if (wr) m_status->setText("stopped");
    else m_status->setText("idle");

    if (st == kRecRecording)   m_time->setText(mmss(wr));
    else if (st == kRecPlaying) m_time->setText(mmss(pl) + " / " + mmss(tot));
    else if (wr)                m_time->setText(mmss(wr));
    else                        m_time->setText("--:--");

    m_progress->setValue(st == kRecPlaying && tot > 0
                             ? int(qBound<double>(0, 1000.0 * pl / tot, 1000))
                             : 0);
    m_progress->setVisible(st == kRecPlaying && tot > 0);

    // Meter whatever is being recorded, so a take that is silent looks silent.
    if (st == kRecRecording) {
        const int b = qBound(0, m_shm->rec.source_bus.load(), kBuses - 1);
        float v[kChan];
        for (int c = 0; c < kChan; ++c)
            v[c] = m_shm->meters.bus_out[b][c].load(std::memory_order_relaxed);
        m_meter->setLevels(v, kChan);
    } else {
        m_meter->setStale(true);
    }

    // Nothing to record to, nothing to play: say so with the control's state.
    // Guarded: refresh() runs thirty times a second, and setEnabled on an
    // unchanged value still walks the widget's children.
    const bool canRec  = !m_recPath->text().trimmed().isEmpty();
    const bool canPlay = !m_playPath->text().trimmed().isEmpty();
    if (m_rec->isEnabled()  != canRec)  m_rec->setEnabled(canRec);
    if (m_play->isEnabled() != canPlay) m_play->setEnabled(canPlay);
    if (m_loop->isEnabled() != canPlay) m_loop->setEnabled(canPlay);

    for (int b = 0; b < m_busBtns.size(); ++b) {
        QSignalBlocker blk(m_busBtns[b]);
        m_busBtns[b]->setChecked(m_shm->rec.bus_on[b].load() != 0);
    }
}

// ---------------------------------------------------------------------------
// VbanDialog
// ---------------------------------------------------------------------------
VbanDialog::VbanDialog(Shared* shm, QWidget* parent) : QDialog(parent), m_shm(shm)
{
    setWindowTitle("VBAN network streams");
    // What to put back if the user cancels. Copied field by field rather than
    // by memcpy of VbanConfig, which holds an atomic the engine is reading.
    for (int i = 0; i < kVbanStreams; ++i) {
        m_out0[i] = m_shm->vban.out[i];
        m_in0[i]  = m_shm->vban.in[i];
    }
    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header("VBAN network streams",
        "Send a bus to another machine on the network, or receive one as a new input."));

    auto* outBox = new QGroupBox("OUTGOING  (a bus sent to a remote host)");
    auto* og = new QGridLayout(outBox);
    og->setSpacing(bbui::gapS() - 1);
    og->addWidget(makeLabel("ON", "caption"), 0, 0);
    og->addWidget(makeLabel("STREAM NAME", "caption"), 0, 1);
    og->addWidget(makeLabel("DESTINATION IP", "caption"), 0, 2);
    og->addWidget(makeLabel("PORT", "caption"), 0, 3);
    og->addWidget(makeLabel("SOURCE BUS", "caption"), 0, 4);
    for (int i = 0; i < kVbanStreams; ++i) {
        VbanOutCfg& c = m_shm->vban.out[i];
        m_out[i].on   = new QCheckBox;              m_out[i].on->setChecked(c.enabled != 0);
        m_out[i].name = new QLineEdit(c.name);      m_out[i].name->setMaximumWidth(bbui::px(130));
        m_out[i].host = new QLineEdit(c.host);      m_out[i].host->setMaximumWidth(bbui::px(130));
        m_out[i].host->setPlaceholderText("e.g. 192.168.1.20");
        m_out[i].port = new QSpinBox;               m_out[i].port->setRange(1, 65535);
        m_out[i].port->setValue(c.port);
        m_out[i].bus  = new QComboBox;
        for (int b = 0; b < kBuses; ++b) m_out[i].bus->addItem(kBusLabel[b]);
        m_out[i].bus->setCurrentIndex(qBound(0, c.source_bus, kBuses - 1));
        og->addWidget(m_out[i].on,   i + 1, 0);
        og->addWidget(m_out[i].name, i + 1, 1);
        og->addWidget(m_out[i].host, i + 1, 2);
        og->addWidget(m_out[i].port, i + 1, 3);
        og->addWidget(m_out[i].bus,  i + 1, 4);
    }
    root->addWidget(outBox);

    auto* inBox = new QGroupBox("INCOMING  (each becomes a selectable source)");
    auto* ig = new QGridLayout(inBox);
    ig->setSpacing(bbui::gapS() - 1);
    ig->addWidget(makeLabel("ON", "caption"), 0, 0);
    ig->addWidget(makeLabel("STREAM NAME", "caption"), 0, 1);
    ig->addWidget(makeLabel("PORT", "caption"), 0, 2);
    ig->addWidget(makeLabel("APPEARS AS", "caption"), 0, 3);
    for (int i = 0; i < kVbanStreams; ++i) {
        VbanInCfg& c = m_shm->vban.in[i];
        m_in[i].on   = new QCheckBox;           m_in[i].on->setChecked(c.enabled != 0);
        m_in[i].name = new QLineEdit(c.name);   m_in[i].name->setMaximumWidth(bbui::px(130));
        m_in[i].port = new QSpinBox;            m_in[i].port->setRange(1, 65535);
        m_in[i].port->setValue(c.port);
        ig->addWidget(m_in[i].on,   i + 1, 0);
        ig->addWidget(m_in[i].name, i + 1, 1);
        ig->addWidget(m_in[i].port, i + 1, 2);
        ig->addWidget(makeLabel(QString("bb_vban_in_%1").arg(i + 1), "caption", Qt::AlignLeft),
                      i + 1, 3);
    }
    root->addWidget(inBox);

    // Three exits meaning three different things is one too many: "Apply"
    // applied, "Close" also applied without saying so, and Escape discarded.
    // Now the words match what happens.
    auto* okBtn     = new QPushButton("Apply and close");
    auto* cancelBtn = new QPushButton("Cancel");
    connect(okBtn,     &QPushButton::clicked, this, [this]{ apply(); accept(); });
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    root->addLayout(bbdlg::buttonRow(okBtn, cancelBtn));
    // Deliberately no default button: Return in a port or an IP field must not
    // push network configuration at the engine and dismiss the dialog.
    bbdlg::tameDefaults(this);
    bbdlg::rememberGeometry(this, "vban");
}

bool VbanDialog::revert()
{
    // Nothing to put back unless Apply was actually pressed. Escape on an
    // untouched dialog should not make the engine rebuild every stream.
    bool changed = false;
    for (int i = 0; i < kVbanStreams && !changed; ++i)
        changed = std::memcmp(&m_shm->vban.out[i], &m_out0[i], sizeof(VbanOutCfg)) != 0
               || std::memcmp(&m_shm->vban.in[i],  &m_in0[i],  sizeof(VbanInCfg))  != 0;
    if (!changed) return false;

    m_shm->vban.seq.fetch_add(1, std::memory_order_acq_rel);
    for (int i = 0; i < kVbanStreams; ++i) {
        m_shm->vban.out[i] = m_out0[i];
        m_shm->vban.in[i]  = m_in0[i];
    }
    m_shm->vban.seq.fetch_add(1, std::memory_order_release);
    return true;
}

void VbanDialog::reject()
{
    if (revert()) {
        m_shm->cmd.store(kCmdVbanReload);
        m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
    }
    QDialog::reject();
}

void VbanDialog::apply()
{
    m_shm->vban.seq.fetch_add(1, std::memory_order_acq_rel);
    for (int i = 0; i < kVbanStreams; ++i) {
        VbanOutCfg& c = m_shm->vban.out[i];
        c.enabled    = m_out[i].on->isChecked() ? 1 : 0;
        c.port       = m_out[i].port->value();
        c.source_bus = m_out[i].bus->currentIndex();
        c.channels   = 2; c.rate = 48000;
        snprintf(c.name, sizeof(c.name), "%s", m_out[i].name->text().toUtf8().constData());
        snprintf(c.host, sizeof(c.host), "%s", m_out[i].host->text().toUtf8().constData());
        VbanInCfg& n = m_shm->vban.in[i];
        n.enabled  = m_in[i].on->isChecked() ? 1 : 0;
        n.port     = m_in[i].port->value();
        n.channels = 2; n.rate = 48000;
        snprintf(n.name, sizeof(n.name), "%s", m_in[i].name->text().toUtf8().constData());
    }
    m_shm->vban.seq.fetch_add(1, std::memory_order_release);
}



// ---------------------------------------------------------------------------
// Device discovery. PipeWire node names ("alsa_output.usb-BEHRINGER_..._00.
// HiFi__Line__sink") are unreadable, so the lists show pactl's `description`
// ("UMC202HD 192k Line A") and keep node.name only as the stored value.
// ---------------------------------------------------------------------------
struct DevEntry { QString id, label; bool captureOnly = false; };

static QString pactlRun(const QStringList& args)
{
    QProcess p;
    p.start("pactl", args);
    if (!p.waitForFinished(3000)) return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
}

// The same thing without stopping the world. The application-routing pass runs
// once a second from the timer that also drives the meters, and it made five of
// these round trips per pass at 16-18 ms each - so the meters dropped a frame
// or two every second, for as long as the mixer was open, and a wedged
// pipewire-pulse would freeze the window for fifteen seconds.
//
// The callback is shared rather than moved: `finished` and `errorOccurred` can
// both be connected, and only one of them will run.
static void pactlAsync(const QStringList& args, QObject* ctx,
                       std::function<void(const QString&)> done)
{
    auto cb = std::make_shared<std::function<void(const QString&)>>(std::move(done));
    auto* p = new QProcess(ctx);
    auto fire = [p, cb](const QString& out) {
        if (!*cb) return;
        auto call = *cb;
        *cb = nullptr;                       // exactly once
        call(out);
        p->deleteLater();
    };
    QObject::connect(p, &QProcess::finished, ctx, [p, fire](int, QProcess::ExitStatus) {
        fire(QString::fromUtf8(p->readAllStandardOutput()));
    });
    QObject::connect(p, &QProcess::errorOccurred, ctx, [fire](QProcess::ProcessError) {
        fire(QString());
    });
    // The synchronous form this replaced gave up after three seconds. Without
    // the same bound a wedged pipewire-pulse leaves the round in flight, and
    // its answer arrives long after the streams it describes have moved.
    QTimer::singleShot(3000, p, [p, fire] {
        if (p->state() != QProcess::NotRunning) p->kill();
        fire(QString());
    });
    p->start("pactl", args);
}

// `own`: include the engine's own endpoints (wanted when choosing where an
// application should play, unwanted when choosing a physical device).
static QVector<DevEntry> parseDevices(const QString& json, bool own)
{
    QVector<DevEntry> v;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray()) return v;
    for (const QJsonValue& val : doc.array()) {
        const QJsonObject o = val.toObject();
        const QString id = o.value("properties").toObject().value("node.name").toString();
        if (id.isEmpty()) continue;
        QString label = o.value("description").toString();
        if (label.isEmpty()) label = id;
        if (label.startsWith("Monitor of ")) continue;      // never route from a monitor
        const bool ours = id.startsWith("bb_");
        const bool vbanIn = id.startsWith("bb_vban_in_");
        if (ours && !own && !vbanIn) continue;
        // The property is authoritative; the name is the fallback, because
        // PipeWire only re-reads pipewire.conf.d at startup and an existing
        // install will not carry the property until it is restarted.
        const bool captureOnly =
            o.value("properties").toObject().value("betterbanana.capture-only").toString() == "true"
            || id == QLatin1String(kStreamSinkName);
        v.append({ id, label, captureOnly });
    }
    return v;
}

static QVector<DevEntry> listDevices(bool sinks, bool own)
{
    return parseDevices(pactlRun({ "-f", "json", "list", sinks ? "sinks" : "sources" }), own);
}

// Puts the entries whose id is in `first` at the top, in that order.
static QVector<DevEntry> promote(QVector<DevEntry> in, const QStringList& first)
{
    QVector<DevEntry> out;
    for (const QString& id : first)
        for (const DevEntry& d : in) if (d.id == id) { out.append(d); break; }
    for (const DevEntry& d : in)
        if (!first.contains(d.id)) out.append(d);
    return out;
}


DuckDialog::DuckDialog(Shared* shm, QWidget* parent) : QDialog(parent), m_shm(shm)
{
    setWindowTitle("Sidechain ducking");
    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header("Sidechain ducking",
        "Strips marked KEY pull down every strip that has a depth set, so music "
        "gets out of the way while you talk."));

    auto* top = new QHBoxLayout;
    top->setSpacing(bbui::gapM());
    m_on = makeToggle("DUCKING ON", "eq", bbui::px(24));
    m_on->setChecked(m_shm->duck_enabled.load() != 0);
    connect(m_on, &QPushButton::toggled, this,
            [this](bool b) { m_shm->duck_enabled.store(b ? 1 : 0); });
    top->addWidget(m_on);

    auto addKnob = [&](const QString& cap, int lo, int hi, int val, double scale,
                       int decimals, const QString& suffix, std::function<void(int)> set) {
        auto* col = new QVBoxLayout;
        col->addWidget(makeLabel(cap, "caption"));
        auto* k = new Knob(lo, hi, val, false, suffix);
        k->setMinimumWidth(bbui::px(52));
        k->setScale(scale);
        k->setDecimals(decimals);
        k->setValue(val);
        QObject::connect(k, &Knob::valueChanged, this, set);
        col->addWidget(k, 0, Qt::AlignHCenter);
        top->addLayout(col);
    };
    // These ranges track bb-ctl's clamps exactly (the "duck" block in
    // tools/bb-ctl.cpp). A knob narrower than the shell accepts leaves a
    // scripted value unreachable here, and pulls it in the moment anyone
    // nudges the knob - a silent edit nobody asked for.
    addKnob("THRESHOLD", -800, 0, int(m_shm->duck_threshold_db.load() * 10), 0.1, 1, " dB",
            [this](int v) { m_shm->duck_threshold_db.store(v / 10.0f); });
    addKnob("ATTACK", 1, 500, int(m_shm->duck_attack_ms.load()), 1.0, 0, " ms",
            [this](int v) { m_shm->duck_attack_ms.store(float(v)); });
    addKnob("RELEASE", 10, 5000, int(m_shm->duck_release_ms.load()), 1.0, 0, " ms",
            [this](int v) { m_shm->duck_release_ms.store(float(v)); });
    top->addStretch();
    m_env = makeLabel("idle", "value", Qt::AlignRight | Qt::AlignVCenter);
    m_env->setMinimumWidth(bbui::px(120));
    top->addWidget(m_env);
    root->addLayout(top);

    auto* grid = new QGridLayout;
    grid->setSpacing(bbui::gapS());
    grid->addWidget(makeLabel("STRIP", "caption", Qt::AlignLeft), 0, 0);
    grid->addWidget(makeLabel("KEY (triggers ducking)", "caption"), 0, 1);
    grid->addWidget(makeLabel("DEPTH (how far it drops)", "caption"), 0, 2);
    for (int i = 0; i < kStrips; ++i) {
        StripParams& p = m_shm->strip[i];
        grid->addWidget(makeLabel(labelFor(m_shm, true, i, kStripTitle[i]), "gain", Qt::AlignLeft), i + 1, 0);
        auto* key = makeToggle("KEY", "rec");
        key->setChecked(p.duck_key.load() != 0);
        connect(key, &QPushButton::toggled, this, [&p](bool b) { p.duck_key.store(b ? 1 : 0); });
        grid->addWidget(key, i + 1, 1);
        auto* depth = new Knob(-600, 0, 0, false, " dB");   // as bb-ctl strip <i> duck
        depth->setMinimumWidth(bbui::px(52));
        depth->setValue(int(p.duck_depth_db.load() * 10));
        connect(depth, &Knob::valueChanged, this, [&p](int v) { p.duck_depth_db.store(v / 10.0f); });
        grid->addWidget(depth, i + 1, 2, Qt::AlignHCenter);
    }
    root->addLayout(grid);

    auto* btns = new QHBoxLayout;
    btns->addStretch();
    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    btns->addWidget(close);
    root->addLayout(btns);

    resize(560, 400);
    bbdlg::rememberGeometry(this, "duck");
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &DuckDialog::refresh);
    m_timer->start(60);
}

void DuckDialog::refresh()
{
    const float e = m_shm->meters.duck_env.load(std::memory_order_relaxed);
    m_env->setText(e < 0.02f ? "idle" : QString::asprintf("ducking  %.0f%%", e * 100.0));
}

// ---------------------------------------------------------------------------
// AppsDialog
// ---------------------------------------------------------------------------

// BetterBanana endpoints first: those are what you normally want to pick.
static void listTargets(bool playback, QStringList& ids, QStringList& labels)
{
    const QStringList prefer = playback
        ? QStringList{ "bb_vaio", "bb_aux", "bb_cable1", "bb_cable2", "bb_cable3" }
        : QStringList{ "bb_b1", "bb_b2" };
    for (const DevEntry& d : promote(listDevices(playback, true), prefer)) {
        if (playback && d.captureOnly) continue;
        ids << d.id;
        labels << (prefer.contains(d.id) ? ("\u2192 " + d.label) : d.label);
    }
}

// One playback or capture stream belonging to an application.
struct StreamInfo {
    int     index = -1;
    QString app;        // application.name - the key auto-routing rules use
    QString label;      // what the user sees
    QString target;     // node.name it is currently attached to
    // What the application itself asked for. When this disagrees with `target`,
    // WirePlumber is replaying a saved choice over the top of the application's
    // own request - and no setting inside the application will fix it.
    QString wanted;
};

static QVector<StreamInfo> parseStreams(const QString& js, const QString& shortList,
                                       bool playback)
{
    QVector<StreamInfo> v;
    const QJsonDocument doc = QJsonDocument::fromJson(js.toUtf8());
    if (!doc.isArray()) return v;

    QMap<int, QString> byId;
    for (const QString& ln : shortList.split('\n', Qt::SkipEmptyParts)) {
        const QStringList f = ln.split('\t');
        if (f.size() >= 2) byId[f.at(0).toInt()] = f.at(1);
    }

    for (const QJsonValue& val : doc.array()) {
        const QJsonObject o = val.toObject();
        const QJsonObject props = o.value("properties").toObject();
        const QString app = props.value("application.name").toString();
        const QString node = props.value("node.name").toString();
        // Our own endpoints are not user applications.
        if (app == "BetterBanana" || node.startsWith("bb_")) continue;

        StreamInfo si;
        si.index = o.value("index").toInt(-1);
        si.app = app.isEmpty()
                   ? props.value("application.process.binary").toString()
                   : app;
        if (si.app.isEmpty()) si.app = node;
        const QString media = props.value("media.name").toString();
        si.label = si.app;
        if (!media.isEmpty() && media != si.app) si.label += "  \u2014  " + media;
        si.target = byId.value(o.value(playback ? "sink" : "source").toInt(-1));
        si.wanted = props.value("target.object").toString();
        v.append(si);
    }
    return v;
}

static QVector<StreamInfo> listStreams(bool playback)
{
    return parseStreams(pactlRun({ "-f", "json", "list",
                                   playback ? "sink-inputs" : "source-outputs" }),
                        pactlRun({ "list", "short", playback ? "sinks" : "sources" }),
                        playback);
}

// Auto-routing rules, remembered per application name.
static QString ruleFor(const QString& app, bool playback)
{
    QSettings st("betterbanana", "gui");
    return st.value(QString("approutes/%1/%2").arg(playback ? "play" : "cap", app)).toString();
}
static void setRule(const QString& app, bool playback, const QString& target)
{
    QSettings st("betterbanana", "gui");
    const QString key = QString("approutes/%1/%2").arg(playback ? "play" : "cap", app);
    if (target.isEmpty()) st.remove(key); else st.setValue(key, target);
}
static QVector<QPair<QString, bool>> allRules()
{
    QVector<QPair<QString, bool>> v;
    QSettings st("betterbanana", "gui");
    for (bool pb : { true, false }) {
        st.beginGroup(QString("approutes/%1").arg(pb ? "play" : "cap"));
        for (const QString& k : st.childKeys()) v.append({ k, pb });
        st.endGroup();
    }
    return v;
}

AppsDialog::AppsDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Applications");
    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header("Applications",
        "Where each program's audio goes. A choice made here is remembered and "
        "re-applied the next time that program starts."));

    auto* playBox = new QGroupBox("PLAYING  (send an app into a BetterBanana strip)");
    m_playLay = new QVBoxLayout(playBox);
    m_playEmpty = makeLabel("nothing playing", "caption", Qt::AlignLeft);
    m_playLay->addWidget(m_playEmpty);
    root->addWidget(playBox);

    auto* capBox = new QGroupBox("RECORDING  (point an app at a BetterBanana bus)");
    m_capLay = new QVBoxLayout(capBox);
    m_capEmpty = makeLabel("nothing recording", "caption", Qt::AlignLeft);
    m_capLay->addWidget(m_capEmpty);
    root->addWidget(capBox);

    auto* memBox = new QGroupBox("REMEMBERED  (not playing now; applied when they next start)");
    m_memLay = new QVBoxLayout(memBox);
    m_memEmpty = makeLabel("no saved application rules yet", "caption", Qt::AlignLeft);
    m_memLay->addWidget(m_memEmpty);
    root->addWidget(memBox);

    root->addWidget(makeLabel(
        "Pick a target and it is remembered; the app is re-routed automatically "
        "whenever it starts playing again.", "caption", Qt::AlignLeft));

    auto* btns = new QHBoxLayout;
    btns->addStretch();
    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    btns->addWidget(close);
    root->addLayout(btns);

    resize(620, 460);
    refresh();
    bbdlg::rememberGeometry(this, "apps");
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AppsDialog::refresh);
    m_timer->start(1000);
}

void AppsDialog::rebuild(bool playback, const QVector<StreamInfo>& streams,
                         const QStringList& devIds, const QStringList& devLabels)
{
    QVBoxLayout* lay = playback ? m_playLay : m_capLay;

    for (int i = m_rows.size() - 1; i >= 0; --i) {
        if (m_rows[i].playback != playback) continue;
        bool found = false;
        for (const auto& s : streams) if (s.index == m_rows[i].index) { found = true; break; }
        if (!found) { delete m_rows[i].holder; m_rows.remove(i); }
    }

    for (const StreamInfo& s : streams) {
        Row* row = nullptr;
        for (auto& r : m_rows) if (r.playback == playback && r.index == s.index) { row = &r; break; }

        if (!row) {
            Row nr;
            nr.index = s.index;
            nr.playback = playback;
            nr.app = s.app;
            nr.holder = new QWidget;
            auto* h = new QHBoxLayout(nr.holder);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(bbui::px(8));
            nr.name = makeLabel(s.label, "gain", Qt::AlignLeft);
            nr.name->setMinimumWidth(bbui::px(200));
            nr.target = new QComboBox;
            nr.target->setMinimumWidth(bbui::px(230));
            const int idx = nr.index;
            const bool pb = playback;
            const QString app = s.app;
            QComboBox* combo = nr.target;
            connect(combo, &QComboBox::activated, combo, [combo, idx, pb, app](int) {
                const QString id = combo->currentData().toString();
                if (id.isEmpty()) return;
                pactlRun({ pb ? "move-sink-input" : "move-source-output",
                           QString::number(idx), id });
                // Remember it, so the app lands here again next time it plays.
                setRule(app, pb, id);
            });
            // A running app's saved rule is otherwise unreachable: the
            // REMEMBERED list below deliberately skips apps that are live, so
            // a bad rule would keep re-routing this stream with nothing in the
            // UI to point at, let alone remove.
            nr.forget = new QPushButton("Forget");
            nr.forget->setFixedWidth(bbui::px(64));
            nr.forget->hide();
            connect(nr.forget, &QPushButton::clicked, this, [this, app, pb] {
                setRule(app, pb, QString());
                m_memShown.clear();
                refresh();
            });
            h->addWidget(nr.name, 0, Qt::AlignVCenter);
            h->addWidget(nr.target, 1, Qt::AlignVCenter);
            h->addWidget(nr.forget, 0, Qt::AlignVCenter);
            lay->addWidget(nr.holder);
            m_rows.append(nr);
            row = &m_rows.last();
        }

        row->name->setText(s.label);
        const QString rule = ruleFor(row->app, playback);
        row->forget->setVisible(!rule.isEmpty());
        row->forget->setToolTip(rule.isEmpty()
            ? QString()
            : QString("Auto-routed to %1 whenever it starts.\nClick to forget the rule.").arg(rule));
        // A stream whose current target is not one of the offered entries used
        // to leave the combo sitting on its first item, reading as a route that
        // does not exist -- and one stray click there would save a rule saying
        // so. Two ways in: the session manager is not routing the stream at all
        // (source/sink is PW_ID_ANY, as for Discord's screen-share capture,
        // which bb-stream-guard wires by port id), or the target is real but
        // deliberately absent from the list, like the capture-only stream bus.
        // Name the actual state instead, on an entry that carries no id, so
        // picking it moves nothing and remembers nothing.
        const QString extra = devIds.contains(s.target)
            ? QString()
            : (s.target.isEmpty() ? QStringLiteral("- not routed by BetterBanana -")
                                  : s.target + "  (not selectable here)");
        const int wantCount = devIds.size() + (extra.isEmpty() ? 0 : 1);
        if (!row->target->view()->isVisible()
            && (row->target->count() != wantCount || row->extra != extra)) {
            row->extra = extra;
            row->target->clear();
            if (!extra.isEmpty()) row->target->addItem(extra, QString());
            for (int i = 0; i < devIds.size(); ++i)
                row->target->addItem(devLabels.value(i), devIds.at(i));
        }
        if (!row->target->view()->isVisible()) {
            const int want = extra.isEmpty() ? row->target->findData(s.target) : 0;
            if (want >= 0 && want != row->target->currentIndex())
                row->target->setCurrentIndex(want);
        }
    }

    (playback ? m_playEmpty : m_capEmpty)->setVisible(streams.isEmpty());
}

// Applications that are not making sound right now but have a saved rule.
void AppsDialog::rebuildRemembered()
{
    QStringList live;
    for (bool pb : { true, false })
        for (const StreamInfo& s : listStreams(pb)) live << s.app;

    QStringList want;
    for (const auto& r : allRules()) if (!live.contains(r.first)) want << r.first;
    want.sort();
    if (want == m_memShown) return;
    m_memShown = want;

    qDeleteAll(m_memRows);
    m_memRows.clear();
    for (const auto& r : allRules()) {
        if (live.contains(r.first)) continue;
        auto* holder = new QWidget;
        auto* h = new QHBoxLayout(holder);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(bbui::px(8));
        auto* nm = makeLabel(r.first + (r.second ? "" : "  (mic)"), "caption", Qt::AlignLeft);
        nm->setMinimumWidth(bbui::px(200));
        auto* tgt = makeLabel(ruleFor(r.first, r.second), "gain", Qt::AlignLeft);
        auto* forget = new QPushButton("Forget");
        forget->setFixedWidth(bbui::px(64));
        const QString app = r.first;
        const bool pb = r.second;
        connect(forget, &QPushButton::clicked, this, [this, app, pb] {
            setRule(app, pb, QString());
            m_memShown.clear();
            rebuildRemembered();
        });
        h->addWidget(nm, 0, Qt::AlignVCenter);
        h->addWidget(tgt, 1, Qt::AlignVCenter);
        h->addWidget(forget, 0, Qt::AlignVCenter);
        m_memLay->addWidget(holder);
        m_memRows.append(holder);
    }
    m_memEmpty->setVisible(m_memRows.isEmpty());
}

void AppsDialog::refresh()
{
    QStringList sinkIds, sinkLabels, srcIds, srcLabels;
    listTargets(true, sinkIds, sinkLabels);
    listTargets(false, srcIds, srcLabels);
    rebuild(true,  listStreams(true),  sinkIds, sinkLabels);
    rebuild(false, listStreams(false), srcIds, srcLabels);
    rebuildRemembered();
}


// ---------------------------------------------------------------------------
// Time alignment.
//
// Two output devices almost never have the same latency. A Bluetooth headset
// runs about a quarter of a second behind a USB interface, so anything feeding
// both arrives twice, far enough apart to hear. The fix is to hold the early
// one back - and it has to be done on the BUS, because a strip feeds both and
// delaying a strip moves both together.
//
// The figures are read, not measured: PipeWire already reports one for every
// device, and for a Bluetooth sink it includes the codec and link delay, which
// is the part nothing else can see.
//
// What this dialog has to do, though, is not report figures - it is to answer
// one question, "do these arrive together?", for somebody who does not think in
// milliseconds. So it adds the two numbers up rather than leaving that to the
// reader, draws the sums to a shared scale so they can be compared by eye,
// leads with a sentence saying what the gap would actually sound like, and ends
// with a tick you can listen to. A reported figure you cannot check is a figure
// you have to take on faith, and the Bluetooth one is negotiated with the
// headset rather than declared by it - so it is the one most worth checking.
// ---------------------------------------------------------------------------

// The one device class whose figure is negotiated per connection rather than
// fixed by the hardware, so it is worth saying which row is one.
static bool looks_bluetooth(const QString& node, const QString& desc)
{
    return node.contains("bluez", Qt::CaseInsensitive)
        || node.contains("bluetooth", Qt::CaseInsensitive)
        || desc.contains("bluetooth", Qt::CaseInsensitive);
}

// mm:ss.mmm is no use here; everything is tens or hundreds of milliseconds.
static QString msText(float ms)
{
    return ms < 0.0f ? QString("--") : QString::asprintf("%.1f ms", ms);
}

AlignBarCell::AlignBarCell(QWidget* parent) : QWidget(parent)
{
    setMinimumWidth(bbui::px(120));
    setMinimumHeight(bbui::px(16));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void AlignBarCell::setScale(float fullMs, float targetMs)
{
    m_full = fullMs > 1.0f ? fullMs : 1.0f;
    m_target = targetMs;
    update();
}

void AlignBarCell::setValues(float latencyMs, float delayMs, bool included)
{
    m_lat = latencyMs;
    m_delay = delayMs > 0.0f ? delayMs : 0.0f;
    m_inc = included;
    update();
}

void AlignBarCell::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = theme();
    const qreal rad = bbui::radWell();
    const QRectF box = QRectF(rect()).adjusted(0.0, bbui::px(3), -1.0, -bbui::px(3));

    p.setPen(Qt::NoPen);
    p.setBrush(t.well);
    p.drawRoundedRect(box, rad, rad);

    auto atX = [&](float ms) {
        const double f = std::clamp((double)ms / (double)m_full, 0.0, 1.0);
        return box.left() + f * box.width();
    };

    if (m_lat < 0.0f) {
        // Nothing reported. A zero-length bar would read as "instant", which is
        // the one thing it definitely does not mean.
        p.setPen(QPen(t.textDim, 1.0, Qt::DotLine));
        const qreal y = box.center().y();
        p.drawLine(QPointF(box.left() + 3, y), QPointF(box.right() - 3, y));
        return;
    }

    // Two segments, because they are two different things: what the device
    // costs and cannot be argued with, and what the mixer chose to add. Told
    // apart three ways over, so no palette can flatten them into one bar:
    // a dimmer tone, a shorter bar, and a seam of trough between them.
    const QColor dev = alignDeviceColour(t, m_inc);
    const QColor add = alignDelayColour(t, m_inc);
    const qreal x0 = box.left(), x1 = atX(m_lat), x2 = atX(m_lat + m_delay);

    p.setBrush(dev);
    if (x1 > x0 + 0.5) {
        QPainterPath path;
        path.addRoundedRect(QRectF(x0, box.top(), x1 - x0, box.height()), rad, rad);
        p.drawPath(path);
    }
    p.setBrush(add);
    if (x2 > x1 + 1.5)
        p.drawRect(QRectF(x1 + 1.0, box.top() + box.height() * 0.22,
                          x2 - x1 - 1.0, box.height() * 0.56));

    // Where everything is trying to meet. A row whose bar ends on this line is
    // aligned; the distance from the end of the bar to the line is the error.
    if (m_target >= 0.0f) {
        const qreal tx = atX(m_target);
        p.setPen(QPen(t.text, 1.0, Qt::DashLine));
        p.drawLine(QPointF(tx, box.top() - bbui::px(2)),
                   QPointF(tx, box.bottom() + bbui::px(2)));
    }
}

AlignDialog::AlignDialog(Shared* shm, QWidget* parent)
    : QDialog(parent), m_shm(shm)
{
    setWindowTitle("Time alignment");
    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header("Time alignment",
        "Making Bluetooth headphones and everything else arrive at the same moment"));

    // Enough sections that a small screen has to be able to scroll them.
    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    auto* holder = new QWidget;
    auto* col = new QVBoxLayout(holder);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(bbui::gapM());
    area->setWidget(holder);
    root->addWidget(area, 1);

    auto section = [&](const QString& title, const QString& sub) {
        auto* head = new QLabel(title);
        head->setProperty("role", "value");
        col->addWidget(head);
        auto* s = new QLabel(sub);
        s->setProperty("role", "caption");
        s->setWordWrap(true);
        col->addWidget(s);
    };

    // --- the verdict ------------------------------------------------------
    // The lead, because it is the only thing most people open this for. A
    // number nobody can interpret is not an answer; "you would hear that as an
    // echo" is.
    m_verdict = new QFrame;
    m_verdict->setProperty("role", "verdict");
    {
        auto* v = new QVBoxLayout(m_verdict);
        v->setContentsMargins(bbui::gapM(), bbui::gapS(), bbui::gapM(), bbui::gapS());
        v->setSpacing(bbui::gapXS());
        m_verdictText = new QLabel;
        m_verdictText->setWordWrap(true);
        m_verdictText->setProperty("role", "verdicttop");
        v->addWidget(m_verdictText);
        m_verdictSub = new QLabel;
        m_verdictSub->setWordWrap(true);
        v->addWidget(m_verdictSub);
    }
    col->addWidget(m_verdict);

    // --- a device that moved after it was aligned --------------------------
    m_driftOffer = new QFrame;
    m_driftOffer->setProperty("role", "offer");
    {
        auto* h = new QHBoxLayout(m_driftOffer);
        h->setContentsMargins(bbui::gapM(), bbui::gapS(), bbui::gapM(), bbui::gapS());
        m_driftText = new QLabel;
        m_driftText->setWordWrap(true);
        h->addWidget(m_driftText, 1);
        auto* again = new QPushButton("Align again");
        again->setProperty("cta", "primary");
        connect(again, &QPushButton::clicked, this, &AlignDialog::alignOutputs);
        h->addWidget(again, 0, Qt::AlignVCenter);
    }
    m_driftOffer->hide();
    col->addWidget(m_driftOffer);

    // --- outputs ----------------------------------------------------------
    section("Outputs — what you hear",
            "The delay belongs on the bus, not the strip. One strip feeds both A1 and "
            "A2, so delaying the strip moves both of them together; only a delay here "
            "can separate two outputs. Ticked outputs are the ones aligning considers.");

    auto* og = new QGridLayout;
    og->setHorizontalSpacing(bbui::gapM());
    og->setVerticalSpacing(bbui::gapS());
    {
        int c = 0;
        for (const char* h : { "", "Device", "", "Device takes", "",
                               "Mixer adds", "", "You hear it at",
                               "How that lands", "Align" }) {
            auto* l = new QLabel(h);
            l->setProperty("role", "caption");
            og->addWidget(l, 0, c++);
        }
    }

    auto addOut = [&](Row& row, int b, int r) {
        auto* tag = new QLabel(labelFor(m_shm, false, b, kBusLabel[b]));
        tag->setProperty("role", "value");
        og->addWidget(tag, r, 0);

        row.dev = new QLabel("-");
        row.dev->setProperty("role", "caption");
        row.dev->setMaximumWidth(bbui::px(200));
        og->addWidget(row.dev, r, 1);

        row.kind = new QLabel("BLUETOOTH");
        row.kind->setProperty("role", "diagchip");
        row.kind->setProperty("sev", "accent");
        row.kind->setToolTip(
            "A Bluetooth sink negotiates its codec with the headset every time it "
            "connects, so this figure is not a property of the hardware - it can be "
            "different tomorrow, and the mixer will say so when it changes.");
        row.kind->hide();
        og->addWidget(row.kind, r, 2);

        row.lat = new QLabel("-");
        og->addWidget(row.lat, r, 3);

        auto* plus = new QLabel("+");
        plus->setProperty("role", "caption");
        og->addWidget(plus, r, 4);

        row.delay = new QDoubleSpinBox;
        row.delay->setRange(0.0, (double)Delay::kMaxMs);
        row.delay->setDecimals(1);
        row.delay->setSingleStep(1.0);
        row.delay->setSuffix(" ms");
        row.delay->setToolTip(
            "How much this output is held back, on top of what the device already "
            "costs.\n\nAligning fills this in for you, but it is a plain control: "
            "start the test tick and hold the arrow keys, and you can close the gap "
            "by ear against whatever the reported figure got slightly wrong.");
        connect(row.delay, &QDoubleSpinBox::valueChanged, this, [this, b](double v) {
            m_shm->bus[b].delay_ms.store((float)v);
            refresh();
        });
        og->addWidget(row.delay, r, 5);

        auto* eq = new QLabel("=");
        eq->setProperty("role", "caption");
        og->addWidget(eq, r, 6);

        row.arrive = new QLabel("-");
        row.arrive->setProperty("role", "value");
        row.arrive->setToolTip("When a sound reaches you through this output: what the "
                               "device costs plus what the mixer is adding. This is the "
                               "number that has to match across outputs.");
        og->addWidget(row.arrive, r, 7);

        row.bar = new AlignBarCell;
        og->addWidget(row.bar, r, 8);

        row.inc = new QCheckBox;
        row.inc->setChecked(true);
        row.inc->setToolTip(
            "Include this output when aligning, and tick it for the test click.\n\n"
            "Turn it off for anything nobody is listening to in the room - a "
            "screen-share sink is heard by people somewhere else, on their own "
            "timeline, so delaying it only makes them wait, and if it were the "
            "slowest it would hold the whole room back to match it.");
        connect(row.inc, &QCheckBox::toggled, this, [this] {
            if (m_clicking) setClickRunning(true);       // retarget a running tick
            refresh();
        });
        og->addWidget(row.inc, r, 9, Qt::AlignCenter);
    };
    for (int b = 0; b < kPhysBuses; ++b) addOut(m_out[b], b, b + 1);
    og->setColumnStretch(1, 1);
    og->setColumnStretch(8, 2);
    col->addLayout(og);

    m_scaleCap = new QLabel;
    m_scaleCap->setProperty("role", "caption");
    m_scaleCap->setWordWrap(true);
    col->addWidget(m_scaleCap);

    // The one that makes an alignment stay one. Off by default, because it
    // writes a delay with nobody watching.
    {
        auto* keep = new QCheckBox("Keep them lined up when a device's latency changes");
        keep->setChecked(QSettings("betterbanana", "gui")
                             .value("align/auto", false).toBool());
        keep->setToolTip(
            "A Bluetooth link negotiates its codec with the headset every time it "
            "connects, so the figure an alignment was built on can be different "
            "tomorrow and the alignment quietly stops being one.\n\n"
            "With this on, the mixer notices a reported latency moving and lines the "
            "outputs up again - but only when they WERE lined up beforehand. A mix "
            "you set by hand never has a spread of zero, so it is never touched.");
        connect(keep, &QCheckBox::toggled, this, [](bool on) {
            QSettings("betterbanana", "gui").setValue("align/auto", on);
        });
        col->addWidget(keep);
    }

    // --- check it by ear ---------------------------------------------------
    section("Check it by ear",
            "The figures are what PipeWire reports, and for Bluetooth that is a "
            "negotiation rather than a measurement, so it can be a few milliseconds "
            "out. This is how you find out.");
    {
        auto* h = new QHBoxLayout;
        h->setSpacing(bbui::gapM());
        m_clickBtn = new QPushButton("Play a test tick");
        m_clickBtn->setCheckable(true);
        m_clickBtn->setToolTip(
            "A short tick on every ticked output, from one shared clock, every "
            "0.7 s.\n\nIf they arrive together you hear one tick. If they do not you "
            "hear a flam - two ticks close together - and turning the delay on the "
            "early one walks the flam closed.\n\nThe tick is inserted after the fader "
            "and the limiter, so a bus you have turned down or muted still ticks.");
        connect(m_clickBtn, &QPushButton::toggled, this, &AlignDialog::setClickRunning);
        h->addWidget(m_clickBtn);
        m_clickHint = new QLabel;
        m_clickHint->setProperty("role", "caption");
        m_clickHint->setWordWrap(true);
        h->addWidget(m_clickHint, 1);
        col->addLayout(h);
    }

    // --- what it costs -----------------------------------------------------
    section("What aligning costs you",
            "Aligning can only hold the quick outputs back; nothing can make a "
            "Bluetooth headset faster. So everything ends up as late as the slowest "
            "thing you are listening on, and anything you are watching stays where it "
            "was.");
    m_costText = new QLabel;
    m_costText->setProperty("role", "caption");
    m_costText->setWordWrap(true);
    m_costText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(m_costText);

    // --- inputs ------------------------------------------------------------
    section("Inputs — what the mixer hears",
            "These are not part of aligning the outputs, and the button below leaves "
            "them alone. A strip delay is for the other job: holding a microphone back "
            "to meet a camera, or lining up two microphones on one source.");

    auto* ig = new QGridLayout;
    ig->setHorizontalSpacing(bbui::gapM());
    ig->setVerticalSpacing(bbui::gapS());
    {
        int c = 0;
        for (const char* h : { "", "Device", "", "Device takes", "",
                               "Mixer adds", "", "Reaches the mix at" }) {
            auto* l = new QLabel(h);
            l->setProperty("role", "caption");
            ig->addWidget(l, 0, c++);
        }
    }
    auto addIn = [&](Row& row, int i, int r) {
        auto* tag = new QLabel(labelFor(m_shm, true, i, kStripTitle[i]));
        tag->setProperty("role", "value");
        ig->addWidget(tag, r, 0);
        row.dev = new QLabel("-");
        row.dev->setProperty("role", "caption");
        row.dev->setMaximumWidth(bbui::px(200));
        ig->addWidget(row.dev, r, 1);
        row.kind = new QLabel("BLUETOOTH");
        row.kind->setProperty("role", "diagchip");
        row.kind->setProperty("sev", "accent");
        row.kind->hide();
        ig->addWidget(row.kind, r, 2);
        row.lat = new QLabel("-");
        ig->addWidget(row.lat, r, 3);
        auto* plus = new QLabel("+");
        plus->setProperty("role", "caption");
        ig->addWidget(plus, r, 4);
        row.delay = new QDoubleSpinBox;
        row.delay->setRange(0.0, (double)Delay::kMaxMs);
        row.delay->setDecimals(1);
        row.delay->setSingleStep(1.0);
        row.delay->setSuffix(" ms");
        row.delay->setToolTip(
            "Hold this input back - for lip-sync against a camera, or against another "
            "microphone on the same source. It moves the strip everywhere at once, on "
            "every bus it feeds.");
        connect(row.delay, &QDoubleSpinBox::valueChanged, this, [this, i](double v) {
            m_shm->strip[i].delay_ms.store((float)v);
            refresh();
        });
        ig->addWidget(row.delay, r, 5);
        auto* eq = new QLabel("=");
        eq->setProperty("role", "caption");
        ig->addWidget(eq, r, 6);
        row.arrive = new QLabel("-");
        row.arrive->setProperty("role", "value");
        ig->addWidget(row.arrive, r, 7);
    };
    for (int i = 0; i < kHwStrips; ++i) addIn(m_in[i], i, i + 1);
    ig->setColumnStretch(1, 1);
    col->addLayout(ig);
    col->addStretch(1);

    m_alignBtn = new QPushButton("Align the outputs");
    m_alignBtn->setToolTip("Hold every ticked output back until they all arrive together "
                           "with the slowest one");
    connect(m_alignBtn, &QPushButton::clicked, this, &AlignDialog::alignOutputs);

    // The way back. Aligning was a button and un-aligning was arithmetic you
    // had to do yourself, which is fine right up until the device you aligned
    // against is unplugged - then every output keeps the padding that was
    // holding it back, and the mixer sounds late for no visible reason.
    m_clearBtn = new QPushButton("Clear the output delays");
    connect(m_clearBtn, &QPushButton::clicked, this, &AlignDialog::undoOrClear);

    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* foot = bbdlg::buttonRow(m_alignBtn, close);
    foot->insertWidget(0, m_clearBtn);
    m_status = new bbdlg::StatusStrip(this);
    foot->insertWidget(0, m_status->widget(), 1);
    root->addLayout(foot);

    // A tick left running in someone's headphones is miserable, so it stops
    // with the dialog as well as on the engine's own dead-man timer.
    connect(this, &QDialog::finished, this, [this] {
        if (m_clicking) m_clickBtn->setChecked(false);
    });
    m_clickStop = new QTimer(this);
    m_clickStop->setSingleShot(true);
    connect(m_clickStop, &QTimer::timeout, this, [this] {
        if (m_clicking) m_clickBtn->setChecked(false);
    });

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AlignDialog::refresh);
    m_timer->start(500);
    bbdlg::rememberGeometry(this, "align2");
    bbdlg::tameDefaults(this);
    resize(qMax(width(), bbui::px(940)), qMax(height(), bbui::px(600)));
    refresh();
}

AlignDialog::~AlignDialog()
{
    if (m_shm) m_shm->click_mask.store(0);
}

void AlignDialog::refresh()
{
    char hw[kHwStrips][kNameLen] = {}, bo[kPhysBuses][kNameLen] = {};
    char hwd[kHwStrips][kNameLen] = {}, bod[kPhysBuses][kNameLen] = {};
    uint32_t seq = 0;
    bool ok = false;
    for (int t = 0; t < 16 && !ok; ++t)
        ok = routing_read(m_shm->routing, seq, hw, bo, hwd, bod);

    int known = 0;
    auto fill = [&](Row& row, const QString& dev, const QString& node,
                    float lat, float delay) {
        row.node = node;
        // Elided, not wrapped: a device description runs to sixty characters
        // ("BetterBanana Stream Bus (capture only - do not select)"), and a
        // column that wide pushes the grid past the window, which stops every
        // paragraph in the dialog from wrapping to the width you can see.
        const QString full = dev.isEmpty() ? QString("- nothing assigned -") : dev;
        row.dev->setToolTip(full);
        row.dev->setText(row.dev->fontMetrics().elidedText(
            full, Qt::ElideRight, bbui::px(190)));
        row.bt = !node.isEmpty() && looks_bluetooth(node, dev);
        if (row.kind) row.kind->setVisible(row.bt);
        // Untick a capture-only sink when it ARRIVES on this row, not on every
        // refresh: someone who deliberately ticks it back on should keep it.
        if (row.inc && node != row.lastDev) {
            row.lastDev = node;
            QSignalBlocker block(row.inc);
            row.inc->setChecked(node != QString(kStreamSinkName));
        }
        if (lat >= 0.0f) { row.lat->setText(msText(lat)); ++known; }
        else             row.lat->setText(dev.isEmpty() ? "--" : "not reported");
        if (!row.delay->hasFocus()) {
            QSignalBlocker block(row.delay);
            row.delay->setValue(delay);
        }
        const float arr = arrival_ms(lat, delay);
        row.arrive->setText(arr < 0.0f ? QString("--") : msText(arr));
    };

    float lat[kPhysBuses], del[kPhysBuses];
    bool  inc[kPhysBuses];
    for (int i = 0; i < kHwStrips; ++i)
        fill(m_in[i], ok ? QString::fromUtf8(hwd[i][0] ? hwd[i] : hw[i]) : QString(),
             ok ? QString::fromUtf8(hw[i]) : QString(),
             m_shm->in_latency_ms[i].load(), m_shm->strip[i].delay_ms.load());
    for (int b = 0; b < kPhysBuses; ++b) {
        lat[b] = m_shm->out_latency_ms[b].load();
        del[b] = m_shm->bus[b].delay_ms.load();
        inc[b] = m_out[b].inc->isChecked();
        fill(m_out[b], ok ? QString::fromUtf8(bod[b][0] ? bod[b] : bo[b]) : QString(),
             ok ? QString::fromUtf8(bo[b]) : QString(),
             lat[b], del[b]);
    }

    // --- the bars ---------------------------------------------------------
    // One scale across every row, or they cannot be compared, which is the only
    // reason to draw them. Rounded up so the longest bar does not touch the end.
    int early = -1, late = -1;
    const float spread = arrival_spread_ms(lat, del, kPhysBuses, inc, &early, &late);
    float full = 50.0f, target = -1.0f;
    for (int b = 0; b < kPhysBuses; ++b) {
        const float a = arrival_ms(lat[b], del[b]);
        if (a > full) full = a;
        if (inc[b] && a > target) target = a;
    }
    full = std::ceil(full * 1.12f / 25.0f) * 25.0f;
    for (int b = 0; b < kPhysBuses; ++b) {
        m_out[b].bar->setScale(full, target);
        m_out[b].bar->setValues(lat[b], del[b], inc[b]);
    }
    m_scaleCap->setText(QString(
        "Bars share one scale, 0 to %1 ms. "
        "<span style=\"color:%2\">■</span> what the device costs&nbsp;&nbsp;"
        "<span style=\"color:%3\">▬</span> what the mixer is adding&nbsp;&nbsp;"
        "<span style=\"color:%4\">┆</span> where they are all trying to meet.")
        .arg((int)full)
        .arg(alignDeviceColour(theme()).name(), alignDelayColour(theme()).name(),
             theme().text.name()));

    // --- is anything being held back for no reason? ------------------------
    // Checked before the gap, and it wins. Aligning writes a delay to meet the
    // slowest device in the set; unplug that device and the padding stays on
    // everything else, so the mixer is late for a reason nothing shows. Reading
    // that as an ordinary gap and offering to align would be answering the
    // wrong question.
    float want[kPhysBuses];
    const float excess = excess_delay_ms(lat, del, kPhysBuses, inc, want);
    int worst = -1;
    for (int b = 0; b < kPhysBuses; ++b) {
        if (!inc[b] || lat[b] < 0.0f) continue;
        if (worst < 0 || del[b] - want[b] > del[worst] - want[worst]) worst = b;
    }

    // --- the verdict ------------------------------------------------------
    QString sev = "good", top, sub;
    if (!known) {
        sev = "warn";
        top = "PipeWire has not reported a latency for anything yet.";
        sub = "Assign a device to an A bus and these fill in. Nothing here needs "
              "calibrating - the figure arrives with the device.";
    } else if (excess > 1.0f && worst >= 0) {
        sev = "bad";
        top = QString("%1 is held back %2 more than anything needs — that is why it "
                      "sounds late.")
                  .arg(labelFor(m_shm, false, worst, kBusLabel[worst]), msText(excess));
        sub = "Alignment holds an output back to meet the slowest device in the set. "
              "Unplug that device, or swap it for a quicker one, and the padding stays "
              "behind with nothing left to justify it. Clearing the output delays puts every "
              "output straight through again; aligning does the same for whatever is "
              "plugged in now.";
    } else if (spread < 0.0f) {
        sev = "warn";
        top = "Only one output is ticked, so there is nothing to line it up with.";
        sub = "Alignment is about two things arriving together. Assign and tick a "
              "second output - your speakers as well as your headphones - and this "
              "becomes a comparison.";
    } else {
        const GapVerdict gv = gap_verdict(spread);
        const QString eName = labelFor(m_shm, false, early, kBusLabel[early]);
        const QString lName = labelFor(m_shm, false, late,  kBusLabel[late]);
        if (gv == kGapTogether) {
            sev = "good";
            top = QString("Everything ticked arrives together, within %1.")
                      .arg(msText(spread));
            sub = "That is under a millisecond apart, which is closer than the ear "
                  "can resolve as two events. Save a preset if you want it to survive "
                  "the engine restarting.";
        } else {
            sev = gv == kGapEcho ? "bad" : "warn";
            top = QString("%1 is %2 ahead of %3 — you would hear that as %4.")
                      .arg(eName, msText(spread), lName, gap_sounds_like(gv));
            sub = QString("Aligning holds %1 back by %2 so the two land together. "
                          "Nothing can make %3 quicker; only the early one can move.")
                      .arg(eName, msText(spread), lName);
        }
    }
    m_verdictText->setText(top);
    m_verdictSub->setText(sub);
    if (m_verdict->property("sev").toString() != sev) {
        m_verdict->setProperty("sev", sev);
        // A dynamic property only re-selects the stylesheet on a restyle, and
        // the children carry their own colour rules.
        for (QWidget* w : QList<QWidget*>{ m_verdict, m_verdictText, m_verdictSub }) {
            w->style()->unpolish(w);
            w->style()->polish(w);
        }
    }
    m_alignBtn->setEnabled(spread >= 0.0f);

    // The way back, and what it will actually do when pressed.
    const bool undoable = undoAvailable();
    float held = 0.0f;
    for (int b = 0; b < kPhysBuses; ++b) held = std::max(held, del[b]);
    m_clearBtn->setText(undoable ? "Undo the alignment"
                                 : "Clear the output delays");
    m_clearBtn->setEnabled(undoable || held > 0.0f);
    m_clearBtn->setToolTip(undoable
        ? "Put every output delay back exactly as it was before the last align"
        : "Set every A-bus delay to zero, so each output goes straight through. "
          "Input delays are left alone - those are for lip-sync, not alignment - "
          "the device latencies are not affected because nothing can change those, "
          "and aligning again is one button away.");
    // When something is being held back for no reason, the way out is the thing
    // to press, not the thing that made it.
    {
        const bool rescue = excess > 1.0f;
        auto cta = [](QPushButton* b, bool on) {
            const QString want = on ? "primary" : QString();
            if (b->property("cta").toString() == want) return;
            b->setProperty("cta", want);
            b->style()->unpolish(b);
            b->style()->polish(b);
        };
        cta(m_clearBtn, rescue);
        cta(m_alignBtn, !rescue);
    }

    // --- has a Bluetooth device moved since it was aligned? ----------------
    // Only worth raising while the alignment is actually out. The offer used to
    // appear whenever a figure had moved, which meant it could sit under a
    // verdict reading "arrives together, within 0.0 ms" and quote a different
    // number as the error - two lines disagreeing about the same thing. The
    // verdict owns the error; this says why it appeared.
    const int drift = spread > 1.0f ? driftedBus() : -1;
    m_driftOffer->setVisible(drift >= 0);
    if (drift >= 0) {
        QSettings cfg("betterbanana", "gui");
        const float was = cfg.value(QString("align/lat%1").arg(drift), -1.0).toFloat();
        m_driftText->setText(
            QString("%1 now reports %2, and it was %3 when you last aligned. A "
                    "Bluetooth link renegotiates its codec on every reconnect, so the "
                    "figure everything was lined up against has moved underneath you.")
                .arg(labelFor(m_shm, false, drift, kBusLabel[drift]),
                     msText(lat[drift]), msText(was)));
    }

    // --- what it costs ----------------------------------------------------
    if (target >= 0.0f) {
        const double sec = target / 1000.0;
        m_costText->setText(
            QString("Ticked outputs land %1 after an application plays a sound, and "
                    "that is as good as it gets while the slowest device is in the "
                    "set.\n\n"
                    "Video does not know about it. A player asks PipeWire what its "
                    "sink costs and is told about the mixer's own virtual sink, not "
                    "about the Bluetooth link past it, so picture runs about %1 "
                    "ahead of sound. Give the player that figure back:\n"
                    "    mpv    --audio-delay=%2        (negative delays the video)\n"
                    "    VLC    Tools → Track synchronisation → %3 ms\n"
                    "Start from that number and trim it by eye; it is close, not exact, "
                    "because the player's own buffering is its business.")
                .arg(msText(target))
                .arg(-sec, 0, 'f', 3)
                .arg(-target, 0, 'f', 0));
    } else {
        m_costText->setText("Nothing ticked has a reported latency yet, so there is no "
                            "figure to give a video player.");
    }

    // --- the tick ---------------------------------------------------------
    int muted = 0, ticked = 0;
    for (int b = 0; b < kPhysBuses; ++b) {
        if (!inc[b] || m_out[b].node.isEmpty()) continue;
        ++ticked;
        if (m_shm->bus[b].mute.load()) ++muted;
    }
    QString hint = m_clicking
        ? QString("Ticking on %1 output%2. One tick means they agree; a flam means they "
                  "do not - turn the delay on the early one until it closes.")
              .arg(ticked).arg(ticked == 1 ? "" : "s")
        : QString("Plays a tick every 0.7 s on the %1 ticked output%2, from one clock. "
                  "One tick = aligned. Two = not.")
              .arg(ticked).arg(ticked == 1 ? "" : "s");
    if (ticked < 2 && !m_clicking)
        hint = "Tick at least two outputs to have something to compare - the point of "
               "the test is one tick against another.";
    if (muted)
        hint += QString("  (%1 of them %2 muted; the tick goes in after the fader, so "
                        "you will still hear it.)")
                    .arg(muted).arg(muted == 1 ? "is" : "are");
    m_clickHint->setText(hint);
    m_clickBtn->setEnabled(ticked > 0);
    m_clickBtn->setText(m_clicking ? "Stop the tick" : "Play a test tick");

    // The engine stops the tick by itself if this window goes away; keep the
    // button honest when it does.
    if (m_clicking && m_shm->click_mask.load() == 0) m_clickBtn->setChecked(false);
}

// Which output's reported latency has moved since the last align, if any. Only
// worth saying for a device that is still the same device: swapping headphones
// for speakers changes the figure too, and that is not drift, it is a new setup.
int AlignDialog::driftedBus() const
{
    QSettings cfg("betterbanana", "gui");
    for (int b = 0; b < kPhysBuses; ++b) {
        if (!m_out[b].inc->isChecked()) continue;
        const QString node = cfg.value(QString("align/node%1").arg(b)).toString();
        if (node.isEmpty() || node != m_out[b].node) continue;
        const float was = cfg.value(QString("align/lat%1").arg(b), -1.0).toFloat();
        const float now = m_shm->out_latency_ms[b].load();
        if (was < 0.0f || now < 0.0f) continue;
        // A couple of milliseconds is quantum jitter, not a renegotiation.
        if (std::fabs(now - was) > 3.0f) return b;
    }
    return -1;
}

// Only while the delays are untouched since the align wrote them. The moment
// somebody nudges one by hand, "undo the alignment" would be a lie about what
// the button is going to do, and it goes back to offering zero.
bool AlignDialog::undoAvailable() const
{
    if (!m_haveUndo) return false;
    float now[kPhysBuses];
    for (int b = 0; b < kPhysBuses; ++b) now[b] = m_shm->bus[b].delay_ms.load();
    return align_undo_available(m_beforeAlign, m_afterAlign, now, kPhysBuses);
}

void AlignDialog::undoOrClear()
{
    QStringList said;
    if (undoAvailable()) {
        for (int b = 0; b < kPhysBuses; ++b) {
            if (std::fabs(m_afterAlign[b] - m_beforeAlign[b]) <= 0.05f) continue;
            m_shm->bus[b].delay_ms.store(m_beforeAlign[b]);
            said << QString("%1 back to %2")
                        .arg(labelFor(m_shm, false, b, kBusLabel[b]), msText(m_beforeAlign[b]));
        }
        m_haveUndo = false;
        m_status->say("Alignment undone - " + said.join(", "), 8000);
    } else {
        for (int b = 0; b < kPhysBuses; ++b) {
            if (m_shm->bus[b].delay_ms.load() <= 0.0f) continue;
            m_shm->bus[b].delay_ms.store(0.0f);
            said << labelFor(m_shm, false, b, kBusLabel[b]);
        }
        m_haveUndo = false;
        m_status->say(said.isEmpty() ? "No output was being held back"
                                     : "Cleared " + said.join(", ")
                                       + " - every output is straight through again",
                      8000);
    }
    // The remembered figures described an alignment that no longer exists, so
    // the drift watch must not go on comparing against them.
    forgetAlignment();
    refresh();
}

void AlignDialog::forgetAlignment()
{
    QSettings cfg("betterbanana", "gui");
    for (int b = 0; b < kPhysBuses; ++b) {
        cfg.remove(QString("align/node%1").arg(b));
        cfg.remove(QString("align/lat%1").arg(b));
    }
}

void AlignDialog::rememberAlignment()
{
    QSettings cfg("betterbanana", "gui");
    for (int b = 0; b < kPhysBuses; ++b) {
        cfg.setValue(QString("align/node%1").arg(b), m_out[b].node);
        cfg.setValue(QString("align/lat%1").arg(b),
                     (double)m_shm->out_latency_ms[b].load());
    }
}

void AlignDialog::setClickRunning(bool on)
{
    int mask = 0;
    if (on)
        for (int b = 0; b < kPhysBuses; ++b)
            if (m_out[b].inc->isChecked() && !m_out[b].node.isEmpty()) mask |= 1 << b;

    m_clicking = on && mask != 0;
    m_shm->click_mask.store(mask);
    if (on && !mask) {
        QSignalBlocker block(m_clickBtn);
        m_clickBtn->setChecked(false);
    }
    // Match the engine's dead-man, so the button does not sit there claiming to
    // be ticking after the engine has given up on it.
    if (m_clicking) m_clickStop->start(kClickMaxSec * 1000);
    else            m_clickStop->stop();
    refresh();
}

void AlignDialog::alignOutputs()
{
    float lat[kPhysBuses], want[kPhysBuses];
    bool  inc[kPhysBuses];
    for (int b = 0; b < kPhysBuses; ++b) {
        lat[b]  = m_shm->out_latency_ms[b].load();
        want[b] = m_shm->bus[b].delay_ms.load();
        inc[b]  = m_out[b].inc->isChecked();
    }
    if (!align_delays(lat, want, kPhysBuses, inc)) {
        QMessageBox::information(this, "BetterBanana",
            "There is nothing to align against yet: PipeWire has not reported a "
            "latency for any output that is ticked.\n\nAssign a device to an A bus "
            "and try again.");
        return;
    }
    QStringList said, left;
    // Everything this is about to overwrite, so it can be put back exactly
    // rather than only flattened to zero.
    for (int b = 0; b < kPhysBuses; ++b)
        m_beforeAlign[b] = m_shm->bus[b].delay_ms.load();
    for (int b = 0; b < kPhysBuses; ++b) {
        const QString name = labelFor(m_shm, false, b, kBusLabel[b]);
        if (!inc[b])          { left << name + " (not ticked)"; continue; }
        if (lat[b] < 0.0f)    { left << name + " (no latency reported)"; continue; }
        m_shm->bus[b].delay_ms.store(want[b]);
        said << QString("%1  held back %2 ms, so it arrives at %3 ms")
                    .arg(name).arg(want[b], 0, 'f', 1).arg(lat[b] + want[b], 0, 'f', 1);
    }
    for (int b = 0; b < kPhysBuses; ++b)
        m_afterAlign[b] = m_shm->bus[b].delay_ms.load();
    m_haveUndo = true;
    rememberAlignment();
    refresh();
    QMessageBox::information(this, "BetterBanana",
        "Outputs aligned:\n\n  " + said.join("\n  ") +
        (left.isEmpty() ? QString()
                        : "\n\nLeft alone:\n  " + left.join("\n  ")) +
        "\n\nEverything ticked now arrives together with the slowest of them. Play the "
        "test tick to hear it, and save a preset if you want it to survive the engine "
        "restarting.");
}

void MainWindow::openAlignDialog()
{
    if (!m_align) m_align = new AlignDialog(m_shm, this);
    m_align->show();
    m_align->raise();
    m_align->activateWindow();
}

// ---------------------------------------------------------------------------
// Diagnostics.
//
// The whole point is that none of these look like a fault. The mixer keeps
// running, the meters keep moving, the unit reads active - and the audio is
// simply somewhere else. Each one below has actually happened.
// ---------------------------------------------------------------------------
struct Finding {
    int     level = 0;                  // 0 problem, 1 warning, 2 note
    QString title;
    QString detail;
    QString fixLabel;                   // empty: nothing to press
    std::function<void()> fix;
};

static QVector<Finding> diagnose(Shared* shm, MainWindow* owner)
{
    QVector<Finding> f;
    auto sname = [shm](int i) { return labelFor(shm, true,  i, kStripTitle[i]); };
    auto bname = [shm](int b) { return labelFor(shm, false, b, kBusLabel[b]); };

    char hw[kHwStrips][kNameLen] = {}, bo[kPhysBuses][kNameLen] = {};
    uint32_t seq = 0;
    bool routed = false;
    for (int t = 0; t < 16 && !routed; ++t) routed = routing_read(shm->routing, seq, hw, bo);

    // Everything PipeWire is currently offering, by node name.
    QSet<QString> present;
    for (const QString& ln : (pactlRun({ "list", "short", "sinks" }) + "\n" +
                              pactlRun({ "list", "short", "sources" }))
                                 .split('\n', Qt::SkipEmptyParts)) {
        const QStringList c = ln.split('\t');
        if (c.size() >= 2) present.insert(c.at(1));
    }

    // --- the engine's own endpoints ---------------------------------------
    // The failure that started the watchdog: same pid, heartbeat still ticking,
    // unit still active, and every node gone from the graph.
    QStringList gone;
    for (const char* n : { "bb_vaio", "bb_aux", "bb_cable1", "bb_b1", "bb_b2" })
        if (!present.contains(QString(n))) gone << n;
    if (!gone.isEmpty())
        f.append({ 0, "The engine's virtual devices are missing from the graph",
                   QString("%1 %2 not in PipeWire's device list. The engine can be "
                           "running - same pid, heartbeat ticking, unit active - with "
                           "every one of its nodes gone, which is what a PipeWire "
                           "restart does to it. No audio moves at all in that state.")
                       .arg(gone.join(", "), gone.size() == 1 ? "is" : "are"),
                   "Restart the engine", [owner] { owner->restartEngine(); } });

    // --- audio routed somewhere that does not exist ------------------------
    for (int i = 0; i < kStrips && routed; ++i) {
        if (shm->strip[i].mute.load()) continue;
        if (i < kHwStrips && !hw[i][0]) continue;          // nothing feeding it
        for (int b = 0; b < kPhysBuses; ++b) {
            if (!shm->strip[i].bus_on[b].load() || bo[b][0]) continue;
            f.append({ 0, QString("%1 feeds %2, which has no output device")
                              .arg(sname(i), bname(b)),
                       QString("An A bus with no device assigned publishes no node, so "
                               "everything routed into it is discarded without a word. "
                               "Either give %1 an output device, or turn its button off "
                               "on %2.").arg(bname(b), sname(i)), {}, {} });
        }
    }

    // --- a device named but not connected ----------------------------------
    for (int i = 0; i < kHwStrips && routed; ++i) {
        const QString dev = QString::fromUtf8(hw[i]);
        if (dev.isEmpty() || dev.startsWith(kCablePrefix)) continue;
        if (shm->strip[i].present.load()) continue;
        f.append({ 0, QString("%1 is set to a device that is not connected").arg(sname(i)),
                   QString("The strip is assigned to \"%1\", which PipeWire is not "
                           "offering. It stays silent until that device comes back.")
                       .arg(dev), {}, {} });
    }
    for (int b = 0; b < kPhysBuses && routed; ++b) {
        const QString dev = QString::fromUtf8(bo[b]);
        if (dev.isEmpty() || present.contains(dev)) continue;
        f.append({ 0, QString("%1 is set to a device that is not connected").arg(bname(b)),
                   QString("The bus is assigned to \"%1\", which PipeWire is not "
                           "offering, so everything routed into it goes nowhere.")
                       .arg(dev), {}, {} });
    }

    // --- a delay that nothing justifies any more ---------------------------
    // The one that took a whole evening to find. The mixer was aligned against
    // Bluetooth earbuds at 303 ms; the earbuds were unplugged, everything moved
    // to the wired interface, and the 282 ms of padding stayed behind on the
    // bus. Every sound was a quarter of a second late, and nothing anywhere
    // said so: the meters moved, the routing was right, the engine was healthy.
    if (routed) {
        float lat[kPhysBuses], del[kPhysBuses], want[kPhysBuses];
        bool  inc[kPhysBuses];
        for (int b = 0; b < kPhysBuses; ++b) {
            lat[b] = shm->out_latency_ms[b].load();
            del[b] = shm->bus[b].delay_ms.load();
            inc[b] = bo[b][0] && std::strcmp(bo[b], kStreamSinkName) != 0;
        }
        const float ex = excess_delay_ms(lat, del, kPhysBuses, inc, want);
        int worst = -1;
        for (int b = 0; b < kPhysBuses; ++b) {
            if (!inc[b] || lat[b] < 0.0f) continue;
            if (worst < 0 || del[b] - want[b] > del[worst] - want[worst]) worst = b;
        }
        // Below the slap threshold it is as likely to be deliberate as stale.
        if (ex >= 50.0f && worst >= 0)
            f.append({ 0, QString("%1 is held back %2 ms more than anything needs")
                              .arg(bname(worst)).arg(ex, 0, 'f', 0),
                       QString("Time alignment holds an output back so it meets the "
                               "slowest device you are listening on. Unplug that device "
                               "- swapping Bluetooth earbuds for wired headphones does "
                               "it - and the padding stays behind with nothing left to "
                               "justify it. Everything through %1 arrives %2 ms late, "
                               "and no meter or setting shows it.")
                           .arg(bname(worst)).arg(ex, 0, 'f', 0),
                       "Clear the output delays",
                       [shm] {
                           for (int b = 0; b < kPhysBuses; ++b)
                               shm->bus[b].delay_ms.store(0.0f);
                       } });
    }

    // --- the echo rule ------------------------------------------------------
    // Whichever bus points at the capture-only stream sink is what a screen
    // share transmits. AUX carries what other people are saying.
    int streamBus = -1;
    for (int b = 0; b < kPhysBuses && routed; ++b)
        if (QString::fromUtf8(bo[b]) == QString(kStreamSinkName)) streamBus = b;
    if (streamBus >= 0 && shm->strip[kStrips - 1].bus_on[streamBus].load())
        f.append({ 0, QString("%1 is routed to the stream bus, so callers hear themselves")
                          .arg(sname(kStrips - 1)),
                   QString("%1 is where incoming voice arrives. Sending it to %2, which "
                           "is what a screen share transmits, mixes everyone's own voice "
                           "back into the stream as an echo - and the people hearing it "
                           "are the only ones who can tell.")
                       .arg(sname(kStrips - 1), bname(streamBus)),
                   "Turn that route off",
                   [shm, streamBus] { shm->strip[kStrips - 1].bus_on[streamBus].store(0); } });

    // --- the ducker ---------------------------------------------------------
    if (shm->duck_enabled.load()) {
        int key = -1;
        for (int i = 0; i < kStrips; ++i)
            if (shm->strip[i].duck_key.load()) { key = i; break; }
        bool depth = false;
        for (int i = 0; i < kStrips; ++i)
            depth = depth || shm->strip[i].duck_depth_db.load() < -0.05f;
        const float thr = shm->duck_threshold_db.load();

        if (key < 0)
            f.append({ 1, "Ducking is on, but no strip is set as the key",
                       "Nothing triggers it, so it never does anything. Mark the strip "
                       "that should cause the ducking - usually the microphone.", {}, {} });
        else if (!depth)
            f.append({ 1, "Ducking is on, but no strip has a depth",
                       "Something triggers it and nothing responds. Give the strips that "
                       "should get out of the way a depth in dB.", {}, {} });

        if (key >= 0 && thr <= -60.0f)
            f.append({ 1, "The ducking threshold is at the bottom of its range",
                       QString("At %1 dB almost anything on %2 - a fan, a keyboard, your "
                               "own speakers bleeding into the microphone - holds the "
                               "ducker permanently open, so the strips it controls never "
                               "come back up. A speaking voice usually sits near -20 dB.")
                           .arg(thr, 0, 'f', 1).arg(sname(key)), {}, {} });

        // What it is doing at this moment, which is the part no setting shows.
        if (key >= 0 && shm->meters.duck_env.load() > 0.7f) {
            const float lin = qMax(shm->meters.strip_pre[key][0].load(),
                                   shm->meters.strip_pre[key][1].load());
            const float db = lin > 1e-6f ? 20.0f * std::log10(lin) : -99.9f;
            f.append({ 1, "The ducker is holding strips down right now",
                       QString("%1 is at %2 dB against a %3 dB threshold. If nobody is "
                               "talking, the threshold is below the room and the ducked "
                               "strips will never come back up.")
                           .arg(sname(key)).arg(db, 0, 'f', 1).arg(thr, 0, 'f', 1),
                       {}, {} });
        }
    }

    // --- WirePlumber overruling an application ------------------------------
    for (const StreamInfo& si : listStreams(true)) {
        if (si.wanted.isEmpty() || si.target.isEmpty() || si.wanted == si.target) continue;
        bool numeric = false;
        si.wanted.toLongLong(&numeric);
        if (numeric) continue;                       // a serial, not a name
        if (!present.contains(si.wanted)) continue;  // it asked for something gone
        f.append({ 0, QString("%1 is not playing where it asked to").arg(si.app),
                   QString("It requested \"%1\" and PipeWire put it on \"%2\". That is "
                           "WirePlumber replaying a saved choice from "
                           "~/.local/state/wireplumber, and it wins over the setting "
                           "inside the application - changing it there will not stick. "
                           "Moving the stream now is what makes WirePlumber save the new "
                           "answer instead.").arg(si.wanted, si.target),
                   QString("Move it to %1").arg(si.wanted),
                   [si] { pactlRun({ "move-sink-input", QString::number(si.index),
                                     si.wanted }); } });
    }

    // --- headroom -----------------------------------------------------------
    const double load = shm->dsp_load.load() / 10.0;
    if (load > 70.0)
        f.append({ 1, QString("The mixer is using %1% of its realtime budget")
                          .arg(load, 0, 'f', 1),
                   "Past about 80% the audio starts breaking up. Formant shifting is the "
                   "most expensive thing here; turning it off on a strip that is not "
                   "using it buys back the most.", {}, {} });

    // --- things left switched on --------------------------------------------
    for (int i = 0; i < kStrips; ++i)
        if (shm->strip[i].solo.load())
            f.append({ 2, QString("%1 is soloed").arg(sname(i)),
                       "Every other input is silenced on the buses this strip feeds. It "
                       "is easy to leave up, and it sounds exactly like the other strips "
                       "having stopped working.",
                       "Clear it", [shm, i] { shm->strip[i].solo.store(0); } });

    for (int b = 0; b < kPhysBuses && routed; ++b)
        if (bo[b][0] && shm->bus[b].mute.load())
            f.append({ 2, QString("%1 is muted").arg(bname(b)),
                       QString("It has \"%1\" assigned, so something is routed there and "
                               "nothing is coming out.").arg(QString::fromUtf8(bo[b])),
                       "Unmute it", [shm, b] { shm->bus[b].mute.store(0); } });

    // --- the system defaults a PipeWire restart throws away ------------------
    const QString defSink = pactlRun({ "get-default-sink" }).trimmed();
    const QString defSrc  = pactlRun({ "get-default-source" }).trimmed();
    if (!defSink.isEmpty() && !defSink.startsWith("bb_") && present.contains("bb_vaio"))
        f.append({ 1, "The system's default output is not a BetterBanana device",
                   QString("New applications will open on \"%1\" and never appear on a "
                           "strip. A PipeWire restart resets this, and it is the usual "
                           "reason everything sounds fine while the mixer shows nothing.")
                       .arg(defSink),
                   "Point it at VAIO", [] { pactlRun({ "set-default-sink", "bb_vaio" }); } });
    if (!defSrc.isEmpty() && !defSrc.startsWith("bb_") && present.contains("bb_b1"))
        f.append({ 1, "The system's default input is not a BetterBanana device",
                   QString("Applications that record will open on \"%1\" rather than on "
                           "what the mixer sends out of B1.").arg(defSrc),
                   "Point it at B1", [] { pactlRun({ "set-default-source", "bb_b1" }); } });

    std::stable_sort(f.begin(), f.end(),
                     [](const Finding& a, const Finding& b) { return a.level < b.level; });
    return f;
}

DiagnoseDialog::DiagnoseDialog(Shared* shm, MainWindow* owner, QWidget* parent)
    : QDialog(parent), m_shm(shm), m_owner(owner)
{
    setWindowTitle("Check this setup");
    auto* root = new QVBoxLayout(this);
    bbdlg::chrome(root);
    root->addWidget(bbdlg::header("Check this setup",
        "Things that are wrong without looking wrong"));

    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    auto* area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    auto* holder = new QWidget;
    m_list = new QVBoxLayout(holder);
    m_list->setContentsMargins(0, 0, 0, 0);
    m_list->setSpacing(bbui::gapS());
    m_list->addStretch(1);
    area->setWidget(holder);
    root->addWidget(area, 1);

    auto* again = new QPushButton("Check again");
    connect(again, &QPushButton::clicked, this, &DiagnoseDialog::recheck);
    auto* close = new QPushButton("Close");
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    root->addLayout(bbdlg::buttonRow(close, again));
    bbdlg::rememberGeometry(this, "diagnose");
    bbdlg::tameDefaults(this);
    resize(qMax(width(), bbui::px(660)), qMax(height(), bbui::px(480)));
    recheck();
}

void DiagnoseDialog::recheck()
{
    while (m_list->count() > 1) {                    // the trailing stretch stays
        QLayoutItem* it = m_list->takeAt(0);
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }

    const QVector<Finding> found = diagnose(m_shm, m_owner);
    int problems = 0, warnings = 0;
    for (const Finding& f : found) { if (f.level == 0) ++problems; else if (f.level == 1) ++warnings; }

    if (found.isEmpty())
        m_summary->setText("Nothing to report. Every check below the surface passed: the "
                           "engine's devices are in the graph, everything routed has "
                           "somewhere to go, and no application has been overruled.");
    else
        m_summary->setText(QString("%1 to fix, %2 worth a look, %3 worth knowing.")
                               .arg(problems).arg(warnings)
                               .arg(found.size() - problems - warnings));

    static const char* kChipRole[] = { "mute", "solo", "accent" };
    static const char* kChipText[] = { "FIX",  "LOOK", "NOTE" };
    for (const Finding& f : found) {
        // A plain QWidget, because that is what QWidget[role="card"] in the
        // theme is written against and what every other card here is.
        auto* card = new QWidget;
        card->setProperty("role", "card");
        auto* lay = new QGridLayout(card);
        lay->setContentsMargins(bbui::gapM(), bbui::gapS(), bbui::gapM(), bbui::gapS());
        lay->setHorizontalSpacing(bbui::gapM());

        auto* chip = new QLabel(kChipText[f.level]);
        chip->setProperty("role", "diagchip");
        chip->setProperty("sev", kChipRole[f.level]);
        chip->setAlignment(Qt::AlignCenter);
        lay->addWidget(chip, 0, 0, Qt::AlignTop);

        auto* title = new QLabel(f.title);
        title->setWordWrap(true);
        title->setProperty("role", "value");
        lay->addWidget(title, 0, 1);

        auto* detail = new QLabel(f.detail);
        detail->setWordWrap(true);
        detail->setProperty("role", "caption");
        lay->addWidget(detail, 1, 1);

        if (!f.fixLabel.isEmpty()) {
            auto* go = new QPushButton(f.fixLabel);
            go->setProperty("cta", "primary");
            const auto act = f.fix;
            connect(go, &QPushButton::clicked, this, [this, act] {
                if (act) act();
                // Re-run rather than tick the row off: a fix can resolve more
                // than one finding, or expose the next one.
                QTimer::singleShot(400, this, &DiagnoseDialog::recheck);
            });
            lay->addWidget(go, 0, 2, 2, 1, Qt::AlignVCenter);
        }
        lay->setColumnStretch(1, 1);
        m_list->insertWidget(m_list->count() - 1, card);
    }
}

void MainWindow::openDiagnoseDialog()
{
    if (!m_diag) m_diag = new DiagnoseDialog(m_shm, this, this);
    else         m_diag->recheck();
    m_diag->show();
    m_diag->raise();
    m_diag->activateWindow();
}

// ---------------------------------------------------------------------------
// Start-at-login. The engine is a systemd user service; the mixer window is a
// plain XDG autostart entry. They are independent on purpose: most people want
// the engine always up (otherwise their virtual devices vanish) but not
// necessarily the window in their face at every login.
// ---------------------------------------------------------------------------
static const char* kUnit = "betterbanana-engine.service";

// `msec` is generous for `systemctl restart`, which does not return until the
// unit has actually come back up.
static QString runProc(const QString& prog, const QStringList& args,
                       int* code = nullptr, int msec = 4000)
{
    QProcess p;
    p.start(prog, args);
    if (!p.waitForFinished(msec)) { if (code) *code = -1; return QString(); }
    if (code) *code = p.exitCode();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// The unit is only present once the app has been installed.
static bool engineUnitInstalled()
{
    const QString s = runProc("systemctl", { "--user", "is-enabled", kUnit });
    return !s.isEmpty() && s != "not-found";
}

static bool engineAutostart()
{
    return runProc("systemctl", { "--user", "is-enabled", kUnit }) == "enabled";
}

static void setEngineAutostart(bool on)
{
    runProc("systemctl", { "--user", on ? "enable" : "disable", kUnit });
}

// Started by systemd, as opposed to merely having the unit installed. An engine
// launched by hand while the unit is installed must not be restarted through
// systemd: that would start a second one alongside the first, and the engine
// refuses to be the second.
static bool engineUnitActive()
{
    return runProc("systemctl", { "--user", "is-active", kUnit }) == "active";
}

// Where bb-engine is, for the case where there is no unit to start.
static QString engineBinary()
{
    // Beside this binary first. A build tree and an install prefix both put the
    // two side by side, and the graphical session's PATH may well not carry
    // ~/.local/bin.
    const QString sib = QCoreApplication::applicationDirPath() + "/bb-engine";
    if (QFileInfo(sib).isExecutable()) return sib;
    const QString found = QStandardPaths::findExecutable("bb-engine");
    if (!found.isEmpty()) return found;
    const QString local = QDir::homePath() + "/.local/bin/bb-engine";
    return QFileInfo(local).isExecutable() ? local : QString();
}

static bool pidAlive(pid_t p) { return p > 0 && ::kill(p, 0) == 0; }

static QString guiAutostartPath()
{
    return QDir::homePath() + "/.config/autostart/betterbanana.desktop";
}

static bool guiAutostart() { return QFile::exists(guiAutostartPath()); }

static void setGuiAutostart(bool on)
{
    const QString path = guiAutostartPath();
    if (!on) { QFile::remove(path); return; }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    // Exec is resolved through PATH so this keeps working if the binary moves
    // between ~/.local/bin and /usr/bin.
    f.write("[Desktop Entry]\n"
            "Type=Application\n"
            "Name=BetterBanana\n"
            "Comment=Virtual audio mixer\n"
            "Exec=bb-gui\n"
            "Icon=betterbanana\n"
            "Terminal=false\n"
            "X-GNOME-Autostart-enabled=true\n");
}

// ---------------------------------------------------------------------------
// MainWindow
// ---------------------------------------------------------------------------
MainWindow::MainWindow(Shared* shm, QWidget* parent)
    : QMainWindow(parent), m_shm(shm)
{
    // Set from the loaded preset, see refreshTitle(). "(Linux)" was a port note.
    refreshTitle();

    auto* central = new QWidget;
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(bbui::gapM(), bbui::gapM(), bbui::gapM(), bbui::gapM());
    outer->setSpacing(bbui::gapM());

    // A banner across the top for the one condition where every control in the
    // window is writing into shared memory nobody is reading.
    m_alert = new QLabel;
    m_alert->setProperty("role", "alert");
    m_alert->setWordWrap(true);
    m_alert->setVisible(false);
    outer->addWidget(m_alert);

    // And a second one for the condition that is not a fault but a loss: the
    // engine restarted underneath the window and took the mix with it. This one
    // carries its own answer, and is deliberately not a modal dialog - it can
    // appear while a call or a game is in the foreground.
    m_offer = new QFrame;
    m_offer->setProperty("role", "offer");
    m_offer->setVisible(false);
    {
        auto* lay = new QHBoxLayout(m_offer);
        lay->setContentsMargins(bbui::px(10), bbui::px(6), bbui::px(6), bbui::px(6));
        lay->setSpacing(bbui::gapM());
        m_offerText = new QLabel;
        m_offerText->setWordWrap(true);
        lay->addWidget(m_offerText, 1);
        auto* put = new QPushButton("Put my mix back");
        put->setProperty("cta", "primary");
        connect(put, &QPushButton::clicked, this, [this] {
            const QByteArray keep = m_recovered;
            m_recovered.clear();
            m_offer->setVisible(false);
            if (keep.isEmpty() || !shm_compatible(m_shm)) return;
            applyState(keep);
            say("Your mix is back", 6000);
        });
        lay->addWidget(put);
        auto* keep = new QPushButton("Keep this one");
        connect(keep, &QPushButton::clicked, this, [this] {
            m_recovered.clear();
            m_offer->setVisible(false);
        });
        lay->addWidget(keep);
    }
    outer->addWidget(m_offer);

    // The preset bar. Loading a preset used to mean Ctrl+O and a file dialog,
    // which is a lot of ceremony for something switched several times an
    // evening - "screen share" is a different mix from "listening to music".
    m_presetBar = new QWidget;
    m_presetLay = new QHBoxLayout(m_presetBar);
    m_presetLay->setContentsMargins(0, 0, 0, 0);
    m_presetLay->setSpacing(bbui::gapS());
    outer->addWidget(m_presetBar);

    // Presets saved from bb-ctl in another terminal show up here too.
    QDir().mkpath(QString::fromStdString(presets_path()));
    m_presetWatch = new QFileSystemWatcher(this);
    m_presetWatch->addPath(QString::fromStdString(presets_path()));
    connect(m_presetWatch, &QFileSystemWatcher::directoryChanged, this,
            [this] { rebuildPresetBar(); });

    // Ctrl+1..9 follow the bar's own order, so what the eye reads left to right
    // is what the fingers count.
    for (int i = 0; i < 9; ++i) {
        auto* sc = new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i + 1)), this);
        connect(sc, &QShortcut::activated, this, [this, i] {
            if (i >= m_presetOrder.size()) return;
            const QString name = m_presetOrder[i];
            if (loadPresetFile(QString::fromStdString(preset_path_for(name.toStdString()))))
                say("Loaded \"" + name + "\"");
        });
    }

    auto* row = new QHBoxLayout;
    row->setSpacing(bbui::gapM());

    auto* inBox = new QGroupBox("INPUTS");
    auto* inRow = new QHBoxLayout(inBox);
    inRow->setSpacing(bbui::gapXS() + 1);
    for (int i = 0; i < kStrips; ++i) {
        auto* s = new StripWidget(m_shm, i, i < kHwStrips, kStripTitle[i]);
        connect(s, &StripWidget::routingChanged, this,
                [this](int idx, const QString& n) {
                    m_hwIn[idx] = n;
                    writeRouting();
                    applyDeviceStrip(idx, n);
                });
        connect(s, &StripWidget::eqEditRequested, this, &MainWindow::openStripEq);
        connect(s, &StripWidget::fxEditRequested, this, &MainWindow::openStripFx);
        connect(s, &StripWidget::statusMessage, this,
                [this](const QString& t) { say(t, 7000); });
        m_strips.push_back(s);
        inRow->addWidget(s);
        // Two classes of input, separated by a real gutter. A 1px bevel in 2px
        // of space read as a rendering seam, not a boundary.
        if (i == kHwStrips - 1) inRow->addSpacing(bbui::gapM() + 2);
    }
    row->addWidget(inBox, 5);

    auto* outBox = new QGroupBox("BUSES");
    auto* outRow = new QHBoxLayout(outBox);
    outRow->setSpacing(bbui::gapXS() + 1);
    for (int b = 0; b < kBuses; ++b) {
        auto* w = new BusWidget(m_shm, b, b < kPhysBuses, kBusLabel[b]);
        connect(w, &BusWidget::routingChanged, this,
                [this](int idx, const QString& n) {
                    m_busOut[idx] = n;
                    writeRouting();
                    applyDeviceEq(idx, n);
                });
        connect(w, &BusWidget::eqEditRequested, this, &MainWindow::openBusEq);
        connect(w, &BusWidget::statusMessage, this,
                [this](const QString& t) { say(t, 7000); });
        m_buses.push_back(w);
        outRow->addWidget(w);
        if (b == kPhysBuses - 1) outRow->addSpacing(bbui::gapM() + 2);
    }
    row->addWidget(outBox, 5);
    outer->addLayout(row, 0);

    // The height the console does not need goes here, above the tape deck,
    // rather than into the faders. Tier 3 puts a dock in it.
    outer->addStretch(1);

    auto* scroll = new QScrollArea;
    m_scroll = scroll;
    m_central = central;
    scroll->setWidget(central);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    // Only the console scrolls. The transport is a control you reach for while
    // something is playing, so putting it inside the scroll area meant that at
    // the window's own minimum height it was the part that fell below the fold.
    // The engine-stopped banner is pinned for the same reason.
    //
    // The recorder is a card in its own right now that cards paint; a plate
    // inside a group box would be framed twice.
    m_recorder = new RecorderWidget(m_shm);

    auto* frame = new QWidget;
    auto* fl = new QVBoxLayout(frame);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(0);
    fl->addWidget(scroll, 1);
    {   // The same gutter the console has, so the transport still reads as a
        // card rather than a band clipped against the window edge.
        auto* row = new QHBoxLayout;
        row->setContentsMargins(bbui::gapM(), 0, bbui::gapM(), bbui::gapM());
        row->addWidget(m_recorder);
        fl->addLayout(row);
    }
    setCentralWidget(frame);

    buildMenus();

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    QSettings st("betterbanana", "gui");
    applyTheme(st.value("theme", 0).toInt());
    // Derived, not guessed: ten cards at their own minimum widths plus the
    // gutters and margins around them, and the console's own content height at
    // the floor travel. Hard-coding it got the app a horizontal scrollbar at
    // its own stated minimum.
    if (m_central) {
        QSize need = m_central->minimumSizeHint().expandedTo(m_central->sizeHint());
        // The transport sits outside the scroll area now, so its height is not
        // in the measurement above.
        if (m_recorder) need.rheight() += m_recorder->sizeHint().height();
        // Width is a hard floor - ten columns cannot overlap. Height is capped
        // well below what the content wants, because a 1366x768 laptop has
        // about 730 usable pixels and scrolling the console is a far better
        // answer there than refusing to open at a usable size.
        setMinimumSize(qMin(need.width() + bbui::px(4), 1600),
                       qMin(need.height() + bbui::px(24), bbui::px(660)));
    }
    // A screen-clamped restore, so the mixer cannot open wider than the display
    // it is opening on. The hard-coded 1500x720 was nothing like the size this
    // window actually gets dragged to.
    restoreWindowGeometry();

    // Start at the floor so the first tick can measure the window's own chrome.
    m_travel = bbui::travel();
    for (auto* s : m_strips) { s->setTravel(m_travel); s->pullFromShm(); }
    for (auto* b : m_buses)  { b->setTravel(m_travel); b->pullFromShm(); }
    refreshDevices();

    // Whatever the mixer looks like when the window opens is undo's starting
    // point, so the first Ctrl+Z comes back here rather than nowhere.
    m_committed = m_seen = QByteArray::fromStdString(preset_serialize(m_shm));
    refreshUndoActions();

    rebuildPresetBar();
    setPresetBarVisible(QSettings("betterbanana", "gui").value("presetBar", true).toBool());

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);
    m_timer->start(33);
}

void MainWindow::refreshTitle()
{
    QString t = "BetterBanana";
    if (!m_presetName.isEmpty()) t += "  -  " + m_presetName;
    if (m_dirty) t += " *";
    if (windowTitle() != t) setWindowTitle(t);
}

// One place, so the timeouts stop being six different numbers chosen at six
// call sites. Anything that reports a change the user did not make gets longer
// on screen than a plain acknowledgement.
void MainWindow::say(const QString& text, int ms)
{
    statusBar()->showMessage(text, ms);
}

// A swatch painted from the palette itself, in the icon gutter Qt already
// reserves for a menu item. Ten hand-transcribed palettes were being chosen
// from ten lines of plain text.
static QIcon themeSwatch(const Theme& t)
{
    QPixmap pm(bbui::px(44), bbui::px(12));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0.5, 0.5, pm.width() - 1.0, pm.height() - 1.0);
    p.setPen(Qt::NoPen);
    p.setBrush(t.bg);
    p.drawRoundedRect(r, 2, 2);
    const QColor bars[] = { t.panel, t.accent, t.busA, t.busB, t.mute, t.solo };
    const double bw = r.width() / 8.0;
    for (int i = 0; i < 6; ++i) {
        p.setBrush(bars[i]);
        p.drawRect(QRectF(r.left() + bw * (i + 1), r.top() + 2, bw - 1, r.height() - 4));
    }
    p.setPen(QPen(t.border, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 2, 2);
    return QIcon(pm);
}

void MainWindow::restoreWindowGeometry()
{
    const QByteArray g = QSettings("betterbanana", "gui").value("geometry/main").toByteArray();
    if (!g.isEmpty() && restoreGeometry(g)) {
        // restoreGeometry has already placed the window, including its frame.
        // Only intervene when the saved geometry does not fit or does not land
        // on this display - and then move the FRAME, via QWidget::move, rather
        // than feeding a frame origin to setGeometry, which positions the
        // client rect and so walked the window up and left on every launch.
        const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
        const QSize fit = size().boundedTo(avail.size() - (frameGeometry().size() - size()));
        if (fit != size()) resize(fit);
        if (!avail.intersects(frameGeometry())) move(avail.topLeft());
        return;
    }
    const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
    resize(qMin(1500, avail.width() - 40), qMin(860, avail.height() - 60));
}

void MainWindow::saveWindowGeometry()
{
    QSettings("betterbanana", "gui").setValue("geometry/main", saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveWindowGeometry();
    QMainWindow::closeEvent(e);
}

// Everything a card can say about itself that is not a level.
void MainWindow::refreshCardStates()
{
    bool anySolo = false;
    for (int i = 0; i < kStrips; ++i)
        anySolo = anySolo || m_shm->strip[i].solo.load(std::memory_order_relaxed) != 0;

    for (int i = 0; i < kStrips && i < m_strips.size(); ++i) {
        auto* w = m_strips[i];
        w->setLive(m_engineLive);
        // Soloing one strip silences four others, and the only evidence used to
        // be one small yellow button on the strip that caused it.
        w->setDimmed(anySolo && m_shm->strip[i].solo.load(std::memory_order_relaxed) == 0);
        // `present` is maintained by the engine at three call sites and was
        // read by the GUI nowhere: an unplugged interface looked identical to a
        // live one.
        const bool named = w->isHardware() && !m_hwIn[qMin(i, kHwStrips - 1)].isEmpty();
        w->setAttached(!named || m_shm->strip[i].present.load(std::memory_order_relaxed) != 0);
    }
    for (auto* b : m_buses) b->setLive(m_engineLive);

    QStringList busNames;
    for (int b = 0; b < kBuses; ++b) busNames << labelFor(m_shm, false, b, kBusLabel[b]);
    for (auto* w : m_strips) w->refreshBusTips(busNames);
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle("About BetterBanana");
    box.setTextFormat(Qt::RichText);
    box.setText(QString("<b>BetterBanana %1</b><br>A Voicemeeter-Banana-style "
                        "mixer for PipeWire.")
                    .arg(QApplication::applicationVersion()));
    box.setInformativeText(
        QString("Engine protocol v%1  ·  %2 Hz  ·  %3 inputs, %4 buses<br><br>"
                "MIT licensed. <a href=\"https://github.com/Zykoraa/Linux-Apps\">"
                "github.com/Zykoraa/Linux-Apps</a>")
            .arg(kVersion).arg(m_shm->samplerate.load()).arg(kStrips).arg(kBuses));
    box.exec();
}

void MainWindow::openVbanDialog() { VbanDialog(m_shm, this).exec(); }

void MainWindow::openBusEq(int bus)
{
    if (bus < 0 || bus >= kBuses) return;
    EqEditorDialog(m_shm, &m_shm->bus[bus].eq, spec_bus_src(bus),
                   labelFor(m_shm, false, bus, kBusLabel[bus]), bus, this).exec();
    m_buses[bus]->pullFromShm();
}

void MainWindow::openStripEq(int strip)
{
    if (strip < 0 || strip >= kStrips) return;
    // -1: a microphone has no measured headphone correction to look up.
    EqEditorDialog(m_shm, &m_shm->strip[strip].eq, spec_strip_src(strip),
                   labelFor(m_shm, true, strip, kStripTitle[strip]), -1, this).exec();
    m_strips[strip]->pullFromShm();
}

void MainWindow::openStripFx(int strip)
{
    if (strip < 0 || strip >= kStrips) return;
    VoiceFxDialog(m_shm, strip, labelFor(m_shm, true, strip, kStripTitle[strip]), this).exec();
    m_strips[strip]->pullFromShm();
}

void MainWindow::openDuckDialog() { DuckDialog(m_shm, this).exec(); }

void MainWindow::openAppsDialog()
{
    // Non-modal: you keep mixing while it polls for new streams.
    if (!m_apps) m_apps = new AppsDialog(this);
    m_apps->show();
    m_apps->raise();
    m_apps->activateWindow();
}

// --- microphone analyzer ----------------------------------------------------
// mic-gain is a separate terminal tool on purpose: it works over SSH and when
// this GUI will not start, which is exactly when a microphone needs diagnosing.
// The mixer's job is only to make it findable and point it at the right source.

static QString findMicGain()
{
    // The graphical session's PATH usually lacks ~/.local/bin - uwsm rebuilds
    // PATH from a POSIX login shell - so look there first rather than trusting
    // the environment we happen to have been launched with.
    const QStringList known = {
        QDir::homePath() + "/.local/bin/mic-gain",
        "/usr/local/bin/mic-gain",
        "/usr/bin/mic-gain",
    };
    for (const QString& p : known)
        if (QFileInfo(p).isExecutable()) return p;
    return QStandardPaths::findExecutable("mic-gain");
}

// Terminals disagree about how to be handed a command. "-e" is the common
// spelling; the ones that want something else are listed with what they want.
static bool launchInTerminal(const QStringList& cmd, QString* err)
{
    QVector<QPair<QString, QStringList>> terms;
    const QString pref = qEnvironmentVariable("TERMINAL");
    if (!pref.isEmpty()) terms.push_back({ pref, { "-e" } });
    terms.append({
        { "kitty",          { "-e" } },
        { "foot",           { "-e" } },
        { "alacritty",      { "-e" } },
        { "ghostty",        { "-e" } },
        { "wezterm",        { "start", "--" } },
        { "konsole",        { "-e" } },
        { "gnome-terminal", { "--" } },
        { "xfce4-terminal", { "-x" } },
        { "xterm",          { "-e" } },
    });
    for (const auto& t : terms) {
        const QString exe = QStandardPaths::findExecutable(t.first);
        if (exe.isEmpty()) continue;
        if (QProcess::startDetached(exe, t.second + cmd)) return true;
    }
    if (err) *err = "No terminal emulator found (tried kitty, foot, alacritty, "
                    "ghostty, wezterm, konsole, gnome-terminal, xterm).";
    return false;
}

void MainWindow::openMicAnalyzer(const QString& source, const QString& label)
{
    const QString tool = findMicGain();
    if (tool.isEmpty()) {
        QMessageBox::information(this, "BetterBanana",
            "mic-gain is not installed.\n\n"
            "It is a separate tool in the same repository - it measures a "
            "microphone and names the control to change. Install it with:\n\n"
            "    mic-gain/install.sh");
        return;
    }
    const QStringList cmd{ tool, "-s", source };
    QString err;
    if (!launchInTerminal(cmd, &err)) {
        QMessageBox::warning(this, "BetterBanana",
            err + "\n\nRun it yourself with:\n\n    " + cmd.join(" "));
        return;
    }
    say("Analysing " + label + " in a terminal");
}

// Rebuilt each time the menu opens, so it names whatever is assigned now.
void MainWindow::populateAnalyzerMenu(QMenu* menu)
{
    menu->clear();
    for (int i = 0; i < kHwStrips; ++i) {
        const QString dev = m_hwIn[i];
        const QString name = labelFor(m_shm, true, i,
                                      QString("HARDWARE INPUT %1").arg(i + 1));
        // A cable carries application audio, not a microphone, and an
        // unassigned strip has nothing to measure.
        if (dev.isEmpty() || dev.startsWith(kCablePrefix)) {
            QAction* a = menu->addAction(
                name + (dev.isEmpty() ? "   (no device)" : "   (virtual cable)"));
            a->setEnabled(false);
            continue;
        }
        menu->addAction(name, this, [this, dev, name] { openMicAnalyzer(dev, name); });
    }
    menu->addSeparator();
    // The B buses are what recording applications actually receive, which is
    // usually the more useful measurement: it includes gate, comp and EQ.
    for (int b = kPhysBuses; b < kBuses; ++b) {
        const QString node = QString("bb_b%1").arg(b - kPhysBuses + 1);
        const QString name = labelFor(m_shm, false, b, kBusLabel[b]);
        menu->addAction(QString("%1   (%2 - what apps receive)").arg(name, node),
                        this, [this, node, name] { openMicAnalyzer(node, name); });
    }
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu("&Preset");
    file->addAction("&Save preset...", QKeySequence("Ctrl+S"), this,
                    &MainWindow::savePresetAs);
    file->addAction("&Load preset...", QKeySequence("Ctrl+O"), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Load preset",
                              QString::fromStdString(presets_path()), "Presets (*.bbp)");
        if (f.isEmpty()) return;
        if (loadPresetFile(f)) say("Loaded " + f);
    });
    file->addSeparator();
    auto* startup = file->addMenu("Load on &startup");
    startup->setToolTip("Which saved preset the audio engine restores when it starts");
    connect(startup, &QMenu::aboutToShow, this,
            [this, startup] { populateStartupMenu(startup); });
    populateStartupMenu(startup);

    auto* edit = menuBar()->addMenu("&Edit");
    m_undoAct = edit->addAction("&Undo", QKeySequence::Undo, this, &MainWindow::undo);
    m_redoAct = edit->addAction("&Redo", QKeySequence::Redo, this, &MainWindow::redo);
    m_undoAct->setToolTip("Step back through changes to the mixer, however they were made");
    refreshUndoActions();

    auto* eng = menuBar()->addMenu("&Engine");
    eng->addAction("&Applications...", QKeySequence("Ctrl+A"), this, &MainWindow::openAppsDialog);
    eng->addSeparator();
    eng->addAction("&Refresh device list", QKeySequence("F5"), this, &MainWindow::refreshDevices);
    eng->addAction("Clear c&lip indicators", this, [this] {
        m_shm->cmd.store(kCmdClearClip);
        m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
    });
    eng->addAction("Reset &meters", this, [this] {
        m_shm->cmd.store(kCmdResetMeters);
        m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
    });
    eng->addSeparator();
    auto* inEq = eng->addMenu("&Input EQ");
    connect(inEq, &QMenu::aboutToShow, this, [this, inEq] {
        inEq->clear();
        for (int i = 0; i < kStrips; ++i)
            inEq->addAction(labelFor(m_shm, true, i, kStripTitle[i]) + "...",
                            this, [this, i] { openStripEq(i); });
    });
    for (int i = 0; i < kStrips; ++i)
        inEq->addAction(labelFor(m_shm, true, i, kStripTitle[i]) + "...",
                        this, [this, i] { openStripEq(i); });

    auto* fxMenu = eng->addMenu("&Voice changer");
    connect(fxMenu, &QMenu::aboutToShow, this, [this, fxMenu] {
        fxMenu->clear();
        for (int i = 0; i < kStrips; ++i)
            fxMenu->addAction(labelFor(m_shm, true, i, kStripTitle[i]) + "...",
                              this, [this, i] { openStripFx(i); });
    });
    for (int i = 0; i < kStrips; ++i)
        fxMenu->addAction(labelFor(m_shm, true, i, kStripTitle[i]) + "...",
                          this, [this, i] { openStripFx(i); });

    auto* eqMenu = eng->addMenu("&Bus EQ");
    connect(eqMenu, &QMenu::aboutToShow, this, [this, eqMenu] {
        eqMenu->clear();
        for (int b = 0; b < kBuses; ++b)
            eqMenu->addAction(labelFor(m_shm, false, b, kBusLabel[b]) + "...",
                              this, [this, b] { openBusEq(b); });
    });
    for (int b = 0; b < kBuses; ++b)
        eqMenu->addAction(labelFor(m_shm, false, b, kBusLabel[b]) + "...",
                          this, [this, b] { openBusEq(b); });
    eng->addAction("&Time alignment...", QKeySequence("Ctrl+T"), this, &MainWindow::openAlignDialog)
       ->setToolTip("Line up outputs that do not arrive at the same moment");
    eng->addAction("Reset &loudness measurement", this, [this] {
        m_shm->cmd.store(kCmdResetLoudness);
        m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
        say("Integrated loudness measurement restarted");
    });
    eng->addAction("Sidechain &ducking...", QKeySequence("Ctrl+D"), this, &MainWindow::openDuckDialog);
    eng->addAction("&VBAN streams...", QKeySequence("Ctrl+B"), this, &MainWindow::openVbanDialog);

    auto* mic = eng->addMenu("Analy&se microphone");
    mic->setToolTip("Measure a microphone and be told which control to change");
    connect(mic, &QMenu::aboutToShow, this, [this, mic] { populateAnalyzerMenu(mic); });
    populateAnalyzerMenu(mic);
    eng->addSeparator();

    eng->addAction("&Check this setup...", this, &MainWindow::openDiagnoseDialog)
       ->setToolTip("Look for the mistakes that do not look like mistakes");
    m_restartAct = eng->addAction("&Restart audio engine...", this,
                                  &MainWindow::restartEngine);
    m_restartAct->setToolTip("Stop and start the mixing process. The first thing to "
                             "try when the audio graph gets stuck");
    // Deliberately no shortcut: this one stops the sound for a moment.
    connect(eng, &QMenu::aboutToShow, this, [this] {
        m_restartAct->setText(m_engineLive ? "&Restart audio engine..."
                                           : "&Start audio engine...");
    });
    eng->addSeparator();

    auto* boot = eng->addMenu("Start at &login");
    m_autoEngine = boot->addAction("Audio engine", this, [this](bool on) {
        setEngineAutostart(on);
        refreshAutostart();
        say(on ? "Engine will start at login"
                                    : "Engine will no longer start at login");
    });
    m_autoEngine->setCheckable(true);
    m_autoGui = boot->addAction("Mixer window", this, [this](bool on) {
        setGuiAutostart(on);
        refreshAutostart();
        say(on ? "Mixer will open at login"
                                    : "Mixer will no longer open at login");
    });
    m_autoGui->setCheckable(true);
    // Reflect changes made outside the app (systemctl, another window).
    connect(boot, &QMenu::aboutToShow, this, &MainWindow::refreshAutostart);
    refreshAutostart();

    eng->addSeparator();
    // close(), not quit(): QApplication::quit() delivers no close event, so
    // the window geometry would never be saved.
    eng->addAction("&Quit GUI", QKeySequence("Ctrl+Q"), this, [this] { close(); });

    auto* view = menuBar()->addMenu("&View");
    m_presetBarAct = view->addAction("&Preset bar", this,
                                     [this](bool on) { setPresetBarVisible(on); });
    m_presetBarAct->setCheckable(true);
    m_presetBarAct->setToolTip("One button per saved preset across the top, Ctrl+1..9 to match");
    view->addSeparator();
    {   // A preference on top of the compositor's scale, not a HiDPI fix.
        auto* zoom = view->addMenu("Interface &size");
        auto* zg = new QActionGroup(this);
        const int cur = QSettings("betterbanana", "gui").value("uiScale", 100).toInt();
        for (int pct : { 100, 125, 150, 175 }) {
            QAction* a = zoom->addAction(QString::number(pct) + "%", this, [this, pct, zg] {
                // Written only once the restart is agreed to. It used to be
                // stored first, so answering No still resized the next launch
                // and the radio button had already moved to say so.
                if (QMessageBox::question(this, "BetterBanana",
                        QString("Restart the mixer window at %1%?\n\n"
                                "The audio engine keeps running, so nothing you "
                                "hear stops.").arg(pct))
                    != QMessageBox::Yes) {
                    const int keep = QSettings("betterbanana", "gui")
                                         .value("uiScale", 100).toInt();
                    for (QAction* other : zg->actions())
                        other->setChecked(other->text() == QString::number(keep) + "%");
                    return;
                }
                QSettings("betterbanana", "gui").setValue("uiScale", pct);
                saveWindowGeometry();
                // Carry the flags forward, or --apps and friends are lost.
                QProcess::startDetached(QApplication::applicationFilePath(),
                                        QApplication::arguments().mid(1));
                close();
            });
            a->setCheckable(true);
            a->setChecked(pct == cur);
            zg->addAction(a);
        }
        view->addSeparator();
    }
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    const auto& themes = builtinThemes();
    for (int i = 0; i < themes.size(); ++i) {
        auto* a = view->addAction(themeSwatch(themes[i]), themes[i].name, this,
                                  [this, i] { applyTheme(i); });
        a->setCheckable(true);
        group->addAction(a);
        m_themeActions.push_back(a);
    }

    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&Keyboard and mouse...", this, [this] {
        QMessageBox::information(this, "Keyboard and mouse",
            "Faders and knobs\n"
            "  drag            change the value\n"
            "  Ctrl + drag     fine control\n"
            "  double-click    back to the default\n"
            "  wheel           step (fader must be clicked first)\n"
            "  arrows          step, once focused\n"
            "  middle-click    jump straight to a position (faders)\n\n"
            "Strips and buses\n"
            "  right-click EQ or FX    open its editor\n"
            "  right-click a title     rename it\n"
            "  right-click a device    remember settings for that device\n"
            "  click a meter           clear that column's clip indicator");
    });
    help->addAction("&About BetterBanana", this, &MainWindow::showAbout);
}

// Lists the saved presets so one can be picked as what the engine restores at
// startup. Rebuilt each time it opens, so a preset saved a moment ago is there.
void MainWindow::populateStartupMenu(QMenu* menu)
{
    menu->clear();
    const QString cur = QString::fromStdString(startup_preset_name());
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);

    QAction* none = menu->addAction("(none - start with a default mixer)", this, [this] {
        set_startup_preset_name(std::string());
        say("The engine will start with a default mixer");
    });
    none->setCheckable(true);
    none->setChecked(cur.isEmpty());
    group->addAction(none);

    QDir dir(QString::fromStdString(presets_path()));
    const QStringList files = dir.entryList({ "*.bbp" }, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        QAction* a = menu->addAction("no presets saved yet  (Preset -> Save preset...)");
        a->setEnabled(false);
        return;
    }
    menu->addSeparator();
    bool matched = false;
    for (const QString& f : files) {
        const QString name = QFileInfo(f).completeBaseName();
        QAction* a = menu->addAction(name, this, [this, name] {
            set_startup_preset_name(name.toStdString());
            say("\"" + name + "\" will load when the engine starts", 6000);
        });
        a->setCheckable(true);
        a->setChecked(name == cur);
        if (name == cur) matched = true;
        group->addAction(a);
    }
    // The marker can also hold an outright path, which no name in the list will
    // match; show it rather than leaving nothing ticked and no explanation.
    if (!cur.isEmpty() && !matched) {
        menu->addSeparator();
        QAction* a = menu->addAction(cur);
        a->setCheckable(true);
        a->setChecked(true);
        a->setEnabled(false);
        group->addAction(a);
    }
}

void MainWindow::refreshAutostart()
{
    if (!m_autoEngine || !m_autoGui) return;
    const bool installed = engineUnitInstalled();
    {
        QSignalBlocker b(m_autoEngine);
        m_autoEngine->setEnabled(installed);
        m_autoEngine->setChecked(installed && engineAutostart());
        m_autoEngine->setToolTip(installed
            ? "Run the audio engine as a systemd user service from login onward"
            : "Not available: run 'make install' so the service unit exists");
    }
    {
        QSignalBlocker b(m_autoGui);
        m_autoGui->setChecked(guiAutostart());
    }
}

void MainWindow::applyTheme(int index)
{
    setThemeIndex(index);
    // Palette first: it carries the theme into the widgets Qt paints natively,
    // which the stylesheet never touches.
    qApp->setPalette(themePalette(theme()));
    qApp->setStyleSheet(buildStyleSheet(theme()));
    if (index >= 0 && index < m_themeActions.size()) m_themeActions[index]->setChecked(true);
    QSettings("betterbanana", "gui").setValue("theme", index);
    // Custom-painted widgets read theme() directly, so just force a repaint.
    for (QWidget* w : findChildren<QWidget*>()) w->update();
    update();
}

// Read device assignment out of the engine and show it. Without this the GUI
// starts up believing nothing is assigned, and the first combo change would
// write those empty strings back, wiping the engine's routing.
void MainWindow::readRouting()
{
    char hw[kHwStrips][kNameLen], out[kPhysBuses][kNameLen];
    uint32_t seq = 0;
    bool ok = false;
    for (int t = 0; t < 16 && !ok; ++t) ok = routing_read(m_shm->routing, seq, hw, out);
    if (!ok) return;
    for (int i = 0; i < kHwStrips; ++i) {
        m_hwIn[i] = QString::fromUtf8(hw[i]);
        m_strips[i]->setDeviceValue(m_hwIn[i]);
    }
    for (int b = 0; b < kPhysBuses; ++b) {
        m_busOut[b] = QString::fromUtf8(out[b]);
        m_buses[b]->setDeviceValue(m_busOut[b]);
    }
}

// ---------------------------------------------------------------------------
// Undo.
//
// Every control here writes straight into shared memory, so rather than
// instrument each one, watch the memory: serialise the whole mixer a couple of
// times a second and commit an entry once it has stopped moving. A fader sweep
// or an EQ drag therefore becomes one step rather than thirty, and a change
// made by bb-ctl in another terminal is undoable too.
// ---------------------------------------------------------------------------
static constexpr int kUndoDepth = 64;

void MainWindow::snapshotTick()
{
    const QByteArray cur = QByteArray::fromStdString(preset_serialize(m_shm));
    if (cur != m_seen) { m_seen = cur; return; }     // still moving; wait for it to settle
    if (cur == m_committed) return;                  // nothing new
    m_undo.append(m_committed);
    if (m_undo.size() > kUndoDepth) m_undo.removeFirst();
    m_redo.clear();
    m_committed = cur;
    refreshUndoActions();
    // The mixer has moved since the preset was loaded or saved. Say so in the
    // title, which on a tiling compositor may be the only chrome there is.
    if (!m_dirty && !m_presetName.isEmpty()) { m_dirty = true; refreshTitle(); }
}

void MainWindow::applyState(const QByteArray& text)
{
    preset_deserialize(m_shm, text.toStdString());
    m_shm->cmd.store(kCmdVbanReload);
    m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
    for (auto* s : m_strips) s->pullFromShm();
    for (auto* b : m_buses)  b->pullFromShm();
    readRouting();
    // Adopt it as the settled state, or the next tick would record the undo
    // itself as a fresh change.
    m_committed = m_seen = text;
    refreshUndoActions();
}

void MainWindow::undo()
{
    if (m_undo.isEmpty()) { say("Nothing to undo"); return; }
    m_redo.append(m_committed);
    applyState(m_undo.takeLast());
    say(QString("Undone  (%1 step%2 left)")
                             .arg(m_undo.size()).arg(m_undo.size() == 1 ? "" : "s"));
}

void MainWindow::redo()
{
    if (m_redo.isEmpty()) { say("Nothing to redo"); return; }
    m_undo.append(m_committed);
    applyState(m_redo.takeLast());
    say("Redone");
}

void MainWindow::refreshUndoActions()
{
    if (m_undoAct) m_undoAct->setEnabled(!m_undo.isEmpty());
    if (m_redoAct) m_redoAct->setEnabled(!m_redo.isEmpty());
}

// The mixer really did change, and it now matches whatever it was changed to.
// Loading a preset and restarting the engine both mean this, and both used to
// leave the snapshot behind: the next tick found a state nobody had committed,
// recorded it as a change, and marked the title dirty - so loading "Screen
// share" showed "Screen share *" half a second later.
void MainWindow::commitAsSettled()
{
    m_undo.append(m_committed);
    if (m_undo.size() > kUndoDepth) m_undo.removeFirst();
    m_redo.clear();
    m_committed = m_seen = QByteArray::fromStdString(preset_serialize(m_shm));
    refreshUndoActions();
}

// ---------------------------------------------------------------------------
// The preset bar.
//
// One button per saved preset across the top, the loaded one lit, Ctrl+1..9 in
// the same order. Presets are how a mixer this size is actually used - a mix
// for a call is not a mix for music - and reaching them through a file dialog
// made switching feel like filing rather than like a control.
// ---------------------------------------------------------------------------
bool MainWindow::loadPresetFile(const QString& path)
{
    if (!load_preset(m_shm, path.toUtf8().constData())) {
        say("Could not read " + path, 6000);
        return false;
    }
    m_shm->cmd.store(kCmdVbanReload);
    m_shm->cmd_seq.fetch_add(1, std::memory_order_release);
    for (auto* w : m_strips) w->pullFromShm();
    for (auto* w : m_buses)  w->pullFromShm();
    readRouting();
    m_presetName = QFileInfo(path).completeBaseName();
    m_dirty = false;
    commitAsSettled();
    refreshTitle();
    rebuildPresetBar();                 // the lit button moves
    // The engine re-finds a device whose node name moved on its next control
    // poll, so give it one before reporting what is missing.
    QTimer::singleShot(800, this, &MainWindow::reportMissingDevices);
    return true;
}

void MainWindow::savePresetAs()
{
    QDir().mkpath(QString::fromStdString(presets_path()));
    QString f = QFileDialog::getSaveFileName(this, "Save preset",
                    QString::fromStdString(presets_path()), "Presets (*.bbp)");
    if (f.isEmpty()) return;
    if (!f.endsWith(".bbp")) f += ".bbp";
    if (!save_preset(m_shm, f.toUtf8().constData())) {
        QMessageBox::warning(this, "BetterBanana", "Could not write " + f);
        return;
    }
    m_presetName = QFileInfo(f).completeBaseName();
    m_dirty = false;
    refreshTitle();
    rebuildPresetBar();
    say("Saved " + f);
    // Asked once, and only while nothing is set: the engine starts with a
    // default mixer until something is chosen, which is worth saying out
    // loud the first time rather than leaving to be discovered.
    if (!startup_preset_name().empty()) return;
    const QString name = QFileInfo(f).completeBaseName();
    if (QMessageBox::question(this, "BetterBanana",
            QString("Load \"%1\" whenever the audio engine starts?\n\n"
                    "Nothing is set at the moment, so the engine currently "
                    "comes up with a default mixer. You can change this "
                    "later under Preset -> Load on startup.").arg(name))
        != QMessageBox::Yes) return;
    set_startup_preset_name(QFileInfo(f).absoluteFilePath() ==
                            QString::fromStdString(preset_path_for(name.toStdString()))
                            ? name.toStdString()
                            : f.toStdString());
    say("\"" + name + "\" will load when the engine starts", 6000);
}

// Right-clicking a preset button. The three things wanted often enough to be
// worth not opening a file manager for: update it, make it the one the engine
// comes up with, and get rid of it.
void MainWindow::presetMenu(QWidget* anchor, const QString& name,
                            const QString& path, const QPoint& pos)
{
    const bool isStartup = QString::fromStdString(startup_preset_name()) == name;

    QMenu m;
    m.addAction("&Load", this, [this, name, path] {
        if (loadPresetFile(path)) say("Loaded \"" + name + "\"");
    });
    m.addAction("&Overwrite with the current mix", this, [this, name, path] {
        if (QMessageBox::question(this, "BetterBanana",
                QString("Replace \"%1\" with the mixer as it is now?").arg(name))
            != QMessageBox::Yes) return;
        if (!save_preset(m_shm, path.toUtf8().constData())) {
            QMessageBox::warning(this, "BetterBanana", "Could not write " + path);
            return;
        }
        m_presetName = name;
        m_dirty = false;
        refreshTitle();
        rebuildPresetBar();
        say("\"" + name + "\" updated");
    });
    QAction* boot = m.addAction("Load when the engine &starts", this,
                                [this, name, isStartup] {
        set_startup_preset_name(isStartup ? std::string() : name.toStdString());
        say(isStartup ? "The engine will start with a default mixer"
                      : "\"" + name + "\" will load when the engine starts", 6000);
    });
    boot->setCheckable(true);
    boot->setChecked(isStartup);
    m.addSeparator();
    QAction* del = m.addAction("&Delete...", this, [this, name, path, isStartup] {
        if (QMessageBox::question(this, "BetterBanana",
                QString("Delete the preset \"%1\"?%2\n\nThe mixer keeps playing "
                        "exactly as it is; only the saved file goes.")
                    .arg(name, isStartup
                        ? "\n\nIt is also what the engine loads at startup, so "
                          "that choice is cleared and the engine will come up with "
                          "a default mixer."
                        : ""))
            != QMessageBox::Yes) return;
        if (!QFile::remove(path)) {
            QMessageBox::warning(this, "BetterBanana", "Could not delete " + path);
            return;
        }
        // A startup marker pointing at a file that is gone makes the engine log
        // a failure and come up default without ever saying why.
        if (isStartup) set_startup_preset_name(std::string());
        // The name stays in the title: the mixer is still set up that way, it
        // just no longer has a file to go back to. Hence the asterisk.
        if (m_presetName == name) { m_dirty = true; refreshTitle(); }
        rebuildPresetBar();
        say("Deleted \"" + name + "\"");
    });
    del->setProperty("cta", "danger");
    m.exec(anchor->mapToGlobal(pos));
}

void MainWindow::setPresetBarVisible(bool on)
{
    QSettings("betterbanana", "gui").setValue("presetBar", on);
    if (m_presetBarAct) m_presetBarAct->setChecked(on);
    if (m_presetBar) m_presetBar->setVisible(on);
}

void MainWindow::rebuildPresetBar()
{
    if (!m_presetLay) return;
    while (QLayoutItem* it = m_presetLay->takeAt(0)) {
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
    m_presetOrder.clear();

    const QString dir = QString::fromStdString(presets_path());
    const QStringList files = QDir(dir).entryList({ "*.bbp" }, QDir::Files, QDir::Name);

    auto* cap = new QLabel("PRESET");
    cap->setProperty("role", "caption");
    m_presetLay->addWidget(cap);

    if (files.isEmpty()) {
        auto* none = new QLabel("nothing saved yet");
        none->setProperty("role", "caption");
        m_presetLay->addWidget(none);
    }

    // Nine, because that is how many Ctrl+<digit> there are. The rest stay
    // reachable through Preset -> Load.
    const int shown = qMin(files.size(), 9);
    for (int i = 0; i < shown; ++i) {
        const QString name = QFileInfo(files[i]).completeBaseName();
        m_presetOrder << name;
        auto* b = new QPushButton(name);
        b->setProperty("role", "preset");
        b->setCheckable(true);
        b->setChecked(name == m_presetName);
        b->setToolTip(QString("Load \"%1\"   (Ctrl+%2)\nRight-click for more")
                          .arg(name).arg(i + 1));
        const QString path = QString::fromStdString(preset_path_for(name.toStdString()));
        connect(b, &QPushButton::clicked, this, [this, name, path] {
            if (loadPresetFile(path)) say("Loaded \"" + name + "\"");
            else rebuildPresetBar();            // put the lit button back
        });
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QPushButton::customContextMenuRequested, this,
                [this, b, name, path](const QPoint& pos) { presetMenu(b, name, path, pos); });
        m_presetLay->addWidget(b);
    }
    if (files.size() > shown) {
        auto* more = new QLabel(QString("+%1 more").arg(files.size() - shown));
        more->setProperty("role", "caption");
        more->setToolTip("Preset -> Load preset... reaches all of them");
        m_presetLay->addWidget(more);
    }

    m_presetLay->addStretch(1);
    auto* add = new QPushButton("Save as...");
    add->setProperty("role", "preset");
    add->setToolTip("Save the mixer as it is now under a new name");
    connect(add, &QPushButton::clicked, this, [this] { savePresetAs(); });
    m_presetLay->addWidget(add);
}

// ---------------------------------------------------------------------------
// Restarting the engine from inside the mixer.
//
// It lives here, next to the undo machinery, because a restart has to leave
// that machinery consistent: the mix as it was before is either put straight
// back, or becomes exactly one undo step.
//
// The shared segment is deliberately never unlinked (see engine.cpp), so this
// window's mapping stays valid across the bounce and there is nothing to
// re-attach. What does NOT survive is the mix: the engine loads the startup
// preset and writes nothing on exit, so without carrying the state across by
// hand a restart would silently revert to whatever Preset -> Load on startup
// names. Hence the checkbox - and hence the state is captured before anything
// is stopped, since by the time it is wanted back the engine has zeroed the
// segment.
//
// The meter timer is stopped for the duration rather than left running, and the
// polling below reads only magic, version, struct_size, the pid and the
// heartbeat - the fields at the top of Shared, which have never moved. A
// rebuilt engine may lay Shared out differently and ftruncate the segment
// SMALLER, and reading a field past the end of a shrunk mapping is a SIGBUS
// rather than a wrong number, so the layout is checked before anything else
// is touched.
// ---------------------------------------------------------------------------
void MainWindow::restartEngine()
{
    if (m_restarting) return;

    const pid_t old = (pid_t)m_shm->engine_pid.load();
    const bool live = m_engineLive && pidAlive(old);

    QMessageBox ask(this);
    ask.setWindowTitle("BetterBanana");
    ask.setIcon(QMessageBox::Question);
    ask.setText(live ? "<b>Restart the audio engine?</b>"
                     : "<b>Start the audio engine?</b>");
    ask.setInformativeText(live
        ? "Sound stops for a second or two. BetterBanana's virtual devices leave "
          "the graph and come back, so anything playing into one needs a moment - "
          "and an application that was pointed at a BetterBanana device by hand, "
          "rather than by a remembered rule, may land somewhere else.\n\n"
          "This window keeps running."
        : "Nothing is being mixed at the moment. Starting the engine puts the "
          "virtual devices back into the graph.");
    QCheckBox* keep = nullptr;
    if (live) {
        keep = new QCheckBox("Put the current mix back afterwards");
        keep->setChecked(true);
        keep->setToolTip("Otherwise the engine comes up with the preset named under "
                         "Preset -> Load on startup");
        ask.setCheckBox(keep);                      // takes ownership
    }
    QPushButton* go = ask.addButton(live ? "Restart" : "Start", QMessageBox::AcceptRole);
    go->setProperty("cta", "primary");
    ask.addButton("Cancel", QMessageBox::RejectRole);
    ask.setDefaultButton(go);
    ask.exec();
    if (ask.clickedButton() != go) return;

    const bool restore = live && keep->isChecked();
    // Read while the engine is still up and the segment still says what the
    // mixer looks like.
    const QByteArray before = QByteArray::fromStdString(preset_serialize(m_shm));

    const bool unit       = engineUnitInstalled();
    const bool viaSystemd = unit && engineUnitActive();
    // Resolved before anything is stopped. Discovering there is nothing to
    // start only after the engine has been killed leaves the machine silent.
    const QString bin = unit ? QString() : engineBinary();
    if (!unit && bin.isEmpty()) {
        QMessageBox::warning(this, "BetterBanana",
            "bb-engine cannot be found, and there is no service to start it.\n\n"
            "Nothing was changed. Run 'make install' so the systemd user unit "
            "exists, or put bb-engine on PATH.");
        return;
    }

    // Puts back everything the wait below disturbs, however this returns.
    struct Busy {
        MainWindow* w;
        bool resumeTimer = true;
        ~Busy() {
            hold_health_watchdog(0);
            QGuiApplication::restoreOverrideCursor();
            w->menuBar()->setEnabled(true);
            w->m_restarting = false;
            // Adopt the new engine as the one being watched, or the tick after
            // this would report the restart the user just asked for as one that
            // happened behind their back.
            w->m_enginePid = w->m_shm->engine_pid.load();
            if (!resumeTimer) return;
            // The counter restarted from zero, so re-prime the stall detector
            // rather than let it read one fresh heartbeat as a stalled one.
            w->m_lastHeartbeat = w->m_shm->engine_heartbeat.load();
            w->m_stallTicks = 0;
            w->m_timer->start(33);
        }
    } busy{this};

    m_restarting = true;
    m_timer->stop();
    // This restart was asked for, so it is not the loss the banner is about.
    m_recovered.clear();
    m_offer->setVisible(false);
    menuBar()->setEnabled(false);
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    // bb-health repairs exactly the symptom this is about to produce. Left to
    // itself it would race: restart the engine a second time, then put its own
    // last-known-good snapshot back over the mix restored below.
    hold_health_watchdog(45);
    m_alert->setText(live ? "Restarting the audio engine..."
                          : "Starting the audio engine...");
    m_alert->setVisible(true);

    // processEvents returns as soon as the queue is empty, so on its own the
    // polling below would be a spin. The window is frozen anyway.
    auto settle = [] {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        QThread::msleep(30);
    };
    settle();

    int code = 0;
    if (viaSystemd) {
        // systemctl restart does not return until the unit is back up.
        runProc("systemctl", { "--user", "restart", kUnit }, &code, 45000);
    } else {
        if (pidAlive(old)) {
            ::kill(old, SIGTERM);
            QElapsedTimer t; t.start();
            while (t.elapsed() < 5000 && pidAlive(old)) settle();
            if (pidAlive(old)) {
                QMessageBox::warning(this, "BetterBanana",
                    QString("The running engine (pid %1) did not stop, so it was "
                            "left alone.\n\nStop it by hand and try again:\n"
                            "    bb-ctl quit").arg(old));
                return;
            }
        }
        // An engine started by hand while the unit exists is stopped above and
        // restarted through systemd, which is where it should have been.
        if (unit) runProc("systemctl", { "--user", "start", kUnit }, &code, 45000);
        else      code = QProcess::startDetached(bin, {}) ? 0 : 1;
    }

    // Wait for a different, living engine to claim the segment, then for it to
    // start mixing. The pid is written before PipeWire is even connected; the
    // heartbeat only moves once the graph is running, and by then the engine
    // has finished loading its own startup preset - so the write below cannot
    // race it.
    QElapsedTimer t; t.start();
    pid_t fresh = 0;
    while (code == 0 && t.elapsed() < 15000) {
        const pid_t p = (pid_t)m_shm->engine_pid.load();
        if (p > 0 && p != old && pidAlive(p)) { fresh = p; break; }
        settle();
    }
    bool ticking = false;
    const uint32_t h0 = m_shm->engine_heartbeat.load();
    while (fresh && t.elapsed() < 25000) {
        if (m_shm->engine_heartbeat.load() != h0) { ticking = true; break; }
        settle();
    }
    if (!ticking) {
        m_alert->setText("The audio engine did not come back.");
        // An engine that got as far as reinitialising the segment and then died
        // has already replaced the mix with its own defaults, so putting it back
        // is what makes the sentence below true.
        const bool kept = restore && shm_compatible(m_shm);
        if (kept) applyState(before);
        QMessageBox::warning(this, "BetterBanana",
            QString("The engine did not come back.%1\n\nWhat stopped it is in "
                    "its log:\n    systemctl --user status betterbanana-engine\n"
                    "    journalctl --user -u betterbanana-engine -n 50")
                .arg(kept ? "\n\nYour mix is still in this window, and "
                            "Preset -> Save preset will write it out." : ""));
        return;
    }

    // A rebuilt engine can speak a different protocol, and everything from here
    // on reads past the header. Checked before any of it.
    if (!shm_compatible(m_shm)) {
        busy.resumeTimer = false;           // never tick against this mapping again
        QMessageBox box(this);
        box.setWindowTitle("BetterBanana");
        box.setIcon(QMessageBox::Warning);
        box.setText("<b>The engine that came back is a different build.</b>");
        box.setInformativeText(
            QString("It speaks protocol v%1 and this window speaks v%2, so the "
                    "window has to be reopened before it can be trusted with "
                    "what the engine says.")
                .arg(m_shm->version.load()).arg(kVersion));
        QPushButton* again = box.addButton("Reopen the mixer", QMessageBox::AcceptRole);
        again->setProperty("cta", "primary");
        box.addButton("Quit", QMessageBox::RejectRole);
        box.setDefaultButton(again);
        box.exec();
        saveWindowGeometry();
        if (box.clickedButton() == again)   // carry the flags forward
            QProcess::startDetached(QApplication::applicationFilePath(),
                                    QApplication::arguments().mid(1));
        qApp->quit();
        return;
    }

    // Every node in the graph is new, so both device lists are stale. Done
    // before the state is written back, so the routing below has something to
    // select from.
    refreshDevices();

    if (restore) {
        // The mixer ends up where it started, so there is nothing to record.
        // applyState adopts this as the settled state, which is what stops the
        // next snapshot from reporting the round trip as a change.
        applyState(before);
    } else {
        // The mixer really did change, to whatever the startup preset says.
        // `before` rather than m_committed: the two are the same here, but this
        // is the state the dialog promised was recoverable.
        m_committed = before;
        commitAsSettled();
        const std::string want = startup_preset_name();
        m_presetName = want.empty()
            ? QString()
            : QFileInfo(QString::fromStdString(want)).completeBaseName();
        m_dirty = false;
        refreshTitle();
        for (auto* w : m_strips) w->pullFromShm();
        for (auto* w : m_buses)  w->pullFromShm();
        readRouting();
    }

    m_alert->setVisible(false);
    m_alert->setText(QString());
    refreshCardStates();
    const QString what = QString("Audio engine %1 (pid %2)")
                             .arg(live ? "restarted" : "started").arg(fresh);
    say(restore            ? what + ", and your mix is back"
        : m_presetName.isEmpty() ? what + " with a default mixer"
                                 : what + QString(" from \"%1\"").arg(m_presetName),
        8000);
    // The engine re-finds a device on its next control poll, so give it one
    // before anything is reported as missing.
    QTimer::singleShot(1500, this, &MainWindow::reportMissingDevices);
}

// The other half of the restart story: an engine that came back without being
// asked. The unit is PartOf=pipewire.service so a PipeWire restart takes the
// engine with it, and the watchdog restarts it whenever its nodes leave the
// graph. Either way what comes back is the startup preset - a choice made once,
// not what was actually set up - and until now that happened silently.
//
// Neither accepting it nor undoing it on the user's behalf is right, so the
// window says what happened and puts the answer next to it.
void MainWindow::offerRecoveredMix()
{
    if (m_recovered.isEmpty() || m_restarting) return;
    if (!shm_compatible(m_shm)) { m_recovered.clear(); return; }

    // Every node in the graph is new, so the device lists are stale whether or
    // not anything turns out to have been lost. Before the messages below,
    // which it would otherwise overwrite.
    refreshDevices();

    const QByteArray now = QByteArray::fromStdString(preset_serialize(m_shm));
    if (now == m_recovered) {           // it came back exactly where it was
        m_recovered.clear();
        say("The audio engine restarted. Your mix came back unchanged.", 6000);
        return;
    }
    m_offerText->setText("The audio engine restarted on its own, and came back with "
                         "its startup preset. The mix you had is one click away.");
    m_offer->setVisible(true);
    // The banner is easy to miss on a window that was not in front, so the
    // status bar carries it as well.
    say("The audio engine restarted - your previous mix can be put back", 12000);
}

// Settings a strip is told to remember follow the microphone, not the slot:
// plug the same interface into another strip and its gate, compressor and EQ
// come with it. Only a real change of the combo reaches here.
void MainWindow::applyDeviceStrip(int strip, const QString& device)
{
    if (device.isEmpty() || strip < 0 || strip >= kStrips) return;
    if (!has_strip_for_device(device.toStdString())) return;
    if (!load_strip_for_device(m_shm, strip, device.toStdString())) return;
    m_strips[strip]->pullFromShm();
    say(
        QString("Restored the settings remembered for %1").arg(device));
}

// After loading a preset, say plainly which of the devices it names are not
// here. Deferred, because the engine re-finds a moved device by description on
// its next control poll and we want to report what is left after that.
void MainWindow::reportMissingDevices()
{
    readRouting();
    QStringList missing;
    for (int i = 0; i < kHwStrips; ++i)
        if (m_strips[i]->deviceMissing())
            missing << QString("Input %1  ->  %2").arg(i + 1).arg(m_hwIn[i]);
    for (int b = 0; b < kPhysBuses; ++b)
        if (m_buses[b]->deviceMissing())
            missing << QString("Bus %1  ->  %2").arg(kBusLabel[b], m_busOut[b]);
    if (missing.isEmpty()) return;
    QMessageBox::information(this, "BetterBanana",
        QString("The preset loaded, but %1 device%2 it names %3 not here, so "
                "those are left unconnected:\n\n  %4\n\n"
                "Plug it in and the engine will pick it up, or choose something "
                "else from the drop-down.")
            .arg(missing.size())
            .arg(missing.size() == 1 ? "" : "s")
            .arg(missing.size() == 1 ? "is" : "are")
            .arg(missing.join("\n  ")));
}

// An EQ profile remembered through the headphone browser belongs to the device,
// not to the bus: point A2 at a different pair and its correction follows. Only
// a real change of the combo reaches here - setDeviceList and setDeviceValue
// both block the combo's signals - so this never fires on a device refresh.
void MainWindow::applyDeviceEq(int bus, const QString& device)
{
    if (device.isEmpty()) return;
    const QString name = AutoEqDialog::applyRemembered(m_shm, bus, device);
    if (name.isEmpty()) return;
    m_buses[bus]->pullFromShm();
    say(QString("%1: applied EQ profile \"%2\"")
                             .arg(kBusLabel[bus], name));
}

void MainWindow::writeRouting()
{
    routing_write_begin(m_shm->routing);
    for (int i = 0; i < kHwStrips; ++i)
        snprintf(m_shm->routing.hw_in[i], kNameLen, "%s", m_hwIn[i].toUtf8().constData());
    for (int b = 0; b < kPhysBuses; ++b)
        snprintf(m_shm->routing.bus_out[b], kNameLen, "%s", m_busOut[b].toUtf8().constData());
    routing_write_end(m_shm->routing);
}

void MainWindow::refreshDevices()
{
    QStringList srcIds, srcLabels, sinkIds, sinkLabels;

    // Hardware strips: the engine's own virtual cables first, then real
    // capture devices. A cable is stored as "cable:N", not a node name.
    for (int c = 0; c < kCables; ++c) {
        srcIds << QString("%1%2").arg(kCablePrefix).arg(c);
        srcLabels << QString("\u2192 BetterBanana Cable %1").arg(c + 1);
    }
    for (const DevEntry& d : listDevices(false, false)) { srcIds << d.id; srcLabels << d.label; }

    // Buses drive real hardware only; offering our own sinks here would invite
    // routing loops.
    for (const DevEntry& d : listDevices(true, false)) { sinkIds << d.id; sinkLabels << d.label; }

    for (int i = 0; i < kHwStrips; ++i) m_strips[i]->setDeviceList(srcIds, srcLabels);
    for (int b = 0; b < kPhysBuses; ++b) m_buses[b]->setDeviceList(sinkIds, sinkLabels);
    readRouting();
    say(QString("%1 capture / %2 playback devices")
                             .arg(srcIds.size() - kCables).arg(sinkIds.size()));
}

// Re-route newly appeared streams according to the saved rules. Applied once
// per stream, so a manual move afterwards is respected.
// Four reads, all in flight at once, none of them on the timer's critical path.
// The writes that follow are rare - only when a stream first appears with a
// rule that does not match where it landed - so they stay synchronous.
void MainWindow::applyAppRules()
{
    if (m_ruleBusy) return;              // a round is still in flight
    m_ruleBusy = true;

    struct Fetch { QString sinks, sinkIn, sourceIn, srcShort, sinkShort; int pending = 5; };
    auto acc = std::make_shared<Fetch>();
    const uint32_t round = ++m_ruleRound;
    auto done = [this, acc, round](const QString&) {
        if (--acc->pending > 0) return;
        // A round the watchdog already gave up on describes a state that has
        // moved on. Acting on it would re-route from a stale snapshot.
        if (round != m_ruleRound) return;
        m_ruleBusy = false;
        // pactlAsync hands back an empty string for a failed or timed-out read;
        // a genuinely empty list is "[]". Nothing at all means we cannot see the
        // session, so there is nothing safe to conclude from it.
        if (acc->sinks.isEmpty() || acc->sinkIn.isEmpty()) return;
        // A partial read must not prune the set of streams already handled: the
        // pruning is the only thing stopping a rule being re-applied, so
        // dropping a key because one of five reads timed out is exactly how an
        // app gets yanked off the sink the user just chose. Keep the stale keys
        // and let the next complete round prune them.
        const bool complete = !acc->sinkShort.isEmpty() && !acc->sourceIn.isEmpty()
                           && !acc->srcShort.isEmpty();
        applyAppRulesWith(acc->sinks, acc->sinkIn, acc->sinkShort,
                          acc->sourceIn, acc->srcShort, complete);
    };
    pactlAsync({ "-f", "json", "list", "sinks" }, this,
               [acc, done](const QString& o) { acc->sinks = o; done(o); });
    pactlAsync({ "-f", "json", "list", "sink-inputs" }, this,
               [acc, done](const QString& o) { acc->sinkIn = o; done(o); });
    pactlAsync({ "list", "short", "sinks" }, this,
               [acc, done](const QString& o) { acc->sinkShort = o; done(o); });
    pactlAsync({ "-f", "json", "list", "source-outputs" }, this,
               [acc, done](const QString& o) { acc->sourceIn = o; done(o); });
    pactlAsync({ "list", "short", "sources" }, this,
               [acc, done](const QString& o) { acc->srcShort = o; done(o); });
}

void MainWindow::applyAppRulesWith(const QString& sinksJson,
                                   const QString& sinkInputs, const QString& sinkShort,
                                   const QString& sourceOutputs, const QString& sourceShort,
                                   bool complete)
{
    // A rule saved before the stream bus was excluded from the target list can
    // still point at it, and re-applying that rule silences the app every time
    // the mixer opens. Drop such a rule instead of honouring it.
    QSet<QString> captureOnly;
    for (const DevEntry& d : parseDevices(sinksJson, true))
        if (d.captureOnly) captureOnly.insert(d.id);

    QSet<int> seen;
    for (bool pb : { true, false }) {
        for (const StreamInfo& s : (pb ? parseStreams(sinkInputs, sinkShort, true)
                                       : parseStreams(sourceOutputs, sourceShort, false))) {
            const int key = pb ? s.index : -(s.index + 1);
            seen.insert(key);
            if (m_ruledStreams.contains(key)) continue;
            m_ruledStreams.insert(key);
            const QString want = ruleFor(s.app, pb);
            if (want.isEmpty() || want == s.target) continue;
            if (pb && captureOnly.contains(want)) { setRule(s.app, pb, QString()); continue; }
            pactlRun({ pb ? "move-sink-input" : "move-source-output",
                       QString::number(s.index), want });
        }
    }
    if (complete) m_ruledStreams.intersect(seen);
}

// Bluetooth is the reason this exists. A link negotiates its codec with the
// headset every time it connects, so the figure an alignment was built on can be
// forty milliseconds different tomorrow - and the alignment quietly stops being
// one, with nothing on screen to say so.
//
// Deliberately narrow, and off unless asked for. It moves a delay only when the
// outputs WERE aligned before the device moved: after that test, writing the
// new figure is restoring the state the user asked for, not inventing one. A
// mix somebody set by hand never has a spread of zero, so it is never touched.
void MainWindow::watchAlignment()
{
    if (m_versionLost || m_restarting) return;
    float now[kPhysBuses], del[kPhysBuses];
    bool  inc[kPhysBuses];

    char hw[kHwStrips][kNameLen] = {}, bo[kPhysBuses][kNameLen] = {};
    uint32_t seq = 0;
    bool routed = false;
    for (int t = 0; t < 16 && !routed; ++t) routed = routing_read(m_shm->routing, seq, hw, bo);

    bool moved = false;
    for (int b = 0; b < kPhysBuses; ++b) {
        now[b] = m_shm->out_latency_ms[b].load();
        del[b] = m_shm->bus[b].delay_ms.load();
        // Same rule as the button and as bb-ctl: the screen-share sink is heard
        // by people on their own timeline and is never part of this.
        inc[b] = routed && bo[b][0] && std::strcmp(bo[b], kStreamSinkName) != 0;
        if (m_outLatSeen[b] >= 0.0f && now[b] >= 0.0f &&
            std::fabs(now[b] - m_outLatSeen[b]) > 3.0f) moved = true;   // not jitter
    }

    // The spread as it was before the device moved. Computed first, because the
    // next loop forgets the old figures whether or not anything is done.
    const float was = arrival_spread_ms(m_outLatSeen, del, kPhysBuses, inc);
    const bool  unchanged = !moved;
    for (int b = 0; b < kPhysBuses; ++b) m_outLatSeen[b] = now[b];
    if (unchanged) return;

    if (!QSettings("betterbanana", "gui").value("align/auto", false).toBool()) return;
    if (!(was >= 0.0f && was <= 1.0f)) return;      // it was not aligned; leave it

    float want[kPhysBuses];
    for (int b = 0; b < kPhysBuses; ++b) want[b] = del[b];
    if (!align_delays(now, want, kPhysBuses, inc)) return;
    for (int b = 0; b < kPhysBuses; ++b)
        if (inc[b] && now[b] >= 0.0f) m_shm->bus[b].delay_ms.store(want[b]);
    say("An output's latency changed - the outputs were lined up again", 8000);
}

// The engine came back as a build that lays the shared segment out
// differently. There is nothing safe left to do here: reading gives the wrong
// field and writing corrupts one, so the window stops rather than carrying on
// looking like it works.
void MainWindow::engineVersionChanged()
{
    if (m_versionLost) return;
    m_versionLost = true;
    m_timer->stop();
    m_restarting = true;                 // belt and braces: no writes from anywhere
    QMessageBox::critical(this, "BetterBanana",
        QString("The audio engine was replaced by a different build.\n\n"
                "This window speaks protocol v%1 and the engine now running does "
                "not, so it can no longer read or change the mix safely - it has "
                "stopped rather than move settings you did not touch.\n\n"
                "Your mix is intact in the engine. Start the mixer again to see it.")
            .arg(kVersion));
    close();
}

void MainWindow::tick()
{
    // restartEngine() stops this timer, but an event already queued when it did
    // would still arrive - and everything below reads a segment that is being
    // reinitialised, possibly by a build that lays it out differently.
    if (m_restarting) return;

    // Did the engine get replaced without being asked? The pid is the only
    // honest witness: the heartbeat stall takes a second to declare, by which
    // time a replacement engine has already reinitialised the segment, and the
    // mix that was in it is gone. Checked before the snapshot below, so
    // m_committed still holds the mixer as it was rather than as it now is.
    {
        const int pid = m_shm->engine_pid.load();
        if (pid > 0 && m_enginePid > 0 && pid != m_enginePid) {
            // The replacement can be a DIFFERENT BUILD. The segment is never
            // unlinked - that is exactly what lets a mix survive a restart -
            // so this window stays mapped over it at the old layout, and every
            // field past the first change of shape is now somewhere else.
            // Nothing announces that: the meters keep moving, and a write lands
            // on whatever now occupies the offset. Installing a build with one
            // new field silently zeroed a bus delay while this was being
            // written, from a window that had not been touched.
            //
            // The pid is stored after the header, so by the time it changes the
            // magic, version and size are all readable - and those three live
            // at the front of the segment, which is mapped whatever the other
            // build's Shared looks like.
            if (!shm_compatible(m_shm)) { engineVersionChanged(); return; }

            // A second restart while an offer is still standing must not
            // replace the mix being offered with the preset that replaced it.
            if (m_recovered.isEmpty()) m_recovered = m_committed;
            // Let the new engine load its own startup preset and publish its
            // nodes before deciding anything was actually lost.
            QTimer::singleShot(2000, this, &MainWindow::offerRecoveredMix);
        }
        if (pid > 0) m_enginePid = pid;
    }

    if (++m_alignTicks >= 60) { m_alignTicks = 0; watchAlignment(); }   // ~2 s
    if (++m_ruleTicks >= 30) { m_ruleTicks = 0; applyAppRules(); }
    // If a pactl round somehow neither finishes nor errors, the busy flag would
    // latch and application routing would stop for the life of the window.
    if (m_ruleBusy && ++m_ruleWait > 300) { m_ruleWait = 0; m_ruleBusy = false; ++m_ruleRound; }
    if (!m_ruleBusy) m_ruleWait = 0;

    if (m_engineLive) {
        for (auto* s : m_strips) s->refreshMeters();
        for (auto* b : m_buses)  b->refreshMeters();
    }
    m_recorder->refresh();

    if (++m_syncTicks >= 15) {          // twice a second
        m_syncTicks = 0;
        for (auto* s : m_strips) s->pullFromShm();
        for (auto* b : m_buses)  b->pullFromShm();
        readRouting();
    }

    if (++m_undoTicks >= 12) {          // about 2.5 times a second
        m_undoTicks = 0;
        snapshotTick();
    }

    // Engine liveness first: the meters below have to know before they paint,
    // or a dead engine leaves ten lit bars frozen exactly where they stopped.
    const uint32_t hb = m_shm->engine_heartbeat.load(std::memory_order_relaxed);
    if (hb == m_lastHeartbeat) ++m_stallTicks; else m_stallTicks = 0;
    m_lastHeartbeat = hb;
    const bool live = m_stallTicks < 30;
    if (live != m_engineLive) {
        m_engineLive = live;
        m_alert->setText(live ? QString()
                              : "The audio engine has stopped responding. Nothing "
                                "you change here is reaching it - try Engine \u2192 "
                                "Restart audio engine.");
        m_alert->setVisible(!live);
        refreshCardStates();
    }

    // dsp load leads: it is the one number here that predicts a glitch. The PID
    // is diagnostics, so it goes last.
    const QString txt = live
        ? QString("dsp %1%   ·   %2 Hz   ·   engine %3")
              .arg(m_shm->dsp_load.load() / 10.0, 0, 'f', 1)
              .arg(m_shm->samplerate.load())
              .arg(m_shm->engine_pid.load())
        : QString("engine not responding");
    // No unconditional setStyleSheet: this used to re-parse a sheet 30x a
    // second for a string that had not changed.
    // Recomputed only when the text or the theme changes: this used to
    // re-parse a stylesheet thirty times a second for a string that had not
    // moved, and then never re-parse it at all when the theme did.
    const QString col = bbcolor::ensureContrast(live ? theme().busA : theme().mute,
                                                theme().panel, bbcolor::kTextFloor)
                            .name(QColor::HexRgb);
    if (m_status->text() != txt) m_status->setText(txt);
    if (m_statusColour != col) {
        m_statusColour = col;
        m_status->setStyleSheet(QString("color:%1;").arg(col));
    }

    if (++m_stateTicks >= 15) { m_stateTicks = 0; refreshCardStates(); }

    // One travel for the whole console, recomputed from the window. A fixed
    // travel left a blank band under a tall window; an unbounded one gave the
    // old 835px meter. This grows to fill and stops, and because every column
    // gets the same number, the meter bridge still reads across.
    if (!m_strips.isEmpty()) {
        // Measured once, at the floor travel: how tall the *content* wants to
        // be when the faders are as short as they go. Everything the viewport
        // has above that is spare, and it goes into the travel.
        //
        // The content's hint, not the window's: the console lives in a
        // QScrollArea, whose own sizeHint is a fixed 576x431 and says nothing
        // about what is inside it.
        if (m_consoleChrome == 0 && m_travel == bbui::travel() && m_central)
            m_consoleChrome = m_central->sizeHint().height();
        const int room = m_scroll ? m_scroll->viewport()->height() : height();
        const int want = m_consoleChrome == 0
            ? bbui::travel()
            : qBound(bbui::travel(),
                     bbui::travel() + room - m_consoleChrome,
                     bbui::travelMax());
        if (want != m_travel) {
            m_travel = want;
            for (auto* s : m_strips) s->setTravel(want);
            for (auto* b : m_buses)  b->setTravel(want);
        }
        // Every card, not just the buses: the tallest pre-fader stack sets the
        // line and everything else is padded up to it.
        int top = 0;
        for (auto* w : m_strips) top = qMax(top, w->meterTop() - w->leadPad());
        for (auto* b : m_buses)  top = qMax(top, b->meterTop() - b->leadPad());
        for (auto* w : m_strips) w->setLeadPad(top - (w->meterTop() - w->leadPad()));
        for (auto* b : m_buses)  b->setLeadPad(top - (b->meterTop() - b->leadPad()));
    }
}
