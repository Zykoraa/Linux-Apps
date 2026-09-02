// betterbanana GUI - the colour contract.
//
// Ten palettes multiply every colour decision by ten, and a foreground picked
// by eye against one of them fails against the others. Eight of the eighty
// "ink on a lit toggle" pairs failed WCAG AA that way, worst of them white on
// Catppuccin Latte's yellow SOLO at 2.62:1.
//
// So no foreground is written literally any more. Ink is derived from the fill
// it sits on, dim text is lifted until it clears a floor, and hover steps in
// Lab rather than through QColor::lighter(), which clamps to nothing once a
// channel is already at 255 - which is why a checked MUTE had no hover state at
// all in the default theme.
//
// Header-only on purpose: it is used by the widgets, by the stylesheet builder
// and by tests/test_contrast.cpp, and none of them should need a link change.
#pragma once

#include <QColor>
#include <algorithm>
#include <cmath>

namespace bbcolor {

// --- WCAG -----------------------------------------------------------------

inline double srgbToLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

inline double relLum(const QColor& c)
{
    return 0.2126 * srgbToLinear(c.redF())
         + 0.7152 * srgbToLinear(c.greenF())
         + 0.0722 * srgbToLinear(c.blueF());
}

inline double contrast(const QColor& a, const QColor& b)
{
    const double la = relLum(a), lb = relLum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// --- CIE L*a*b* -----------------------------------------------------------
//
// Lightness steps have to happen in a perceptual space. Stepping in HSV or
// through lighter()/darker() moves saturated colours by wildly different
// perceived amounts and clamps at the top of the range.

struct Lab { double l, a, b; };

inline Lab toLab(const QColor& c)
{
    // sRGB -> linear -> XYZ (D65) -> Lab
    const double r = srgbToLinear(c.redF());
    const double g = srgbToLinear(c.greenF());
    const double b = srgbToLinear(c.blueF());

    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const double y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b) / 1.00000;
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;

    auto f = [](double t) {
        return t > 0.008856 ? std::cbrt(t) : (7.787 * t + 16.0 / 116.0);
    };
    const double fx = f(x), fy = f(y), fz = f(z);
    return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

inline QColor fromLab(const Lab& lab, int alpha = 255)
{
    const double fy = (lab.l + 16.0) / 116.0;
    const double fx = fy + lab.a / 500.0;
    const double fz = fy - lab.b / 200.0;

    auto inv = [](double t) {
        const double t3 = t * t * t;
        return t3 > 0.008856 ? t3 : (t - 16.0 / 116.0) / 7.787;
    };
    const double x = inv(fx) * 0.95047, y = inv(fy), z = inv(fz) * 1.08883;

    double r =  3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    double b =  0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    auto enc = [](double v) {
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(std::max(v, 0.0), 1.0 / 2.4) - 0.055;
        return std::clamp(v, 0.0, 1.0);
    };
    QColor out;
    out.setRgbF(enc(r), enc(g), enc(b));
    out.setAlpha(alpha);
    return out;
}

// --- The four laws --------------------------------------------------------

// Law 1: ink is never written literally, it is chosen for the fill it lands on.
//
// Measured, not thresholded. A luminance threshold is the usual shortcut and it
// is right almost everywhere - but the ink here is #101014 rather than pure
// black, which moves the crossover, and Solarized Dark's eqOn #6c71c4 lands
// close enough to it that the shortcut picked the worse of the two: 4.34:1
// where the other choice gives 4.84:1. Comparing both costs two multiplies and
// is exact for every palette, including ones nobody has written yet.
inline QColor onColor(const QColor& fill)
{
    static const QColor kInk("#101014"), kPaper("#ffffff");
    return contrast(kInk, fill) >= contrast(kPaper, fill) ? kInk : kPaper;
}

// The same law, applied to the other side of the pair. A fill whose luminance
// sits at the black/white crossover cannot carry readable text in either
// direction: Solarized Dark's eqOn #6c71c4 gives 4.34:1 against the dark ink
// and 4.38:1 against white, so no choice of ink saves it. Since the palette and
// the ink are both ours, move the fill instead - the smallest step in L* that
// lets its best ink clear the floor. Every fill that already passes is returned
// untouched, so nine palettes are unaffected.
inline QColor onColor(const QColor& fill);      // fwd, defined above

inline QColor fitFill(const QColor& fill, double floor)
{
    if (contrast(onColor(fill), fill) >= floor) return fill;

    QColor bestC = fill;
    double bestR = contrast(onColor(fill), fill);
    for (double dir : { -1.0, 1.0 }) {
        Lab lab = toLab(fill);
        for (int i = 0; i < 60; ++i) {
            lab.l = std::clamp(lab.l + dir, 0.0, 100.0);
            const QColor c = fromLab(lab, fill.alpha());
            const double r = contrast(onColor(c), c);
            if (r > bestR) { bestR = r; bestC = c; }
            if (r >= floor) return c;
            if (lab.l <= 0.0 || lab.l >= 100.0) break;
        }
    }
    return bestC;
}

// Law 2: text clears a floor against the surface it is actually drawn on.
// Walks L* toward whichever end of the range helps, so a dim grey lifts on a
// dark theme and darkens on a light one, keeping its hue.
inline QColor ensureContrast(const QColor& fg, const QColor& bg, double ratio)
{
    if (contrast(fg, bg) >= ratio) return fg;

    Lab lab = toLab(fg);
    const bool lighten = relLum(bg) < 0.1791;
    const double step = lighten ? 1.0 : -1.0;

    QColor best = fg;
    double bestRatio = contrast(fg, bg);
    for (int i = 0; i < 100; ++i) {
        lab.l = std::clamp(lab.l + step, 0.0, 100.0);
        const QColor c = fromLab(lab, fg.alpha());
        const double r = contrast(c, bg);
        if (r > bestRatio) { bestRatio = r; best = c; }
        if (r >= ratio) return c;
        if (lab.l <= 0.0 || lab.l >= 100.0) break;
    }
    // Could not reach the floor without leaving the colour behind entirely;
    // hand back the best we found rather than something unrecognisable.
    return best;
}

// A perceptual lightness step, for hover and pressed states. `pct` is in L*
// points, signed. Unlike lighter(), this still moves a fully saturated colour.
inline QColor nudge(const QColor& c, double pct)
{
    Lab lab = toLab(c);
    lab.l = std::clamp(lab.l + pct, 0.0, 100.0);
    return fromLab(lab, c.alpha());
}

// Hover that always moves, in whichever direction there is room.
inline QColor hoverOf(const QColor& c)
{
    return nudge(c, toLab(c).l > 62.0 ? -7.0 : 8.0);
}

// CIE76 distance. Coarse, but the question here is only ever "would a user
// call these two the same colour", and for that it is enough.
inline double deltaE(const QColor& a, const QColor& b)
{
    const Lab x = toLab(a), y = toLab(b);
    return std::sqrt((x.l - y.l) * (x.l - y.l) + (x.a - y.a) * (x.a - y.a)
                   + (x.b - y.b) * (x.b - y.b));
}

// A colour blended toward another by t (0..1), in Lab. Used for ghosted rungs
// and disabled ink, where alpha compositing over an unknown parent is wrong.
inline QColor mix(const QColor& a, const QColor& b, double t)
{
    const Lab la = toLab(a), lb = toLab(b);
    return fromLab({ la.l + (lb.l - la.l) * t,
                     la.a + (lb.a - la.a) * t,
                     la.b + (lb.b - la.b) * t });
}

// The floors themselves, so the stylesheet, the painters and the test all
// quote the same numbers.
constexpr double kTextFloor   = 4.5;   // anything below 14px
constexpr double kLargeFloor  = 3.0;   // >=18px, or bold >=14px
constexpr double kBoundFloor  = 3.0;   // borders, state fills, indicators

} // namespace bbcolor
