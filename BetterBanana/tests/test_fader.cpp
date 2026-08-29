// Verifies the fader's interaction contract without a display:
// double-click snaps to unity, wheel is ignored unless focused, drag maps
// position to value, and the range is clamped.
#include "../gui/knob.h"
#include "../gui/theme.h"
#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cstdio>

static int fails = 0;
static void chk(bool ok, const char* what, int got, int want)
{
    if (ok) std::printf("  ok    %-52s %d\n", what, got);
    else { std::printf("  FAIL  %-52s got %d want %d\n", what, got, want); ++fails; }
}

static void dblClick(QWidget* w, QPointF pos)
{
    QMouseEvent press(QEvent::MouseButtonPress, pos, w->mapToGlobal(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, pos, w->mapToGlobal(pos),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &dbl);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    Fader f(-600, 120, 0);
    f.resize(40, 200);

    std::printf("\n[fader]\n");
    f.setValue(-235);
    chk(f.value() == -235, "setValue stores the requested value", f.value(), -235);

    dblClick(&f, QPointF(12, 150));
    chk(f.value() == 0, "double-click snaps to 0 dB", f.value(), 0);

    f.setValue(-412);
    dblClick(&f, QPointF(12, 20));
    chk(f.value() == 0, "double-click near the top also snaps to 0 dB", f.value(), 0);

    f.setValue(9999);
    chk(f.value() == 120, "clamps to the top of the range", f.value(), 120);
    f.setValue(-9999);
    chk(f.value() == -600, "clamps to the bottom of the range", f.value(), -600);

    // A wheel event on an unfocused fader must be ignored, so a stray scroll
    // over the window cannot alter the mix.
    f.setValue(-100);
    f.clearFocus();
    QWheelEvent wheel(QPointF(12, 100), f.mapToGlobal(QPointF(12, 100)),
                      QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&f, &wheel);
    chk(f.value() == -100, "wheel ignored while unfocused", f.value(), -100);

    f.setFocus(Qt::MouseFocusReason);
    if (f.hasFocus()) {
        QWheelEvent wheel2(QPointF(12, 100), f.mapToGlobal(QPointF(12, 100)),
                           QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
        QApplication::sendEvent(&f, &wheel2);
        chk(f.value() == -90, "wheel moves by 1 dB once focused", f.value(), -90);
    } else {
        std::printf("  skip  wheel-when-focused (offscreen platform grants no focus)\n");
    }

    // A single press positions the fader; only the second click resets it.
    f.setValue(-300);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(12, 190),
                      f.mapToGlobal(QPointF(12, 190)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&f, &press);
    chk(f.value() < -400, "single click near the bottom lowers the fader", f.value(), -400);

    std::printf("\n%s (%d failure%s)\n\n", fails ? "FAILED" : "ALL PASSED",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
