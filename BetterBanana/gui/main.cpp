#include "mainwindow.h"
#include "metrics.h"
#include "theme.h"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

using namespace bb;

// A stray scroll over the window must never change the mix. Qt lets the wheel
// drive combo boxes and sliders even without focus, so an accidental scroll
// silently re-routes an application or nudges a fader.
//
// The obvious form of this rule blocks far too much. QScrollBar *is* a
// QAbstractSlider, and QAbstractScrollArea forwards a wheel event to its
// scrollbar by sending it, which passes through this filter - so the guard used
// to stop the main window, the EQ band table and the Apps dialog from being
// scrolled with the wheel at all. Exempt scrollbars, and forward a swallowed
// event to the nearest scroll area so the page still moves under a combo box.
class WheelGuard : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (ev->type() != QEvent::Wheel) return false;
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w) return false;
        if (m_reentering) return false;           // our own forwarded event

        // Scrollbars are how scrolling happens; never block them.
        if (qobject_cast<QScrollBar*>(w)) return false;

        const bool block = qobject_cast<QComboBox*>(w)
                        || (qobject_cast<QAbstractSlider*>(w) && !w->hasFocus());
        if (!block) return false;

        // Hand it to the enclosing scroll area instead of dropping it, so the
        // wheel still pans the window when the pointer happens to be over a
        // device picker.
        for (QWidget* p = w->parentWidget(); p; p = p->parentWidget()) {
            if (auto* area = qobject_cast<QAbstractScrollArea*>(p)) {
                if (QScrollBar* sb = area->verticalScrollBar();
                    sb && sb->isVisible() && sb->maximum() > sb->minimum()) {
                    m_reentering = true;
                    QCoreApplication::sendEvent(sb, ev);
                    m_reentering = false;
                }
                break;
            }
        }
        return true;
    }

private:
    bool m_reentering = false;
};

// The only screen a user without a running engine ever sees. It used to be a
// bare desktop-coloured box telling them to run "build/bb-engine", which is
// wrong for anyone who installed with the documented script: that ships a
// systemd user unit and no build directory.
static bool offerToStartEngine()
{
    QMessageBox box;
    box.setWindowTitle("BetterBanana");
    box.setIcon(QMessageBox::Warning);
    box.setText("<b>The audio engine is not running.</b>");
    box.setInformativeText(
        "BetterBanana's mixing happens in a separate process. Start it and this "
        "window will open.");
    QPushButton* start = box.addButton("Start the engine", QMessageBox::AcceptRole);
    start->setProperty("cta", "primary");
    box.addButton("Quit", QMessageBox::RejectRole);
    box.setDefaultButton(start);
    box.exec();
    if (box.clickedButton() != start) return false;

    QProcess::execute("systemctl", { "--user", "start", "betterbanana-engine" });
    // Poll rather than sleep, so the dialog stays responsive.
    QElapsedTimer t; t.start();
    while (t.elapsed() < 4000) {
        const int fd = shm_open(kShmName, O_RDWR, 0600);
        if (fd >= 0) { close(fd); return true; }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    QMessageBox::critical(nullptr, "BetterBanana",
        "The engine did not start.\n\nTry it by hand to see why:\n"
        "    systemctl --user status betterbanana-engine\n"
        "or run bb-engine directly.");
    return false;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("betterbanana");
    app.setOrganizationName("betterbanana");
    app.setApplicationVersion(BB_VERSION);
    app.setApplicationDisplayName("BetterBanana");
    // Wayland matches a window to its .desktop entry by app_id, and without
    // this the app_id is the executable's basename - which is why the mixer
    // showed a generic placeholder in the taskbar and in Alt-Tab.
    QGuiApplication::setDesktopFileName("betterbanana");
    app.setWindowIcon(QIcon::fromTheme("betterbanana",
                                       QIcon(":/betterbanana.svg")));

    // Must precede any stylesheet: it is what draws the combo and spin arrows,
    // which a stylesheet cannot colour per theme.
    app.setStyle(createThemedStyle());

    {   // Theme the error boxes below too: they used to render in the desktop's
        // colours because the theme was only installed by MainWindow.
        QSettings st("betterbanana", "gui");
        bbui::setScale(st.value("uiScale", 100).toInt() / 100.0);
        setThemeIndex(st.value("theme", 0).toInt());
        app.setPalette(themePalette(theme()));
        app.setStyleSheet(buildStyleSheet(theme()));
    }

    int fd = shm_open(kShmName, O_RDWR, 0600);
    if (fd < 0) {
        if (!offerToStartEngine()) return 1;
        fd = shm_open(kShmName, O_RDWR, 0600);
        if (fd < 0) return 1;
    }
    void* m = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        QMessageBox::critical(nullptr, "BetterBanana",
            "Cannot map the engine's shared state.\n\n"
            "The engine is running but this window cannot reach it. Restarting "
            "the engine usually clears it:\n    systemctl --user restart betterbanana-engine");
        return 1;
    }
    Shared* shm = static_cast<Shared*>(m);
    if (!shm_compatible(shm)) {
        QMessageBox::critical(nullptr, "BetterBanana",
            QString("The engine and this window are different versions.\n\n"
                    "This window speaks protocol v%1. Restart the engine so both "
                    "sides come from the same build:\n"
                    "    systemctl --user restart betterbanana-engine")
                .arg(kVersion));
        return 1;
    }

    // A mixer that was killed with an EQ editor open leaves the engine
    // analysing a signal nobody is watching. Starting fresh means no editor is
    // open yet, so say so and let it go idle.
    shm->spec.source.store(kSpecNone);

    app.installEventFilter(new WheelGuard(&app));

    MainWindow w(shm);
    w.show();
    // --vban opens the network stream dialog straight away.
    if (app.arguments().contains("--vban"))
        QTimer::singleShot(0, &w, &MainWindow::openVbanDialog);
    if (app.arguments().contains("--duck"))
        QTimer::singleShot(0, &w, &MainWindow::openDuckDialog);
    if (app.arguments().contains("--apps"))
        QTimer::singleShot(0, &w, &MainWindow::openAppsDialog);
    if (app.arguments().contains("--align"))
        QTimer::singleShot(0, &w, &MainWindow::openAlignDialog);
    if (app.arguments().contains("--check"))
        QTimer::singleShot(0, &w, &MainWindow::openDiagnoseDialog);
    const int eqAt = app.arguments().indexOf("--eq");
    if (eqAt >= 0 && eqAt + 1 < app.arguments().size()) {
        const int bus = app.arguments().at(eqAt + 1).toInt();
        QTimer::singleShot(0, &w, [&w, bus] { w.openBusEq(bus); });
    }
    return app.exec();
}
