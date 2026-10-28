// SPDX-License-Identifier: AGPL-3.0-or-later
// How a sound is stretched in time, shifted in pitch and played backwards, as audio clips play it (roadmap step 2.18):
// Rubber Band Library's R3 engine, offline, over the whole sound at once, which played no drum hit twice, every hit on
// its beat, and a held chord cleanly, where SoundTouch did none of those (docs/design.md, section 23). It reads and
// writes through functions, so that whoever calls it decides where the sound comes from and where it goes.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>

namespace phazite::stretch
{
// How a sound plays: so many times as long (above 1 is slower), shifted in pitch by semitones, and backwards or not.
struct Settings
{
    double timeRatio = 1.0;
    double semitones = 0.0;
    bool reversed = false;

    bool operator== (const Settings&) const = default;
};

// Whether the settings play a sound as it is.
bool isUnchanged (const Settings&);

// How many frames a sound of so many frames lasts with the settings.
juce::int64 getStretchedLength (juce::int64 frames, const Settings&);

// The sound to render: its channels, rate and length in frames, and a function that fills the first frames of a buffer
// (as many as given) with the sound from a frame on, with silence past its end.
struct Source
{
    int numChannels = 0;
    double sampleRate = 0.0;
    juce::int64 length = 0;
    std::function<void (juce::AudioBuffer<float>&, juce::int64 start, int numFrames)> read;
};

// Takes the next frames rendered (the buffer's first numFrames), and says whether to go on.
using Sink = std::function<bool (const juce::AudioBuffer<float>&, int numFrames)>;

// Renders all of a sound with the settings into the sink, which gets getStretchedLength() frames in all, in blocks.
// Returns false if the sink, or shouldStop (asked between blocks), stopped it. It takes a while: about 20 times faster
// than real time for each channel when it stretches or shifts the pitch, and a copy otherwise.
bool render (const Source&, const Settings&, const Sink&, const std::function<bool()>& shouldStop = {});
} // namespace phazite::stretch
