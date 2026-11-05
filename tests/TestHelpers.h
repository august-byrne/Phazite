// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared includes and helpers for Phazite's tests.
#pragma once

#include "model/mixer/FreezeModel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

// doctest (external/doctest), whose warnings are off as a system header.
#include <doctest/doctest.h>

namespace phazite
{
class EngineContext;
}

namespace phazite::test
{
// The engine context that all tests share, made by TestMain.cpp like the app's (no audio device opened, no plug-in
// scanning), with plug-ins hosted in the tests' helper.
EngineContext& getEngineContext();

// The system's temporary folder, which other programs share. The tests' temporary folder (juce::File::tempDirectory) is
// a folder in it of this test program's own, made by TestMain.cpp: CTest runs a test program for each test file, several
// at once, and the tests give what they write there fixed names.
juce::File getSystemTemporaryFolder();

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

    juce::Array<juce::File> wrote; // the files renders wrote for the user

    void renderWrote (const juce::String&, const juce::File& file) override { wrote.add (file); }
};

// A folder in the temp directory, deleted again at the end of the test.
struct TemporaryFolder
{
    TemporaryFolder() { folder.createDirectory(); }
    ~TemporaryFolder() { folder.deleteRecursively(); }

    const juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteTests", {}, false);
};

// The presets that come with Phazite: resources/presets in the source tree, above the test program, as the golden tests
// find tests/ (the app finds them next to itself).
inline juce::File getFactoryPresetsFolder()
{
    auto folder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    while (! folder.getChildFile ("resources").getChildFile ("presets").isDirectory() && ! folder.isRoot())
        folder = folder.getParentDirectory();

    return folder.getChildFile ("resources").getChildFile ("presets");
}

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

// Writes a loop of clicks into a folder, one on each beat at a tempo, each a burst of noise of its own, named for its tempo
// so that it follows the song's ("Clicks 120 BPM.wav"), and returns it.
inline juce::File writeClicks (const juce::File& folder, int beats, double bpm)
{
    constexpr double sampleRate = 44100.0;
    const auto file = folder.getChildFile ("Clicks " + juce::String (bpm, 0) + " BPM.wav");
    const auto beatSamples = (int) std::lround (60.0 / bpm * sampleRate);
    juce::AudioBuffer<float> audio (1, beats * beatSamples);
    audio.clear();
    juce::Random random (3);

    for (int beat = 0; beat < beats; ++beat)
        for (int i = 0; i < 88; ++i)
            audio.setSample (0, beat * beatSamples + i, 0.5f * std::exp ((float) -i / 30.0f) * (random.nextFloat() * 2.0f - 1.0f));

    folder.createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    writer.reset();
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
