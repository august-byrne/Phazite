# Stereo

*Design section 9.*

The one device for a channel's two sides: it makes a sound wider or narrower without changing how it sounds in mono,
keeps the bass in the middle, balances the sides, plays them in mono to check a mix, swaps them, flips either side's
polarity, takes off an offset and sets the gain, as other programs' utility devices do. It changes nothing until a
control is turned.

## Parameters

The editor groups them as Width, Bass, Sides and Output.

### Width

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Width | 0 to 200 % | 100 % | Turns the side (what differs between the left and the right) down to mono at 0 % or up to twice as wide at 200 %. |
| Spread | 0 to 100 % | 0 % | Makes stereo from the middle, for a mono sound, without changing the mono sum. |

### Bass

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Bass Mono | Off, On | Off | Keeps what is below the Bass Mono Frequency in the middle. |
| Bass Mono Frequency | 40 to 500 Hz | 120 Hz | Where Bass Mono splits, and below which Spread spreads nothing, whether Bass Mono is on or off. The editor calls it Frequency. |

### Sides

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Balance | 100 % left to 100 % right | Center | Fades the side it is turned away from; the side it is turned toward stays as it is, so that nothing is ever louder. Shown as a track's pan is. |
| Mono | Off, On | Off | Both sides play the middle, as Width at 0 % with no Spread does: a switch for checking a mix in mono. |
| Swap Sides | Off, On | Off | The left and the right change places. The editor calls it Swap. |
| Invert Left | Off, On | Off | Flips the polarity of what comes in on the left, wherever it goes out. The editor calls it Flip Left. |
| Invert Right | Off, On | Off | Flips the polarity of what comes in on the right. The editor calls it Flip Right. |

### Output

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| DC Filter | Off, On | Off | Takes away an offset and what wanders a few hertz at most, at 12 dB an octave below 5 Hz. |
| Gain | -36 to +36 dB | 0 dB | A gain on what goes out. |

It has no Mix parameter: all of what it puts out plays.

## How it works

In this order, each sample:

1. Each side's polarity is flipped, so that the rest works on the sound put right.
2. Swap Sides: the left and the right change places, crossfaded over about 20 ms as it is switched, so that it never
   clicks.
3. The sound goes through mid and side: the middle is half the sum of the two sides and the side half their
   difference. Spread adds to the side a copy of the middle smeared over 20 ms by velvet noise (Alary et al., 2017): 30
   pulses, one in each stretch of the 20 ms at a place and of a sign drawn from a fixed seed, each quieter than the one
   before, and all together as loud as the middle itself. It sounds wide and takes no delay; and since only the side
   changes, the left plus the right stays the middle twice, so the mono sum never changes, unlike the comb a Haas delay
   makes in mono. The middle that Spread reads is first cut below the Bass Mono Frequency at 24 dB an octave, so the
   bass is never spread. The side is then multiplied by the Width. Bass Mono takes the side's bass away with a
   Linkwitz-Riley crossover of 24 dB an octave and passes the middle through the matching all-pass, so that the two
   stay in phase. Both run all the time, so switching Bass Mono crossfades. The frequency glides in octaves when it is
   turned.
4. Mono turns what is left of the side down to nothing, so that both sides play the middle.
5. Balance: for a balance b from -1 (left) to 1 (right), the right is turned down by cos (π/2 · max (0, -b)) and the
   left by cos (π/2 · max (0, b)): at the centre both stay exactly as they are, and turned all the way one side is
   silent.
6. The DC filter: a 12 dB high-pass at 5 Hz on each side (`src/devices/dsp/DcBlocker.h`), as the instruments have. It
   turns the phase of the lows, so it plays only while it is on: switched on, it starts at rest and fades in over 5 ms;
   switched off, it fades out over as long and stops, and the sound passes as it was, bit for bit.
7. The gain.

Every change of a knob or a switch glides over about 20 ms (the DC filter's over 5 ms). A mono channel gets the left's
polarity, the DC filter and the gain only. The device measures how alike the two sides are, coming in and going out
(after the balance, Mono and the gain), from -1 (each the other upside down) through 0 (unrelated) to +1 (the same),
averaged over about half a second, for its editor. It has no latency.

## Its editor

A half disc of what it plays, in the manner of iZotope's Imager: each moment a dot, mono standing straight up and the
two sides out of step lying flat, over the faint dots of what comes in, with the L and R lines that are safe to stay
within, and a fan opened as wide as the Width. Under it is a bar of how alike the two sides are, from -1 to +1, red
below 0, where the mono sum loses sound, with a mark for what comes in.

A drag across the disc turns the fan's edge as far as the pointer turns around the disc's centre, from the Width set
by hand (a press changes nothing, and Escape takes the drag back), as one undo step, and the wheel opens or closes it a
step at a time. Beside the picture are its parts, in the order the sound goes through them: Width (Width and Spread as
large knobs) over Bass (Bass Mono and Frequency), then Sides (Balance, Mono, Swap, Flip Left and Flip Right) over Output
(DC Filter and Gain).

## Limits

- One band: no width per frequency band, as iZotope's Imager has.
- No Mix, so it can't be blended with the sound coming in.
- One balance law: no choice of pan laws. Moving the sound around (auto-pan, tremolo) is a modulator's work on Balance
  ([decisions.md](../decisions.md)).
- The picture's dots are scaled so that the loudest reaches the rim, so it shows the image's shape but not its level.

## In the code

- DSP: `src/devices/stereo/Stereo.h`, `src/devices/stereo/Stereo.cpp`; the crossover and its all-pass,
  `src/devices/dsp/Crossover.h`; the DC filter, `src/devices/dsp/DcBlocker.h`.
- Editor: `src/ui/devices/StereoEditor.h`, `src/ui/devices/StereoEditor.cpp`.
- Tests: `tests/StereoTests.cpp`, `tests/EffectDeviceTests.cpp`, `tests/EffectEditorTests.cpp` and
  `tests/GraphEditingTests.cpp` (the editor), `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Stereo/` ([presets.md](../../presets.md)).
