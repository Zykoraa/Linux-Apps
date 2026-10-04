// betterbanana - the mix matrix: one strip summed into every bus it feeds.
//
// Lifted out of Engine::mix_chunk so the routing rules - in particular the
// pre-fader send - can be tested without PipeWire.
//
// A bus takes a strip one of two ways:
//
//   post-fader (every bus, by default): the strip as its fader leaves it.
//   pre-fader  (BusParams::prefader):   the strip BEFORE its fader, at the
//                                       strip's own send level for that bus.
//
// The pre-fader tap is what lets one fader set what you hear in your
// headphones while a send sets what a stream hears: pull music down for
// yourself and the viewers still get it at full level. It is taken after pan
// and the strip delay, so placement and lip-sync carry to both paths; ducking
// and mute apply to both too (mute is a kill switch, and a stream is the one
// place where "muted but still going out" really hurts). Solo does not apply
// to a pre-fader bus - see Engine::mix_chunk - since soloing something to
// listen to it must never cut what the stream is hearing.
#pragma once

#include "../common/protocol.h"
#include "dsp.h"

#include <cstdint>

namespace bb {

// How one strip reaches one bus, decided once per block.
struct BusRoute {
    bool on = false;            // strip routed to this bus at all
    bool prefader = false;      // take the pre-fader tap at the send level
    bool solo_blocked = false;  // another strip is soloed on this bus
};

// Per-strip send gains, smoothed like the fader so a send move is not a click.
struct StripSends {
    SmoothGain g[kBuses];

    void configure(float sr)
    {
        for (int b = 0; b < kBuses; ++b) { g[b].configure(sr, 15.0f); g[b].snap(1.0f); }
    }
};

// Linear gain a strip's send should head for. Mute ramps it to silence the
// same way it ramps the fader.
inline float send_target(float send_db, bool muted)
{
    if (muted || !(send_db > kSendOffDb)) return 0.0f;
    return db_to_lin(send_db > kSendMaxDb ? kSendMaxDb : send_db);
}

// Adds one strip into every bus accumulator.
//   acc[b]  interleaved stereo accumulator for bus b, n frames
//   post    the strip after its fader (and pan, and delay)
//   pre     the strip before its fader, after pan and delay; may be null when
//           no route asks for it
//   dg      per-frame duck gain, already advanced once per frame
//   target  linear send targets per bus (send_target())
inline void matrix_add_strip(float* const acc[kBuses], const float* post, const float* pre,
                             const float* dg, uint32_t n, const BusRoute route[kBuses],
                             const float target[kBuses], StripSends& sends)
{
    for (int b = 0; b < kBuses; ++b) {
        const BusRoute& r = route[b];
        const bool pf = r.prefader && pre;
        if (!r.on || r.solo_blocked) {
            // Not feeding this bus: park the send at its target, so switching
            // the route on starts at the right level rather than a stale one.
            sends.g[b].snap(target[b]);
            continue;
        }
        float* a = acc[b];
        if (pf) {
            SmoothGain& g = sends.g[b];
            g.set_target(target[b]);
            for (uint32_t f = 0; f < n; ++f) {
                const float k = g.next() * dg[f];
                a[f * kChan]     += pre[f * kChan]     * k;
                a[f * kChan + 1] += pre[f * kChan + 1] * k;
            }
        } else {
            for (uint32_t f = 0; f < n; ++f) {
                a[f * kChan]     += post[f * kChan]     * dg[f];
                a[f * kChan + 1] += post[f * kChan + 1] * dg[f];
            }
        }
    }
}

} // namespace bb
