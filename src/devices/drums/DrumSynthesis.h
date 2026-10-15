// SPDX-License-Identifier: AGPL-3.0-or-later
// Drums that Phazite synthesizes, after Sound On Sound's "Synth Secrets" on drums (2002) and the analysis of the 808's
// circuits by Werner, Abel and Smith (DAFx-14, ICMC 2014): the drum sampler's default kit, and some of Phazite Sounds
// (tools/soundkit/). Each is seeded, so it comes out the same every time. They allocate: never on the audio thread.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite::devices::drums
{
using Sound = juce::AudioBuffer<float>;

// White noise from -1 to 1.
float noise (juce::Random&);

// tanh saturation, scaled so that 1 stays 1.
float softClip (double x, double drive);

// A kick, a tom or an 808: a sine whose pitch falls from startHz to endHz, a click, and saturation.
Sound makeKick (double sampleRate, double startHz, double endHz, double pitchSeconds, double decaySeconds, double length, double click,
                double drive);

// A drum's shell, two tuned sines, and its snares, high-passed noise that rings for snaresSeconds.
Sound makeSnare (double sampleRate, double lowHz = 185.0, double highHz = 330.0, double snaresSeconds = 0.11);

// Band-passed noise, struck four times about 10 ms apart, then a tail: several hands, as the 808's clap does it.
Sound makeClap (double sampleRate);

// Six square waves at the 808's cymbal frequencies, band-passed and high-passed: a hat, closed or open, or a cymbal.
Sound makeHat (double sampleRate, double decaySeconds, double length);

// Two square waves, 540 and 800 Hz, through a band-pass: a strike, then a ringing tail.
Sound makeCowbell (double sampleRate);
} // namespace phazite::devices::drums
