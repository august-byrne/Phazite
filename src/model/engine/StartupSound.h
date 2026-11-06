// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound Phazite makes once it has opened (Settings::startupSound turns it off): a quick bright blip, six metallic steps
// that zig-zag in a tenth of a second, whose echoes bounce quickly between the sides and ring on. Phazite's own, synthesized
// as it plays, since no sounds go in this repository, its echoes made by Phazite's delay (devices/delay/Delay.h). It plays
// on the previews' engine, at the previews' volume (Preview.h).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite
{
class Previewer;

// The sound, in stereo at a sample rate: 1.5 seconds, peaking at -6 dBFS in its blip, ending in silence, and the same every
// time.
juce::AudioBuffer<float> makeStartupSound (double sampleRate);

// Plays it through the previewer, at the rate the previews play at, in place of what it plays.
void playStartupSound (Previewer&);
} // namespace phazite
