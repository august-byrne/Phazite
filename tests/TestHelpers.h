// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared includes and helpers for Phazite's tests.
#pragma once

#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/internal/AudioWork.h"
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

// How long a test waits for something it waits on to happen, unless it says otherwise: two minutes, which only a hang
// reaches. A test waits for a condition, never for a time, so that a machine busy with other work (ctest runs four test
// files at once, CI's sanitizers run slowly) slows it rather than fails it; and a short limit is kept only where how soon
// is what is tested, and says so. Eight minutes with MSVC's AddressSanitizer (windows-asan, a Debug build), where what
// takes seconds takes minutes: its first run stretched a loop for more than two (StatusBarTests, 2026-11-15).
#if defined(_MSC_VER) && defined(__SANITIZE_ADDRESS__)
inline constexpr int hangTimeout = 480000;
#else
inline constexpr int hangTimeout = 120000;
#endif

// The tests have no message loop, but some engine work happens on the message thread's timers (finding
// MIDI devices, for example). This runs the timers that are due, as the loop would, until the
// condition holds. Returns false if it still doesn't after timeoutMilliseconds.
bool runTimersUntil (const std::function<bool()>& condition, int timeoutMilliseconds = hangTimeout);

// While one lives, a JUCE assertion at this place (JUCE's file and line, "juce_ValueTree.cpp:569"; for an assertion
// written over several lines its first and last, "juce_Time.cpp:170-175", as compilers differ in which they give) doesn't
// fail the test it fires in (TestMain.cpp fails a test for any other): for a test that makes, on purpose, the very fault
// JUCE asserts against. Several places are named with | between them. Made on the test's own thread, on any thread an
// assertion fires on (an audio thread's too); one made while another lives adds its places to the other's, for as long as
// it lives, and goes first.
class ExpectedAssertion
{
public:
    explicit ExpectedAssertion (const char* place);
    ~ExpectedAssertion();

    // How often it fired while this lived, at this one's places (an assertion at the places of several living counts for
    // the one made last): none in a build without JUCE's assertions.
    int getCount() const;

private:
    size_t level = 0; // its place among those living, the first 0

    ExpectedAssertion (const ExpectedAssertion&) = delete;
    ExpectedAssertion& operator= (const ExpectedAssertion&) = delete;
};

// How many JUCE assertions have fired on audio threads since the tests started (TestMain.cpp): a block of the tests' audio
// device, the engine's workers. The assertion net notes each there without a lock or an allocation of its own, and fails
// the test it fired in from the test's thread; what JUCE's logAssertion() allocated for its message is left out of the
// real-time audit's counts, which say that an assertion fired instead (known bug 153).
int getNumAudioThreadAssertions();
// Waits until the imports, Normalize and Quantize Audio started in a project have read their audio, in the background
// (model/arrangement/internal/AudioWork.h), and made their changes.
inline void waitForAudioWork (const ProjectContext& context)
{
    REQUIRE (runTimersUntil ([&context] { return ! isReadingAudio (context); }));
}

// The same, for whatever context the test's song is open in (a device's handle reaches its song alone).
inline void waitForAudioWork()
{
    REQUIRE (runTimersUntil ([] { return ! AudioWork::isAnyBusy(); }));
}

// Runs freezes and bounces as the app's main window does, without a window: it keeps the render while it runs, and
// what came of each.
struct RenderHost : public TrackRenderHost
{
    std::unique_ptr<TrackRender> render;
    juce::StringArray finished; // "<title>: ok", or "<title>: <why it failed>"

    void runRender (std::unique_ptr<TrackRender> r) override { render = std::move (r); }

    // As the main window's: the render goes first (it is calling back, and lets go of itself last), and what it said is used
    // after, so that a render whose title or result goes with it fails here too.
    void renderFinished (const juce::String& title, const juce::Result& result) override
    {
        render = nullptr;
        finished.add (title + ": " + (result.wasOk() ? juce::String ("ok") : result.getErrorMessage()));
    }

    juce::Array<juce::File> wrote; // the files renders wrote for the user

    void renderWrote (const juce::String&, const juce::File& file) override { wrote.add (file); }

    bool isShowingRender() const override { return render != nullptr; }
};

// A folder in the temp directory, deleted again at the end of the test.
struct TemporaryFolder
{
    TemporaryFolder() { folder.createDirectory(); }
    ~TemporaryFolder() { folder.deleteRecursively(); }

    const juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteTests", {}, false);
};

// A folder that links to another: a junction on Windows, which any user may make, and a symbolic link elsewhere. True if
// it was made.
inline bool linkFolder (const juce::File& link, const juce::File& target)
{
#if JUCE_WINDOWS
    juce::ChildProcess mklink;
    return mklink.start (juce::StringArray { "cmd", "/c", "mklink", "/J", link.getFullPathName(), target.getFullPathName() })
           && mklink.waitForProcessToFinish (10000) && link.isDirectory();
#else
    return target.createSymbolicLink (link, false) && link.isDirectory();
#endif
}

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
