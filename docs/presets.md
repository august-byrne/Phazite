# Phazite's presets

The presets that come with Phazite (`resources/presets/<device>/<kind>/<name>.phazitepreset`) and the drum sampler's kits, whose sounds Phazite synthesizes as it needs them ([design section 9](design.md#9-devices-containers-and-presets)). This page records what each instrument's presets cover, how classic instruments are recreated and named, how the presets are leveled, and how to check them. The pass of 2026-11-08 compared every instrument's presets with what the leading instruments ship and added the classic sounds producers expect.

## What they cover

| Instrument | Presets | Kinds |
|---|---|---|
| Synth | 49 | Bass, Brass, FX, Keys, Lead, Pad, Percussion, Pluck, Strings |
| Wavetable | 30 | Bass, Chords, FX, Keys, Lead, Pad, Percussion, Pluck |
| FM | 36 | Bass, Brass, FX, Keys, Lead, Mallets, Pad, Percussion, Pluck, Strings, Winds |
| Sampler | 8 | Bass, FX, Lead |
| Drum Sampler | 11 kits | 808, 909, Trap, House, Lo-Fi, Rhythm Box, Percussion, Punchy, Tight, Wide, Synthwave |

The kinds follow what Ableton Live, Logic, Bitwig and Arturia sort their factory sounds into: brass, strings, mallets, winds and percussion have kinds of their own rather than sitting under Lead, Pad or Pluck. Each preset's name is its own across the instruments (the FM synth's Brass and the synth's Poly Brass, not two "Brass"), so that a search finds one thing by a name.

**Conventions:**
- **A bass sounds at the note played**, as on Phazite's other instruments and in the piano roll; a sub an octave down is part of the sound where the preset has one. The DX7 transposes some of its basses an octave down; their FM presets here have their ratios doubled instead.
- **Velocity changes brightness, not only level,** where a played instrument does: plucks, keys, brass and basses close their filter for softer notes (the synth's Filter Velocity), and the FM presets' modulators follow velocity as their originals' do.
- **The mod wheel brings in vibrato** on the leads, brass and strings that don't use their LFO otherwise: the LFO is on at no depth, to the pitch, and the wheel adds to its depth, as on the synths these come from.
- **Presets are dry:** no reverb or delay inside them, which users of other libraries prefer (they turn off on-board effects first). Phazite's own effects follow the instrument.

## Naming classic instruments

The presets recreate classic instruments' sounds, but are named by their sound, not by a trademark (the research of 2026-11-08, from the USPTO's records and how other software names its presets; this isn't legal advice):

- **808 and 909** name sounds and kits ("808 Kit", "909 Ride"), as Ableton names its "Kit-Core 808" and "909 Core Kit": no registration for "808" or "909" alone was found, and dictionaries define "808" as the sound. **"TR-808", "TR-909", "TB-303" and "Roland" are never used**: Roland registered them in 2020 and 2021 for plug-ins and sound recordings, and enforces them.
- **Every other classic gets a name for its sound**, as Ableton, Logic and Bitwig name theirs: Acid Bass (after the TB-303), DCO Bass and Strings and PWM Pad (Juno), Analog Brass (Jupiter), Poly Brass (OB-X), Poly Pad (Prophet), Ladder Bass and Fat Lead (Minimoog), Pedal Bass (Taurus), Mono Bass (SH-101), String Machine (Solina), Cinematic Brass (CS-80), Scream Lead (MS-20), Duo Lead (Odyssey), Hoover (Alpha Juno), Tine E-Piano (the DX7's E.PIANO 1), Lately Bass (the TX81Z's LatelyBass, a patch's name), 16-Bit Bass and Lead (the Genesis), House Organ (the M1's Organ 2), Rhythm Box Kit (the CR-78). Near-variants of a mark ("Moogy", "Wurli") aren't used either: Moog had Cherry Audio rename its "Minimode".
- **This page names the originals** in plain sentences, to say what a preset is after. The About box and `NOTICES.txt` say that other names of products are their owners' trademarks, named only to say what Phazite works with or what a sound is like, that Phazite isn't affiliated with their owners, and that its presets hold no recordings of their instruments (`resources/licenses/notices.json`).

## The classic sounds and where their settings come from

**Drum machines** (the drum sampler's kits, synthesized in `src/devices/drums/DrumMachines.cpp`):
- **808 Kit:** the TR-808's sixteen voices, General MIDI's drums where it has them (kick, rim shot, snare, clap, toms, hats, cymbal) and its own where it hasn't (claves for the second snare, maracas for the pedal hat, its three congas between the toms, its cowbell for the ride). Modeled on Werner, Abel and Smith's analyses of its bass drum (DAFx-14) and cymbal (ICMC 2014), the service notes' chart of each voice's pitch and decay, and published fits to the schematic: the kick a resonator at 50 Hz, its first 3.7 ms three times as fast, then a sigh from 23 % above; the snare's resonators at 238 and 476 Hz with noise from 2.75 to 5 kHz; toms at 90, 135 and 185 Hz and congas at 185, 280 and 400 Hz, each as short as the chart says; the hats, cymbal and cowbell from its six square oscillators (205.3 to 800 Hz) through its bands (7.1 and 3.44 kHz; the cowbell's 890 Hz), with no noise in them, as the circuit has none.
- **909 Kit:** the TR-909's analog kick (a rounded triangle swept down to 52 Hz), snare (two triangles 1.585 apart and noise that holds, then falls), toms (three partials, 1 : 1.5 : 2.75), rim shot and clap, and its sampled hats, crash and ride as dense metal at six bits, read back at 30 kHz as its converter reads them; a snappier second snare, a pedal hat, and each tom tuned low and high.
- **Rhythm Box Kit:** the CR-78's fourteen voices and its open hat: its small kick, its hats and cymbal of ringing band-passed noise with its metallic beat (three squares through a band-pass at 5.6 kHz), its guiro's scrape, maracas, tambourine, cowbell, claves, bongos and conga. Little is published about its circuits; these follow what is.
- **Trap Kit:** what producers expect of a trap kit: a short kick over a long 808 (a sine falling an octave onto G1 over 22 ms, saturated), a crisp snare, a layered clap, tight hats for rolls, a snap, toms, a saturated 808 cowbell as phonk plays it, a shaker, a tambourine and a crash.
- **Percussion Kit:** hand percussion, as Bitwig and FL Studio ship it: congas (open and slapped), bongos, timbales, agogo bells, woodblocks, claves, a shaker, a cabasa, a tambourine and a triangle.
- **Synthwave Kit** (`src/devices/drums/FactoryDrums.cpp`, from `DrumSynthesis.h`): 80s synthwave drums, made for the example song Exodus after the samples its original plays (from the free 80s Retro Futuristic Drum Pack), each measured from the original's stems, none of them copied: a kick that chirps down from 1.2 kHz and falls from 280 Hz to 64 Hz in about 11 ms, held 40 ms and then cut short, as the sample's falls 20 dB between 60 and 80 ms; a snare whose tone near 165 Hz carries most of it under noise from 3 to 7.5 kHz; a clap near 1.1 kHz over a long tail, both wide as the samples are; a woody rim knock, toms from 82 to 196 Hz, and hats and cymbals, each hat a bright layer over a dark one.

**Exodus's sounds**, measured against the original's stems the same way: the wavetable's Harmonic Pluck, the Harmonic Pad's sound with the envelope of the original's PADsynth layer (up in 25 ms, down 40 dB over 270 ms, released in 41 ms), so that an arpeggio's sixteenths fade before the next, as the original's do; and the compressor's Sidechain Pump, the pumping the original's peak controller makes: 30 : 1 from -46 dB, keyed below 150 Hz, in 3 ms and back over 50 ms, which takes a kick's beat down about 30 dB and brings it back in about 200 ms.

**Synths** (Phazite's synth, after the 56 Juno-60 factory patches as the Hera emulator reads them, Sequential's own Prophet-5 recipes, the Alpha Juno's factory patch sheet, Open303's model of the TB-303, Adam Szabo's analysis of the JP-8000's supersaw, and Gordon Reid's Synth Secrets): Acid Bass glides (60 ms) only into a note played over a held one, as the 303 slides; DCO Strings and PWM Pad sweep a pulse's width with the LFO, PWM Pad's coming in over 1.5 s as the Juno's LFO delay brings it; Sync Lead sweeps a hard-synced oscillator with the filter envelope, as the Prophet-5's poly-mod does; Ladder Bass, Pedal Bass, Fat Lead and Scream Lead drive the filter, as a Minimoog's and an MS-20's do; Poly Pad, Poly Brass and the pads drift a little, as analog voices do. The chorus several of these want is an effect in Phazite, put after the synth.

**The chorus** (the Chorus effect, `src/devices/motion/Chorus.h`): Chorus I and Chorus II play the Juno-60's two modes (a copy each side, its delay swept from 1.66 to 5.35 ms by a triangle at 0.513 or 0.863 Hz, the sides opposite, no feedback), and Chorus I + II its two pressed together (from 3.3 to 3.7 ms at 9.75 Hz), their copies darkened above 10 kHz, a guess at what its bucket-brigade lines and their filters take; String Ensemble the string machines' ensemble (three copies a side, a third of a cycle apart, each swept by a slow sine and a fast one, 0.6 and 6 Hz, as the ARP and Solina ensembles sweep theirs); and Subtle Chorus, Doubler, Subtle Width, Lush Ensemble, Vibrato, Tape Wobble and Metallic Chorus for the uses other DAWs' choruses ship presets for.

**FM** (the FM synth, after the DX7's ROM1A, the TX81Z's and the Sega Genesis's voices, decoded from their data and converted as Dexed's engine (msfa) and ymfm work them out: a modulator's index is its level squared times 4π, a DX7 output level OL is a level of 2^((OL - 99) / 16), and the DX7's feedback FB is 2^(FB - 7)): Tine E-Piano (E.PIANO 1), Pick Bass (BASS 1), Brass (BRASS 1), String Section (STRINGS 1), Marimba, Tubular Bells, Vibraphone, Clav, Harpsichord, Flute and Koto from ROM1A, each its six operators made four; Lately Bass and Nylon Pluck from the TX81Z; 16-Bit Bass and Lead from Sonic's first zone; Bell as John Chowning described it (1 : 1.4); and FM drums and Amapiano's log drum.

## Leveling

A preset's level is set so that switching presets doesn't jump in loudness, and so that nothing clips, as the libraries users praise do and as users ask of the ones they don't (Diva's and SynthMaster's presets were re-leveled after complaints):
- **The loudness of one note at velocity 100,** its loudest momentary reading (EBU R128, 400 ms), aims at about -16 LUFS for the sustained kinds (basses, keys, brass, winds), -15 for leads, -17 for pads, strings and chords, and -19 to -20 for plucks, mallets, percussion and effects, whose short notes a 400 ms reading hears quieter than the ear does. A new instrument plays at about -14 to -16.
- **Nothing peaks above -1 dBFS:** one note, and for a preset that plays chords (polyphonic, and not a bass, a drum or an effect) the four-note chord the preset tests play. Presets were leveled to -3 dBFS, for room.
- **A kit's pads are balanced as a kit:** the kick its loudest, the snare and clap 3 to 5 LU under it, toms and percussion 5 to 8, hats and cymbals 8 to 11.
- **The example songs keep their mix:** where a preset they use was re-leveled, the song's track is turned the other way.

`tests/FactoryPresetTests.cpp` checks the peaks, and that each preset's loudness is within 12 LU of its instrument's median.

## Checking them

`tests/PresetReport.cpp` measures every instrument preset and writes WAV files to listen to. ctest skips it; run it from a `windows-profile` build:

```
$env:PHAZITE_PRESET_REPORT = "<a folder>"
build\windows-profile\phazite_tests_artefacts\RelWithDebInfo\phazite_tests.exe --test-suite=preset-report --no-skip
```

`PHAZITE_PRESET_FILTER` limits it to presets whose path holds a text ("Synth/Bass"). The folder gets `report.md`: for each preset, its peak, loudness, attack, decay, release, brightness (spectral centroid) at 30 ms, 250 ms and 1 s, pitch against the note played, stereo width, what velocity does to its level and brightness, and its chord's peak; for each kit's pads, their peak, loudness, length, brightness and pitch. Beside it, a WAV of each preset's note and of a phrase of its kind (a bass line, a melody, chords, an arpeggio, a beat). The 2026-11-08 pass used it to find what was wrong before it was fixed: the Square Lead fading out and in each 1.3 s (two squares 5 cents apart at equal levels), the Comb Pluck 15 dB quieter than the rest, the 808 kit's snare ringing three times as long as an 808's and its clap's noise centred near 5 kHz where an 808's peaks at 1 kHz, its kick sweeping as a 909's does, and its hats full of noise an 808 hasn't, no Synth preset brighter for a harder note, and three FM basses an octave below the note played.

## What is left

- **Track presets that come with Phazite** (an instrument with its effects and macros, as Live's racks and Logic's patches are), for the sounds whose effects are part of them: the Juno pads with their chorus, the string machine with its ensemble.
- **An arpeggiator,** for the arps and sequences other libraries have.
- **Recorded instruments** (pianos, guitars, orchestral sounds) through the sampler's SFZ multisamples and CC0 libraries in Phazite Sounds ([design section 9](design.md#9-devices-containers-and-presets)).
- **A drum synth** with knobs for each voice (an 808's Decay and Tone, a 909's Tune and Attack), planned after v1 ([design section 15](design.md#15-sound-generator)): the kits here are each voice at one setting.
- **Listening:** every number above was measured, not heard; the owner's ears are the last check.
