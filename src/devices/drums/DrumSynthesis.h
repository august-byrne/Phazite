// SPDX-License-Identifier: AGPL-3.0-or-later
// Drums that Phazite synthesizes, after Sound On Sound's "Synth Secrets" on drums (2002) and the analysis of the 808's
// circuits by Werner, Abel and Smith (DAFx-14, ICMC 2014): the drum sampler's factory sounds (FactoryDrums.h), and some of
// Phazite Sounds (tools/soundkit/). Each is seeded, so it comes out the same every time. They allocate: never on the
// audio thread.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite::devices::drums
{
using Sound = juce::AudioBuffer<float>;

// White noise from -1 to 1.
float noise (juce::Random&);

// tanh saturation, scaled so that 1 stays 1.
float softClip (double x, double drive);

// A kick, a tom, an 808, a rim or a clave: a sine whose pitch falls from startHz to endHz, a click, and saturation.
Sound makeKick (double sampleRate, double startHz, double endHz, double pitchSeconds, double decaySeconds, double length, double click,
                double drive);

// A kick or a drum shaped more closely, as a sampled one measures: its pitch falling in two stages, a chirp from chirpHz
// in its first milliseconds and then from startHz to endHz; held at full level for holdSeconds before it decays, as a
// gated drum is; and a click of noise band-passed around clickHz.
struct KickShape
{
    double chirpHz = 0.0, chirpSeconds = 0.0015; // no chirp at 0
    double startHz = 160.0, endHz = 50.0, pitchSeconds = 0.03;
    double holdSeconds = 0.0, decaySeconds = 0.3;
    double click = 0.5, clickHz = 1000.0, clickSeconds = 0.003;
    double noise = 0.0, noiseHz = 4000.0, noiseSeconds = 0.05; // a breath of band-passed noise under the body
    double length = 0.6, drive = 1.6;
    int seed = 1;
};

Sound makeKick (double sampleRate, const KickShape&);

// A snare: its shell, two tuned sines that fade each at its own pace, and its snares, band-passed noise that rings for
// snaresSeconds. The defaults are the default kit's snare.
struct SnareShape
{
    double lowHz = 185.0, highHz = 330.0;
    double lowSeconds = 0.07, highSeconds = 0.045;
    double snaresSeconds = 0.11;
    double snaresLevel = 0.55;                           // against the shell's tones, at most 1
    double snaresLowCut = 800.0, snaresHighCut = 9000.0; // Hz
    double length = 0.45, drive = 1.6;
    int seed = 2;
    double lowLevel = 0.6, highLevel = 0.4; // the shell's tones' levels
};

Sound makeSnare (double sampleRate, const SnareShape& = {});

// A clap: band-passed noise struck four times about 10 ms apart (times spread), then a tail: several hands, as the 808's
// clap does it.
struct ClapShape
{
    double centreHz = 1100.0, q = 1.3, lowCut = 500.0;
    double spread = 1.0; // the strikes' spacing, against the 808's
    double tailSeconds = 0.13;
    double length = 0.55;
    int seed = 3;
    double tailLevel = 1.0; // the tail's level against the strikes', as a room or a plate leaves it
};

Sound makeClap (double sampleRate, const ClapShape& = {});

// A hat, closed or open, or a cymbal: six square waves at the 808's cymbal frequencies (times pitch), with some noise,
// band-passed and high-passed.
struct HatShape
{
    double decaySeconds = 0.05, length = 0.25;
    double noise = 0.15; // against the square waves' metal
    double centreHz = 7100.0, lowCut = 6000.0;
    double pitch = 1.0;
    int seed = 4;
};

Sound makeHat (double sampleRate, const HatShape&);

// Two square waves, 540 and 800 Hz, through a band-pass: a strike, then a ringing tail.
Sound makeCowbell (double sampleRate);

// Two mono sounds as one stereo sound, the left and the right, as long as the longer.
Sound makeStereo (const Sound& left, const Sound& right);

// A drum as an old sampler played it back: read at heldRate with bits of resolution, its top cut at cutoffHz, and
// saturated with drive. In place.
struct DustShape
{
    double heldRate = 22050.0;
    int bits = 12;
    double cutoffHz = 6000.0;
    double drive = 1.5;
};

void makeDusty (Sound&, double sampleRate, const DustShape&);
} // namespace phazite::devices::drums
