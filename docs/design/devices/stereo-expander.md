# Stereo Expander

*Design section 9.*

Makes a sound wider or narrower without changing how it sounds in mono. It also keeps the bass in the middle, flips
either side's polarity and has a gain, so it does part of what other programs' utility devices do. It changes nothing
until a control is turned.

## Parameters

The editor groups them as Width, and Bass and Polarity.

### Width

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Width | 0 to 200 % | 100 % | Turns the side (what differs between the left and the right) down to mono at 0 % or up to twice as wide at 200 %. |
| Spread | 0 to 100 % | 0 % | Makes stereo from the middle, for a mono sound, without changing the mono sum. |

### Bass and Polarity

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Bass Mono | Off, On | Off | Keeps what is below the Bass Mono Frequency in the middle. |
| Bass Mono Frequency | 40 to 500 Hz | 120 Hz | Where Bass Mono splits, and below which Spread spreads nothing, whether Bass Mono is on or off. The editor calls it Frequency. |
| Gain | -36 to +36 dB | 0 dB | A gain on what goes out. |
| Invert Left | Off, On | Off | Flips the left side's polarity. The editor calls it Flip Left. |
| Invert Right | Off, On | Off | Flips the right side's polarity. The editor calls it Flip Right. |

It has no Mix parameter: all of what it puts out plays.

## How it works

Each side's polarity is flipped first, so that the rest works on the sound put right. The sound then goes through mid
and side: the middle is half the sum of the two sides and the side half their difference.

Spread adds to the side a copy of the middle smeared over 20 ms by velvet noise (Alary et al., 2017): 30 pulses, one
in each stretch of the 20 ms at a place and of a sign drawn from a fixed seed, each quieter than the one before, and
all together as loud as the middle itself. It sounds wide and takes no delay; and since only the side changes, the
left plus the right stays the middle twice, so the mono sum never changes, unlike the comb a Haas delay makes in mono.
The middle that Spread reads is first cut below the Bass Mono Frequency at 24 dB an octave, so the bass is never
spread.

The side is then multiplied by the Width.

Bass Mono takes the side's bass away with a Linkwitz-Riley crossover of 24 dB an octave and passes the middle through
the matching all-pass, so that the two stay in phase. Both run all the time, so switching Bass Mono crossfades. The
frequency glides in octaves when it is turned.

The gain comes last. Every change glides over about 20 ms. A mono channel gets the gain and the left's polarity only.
The device measures how alike the two sides are, coming in and going out, from -1 (each the other upside down)
through 0 (unrelated) to +1 (the same), averaged over about half a second, for its editor. It has no latency.

## Its editor

A half disc of what it plays, in the manner of iZotope's Imager: each moment a dot, mono standing straight up and the
two sides out of step lying flat, over the faint dots of what comes in, with the L and R lines that are safe to stay
within, and a fan opened as wide as the Width. Under it is a bar of how alike the two sides are, from -1 to +1, red
below 0, where the mono sum loses sound, with a mark for what comes in.

A drag across the disc turns the fan's edge as far as the pointer turns around the disc's centre, from the Width set
by hand (a press changes nothing, and Escape takes the drag back), as one undo step, and the wheel opens or closes it a
step at a time. Beside the picture are Width and Spread as large knobs, then Bass Mono, Frequency, Gain, Flip Left and
Flip Right.

## Limits

- It is not a full utility device: no pan or balance (a track's pan does that), no channel swap, no mono switch other
  than Width at 0 %, and no DC filter.
- One band: no width per frequency band, as iZotope's Imager has.
- No Mix, so it can't be blended with the sound coming in.
- The picture is mirrored: a sound on the left side alone is drawn along the R line.
- The picture's dots are scaled so that the loudest reaches the rim, so it shows the image's shape but not its level.

## In the code

- DSP: `src/devices/stereo/StereoExpander.h`, `src/devices/stereo/StereoExpander.cpp`; the crossover and its
  all-pass, `src/devices/dsp/Crossover.h`.
- Editor: `src/ui/devices/StereoExpanderEditor.h`, `src/ui/devices/StereoExpanderEditor.cpp`.
- Tests: `tests/StereoExpanderTests.cpp`, `tests/EffectDeviceTests.cpp`, `tests/EffectEditorTests.cpp` and
  `tests/GraphEditingTests.cpp` (the editor), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Stereo Expander/` ([presets.md](../../presets.md)).
