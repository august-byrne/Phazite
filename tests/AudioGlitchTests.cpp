// SPDX-License-Identifier: AGPL-3.0-or-later
// Glitches on a real audio output (Windows), for crackles heard in Windows Audio's shared modes and not in its exclusive
// mode (the owner, 2026-11-02): an example song plays through Phazite's engine, as the app plays it, on its threads, on an
// output opened in a shared mode, the engine's sound replaced by a quiet pilot tone; and what Windows' audio engine mixes
// for that output is captured as WASAPI's loopback gives it. Counted: the capture's discontinuities (Windows' engine missed
// a period), the pilot's breaks (a block of Phazite's came late), and how long the engine's blocks took. Not in ctest: it
// needs a real output, takes half a minute, and its numbers are the machine's. Run it as
//     phazite_tests.exe --test-suite=device --no-skip
// with, in the environment: PHAZITE_GLITCH_OUTPUT (the output's name, by default the first that isn't Bluetooth),
// PHAZITE_GLITCH_TYPE ("Windows Audio", the default, or "Windows Audio (Low Latency Mode)"), PHAZITE_GLITCH_SECONDS (30),
// PHAZITE_GLITCH_PATH ("app", the default: through Tracktion's device manager as the app plays; or "engine": Phazite's
// engine alone), PHAZITE_GLITCH_SONG ("off": the pilot alone, to compare), PHAZITE_GLITCH_THREADS (the engine's threads,
// by default the app's), PHAZITE_GLITCH_BUSY (threads kept busy at the normal priority meanwhile, as a busy UI would, 0 by
// default), PHAZITE_GLITCH_ALLOCATE (1: those threads allocate as they go, as a UI painting does), PHAZITE_GLITCH_RESTART
// (milliseconds between the song stopped and played again from its start, 0 by default) and PHAZITE_GLITCH_STALL
// (milliseconds a block is held up every 5 seconds, to check that what is counted catches it, 0 by default).
//
// What it found on the owner's machine (2026-11-02, debug build, 48 kHz, 480 samples): no break and no late block on its
// Realtek output or its Bluetooth headphones in either shared mode, with or without the song, restarted every 2 s, with
// threads busy computing or allocating; a shared stream's buffer is 1056 samples (22 ms) on each output (the second test
// case), and a block held up 12 ms still played whole. The app logs blocks that come late (Engine::takeBlockTimes()).
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/MixerModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <audioclient.h>
 #include <mmdeviceapi.h>
 #include <windows.h>

namespace
{
// An output's name as Windows lists it (functiondiscoverykeys_devpkey.h's PKEY_Device_FriendlyName).
const PROPERTYKEY friendlyNameKey { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

constexpr double pilotHz = 997.0;
constexpr float pilotLevel = 0.001f; // -60 dBFS

juce::String getVariable (const char* name, const juce::String& fallback)
{
    const auto value = juce::SystemStats::getEnvironmentVariable (name, {});
    return value.isNotEmpty() ? value : fallback;
}

// Where Windows' engine mixes an output: its loopback, read every few milliseconds on a thread of its own. Each packet's
// discontinuity flag counts one of the engine's glitches; the pilot's samples, each against the two before (a sine's next
// sample is 2 cos w times the last less the one before), count its breaks.
class LoopbackCapture final : private juce::Thread
{
public:
    explicit LoopbackCapture (const juce::String& outputName)
        : juce::Thread ("Loopback capture"),
          name (outputName)
    {
    }

    ~LoopbackCapture() override { stopThread (2000); }
    void stop() { stopThread (2000); }

    bool start()
    {
        startThread();
        return opened.wait (5000) && ok.load();
    }

    std::atomic<int> discontinuities { 0 }, breaks { 0 }, packets { 0 };
    juce::String breakTimes; // seconds into the capture of its first breaks, written by its thread, read once it has stopped
    std::atomic<double> sampleRate { 0.0 }, peak { 0.0 }; // the pilot's, as captured: the capture hears it

private:
    juce::String name;
    juce::WaitableEvent opened;
    std::atomic<bool> ok { false };

    void run() override
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        capture();
        opened.signal();
        CoUninitialize();
    }

    void capture()
    {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDeviceCollection* outputs = nullptr;
        IMMDevice* found = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        WAVEFORMATEX* format = nullptr;

        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator),
                                      (void**) &enumerator)))
            return;

        UINT count = 0;

        if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &outputs)))
            outputs->GetCount (&count);

        for (UINT i = 0; i < count && found == nullptr; ++i)
        {
            IMMDevice* device = nullptr;
            IPropertyStore* properties = nullptr;
            PROPVARIANT value;
            PropVariantInit (&value);

            if (SUCCEEDED (outputs->Item (i, &device)) && SUCCEEDED (device->OpenPropertyStore (STGM_READ, &properties))
                && SUCCEEDED (properties->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR
                && juce::String (value.pwszVal) == name)
                std::swap (found, device);

            PropVariantClear (&value);

            if (properties != nullptr)
                properties->Release();

            if (device != nullptr)
                device->Release();
        }

        const auto done = [&]
        {
            if (format != nullptr)
                CoTaskMemFree (format);

            for (IUnknown* each : std::initializer_list<IUnknown*> { captureClient, client, found, outputs, enumerator })
                if (each != nullptr)
                    each->Release();
        };

        if (found == nullptr || FAILED (found->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &client))
            || FAILED (client->GetMixFormat (&format))
            || FAILED (client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, format, nullptr))
            || FAILED (client->GetService (__uuidof (IAudioCaptureClient), (void**) &captureClient)) || FAILED (client->Start()))
        {
            done();
            return;
        }

        // The mix as 32-bit floats, Windows' own format for a shared mode's mix.
        const auto channels = (int) format->nChannels;
        sampleRate = (double) format->nSamplesPerSec;
        const auto twoCos = 2.0 * std::cos (juce::MathConstants<double>::twoPi * pilotHz / sampleRate.load());
        double last = 0.0, beforeLast = 0.0;
        auto known = 0; // samples since a break, of the two needed to predict the next
        std::int64_t captured = 0;
        ok = true;
        opened.signal();

        while (! threadShouldExit())
        {
            juce::Thread::sleep (3);
            UINT32 packet = 0;

            while (SUCCEEDED (captureClient->GetNextPacketSize (&packet)) && packet > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                if (FAILED (captureClient->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                    break;

                ++packets;

                // The first packet is a discontinuity as the capture starts.
                if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && packets.load() > 1)
                    ++discontinuities;

                const auto* samples = reinterpret_cast<const float*> (data);

                for (UINT32 frame = 0; frame < frames; ++frame)
                {
                    const auto sample = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ? 0.0 : (double) samples[frame * (UINT32) channels];

                    peak = std::max (peak.load(), std::abs (sample));

                    // Not in its first half second, as the pilot starts.
                    const auto settled = ++captured > (std::int64_t) (sampleRate.load() / 2.0);

                    if (known >= 2 && std::abs (sample - (twoCos * last - beforeLast)) > pilotLevel * 0.2 && settled)
                    {
                        if (++breaks <= 10)
                            breakTimes << juce::String ((double) captured / sampleRate.load(), 2) << " s ";

                        known = 0;
                    }

                    beforeLast = last;
                    last = sample;
                    ++known;
                }

                captureClient->ReleaseBuffer (frames);
            }
        }

        client->Stop();
        done();
    }
};

// A callback of the device's: Phazite's engine's block, as the app's PlaybackHost plays it, if given the engine (else the
// app's own callbacks play it, before this one), then the pilot in its place (JUCE adds what callbacks after the first
// play).
struct Player final : juce::AudioIODeviceCallback
{
    explicit Player (phazite::engine::Engine* e)
        : engine (e)
    {
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override { rate = device->getCurrentSampleRate(); }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const auto start = juce::Time::getHighResolutionTicks();

        if (lastStart != 0)
            longestGap = std::max (longestGap.load(), juce::Time::highResolutionTicksToSeconds (start - lastStart) * 1000.0);

        lastStart = start;

        if (engine != nullptr)
            engine->process (nullptr, 0, outputs, numOutputs, numSamples);

        // A block held up on purpose now and then, to see that what is counted catches it.
        if (stallMilliseconds > 0 && blocks.load() % 500 == 250)
            juce::Thread::sleep (stallMilliseconds);

        for (int i = 0; i < numSamples; ++i, ++pilotSample)
        {
            const auto value = pilotLevel * (float) std::sin (juce::MathConstants<double>::twoPi * pilotHz * (double) pilotSample / rate);

            for (int channel = 0; channel < numOutputs; ++channel)
                if (outputs[channel] != nullptr)
                    outputs[channel][i] = value;
        }

        const auto took = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start) * 1000.0;
        longestBlock = std::max (longestBlock.load(), took);

        if (took > 5.0)
            ++slowBlocks;
        ++blocks;
    }

    phazite::engine::Engine* engine;
    int stallMilliseconds = 0;
    double rate = 48000.0;
    std::int64_t pilotSample = 0, lastStart = 0;
    std::atomic<double> longestBlock { 0.0 }, longestGap { 0.0 };
    std::atomic<int> blocks { 0 }, slowBlocks { 0 };
};
} // namespace

TEST_CASE ("Device glitches: an example song through Phazite's engine on a real output in a shared mode, its mix captured"
           * doctest::test_suite ("device") * doctest::skip())
{
    // As the app plays (PHAZITE_GLITCH_PATH "app", the default): the output opened through Tracktion's device manager, whose
    // callback and Phazite's engine's play on it, with Tracktion's threads as the app has them. Or ("engine") Phazite's
    // engine alone, on a device manager of its own.
    const auto asApp = getVariable ("PHAZITE_GLITCH_PATH", "app") == "app";
    auto& tracktion = phazite::test::getEngine();
    auto& behaviour = static_cast<phazite::EngineBehaviour&> (tracktion.getEngineBehaviour());
    juce::AudioDeviceManager own;
    auto& devices = asApp ? tracktion.getDeviceManager().deviceManager : own;
    const auto typeBefore = devices.getCurrentAudioDeviceType();
    const auto setupBefore = devices.getAudioDeviceSetup();

    devices.getAvailableDeviceTypes(); // which makes them
    devices.setCurrentAudioDeviceType (getVariable ("PHAZITE_GLITCH_TYPE", "Windows Audio"), true);
    auto* type = devices.getCurrentDeviceTypeObject();
    REQUIRE (type != nullptr);
    type->scanForDevices();

    // The output: the one named, or else the first that isn't Bluetooth (whose headphones its user may be wearing).
    auto outputName = getVariable ("PHAZITE_GLITCH_OUTPUT", {});

    for (const auto& name : type->getDeviceNames (false))
        if (outputName.isEmpty() && ! name.containsIgnoreCase ("headphone pro") && ! name.containsIgnoreCase ("hands-free"))
            outputName = name;

    REQUIRE (outputName.isNotEmpty());

    // The engine's threads, as the app's (unless asked for others), and Tracktion's, set while nothing plays.
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    auto& engine = host.getEngine();
    const auto threads =
        getVariable ("PHAZITE_GLITCH_THREADS", juce::String (std::max (1, juce::SystemStats::getNumPhysicalCpus() - 1))).getIntValue();
    engine.setNumThreads (threads);

    if (asApp)
        behaviour.setNumAudioThreads (threads);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = outputName;
    setup.sampleRate = 48000.0;
    setup.bufferSize = 480;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = true;
    REQUIRE ((asApp ? devices.setAudioDeviceSetup (setup, true) : devices.initialise (0, 2, nullptr, false, {}, &setup)).isEmpty());
    auto* device = devices.getCurrentAudioDevice();
    REQUIRE (device != nullptr);
    const auto opened = devices.getCurrentAudioDeviceType() + ", " + juce::String (device->getCurrentSampleRate()) + " Hz, "
                        + juce::String (device->getCurrentBufferSizeSamples()) + " samples";

    if (! asApp)
        host.prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());

    // House Groove, as Help > Example Songs makes it, playing, its master silent: what it plays is all worked out, and the
    // pilot is all that is heard.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (
        tracktion, temp.folder, "houseGroove", error,
        phazite::getExampleSongContent (phazite::test::getEngineContext(), "houseGroove", phazite::test::getFactoryPresetsFolder()));
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    const auto withSong = getVariable ("PHAZITE_GLITCH_SONG", "on") != "off"; // off: the pilot alone, to compare
    setEdit (context, &project->getEdit());
    phazite::getMasterVolume (context).setValue (0.0f);
    phazite::test::settle();

    if (withSong)
        phazite::playOrStop (project->getEdit());

    phazite::test::dispatchPendingMessages();

    LoopbackCapture capture (outputName);
    REQUIRE (capture.start());

    Player player (asApp ? nullptr : &engine);
    player.stallMilliseconds = getVariable ("PHAZITE_GLITCH_STALL", "0").getIntValue();
    devices.addAudioCallback (&player);

    // Threads at the normal priority kept busy meanwhile, as a busy UI keeps one.
    std::atomic<bool> busy { true };
    std::vector<std::thread> busyThreads;

    const auto busyCount = getVariable ("PHAZITE_GLITCH_BUSY", "0").getIntValue();
    const auto allocating = getVariable ("PHAZITE_GLITCH_ALLOCATE", "0") != "0"; // busy allocating, as a UI painting is

    for (int i = 0; i < busyCount; ++i)
        busyThreads.emplace_back (
            [&busy, allocating]
            {
                volatile double sink = 0.0;

                while (busy.load())
                {
                    if (allocating)
                    {
                        juce::String text;

                        for (int n = 0; n < 64; ++n)
                            text << n << ", ";

                        std::vector<float> points ((size_t) (64 + text.length()));
                        sink = sink + (double) points.size();
                    }
                    else
                    {
                        sink = sink + std::sqrt (sink + 1.0);
                    }
                }
            });

    // The song stopped and played again from its start now and then (PHAZITE_GLITCH_RESTART, the milliseconds between),
    // as playback starting does what steady playback doesn't.
    const auto seconds = getVariable ("PHAZITE_GLITCH_SECONDS", "30").getIntValue();
    const auto restartEvery = (juce::uint32) getVariable ("PHAZITE_GLITCH_RESTART", "0").getIntValue();
    auto nextToggle = juce::Time::getMillisecondCounter() + restartEvery;
    auto restarts = 0;

    phazite::test::dispatchMessagesUntil (
        [&]
        {
            if (withSong && restartEvery > 0 && juce::Time::getMillisecondCounter() >= nextToggle)
            {
                const auto playing = phazite::isPlaying (context);
                phazite::playOrStop (project->getEdit());
                nextToggle += playing ? 300 : restartEvery;
                restarts += playing ? 0 : 1;
            }

            return false;
        },
        seconds * 1000);

    busy = false;

    for (auto& each : busyThreads)
        each.join();

    devices.removeAudioCallback (&player);
    capture.stop();

    if (withSong)
        phazite::playOrStop (project->getEdit());
    setEdit (context, nullptr);
    project = nullptr;

    if (asApp)
    {
        devices.setCurrentAudioDeviceType (typeBefore, true);
        devices.setAudioDeviceSetup (setupBefore, true);
        behaviour.setNumAudioThreads (1);
    }
    else
    {
        devices.closeAudioDevice();
    }

    engine.setNumThreads (1);
    host.prepare (48000.0, 256);

    MESSAGE (juce::String (asApp ? "As the app: " : "The engine alone: ")
             << "output " << outputName << " (" << opened << "), " << threads << " engine threads, " << busyCount << " busy threads, "
             << juce::String (withSong ? "the song" : "no song") << ", " << seconds << " s: " << capture.discontinuities.load()
             << " discontinuities in Windows' mix, " << capture.breaks.load() << " breaks in the pilot (" << capture.breakTimes
             << "heard at " << juce::Decibels::gainToDecibels ((float) capture.peak.load()) << " dBFS), " << player.blocks.load()
             << " blocks, the longest " << player.longestBlock.load() << " ms (" << player.slowBlocks.load()
             << " over 5 ms), the longest gap between blocks " << player.longestGap.load() << " ms, " << restarts << " restarts");
}

TEST_CASE ("Device timing: House Groove's first blocks from a stop, each as long as it takes the engine" * doctest::test_suite ("device")
           * doctest::skip())
{
    // As the app plays it, on the app's threads, at 48 kHz in blocks of 480 (10 ms): the first play after it opens, then
    // another from a stop, each block of the engine timed (the owner's log, 2026-11-03, had a block 11.5 ms late as
    // playback started).
    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    auto& engine = host.getEngine();
    const auto threads =
        getVariable ("PHAZITE_GLITCH_THREADS", juce::String (std::max (1, juce::SystemStats::getNumPhysicalCpus() - 1))).getIntValue();
    engine.setNumThreads (threads);
    host.prepare (48000.0, 480);

    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (
        phazite::test::getEngine(), temp.folder, "houseGroove", error,
        phazite::getExampleSongContent (phazite::test::getEngineContext(), "houseGroove", phazite::test::getFactoryPresetsFolder()));
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &project->getEdit());
    phazite::test::settle();

    juce::AudioBuffer<float> block (2, 480);
    juce::String report ("\n");

    // PHAZITE_GLITCH_IDLE: seconds the computer idles before each play, as before the owner pressed play.
    const auto idleMilliseconds = (int) (getVariable ("PHAZITE_GLITCH_IDLE", "0").getDoubleValue() * 1000.0);

    for (const auto* run : { "first play", "played again" })
    {
        if (idleMilliseconds > 0)
            juce::Thread::sleep (idleMilliseconds);

        phazite::playOrStop (project->getEdit());
        phazite::test::dispatchPendingMessages();
        std::vector<double> took;

        for (int i = 0; i < 300; ++i)
        {
            const auto start = juce::Time::getHighResolutionTicks();
            host.process (nullptr, 0, block.getArrayOfWritePointers(), 2, 480);
            took.push_back (juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start) * 1000.0);
        }

        phazite::playOrStop (project->getEdit());
        phazite::test::dispatchPendingMessages();

        report << run << ": the first blocks ";

        for (size_t i = 0; i < 6; ++i)
            report << juce::String (took[i], 2) << " ";

        const auto longest = std::max_element (took.begin(), took.end());
        report << "ms; the longest " << juce::String (*longest, 2) << " ms (block " << (int) (longest - took.begin()) << "), then "
               << juce::String (*std::max_element (took.begin() + 6, took.end()), 2) << " ms at most after the first six\n";
    }

    setEdit (context, nullptr);
    project = nullptr;
    engine.setNumThreads (1);
    host.prepare (48000.0, 256);
    MESSAGE (report);
}

TEST_CASE ("Device sound: House Groove's intro rendered, its clicks found where the kicks start and end" * doctest::test_suite ("device")
           * doctest::skip())
{
    // The owner hears about ten crackles in the intro (8 bars of the kick on each beat and hats between, 2026-11-03), with
    // no block late: what plays is looked through for samples that jump from the line of the two before by far more than
    // the sound around them does (its second difference against the median of its neighbourhood's), each told by how far
    // after a kick's start it is (a kick starts each beat, 0.484 s at 124 BPM, and its note ends a quarter beat later).
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (
        phazite::test::getEngine(), temp.folder, "houseGroove", error,
        phazite::getExampleSongContent (phazite::test::getEngineContext(), "houseGroove", phazite::test::getFactoryPresetsFolder()));
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &project->getEdit());
    phazite::test::settle();

    constexpr double rate = 48000.0, beat = 60.0 / 124.0;
    const auto audio = phazite::renderWithOwnEngine (project->getEdit(), (int) (8 * 4 * beat * rate), rate, 480);
    const auto* left = audio.getReadPointer (0);
    const auto n = audio.getNumSamples();
    std::vector<float> d2 ((size_t) n, 0.0f);

    for (int i = 2; i < n; ++i)
        d2[(size_t) i] = std::abs (left[i] - 2.0f * left[i - 1] + left[i - 2]);

    struct Click
    {
        int sample;
        float size, ratio;
    };

    std::vector<Click> clicks;
    constexpr int around = 240; // 5 ms each way

    for (int i = around; i < n - around; ++i)
    {
        if (d2[(size_t) i] < 0.01f)
            continue;

        std::vector<float> neighbourhood (d2.begin() + i - around, d2.begin() + i + around);
        std::nth_element (neighbourhood.begin(), neighbourhood.begin() + around, neighbourhood.end());
        const auto median = std::max (1.0e-5f, neighbourhood[(size_t) around]);

        if (d2[(size_t) i] > 30.0f * median && (clicks.empty() || i - clicks.back().sample > 480))
            clicks.push_back ({ i, d2[(size_t) i], d2[(size_t) i] / median });
    }

    phazite::engine::LoudnessMeter meter;
    meter.prepare (rate, n);
    meter.process (audio.getArrayOfReadPointers(), 2, n);
    juce::String report;
    report << "\nPeak " << juce::Decibels::toString (juce::Decibels::gainToDecibels (audio.getMagnitude (0, n))) << ", true peak "
           << juce::String (meter.getReadings().truePeak.value_or (-100.0f), 2) << " dBTP; " << (int) clicks.size()
           << " jumps far over their neighbourhood's:\n";

    for (const auto& click : clicks)
    {
        const auto seconds = click.sample / rate;
        const auto intoBeat = std::fmod (seconds, beat);
        report << "  " << juce::String (seconds, 3) << " s (" << juce::String (intoBeat * 1000.0, 1) << " ms after a beat), "
               << juce::String (click.size, 4) << ", " << juce::String (click.ratio, 0) << " times its neighbourhood's\n";
    }

    setEdit (context, nullptr);
    MESSAGE (report);
}

TEST_CASE ("Device sound: House Groove's intro played live sounds the same when the engine is given the song again as it plays"
           * doctest::test_suite ("device") * doctest::skip())
{
    // What plays live, through the app's engine a block at a time: once as it is, and once given a new plan of the same song
    // every half second, as a change of the song gives it one; any difference is where a new plan changed the sound.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (
        phazite::test::getEngine(), temp.folder, "houseGroove", error,
        phazite::getExampleSongContent (phazite::test::getEngineContext(), "houseGroove", phazite::test::getFactoryPresetsFolder()));
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &project->getEdit());
    phazite::test::settle();
    auto* session = phazite::findSession (project->getEdit());
    REQUIRE (session != nullptr);

    const auto playIntro = [&] (bool newPlans)
    {
        phazite::test::EnginePlayback playback;
        phazite::playOrStop (project->getEdit());
        juce::AudioBuffer<float> audio (2, 0);

        for (int part = 0; part < 30; ++part)
        {
            if (newPlans)
                session->update();

            const auto next = playback.play (0.5);
            audio.setSize (2, audio.getNumSamples() + next.getNumSamples(), true);

            for (int channel = 0; channel < 2; ++channel)
                audio.copyFrom (channel, audio.getNumSamples() - next.getNumSamples(), next, channel, 0, next.getNumSamples());
        }

        phazite::playOrStop (project->getEdit());
        phazite::test::dispatchPendingMessages();
        return audio;
    };

    const auto asItIs = playIntro (false), givenAgain = playIntro (true);
    REQUIRE (asItIs.getNumSamples() == givenAgain.getNumSamples());
    auto largest = 0.0f;
    auto at = 0;

    for (int i = 0; i < asItIs.getNumSamples(); ++i)
        if (const auto difference = std::abs (asItIs.getSample (0, i) - givenAgain.getSample (0, i)); difference > largest)
        {
            largest = difference;
            at = i;
        }

    setEdit (context, nullptr);
    MESSAGE ("\nPlayed live as it is, and given the song again every half second: the largest difference "
             << largest << " at " << juce::String (at / phazite::test::EnginePlayback::sampleRate, 3) << " s, the mix's peak "
             << asItIs.getMagnitude (0, asItIs.getNumSamples()));
}

TEST_CASE ("Device sound: where in House Groove's intro the sound above 6 kHz is that a kick shouldn't have"
           * doctest::test_suite ("device") * doctest::skip())
{
    // Between a kick's first 10 ms and the hat half a beat after it there should be next to nothing above 6 kHz: crackle
    // there is distortion. Measured in an export (PHAZITE_GLITCH_FILE), and in renders with the master's multiband and the
    // drum bus's compressor each switched off, to tell which device makes it.
    constexpr double beat = 60.0 / 124.0;

    const auto measure = [] (const juce::AudioBuffer<float>& audio, double rate)
    {
        juce::IIRFilter first, second;
        first.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 6000.0));
        second.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 6000.0));
        std::vector<float> high (audio.getReadPointer (0), audio.getReadPointer (0) + audio.getNumSamples());
        first.processSamples (high.data(), (int) high.size());
        second.processSamples (high.data(), (int) high.size());

        std::vector<double> levels;

        for (int k = 0; k < 32; ++k)
        {
            const auto from = (int) ((k * beat + 0.01) * rate), to = (int) ((k * beat + 0.2) * rate);
            auto peak = 0.0f;

            for (int i = from; i < to && i < (int) high.size(); ++i)
                peak = std::max (peak, std::abs (high[(size_t) i]));

            levels.push_back (juce::Decibels::gainToDecibels (peak, -120.0f));
        }

        auto sorted = levels;
        std::sort (sorted.begin(), sorted.end());
        juce::String text;
        text << "above 6 kHz in the kicks' bodies: median " << juce::String (sorted[16], 1) << " dB, loudest "
             << juce::String (sorted.back(), 1) << " dB; "
             << (int) std::count_if (levels.begin(), levels.end(), [] (double level) { return level > -50.0; })
             << " of 32 kicks over -50 dB:";

        for (int k = 0; k < 32; ++k)
            text << " " << juce::String (levels[(size_t) k], 0);

        return text;
    };

    juce::String report ("\n");

    if (const auto path = getVariable ("PHAZITE_GLITCH_FILE", {}); path.isNotEmpty())
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (path)));
        REQUIRE (reader != nullptr);
        juce::AudioBuffer<float> audio (2, (int) (8 * 4 * beat * reader->sampleRate));
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
        report << "The export (" << juce::String (reader->sampleRate) << " Hz, peak "
               << juce::Decibels::toString (juce::Decibels::gainToDecibels (audio.getMagnitude (0, audio.getNumSamples())))
               << "): " << measure (audio, reader->sampleRate) << "\n";
    }

    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (
        phazite::test::getEngine(), temp.folder, "houseGroove", error,
        phazite::getExampleSongContent (phazite::test::getEngineContext(), "houseGroove", phazite::test::getFactoryPresetsFolder()));
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &project->getEdit());
    phazite::test::settle();

    phazite::TrackID drums;

    for (const auto id : phazite::getAllPlaylistTracks (context))
        if (const auto row = phazite::getTrackRow (context, id); row.has_value() && row->name == "Drums")
            drums = id;

    const auto master = phazite::getEffects (context, {});
    const auto bus = phazite::getEffects (context, drums);
    REQUIRE (! master.empty());
    REQUIRE (! bus.empty());

    const auto render = [&] (const juce::String& what)
    {
        constexpr double rate = 48000.0;
        const auto audio = phazite::renderWithOwnEngine (project->getEdit(), (int) (8 * 4 * beat * rate), rate, 480);
        report << what << " (peak "
               << juce::Decibels::toString (juce::Decibels::gainToDecibels (audio.getMagnitude (0, audio.getNumSamples())))
               << "): " << measure (audio, rate) << "\n";
    };

    render ("Rendered as it is");
    phazite::setDeviceOn (context, master.front().id, false);
    render ("Without the master's " + master.front().name);
    phazite::setDeviceOn (context, master.front().id, true);
    phazite::setDeviceOn (context, bus.front().id, false);
    render ("Without the drum bus's " + bus.front().name);
    phazite::setDeviceOn (context, master.front().id, false);
    render ("Without either");

    setEdit (context, nullptr);
    MESSAGE (report);
}

TEST_CASE ("Device facts: each output's periods, and the buffer a shared stream gets as JUCE opens one" * doctest::test_suite ("device")
           * doctest::skip())
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* outputs = nullptr;
    UINT count = 0;

    if (SUCCEEDED (
            CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &enumerator))
        && SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &outputs)))
        outputs->GetCount (&count);

    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* device = nullptr;
        IPropertyStore* properties = nullptr;
        IAudioClient* client = nullptr;
        WAVEFORMATEX* format = nullptr;
        PROPVARIANT value;
        PropVariantInit (&value);
        juce::String line;

        if (SUCCEEDED (outputs->Item (i, &device)) && SUCCEEDED (device->OpenPropertyStore (STGM_READ, &properties))
            && SUCCEEDED (properties->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR)
            line << juce::String (value.pwszVal) << ": ";

        REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;

        if (device != nullptr && SUCCEEDED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &client))
            && SUCCEEDED (client->GetMixFormat (&format)) && SUCCEEDED (client->GetDevicePeriod (&defaultPeriod, &minimumPeriod)))
        {
            line << (int) format->nSamplesPerSec << " Hz, periods " << defaultPeriod / 10000.0 << " ms (" << minimumPeriod / 10000.0
                 << " ms at least)";

            // As JUCE's shared mode opens it: event driven, a buffer asked for as long as the default period.
            const auto hr =
                client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, defaultPeriod, 0, format, nullptr);
            UINT32 frames = 0;
            REFERENCE_TIME latency = 0;

            if (SUCCEEDED (hr) && SUCCEEDED (client->GetBufferSize (&frames)) && SUCCEEDED (client->GetStreamLatency (&latency)))
                line << ", shared buffer " << (int) frames << " frames (" << frames * 1000.0 / format->nSamplesPerSec
                     << " ms), stream latency " << latency / 10000.0 << " ms";
            else
                line << ", no shared stream (0x" << juce::String::toHexString ((int) hr) << ")";
        }

        MESSAGE (line);
        PropVariantClear (&value);

        if (format != nullptr)
            CoTaskMemFree (format);

        for (IUnknown* each : std::initializer_list<IUnknown*> { client, properties, device })
            if (each != nullptr)
                each->Release();
    }

    for (IUnknown* each : std::initializer_list<IUnknown*> { outputs, enumerator })
        if (each != nullptr)
            each->Release();

    CoUninitialize();
}
#endif
