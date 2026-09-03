// The custom widgets, driven by real events.
//
// Every case here is a defect that shipped once: a rename that came back, a
// fader that jumped when you took Ctrl mid-drag, a mis-click that slammed a
// live channel to -60, a pan axis the engine has never read, and a tooltip
// that overwrote the one its owner set. Needs no display and no engine.
#include "../gui/knob.h"
#include "../gui/widgets.h"
#include "../gui/theme.h"
#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <cstdio>

static int fails = 0;
static void chk(bool ok, const char* what, const QString& d = QString())
{ if (!ok) { ++fails; printf("  FAIL  %-52s %s\n", what, qPrintable(d)); }
  else printf("  ok    %s\n", what); }

int main(int c, char** v)
{
    QApplication a(c, v);
    a.setStyle(createThemedStyle());
    setThemeIndex(0);
    a.setPalette(themePalette(theme()));
    a.setStyleSheet(buildStyleSheet(theme()));
    printf("[fixes]\n");

    // 1. A rename through a QLabel* must stick, and must survive a repaint.
    {
        ElidedLabel* e = new ElidedLabel("HW INPUT 1");
        e->setProperty("role", "header");
        e->resize(60, 20);              // narrow enough to force elision
        QLabel* asBase = e;             // how every caller holds it
        asBase->setText("Podcast Mic");
        QPixmap pm(e->size());
        for (int i = 0; i < 3; ++i) e->render(&pm);
        chk(asBase->text() == "Podcast Mic",
            "rename through a QLabel* survives repaints", asBase->text());
        chk(e->toolTip() == "Podcast Mic",
            "the elided label offers the full name as a tooltip", e->toolTip());
        e->resize(400, 20);
        e->render(&pm);
        chk(e->toolTip().isEmpty(), "the tooltip clears once it fits", e->toolTip());
        delete e;
    }

    // 2. Taking Ctrl mid-drag must not jump the value.
    {
        Fader f(-600, 120, 0);
        f.resize(44, 300);
        f.setValue(-300);
        auto send = [&](QEvent::Type ty, double y, Qt::KeyboardModifiers m, Qt::MouseButton b) {
            QMouseEvent ev(ty, QPointF(12, y), f.mapToGlobal(QPointF(12, y)),
                           b, b == Qt::NoButton ? Qt::LeftButton : b, m);
            QApplication::sendEvent(&f, &ev);
        };
        // Grab the cap, drag 40px coarse, then take Ctrl and move one pixel.
        const double capY = 300.0 * (1.0 - (double(-300 + 600) / 720.0));
        send(QEvent::MouseButtonPress, capY, Qt::NoModifier, Qt::LeftButton);
        send(QEvent::MouseMove, capY - 40, Qt::NoModifier, Qt::NoButton);
        const int coarse = f.value();
        send(QEvent::MouseMove, capY - 41, Qt::ControlModifier, Qt::NoButton);
        const int afterCtrl = f.value();
        send(QEvent::MouseButtonRelease, capY - 41, Qt::ControlModifier, Qt::LeftButton);
        chk(qAbs(afterCtrl - coarse) <= 5,
            "taking Ctrl mid-drag does not jump the gain",
            QString("coarse %1 -> %2 (%3 raw)").arg(coarse).arg(afterCtrl)
                .arg(afterCtrl - coarse));
    }

    // 3. A left press away from the cap pages; it does not slam.
    {
        Fader f(-600, 120, 0);
        f.resize(44, 300);
        f.setValue(0);
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(12, 290),
                       f.mapToGlobal(QPointF(12, 290)),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&f, &ev);
        chk(f.value() < 0 && f.value() > -200,
            "a mis-click near the bottom pages, it does not slam to -60",
            QString::number(f.value() / 10.0));
    }

    // 4. The XY pad never reports a Y the engine would not read.
    {
        XYPad p;
        p.setValues(40, 90);
        chk(p.yValue() == 0, "the pan control pins Y", QString::number(p.yValue()));
        chk(p.xValue() == 40, "the pan control keeps X", QString::number(p.xValue()));
    }

    // 5. The meter keeps whatever its owner called the column.
    {
        LevelMeter m(2);
        m.resize(20, 300);
        m.setToolTip("Click to clear this strip's clip indicator");
        QEnterEvent e(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
        QApplication::sendEvent(&m, &e);
        chk(m.toolTip().contains("this strip's"),
            "the meter tooltip keeps the per-column wording", m.toolTip());
    }

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
