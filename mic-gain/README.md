# mic-gain

A microphone analyzer for PipeWire that tells you which knob to turn, and which
way, instead of leaving you to guess by ear.

## Why

Monitoring level and recording level are two different things, and on a USB
interface they are two different knobs. The headphone dial on a UMC202HD sits
*after* the converter, so turning it up makes a whisper-quiet mic sound perfect,
and turning it down makes a clipping mic sound tame. Your ears cannot tell you
where the gain belongs — by design, the level you hear is not the level that
gets recorded.

Put an external channel strip like a dbx 286s in front and it gets worse: now
three knobs change the level, and only one of them should.

So `mic-gain` measures the signal on the way in and reduces it to instructions
naming the control to move and the direction to move it.

## Install

```sh
./install.sh
```

Copies the script to `~/.local/bin/mic-gain`. No root, no build step.

Needs `python3` and PipeWire (`pw-record`, `pactl`) — both already present on
any PipeWire desktop. There are no third-party dependencies: the spectrum uses
a hand-rolled radix-2 FFT rather than numpy, matching the other tools here.

## Use

```sh
mic-gain                       # live analyzer: meters, spectrum, diagnosis
mic-gain --calibrate           # guided measurement -> a written settings report
mic-gain --list                # show the inputs it can measure
mic-gain --preamp 'dbx 286s'   # name that strip's controls in the advice
mic-gain -s bb_b1              # measure what applications actually record
mic-gain -p stream             # aim for streaming levels instead
```

In the live view: `q` quits, `r` resets the statistics, and `1` / `2` / `3`
switch between level only, spectrum only, and both.

Use `--ascii` on a terminal that renders box-drawing characters double-width;
every such glyph is East-Asian-ambiguous and will otherwise shear the frame.

### External channel strips

`--preamp NAME` tells the tool a strip sits between the mic and the interface.
Level advice then points at the strip's OUTPUT gain rather than the interface,
and every other control it names is labelled as belonging to that strip.

The staging it assumes: set the interface gain once to a clean reference and
leave it — using the 1/4" TRS side of the combo jack, which is the line input,
since an XLR cable puts the strip's line-level output back into the interface's
own mic preamp and stacks two of them. Set the strip's INPUT gain for the
compression you want, watching its gain-reduction meter, because that knob
changes the sound and not just the level. Then set recording level with the
strip's OUTPUT gain.

Knob-degree hints ("turn it about 40° clockwise") assume roughly 50 dB over a
300° sweep, which is the interface knob. Under `--preamp` they are suppressed
unless `MIC_DEG_PER_DB` is set, because the taper of a knob the tool cannot see
is not worth guessing at.

## What it measures

The statistics are picked to survive what makes naive meters lie:

- **Speech level** is a 300 ms sliding RMS, gated to drop the pauses between
  words. An ungated average is mostly silence and reads absurdly low.
- **The gate** cannot be derived from the signal alone — someone who talks
  without pausing makes the "floor" land on speech and gates everything out —
  so it is also capped relative to the loud end, and `--calibrate` measures a
  real floor during a dedicated silent phase.
- **Headroom** uses the 95th percentile of true sample peaks, so one chair creak
  does not cost 6 dB of signal. The absolute maximum is kept for clipping.
- **The recommendation** is the smaller of "what centres the average" and "what
  keeps peaks safe", because peaks ruin a take and the average only makes it
  comfortable.
- **Spectral analysis** is speech-gated too. Analysing the whole stream measures
  the room's air conditioning, not the voice.
- **Channels** are measured separately and a dead one is excluded, since on a
  two-input interface the mic is usually on input 1 and input 2 is hiss.

Level, peak, crest and clipping are exact measurements. The tone findings are
heuristics on *ratios* between frequency bands — never absolute levels, so the
gain knob cannot change them — and each one prints the number it came from so
you can disagree with it.

## Cost

About 6% of one core for the full live view, measured; the FFT (n=2048, ~1.3 ms)
is roughly half of that. `--calibrate` is cheaper still.

## Environment

| Variable | Effect |
| --- | --- |
| `MIC_SOURCE` | Default source name (overridden by `--source`) |
| `MIC_PROFILE` | Default profile: `voice`, `stream` or `safe` |
| `MIC_PREAMP` | External channel strip, e.g. `dbx 286s` |
| `MIC_DEG_PER_DB` | Knob degrees per dB for the "how far to turn" hint |

## Note

This is a standalone tool. It shares a repository with BetterBanana but does not
depend on it — it talks to PipeWire directly. It is useful alongside it, though:
pointing it at a BetterBanana bus with `-s bb_b1` measures what applications
actually receive, after that processing, rather than what the interface captures.
