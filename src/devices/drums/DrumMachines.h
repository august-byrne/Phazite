// SPDX-License-Identifier: AGPL-3.0-or-later
// The voices of the classic drum machines, made as their circuits make them (docs/presets.md), for the drum sampler's kits
// that are after them (FactoryDrums.h): the Roland TR-808's sixteen, after Werner, Abel and Smith's analyses of its bass drum
// and its cymbal (DAFx-14, ICMC 2014) and its service notes' chart of each voice's pitch and decay; the TR-909's analog
// voices, and its sampled hats and cymbals as dense metal at six bits; and a CR-78's, from what is known of them. Times are
// as the service notes give them: a decay's T20, the time it takes to fall 20 dB. Each is seeded, so it comes out the same
// every time. They allocate: never on the audio thread.
#pragma once

#include "DrumSynthesis.h"

namespace phazite::devices::drums
{
//==============================================================================
// The 808's. Its bass drum: a bridged-T resonator at about 50 Hz, its first half cycle three times as fast (the crisp
// start), then a sigh from a quarter above down onto its pitch, its 1 ms trigger leaking through as a click, and Tone a
// low-pass over it all (300 Hz to 7.2 kHz). Decay from 0.05 to 0.9 s (T20).
Sound make808Kick (double sampleRate, double decaySeconds = 0.8, double toneHz = 1600.0);

// Its snare: two resonators (238 and 476 Hz) that Tone crossfades, and its snares, noise from 2.75 to 5 kHz, as loud as
// Snappy says (both from 0 to 1).
Sound make808Snare (double sampleRate, double tone = 0.5, double snappy = 0.6);

// Its toms (with a faint low noise behind each, its "reverb") and congas (without): one resonator each, swelling in over a
// few milliseconds, a little sharp as it is struck.
Sound make808Tom (double sampleRate, double hz, double decaySeconds);
Sound make808Conga (double sampleRate, double hz, double decaySeconds);

// Its rim shot (two resonators chopped short and clipped by its VCA, high-passed), claves (a 2.5 kHz ring cut at 21 ms),
// hand clap (noise at 1.1 kHz struck four times 11.5 ms apart, and a tail that rises slowly behind them), maracas (noise
// above 10 kHz, opening for 32 ms), cowbell (its 540 and 800 Hz squares through a band-pass at 890 Hz) and cymbal (its six
// squares through bands at 3.44 and 7.1 kHz, each with an envelope of its own, Tone balancing them).
Sound make808Rim (double sampleRate);
Sound make808Claves (double sampleRate);
Sound make808Clap (double sampleRate);
Sound make808Maracas (double sampleRate);
Sound make808Cowbell (double sampleRate, double drive = 0.0); // above 0, saturated, as phonk plays it
Sound make808Cymbal (double sampleRate, double decaySeconds = 0.8, double tone = 0.5);

// Its hats: the six squares' 7.1 kHz band through its VCA and a resonant high-pass, the closed hat's at 11.7 kHz, the open
// hat's at 7.8 kHz, cut short where its Decay says (from 0.09 to 0.6 s).
Sound make808ClosedHat (double sampleRate);
Sound make808OpenHat (double sampleRate, double decaySeconds = 0.45);

//==============================================================================
// The 909's. Its bass drum: a triangle rounded by diodes into a near sine, swept down from four times its 52 Hz over a time
// that Tune sets, holding its level for a moment before it falls (Decay's T20), with a click of low-passed noise (Attack,
// from 0 to 1).
Sound make909Kick (double sampleRate, double sweepSeconds = 0.018, double decaySeconds = 0.45, double attack = 0.6);

// Its snare: two rounded triangles a ratio of 1.585 apart, bumped up as they start, and noise that holds, then falls fast:
// Tone, from 0 to 1, how long; Snappy, how loud.
Sound make909Snare (double sampleRate, double tone = 0.5, double snappy = 0.7, double hz = 190.0);

// Its toms: three partials (1, 1.5 and 2.75 times the pitch) swept down together, the lowest ringing for the decay.
Sound make909Tom (double sampleRate, double hz, double decaySeconds = 0.35);

// Its rim shot (three resonators clipped by diodes, short and high-passed) and hand clap (the 808's, cleaner, its tail
// longer).
Sound make909Rim (double sampleRate);
Sound make909Clap (double sampleRate);

// Its sampled metal, as its six-bit converter plays it back at 30 kHz: hats (closed and open), crash and ride.
Sound make909ClosedHat (double sampleRate);
Sound make909OpenHat (double sampleRate, double decaySeconds = 0.3);
Sound make909Crash (double sampleRate);
Sound make909Ride (double sampleRate);

//==============================================================================
// A rhythm box's, as the CR-78's: a small kick and snare, a rim, hats and cymbal of ringing band-passed noise, its metallic
// beat (three squares through a band-pass at 5.6 kHz), maracas, tambourine, a guiro's scrape, and pitched resonators for its
// cowbell, claves, bongos and conga.
Sound makeBoxKick (double sampleRate);
Sound makeBoxSnare (double sampleRate);
Sound makeBoxRim (double sampleRate);
Sound makeBoxHat (double sampleRate, double decaySeconds);
Sound makeBoxCymbal (double sampleRate);
Sound makeBoxMetal (double sampleRate);
Sound makeBoxMaracas (double sampleRate);
Sound makeBoxTambourine (double sampleRate);
Sound makeBoxGuiro (double sampleRate);
Sound makeBoxCowbell (double sampleRate);
Sound makeBoxClaves (double sampleRate, double hz = 2200.0, double decaySeconds = 0.05);
Sound makeBoxDrum (double sampleRate, double hz, double decaySeconds); // a bongo or conga

//==============================================================================
// What a modern kit has: an 808 played as a bass, long, falling an octave onto its pitch and saturated so that it carries
// on small speakers; a short kick that cuts through over it; a crisp snare and a layered clap; tight hats for rolls; and a
// finger snap.
Sound makeLongBass808 (double sampleRate, double hz = 49.0, double decaySeconds = 2.2, double drive = 2.2);
Sound makePunchKick (double sampleRate);
Sound makeCrispSnare (double sampleRate);
Sound makeLayeredClap (double sampleRate);
Sound makeSnap (double sampleRate);
Sound makeTightHat (double sampleRate, double decaySeconds);

// Hand percussion: a conga or bongo (open, or slapped), a timbale, an agogo bell, a woodblock, a shaker, a tambourine, a
// triangle (open or muted), and a cabasa.
Sound makeHandDrum (double sampleRate, double hz, double decaySeconds, bool slapped);
Sound makeTimbale (double sampleRate, double hz);
Sound makeAgogo (double sampleRate, double hz);
Sound makeWoodblock (double sampleRate, double hz);
Sound makeShaker (double sampleRate);
Sound makeTambourine (double sampleRate);
Sound makeTriangle (double sampleRate, bool muted);
Sound makeCabasa (double sampleRate);
} // namespace phazite::devices::drums
