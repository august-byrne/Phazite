// SPDX-License-Identifier: AGPL-3.0-or-later
// The metronome's click (Phazite's own, synthesized, since no sounds go in this repository): a short knock on each
// beat, higher and louder on a bar's first, the same on both sides.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace phazite::engine
{
// One click, mono, at a sample rate: a bar's first beat's (accent) or another beat's.
juce::AudioBuffer<float> makeClick (double sampleRate, bool accent);
} // namespace phazite::engine
