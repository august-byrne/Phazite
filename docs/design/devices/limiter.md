# Limiter

*Design section 9.*

A look-ahead limiter, meant as the master's last device: it raises the sound by its gain and keeps it under a ceiling,
between the samples too (true peaks).

## Parameters

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Gain | 0 to +24 dB | 0 dB | Raises the sound before it is limited. |
| Ceiling | -12 to 0 dB | -1 dB | The level nothing goes over. |
| Release | 1 to 1000 ms | 100 ms | The time constant with which the gain comes back after a peak. |

The Limiter has no Mix: mixed with the sound as it came in, peaks would pass the ceiling. Its runner blends nothing
back in, and the device's row in the Devices view has no Mix to turn.

## How it works

The gain glides to where it is set over about 20 ms. For each sample, the limiter finds the peak it has to keep under
the ceiling: the sample itself and the points of the waveform between it and its neighbours on either side, as a
converter would rebuild them. It finds those points as ITU-R BS.1770-5 (Annex 2) does, oversampling four times with
the Recommendation's 48-tap interpolating filter (`src/devices/dynamics/TruePeak.h`). The master's loudness meter reads
true peaks with the same detector, so the two agree.

The gain a peak needs is the ceiling divided by the peak. That gain is held at the least needed over the last 2 ms
(a running minimum), let back up with the Release as a time constant, and then averaged over the same 2 ms, so that
it ramps down in a straight line and is all the way down as the peak plays. The sound itself is delayed by the 2 ms
and by what the true-peak filter reads after a sample, and multiplied by that gain. A final clamp at the ceiling
catches rounding only, so no sample ever goes over the ceiling.

Both channels are turned down alike, by the louder one's peak.

Its latency is the look-ahead and five samples: 101 samples at 48 kHz. The engine makes up for it on the other
tracks. While Low Latency is on, the limiter is left out of what plays and the sound passes as it comes.

It keeps three meters for its editor: the loudest sample coming in (after the gain), the most it turned the sound
down, and the loudest sample going out.

## Its editor

- **History:** the last two bars at the song's tempo, scrolling from right to left over lines at the beats and bars:
  what comes in (after the gain), pale; what goes out, in the device's colour; and how far the sound is turned down,
  in red, hanging from the top, the deepest dips numbered. The ceiling is a line across it: near it the pointer
  becomes an up-and-down arrow, the line lights up and the hint bar says what a drag does; a drag moves the Ceiling,
  as one undo step. Each reading is the most the meters reached since the one before, drawn across the time it
  covers, so each hit's reduction shows however short; shown again after it was hidden, the history starts empty.
- Beside it, three meters: **In**, **GR** (gain reduction, from the top) and **Out**, with the ceiling marked on In
  and Out.
- **Gain**, as a large knob, then **Ceiling** and **Release**.

## Limits

- The look-ahead is fixed at 2 ms and its ramp is a straight line; it cannot be shortened or lengthened.
- One release time: no automatic or two-stage release, and no choice of limiting style.
- The channels are always linked: no unlinking and no mid/side.
- No dither, and no loudness or true-peak numbers in the device: those are the master's loudness meter's.
- True peaks are found at four times the rate, which reads a bright sine's peak within a quarter of a decibel; a meter
  that looks more finely can read a true peak that much over the ceiling.
- The gain change is not oversampled.
- No switch to hear the limiting at unity gain.
- The history takes the meters once at each refresh of the display, the most they reached since the last: a short peak
  shows at its depth, but the release's shape within a refresh draws coarsely. Shown again after it was hidden, its
  meters and its history start from the sound now.
- A drag of the ceiling's line starts from the Ceiling as modulation plays it, and sets that as the value by hand.

## In the code

- DSP: `src/devices/dynamics/Limiter.h`, `src/devices/dynamics/Limiter.cpp`; the true-peak detector in
  `src/devices/dynamics/TruePeak.h` (see [dsp-audit.md](../../archive/dsp-audit.md)).
- Editor: `src/ui/devices/LimiterEditor.h`, `src/ui/devices/LimiterEditor.cpp`; the history in
  `src/ui/devices/LevelHistoryView.h`.
- Tests: `tests/LimiterTests.cpp`, `tests/TruePeakTests.cpp`, `tests/LimiterEditorTests.cpp`.
- Presets: `resources/presets/Limiter/` (see [presets.md](../../presets.md)).
