#include "theme.h"
#include "color.h"
#include "metrics.h"
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>

static int g_index = 0;

static QColor deriveWell(const QColor& panel, bool dark);

// Palettes are transcribed from each project's published colour definitions.
const QVector<Theme>& builtinThemes()
{
    static QVector<Theme> v;
    if (!v.isEmpty()) return v;

    auto mk = [](const char* name, bool dark,
                 const char* bg, const char* panel, const char* panelAlt, const char* header,
                 const char* border, const char* text, const char* textDim, const char* accent,
                 const char* busA, const char* busB, const char* mute, const char* solo,
                 const char* mono, const char* eqOn, const char* rec,
                 const char* mLow, const char* mMid, const char* mHigh, const char* mPeak,
                 const char* mHold) {
        Theme t;
        t.name = name; t.dark = dark;
        t.bg = QColor(bg); t.panel = QColor(panel); t.panelAlt = QColor(panelAlt);
        t.header = QColor(header); t.border = QColor(border);
        t.well = deriveWell(t.panel, dark);
        t.text = QColor(text); t.textDim = QColor(textDim); t.accent = QColor(accent);
        t.busA = QColor(busA); t.busB = QColor(busB); t.mute = QColor(mute);
        t.solo = QColor(solo); t.mono = QColor(mono); t.eqOn = QColor(eqOn); t.rec = QColor(rec);
        t.meterLow = QColor(mLow); t.meterMid = QColor(mMid); t.meterHigh = QColor(mHigh);
        t.meterPeak = QColor(mPeak); t.meterHold = QColor(mHold);
        return t;
    };

    // name              dark  bg        panel     panelAlt  header    border    text      textDim   accent    busA      busB      mute      solo      mono      eqOn      rec       mLow      mMid      mHigh     mPeak     mHold
    v << mk("BetterBanana Dark", true,
            "#15181d","#1f242c","#12151a","#2b3038","#333a45","#dfe6f0","#7d8a99","#6ee7a8",
            "#6ee7a8","#8fd0ff","#ff6b6b","#ffd24a","#7fb2ff","#c9a7ff","#ff5c5c",
            "#3ccf7a","#d8c65a","#e8903c","#e63f3f","#e8eef7");
    v << mk("Catppuccin Mocha", true,
            "#1e1e2e","#313244","#181825","#45475a","#585b70","#cdd6f4","#a6adc8","#89b4fa",
            "#a6e3a1","#89b4fa","#f38ba8","#f9e2af","#89dceb","#cba6f7","#eba0ac",
            "#a6e3a1","#f9e2af","#fab387","#f38ba8","#f5e0dc");

    v << mk("Catppuccin Latte", false,
            "#eff1f5","#e6e9ef","#dce0e8","#ccd0da","#bcc0cc","#4c4f69","#6c6f85","#1e66f5",
            "#40a02b","#1e66f5","#d20f39","#df8e1d","#209fb5","#8839ef","#e64553",
            "#40a02b","#df8e1d","#fe640b","#d20f39","#4c4f69");

    v << mk("Everforest Dark", true,
            "#2d353b","#3d484d","#272e33","#475258","#4f585e","#d3c6aa","#859289","#a7c080",
            "#a7c080","#7fbbb3","#e67e80","#dbbc7f","#83c092","#d699b6","#e67e80",
            "#a7c080","#dbbc7f","#e69875","#e67e80","#d3c6aa");

    v << mk("Nord", true,
            "#2e3440","#3b4252","#272c36","#434c5e","#4c566a","#eceff4","#a9b3c4","#88c0d0",
            "#a3be8c","#81a1c1","#bf616a","#ebcb8b","#8fbcbb","#b48ead","#bf616a",
            "#a3be8c","#ebcb8b","#d08770","#bf616a","#eceff4");

    v << mk("Gruvbox Dark", true,
            "#282828","#3c3836","#1d2021","#504945","#665c54","#ebdbb2","#a89984","#8ec07c",
            "#b8bb26","#83a598","#fb4934","#fabd2f","#8ec07c","#d3869b","#fb4934",
            "#b8bb26","#fabd2f","#fe8019","#fb4934","#ebdbb2");

    v << mk("Tokyo Night", true,
            "#1a1b26","#24283b","#16161e","#292e42","#3b4261","#c0caf5","#7f88b0","#7aa2f7",
            "#9ece6a","#7dcfff","#f7768e","#e0af68","#7aa2f7","#bb9af7","#f7768e",
            "#9ece6a","#e0af68","#ff9e64","#f7768e","#c0caf5");

    v << mk("Dracula", true,
            "#282a36","#343746","#21222c","#44475a","#565a6f","#f8f8f2","#9aa0c0","#bd93f9",
            "#50fa7b","#8be9fd","#ff5555","#f1fa8c","#8be9fd","#bd93f9","#ff5555",
            "#50fa7b","#f1fa8c","#ffb86c","#ff5555","#f8f8f2");
    v << mk("Rose Pine", true,
            "#191724","#1f1d2e","#14121f","#26233a","#403d52","#e0def4","#908caa","#c4a7e7",
            "#9ccfd8","#c4a7e7","#eb6f92","#f6c177","#31748f","#c4a7e7","#eb6f92",
            "#9ccfd8","#f6c177","#ebbcba","#eb6f92","#e0def4");

    v << mk("Solarized Dark", true,
            "#002b36","#073642","#00212b","#0b4451","#586e75","#93a1a1","#657b83","#2aa198",
            "#859900","#268bd2","#dc322f","#b58900","#2aa198","#6c71c4","#dc322f",
            "#859900","#b58900","#cb4b16","#dc322f","#93a1a1");

    return v;
}

const Theme& theme()
{
    const auto& all = builtinThemes();
    return all.at(qBound(0, g_index, all.size() - 1));
}

void setThemeIndex(int i) { g_index = qBound(0, i, builtinThemes().size() - 1); }
int  themeIndex()        { return g_index; }

// A recess against the card it sits in. Derived, so the ten palettes need no
// hand-editing and an eleventh gets a correct trough for free. panelAlt used to
// do this job as well as being the face of every button and combo - one value
// with two opposite meanings is why nothing looked pressable and nothing looked
// inset.
static QColor deriveWell(const QColor& panel, bool dark)
{
    QColor w = bbcolor::nudge(panel, dark ? -6.0 : -7.0);
    for (int i = 0; i < 14 && bbcolor::contrast(w, panel) < 1.28; ++i)
        w = bbcolor::nudge(w, -2.0);
    // Near the black end of the range there is no room left below, and every
    // dark UI answers that the same way: the trough goes up, not down.
    if (bbcolor::contrast(w, panel) < 1.18) {
        w = panel;
        for (int i = 0; i < 14 && bbcolor::contrast(w, panel) < 1.28; ++i)
            w = bbcolor::nudge(w, 2.0);
    }
    return w;
}

QColor onFill(const QColor& fill) { return bbcolor::onColor(fill); }

// Five buses, five chips you can tell apart - without breaking "A is green,
// B is blue", which is the thing the colours are for.
//
// All three A-chips used to be one colour and both B-chips another, so an
// assign row said how many buses a strip fed but never which. Rotating the hue
// per bus fixes that and costs the family: pushed far enough apart to be
// legible at 18px, Mocha's A3 came out orange, which says the wrong thing.
//
// So the family keeps its hue and the members differ in lightness, which the
// eye separates readily and which cannot leave the family. The offsets are then
// searched outward from those defaults, because a lightness step can walk a
// chip into `mono` or `eqOn` sitting on the same card - Everforest's B1 landed
// 2.6 dE from mono. tests/test_contrast.cpp holds both properties.
QColor busChipColour(const Theme& t, int bus)
{
    constexpr int kChips = 5, kPhys = 3;      // A1..A3, B1..B2
    bus = qBound(0, bus, kChips - 1);

    const QColor roles[] = {
        bbcolor::fitFill(t.mute, bbcolor::kTextFloor),
        bbcolor::fitFill(t.solo, bbcolor::kTextFloor),
        bbcolor::fitFill(t.mono, bbcolor::kTextFloor),
        bbcolor::fitFill(t.eqOn, bbcolor::kTextFloor),
        bbcolor::fitFill(t.rec,  bbcolor::kTextFloor),
    };
    static const double kStep[kChips] = { 0.0, 15.0, -15.0, 0.0, 16.0 };

    auto make = [&](int b, double extra) {
        const QColor base = b < kPhys ? t.busA : t.busB;
        return bbcolor::fitFill(bbcolor::nudge(base, kStep[b] + extra),
                                bbcolor::kTextFloor);
    };

    QColor chosen[kChips];
    for (int b = 0; b <= bus; ++b) {
        double bestScore = -1.0, bestExtra = 0.0;
        bool done = false;
        for (double mag = 0.0; mag <= 26.0 && !done; mag += 2.0) {
            for (int sign : { 1, -1 }) {
                if (mag == 0.0 && sign < 0) continue;
                const double extra = mag * sign;
                const QColor c = make(b, extra);
                double worst = 1e9;
                for (const QColor& r : roles) worst = qMin(worst, bbcolor::deltaE(c, r));
                for (int o = 0; o < b; ++o) worst = qMin(worst, bbcolor::deltaE(c, chosen[o]));
                if (worst > bestScore) { bestScore = worst; bestExtra = extra; }
                if (worst >= 12.0) { done = true; break; }
            }
        }
        chosen[b] = make(b, bestExtra);
    }
    return chosen[bus];
}

QColor dimOn(const Theme& t, const QColor& bg)
{
    return bbcolor::ensureContrast(t.textDim, bg, bbcolor::kTextFloor);
}

QString buildStyleSheet(const Theme& t)
{
    auto c = [](const QColor& x) { return x.name(QColor::HexRgb); };
    using namespace bbui;

    // Captions land on bg, on a card and on a control face. Solve dim text
    // against the worst of the three rather than picking one and hoping - it
    // used to fail 4.5:1 on `panel` in six themes, and captions are the only
    // thing naming half these controls.
    QColor worst = t.bg;
    double lo = bbcolor::contrast(t.textDim, t.bg);
    for (const QColor& s : { t.panel, t.panelAlt, t.well }) {
        const double r = bbcolor::contrast(t.textDim, s);
        if (r < lo) { lo = r; worst = s; }
    }
    const QString dim  = c(dimOn(t, worst));
    // Ink that is only ever disabled, and boundaries that must still be seen.
    const QString off  = c(bbcolor::mix(dimOn(t, t.panel), t.panel, 0.45));
    const QString edge = c(bbcolor::ensureContrast(t.border, t.well, bbcolor::kBoundFloor));

    const QString onAccent = c(onFill(t.accent));

    QString s;
    s += QString("QWidget{color:%1;font-size:%2px;}").arg(c(t.text)).arg(fsControl());
    s += QString("QMainWindow,QDialog,QScrollArea,QScrollArea>QWidget>QWidget{background:%1;}")
            .arg(c(t.bg));

    // --- surfaces ---------------------------------------------------------
    s += QString("QGroupBox{background:transparent;border:1px solid %1;border-radius:%2px;"
                 "margin-top:%3px;padding-top:%4px;font-size:%5px;font-weight:bold;color:%6;}")
            .arg(c(t.border)).arg(radCard()).arg(px(14)).arg(px(6)).arg(fsControl()).arg(dim);
    s += QString("QGroupBox::title{subcontrol-origin:margin;left:%1px;padding:0 %2px;color:%3;}")
            .arg(px(10)).arg(px(5)).arg(dim);
    s += QString("QGroupBox:disabled{border-color:%1;color:%1;}").arg(off);

    // Strips, buses and the tape deck are raised cards against the window.
    // WA_StyledBackground has to be set on each of them for Qt to honour this -
    // a moc'd QWidget subclass never gets it automatically, which is why this
    // rule painted nothing at all for the whole life of the app.
    s += QString("QWidget[role=\"card\"]{background:%1;border:1px solid %2;border-radius:%3px;}")
            .arg(c(t.panel), c(t.border)).arg(radCard());
    // Virtual inputs and virtual buses carry the distinction in the edge, not
    // the fill: panelAlt is the face of every button and combo sitting on them,
    // so a second plate colour would swallow its own controls.
    s += QString("QWidget[role=\"cardVirtual\"]{background:%1;border:1px solid %2;"
                 "border-radius:%3px;}").arg(c(t.panel), c(t.busB)).arg(radCard());

    // A strip that is not soloed while something else is, and a strip whose
    // device is named but not attached. Both are states the engine already
    // knew about and the window never showed.
    // A strip that a solo elsewhere has silenced. The first attempt blended
    // toward bg and measured 1.03:1 against an undimmed card - which is to say
    // it showed nothing. This steps away from the card in the direction there
    // is room, and marks the edge in the solo colour so the reason is legible
    // and not only the effect.
    {
        const QColor dimCard = bbcolor::nudge(t.panel, t.dark ? -7.0 : 7.0);
        s += QString("QWidget[role=\"card\"][dim=\"true\"],"
                     "QWidget[role=\"cardVirtual\"][dim=\"true\"]"
                     "{background:%1;border-color:%2;}")
                .arg(c(dimCard), c(bbcolor::mix(t.solo, t.panel, 0.55)));
        // Deliberately no descendant rule for the labels inside a dimmed card.
        // A "QWidget[role=...] QLabel" selector makes QStyleSheetStyle take
        // over every label in every strip, and it then computes their size
        // hints through the stylesheet box model - which widened the card's
        // layout minimum from 140px to 166px, for all ten of them, whether or
        // not anything was ever soloed. The plate and the edge carry the state.
    }
    s += QString("QLabel[role=\"header\"][nodev=\"true\"],"
                 "QLabel[role=\"headerA\"][nodev=\"true\"]"
                 "{background:%1;color:%2;}")
            .arg(c(bbcolor::mix(t.mute, t.panel, 0.62)), c(t.text));
    {
        const QColor a = bbcolor::fitFill(t.mute, bbcolor::kTextFloor);
        s += QString("QLabel[role=\"alert\"]{background:%1;color:%2;font-size:%3px;"
                     "font-weight:bold;padding:6px 10px;border-radius:%4px;}")
                .arg(c(a), c(onFill(a))).arg(fsBody()).arg(radCtl());
    }

    // --- text -------------------------------------------------------------
    s += QString("QLabel{background:transparent;color:%1;}").arg(c(t.text));
    s += QString("QLabel[role=\"caption\"]{color:%1;font-size:%2px;}").arg(dim).arg(fsCaption());
    // Three five-letter words share a knob grid barely a hundred pixels wide,
    // directly above the values they name - so they can afford a step down
    // where a free-standing caption cannot.
    s += QString("QLabel[role=\"knobcap\"]{color:%1;font-size:%2px;}").arg(dim).arg(fsTiny());
    s += QString("QLabel[role=\"value\"]{color:%1;font-size:%2px;font-weight:bold;}")
            .arg(c(bbcolor::ensureContrast(t.accent, t.panel, bbcolor::kTextFloor)))
            .arg(fsCaption());
    {
        const QColor plate = bbcolor::fitFill(t.header, bbcolor::kTextFloor);
        s += QString("QLabel[role=\"header\"]{background:%1;color:%2;font-weight:bold;"
                     "font-size:%3px;padding:4px 2px;border-radius:%4px;}")
                .arg(c(plate), c(onFill(plate))).arg(fsControl()).arg(radCtl());
    }
    // Bus plates used to be a saturated slab with hard-coded ink on top - four
    // of the eight AA failures in the app lived here. The identity moves to the
    // edge and the label goes back to ordinary card text.
    for (const char* r : { "headerA", "headerB" })
        s += QString("QLabel[role=\"%1\"]{background:%2;color:%3;font-weight:bold;font-size:%4px;"
                     "padding:%5px %6px %5px %7px;border-radius:%8px;border-left:%9px solid %10;}")
                .arg(r, c(t.panelAlt), c(t.text)).arg(fsControl())
                .arg(px(4)).arg(px(2)).arg(px(6)).arg(radCtl()).arg(px(3))
                .arg(c(QLatin1String(r) == QLatin1String("headerA") ? t.busA : t.busB));
    s += QString("QLabel[role=\"gain\"]{color:%1;font-size:%2px;font-weight:bold;}")
            .arg(c(t.text)).arg(fsReadout());
    s += QString("QLabel[role=\"display\"]{color:%1;font-size:%2px;font-weight:bold;}")
            .arg(c(t.text)).arg(fsDisplay());
    s += QString("QLabel[role=\"prose\"]{color:%1;font-size:%2px;}").arg(dim).arg(fsBody());
    s += QString("QLabel[role=\"tag\"]{background:%1;color:%2;font-size:%3px;font-weight:bold;"
                 "padding:%5px %6px;border-radius:%4px;}")
            .arg(c(t.well), dim).arg(fsCaption()).arg(radWell()).arg(px(1)).arg(px(4));
    s += QString("QLabel:disabled{color:%1;}").arg(off);

    // --- device pickers ---------------------------------------------------
    s += QString("QComboBox{background:%1;color:%2;border:1px solid %3;border-radius:%4px;"
                 "padding:%6px %7px;font-size:%5px;}")
            .arg(c(t.panelAlt), c(t.text), c(t.border)).arg(radCtl()).arg(fsControl())
            .arg(px(1)).arg(px(4));
    s += QString("QComboBox:hover{border-color:%1;}").arg(c(t.accent));
    s += QString("QComboBox:focus{border-color:%1;outline:1px solid %1;}").arg(c(t.accent));
    s += QString("QComboBox:disabled{color:%1;border-color:%1;}").arg(off);
    // The assigned device is named in the state but is not on the system.
    s += QString("QComboBox[bad=\"true\"]{border-color:%1;color:%2;}")
            .arg(c(t.mute), c(bbcolor::ensureContrast(t.mute, t.panelAlt, bbcolor::kTextFloor)));
    // The popup used to jump from 9px to 11px the moment it opened, a 22% step
    // on every device list. Same size as the closed control, set on the view
    // selector that already existed - the ::drop-down subcontrol stays
    // untouched, so the style keeps drawing the arrow.
    s += QString("QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%3;"
                 "selection-color:%4;border:1px solid %5;font-size:%6px;padding:2px;}")
            .arg(c(t.panelAlt), c(t.text), c(t.accent), onAccent, c(t.border)).arg(fsControl());

    // --- buttons ----------------------------------------------------------
    //
    // Two populations, one rule each. Prose buttons ("Calibrate to my voice...",
    // "Save as...") were being set in 9px bold caps-styled chrome because the
    // three-letter mixer chips and they shared a single rule. makeToggle()
    // always sets a role property and no prose button anywhere does, so the
    // split is free.
    s += QString("QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:%4px;"
                 "padding:%6px %7px;font-size:%5px;font-weight:400;}")
            .arg(c(t.panelAlt), c(t.text), c(t.border)).arg(radCtl()).arg(fsBody())
            .arg(px(5)).arg(px(12));
    s += QString("QPushButton:hover{border-color:%1;}").arg(c(t.accent));
    s += QString("QPushButton:pressed{background:%1;}").arg(c(t.header));
    s += QString("QPushButton:focus{outline:1px solid %1;}").arg(c(t.accent));
    s += QString("QPushButton:disabled{color:%1;border-color:%1;}").arg(off);

    s += QString("QPushButton[role]{padding:2px 3px;font-size:%1px;font-weight:bold;color:%2;}")
            .arg(fsChip()).arg(dim);
    s += QString("QPushButton[role]:hover{border-color:%1;color:%2;}")
            .arg(c(t.accent), c(t.text));
    s += QString("QPushButton[role]:disabled{color:%1;border-color:%1;}").arg(off);
    // Five of these across one card, beside a fader and a meter.
    s += QString("QPushButton[bus]{padding:2px 1px;font-size:%1px;}").arg(fsTiny());

    struct RoleColour { const char* role; QColor col; };
    const RoleColour roles[] = {
        { "busA", t.busA }, { "busB", t.busB }, { "mute", t.mute }, { "solo", t.solo },
        { "mono", t.mono }, { "eq",   t.eqOn }, { "rec",  t.rec  }, { "accent", t.accent },
        // The off half of the record indicator's pulse. Recording is the one
        // state in this app that is destructive to get wrong, and a static red
        // chip looks the same as a chip that is merely armed.
        { "recdim", bbcolor::mix(t.rec, t.panel, 0.55) },
    };
    for (const auto& r0 : roles) {
        // A lit chip is a fill and its ink together; the pair has to clear the
        // floor, and where no ink can, the fill moves.
        const RoleColour r{ r0.role, bbcolor::fitFill(r0.col, bbcolor::kTextFloor) };
        s += QString("QPushButton[role=\"%1\"]:checked{background:%2;color:%3;border-color:%2;}")
                .arg(r.role, c(r.col), c(onFill(r.col)));
        // lighter() clamps once a channel is at 255, which left a checked MUTE,
        // SOLO, MONO, EQ and REC with no hover feedback at all in the default
        // theme. A Lab step always moves, in whichever direction there is room.
        s += QString("QPushButton[role=\"%1\"]:checked:hover{background:%2;color:%3;}")
                .arg(r.role, c(bbcolor::hoverOf(r.col)), c(onFill(bbcolor::hoverOf(r.col))));
        // A checked chip out-specifies a bare :disabled, so it needs its own.
        s += QString("QPushButton[role=\"%1\"]:checked:disabled{background:%2;color:%3;"
                     "border-color:%2;}")
                .arg(r.role, c(bbcolor::mix(r.col, t.panel, 0.55)), off);
    }

    // Five buses, five hues. All A-buses used to share one colour and all
    // B-buses another, so an assign row said how many buses a strip fed but
    // never which.
    for (int b = 0; b < 5; ++b) {
        QColor h = busChipColour(t, b);
        s += QString("QPushButton[bus=\"%1\"]:checked{background:%2;color:%3;"
                     "border-color:%2;}")
                .arg(b).arg(c(h), c(onFill(h)));
        s += QString("QPushButton[bus=\"%1\"]:checked:hover{background:%2;color:%3;}")
                .arg(b).arg(c(bbcolor::hoverOf(h)), c(onFill(bbcolor::hoverOf(h))));
    }

    // Call-to-action weight, so Delete stops looking exactly like Export.
    s += QString("QPushButton[cta=\"primary\"]{background:%1;color:%2;border-color:%1;"
                 "font-weight:bold;}").arg(c(t.accent), onAccent);
    s += QString("QPushButton[cta=\"primary\"]:hover{background:%1;}")
            .arg(c(bbcolor::hoverOf(t.accent)));
    s += QString("QPushButton[cta=\"danger\"]{color:%1;border-color:%1;}")
            .arg(c(bbcolor::ensureContrast(t.mute, t.panelAlt, bbcolor::kTextFloor)));
    s += QString("QPushButton[cta=\"danger\"]:hover{background:%1;color:%2;border-color:%1;}")
            .arg(c(t.mute), c(onFill(t.mute)));
    // A cta selector out-specifies a bare :disabled, so without these a
    // disabled primary button went on painting itself as a lit call to action -
    // which two dialogs had to work around locally because they could not
    // reach this file.
    s += QString("QPushButton[cta=\"primary\"]:disabled{background:%1;color:%2;border-color:%1;}")
            .arg(c(bbcolor::mix(t.accent, t.panel, 0.6)), off);
    s += QString("QPushButton[cta=\"danger\"]:disabled{color:%1;border-color:%1;background:%2;}")
            .arg(off, c(t.panelAlt));

    // --- check boxes ------------------------------------------------------
    //
    // Qt draws ::indicator from QPalette::Base, which is panelAlt: measured
    // 1.03:1 against a dialog. That is the EQ band table's entire ON column,
    // the VBAN dialog and the AutoEq browser - the only on/off control in the
    // app that is not a coloured chip, invisible in exactly the place it is the
    // only mechanism. Styling the subcontrol means Qt stops drawing a tick, so
    // a checked box becomes a filled accent swatch: correct here, it matches
    // the app's own filled-chip vocabulary and needs no bundled asset.
    s += QString("QCheckBox{color:%1;font-size:%2px;spacing:6px;}")
            .arg(c(t.text)).arg(fsControl());
    s += QString("QCheckBox::indicator{width:%4px;height:%4px;border:1px solid %1;"
                 "border-radius:%2px;background:%3;}")
            .arg(edge).arg(radWell()).arg(c(t.well)).arg(px(13));
    s += QString("QCheckBox::indicator:hover{border-color:%1;}").arg(c(t.accent));
    s += QString("QCheckBox::indicator:checked{background:%1;border-color:%1;}")
            .arg(c(t.accent));
    s += QString("QCheckBox::indicator:disabled{border-color:%1;background:%2;}")
            .arg(off).arg(c(t.panel));
    s += QString("QCheckBox:disabled{color:%1;}").arg(off);

    // --- sliders (the plain Qt ones; Fader is custom-painted) --------------
    s += QString("QSlider::groove:vertical{background:%1;width:5px;border-radius:%2px;}")
            .arg(c(t.well)).arg(radWell());
    s += QString("QSlider::sub-page:vertical,QSlider::add-page:vertical{background:%1;"
                 "border-radius:%2px;}").arg(c(t.well)).arg(radWell());
    s += QString("QSlider::handle:vertical{background:%1;border:1px solid %2;height:11px;"
                 "margin:0 -7px;border-radius:%3px;}")
            .arg(c(t.text), c(t.border)).arg(radWell());
    s += QString("QSlider::handle:vertical:hover{background:%1;border-color:%1;}")
            .arg(c(t.accent));

    // --- menus ------------------------------------------------------------
    //
    // The menu bar is the app's entire navigation: the app router, both EQs,
    // the voice changer, ducking, VBAN, the analyser and both autostart
    // switches are reachable nowhere else.
    s += QString("QMenuBar{background:%1;color:%2;border-bottom:1px solid %3;}")
            .arg(c(t.bg), c(t.text), c(t.border));
    s += QString("QMenuBar::item{padding:%1px %2px;background:transparent;}")
            .arg(px(4)).arg(px(9));
    s += QString("QMenuBar::item:selected{background:%1;color:%2;}").arg(c(t.accent), onAccent);
    s += QString("QMenu{background:%1;color:%2;border:1px solid %3;padding:4px;}")
            .arg(c(t.panel), c(t.text), c(t.border));
    s += QString("QMenu::item{padding:%3px %4px;border-radius:%1px;font-size:%2px;}")
            .arg(radCtl()).arg(fsBody()).arg(px(5)).arg(px(26));
    s += QString("QMenu::item:selected{background:%1;color:%2;}").arg(c(t.accent), onAccent);
    s += QString("QMenu::item:disabled{color:%1;}").arg(off);
    // Fusion's own separator is a 3-D etched groove that reads as damage on a
    // flat palette.
    s += QString("QMenu::separator{height:1px;background:%1;margin:4px 8px;}").arg(c(t.border));
    s += QString("QMenu::indicator{width:%1px;height:%1px;}").arg(px(13));

    // --- status bar -------------------------------------------------------
    // Seventeen showMessage() calls carry the app's whole feedback vocabulary,
    // and they used to land as dim text on the window colour.
    s += QString("QStatusBar{background:%1;color:%2;border-top:1px solid %3;}")
            .arg(c(t.panel), c(t.text), c(t.border));
    s += QString("QStatusBar::item{border:0;}");
    s += QString("QStatusBar QLabel{color:%1;font-size:%2px;}").arg(dim).arg(fsCaption());

    // --- scroll bars ------------------------------------------------------
    // Handles were t.border on panelAlt: 1.37:1 to 3.12:1, failing 3:1 in nine
    // of ten themes, with no width, no minimum length, and the platform's
    // stepper arrows still drawn at both ends.
    s += QString("QScrollBar:vertical{background:%1;border:0;width:%2px;margin:0;}")
            .arg(c(t.well)).arg(px(11));
    s += QString("QScrollBar:horizontal{background:%1;border:0;height:%2px;margin:0;}")
            .arg(c(t.well)).arg(px(11));
    s += QString("QScrollBar::handle:vertical{background:%1;border-radius:%2px;min-height:%3px;"
                 "margin:%4px;}").arg(edge).arg(px(3)).arg(px(28)).arg(px(2));
    s += QString("QScrollBar::handle:horizontal{background:%1;border-radius:%2px;min-width:%3px;"
                 "margin:%4px;}").arg(edge).arg(px(3)).arg(px(28)).arg(px(2));
    s += QString("QScrollBar::handle:hover{background:%1;}").arg(c(t.accent));
    s += QString("QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;border:0;"
                 "background:none;}");
    s += QString("QScrollBar::add-page,QScrollBar::sub-page{background:none;}");

    // --- misc -------------------------------------------------------------
    s += QString("QToolTip{background:%1;color:%2;border:1px solid %3;padding:%5px %6px;"
                 "font-size:%4px;}")
            .arg(c(t.panel), c(t.text), c(t.border)).arg(fsBody()).arg(px(3)).arg(px(6));
    s += QString("QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:%4px;"
                 "padding:%6px %7px;font-size:%5px;}")
            .arg(c(t.well), c(t.text), c(t.border)).arg(radCtl()).arg(fsControl())
            .arg(px(3)).arg(px(5));
    s += QString("QLineEdit:focus{border-color:%1;}").arg(c(t.accent));
    s += QString("QLineEdit:disabled{color:%1;border-color:%1;}").arg(off);
    // The field that could not be opened marks itself.
    s += QString("QLineEdit[bad=\"true\"]{border-color:%1;color:%2;}")
            .arg(c(t.mute), c(bbcolor::ensureContrast(t.mute, t.well, bbcolor::kTextFloor)));
    // Font and colour only, deliberately. Giving a spin box a background or a
    // border makes the stylesheet engine take the widget over, and it then
    // draws no step arrows at all - and unlike a combo's, those cannot be
    // handed back to the style. A colour-only rule is safe; verified.
    s += QString("QAbstractSpinBox{font-size:%1px;}").arg(fsControl());
    s += QString("QAbstractSpinBox:disabled{color:%1;}").arg(off);
    s += QString("QFrame[role=\"sep\"]{color:%1;}").arg(c(t.border));
    s += QString("QSplitter::handle{background:%1;}").arg(c(t.bg));
    s += QString("QSplitter::handle:hover{background:%1;}").arg(c(t.accent));
    s += QString("QListWidget{background:%1;border:1px solid %2;border-radius:%3px;}")
            .arg(c(t.well), c(t.border)).arg(radCtl());
    s += QString("QListWidget::item{padding:%2px %3px;border-radius:%1px;}")
            .arg(radWell()).arg(px(4)).arg(px(6));
    s += QString("QListWidget::item:selected{background:%1;color:%2;}").arg(c(t.accent), onAccent);
    s += QString("QProgressBar{background:%1;border:0;border-radius:%2px;height:%3px;"
                 "text-align:center;}").arg(c(t.well)).arg(radWell()).arg(px(6));
    s += QString("QProgressBar::chunk{background:%1;border-radius:%2px;}")
            .arg(c(t.accent)).arg(radWell());
    return s;
}
// ---------------------------------------------------------------------------
// Arrow painting.
//
// Qt gives no way to tint a stylesheet arrow per theme, and suppresses its own
// arrow as soon as the subcontrol is styled. Drawing it here reads theme() at
// paint time, so it follows a theme switch with no work and stays legible on
// the light theme as well as the nine dark ones.
// ---------------------------------------------------------------------------
namespace {

class ThemedStyle : public QProxyStyle {
public:
    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt,
                       QPainter* p, const QWidget* w) const override
    {
        if (pe != PE_IndicatorArrowDown && pe != PE_IndicatorArrowUp) {
            QProxyStyle::drawPrimitive(pe, opt, p, w);
            return;
        }
        const Theme& t = theme();
        const bool hot = opt->state & State_MouseOver;
        const QRect r = opt->rect;

        // A small, flat triangle - no button chrome, to match the rest of the UI.
        const int width = qMin(9, qMax(6, qMin(r.width(), r.height()) - 2));
        QRectF box(0, 0, width, width * 0.55);
        box.moveCenter(QRectF(r).center());

        QPolygonF tri;
        if (pe == PE_IndicatorArrowDown)
            tri << box.topLeft() << box.topRight()
                << QPointF(box.center().x(), box.bottom());
        else
            tri << box.bottomLeft() << box.bottomRight()
                << QPointF(box.center().x(), box.top());

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(Qt::NoPen);
        p->setBrush(hot ? t.accent : t.textDim);
        p->drawPolygon(tri);
        p->restore();
    }
};

} // namespace

// A null base style makes QProxyStyle defer to the application's default, so
// whatever the desktop selected (Breeze, Fusion, qt6ct's pick) still shows
// through everywhere except the arrows.
QStyle* createThemedStyle() { return new ThemedStyle; }

// See the header: this covers everything the stylesheet cannot reach.
QPalette themePalette(const Theme& t)
{
    const QColor onAccent = onFill(t.accent);
    QPalette p;
    p.setColor(QPalette::Window,          t.bg);
    p.setColor(QPalette::WindowText,      t.text);
    p.setColor(QPalette::Base,            t.well);
    p.setColor(QPalette::AlternateBase,   t.panel);
    p.setColor(QPalette::Text,            t.text);
    p.setColor(QPalette::Button,          t.panelAlt);
    p.setColor(QPalette::ButtonText,      t.text);
    p.setColor(QPalette::BrightText,      t.rec);
    p.setColor(QPalette::Highlight,       t.accent);
    p.setColor(QPalette::HighlightedText, onAccent);
    p.setColor(QPalette::ToolTipBase,     t.panel);
    p.setColor(QPalette::ToolTipText,     t.text);
    p.setColor(QPalette::PlaceholderText, t.textDim);
    // Frame shading. Qt derives 3-D edges from these, so leaving them at the
    // desktop's values outlines every native widget in the wrong colour.
    p.setColor(QPalette::Light,           t.panel);
    p.setColor(QPalette::Midlight,        t.header);
    p.setColor(QPalette::Mid,             t.border);
    p.setColor(QPalette::Dark,            t.header);
    p.setColor(QPalette::Shadow,          t.bg);
    p.setColor(QPalette::Disabled, QPalette::Text,       t.textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.textDim);
    return p;
}
