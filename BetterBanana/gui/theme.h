// betterbanana GUI - colour themes.
//
// One global palette drives everything. Plain Qt widgets are styled through an
// application-wide stylesheet keyed on dynamic properties (role="mute" etc.),
// so switching themes is a single setStyleSheet() call; the custom-painted
// widgets read theme() directly in their paintEvent.
#pragma once

#include <QColor>
#include <QPalette>
#include <QString>
#include <QVector>

struct Theme {
    QString name;

    QColor bg;         // window background
    QColor panel;      // strip / group background - the card plane
    QColor panelAlt;   // raised control faces: buttons, combos, line edits
    QColor well;       // recessed: troughs, knob tracks, grooves, plot grounds
    QColor header;     // strip title plate
    QColor border;

    QColor text;
    QColor textDim;

    QColor accent;     // primary highlight (knob arcs, focus)
    QColor busA;       // A1..A3 assign buttons
    QColor busB;       // B1/B2 assign buttons
    QColor mute;
    QColor solo;
    QColor mono;
    QColor eqOn;
    QColor rec;        // record-armed / recording

    QColor meterLow;   // <= -18 dB
    QColor meterMid;   // -18 .. -6
    QColor meterHigh;  // -6 .. -1
    QColor meterPeak;  // clipping
    QColor meterHold;

    bool dark = true;
};

class QStyle;

// Combo boxes lose their arrow the moment a stylesheet touches ::drop-down, and
// a stylesheet cannot colour one per theme anyway - `image:` wants a file, and
// one baked colour cannot serve both the dark themes and Catppuccin Latte.
// Painting it in the style instead keeps it in step with whatever theme is live.
// Wraps the platform's own style, so a qt6ct/Breeze setup still looks native.
QStyle*               createThemedStyle();

// The widgets Qt paints itself - spin-box arrows, check-box indicators, plain
// list views - read the palette, not the stylesheet. Without this they follow
// the *desktop's* colours instead of the app's, which on a dark desktop leaves
// the light theme showing dark-on-dark boxes.
QPalette              themePalette(const Theme& t);

// Ink for a fill, and dim text lifted to clear its floor. Both derived rather
// than written, so a palette cannot ship an unreadable pair. See gui/color.h.
QColor                onFill(const QColor& fill);
QColor                dimOn(const Theme& t, const QColor& bg);

const QVector<Theme>& builtinThemes();
const Theme&          theme();                 // current
void                  setThemeIndex(int i);
int                   themeIndex();
QString               buildStyleSheet(const Theme& t);
