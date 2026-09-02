// Spectrum analyser: the FFT itself, the calibration of the log bands, and the
// tap the audio thread writes into.
#include "../engine/spectrum.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace bb;

static int g_fail = 0, g_total = 0;

static void check(bool ok, const char* what)
{
    ++g_total;
    if (!ok) { ++g_fail; std::printf("  FAIL  %s\n", what); }
}

static void near(double got, double want, double tol, const char* what)
{
    ++g_total;
    if (std::fabs(got - want) > tol) {
        ++g_fail;
        std::printf("  FAIL  %s: got %.3f want %.3f (+-%.3f)\n", what, got, want, tol);
    }
}

// Which display band contains `f`.
static int band_of(float f, float lo = 20.0f, float hi = 20000.0f)
{
    const double ratio = std::pow((double)hi / lo, 1.0 / kSpecBins);
    return (int)std::floor(std::log((double)f / lo) / std::log(ratio));
}

static void fill_sine(float* buf, int n, double freq, double sr, double amp)
{
    for (int i = 0; i < n; ++i) buf[i] = (float)(amp * std::sin(2.0 * M_PI * freq * i / sr));
}

int main()
{
    std::printf("test_spectrum\n");

    // --- the transform itself ----------------------------------------------
    {
        // A DC signal puts all its energy in bin 0 and nothing anywhere else.
        std::vector<float> re(64, 1.0f), im(64, 0.0f);
        fft(re.data(), im.data(), 64);
        near(re[0], 64.0, 1e-3, "fft: DC lands in bin 0");
        double rest = 0.0;
        for (int i = 1; i < 64; ++i) rest += std::fabs(re[i]) + std::fabs(im[i]);
        near(rest, 0.0, 1e-3, "fft: DC leaks nowhere else");
    }
    {
        // A cosine at exactly bin 4 puts n/2 in bins 4 and 60 and nothing else.
        const int n = 64;
        std::vector<float> re(n), im(n, 0.0f);
        for (int i = 0; i < n; ++i) re[i] = (float)std::cos(2.0 * M_PI * 4 * i / n);
        fft(re.data(), im.data(), n);
        near(std::hypot(re[4], im[4]), n / 2.0, 1e-2, "fft: tone lands in its own bin");
        near(std::hypot(re[8], im[8]), 0.0,     1e-2, "fft: tone leaks nowhere else");
    }

    // --- calibration --------------------------------------------------------
    SpectrumAnalyzer an;
    an.configure();
    std::vector<float> buf(kSpecFft);

    {
        // A full-scale 1 kHz sine must read 0 dBFS in the band that holds it.
        fill_sine(buf.data(), kSpecFft, 1000.0, 48000.0, 1.0);
        an.reset();
        an.analyze(buf.data(), 48000.0f, 20.0f, 20000.0f, 0.0f);
        const int b = band_of(1000.0f);
        near(an.disp[b], 0.0, 0.6, "1 kHz full scale reads 0 dBFS");

        // and nowhere near it.
        check(an.disp[b - 6] < -40.0f, "1 kHz tone does not smear two octaves down");
        check(an.disp[b + 6] < -40.0f, "1 kHz tone does not smear two octaves up");
    }
    {
        // Half amplitude is 6 dB down, wherever it sits.
        fill_sine(buf.data(), kSpecFft, 4000.0, 48000.0, 0.5);
        an.reset();
        an.analyze(buf.data(), 48000.0f, 20.0f, 20000.0f, 0.0f);
        near(an.disp[band_of(4000.0f)], -6.02, 0.6, "half scale reads -6 dBFS");
    }
    {
        // The bottom bands are narrower than one FFT bin; they must still
        // report the tone rather than silence.
        fill_sine(buf.data(), kSpecFft, 50.0, 48000.0, 1.0);
        an.reset();
        an.analyze(buf.data(), 48000.0f, 20.0f, 20000.0f, 0.0f);
        check(an.disp[band_of(50.0f)] > -6.0f, "50 Hz tone is found in a sub-bin-wide band");
    }
    {
        // Silence sits on the floor everywhere.
        std::memset(buf.data(), 0, sizeof(float) * kSpecFft);
        an.reset();
        an.analyze(buf.data(), 48000.0f, 20.0f, 20000.0f, 0.0f);
        bool floored = true;
        for (int b = 0; b < kSpecBins; ++b)
            if (an.disp[b] > SpectrumAnalyzer::kFloorDb + 0.01f) floored = false;
        check(floored, "silence reads as the floor in every band");
    }
    {
        // Decay: a band that was loud falls by exactly fall_db per quiet call.
        fill_sine(buf.data(), kSpecFft, 1000.0, 48000.0, 1.0);
        an.reset();
        an.analyze(buf.data(), 48000.0f, 20.0f, 20000.0f, 0.0f);
        const int b = band_of(1000.0f);
        const float loud = an.disp[b];
        std::memset(buf.data(), 0, sizeof(float) * kSpecFft);
        an.analyze(buf.data(), 48000.0f, 20.0f, 20000.0f, 3.0f);
        near(an.disp[b], loud - 3.0, 0.01, "a band falls by fall_db when the tone stops");
    }

    // --- the tap ------------------------------------------------------------
    {
        SpecTap tap;
        // Write more than the window, then check we get the newest samples.
        std::vector<float> block(2 * kChan);
        for (int i = 0; i < 6000; ++i) {
            block[0] = (float)i; block[1] = (float)i;         // frame 2i
            block[2] = (float)i; block[3] = (float)i;         // frame 2i+1
            tap.write_stereo(block.data(), 2);
        }
        std::vector<float> out(kSpecFft);
        tap.snapshot(out.data(), kSpecFft);
        check(out[kSpecFft - 1] == 5999.0f, "tap snapshot ends at the newest frame");
        check(out[0] == out[1], "tap snapshot is contiguous");

        // Stereo is summed to mono, not just left-channel.
        SpecTap t2;
        float st[kChan] = { 1.0f, -1.0f };
        for (int i = 0; i < kSpecFft; ++i) t2.write_stereo(st, 1);
        std::vector<float> o2(kSpecFft);
        t2.snapshot(o2.data(), kSpecFft);
        near(o2[kSpecFft - 1], 0.0, 1e-6, "tap sums L and R");
    }

    std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
