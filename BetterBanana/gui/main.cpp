#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QAbstractSlider>
#include <QComboBox>
#include <QEvent>
#include <QWidget>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

using namespace bb;

// A stray scroll over the window must never change the mix. Qt lets the wheel
// drive combo boxes and sliders even without focus, so an accidental scroll
// silently re-routes an application or nudges a fader. Swallow those.
class WheelGuard : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (ev->type() != QEvent::Wheel) return false;
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w) return false;
        // Device pickers: never scrollable, focused or not.
        if (qobject_cast<QComboBox*>(w)) return true;
        // Faders: only once deliberately focused by clicking them.
        if (qobject_cast<QAbstractSlider*>(w) && !w->hasFocus()) return true;
        return false;
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("betterbanana");
    // Must precede any stylesheet: it is what draws the combo and spin arrows,
    // which a stylesheet cannot colour per theme.
    app.setStyle(createThemedStyle());

    int fd = shm_open(kShmName, O_RDWR, 0600);
    if (fd < 0) {
        QMessageBox::critical(nullptr, "BetterBanana",
            "The audio engine is not running.\n\nStart it first:\n    build/bb-engine");
        return 1;
    }
    void* m = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        QMessageBox::critical(nullptr, "BetterBanana", "Cannot map the engine's shared state.");
        return 1;
    }
    Shared* shm = static_cast<Shared*>(m);
    if (!shm_compatible(shm)) {
        QMessageBox::critical(nullptr, "BetterBanana",
            "Engine/GUI version mismatch. Rebuild both.");
        return 1;
    }

    // A mixer that was killed with an EQ editor open leaves the engine
    // analysing a signal nobody is watching. Starting fresh means no editor is
    // open yet, so say so and let it go idle.
    shm->spec.source.store(kSpecNone);

    app.installEventFilter(new WheelGuard(&app));

    MainWindow w(shm);
    w.resize(1500, 720);
    w.show();
    // --vban opens the network stream dialog straight away.
    if (app.arguments().contains("--vban"))
        QTimer::singleShot(0, &w, &MainWindow::openVbanDialog);
    if (app.arguments().contains("--duck"))
        QTimer::singleShot(0, &w, &MainWindow::openDuckDialog);
    if (app.arguments().contains("--apps"))
        QTimer::singleShot(0, &w, &MainWindow::openAppsDialog);
    const int eqAt = app.arguments().indexOf("--eq");
    if (eqAt >= 0 && eqAt + 1 < app.arguments().size()) {
        const int bus = app.arguments().at(eqAt + 1).toInt();
        QTimer::singleShot(0, &w, [&w, bus] { w.openBusEq(bus); });
    }
    return app.exec();
}
