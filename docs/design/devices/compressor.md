# Compressor

*Design section 9.*

A compressor with three modes. **Compress** turns each level up or down as a drawn curve says, in one of five
characters, hearing its own sound or another track (its key, the track its sidechain listens to); with its Hold and a
curve drawn as a step it is a gate. **Duck** turns the sound down while its key plays, for sidechain pumping.
**Transient** turns the start of each hit and what follows it up or down, whatever the level, as a transient shaper does.

## Parameters

### Shared by the modes

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | Compress, Duck, Transient | Compress | Which of the three the device does. The editor's three tabs set it. |
| Attack | 0.1 to 200 ms | 10 ms | Compress: the time constant of the gain going down. Duck: the time the sound takes to go down by the whole Depth. |
| Release | 0.1 to 2000 ms | 120 ms | Compress: the time constant of the gain coming back. Duck: the time the sound takes to come back. Its knob's middle is at 100 ms, as before its range reached under 5 ms. |
| Sidechain Low Cut | 20 to 500 Hz | 20 Hz | A 12 dB per octave low cut on what the detector hears, so that bass doesn't pump it (Transient's too). Labelled Low Cut. |
| Sidechain High Cut | 100 Hz to 20 kHz | 20 kHz (off) | A 24 dB per octave high cut on what the detector hears in Compress and Duck, so that only a kick's body is heard from a whole kit. Labelled High Cut. |
| Mix | 0 to 100 % | 100 % | How much of what goes out is the compressor's sound, for parallel compression; applied by the device's runner, not by the DSP (see [Devices](../devices.md)). |

### Compress

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Character | Clean, VCA, FET, Opto, Tube | Clean | How the compressor hears and reacts (below). |
| Input Gain | -24 to +24 dB | 0 dB | A gain before the compressor, which also drives its detector while it hears its own sound (not a key). Labelled In. |
| Look-Ahead | 0 to 10 ms | 0 ms | How far ahead of the sound the detector hears, so that the gain is down as a peak plays. |
| Hold | 0 to 500 ms | 0 ms | How long the detector holds the loudest level it heard (the parameter Level Hold). |
| Makeup | -12 to +24 dB | 0 dB | A gain after it (Transient's too). Labelled Out. |
| Auto Makeup | Off, On | Off | A gain after it of half the decibels the curve takes off a 0 dB level, which Makeup adds to. Labelled Auto. |
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

### Transient

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Punch | -100 to +100 % | 0 % | Turns each hit's attack up (to +15 dB at 100 %) or down (to -15 dB). |
| Sustain | -100 to +100 % | 0 % | Turns what follows the attack up (to +18 dB) or down (to -18 dB). |
| Speed | 1 to 50 ms | 10 ms | How long an attack is taken to last: short for hats and clicks, long for kicks and basses. |

Each mode ignores the others' parameters. While it ducks, the curve, Character, Input Gain, Look-Ahead, Hold, Drive,
Makeup and Auto Makeup do nothing; while it compresses, Threshold, Depth, Duck's Hold and Release Curve; Transient uses
Punch, Sustain, Speed, Makeup, the Sidechain Low Cut and the Mix, and ignores the rest.

## How it works

Every mode turns both channels by the same gain, worked out from the louder one, so the stereo image holds.

### Compress

The compressor is feed-forward: its detector hears the sound as it comes in, after the Input Gain and through the
Sidechain Low Cut and High Cut, never what it puts out. So once settled it follows the curve exactly, whatever its
character. While its sidechain listens to a track, the detector hears that track (the key) in its place, through the
same cuts, and not through the Input Gain, as a keyed compressor does: a pad keyed by a kick is turned down by what the
curve gives the kick's level, and not between the kicks once released.

The curve gives level out against level in, through up to eight points, on axes where an amplitude `a` sits at
`ln (1 + 2a) / ln 9`: silence at one end, 0 dB in the middle, +12 dB at the other. A line is straight on those axes,
bent by its tension, or shaped as an S, a hold or stairs. By its shape alone the curve compresses or expands, downward
or upward, limits or gates; there is no threshold, ratio or knee. The compressor reads the gain for a level from a
table with an entry every quarter of a decibel from -120 dB up, made again only when the curve changes. The curve,
its editor and its shapes are the [Multiband](multiband.md)'s too.

The gain moves in decibels with the Attack as it goes down and the Release as it comes back, as time constants: after
that time it has gone 63 % of the way (Giannoulis, Massberg and Reiss, 2012).

The Hold has the detector hear the loudest level over its last Hold milliseconds, a millisecond at a time (as Duck's
detector holds its key's 10 ms), so that the reduction stays down that long after a peak, and a curve that gates stays
open that long after the sound falls under its step: a gate drawn on the curve (its shapes Gate under -30, -40 and -50
dB) then opens once and closes once on a ringing drum, where without it the gate closed at each crossing of the drum's
wave through 0, and chattered. A gate closes at the Attack (the gain going down) and opens at the Release (the gain
coming back), the reverse of a gate's own words, which the two knobs' tooltips and the shapes' menu say; both reach down
to a tenth of a millisecond, so that a gate opens at once from as far down as it shut the sound. At 0 ms the Hold plays
no part.

The Look-Ahead delays the sound by its milliseconds while the detector hears it as it comes, so that the gain is down
as a peak plays: with 5 ms and an attack of 1 ms, a burst's first millisecond is within a decibel of where it settles.
It is part of the latency, which follows the Look-Ahead set by hand (below). As automation moves the Look-Ahead, the
place the gain is read from glides an eighth of a sample a sample, so that the gain never steps: it moves at most an
eighth faster than it would. With the drive on, the makeup the drive takes off and puts back, and the reduction FET's
and Tube's colour follows, are read as late as the gain, so that each is the one worked out for the sound it turns.

Auto Makeup adds half the decibels the curve takes off a 0 dB level, read from its table of gains: 6.75 dB for the
default curve, which takes 13.5 off 0 dB, and less than none for a curve that adds level there. It is a property of the
curve alone, so it never pumps with the sound, and it glides with the Makeup as the curve is edited. Makeup adds to it.

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
filters while the drive plays (and, where the sound takes more latency than the drive's, the same delay before them, as
the sound does), so that the two meet in phase at any mix, from the moment the Mix leaves 100 %.

Compressing, the device reports the drive's delay and the Look-Ahead as its latency, 6 samples and the Look-Ahead
rounded to samples, driven or not, and the sound comes exactly that late.

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

### Transient

It turns each hit's attack and what follows it up or down by the shape of the sound's level over time, not by how
loud it is, as the differential-envelope transient shapers do (the SPL Transient Designer's principle), which a curve
of level in against level out cannot. It hears its own sound, never a key, through the Sidechain Low Cut: the louder
channel, the loudest over the last 10 ms (as Duck hears its key, so that a steady tone reads as steady), in decibels from
-90 dB up. Four followers of that level, each rising and falling at times of its own, which run in Transient alone (in
the other modes its ear keeps only the loudest of the last 10 ms, two filters and a comparison a sample; switched to
Transient, its followers start at the level heard then, so that no burst of gain comes as it starts):

- **The attack:** a fast follower rises in 0.2 ms, a slow one in the Speed; both fall in four times the Speed and 20
  ms more. How far the fast one is over the slow one is the attack: large while a hit rises, nothing on a steady sound.
- **What follows it:** two followers both rise in 1 ms; a long one falls over 600 ms, a short one as the attack's
  fall. How far the long one is over the short one is what follows the attack: large after a hit has passed, nothing on
  a steady sound.

The gain is Punch's share of 15 dB as the attack's part goes from 1 to 12 dB (all of it from 12 dB, none at 1 dB or
less), and Sustain's share of 18 dB as the other part does, kept within 24 dB either way and glided over 1 ms so that
it never steps; then Makeup. The decibel each part leaves unheard keeps a steady sound exactly as it came: a tone
through the low cut rings a few tenths of a decibel as it starts, which the long follower held for a second. Since
both parts are differences of decibels, a hit turns the same at -30 dB as at -6 dB, and a steady sine at any level
comes out within a tenth of a decibel of how it went in. Its latency is Compress's without a Look-Ahead.

### Changing mode

From one mode to another the gain goes on from where it was: each mode's from what the one before turned the sound by
(Duck's no further down than its Depth), each then moving as its own times say, so that a switch doesn't click. And what
is on its way through the device as the mode changes is turned by the mode it was meant for: every mode sends the sound
through the same delays, Compress and Transient turning it as it leaves the first and Duck as it leaves the drive's, so
Duck turns what leaves the first by the gain Compress or Transient worked out for it, and they turn what leaves the
drive's by Duck's. Where the mode before would have looked ahead at what is still to come, its gain for that is the one
it had as it changed. Through any change, each millisecond's level stays between the two modes' settled levels, within a
decibel.

### Choosing a sidechain

Choosing a sidechain for a compressor whose every other parameter is still as it was made (but for its mode) sets its
mode to Duck in the same undo step, set up to pump under a kick: Threshold -14 dB, Depth 14 dB, Attack 1 ms, Hold 120
ms, Release 80 ms; and, when the key's track plays a Drum Sampler, the cuts at 60 and 150 Hz, so that it hears the
kick's body and not the snare or the hats. A compressor set its own way keeps its mode: compressing, it is then keyed by the
track, and shaping transients, it keeps the track for when it compresses or ducks again.

### Listen

While Listen is on and shows, on Compress's page or Duck's, the compressor plays what its detector hears, through the cuts,
in place of the sound, whatever the mix, faded over 5 ms. The editor asks for it again at each refresh, and the
compressor stops half a second after the last time it was asked, so nothing plays a key while its editor is hidden.
Listen is not a parameter and is not saved.

### Latency and Low Latency

The latency follows the mode and the Look-Ahead set by hand: the drive's 6 samples and the Look-Ahead for Compress, 10
ms for Duck, the drive's 6 samples for Transient. Automation that switches the mode or moves the Look-Ahead while the
song plays does not change it: a compressor set to Compress that automation has duck does so without looking ahead,
one set to Duck that automation has compress still comes 10 ms late and may look ahead as far, and automation of the
Look-Ahead moves how far the detector is ahead up to what the latency set by hand allows. A drag of the Look-Ahead's knob
makes the device anew once, as the drag ends, not at each step (each would start it again from silence); meanwhile it
plays the value within the latency it has. While Low Latency is on, a
compressor whose latency is over 1 ms is left out of what plays and the sound passes as it comes: one set to Duck, and
one set to Compress with a Look-Ahead of about 0.9 ms or more (at 48 kHz).

## Its editor

Three tabs along its top, Compress, Duck and Transient, are the mode: a click sets it, as one undo step, and shows its
page. Each page fits the default dock without scrolling or pages.

- **Compress:** the five characters as a row of buttons; the curve, lit up to the level the detector hears, with a
  slim meter beside it of how far the sound is turned down (from its middle down) or up (from its middle up), and Auto
  Makeup's switch (Auto) by its tools; and beside them the history, the knobs Attack, Release, Hold, Look-Ahead, In,
  Drive and Out, and the Sidechain part, Duck's, which in the default dock show one at a time under their tabs, the
  knobs first.
- **Duck:** the Sidechain part, with the picker of the track it listens to, the Low Cut and High Cut and Listen; the
  history, which is this page's graph; and the knobs Threshold, Depth, Attack, Hold, Release and Curve.
- **Transient:** the history, its gain a line above and below the middle; and the knobs Punch and Sustain, large, over
  Speed, Out and Low Cut.

The curve is edited by its points: drag a point (Shift keeps it level, Ctrl keeps it where it is across, and with
Snap on it goes onto a grid line); drag the hollow handle in the middle of a line up or down to bend it (Ctrl in fine
steps), and right-click it to straighten the line; right-click a point to type its levels, for its line's type or to delete it
(Alt and a click also deletes it); right-click elsewhere to add a point. The first and last points stay at the curve's ends. Each
change is one undo step. Under the curve are its menu (the shapes to start from: Limit at 0 dB, No Compression,
Compress 2:1 over -12 dB, Compress 4:1 over -18 dB, Gate under -30 dB, Gate under -40 dB, Gate under -50 dB, Expand
under -24 dB, Lift under -30 dB and
Squeeze Both Ways; copy and paste, also to and from a Multiband's band; flip, smooth and reset), Freeze, which stops
editing, and Snap.

The curve by numbers: while a point is under the pointer, dragged or chosen from the keyboard, the curve's top right
corner reads it, "In -18.0 dB, Out -12.0 dB", and under that the ratio of the line that leaves it where that is a
single curve, from its ends: "4.0:1", "1:2.0" for one that expands, "Limit" from 20:1 and "Gate" past 1:10. A point's
Type Values... opens a field over it, as an automation point's Type a Value... does, which takes "in, out" in decibels
("-18, -12"; "silence" for none), the curve's first and last points their level out alone, and moves it there as one
undo step, Move Point, as a drag is. The two levels are separated by a semicolon, white space, or a comma followed by a
space or a sign, so that a comma between digits is a decimal comma ("-18,5, -12"; an end's "-3,5" is -3.5 dB). Text it
can't read, or a level missing, it refuses and says why in the curve's corner (and to screen readers) until the next
edit or move of the pointer; the automation points' and the knobs' typed fields leave the value as it was without a word.
Tab reaches the curve (a click on it doesn't take the keys, as on every graph); Left and Right choose a point, lit as
under the pointer; with Ctrl the arrows move it a tenth of a decibel in (Left, Right) or out (Up, Down), with Shift a
decibel, each run of presses one undo step (it ends half a second after the last press, at a choice of another point, a
key that isn't the curve's, a click, another curve shown, as the Multiband's next band, or the keys gone elsewhere), as
a turn of the EQ's wheel is. Screen readers read the chosen point's numbers as the curve's value.

The history shows the last two bars at the song's tempo, scrolling from right to left over lines at the beats and
bars: a pale trace, what goes out over it in the device's colour, and how far the sound is turned down in red,
hanging from the top, the deepest dips numbered (on Transient's page, how far it is turned up or down, as a line about
the middle). On Duck's page, and on Compress's while a key is heard, the key shows as a line in its track's colour,
named, in place of the red; and on Duck's the Threshold is a line across the history to drag up or down against the key's level, as one undo step.

Mix is the knob on the device's row in the Devices view.

## Limits

- Compress has no automatic release, and its look-ahead is at most 10 ms.
- While the Look-Ahead's knob is dragged, the compressor looks ahead no further than the latency it had as the drag
  began; the new latency comes as the drag ends.
- Transient has no look-ahead, and hears only its own sound.
- The channels are always linked: no unlinked, mid/side or per-channel detection.
- There is no threshold, ratio or knee as numbers to set: a point's levels are typed and its line's ratio is read,
  but a ratio or a knee is drawn, not typed. A line's ratio is shown from its ends, though its tension bends it.
- A gate drawn on the curve shuts the sound as far as the curve's table goes, 120 dB under full scale; quieter sound
  under it is turned down less than that.
- The key has two cuts and no other filter, and no gain of its own.
- The gain change itself is not oversampled, and the drive runs at twice the rate with no higher setting.
- The characters are choices of detector, release and colour, not models of the circuits they are named after.
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
  `tests/SidechainPresetTests.cpp`; the curve by numbers in the Multiband's editor in `tests/MultibandEditorTests.cpp`.
- Presets: `resources/presets/Compressor/` (see [presets.md](../../presets.md)).
