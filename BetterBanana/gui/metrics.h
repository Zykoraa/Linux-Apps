// betterbanana GUI - the spacing, radius and type scales.
//
// Everything used to be a bare literal at the call site: seven different gaps,
// five different row heights and three font sizes spanning 1.22x, which is why
// a section heading, a bypass chip and the dB readout the whole product exists
// to show all carried the same weight.
//
// Two things are deliberately runtime rather than constexpr. The interface
// scale, because retrofitting it later would mean touching every call site
// twice. And the type scale, because the sizes used to be absolute pixels: on a
// desktop asking for 13px the combo boxes rendered at 9, a third smaller than
// every other window on the screen.
#pragma once

#include <QFont>
#include <QFontInfo>
#include <QGuiApplication>
#include <algorithm>
#include <cmath>

namespace bbui {

// --- interface scale ------------------------------------------------------
//
// A preference on top of the compositor's own output scale, not a HiDPI fix.
// Qt6 already handles display scaling; this is for people who want the mixer
// bigger than their desktop's idea of 100%.
inline double& scaleRef() { static double s = 1.0; return s; }
inline double  scale()             { return scaleRef(); }

// How much bigger the desktop's own font is than the 10pt this design was laid
// out against.
//
// Point size, not pixel size, deliberately. A font's pixel size already carries
// the display's DPI, and Qt6 has separately scaled the whole UI for that - so
// deriving geometry from it counts the same scaling twice and made the window
// 23% wider on a 115dpi offscreen surface than on a 96dpi one, for the same
// user preference. Point size is what the user actually chose.
//
// Cached: px() is called from every constructor and several paintEvents.
inline double& fontRatioRef() { static double r = -1.0; return r; }
inline double  fontRatio()
{
    double& r = fontRatioRef();
    if (r < 0.0) {
        const QFont f = QGuiApplication::font();
        r = f.pointSizeF() > 0.0
                ? std::clamp(f.pointSizeF() / 10.0, 0.85, 2.0)
                : std::clamp(std::max(9, f.pixelSize()) / 13.0, 0.85, 2.0);
    }
    return r;
}

inline void setScale(double s)
{
    scaleRef() = std::clamp(s, 1.0, 2.0);
    fontRatioRef() = -1.0;              // re-measure against the current font
}

// Sizes track the type, not just the interface-size preference. They used to be
// decoupled, so a desktop asking for a 16px font grew every label by 23% inside
// boxes that stayed exactly where they were.
inline int px(int v) { return int(std::lround(v * fontRatio() * scale())); }

// --- spacing, base 4 ------------------------------------------------------

inline int gapXS() { return px(2);  }   // parts of one object: knob grids, chip rows
inline int gapS()  { return px(4);  }   // rows inside a card
inline int gapM()  { return px(8);  }   // between cards, and card inner margin
inline int gapL()  { return px(16); }   // class boundaries (hardware | virtual)

inline int rowH()  { return px(22); }   // every clickable row, one value

// Fader and meter travel. Capped, because both used to be Expanding with no
// maximum and the card row was the only stretch consumer in the window - so a
// tall window bought 800px of empty groove and not one pixel of control.
// One value, not a range: every fader and every meter in the console is this
// tall, so the same dBFS is the same number of pixels in every column. Four
// different meter heights used to put -20 dBFS 114px apart between a strip and
// a virtual bus, which makes a meter bridge you cannot read across.
inline int travel()    { return px(280); }   // the floor
inline int travelMax() { return px(760); }   // and the ceiling

// Half the fader cap, and the inset its groove needs so the cap does not hang
// off either end. The level meter reads the same constant: the two used to map
// their scales differently, so -60 on the fader and -60 on the meter beside it
// sat up to 8.5px apart.
inline int capHalf()     { return px(8); }
inline int travelInset() { return capHalf() + px(2); }

// --- radius ---------------------------------------------------------------

inline int radWell() { return px(2); }   // troughs, grooves, the XY plate
inline int radCtl()  { return px(3); }   // buttons, combos, chips, line edits
inline int radCard() { return px(6); }   // cards and group boxes

// --- type -----------------------------------------------------------------
//
// Anchored to the desktop's own font size so today's proportions are preserved
// at the common 13px default and everything grows together above it.
inline double typeK() { return fontRatio() * scale(); }

inline int fs(double logical)
{
    return std::max(8, int(std::lround(logical * typeK())));
}

inline int fsCaption() { return fs(10); }   // knob names, sub-labels, field labels
inline int fsBody()    { return fs(12); }   // prose, list items, menu items
inline int fsControl() { return fs(11); }   // buttons, combos, header plates
// The densest row in the app: three four-letter chips across a ~140px card.
// At the control step MUTE needs 44px and gets 43, so it clipped to "MUTI".
inline int fsChip()    { return fs(9);  }
// The bus-assign row packs five chips across a card that also has to hold a
// fader and a meter, so it gets its own step below the others.
inline int fsTiny()    { return fs(8);  }
inline int fsReadout() { return fs(13); }   // dB values, timecode, peak numbers
inline int fsDisplay() { return fs(15); }   // section and dialog titles

// Digits that do not shift sideways as a value changes. Must be applied from
// inside paintEvent or applyTheme: qApp->setStyleSheet() drops OpenType feature
// tags and resets letter spacing, so anything set in a constructor is gone at
// the first theme switch.
inline void makeTabular(QFont& f)
{
    f.setFeature(QFont::Tag("tnum"), 1);
    f.setFeature(QFont::Tag("lnum"), 1);
}

} // namespace bbui
