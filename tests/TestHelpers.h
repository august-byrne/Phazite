// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared includes and helpers for Phazite's tests.
#pragma once

#include "model/mixer/FreezeModel.h"

#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

// doctest, through Tracktion's wrapper, which silences doctest's warnings under JUCE's warning flags.
#include <tracktion_engine/../3rd_party/doctest/tracktion_doctest.hpp>

namespace te = tracktion;

namespace phazite
{
class EngineContext;
}

namespace phazite::test
{
// The Engine that all tests share, made by TestMain.cpp like the app's (no audio device, no plug-in
// scanning). Its settings live in a temporary folder, so tests never touch Phazite's real settings.
te::Engine& getEngine();
EngineContext& getEngineContext(); // the Engine as the UI reaches it, as the app's services make it

// The tests have no message loop, but some engine work happens on the message thread's timers (finding
// MIDI devices, for example). This runs the timers that are due, as the loop would, until the
// condition holds. Returns false if it still doesn't after timeoutMilliseconds.
bool runTimersUntil (const std::function<bool()>& condition, int timeoutMilliseconds = 2000);

// Runs freezes and bounces as the app's main window does, without a window: it keeps the render while it runs, and
// what came of each.
struct RenderHost : public TrackRenderHost
{
    std::unique_ptr<TrackRender> render;
    juce::StringArray finished; // "<title>: ok", or "<title>: <why it failed>"

    void runRender (std::unique_ptr<TrackRender> r) override { render = std::move (r); }

    void renderFinished (const juce::String& title, const juce::Result& result) override
    {
        finished.add (title + ": " + (result.wasOk() ? juce::String ("ok") : result.getErrorMessage()));
        render = nullptr; // it is calling back, and lets go of itself last
    }
};

// A folder in the temp directory, deleted again at the end of the test.
struct TemporaryFolder
{
    TemporaryFolder() { folder.createDirectory(); }
    ~TemporaryFolder() { folder.deleteRecursively(); }

    const juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteTests", {}, false);
};

// Writes a WAV file (and the folders it is in) of a sine, some seconds long, and returns it: by default a quiet A
// (220 Hz, at a quarter of full scale).
inline juce::File writeSound (const juce::File& file, double seconds, double frequency = 220.0, float amplitude = 0.25f)
{
    constexpr double sampleRate = 44100.0;
    juce::AudioBuffer<float> buffer (1, (int) (seconds * sampleRate));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample (0, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / sampleRate));

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16)))
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());

    return file;
}
} // namespace phazite::test

// doctest prints juce::Strings in failure messages.
namespace juce
{
inline doctest::String toString (const juce::String& s)
{
    return s.toRawUTF8();
}
} // namespace juce
