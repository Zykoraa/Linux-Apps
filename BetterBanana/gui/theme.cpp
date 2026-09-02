#include "theme.h"
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>

static int g_index = 0;

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

QString buildStyleSheet(const Theme& t)
{
    auto c   = [](const QColor& x) { return x.name(QColor::HexRgb); };
    // Text that sits on top of a saturated "on" colour.
    const QString onText = t.dark ? "#12141a" : "#ffffff";

    QString s;
    s += QString("QWidget{color:%1;font-size:11px;}").arg(c(t.text));
    s += QString("QMainWindow,QDialog,QScrollArea,QScrollArea>QWidget>QWidget{background:%1;}")
            .arg(c(t.bg));

    s += QString("QGroupBox{background:transparent;border:1px solid %1;border-radius:6px;"
                 "margin-top:14px;padding-top:6px;font-weight:bold;color:%2;}")
            .arg(c(t.border), c(t.textDim));
    // Strips, buses and the tape deck are raised cards against the window.
    s += QString("QWidget[role=\"card\"]{background:%1;border:1px solid %2;border-radius:5px;}")
            .arg(c(t.panel), c(t.border));
    s += QString("QGroupBox::title{subcontrol-origin:margin;left:10px;padding:0 5px;color:%1;}")
            .arg(c(t.textDim));

    s += QString("QLabel{background:transparent;color:%1;}").arg(c(t.text));
    s += QString("QLabel[role=\"caption\"]{color:%1;font-size:9px;}").arg(c(t.textDim));
    s += QString("QLabel[role=\"value\"]{color:%1;font-size:9px;font-weight:bold;}").arg(c(t.accent));
    s += QString("QLabel[role=\"header\"]{background:%1;color:%2;font-weight:bold;font-size:9px;"
                 "padding:4px 2px;border-radius:3px;}").arg(c(t.header), c(t.text));
    s += QString("QLabel[role=\"headerA\"]{background:%1;color:%2;font-weight:bold;font-size:10px;"
                 "padding:4px 2px;border-radius:3px;}").arg(c(t.busA), onText);
    s += QString("QLabel[role=\"headerB\"]{background:%1;color:%2;font-weight:bold;font-size:10px;"
                 "padding:4px 2px;border-radius:3px;}").arg(c(t.busB), onText);
    s += QString("QLabel[role=\"gain\"]{color:%1;font-size:10px;font-weight:bold;}").arg(c(t.text));

    s += QString("QComboBox{background:%1;color:%2;border:1px solid %3;border-radius:3px;"
                 "padding:2px 4px;font-size:9px;}")
            .arg(c(t.panelAlt), c(t.text), c(t.border));
    s += QString("QComboBox:hover{border-color:%1;}").arg(c(t.accent));
    s += QString("QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%3;"
                 "selection-color:%4;border:1px solid %5;}")
            .arg(c(t.panelAlt), c(t.text), c(t.accent), onText, c(t.border));
    // Deliberately no QComboBox::drop-down rule: styling that subcontrol makes
    // the stylesheet engine own it, and it then never asks the style to draw an
    // arrow - which is how the drop-downs ended up looking like flat text
    // fields. createThemedStyle() paints the arrow instead.

    // Base toggle button, then one rule per role for the checked state.
    s += QString("QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:3px;"
                 "padding:2px 3px;font-size:9px;font-weight:bold;}")
            .arg(c(t.panelAlt), c(t.textDim), c(t.border));
    s += QString("QPushButton:hover{border-color:%1;color:%2;}").arg(c(t.accent), c(t.text));
    s += QString("QPushButton:pressed{background:%1;}").arg(c(t.header));

    struct RoleColour { const char* role; QColor col; };
    const RoleColour roles[] = {
        { "busA", t.busA }, { "busB", t.busB }, { "mute", t.mute }, { "solo", t.solo },
        { "mono", t.mono }, { "eq",   t.eqOn }, { "rec",  t.rec  }, { "accent", t.accent },
    };
    for (const auto& r : roles) {
        s += QString("QPushButton[role=\"%1\"]:checked{background:%2;color:%3;border-color:%2;}")
                .arg(r.role, c(r.col), onText);
        s += QString("QPushButton[role=\"%1\"]:checked:hover{background:%2;}")
                .arg(r.role, c(r.col.lighter(115)));
    }

    s += QString("QSlider::groove:vertical{background:%1;width:5px;border-radius:2px;}")
            .arg(c(t.panelAlt));
    s += QString("QSlider::sub-page:vertical{background:%1;border-radius:2px;}").arg(c(t.panelAlt));
    s += QString("QSlider::add-page:vertical{background:%1;border-radius:2px;}").arg(c(t.panelAlt));
    s += QString("QSlider::handle:vertical{background:%1;border:1px solid %2;height:11px;"
                 "margin:0 -7px;border-radius:2px;}")
            .arg(c(t.text), c(t.border));
    s += QString("QSlider::handle:vertical:hover{background:%1;border-color:%1;}").arg(c(t.accent));

    s += QString("QMenuBar{background:%1;color:%2;}").arg(c(t.bg), c(t.text));
    s += QString("QMenuBar::item:selected{background:%1;color:%2;}").arg(c(t.accent), onText);
    s += QString("QMenu{background:%1;color:%2;border:1px solid %3;}")
            .arg(c(t.panel), c(t.text), c(t.border));
    s += QString("QMenu::item:selected{background:%1;color:%2;}").arg(c(t.accent), onText);
    s += QString("QStatusBar{background:%1;color:%2;}").arg(c(t.bg), c(t.textDim));
    s += QString("QScrollBar:horizontal,QScrollBar:vertical{background:%1;border:0;}").arg(c(t.panelAlt));
    s += QString("QScrollBar::handle{background:%1;border-radius:3px;}").arg(c(t.border));
    s += QString("QToolTip{background:%1;color:%2;border:1px solid %3;}")
            .arg(c(t.panel), c(t.text), c(t.border));
    s += QString("QLineEdit{background:%1;color:%2;border:1px solid %3;border-radius:3px;"
                 "padding:1px 3px;font-size:9px;}")
            .arg(c(t.panelAlt), c(t.text), c(t.border));
    s += QString("QLineEdit:focus{border-color:%1;}").arg(c(t.accent));
    // Font only, deliberately. Giving a spin box a background or border here
    // makes the stylesheet engine take the widget over, and it then draws no
    // step arrows at all - and unlike a combo's, those cannot be handed back to
    // the style. The palette above already colours it to match the theme.
    s += "QAbstractSpinBox{font-size:9px;}";
    s += QString("QCheckBox{color:%1;font-size:9px;}").arg(c(t.text));
    s += QString("QFrame[role=\"sep\"]{color:%1;}").arg(c(t.border));
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
    const QColor onAccent = t.dark ? QColor("#12141a") : QColor("#ffffff");
    QPalette p;
    p.setColor(QPalette::Window,          t.bg);
    p.setColor(QPalette::WindowText,      t.text);
    p.setColor(QPalette::Base,            t.panelAlt);
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
