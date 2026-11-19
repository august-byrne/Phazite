# Vocoder

*Design section 9.*

Put on a voice's track, it makes another sound, the carrier, speak with the voice: the robot voice and the talking
synth. The voice is what passes through the device; the carrier is a buzz or a noise of its own, the voice itself, or
another track picked as its sidechain.

## Parameters

The editor groups them as Carrier, Voice, Range and Response.

### Carrier

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Carrier | Buzz, Noise, Voice, Track | Buzz | What speaks. Buzz is a bright saw; Noise a whisper; Voice the voice itself, smeared; Track the sound of the track picked as the device's sidechain. |
| Pitch | -24 to +24 semitones | 0 | The buzz's pitch, from C3. Automating it plays a tune. |

### Voice

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Formant | -12 to +12 semitones | 0 | Moves the bands the carrier plays through up or down against the bands that listen, for a smaller or larger voice. |
| Unvoiced | 0 to 100 % | 50 % | Adds noise to the carrier where the voice has consonants ("s", "t"), so that they stay clear. |

### Range

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Bands | 8, 12, 16, 24, 32, 40 | 16 | How many bands the voice and the carrier are split into. A change fades the sound out and in again over 10 ms each way. |
| Low Frequency | 40 Hz to 1 kHz | 100 Hz | The lowest band's centre. The editor calls it Low. |
| High Frequency | 2 to 18 kHz | 10 kHz | The highest band's centre, kept at least one and a half times the lowest. The editor calls it High. |
| Band Width | 25 to 200 % | 100 % | How wide each band is: at 100 % neighbouring bands meet at their -3 dB points. The editor calls it Width. |

### Response

| Parameter | Range | Default | What it does |
| --- | --- | --- | --- |
| Attack | 0.5 to 50 ms | 2 ms | How fast a band's level follows the voice up. |
| Release | 5 to 500 ms | 20 ms | How fast a band's level follows the voice down. |
| Brightness | -100 to +100 % | 0 % | Tilts the bands' levels around 1 kHz, by up to 3 dB an octave either way. The editor calls it Bright. |
| Stereo | 0 to 100 % | 0 % | Sends the bands left and right in turn. The mono sum stays as it was. |
| Output | -24 to +24 dB | 0 dB | A gain on what goes out. |
| Mix | 0 to 100 % | 100 % | How much of what plays is the vocoder's sound and how much the voice as it came in. The device's runner applies it, not the vocoder; it is in the device's line in the Devices view ([Devices](../devices.md)). |

## How it works

It is a channel vocoder. The voice, both sides together, goes through a bank of band-pass filters, spaced evenly on a
logarithmic scale from the Low Frequency to the High Frequency, each band two second-order sections. Each band's
level is followed with the Attack and the Release, but never faster than two cycles of the band's centre, so that the
follower tracks the level and not the wave. The carrier goes through a second bank with the same widths, its centres
moved by the Formant, and each of its bands is multiplied by the voice's level in the matching band. A band of the
voice quieter than -70 dB plays nothing.

The carriers: Buzz is a band-limited saw at C3 moved by the Pitch, so the vocoder speaks the moment it is put on a
voice; Noise is white noise from a fixed seed, so a render is the same each time; Voice is the voice itself; Track is
the sound of the sidechain's track, both sides together, and silence while none is picked.

The carrier is evened out band by band: each band is divided by the loudest of the carrier's levels there and in its
two neighbours, so that a lone harmonic isn't raised in the bands around it. That level is never taken as less than a
floor: 12 dB under what the band would hold of pink noise as loud as the carrier has been lately. So a band the carrier
hardly reaches, and the carrier's tail, stay down, the vocoder is about as loud as the voice whatever the carrier, and
it is silent when the carrier is.

Unvoiced measures what share of the voice is above 4 kHz and adds white noise to the carrier for it: none at a share
of 30 %, all of it at 60 %, as loud as the carrier has been lately times the Unvoiced, so a silent carrier stays
silent.

Every other band plays upside down, so that neighbouring bands add up where they overlap instead of cancelling.
Stereo pans the even bands one way and the odd bands the other. The vocoder has no latency.

For how a sidechain's track is picked and what it hears of a muted or soloed track, see [Devices](../devices.md).
Picking a track for the vocoder makes Track the carrier in the same undo step.

## Its editor

A hardware vocoder's bank of lights: a column for each band, as high as the voice is in it, standing where the carrier
plays that band, over a mark where the band listens to the voice, so the Formant shows as the columns leaning away
from their marks. The picture names the carrier (the buzz's note, as "Buzz C3"; the track that feeds it; or, in the
warning colour, "Track: pick one by the carrier" while Track has none), shows how loud the carrier plays, and has a
light for the consonants it hears.

A drag sideways over the picture moves the Formant, as one undo step: the pointer is a left-and-right arrow there, the
lines from the marks to the columns light up, and the hint bar says what a drag does. Beside the picture are the
Carrier and its Pitch, with the picker of the carrier's track next to them; Formant and Unvoiced as large knobs; then
the Range and Response controls.

## Limits

- The Buzz is one saw at one pitch. It takes no notes, so it can't play chords, and it doesn't follow the voice's
  pitch; a tune needs Pitch automated, or a synth's track as the carrier.
- No level, pan or solo for single bands, and no freeze of the voice's shape.
- 40 bands at most, and the voice and a Track carrier are each taken as mono.
- The device goes on the voice's track, with the carrier as its sidechain. It can't go on the carrier's track with the
  voice as its sidechain, as some vocoders work.
- No switch for the evening-out of the carrier: it is always on.

## Listening set

Ten minutes, on a spoken or sung vocal of your own, with a synth playing chords on another track. Stage 6 changed
nothing in the Vocoder, so this set judges it as it stands.

| What to play | The setting | Listen for |
| --- | --- | --- |
| A spoken phrase | A new Vocoder (Buzz at C3, 16 bands) | The words are understood, as a robot speaks them, at about the voice's level |
| The same phrase | `Vocoder/Voices/Robot Voice`, then `Vocoder/Voices/Whisper Voice` | A clear robot; a whisper whose words are still clear |
| A sung phrase, with a synth's chords on another track | `Vocoder/Sidechain/Synth Choir`, the synth's track as its carrier | The chords sing the words, in tune with the synth; silent when the synth is |
| A phrase with many "s" and "t" sounds | Unvoiced at 0 %, then 100 % | At 0 % the consonants smear; at 100 % they stay clear |
| The same phrase | `Vocoder/Formant/Formant Down`, then `Vocoder/Formant/Formant Up` | A larger, then a smaller voice, the words still clear |
| The phrase: the test of its bands | Bands 8, then 40, while it plays | 8 coarse and buzzy, 40 near speech; the change fades without a click |
| A sung melody | Buzz, Pitch 0 | The Limits' weakest place: the buzz stays on one note whatever the voice sings, and takes no chords; whether that is how you would use it |

## Verdict

- Its band followers never move faster than two cycles of their band, the carrier is evened band by band so the
  vocoder sounds about as loud as the voice whatever the carrier, and it is silent when the carrier is (this page). No
  measure of its intelligibility is written down.
- Against a flagship vocoder: its own carrier is one saw at one pitch that takes no notes, so chords need another
  track; no level, pan or solo for single bands; no freeze of the voice's shape; the voice taken as mono; no pitch
  tracking of the voice (no backlog row).
- **Short of it: a carrier that plays notes and chords, and controls for single bands** (by measurement; to be
  confirmed by ear).

## In the code

- DSP: `src/devices/vocoder/Vocoder.h`, `src/devices/vocoder/Vocoder.cpp`; its filters, `src/devices/dsp/Filters.h`;
  the level followers, `src/devices/dsp/Envelopes.h`; the buzz, `src/devices/dsp/Oscillators.h`.
- Editor: `src/ui/devices/VocoderEditor.h`, `src/ui/devices/VocoderEditor.cpp`; the carrier's picker,
  `src/ui/devices/SidechainMenus.h`.
- Tests: `tests/VocoderTests.cpp`, `tests/SidechainTests.cpp`, `tests/EffectEditorTests.cpp` (the editor),
  `tests/RealtimeAuditTests.cpp`.
- Presets: `resources/presets/Vocoder/` ([presets.md](../../presets.md)).
