# Sound generator

*Design section 15.*

The sound generator is to make new kicks, snares and other drum sounds a set at a time, for the user to try in the
song and keep or favourite the ones they like. None of it is built: it is planned for after v1.

## Status

| Feature | State | Note |
| --- | --- | --- |
| The generator panel: a grid of sounds by drum type and character | Planned | After v1 ([roadmap](../roadmap.md#after-10), "Sound generator") |
| Keep, favourite, lock, more like this, history | Planned | After v1 |
| Sounds fitted to the project: key, tempo, loudness | Planned | After v1 |
| A recipe kept with every sound | Planned | After v1 |
| A drum synthesizer as an instrument | Planned | After v1 |
| Breeding two favourites; generation that complements the kit in use | Planned | After v1, after the first version |
| A trained (neural) model as the engine | Planned | After v1; no model with a licence that fits Phazite's exists |

## What is intended

**For the user:**

- **Open it** from the browser, from a drum pad, or from a drum track's right-click menu.
- **Pick a type:** kick, 808, snare, clap, closed hat, open hat, tom, rim or percussion. Optionally add character
  words: punchy, boomy, tight, dirty, dark, bright, short, long, lo-fi.
- **Generate** fills a grid with 12 sounds. A click plays one. Trying one in context swaps it into the playing loop
  for the moment.
- **For each sound:** keep it (it goes into the project), favourite it (it goes into the library), lock it (it stays
  while the rest are made again), ask for more like it (the next set varies around it, by an amount a slider sets),
  or drag it onto a track or a pad.
- **It uses the project:** kicks, 808s and toms tuned to the project's key, decays that fit the tempo, loudness that
  matches the rest of the kit. Later, it steers away from the frequencies the drums in use already fill.
- **History** steps back and forth through earlier sets.
- **Every sound keeps its recipe** (the engine's version, the seed and the parameters), so it can be opened again
  and changed in the drum synthesizer, or rendered again at any sample rate.

**The first engine: a drum synthesizer plus evolution.**

- Each drum type is a synthesis patch: a kick is a pitched oscillator with a pitch envelope, a click and saturation;
  a snare is a tuned body plus filtered noise; hats are metallic oscillators or FM through filters.
- Generate draws the patch's parameters from ranges tuned for each character word, measures the results (loudness,
  length, brightness) and discards weak ones.
- More like this changes the chosen sound's parameters a little. Later, breeding combines two favourites.
- The drum synthesizer is also an instrument, so a generated kick can stay live and editable on a track.

## What exists that it would build on

- Phazite already synthesizes drum sounds from recipes, for the drum sampler's factory kits: kicks, snares, claps,
  hats, struck sounds and rooms, each from a shape of parameters and a seed (`src/devices/drums/DrumSynthesis.h`,
  `FactoryDrums.h`, `DrumMachines.h`). They are fixed recipes, not generated, and there is no drum synthesizer
  device.
- Swap ([browser.md](browser.md)) already tries a sound on a drum pad in the playing beat and keeps it or puts the
  old one back.

## Decisions

- **Sets of sounds to choose from,** not one sound made to order: the user keeps what they like each time.
- **Synthesis and evolution first, not a trained model.** No drum-generation model with a licence that fits the AGPL
  exists, synthesized drums are a staple of electronic music anyway, and a recipe is small, repeatable and editable.
  A trained model would add most for realistic acoustic drums, later, behind the same interface.
- **Every sound keeps its recipe,** so results can be reproduced and labelled accurately ([ai.md](ai.md)).
- **After v1,** not in it.

## Not built

Everything on this page: Planned, after v1.

## In the code

Nothing yet. The drum synthesis it would start from is in `src/devices/drums/`.
