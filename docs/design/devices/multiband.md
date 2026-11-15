# Multiband

*Design section 9.*

Three-band dynamics with a master band after them, laid out and working as FL Studio's Maximus does: each band has a
compressor whose curve is drawn, a saturation and a stereo control. It compresses, expands, limits or gates each part
of the spectrum by its own curve, and can duck single bands under another track.

## Parameters

There are four bands: Low, Mid, High and Master. A band's parameters are named with its band ("Low Attack", "Master
Post").

### Bands (the split)

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Low Cut | 10 to 500 Hz | 20 Hz | A 12 dB per octave low cut before the split. |
| Low Frequency | 20 Hz to 2 kHz | 120 Hz | The crossover between the low and mid bands. Labelled Low Freq. |
| High Frequency | 500 Hz to 20 kHz | 5 kHz | The crossover between the mid and high bands; never below the low one. Labelled High Freq. |
| Low Slope, High Slope | 12 dB, 24 dB | 24 dB | Each crossover's steepness, per octave. |
| LMH Delay | 0 to 5 ms | 2 ms | How far the low, mid and high bands' compressors look ahead. |
| LMH Mix | 0 to 100 % | 100 % | How much of what goes to the master band is the three bands, against the sound as it came in. |
| Low, Mid, High Solo | Off, On | Off | Only the soloed bands play, and the master band after them. The master has no solo. |
| Mid Drives Master | Off, On | Off | The master's compressor hears the mid band in place of what it plays; the low and high bands' compressors are off meanwhile. Labelled Mid Drives. |

### Each band (Low, Mid, High and Master)

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Mode | On, Comp Off, Muted, Off | On | Comp Off leaves out the compressor alone. Muted silences the band. Off passes the band as it came: a low or high band that is off goes into the mid band, which then covers its room. |
| Stereo | -100 to +100 % | 0 % | The band's stereo separation: mono at -100 %, as it is at 0 %, twice as wide at +100 %. |
| Pre | -24 to +24 dB | 0 dB | A gain into the band's compressor. |
| Post | -24 to +24 dB | 0 dB | A gain after it: the band's level. The master's is the level of what comes out. The low, mid and high bands' are the Low, Mid and High knobs under the bands' graph. |
| Attack | 0 to 500 ms | 10 ms; the master 2 ms | How fast the compressor grips. The master also looks ahead by its attack, up to 5 ms. |
| Release | 0 to 2000 ms | 0 ms | The first release, which speeds up and is done in its time. |
| Release 2 | 0 to 2000 ms | 100 ms | The second release, which slows into the gain. The two together make an S. |
| Sustain | 0 to 1000 ms | 20 ms | How long the detector holds a peak, or the time it averages over. |
| Curve | 1 to 8 | 2 | How many smoothing stages the attack and the second release go through: 1 starts sharply, 8 roundly. |
| Release Curve | 1 to 8 | 3 | How far the first release speeds up. Labelled Rel Curve. |
| Detection | Peak, RMS | Peak | Whether the compressor hears each peak, held for the Sustain, or the average over it. |
| Saturation | -100 to +100 % | 0 % | Rounds the band off into its Ceiling: softly below 0 %, more firmly above it; the further from 0 % either way, the lower the level it starts at, down to 24 dB under the ceiling. Labelled Sat. |
| Ceiling | -24 to +6 dB | 0 dB | The most the saturation lets out. It does nothing while Saturation is 0 %. |
| Sidechain | Off, On | Off | The band's compressor hears the key in place of its own band. |
| Points | 2 to 8 | 3 | How many points the band's curve has. |
| Point 1 to 8 X, Y | 0 to 100 % | The default curve | Each point's place across (level in) and up (level out) the curve's axes. |
| Point 1 to 8 Tension | -100 to +100 % | 0 % | How the line to the point bends. |
| Point 1 to 8 Type | Single Curve, Double Curve, Hold, Stairs, Smooth Stairs | Single Curve | The shape of the line to the point. |

Every band's default curve is a limiter at 0 dB: level out equals level in up to 0 dB and stays there above it. So a
new Multiband changes nothing quieter than that.

### Sidechain

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Sidechain Low Cut | 20 to 500 Hz | 20 Hz | A 12 dB per octave low cut on the key. Labelled Low Cut. |
| Sidechain High Cut | 100 Hz to 20 kHz | 20 kHz (off) | A 24 dB per octave high cut on the key. Labelled High Cut. |

The Multiband has no Mix: LMH Mix is its blend, before the master band, which still shapes what comes out. Its
runner blends nothing back in.

## How it works

The sound goes through the low cut and splits into three bands at two Linkwitz-Riley crossovers, which add back up
flat: at 24 dB per octave, two Butterworth sections each way; at 12 dB, one section each way with the high side
turned over. The bands of a crossover add up to the sound through an all-pass, so whatever meets them goes through
the same all-pass: the low band around the second crossover, and the sound as it came in, which LMH Mix blends with
the bands, around both. They then meet in phase.

Each band goes through, in order: its stereo separation (a gain on its side signal), its Pre gain, its compressor, its
Post gain and its saturation. The three bands, mixed with the delayed sound as it came in by LMH Mix, go through the
master band's same chain.

**The compressors.** Each reads its gain from its curve, which is the [Compressor](compressor.md)'s: level out
against level in through up to eight points, on axes where an amplitude `a` sits at `ln (1 + 2a) / ln 9` (silence, 0
dB in the middle, +12 dB), so that it compresses or expands, downward or upward, limits or gates by its shape. The
detector hears the band's peak, the louder channel's, held for the Sustain, or its average over the Sustain. The gain
goes down through as many one-pole stages as the Curve box says, the Attack shared among them. It comes back through
two releases: Release follows a power curve that speeds up and reaches the gain wanted as its time is up (Release
Curve sets the power), and Release 2 goes through the same stages as the attack, slowing into the gain.

**Look-ahead.** The low, mid and high bands look ahead by LMH Delay and the master by its Attack, each up to 5 ms, by
delaying what the compressor hears less than the sound. A band can therefore be down as a peak arrives.

**Saturation.** Up to its threshold the sound passes as it is; past it, it rounds off into the Ceiling, as `tanh`
does (below 0 %) or by a cubic that reaches the ceiling flat (above 0 %). The curve plays at twice the sample rate by
its antiderivative (first-order antiderivative anti-aliasing: Parker, Zavalishin and Le Bivic, 2016), so that what it
adds doesn't fold back as aliasing. While any of the three bands saturates, all three and the sound mixed back in go
that way, so that they stay in step; while none does, they take a plain delay of the same length, which leaves a
limited peak where it is. The master's saturation works the same way on its own.

**Latency.** Always the most the two look-aheads take, 10 ms, and the delay of the two saturations in a row, whatever
the settings. The engine makes up for it on the other tracks. While Low Latency is on, the Multiband is left out of
what plays and the sound passes as it comes.

**Sidechain.** The key is the track the device's sidechain listens to, or what comes in when none is set (see
[Devices](../devices.md)), through the two cuts. A band whose Sidechain is on has its compressor hear the key in place
of its own band (the master's, in place of the mid band too), so that only the lows duck under a kick, say. A band's
Pre gain does not drive the key. Nothing about the key changes the sound while no band's Sidechain is on.

Choosing a sidechain for a Multiband whose parameters are all still as it was made sets it up to duck its lows, in
the same undo step: the low band's Sidechain on, its curve 8:1 over -30 dB, its Attack 1 ms and its Release 2 150 ms,
and the key's High Cut at 150 Hz when the key's track plays a Drum Sampler.

**Listen** plays the key, as the bands hear it through the cuts, in place of what comes out, as late as it and faded
over 5 ms, while its switch is on and the editor shows. It stops half a second after the editor last asked. It is not
a parameter and is not saved.

## Its editor

Two parts side by side, on one page in the default dock.

**Bands.** The three bands across the spectrum, each in its colour and as high as its level (its Post), with the
spectrum of what the Multiband plays over them and each crossover's slope (12 or 24 dB) to pick in a top corner. Drag a
crossover or the low cut's edge left or right; drag a band up or down for its level, from where it was; click a band
to show it in the Band part; double-click any of them to put it back to its default. What a drag would take lights up
under the pointer, the pointer shows which way it moves, and the hint bar says what it does. Under the graph, always
with it, are the knobs Low Cut, Low Freq, High Freq and LMH Delay, and Low, Mid, High and LMH Mix. A level dragged in
the graph and its knob are the same parameter.

**Band.** Tabs for Low, Mid, High and Master show one band at a time: its curve, lit up to the level its compressor
hears, with its menu; its mode; its switches (Solo and Sidechain; for the master, Sidechain and Mid Drives); and its
knobs Pre, Stereo, Sat and Ceiling, Attack, Release, Sustain and Release 2, the Curve and Rel Curve boxes, Peak or
RMS, and for the master its Post. The curve is edited as the [Compressor](compressor.md)'s is (the same component):
points drag, with Shift to keep them level and Ctrl to keep them where they are across; handles bend lines; a
right-click adds a point, picks a line's type or deletes; Alt and a click deletes. Its menu has the shapes to start
from, copy and paste (also to and from a Compressor), flip, smooth and reset, with Freeze and Snap.

Two things share the Band part's space where the dock is narrow, and show beside it where there is room:

- **Monitor**, by its switch, in place of the band's knobs: the selected band's last seconds, scrolling at a speed
  to set, paused by a click. Its Traces menu picks what shows: In, Band In, Out, Heard (the key, in its track's
  colour and named, while the band's Sidechain is on), Gain, All Gains and Pre/Post.
- **Sidechain**, by a tab after the bands', in place of the band: the picker of the track the key comes from, the
  key's Low Cut and High Cut, Listen, and a line saying which bands hear the key.

Which band shows, Monitor's switch, traces and speed, the Sidechain tab, Freeze, Snap and Listen belong to the editor
for the session, not to the project. Each edit of a parameter is one undo step.

## Limits

- Three bands, fixed: bands cannot be added, removed or placed freely.
- The crossovers are minimum phase: no linear-phase crossovers.
- The whole chain is not oversampled; only the saturations run at twice the rate, with no higher setting.
- A new Multiband's limit at 0 dB is a compressor's curve with an attack, not a brick wall: fast peaks pass over it.
  The [Limiter](limiter.md) keeps a ceiling.
- When the first band's Saturation leaves 0 %, or the last one returns to it, the bands switch between the plain
  delay and their saturators in one sample, without a fade: a few samples are skipped or repeated.
- The curve is small in the dock and its axes crowd quiet levels together, so points far below 0 dB are hard to
  place; points cannot be typed or moved with the keyboard, and no threshold or ratio is shown.
- The bands' graph draws the bands as blocks, not the crossovers' slopes, and shows no gain reduction on the spectrum.
- Monitor takes one reading at each refresh of the display.
- A drag of a band's level in the graph starts from the level as modulation plays it, and sets that as the value by
  hand.

## In the code

- DSP: `src/devices/dynamics/Multiband.h`, `src/devices/dynamics/Multiband.cpp`; the curve in
  `src/devices/dynamics/DynamicsCurve.h`; the saturation's oversampling and anti-aliasing in
  `src/devices/dsp/Oversampling.h` and `src/devices/dsp/Saturation.h`.
- What choosing a sidechain sets: `src/model/mixer/Sidechain.cpp`.
- Editor: `src/ui/devices/MultibandEditor.h`, `src/ui/devices/MultibandEditor.cpp`; the curve in
  `src/ui/devices/DynamicsCurve.h`; the sidechain's picker in `src/ui/devices/SidechainMenus.h`.
- Tests: `tests/MultibandTests.cpp`, `tests/MultibandEditorTests.cpp`, `tests/AntialiasingTests.cpp` (the
  saturation), `tests/SidechainTests.cpp`.
- Presets: `resources/presets/Multiband/` (see [presets.md](../../presets.md)).
