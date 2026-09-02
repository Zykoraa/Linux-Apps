// betterbanana - the voice changer.
//
// Header-only, float32, allocation-free: everything here is called from the
// PipeWire realtime thread, like dsp.h. Each effect has an "off" state it
// checks first, so an inactive rack costs one branch per sample.
#pragma once

#include "dsp.h"
#include "formant.h"
#include "../common/protocol.h"

#include <cmath>

namespace bb {

// ---------------------------------------------------------------------------
// Delay-line pitch shifter.
//
// Two read heads chase the write head at the pitch ratio, half a window apart,
// crossfaded so whichever one is about to run off the end of its window is
// already silent. This is the classic granular shifter: cheap, allocation-free
// and no FFT on the audio thread. The price is a faint warble on speech that a
// phase vocoder would not have, at a grain rate of |ratio-1| * sr / window -
// about 8 Hz for a five-semitone shift, 23 Hz for an octave.
//
// To pitch UP the read head must move through the buffer faster than the write
// head, which would mean reading samples that do not exist yet. So it starts a
// whole window behind and catches up, wrapping back when it arrives: the delay
// falls at (ratio - 1) samples per sample.
//
// At zero semitones the heads sit at fixed offsets and their sum is a comb
// filter, not a copy - so zero bypasses rather than running the maths.
// ---------------------------------------------------------------------------
constexpr int kPitchWindow = 2048;              // nominal sweep, 43 ms at 48 kHz
constexpr int kPitchBuf    = kPitchWindow * 2;
constexpr float kPitchXfade = 0.25f;            // fraction of the sweep spent blending

// The largest sweep that keeps every tap inside the buffer: the partner tap
// reaches 1.75 sweeps back at the seam.
constexpr int kPitchMaxLen = int(kPitchBuf / 1.75f) - 8;
constexpr int kPitchMinLen = 1024;

// ---------------------------------------------------------------------------
// Tracks the period of whatever is being sung or spoken, cheaply.
//
// The pitch shifter splices its read head back by one sweep every time it runs
// out of window. If that sweep is an arbitrary length, the waveform either side
// of the splice does not line up, and the mismatch repeats at the sweep rate -
// which for a big shift lands between 15 and 20 Hz, exactly where the ear hears
// roughness. Hence "robotic". Land the splice on a whole number of pitch
// periods instead and the two sides match.
//
// Autocorrelation on a signal decimated by four: a quarter of the resolution
// costs nothing here, because a period only has to be right to within a sample
// or two of the original rate to kill the discontinuity.
// ---------------------------------------------------------------------------
struct PeriodTracker {
    static constexpr int kDec    = 4;
    static constexpr int kWin    = 512;                 // 43 ms of decimated audio
    static constexpr int kUpdate = 256;                 // re-measure every ~21 ms
    static constexpr int kLagLo  = 24;                  // 500 Hz at 12 kHz
    static constexpr int kLagHi  = 200;                 // 60 Hz

    static constexpr int kFine = 2048;                  // full-rate history

    float ring[kWin] = {};
    float lin[kWin] = {};
    float fr[kFine] = {};                               // full rate, for refinement
    float flin[kFine] = {};
    int   wi = 0, fi = 0, dec_n = 0, since = 0;
    float acc = 0.0f;
    float period = 0.0f;                                // original samples, 0 = unknown

    void reset()
    {
        for (float& v : ring) v = 0.0f;
        for (float& v : fr) v = 0.0f;
        wi = fi = dec_n = since = 0; acc = 0.0f; period = 0.0f;
    }

    inline void push(float x)
    {
        fr[fi] = x;
        fi = (fi + 1) % kFine;
        acc += x;
        if (++dec_n < kDec) return;
        ring[wi] = acc / kDec;
        acc = 0.0f; dec_n = 0;
        wi = (wi + 1) % kWin;
        if (++since >= kUpdate) { since = 0; measure(); }
    }

    void measure()
    {
        for (int i = 0; i < kWin; ++i) lin[i] = ring[(wi + i) % kWin];
        double e0 = 0.0;
        for (int i = 0; i < kWin; ++i) e0 += double(lin[i]) * lin[i];
        if (e0 < 1e-6) { period = 0; return; }           // silence: nothing to lock to

        double best = 0.0;
        int    bestLag = 0;
        for (int lag = kLagLo; lag <= kLagHi; ++lag) {
            const int m = kWin - lag;
            double num = 0.0, ea = 0.0, eb = 0.0;
            for (int i = 0; i < m; ++i) {
                num += double(lin[i]) * lin[i + lag];
                ea  += double(lin[i]) * lin[i];
                eb  += double(lin[i + lag]) * lin[i + lag];
            }
            const double den = std::sqrt(ea * eb) + 1e-12;
            const double r = num / den;
            if (r > best) { best = r; bestLag = lag; }
        }
        // Below this the signal is a consonant or noise; there is no period to
        // preserve, and forcing one would be worse than leaving it alone.
        if (best <= 0.6 || bestLag == 0) { period = 0.0f; return; }

        // The decimated search only resolves the period to four samples, and
        // four samples of error multiplied by however many periods make up a
        // sweep leaves the harmonics badly out of phase across the seam - which
        // is the whole thing this is trying to avoid. Refine at full rate, then
        // interpolate between samples.
        for (int i = 0; i < kFine; ++i) flin[i] = fr[(fi + i) % kFine];
        const int coarse = bestLag * kDec;
        const int span = kDec + 2;
        const int m = kFine / 2;
        double fbest = -1e30, fm1 = 0.0, fp1 = 0.0;
        int    flag = coarse;
        for (int lag = coarse - span; lag <= coarse + span; ++lag) {
            if (lag < kLagLo * kDec || lag + m >= kFine) continue;
            double num = 0.0;
            for (int i = 0; i < m; ++i) num += double(flin[i]) * flin[i + lag];
            if (num > fbest) { fbest = num; flag = lag; }
        }
        auto corr = [&](int lag) {
            if (lag < 1 || lag + m >= kFine) return 0.0;
            double v = 0.0;
            for (int i = 0; i < m; ++i) v += double(flin[i]) * flin[i + lag];
            return v;
        };
        fm1 = corr(flag - 1); fp1 = corr(flag + 1);
        const double den = fm1 - 2 * fbest + fp1;
        const double off = std::fabs(den) < 1e-12 ? 0.0 : 0.5 * (fm1 - fp1) / den;
        period = float(flag) + float(std::clamp(off, -1.0, 1.0));
    }
};

struct PitchShifter {
    float buf[kPitchBuf] = {};
    int   wr = 0;
    float phase = 0.0f;                 // current delay, in samples
    float ratio = 1.0f;
    float len = kPitchWindow;           // sweep length actually in use
    float pending = kPitchWindow;       // applied at the next seam
    bool  active = false;
    bool  coherent = false;             // is the sweep a whole number of periods?

    float lo() const { return len * 0.5f; }
    float hi() const { return len * 1.5f; }

    void set_semitones(float st)
    {
        const bool was = active;
        active = std::fabs(st) > 0.01f;
        ratio = std::pow(2.0f, clampf(st, -24.0f, 24.0f) / 12.0f);
        if (active && !was) reset();
    }

    // Sweep an exact whole number of pitch periods, as near the nominal window
    // as that allows. Zero period (unvoiced, silent) falls back to the nominal.
    // The sweep is a whole number of periods, so it may be fractional in
    // samples - the taps interpolate anyway, and rounding to whole samples is
    // precisely the error this exists to remove.
    void set_period(float samples)
    {
        if (samples <= 1.0f) {
            pending = float(kPitchWindow);
            coherent = false;
            return;
        }
        int k = std::max(1, (int)std::lround(double(kPitchWindow) / samples));
        float want = k * samples;
        while (want > kPitchMaxLen && k > 1) want = --k * samples;
        while (want < kPitchMinLen && (k + 1) * samples <= kPitchMaxLen) want = ++k * samples;
        coherent = want >= kPitchMinLen && want <= kPitchMaxLen;
        pending = coherent ? want : float(kPitchWindow);
    }

    void reset()
    {
        for (int i = 0; i < kPitchBuf; ++i) buf[i] = 0.0f;
        len = pending;
        phase = len;
    }

    inline float tap(float delay) const
    {
        float p = float(wr) - delay;
        while (p < 0.0f) p += float(kPitchBuf);
        const int i0 = int(p) % kPitchBuf;
        const int i1 = (i0 + 1) % kPitchBuf;
        const float f = p - std::floor(p);
        return buf[i0] * (1.0f - f) + buf[i1] * f;
    }

    inline float process(float x)
    {
        buf[wr] = x;
        wr = (wr + 1) % kPitchBuf;
        if (!active) return x;

        phase += 1.0f - ratio;              // ratio > 1: the head catches up

        float dist, other;
        if (ratio > 1.0f) {
            if (phase < lo()) {
                // A seam is the one moment the sweep length can change without
                // the delay jumping audibly: the crossfade is already covering it.
                if (pending != len) { len = pending; phase = std::clamp(phase, lo(), hi()); }
                phase += len;
                if (phase >= hi()) phase = hi() - 1.0f;
            }
            dist  = phase - lo();
            other = phase + len;
        } else {
            if (phase >= hi()) {
                if (pending != len) { len = pending; phase = std::clamp(phase, lo(), hi()); }
                phase -= len;
                if (phase < lo()) phase = lo();
            }
            dist  = hi() - phase;
            other = phase - len;
        }

        // Only blend near the seam. Holding a single head for most of the sweep
        // is what keeps the pitched tone intact: two heads summed the whole time
        // sit at a fixed delay apart and cancel each other wherever that offset
        // is a half period, which sounds hollow.
        const float zone = kPitchXfade * len;
        if (dist >= zone) return tap(phase);

        const float b = 1.0f - dist / zone;
        // Which crossfade law depends on whether the two taps are reading the
        // same thing. Once the sweep is a whole number of pitch periods they
        // are near-identical, and summing identical signals with the
        // constant-POWER law puts +3 dB in the middle of every seam - which is
        // a worse buzz than the one the alignment just removed. Correlated
        // wants constant amplitude; uncorrelated still wants constant power.
        float g0, g1;
        if (coherent) {
            g0 = 0.5f * (1.0f + std::cos(kPi * b));
            g1 = 1.0f - g0;
        } else {
            const float a = b * kPi * 0.5f;
            g0 = std::cos(a);
            g1 = std::sin(a);
        }
        return tap(phase) * g0 + tap(other) * g1;
    }
};

// ---------------------------------------------------------------------------
struct RingMod {
    float phase = 0.0f, inc = 0.0f, mix = 0.0f;
    bool  active = false;

    void configure(float sr, float hz, float m)
    {
        active = hz > 0.5f && m > 0.001f;
        inc = 2.0f * kPi * hz / sr;
        mix = clampf(m, 0.0f, 1.0f);
    }
    inline float process(float x)
    {
        if (!active) return x;
        phase += inc;
        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
        return x * (1.0f - mix) + x * std::sin(phase) * mix;
    }
    void reset() { phase = 0.0f; }
};

// ---------------------------------------------------------------------------
// Bit depth and sample rate, separately: one gives you quantisation grit, the
// other the aliasing whine of an old sampler.
struct Crusher {
    int   bits = 0;                     // 0 = off
    int   down = 1;                     // 1 = off
    int   count = 0;
    float held = 0.0f;

    inline float process(float x)
    {
        if (down > 1) {
            if (count == 0) held = x;
            if (++count >= down) count = 0;
            x = held;
        }
        if (bits >= 2 && bits < 16) {
            const float levels = float(1 << (bits - 1));
            x = std::round(x * levels) / levels;
        }
        return x;
    }
    void reset() { count = 0; held = 0.0f; }
};

// ---------------------------------------------------------------------------
constexpr int kChorusBuf = 4096;        // 85 ms at 48 kHz

struct Chorus {
    float buf[kChorusBuf] = {};
    int   wr = 0;
    float phase = 0.0f, inc = 0.0f;
    float base = 0.0f, depth = 0.0f;    // samples
    float mix = 0.0f;
    bool  active = false;

    void configure(float sr, float depth_ms, float hz, float m)
    {
        active = depth_ms > 0.01f && m > 0.001f;
        base  = 0.015f * sr;                                  // 15 ms centre
        depth = clampf(depth_ms, 0.0f, 12.0f) * 0.001f * sr;
        inc   = 2.0f * kPi * clampf(hz, 0.01f, 8.0f) / sr;
        mix   = clampf(m, 0.0f, 1.0f);
    }
    inline float process(float x)
    {
        buf[wr] = x;
        wr = (wr + 1) % kChorusBuf;
        if (!active) return x;
        phase += inc;
        if (phase > 2.0f * kPi) phase -= 2.0f * kPi;
        const float d = base + depth * std::sin(phase);
        float p = float(wr) - d;
        while (p < 0.0f) p += float(kChorusBuf);
        const int i0 = int(p) % kChorusBuf;
        const int i1 = (i0 + 1) % kChorusBuf;
        const float f = p - std::floor(p);
        const float wet = buf[i0] * (1.0f - f) + buf[i1] * f;
        return x * (1.0f - mix * 0.5f) + wet * mix;
    }
    void reset() { for (int i = 0; i < kChorusBuf; ++i) buf[i] = 0.0f; wr = 0; phase = 0.0f; }
};

// ---------------------------------------------------------------------------
constexpr int kEchoBuf = 48000;         // 1 s at 48 kHz

struct Echo {
    float buf[kEchoBuf] = {};
    int   wr = 0, delay = 0;
    float fb = 0.0f, mix = 0.0f;
    bool  active = false;

    void configure(float sr, float ms, float feedback, float m)
    {
        active = ms > 1.0f && m > 0.001f;
        delay = (int)clampf(ms * 0.001f * sr, 1.0f, float(kEchoBuf - 1));
        fb  = clampf(feedback, 0.0f, 0.95f);
        mix = clampf(m, 0.0f, 1.0f);
    }
    inline float process(float x)
    {
        if (!active) return x;
        int rd = wr - delay;
        if (rd < 0) rd += kEchoBuf;
        const float d = buf[rd];
        buf[wr] = x + d * fb;
        wr = (wr + 1) % kEchoBuf;
        return x + d * mix;
    }
    void reset() { for (int i = 0; i < kEchoBuf; ++i) buf[i] = 0.0f; wr = 0; }
};

// ---------------------------------------------------------------------------
// Reverb: eight damped comb filters in parallel into four allpasses in series.
//
// The Schroeder/Freeverb arrangement - decades old, cheap, and the reason a
// voice sounds like it is in a room rather than in a wire. The comb lengths are
// mutually prime so their echoes do not pile up into a ringing tone, and the
// damping is a one-pole lowpass inside each feedback loop, which is what makes
// the tail lose its highs the way a real room does.
//
// The right channel runs slightly longer delays than the left. That offset is
// the whole of the stereo image: without it both channels decay identically and
// the result collapses to the middle.
// ---------------------------------------------------------------------------
constexpr int kRvCombs   = 8;
constexpr int kRvAps     = 4;
constexpr int kRvMaxComb = 2048;
constexpr int kRvMaxAp   = 768;

struct Reverb {
    float comb[kRvCombs][kRvMaxComb] = {};
    float ap[kRvAps][kRvMaxAp] = {};
    int   comb_len[kRvCombs] = {}, comb_i[kRvCombs] = {};
    int   ap_len[kRvAps] = {}, ap_i[kRvAps] = {};
    float store[kRvCombs] = {};          // damping lowpass state, one per comb
    float feedback = 0.0f, damp1 = 0.0f, damp2 = 1.0f, mix = 0.0f;
    bool  active = false;

    void configure(float sr, int spread)
    {
        // Freeverb's tunings, quoted at 44.1 kHz and scaled to whatever we run at.
        static const int kComb[kRvCombs] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int kAp[kRvAps]     = { 556, 441, 341, 225 };
        const float k = sr / 44100.0f;
        for (int i = 0; i < kRvCombs; ++i) {
            comb_len[i] = std::min(int(kComb[i] * k) + spread, kRvMaxComb - 1);
            comb_i[i] = 0;
            store[i] = 0.0f;
            for (int j = 0; j < kRvMaxComb; ++j) comb[i][j] = 0.0f;
        }
        for (int i = 0; i < kRvAps; ++i) {
            ap_len[i] = std::min(int(kAp[i] * k) + spread, kRvMaxAp - 1);
            ap_i[i] = 0;
            for (int j = 0; j < kRvMaxAp; ++j) ap[i][j] = 0.0f;
        }
    }

    void set(float size, float damp, float m)
    {
        active = m > 0.001f;
        feedback = 0.70f + clampf(size, 0.0f, 1.0f) * 0.28f;   // 0.70 .. 0.98
        damp1 = clampf(damp, 0.0f, 1.0f) * 0.4f;
        damp2 = 1.0f - damp1;
        mix = clampf(m, 0.0f, 1.0f);
    }

    inline float process(float x)
    {
        if (!active) return x;
        const float in = x * 0.015f;          // the classic input gain; the combs are hot
        float wet = 0.0f;
        for (int i = 0; i < kRvCombs; ++i) {
            const float y = comb[i][comb_i[i]];
            store[i] = y * damp2 + store[i] * damp1;
            comb[i][comb_i[i]] = in + store[i] * feedback;
            if (++comb_i[i] >= comb_len[i]) comb_i[i] = 0;
            wet += y;
        }
        for (int i = 0; i < kRvAps; ++i) {
            const float y = ap[i][ap_i[i]];
            ap[i][ap_i[i]] = wet + y * 0.5f;
            wet = y - wet;
            if (++ap_i[i] >= ap_len[i]) ap_i[i] = 0;
        }
        return x * (1.0f - mix * 0.5f) + wet * mix;
    }
};

// ---------------------------------------------------------------------------
// tanh saturation, normalised so turning it up changes the shape rather than
// just the level.
inline float drive_shape(float x, float k)
{
    return std::tanh(x * k) / std::tanh(k);
}

// ---------------------------------------------------------------------------
// The whole rack for one strip. Mirrors EqChain: cached parameters, an
// update() off the shm block, and a per-sample process().
// ---------------------------------------------------------------------------
struct VoiceFxChain {
    PitchShifter   pitch[kChan];
    FormantShifter formant[kChan];
    PeriodTracker  period;          // one voice, so one period for both channels
    RingMod      ring[kChan];
    Crusher      crush[kChan];
    Chorus       chorus[kChan];
    Echo         echo[kChan];
    Reverb       reverb[kChan];
    SmoothGain   out[kChan];
    float        drive_k = 1.0f;
    bool         drive_on = false;

    float c_pitch = 1e9f, c_fmt = 1e9f, c_drive = -1.0f;
    int   c_fmt_on = -1;
    float c_rhz = -1.0f, c_rmix = -1.0f;
    int   c_bits = -1, c_down = -1;
    float c_ems = -1.0f, c_efb = -1.0f, c_emix = -1.0f;
    float c_cms = -1.0f, c_chz = -1.0f, c_cmix = -1.0f;
    float c_vsize = -1.0f, c_vdamp = -1.0f, c_vmix = -1.0f;
    bool  init = false;

    void configure(float sr)
    {
        for (int c = 0; c < kChan; ++c) {
            out[c].configure(sr, 15.0f);
            out[c].snap(1.0f);
            pitch[c].reset();
            formant[c].configure();
            // The right channel's delays run a little longer; that offset is
            // the entire stereo image of the reverb.
            reverb[c].configure(sr, c == 0 ? 0 : 23);
        }
        period.reset();
    }

    void update(const VoiceFx& p, float sr)
    {
        const float st   = p.pitch.load(std::memory_order_relaxed);
        const float fst  = p.formant.load(std::memory_order_relaxed);
        const int   fon  = p.formant_on.load(std::memory_order_relaxed);
        const float dr   = p.drive.load(std::memory_order_relaxed);
        const float rhz  = p.ring_hz.load(std::memory_order_relaxed);
        const float rmix = p.ring_mix.load(std::memory_order_relaxed);
        const int   bits = p.bits.load(std::memory_order_relaxed);
        const int   down = p.downsample.load(std::memory_order_relaxed);
        const float ems  = p.echo_ms.load(std::memory_order_relaxed);
        const float efb  = p.echo_fb.load(std::memory_order_relaxed);
        const float emix = p.echo_mix.load(std::memory_order_relaxed);
        const float cms  = p.chorus_ms.load(std::memory_order_relaxed);
        const float chz  = p.chorus_hz.load(std::memory_order_relaxed);
        const float cmix = p.chorus_mix.load(std::memory_order_relaxed);
        const float rsz  = p.reverb_size.load(std::memory_order_relaxed);
        const float rdp  = p.reverb_damp.load(std::memory_order_relaxed);
        const float rmx  = p.reverb_mix.load(std::memory_order_relaxed);

        for (int c = 0; c < kChan; ++c) {
            if (!init || st != c_pitch) pitch[c].set_semitones(st);
            // The pitch stage has already moved the formants by its own ratio,
            // so this stage only has to make up the difference. That is what
            // lets the two controls read as absolutes: "pitch +6, formant +3"
            // means exactly that, and leaving formant at 0 while pitching up
            // pulls the formants back down to where they started.
            if (!init || fst != c_fmt || st != c_pitch || fon != c_fmt_on)
                formant[c].set_shift(fon ? fst - st : 0.0f);
            if (!init || rhz != c_rhz || rmix != c_rmix) ring[c].configure(sr, rhz, rmix);
            if (!init || bits != c_bits || down != c_down) {
                crush[c].bits = bits;
                crush[c].down = down < 1 ? 1 : down;
            }
            if (!init || ems != c_ems || efb != c_efb || emix != c_emix)
                echo[c].configure(sr, ems, efb, emix);
            if (!init || cms != c_cms || chz != c_chz || cmix != c_cmix)
                chorus[c].configure(sr, cms, chz, cmix);
            if (!init || rsz != c_vsize || rdp != c_vdamp || rmx != c_vmix)
                reverb[c].set(rsz, rdp, rmx);
            out[c].set_target(db_to_lin(p.gain_db.load(std::memory_order_relaxed)));
        }
        if (!init || dr != c_drive) {
            drive_on = dr > 0.01f;
            drive_k = 1.0f + clampf(dr, 0.0f, 10.0f) * 1.5f;
        }
        c_pitch = st; c_fmt = fst; c_fmt_on = fon; c_drive = dr;
        c_rhz = rhz; c_rmix = rmix;
        c_bits = bits; c_down = down;
        c_ems = ems; c_efb = efb; c_emix = emix;
        c_cms = cms; c_chz = chz; c_cmix = cmix;
        c_vsize = rsz; c_vdamp = rdp; c_vmix = rmx;
        init = true;
    }

    inline float process(int c, float x)
    {
        // Track on the left channel only and share the answer: it is one voice,
        // and both shifters must splice at the same length to stay coherent.
        // Only worth tracking when the pitch stage is actually running; a rack
        // doing nothing but echo should not pay for an autocorrelation.
        if (c == 0 && pitch[0].active) {
            period.push(x);
            const float p = period.period;
            for (int i = 0; i < kChan; ++i) pitch[i].set_period(p);
        }
        x = pitch[c].process(x);
        x = formant[c].process(x);
        if (drive_on) x = drive_shape(x, drive_k);
        x = ring[c].process(x);
        x = crush[c].process(x);
        x = chorus[c].process(x);
        x = echo[c].process(x);
        // Reverb last: it should be the room the finished voice is standing in,
        // not something the distortion then chews on.
        x = reverb[c].process(x);
        return x * out[c].next();
    }
};

} // namespace bb
