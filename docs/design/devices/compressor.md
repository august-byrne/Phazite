# Compressor

*Design section 9.*

A compressor with two modes. **Compress** turns each level of its own sound up or down as a drawn curve says, in one
of five characters. **Duck** turns the sound down while another track plays (its key, the track its sidechain listens
to), for sidechain pumping.

## Parameters

### Both modes

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | Compress, Duck | Compress | Which of the two the device does. The editor's two tabs set it. |
| Attack | 0.1 to 200 ms | 10 ms | Compress: the time constant of the gain going down. Duck: the time the sound takes to go down by the whole Depth. |
| Release | 5 to 2000 ms | 120 ms | Compress: the time constant of the gain coming back. Duck: the time the sound takes to come back. |
| Sidechain Low Cut | 20 to 500 Hz | 20 Hz | A 12 dB per octave low cut on what the detector hears, so that bass doesn't pump it. Labelled Low Cut. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the compressor's sound, for parallel compression; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

### Compress

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Character | Clean, VCA, FET, Opto, Tube | Clean | How the compressor hears and reacts (below). |
| Input Gain | -24 to +24 dB | 0 dB | A gain before the compressor, which also drives its detector. Labelled In. |
| Makeup | -12 to +24 dB | 0 dB | A gain after it. Labelled Out. |
| Drive | 0 to 100 % | 0 % | Saturates what goes out in the character's colour, up to 18 dB into a soft curve and back out at the level it went in. |
| Points | 2 to 8 | 4 | How many points the curve has. |
| Point 1 to 8 X | 0 to 100 % | The default curve | The point's place across the curve's axis (level in): silence at 0 %, 0 dB at 50 %, +12 dB at 100 %. |
| Point 1 to 8 Y | 0 to 100 % | The default curve | The point's place up the same axis (level out). |
| Point 1 to 8 Tension | -100 to +100 % | The default curve | How the line to the point bends. |
| Point 1 to 8 Type | Single Curve, Double Curve, Hold, Stairs, Smooth Stairs | Single Curve | The shape of the line to the point. |

The default curve compresses 4:1 over -18 dB. Points past the count repeat the last one. The points are parameters
like any other, but are edited by dragging the curve.

### Duck

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Threshold | -60 to 0 dB | -24 dB | The key's level that starts a duck. |
| Depth | 0 to 60 dB | 12 dB | How far the sound goes down once the key is 6 dB or more over the threshold; less far over, a straight share of it. |
| Hold | 0 to 1000 ms | 0 ms | The least time the sound stays down, counted from the hit. |
| Release Curve | -100 to +100 % | 0 % | Bends the way back: 0 is a straight line in decibels, above 0 it is slow at first, below 0 quick at first. Labelled Curve. |
| Sidechain High Cut | 100 Hz to 20 kHz | 20 kHz (off) | A 24 dB per octave high cut on the key, so that only a kick's body is heard from a whole kit. Labelled High Cut. |

Each mode ignores the other's parameters. While it ducks, the curve, Character, Input Gain, Drive and Makeup do
nothing; while it compresses, Threshold, Depth, Hold, Release Curve and the Sidechain High Cut do nothing.

## How it works

Both modes turn both channels by the same gain, worked out from the louder one, so the stereo image holds.

### Compress

The compressor is feed-forward: its detector hears the sound as it comes in, after the Input Gain and through the
Sidechain Low Cut, never what it puts out. So once settled it follows the curve exactly, whatever its character. It
does not hear a sidechain in this mode; one that is set is kept for Duck.

The curve gives level out against level in, through up to eight points, on axes where an amplitude `a` sits at
`ln (1 + 2a) / ln 9`: silence at one end, 0 dB in the middle, +12 dB at the other. A line is straight on those axes,
bent by its tension, or shaped as an S, a hold or stairs. By its shape alone the curve compresses or expands, downward
or upward, limits or gates; there is no threshold, ratio or knee. The compressor reads the gain for a level from a
table with an entry every quarter of a decibel from -120 dB up, made again only when the curve changes. The curve,
its editor and its shapes are the [Multiband](multiband.md)'s too.

The gain moves in decibels with the Attack as it goes down and the Release as it comes back, as time constants: after
that time it has gone 63 % of the way (Giannoulis, Massberg and Reiss, 2012).

The characters:

- **Clean** hears each peak and adds no colour.
- **VCA** hears the average level (RMS over about 10 ms, scaled so that a steady sine reads as its peak), so short
  peaks pass.
- **FET** hears each peak, and adds odd harmonics the further it turns down: a tenth of Drive's range once it turns
  down 12 dB. How far it turns down is followed smoothly (over about 20 ms), sample by sample, so the colour plays the
  same however the song's blocks are cut.
- **Opto** hears the average, and lets go in two stages: at the Release for the first half of the way back, then six
  times slower, and up to twelve times slower again after it has held more than 3 dB of reduction for a while.
- **Tube** hears the average, and adds even harmonics the further it turns down, as FET adds odd ones.

Drive pushes what goes out up to 18 dB into a curve and takes the same gain back off: `tanh` for Clean and FET, with
a share of `tanh` squared for VCA (a little), Opto (more) and Tube (the most), which adds even harmonics and whose DC
is filtered out. Over the first tenth of Drive's travel (1.8 dB into the curve) the curve fades in from a straight
line, so the sound never jumps as Drive leaves 0: the curve came in whole at once, which took a full-scale peak 2.4 dB
down. FET's and Tube's colour fades its curve in over their first 3 dB of reduction (a tenth of a dB of reduction
colours a hair), and from there on they have the whole curve, as they were tuned by ear: at 6 dB of reduction their
harmonics are the whole curve's, within half a decibel. The curve plays at twice the sample rate by its
antiderivative (first-order antiderivative anti-aliasing: Parker, Zavalishin and Le Bivic, 2016), so that its
harmonics don't fold back as aliasing and what it leaves straight stays level to 20 kHz; then a first-order all-pass
makes its delay up to a whole 6 samples (it is 5.137 at low frequencies), as JUCE's oversampler makes up its own.
While the curve plays no part (FET with nothing turned down, say), the straight line alone plays. While nothing drives
the sound it takes a plain delay of the same length, and the two paths cross-fade over 1.5 ms, once the drive has
played 256 samples from silence for what its filters start with to ring out, or 150 ms for a curve with even
harmonics (VCA, Opto and Tube) whose curve is already in, for the filter that takes their offset out to settle: so that
no offset comes in with it, a thump, as Duck turns to Compress with Drive up. Drive leaving 0 starts with its curve out,
so the filter settles as the curve comes in, and the drive is heard after 256 samples on any character. A change of
character while the drive is heard warms the new curve up beside the one playing, on the same sound (256 samples, or
150 ms for one with even harmonics), and the sound then fades from one to the other over 1.5 ms: it stays driven all
through, its level moving no more than either curve's, and no offset comes in. The copy of what came in for the mix goes through the same
filters while the drive plays (and, where the sound then takes Duck's latency, the same delay after them, as it does
while ducking), so that the two meet in phase at any mix, from the moment the Mix leaves 100 %.

Compressing, the device reports the drive's delay as its latency, 6 samples, driven or not, and the driven sound comes
exactly that late.

### Duck

The detector hears the key: the track the sidechain listens to, or the device's own sound when none is set (see
[Devices](../devices.md) for how a sidechain is chosen and routed). It hears it through the two cuts, as a peak
level, the loudest over the last 10 ms, so that the cycles of a kick's low notes don't let the sound go between them.

When the key rises over the Threshold (a hit), the sound goes down in a straight line in decibels, the whole Depth
over the Attack. It stays down for at least the Hold from the hit, and for as long as the key stays over the
threshold. Then it comes back over the Release, straight in decibels or bent by the Release Curve. A new hit during
the release grips again from where the gain is.

Ducking, it looks ahead: the sound comes out 10 ms later than it goes in, while the detector hears the key as it
comes, so that the sound is already down as the hit plays. An attack of up to 10 ms ends at the hit; a longer one
starts 10 ms before it. The 10 ms are the device's latency, which the engine makes up for on the other tracks.

### Choosing a sidechain

Choosing a sidechain for a compressor sets its mode to Duck in the same undo step. If every other parameter is still
as the device was made, it is also set up to pump under a kick: Threshold -14 dB, Depth 14 dB, Attack 1 ms, Hold 120
ms, Release 80 ms; and, when the key's track plays a Drum Sampler, the cuts at 60 and 150 Hz, so that it hears the
kick's body and not the snare or the hats.

### Listen

While Listen is on and the editor shows Duck's page, the compressor plays what its detector hears, through the cuts,
in place of the sound, whatever the mix, faded over 5 ms. The editor asks for it again at each refresh, and the
compressor stops half a second after the last time it was asked, so nothing plays a key while its editor is hidden.
Listen is not a parameter and is not saved.

### Latency and Low Latency

The latency follows the mode set by hand: the drive's few samples for Compress, 10 ms for Duck. Automation that
switches the mode while the song plays does not change it: a compressor set to Compress that automation has duck does
so without looking ahead, and one set to Duck that automation has compress still comes 10 ms late. While Low
Latency is on, a compressor set to Duck is left out of what plays and the sound passes as it comes.

## Its editor

Two tabs along its top, Compress and Duck, are the mode: a click sets it, as one undo step, and shows its page. Each
page fits the default dock without scrolling or pages.

- **Compress:** the five characters as a row of buttons; the curve, lit up to the level the detector hears, with a
  slim meter beside it of how far the sound is turned down (from its middle down) or up (from its middle up); the
  history; and the knobs Attack, Release, In, Drive, Out and Low Cut.
- **Duck:** the Sidechain part, with the picker of the track it listens to, the Low Cut and High Cut and Listen; the
  history, which is this page's graph; and the knobs Threshold, Depth, Attack, Hold, Release and Curve.

The curve is edited by its points: drag a point (Shift keeps it level, Ctrl keeps it where it is across, and with
Snap on it goes onto a grid line); drag the hollow handle in the middle of a line up or down to bend it (Ctrl in fine
steps), and right-click it to straighten the line; right-click a point for its line's type or to delete it (Alt and a
click also deletes it); right-click elsewhere to add a point. The first and last points stay at the curve's ends. Each
change is one undo step. Under the curve are its menu (the shapes to start from: Limit at 0 dB, No Compression,
Compress 2:1 over -12 dB, Compress 4:1 over -18 dB, Gate under -30 dB, Expand under -24 dB, Lift under -30 dB and
Squeeze Both Ways; copy and paste, also to and from a Multiband's band; flip, smooth and reset), Freeze, which stops
editing, and Snap.

The history shows the last two bars at the song's tempo, scrolling from right to left over lines at the beats and
bars: a pale trace, what goes out over it in the device's colour, and how far the sound is turned down in red,
hanging from the top, the deepest dips numbered. On Duck's page the key shows as a line in its track's colour, named,
and the Threshold is a line across the history to drag up or down against the key's level, as one undo step.

Mix is the knob on the device's row in the Devices view.

## Limits

- Compress hears only its own sound: there is no level-following compression from an external sidechain. Only Duck
  uses the key, and it ducks by a fixed Depth.
- Compress has no look-ahead, no automatic makeup gain and no automatic release.
- The channels are always linked: no unlinked, mid/side or per-channel detection.
- There is no threshold, ratio or knee as numbers. The curve's points cannot be typed or moved with the keyboard, no
  ratio is shown, and the axes crowd quiet levels together, so a point far below 0 dB is hard to place exactly.
- The key has two cuts and no other filter, and no gain of its own.
- The gain change itself is not oversampled, and the drive runs at twice the rate with no higher setting.
- The characters are choices of detector, release and colour, not models of the circuits they are named after.
- On Compress's page the history's pale trace is the level the detector hears (after the Input Gain and the low cut,
  an average for VCA, Opto and Tube), not the level that came in.
- The history takes one reading at each refresh of the display, so a fast attack or release draws as a straight line.
- A drag of the threshold's line starts from the Threshold as modulation plays it, and sets that as the value by
  hand.

## In the code

- DSP: `src/devices/dynamics/Compressor.h`, `src/devices/dynamics/Compressor.cpp`; the curve in
  `src/devices/dynamics/DynamicsCurve.h`; the drive's oversampling and anti-aliasing in
  `src/devices/dsp/Oversampling.h` and `src/devices/dsp/Saturation.h`; the gain's follower in
  `src/devices/dsp/Envelopes.h`.
- What choosing a sidechain sets: `src/model/mixer/Sidechain.cpp`.
- Editor: `src/ui/devices/CompressorEditor.h`, `src/ui/devices/CompressorEditor.cpp`; the curve in
  `src/ui/devices/DynamicsCurve.h`; the history in `src/ui/devices/LevelHistoryView.h`; the sidechain's picker in
  `src/ui/devices/SidechainMenus.h`.
- Tests: `tests/CompressorTests.cpp`, `tests/CompressorEditorTests.cpp`, `tests/SidechainTests.cpp`,
  `tests/SidechainPresetTests.cpp`.
- Presets: `resources/presets/Compressor/` (see [presets.md](../../presets.md)).
