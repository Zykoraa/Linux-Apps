// The STREAM knob on a real strip card, driven by real key and wheel events.
//
// It used to be disabled whenever it could not act, and a disabled knob just
// ignores you - which a user reported, reasonably, as "the knobs are broken".
// Now: on a stream bus that still follows the faders, the first turn makes it
// pre-fader (silently - sends start at the faders); on a strip with no stream
// level at all it refuses, and says why. And showing a value never sets one.
#include "../gui/mainwindow.h"
#include "../gui/knob.h"
#include "../gui/theme.h"
#include "../common/streamsetup.h"

#include <QApplication>
#include <QKeyEvent>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void chk(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

// The STREAM knob is the one whose tooltip talks about the stream.
static Knob* sendKnob(StripWidget& w)
{
    for (Knob* k : w.findChildren<Knob*>()) {
        const QString t = k->toolTip().toLower();
        if (t.contains("stream") || t.contains("viewers") || t.contains("callers")) return k;
    }
    return nullptr;
}

static void keyUp(QWidget* w)
{
    QKeyEvent e(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

static void wheelUp(QWidget* w)
{
    QWheelEvent e(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120), Qt::NoButton,
                  Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setPalette(themePalette(theme()));
    app.setStyleSheet(buildStyleSheet(theme()));

    auto shm = std::make_unique<Shared>();
    Shared* s = shm.get();
    set_defaults(s);
    routing_write_begin(s->routing);
    std::snprintf(s->routing.bus_out[2], kNameLen, "%s", kStreamSinkName);
    std::snprintf(s->routing.hw_in[1], kNameLen, "%s", "cable:0");
    routing_write_end(s->routing);
    s->stream.bus.store(2);                 // what the engine publishes
    s->strip[1].bus_on[2].store(1);         // HW IN 2 -> stream
    s->strip[3].bus_on[2].store(1);         // VAIO    -> stream
    s->strip[1].gain_db.store(-20.0f);
    s->strip[3].gain_db.store(-5.0f);

    QString said;
    auto listen = [&](StripWidget& w) {
        QObject::connect(&w, &StripWidget::statusMessage, [&](const QString& t) { said = t; });
    };

    // --- a strip on a stream bus that still follows the faders ------------
    {
        StripWidget hw2(s, 1, true, "HW IN 2");
        listen(hw2);
        hw2.pullFromShm();
        Knob* k = sendKnob(hw2);
        chk(k != nullptr, "the strip has a STREAM knob");
        if (!k) return 1;
        chk(k->isEnabled(), "it is not disabled");
        chk(k->value() == -200, "it shows what the stream gets now: the fader, -20 dB");
        hw2.pullFromShm();
        chk(s->bus[2].prefader.load() == 0, "showing that changes nothing in the mixer");

        keyUp(k);
        chk(s->bus[2].prefader.load() == 1, "turning it makes the stream bus pre-fader");
        // One arrow step on a -60..+12 knob is 3.5 dB: from the fader's -20,
        // not from 0 dB or wherever the send last was.
        chk(std::fabs(s->strip[1].send_db[2].load() - (-16.5f)) < 0.01f,
            "and this strip's send moves on from where the stream was");
        chk(s->strip[3].send_db[2].load() == -5.0f,
            "while every other send starts at its own fader, so nothing else jumps");
        chk(said.contains("pre-fader"), "and the status bar says what happened");

        said.clear();
        wheelUp(k);
        chk(said.isEmpty(), "after that, turning it is just turning it");
        const float before = s->strip[1].send_db[2].load();
        s->strip[1].gain_db.store(-60.0f);
        hw2.pullFromShm();
        chk(s->strip[1].send_db[2].load() == before,
            "and the fader no longer moves what the stream gets");
    }

    // --- strips with no stream level refuse, and say why -------------------
    {
        StripWidget mic(s, 0, true, "HW IN 1");
        listen(mic);
        mic.pullFromShm();
        Knob* k = sendKnob(mic);
        chk(k != nullptr && k->isEnabled(), "an unrouted strip's knob still takes a click");
        if (!k) return 1;
        const float send = s->strip[0].send_db[2].load();
        said.clear();
        keyUp(k);
        wheelUp(k);
        chk(s->strip[0].send_db[2].load() == send && !s->strip[0].bus_on[2].load(),
            "but turning it changes nothing, and routes nothing");
        chk(said.contains("does not go to the stream"), "and it says why");
    }
    {
        StripWidget aux(s, kStrips - 1, false, "AUX");
        listen(aux);
        aux.pullFromShm();
        Knob* k = sendKnob(aux);
        if (!k) { chk(false, "AUX has a STREAM knob"); return 1; }
        said.clear();
        keyUp(k);
        chk(!s->strip[kStrips - 1].bus_on[2].load(), "AUX is never put on the stream");
        chk(said.contains("callers"), "and the reason given is the callers");
    }
    {
        s->stream.bus.store(-1);
        StripWidget hw2(s, 1, true, "HW IN 2");
        listen(hw2);
        hw2.pullFromShm();
        Knob* k = sendKnob(hw2);
        if (!k) { chk(false, "the knob exists with no stream bus"); return 1; }
        said.clear();
        keyUp(k);
        chk(said.contains("No stream bus"), "with no stream bus it points at the setup");
        s->stream.bus.store(2);
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
