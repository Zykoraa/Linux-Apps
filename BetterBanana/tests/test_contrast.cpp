// Enforces the colour contract in gui/color.h across every built-in palette.
//
// The whole colour half of this app's design used to be one bug repeated: a
// foreground written as a literal and never measured against the ten
// backgrounds it would land on. Eight of the eighty "ink on a lit toggle" pairs
// failed WCAG AA that way - white on Catppuccin Latte's yellow SOLO at 2.62:1
// being the worst - and the 9px caption grey failed on `panel` in six themes.
//
// Nothing here needs a display or an event loop.
#include "../gui/color.h"
#include "../gui/theme.h"

#include <QGuiApplication>
#include <cstdio>
#include <vector>

static int fails = 0;

static void chk(bool ok, const char* what, const QString& detail = QString())
{
    if (ok) return;
    ++fails;
    std::printf("  FAIL  %-58s %s\n", what, qPrintable(detail));
}

static QString pair(const char* a, const QColor& fg, const char* b, const QColor& bg)
{
    return QString("%1 %2 on %3 %4 = %5:1")
        .arg(a, fg.name(QColor::HexRgb), b, bg.name(QColor::HexRgb))
        .arg(bbcolor::contrast(fg, bg), 0, 'f', 2);
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);      // builtinThemes() needs QColor only
    std::printf("[contrast]\n");

    const auto& themes = builtinThemes();
    chk(themes.size() >= 10, "every built-in palette is present",
        QString::number(themes.size()));

    for (const Theme& t : themes) {
        const QString n = t.name;

        // --- Law 1: ink is derived from the fill, never written -----------
        struct Role { const char* name; QColor col; };
        const Role roles[] = {
            { "busA", t.busA }, { "busB", t.busB }, { "mute", t.mute },
            { "solo", t.solo }, { "mono", t.mono }, { "eqOn", t.eqOn },
            { "rec",  t.rec  }, { "accent", t.accent },
            { "header", t.header }, { "meterPeak", t.meterPeak },
        };
        for (const Role& r : roles) {
            const QColor fill = bbcolor::fitFill(r.col, bbcolor::kTextFloor);
            const QColor ink  = onFill(fill);
            chk(bbcolor::contrast(ink, fill) >= bbcolor::kTextFloor,
                "a lit fill and its ink clear 4.5:1 together",
                n + ": " + pair("ink", ink, r.name, fill));
            // The correction must be small enough that the palette is still
            // recognisably the palette it was transcribed from.
            chk(bbcolor::contrast(fill, r.col) <= 1.35,
                "fitFill moves a colour as little as it can",
                n + ": " + pair("fitted", fill, r.name, r.col));
        }

        // --- Law 2: dim text clears its floor on every surface it lands on -
        for (const auto& surf : std::vector<std::pair<const char*, QColor>>{
                 { "bg", t.bg }, { "panel", t.panel },
                 { "panelAlt", t.panelAlt }, { "well", t.well } }) {
            const QColor lifted = dimOn(t, surf.second);
            chk(bbcolor::contrast(lifted, surf.second) >= bbcolor::kTextFloor,
                "dimOn clears 4.5:1 on every surface",
                n + ": " + pair("dim", lifted, surf.first, surf.second));
        }

        // Body text is used as-is, so it has to pass on its own.
        chk(bbcolor::contrast(t.text, t.panel) >= bbcolor::kTextFloor,
            "body text clears 4.5:1 on a card", n + ": " + pair("text", t.text, "panel", t.panel));
        chk(bbcolor::contrast(t.text, t.bg) >= bbcolor::kTextFloor,
            "body text clears 4.5:1 on the window",
            n + ": " + pair("text", t.text, "bg", t.bg));

        // --- Surfaces: a card must be visible against the window ----------
        //
        // These used to sit at 1.07:1 in Rose Pine and 1.08:1 in Latte, so with
        // the card layer painting nothing at all there was no structure left in
        // the window whatever.
        chk(bbcolor::contrast(t.panel, t.bg) >= 1.06,
            "a card is distinguishable from the window",
            n + ": " + pair("panel", t.panel, "bg", t.bg));
        chk(bbcolor::contrast(t.well, t.panel) >= 1.20,
            "a trough reads as recessed into its card",
            n + ": " + pair("well", t.well, "panel", t.panel));

        // --- Law: boundaries -----------------------------------------------
        const QColor edge = bbcolor::ensureContrast(t.border, t.well, bbcolor::kBoundFloor);
        chk(bbcolor::contrast(edge, t.well) >= bbcolor::kBoundFloor,
            "scrollbar and indicator edges clear 3:1",
            n + ": " + pair("edge", edge, "well", t.well));

        // --- hover always moves --------------------------------------------
        //
        // QColor::lighter() clamps once a channel is at 255, which left a
        // checked MUTE, SOLO, MONO, EQ and REC with no hover feedback at all in
        // the default theme.
        for (const Role& r : roles) {
            const QColor h = bbcolor::fitFill(bbcolor::hoverOf(r.col), bbcolor::kTextFloor);
            chk(bbcolor::contrast(h, r.col) >= 1.06,
                "hover is visibly different from the resting fill",
                n + ": " + pair("hover", h, r.name, r.col));
            chk(bbcolor::contrast(onFill(h), h) >= bbcolor::kTextFloor,
                "hovered ink still clears 4.5:1",
                n + ": " + pair("ink", onFill(h), "hover", h));
        }
    }

    // --- the helpers themselves ------------------------------------------
    chk(bbcolor::contrast(QColor("#000000"), QColor("#ffffff")) > 20.9,
        "black on white is 21:1");
    chk(qAbs(bbcolor::contrast(QColor("#777777"), QColor("#777777")) - 1.0) < 1e-9,
        "a colour against itself is 1:1");
    {   // Latte's busA sits at L=0.264, just above the crossover. A rounded
        // luminance threshold would hand it white at 3.34:1; measuring both
        // candidates picks the dark ink at 6.28:1.
        const QColor ink = bbcolor::onColor(QColor("#40a02b"));
        chk(ink == QColor("#101014"), "onColor picks dark ink for mid-green",
            ink.name(QColor::HexRgb));
    }
    {   // Lab round-trip, so a nudge of zero is a no-op.
        const QColor c("#6ee7a8");
        const QColor back = bbcolor::fromLab(bbcolor::toLab(c));
        chk(qAbs(back.red() - c.red()) <= 1 && qAbs(back.green() - c.green()) <= 1
                && qAbs(back.blue() - c.blue()) <= 1,
            "sRGB -> Lab -> sRGB round-trips", back.name(QColor::HexRgb));
    }
    {   // ensureContrast keeps the hue it was given.
        const QColor lifted = bbcolor::ensureContrast(QColor("#7d8a99"), QColor("#1f242c"), 7.0);
        chk(bbcolor::contrast(lifted, QColor("#1f242c")) >= 7.0,
            "ensureContrast reaches a demanding floor",
            lifted.name(QColor::HexRgb));
        chk(qAbs(lifted.hue() - QColor("#7d8a99").hue()) <= 12,
            "ensureContrast keeps the hue", QString::number(lifted.hue()));
    }

    std::printf("\n%s (%d failure%s)\n\n", fails ? "FAILED" : "ALL PASSED",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
