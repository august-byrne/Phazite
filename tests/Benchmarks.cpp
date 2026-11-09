// SPDX-License-Identifier: AGPL-3.0-or-later
// Benchmarks (docs/performance-audit.md): what each device costs and how soon it sounds, how soon a live note sounds
// through Phazite's engine, what a project costs stopped and playing, how long saving, opening and the UI's work take, and what
// scrolling and zooming a big song cost.
// ctest and CI skip them; run them from an optimised build (windows-profile or windows-release) with
//   phazite_tests.exe --test-suite=benchmark --no-skip
// and record the numbers in docs/performance-audit.md. Times depend on the machine: compare runs on the same one.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "HostedDevice.h"
#include "SandboxedPlugins.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "app/ClipWaveforms.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/fm/FmSynth.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/stretch/Stretch.h"
#include "devices/synth/Synth.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/audio/AudioRegion.h"
#include "host/ThreadCpu.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/PreviewLoop.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <thread>
#include <vector>

namespace
{
namespace dev = phazite::devices;
using phazite::test::HostedDevice;

constexpr double benchRate = HostedDevice::sampleRate;
constexpr int benchBlock = HostedDevice::blockSize;

double millisecondsOf (const std::function<void()>& f)
{
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

// One of Phazite's devices at the end of a channel's devices (an instrument in place of a track's instrument); its ID.
phazite::DeviceID addDevice (phazite::Song& song, phazite::TrackID channel, const char* typeID)
{
    const auto id = phazite::insertDeviceState (song, channel, phazite::makeOwnDeviceState (song, typeID));
    REQUIRE (id.isValid());
    return id;
}

//==============================================================================
// One device, as a benchmark plays it.
struct DeviceCase
{
    // Every member has a default, so that the cases can leave any of them out (GCC's -Wmissing-field-initializers).
    juce::String name {};
    std::function<std::unique_ptr<dev::Processor>()> make {};
    const dev::DeviceInfo* info = nullptr;
    int notes = 0;                                    // for an instrument: held from the start; drums hit again every 12 blocks
    std::function<void (std::vector<float>&)> set {}; // changes to the defaults
    bool silentInput = false;                         // for an effect: silence instead of noise
};

template <typename Device>
std::function<std::unique_ptr<dev::Processor>()> maker()
{
    return [] { return std::make_unique<Device>(); };
}

std::vector<float> defaultsOf (const dev::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

bool isInstrument (const DeviceCase& c)
{
    return c.info->kind == dev::DeviceKind::instrument;
}

bool isDrums (const DeviceCase& c)
{
    return c.info == &dev::DrumSampler::info;
}

// process()'s time over 20 s of stereo audio, as a percentage of the audio's length (100 % = one core can't keep up).
double percentOfCore (const DeviceCase& c)
{
    auto processor = c.make();
    processor->prepare (benchRate, benchBlock);
    auto values = defaultsOf (*c.info);

    if (c.set)
        c.set (values);

    juce::AudioBuffer<float> buffer (2, benchBlock), noise (2, benchBlock);
    juce::Random random (7);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < benchBlock; ++i)
            noise.setSample (channel, i, (random.nextFloat() * 2.0f - 1.0f) * 0.25f);

    std::vector<dev::NoteEvent> notes;

    for (int n = 0; n < c.notes; ++n)
        notes.push_back ({ .type = dev::NoteEvent::Type::noteOn, .sample = 0, .number = isDrums (c) ? 36 + n : 48 + n * 3, .value = 0.8f });

    const int numBlocks = juce::roundToInt (20.0 * benchRate / benchBlock);
    const int warmUp = 20;
    std::chrono::steady_clock::time_point start;

    for (int b = 0; b < warmUp + numBlocks; ++b)
    {
        if (b == warmUp)
            start = std::chrono::steady_clock::now();

        if (isInstrument (c) || c.silentInput)
            buffer.clear();
        else
            for (int channel = 0; channel < 2; ++channel)
                buffer.copyFrom (channel, 0, noise, channel, 0, benchBlock);

        const bool hit = isInstrument (c) && (b == 0 || (isDrums (c) && b % 12 == 0));
        const dev::ProcessContext context { .channels = buffer.getArrayOfWritePointers(),
                                            .numChannels = 2,
                                            .numSamples = benchBlock,
                                            .events = hit ? std::span<const dev::NoteEvent> (notes) : std::span<const dev::NoteEvent>(),
                                            .parameters = values };
        processor->process (context);
    }

    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    return 100.0 * seconds / (numBlocks * benchBlock / benchRate);
}

// How long an instrument's note takes to be heard: the first sample above -60 dBFS, and above -30 dBFS, in ms.
std::pair<double, double> timeToSound (const DeviceCase& c)
{
    auto processor = c.make();
    processor->prepare (benchRate, benchBlock);
    const auto values = defaultsOf (*c.info);
    juce::AudioBuffer<float> buffer (2, benchBlock);
    const dev::NoteEvent note { .type = dev::NoteEvent::Type::noteOn, .sample = 0, .number = isDrums (c) ? 36 : 60, .value = 0.8f };
    double quiet = -1.0, loud = -1.0;

    for (int b = 0; b < 20 && loud < 0.0; ++b)
    {
        buffer.clear();
        const dev::ProcessContext context { .channels = buffer.getArrayOfWritePointers(),
                                            .numChannels = 2,
                                            .numSamples = benchBlock,
                                            .events =
                                                b == 0 ? std::span<const dev::NoteEvent> (&note, 1) : std::span<const dev::NoteEvent>(),
                                            .parameters = values };
        processor->process (context);

        for (int i = 0; i < benchBlock && loud < 0.0; ++i)
        {
            const auto level = std::max (std::abs (buffer.getSample (0, i)), std::abs (buffer.getSample (1, i)));
            const auto ms = (b * benchBlock + i) * 1000.0 / benchRate;

            if (quiet < 0.0 && level > 0.001f)
                quiet = ms;

            if (level > 0.0316f)
                loud = ms;
        }
    }

    return { quiet, loud };
}

//==============================================================================

juce::String milliseconds (double ms, int decimals = 2)
{
    return juce::String (ms, decimals) + " ms";
}

// Paints part of a component into a software image, as the screen would: the app draws with Direct2D, which fills pixels
// differently but walks the same notes and clips. (createComponentSnapshot() makes a Direct2D image on Windows, which costs
// ~170 ms by itself.) Returns the time it took.
double paintTime (juce::Component& c, juce::Rectangle<int> area)
{
    juce::Image image (juce::Image::ARGB, juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), true,
                       juce::SoftwareImageType());
    juce::Graphics g (image);
    g.setOrigin (-area.getPosition());
    g.reduceClipRegion (area);
    return millisecondsOf ([&] { c.paintEntireComponent (g, false); });
}
} // namespace

//==============================================================================
TEST_CASE ("Benchmark: each device's CPU, latency and time to sound" * doctest::test_suite ("benchmark") * doctest::skip())
{
    const auto maxOf = [] (const dev::DeviceInfo& info, int index) { return info.parameters[(size_t) index].maximum; };

    const std::vector<DeviceCase> cases {
        { "Synth, 1 note", maker<dev::Synth>(), &dev::Synth::info, 1 },
        { "Synth, 8 notes", maker<dev::Synth>(), &dev::Synth::info, 8 },
        { "Synth, 16 notes, 7-voice unison", maker<dev::Synth>(), &dev::Synth::info, 16,
          [&] (std::vector<float>& v) { v[dev::Synth::unisonVoices] = maxOf (dev::Synth::info, dev::Synth::unisonVoices); } },
        { "Synth, idle", maker<dev::Synth>(), &dev::Synth::info, 0 },
        { "Wavetable, 1 note", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 1 },
        { "Wavetable, 8 notes", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 8 },
        { "Wavetable, 8 notes, full unison", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 8, [&] (std::vector<float>& v)
          { v[dev::WavetableSynth::unisonVoices] = maxOf (dev::WavetableSynth::info, dev::WavetableSynth::unisonVoices); } },
        { "Wavetable, idle", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 0 },
        { "Drum Sampler, 4 pads every 16th", maker<dev::DrumSampler>(), &dev::DrumSampler::info, 4 },
        { "Drum Sampler, idle", maker<dev::DrumSampler>(), &dev::DrumSampler::info, 0 },
        { "Sampler, 1 note, looping", maker<dev::Sampler>(), &dev::Sampler::info, 1,
          [] (std::vector<float>& v) { v[dev::Sampler::loopOn] = 1.0f; } },
        { "Sampler, 8 notes, looping", maker<dev::Sampler>(), &dev::Sampler::info, 8,
          [] (std::vector<float>& v) { v[dev::Sampler::loopOn] = 1.0f; } },
        { "Sampler, 16 notes, looping, filtered", maker<dev::Sampler>(), &dev::Sampler::info, 16,
          [] (std::vector<float>& v)
          {
              v[dev::Sampler::loopOn] = 1.0f;
              v[dev::Sampler::filterOn] = 1.0f;
          } },
        { "Sampler, idle", maker<dev::Sampler>(), &dev::Sampler::info, 0 },
        { "FM, 1 note, held", maker<dev::FmSynth>(), &dev::FmSynth::info, 1,
          [] (std::vector<float>& v)
          {
              for (int op = 0; op < dev::FmSynth::numOperators; ++op)
                  v[(size_t) dev::FmSynth::getParameterIndex (op, dev::FmSynth::sustain)] = 1.0f;
          } },
        { "FM, 8 notes, held", maker<dev::FmSynth>(), &dev::FmSynth::info, 8,
          [] (std::vector<float>& v)
          {
              for (int op = 0; op < dev::FmSynth::numOperators; ++op)
                  v[(size_t) dev::FmSynth::getParameterIndex (op, dev::FmSynth::sustain)] = 1.0f;
          } },
        { "FM, 8 notes, held, spread", maker<dev::FmSynth>(), &dev::FmSynth::info, 8,
          [] (std::vector<float>& v)
          {
              for (int op = 0; op < dev::FmSynth::numOperators; ++op)
                  v[(size_t) dev::FmSynth::getParameterIndex (op, dev::FmSynth::sustain)] = 1.0f;

              v[dev::FmSynth::spread] = 12.0f;
          } },
        { "FM, 16 notes, held, filtered", maker<dev::FmSynth>(), &dev::FmSynth::info, 16,
          [] (std::vector<float>& v)
          {
              for (int op = 0; op < dev::FmSynth::numOperators; ++op)
                  v[(size_t) dev::FmSynth::getParameterIndex (op, dev::FmSynth::sustain)] = 1.0f;

              v[dev::FmSynth::filterOn] = 1.0f;
          } },
        { "FM, idle", maker<dev::FmSynth>(), &dev::FmSynth::info, 0 },
        { "EQ", maker<dev::Equalizer>(), &dev::Equalizer::info },
        { "Filter", maker<dev::FilterEffect>(), &dev::FilterEffect::info },
        { "Compressor", maker<dev::Compressor>(), &dev::Compressor::info },
        { "Multiband", maker<dev::Multiband>(), &dev::Multiband::info },
        { "Limiter", maker<dev::Limiter>(), &dev::Limiter::info },
        { "Saturator", maker<dev::Saturator>(), &dev::Saturator::info },
        { "Delay", maker<dev::Delay>(), &dev::Delay::info },
        { "Reverb", maker<dev::Reverb>(), &dev::Reverb::info },
        { "Stereo Expander, spreading", maker<dev::StereoExpander>(), &dev::StereoExpander::info, 0,
          [] (std::vector<float>& v) { v[dev::StereoExpander::spread] = 1.0f; } },
        { "Phaser, six notches", maker<dev::Phaser>(), &dev::Phaser::info, 0,
          [] (std::vector<float>& v) { v[dev::Phaser::notches] = 5.0f; } },
        { "Flanger", maker<dev::Flanger>(), &dev::Flanger::info },
        { "Chorus, Ensemble", maker<dev::Chorus>(), &dev::Chorus::info, 0, [] (std::vector<float>& v) { v[dev::Chorus::mode] = 1.0f; } },
        { "Vocoder, 40 bands", maker<dev::Vocoder>(), &dev::Vocoder::info, 0,
          [] (std::vector<float>& v) { v[dev::Vocoder::bands] = 5.0f; } },
        { "Reverb, silence in", maker<dev::Reverb>(), &dev::Reverb::info, 0, {}, true },
        { "Multiband, silence in", maker<dev::Multiband>(), &dev::Multiband::info, 0, {}, true },
    };

    // Every one of Phazite's devices is here.
    for (const auto* info : phazite::getOwnDevices())
    {
        INFO (std::string (info->name));
        CHECK (std::any_of (cases.begin(), cases.end(), [info] (const DeviceCase& c) { return c.info == info; }));
    }

    juce::String report ("\n| Device | CPU (% of a core) | Latency | Heard after (-60 / -30 dB) |\n|---|---|---|---|\n");

    for (const auto& c : cases)
    {
        auto processor = c.make();
        processor->prepare (benchRate, benchBlock);
        const auto latency = processor->getLatencySamples();
        report << "| " << c.name << " | " << juce::String (percentOfCore (c), 3) << " | "
               << (latency == 0 ? juce::String ("0")
                                : juce::String (latency) + " samples (" + milliseconds (latency * 1000.0 / benchRate) + ")")
               << " | ";

        if (isInstrument (c) && c.notes == 1)
        {
            const auto [quiet, loud] = timeToSound (c);
            report << milliseconds (quiet) << " / " << milliseconds (loud);
        }

        report << " |\n";
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: a sound made ready for the sampler as it loads, a minute of it in stereo" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // Noise with a hit every half second, at 48 kHz: where its hits begin found, then its octaves made.
    dev::Sampler::Sound sound;
    sound.audio.setSize (2, (int) (60.0 * benchRate));
    juce::Random random (7);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < sound.audio.getNumSamples(); ++i)
            sound.audio.setSample (channel, i, (random.nextFloat() - 0.5f) * (i % 24000 < 2400 ? 1.0f : 0.05f));

    const auto hits = millisecondsOf ([&] { sound.onsets = dev::detectOnsets (sound.audio, benchRate); });
    const auto octaves = millisecondsOf ([&] { dev::Sampler::makeOctaves (sound); });
    CHECK (sound.onsets.size() > 1);
    MESSAGE ("Its hits found: " << milliseconds (hits, 1) << "; its octaves made: " << milliseconds (octaves, 1));
}

TEST_CASE ("Benchmark: a live note's way through Phazite's engine" * doctest::test_suite ("benchmark") * doctest::skip())
{
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    HostedDevice device;

    // A project, as the app plays it, through Phazite's engine.
    auto song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::setSong (context, song.get());
    const auto tracks = phazite::ensureTracks (*song, 2);
    addDevice (*song, tracks[0], dev::Synth::typeID);
    phazite::routeLiveMidiInput (*song);
    REQUIRE (phazite::getLiveMidiTrack (*song) == tracks[0]);
    phazite::test::settle (500);

    // Silence first, then a note played between two blocks, as the computer keyboard plays one.
    juce::String report ("\n");
    const auto playNote = [&] (const juce::String& what)
    {
        device.process (50);
        audio.playLive (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        const auto samples = device.samplesToFirstSound();
        audio.playLive (juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        report << what << ": heard "
               << (samples < 0 ? juce::String ("never")
                               : juce::String (samples) + " samples (" + milliseconds (samples * 1000.0 / benchRate) + ")")
               << " after the next block starts\n";
    };

    playNote ("A synth on a track");

    // A limiter on another track: delay compensation holds back the track played too.
    const auto limiter = addDevice (*song, tracks[1], dev::Limiter::typeID);
    phazite::test::settle (500);
    playNote ("With a limiter on another track");

    // Switched off, the limiter delays nothing.
    phazite::setDeviceOn (*song, limiter, false);
    phazite::test::settle (500);
    playNote ("With that limiter switched off");
    phazite::setDeviceOn (*song, limiter, true);

    // A limiter on the master too.
    addDevice (*song, phazite::getMasterID (*song), dev::Limiter::typeID);
    phazite::test::settle (500);
    playNote ("With a limiter on the master too");

    MESSAGE (report);
    phazite::setSong (context, nullptr);
}

TEST_CASE ("Benchmark: a project's CPU, stopped and playing" * doctest::test_suite ("benchmark") * doctest::skip())
{
    HostedDevice device;

    enum class Kind
    {
        empty,
        synths,
        drums
    };

    const auto makeSong = [] (int numTracks, Kind kind)
    {
        auto song = phazite::createSong();

        for (const auto track : phazite::ensureTracks (*song, numTracks))
        {
            if (kind == Kind::empty)
                continue;

            // A drum sampler with all its pads' parameters: a beat of a kick, a snare and sixteenth hats, the hats choking.
            if (kind == Kind::drums)
            {
                addDevice (*song, track, dev::DrumSampler::typeID);
                const auto clip =
                    phazite::test::addMidiClip (*song, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (32.0) }, "Beat");

                for (int sixteenth = 0; sixteenth < 128; ++sixteenth)
                {
                    const auto at = phazite::BeatPosition::fromBeats (sixteenth * 0.25);
                    const auto length = phazite::BeatDuration::fromBeats (0.2);
                    phazite::test::addMidiNote (*song, clip, sixteenth % 8 == 6 ? 46 : 42, at, length, 90);

                    if (sixteenth % 4 == 0)
                        phazite::test::addMidiNote (*song, clip, sixteenth % 8 == 0 ? 36 : 38, at, length, 110);
                }

                continue;
            }

            for (const auto* type : { dev::Synth::typeID, dev::Equalizer::typeID, dev::Compressor::typeID, dev::Reverb::typeID })
                addDevice (*song, track, type);

            // A chord on every beat for 32 beats.
            const auto clip =
                phazite::test::addMidiClip (*song, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (32.0) }, "Chords");

            for (int beat = 0; beat < 32; ++beat)
                for (int note : { 48, 52, 55, 59 })
                    phazite::test::addMidiNote (*song, clip, note, phazite::BeatPosition::fromBeats (beat),
                                                phazite::BeatDuration::fromBeats (0.9), 100);
        }

        return song;
    };

    // Each a project, as the app plays it through Phazite's engine, on one thread and then on four.
    auto& playing = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    const auto threads = playing.getNumThreads();
    juce::String report (
        "\n| Project | Threads | Stopped | Playing | The process's CPU playing, a block each block's length (% of a core) |\n"
        "|---|---|---|---|---|\n");

    // The whole process's CPU while blocks come as an audio device brings them, one each block's length: what threads
    // that wait for the next block cost meanwhile.
    const auto processCpuInRealTime = [&device] (int count)
    {
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration> (
            std::chrono::duration<double> (HostedDevice::blockSize / HostedDevice::sampleRate));
        auto next = std::chrono::steady_clock::now();
        const auto start = next;
        const auto cpuBefore = phazite::test::getProcessCpuSeconds();

        for (int block = 0; block < count; ++block)
        {
            next += period;
            device.process (1, {}, 0);
            std::this_thread::sleep_until (next);
        }

        const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        return 100.0 * (phazite::test::getProcessCpuSeconds() - cpuBefore) / seconds;
    };

    for (const auto& [numTracks, kind] : { std::pair (20, Kind::empty), std::pair (20, Kind::synths), std::pair (20, Kind::drums) })
    {
        for (const auto numThreads : { 1, 4 })
        {
            playing.setNumThreads (numThreads); // while no block plays
            auto song = makeSong (numTracks, kind);
            phazite::ProjectContext context { phazite::test::getEngineContext() };
            phazite::setSong (context, song.get());
            phazite::routeLiveMidiInput (*song);
            phazite::test::settle (800);
            const auto stopped = device.percentOfRealTime (500);
            phazite::playOrStop (*song);
            phazite::test::settle (500);
            const auto played = device.percentOfRealTime (500);
            const auto processCpu = processCpuInRealTime (400);
            phazite::stopAndReturn (*song);
            phazite::test::settle (300);
            report << "| " << numTracks
                   << (kind == Kind::synths  ? " tracks, each a synth, an EQ, a compressor and a reverb, chords"
                       : kind == Kind::drums ? " tracks, each a drum sampler, a beat"
                                             : " empty tracks")
                   << " | " << numThreads << " | " << juce::String (stopped, 2) << " % | " << juce::String (played, 2) << " % | "
                   << juce::String (processCpu, 1) << " % |\n";
            phazite::setSong (context, nullptr);
            song = nullptr;
            phazite::test::settle (300);
        }
    }

    // 20 tracks, each a plug-in synth in a helper (the tests' stand-in, all its instances in one helper, each with an audio
    // thread of its own), chords: what the round trips to helpers cost a project.
    {
        phazite::test::SandboxedPlugins sandboxed;

        for (const auto numThreads : { 1, 4 })
        {
            playing.setNumThreads (numThreads);
            phazite::test::TestSong song;

            for (int i = 0; i < 20; ++i)
            {
                const auto track = i == 0 ? song.firstTrack : song.addTrack ("Synth " + juce::String (i + 1));
                song.setInstrument (track, "PhaziteTestSynth");
                const auto clip = song.addMidiClip (track, 0.0, 32.0);

                for (int beat = 0; beat < 32; ++beat)
                    song.addChord (clip, { 48, 52, 55, 59 }, beat, 0.9);
            }

            phazite::test::settle (800);
            const auto stopped = device.percentOfRealTime (500);
            song.run ("transport.playStop");
            phazite::test::settle (500);
            const auto played = device.percentOfRealTime (500);
            const auto processCpu = processCpuInRealTime (400);
            song.run ("transport.playStop");
            phazite::test::settle (300);
            report << "| 20 tracks, each a plug-in synth in a helper (the tests' stand-in), chords | " << numThreads << " | "
                   << juce::String (stopped, 2) << " % | " << juce::String (played, 2) << " % | " << juce::String (processCpu, 1)
                   << " % |\n";
        }
    }

    playing.setNumThreads (threads);

    MESSAGE (report);
}

TEST_CASE ("Benchmark: saving and opening a big project" * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Big", error);
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    const auto tracks = phazite::ensureTracks (song, 40);

    // 40 tracks, each a synth, an EQ and a compressor, and 16 bars of 16th notes: 10,240 notes.
    for (const auto track : tracks)
    {
        for (const auto* type : { dev::Synth::typeID, dev::Equalizer::typeID, dev::Compressor::typeID })
            addDevice (song, track, type);

        const auto clip =
            phazite::test::addMidiClip (song, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (64.0) }, "Notes");

        for (int step = 0; step < 256; ++step)
            phazite::test::addMidiNote (song, clip, 48 + step % 24, phazite::BeatPosition::fromBeats (step * 0.25),
                                        phazite::BeatDuration::fromBeats (0.2), 100);
    }

    phazite::test::settle (500);
    juce::String report ("\n");
    report << "Save (40 tracks, 10,240 notes): " << milliseconds (millisecondsOf ([&] { REQUIRE (project->save().wasOk()); }), 1);
    report << " (file " << juce::String (project->getFile().getSize() / 1024) << " KB)\n";
    phazite::test::settle (300);
    report << "Save again: " << milliseconds (millisecondsOf ([&] { REQUIRE (project->save().wasOk()); }), 1) << "\n";

    phazite::renameTrack (song, tracks[0], "Changed");
    phazite::test::settle (100);
    report << "Autosave: " << milliseconds (millisecondsOf ([&] { REQUIRE (project->autosave().wasOk()); }), 1) << "\n";

    const auto file = project->getFile();
    REQUIRE (project->save().wasOk());
    project = nullptr;
    phazite::test::settle (300);
    std::unique_ptr<phazite::Project> reopened;
    report << "Open: " << milliseconds (millisecondsOf ([&] { reopened = phazite::Project::open (file, false, error); }), 1) << "\n";
    REQUIRE (reopened != nullptr);
    MESSAGE (report);
}

TEST_CASE ("Benchmark: rendering audio clips stretched and pitched" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // What a clip that follows the tempo, or is pitched, waits for before it sounds (roadmap step 2.18): Rubber Band's R3
    // engine over its whole file (src/devices/stretch/Stretch.h), here stereo noise at 44.1 kHz with a hit on every eighth
    // note at 120 BPM.
    juce::String report ("\n");
    juce::Random random (5);

    for (const auto seconds : { 8.0, 60.0 })
    {
        juce::AudioBuffer<float> audio (2, (int) (seconds * 44100.0));

        for (int i = 0; i < audio.getNumSamples(); ++i)
            for (int channel = 0; channel < 2; ++channel)
                audio.setSample (channel, i, 0.3f * std::exp (-(float) (i % 11025) / 2000.0f) * (random.nextFloat() * 2.0f - 1.0f));

        const phazite::stretch::Source source { 2, 44100.0, audio.getNumSamples(),
                                                [&audio] (juce::AudioBuffer<float>& buffer, juce::int64 start, int frames)
                                                {
                                                    buffer.clear (0, frames);
                                                    const auto count = (int) std::min ((juce::int64) frames, audio.getNumSamples() - start);

                                                    for (int channel = 0; channel < 2 && count > 0; ++channel)
                                                        buffer.copyFrom (channel, 0, audio, channel, (int) start, count);
                                                } };
        const std::pair<const char*, phazite::stretch::Settings> cases[] = { { "stretched from 120 to 100 BPM", { 1.2, 0.0, false } },
                                                                             { "pitched up 3 semitones", { 1.0, 3.0, false } } };

        for (const auto& [name, settings] : cases)
        {
            const auto ms = millisecondsOf (
                [&] { REQUIRE (phazite::stretch::render (source, settings, [] (const juce::AudioBuffer<float>&, int) { return true; })); });
            report << juce::String (seconds, 0) << " s in stereo, " << name << ": " << milliseconds (ms, 0) << " ("
                   << juce::String (seconds * 1000.0 / ms, 1) << " times as fast as it plays)\n";
        }
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: audio clips of files at another rate than the engine's" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // What a clip costs as it plays (docs/dsp-audit.md, 3.1): 64 clips of a stereo file, in memory, on an engine at 48 kHz,
    // played for 10 s in blocks of 512: a file at the engine's rate, which plays as it is, and files at 44.1 and 96 kHz.
    namespace engine = phazite::engine;

    struct NoiseReader final : engine::SampleReader
    {
        explicit NoiseReader (double rate)
            : fileRate (rate)
        {
        }

        int getNumChannels() const override { return 2; }
        double getSampleRate() const override { return fileRate; }
        std::int64_t getLength() const override { return (std::int64_t) (12.0 * fileRate); }

        bool read (juce::AudioBuffer<float>& buffer, std::int64_t, int numSamples) override
        {
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < numSamples; ++i)
                    buffer.setSample (channel, i, 0.3f * (random.nextFloat() * 2.0f - 1.0f));

            return true;
        }

        double fileRate;
        juce::Random random { 7 };
    };

    constexpr double engineRate = 48000.0;
    constexpr int blockSize = 512, numClips = 64;
    juce::String report ("\n");

    for (const auto fileRate : { 48000.0, 44100.0, 96000.0 })
    {
        auto file = std::make_shared<engine::AudioFile> (std::make_unique<NoiseReader> (fileRate), nullptr);
        file->readNow (0, file->getLength());
        std::vector<engine::AudioRegion> regions;

        for (int i = 0; i < numClips; ++i)
        {
            engine::AudioRegion region;
            region.file = file;
            region.state = std::make_shared<engine::RegionState> (*file, engineRate);
            region.length = (std::int64_t) (10.0 * engineRate);
            regions.push_back (region);
        }

        juce::AudioBuffer<float> out (2, blockSize), scratch (2, 9 * blockSize + 16);
        const auto ms = millisecondsOf (
            [&]
            {
                for (std::int64_t at = 0; at < (std::int64_t) (10.0 * engineRate); at += blockSize)
                {
                    out.clear();
                    float* channels[] = { out.getWritePointer (0), out.getWritePointer (1) };

                    for (const auto& region : regions)
                        region.play (channels, at, blockSize, engineRate, false, scratch);
                }
            });

        report << numClips << " clips of a file at " << juce::String (fileRate / 1000.0, 1) << " kHz: " << juce::String (ms / 100.0, 2)
               << " % of a core (" << juce::String (ms / 100.0 / numClips, 3) << " % each)\n";
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: a loop previewed in time, until it can start" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // What the browser waits for before a loop it previews sounds (src/model/engine/internal/PreviewLoop.h): its start
    // stretched by Rubber Band, with the render running at least twice as fast as the loop plays. Four bars at 120 BPM of
    // stereo noise at 44.1 kHz, previewed at 100 BPM, played at 48 kHz.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Loop 120 BPM.wav");
    {
        juce::AudioBuffer<float> audio (2, (int) (8.0 * 44100.0));
        juce::Random random (5);

        for (int i = 0; i < audio.getNumSamples(); ++i)
            for (int channel = 0; channel < 2; ++channel)
                audio.setSample (channel, i, 0.3f * std::exp (-(float) (i % 11025) / 2000.0f) * (random.nextFloat() * 2.0f - 1.0f));

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    juce::String report ("\n");

    for (int run = 0; run < 3; ++run)
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        phazite::StretchedLoop loop (file, 1.2, 48000.0);
        REQUIRE (loop.isValid());

        while (! loop.isReadyToPlay())
            juce::Thread::sleep (1);

        const auto ready = juce::Time::getMillisecondCounterHiRes() - started;

        while (! loop.isComplete())
            juce::Thread::sleep (1);

        const auto complete = juce::Time::getMillisecondCounterHiRes() - started;
        report << "Four bars, stretched from 120 to 100 BPM: ready to start in " << milliseconds (ready, 0) << ", all of it in "
               << milliseconds (complete, 0) << "\n";
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: the UI's work on a big song" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // A native image held throughout, as the app's window holds Direct2D's device on Windows for as long as it is open:
    // without one, the first native image painted (the time bar's overview's picture) made the device, about 200 ms, which
    // the whole window's paint then counted (2026-11-07).
    juce::Image keepsNativeImages (juce::Image::ARGB, 1, 1, true);
    {
        juce::Graphics g (keepsNativeImages);
    }

    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    auto* open = phazite::findSong (screen.session.getContext());
    REQUIRE (open != nullptr);
    auto& song = *open;

    // 16 tracks with a synth, each 16 clips of 4 bars with 64 notes: 256 clips and 16,384 notes.
    const auto tracks = phazite::ensureTracks (song, 16);
    phazite::ClipID firstClip;
    phazite::DeviceID synth; // the first synth added

    for (const auto track : tracks)
    {
        if (! phazite::getInstrument (song, track).isValid())
        {
            const auto added = addDevice (song, track, dev::Synth::typeID);

            if (! synth.isValid())
                synth = added;
        }

        for (int c = 0; c < 16; ++c)
        {
            const auto start = phazite::BeatPosition::fromBeats (c * 16.0);
            const auto clip = phazite::test::addMidiClip (song, track, { start, start + phazite::BeatDuration::fromBeats (16.0) }, "Part");

            for (int n = 0; n < 64; ++n)
                phazite::test::addMidiNote (song, clip, 48 + (n * 7) % 24, phazite::BeatPosition::fromBeats (n * 0.25),
                                            phazite::BeatDuration::fromBeats (0.2), 100);

            if (! firstClip.isValid())
                firstClip = clip;
        }
    }

    // The first clip selected, so that the piano roll shows it, with the other tracks' notes as ghosts.
    REQUIRE (firstClip.isValid());
    getSelection (screen.session.getContext()).selectOnly (firstClip);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 1500);
    juce::String report ("\n| The UI's work, 16 tracks, 256 clips, 16,384 notes | Time |\n|---|---|\n");

    // A parameter's change, as a knob's drag makes one at each mouse move, and what the views do with it.
    REQUIRE (synth.isValid());
    auto cutoff = phazite::makeParameterHandle (song, { synth, "cutoff" }, "Change Cutoff");
    REQUIRE (cutoff.exists());
    double total = 0.0;
    const int changes = 20;
    cutoff.beginGesture();

    for (int i = 0; i < changes; ++i)
    {
        phazite::test::dispatchPendingMessages();
        cutoff.setValue (1000.0f + 100.0f * (float) i);
        total += millisecondsOf ([] { phazite::test::dispatchPendingMessages(); });
    }

    cutoff.endGesture();
    report << "| A parameter's change, as the views handle it | " << milliseconds (total / changes) << " |\n";

    const auto bounds = screen.shell.getLocalBounds();
    report << "| The whole window painted | " << milliseconds (paintTime (screen.shell, bounds), 1) << " |\n";

    const std::function<juce::Component*(juce::Component&, const std::function<bool (juce::Component&)>&)> find =
        [&find] (juce::Component& parent, const std::function<bool (juce::Component&)>& is) -> juce::Component*
    {
        for (auto* child : parent.getChildren())
        {
            if (is (*child))
                return child;

            if (auto* found = find (*child, is))
                return found;
        }

        return nullptr;
    };

    const auto columnOf = [] (juce::Component& c)
    {
        double sum = 0.0;

        for (int i = 0; i < 10; ++i)
            sum += paintTime (c, { 200 + i * 20, 0, 11, c.getHeight() });

        return sum / 10;
    };

    report << "| An 11-px column of the window, as a playhead repaints | " << milliseconds (columnOf (screen.shell)) << " |\n";

    if (auto* playlist = find (screen.shell, [] (juce::Component& c) { return dynamic_cast<phazite::ui::Playlist*> (&c) != nullptr; }))
        report << "| ... of the playlist | " << milliseconds (columnOf (*playlist)) << " |\n";

    if (auto* pianoRoll = find (screen.shell, [] (juce::Component& c) { return dynamic_cast<phazite::ui::PianoRoll*> (&c) != nullptr; }))
        report << "| ... of the piano roll | " << milliseconds (columnOf (*pianoRoll)) << " |\n";

    // Saved, then a notification that changes nothing: the comparison with the saved copy goes through everything.
    auto* project = screen.session.getProject();
    REQUIRE (project != nullptr);
    REQUIRE (project->save().wasOk());
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    phazite::findTrackState (song, tracks[5]).sendPropertyChangeMessage (phazite::schema::trackName);
    report << "| The check for unsaved changes after a notification that changes nothing | "
           << milliseconds (millisecondsOf ([&] { project->hasUnsavedChanges(); })) << " |\n";
    report << "| Save | " << milliseconds (millisecondsOf ([&] { REQUIRE (project->save().wasOk()); }), 1) << " |\n";

    // Autosave: what the UI thread does (the devices' states and a copy of the song), the rest on a background thread.
    phazite::renameTrack (song, tracks[3], "Changed");
    phazite::test::dispatchPendingMessages();
    report << "| Autosave, on the UI thread | " << milliseconds (millisecondsOf ([&] { project->autosaveInBackground(); }), 1) << " |\n";
    phazite::test::runTimersUntil ([project] { return ! project->isAutosaving(); }, 10000);

    // A note moved, as a drag moves it a grid step at a time: the views, and the engine given the song again.
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    const auto notes = phazite::getClipNotes (song, firstClip);
    REQUIRE_FALSE (notes.empty());
    const auto note = notes.front();
    const auto contentStart = phazite::getContentStart (phazite::findClipState (song, firstClip));
    double moving = 0.0;

    for (int step = 1; step <= 10; ++step)
        moving += millisecondsOf (
            [&]
            {
                phazite::moveNote (song, note.getRef(), contentStart + phazite::BeatDuration::fromBeats (0.25 * step), note.pitch);
                phazite::test::dispatchMessagesUntil ([] { return false; }, 30); // the session gives the engine the song on the loop
            });

    report << "| A note moved a step, with what follows (the views, the engine's new plan) | " << milliseconds (moving / 10 - 30.0)
           << " |\n";

    MESSAGE (report);
}

TEST_CASE ("Benchmark: frosted glass behind popups over a big song" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // What a tooltip, a menu and the command search cost on their frosted glass (ui/theme/Popups.h): as each opens, what
    // is behind it painted, shrunk, blurred and made into images of the screen's kind; then the glass painted, as a repaint
    // of the popup paints it, into an image of the screen's kind (Direct2D's on Windows). Over the playlist and the piano
    // roll of 16 tracks, each of 16 clips of 4 bars with 64 notes.
    phazite::ui::LookAndFeel lookAndFeel;
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    auto* open = phazite::findSong (screen.session.getContext());
    REQUIRE (open != nullptr);
    auto& song = *open;

    for (const auto track : phazite::ensureTracks (song, 16))
    {
        for (int c = 0; c < 16; ++c)
        {
            const auto start = phazite::BeatPosition::fromBeats (c * 16.0);
            const auto clip = phazite::test::addMidiClip (song, track, { start, start + phazite::BeatDuration::fromBeats (16.0) }, "Part");

            for (int n = 0; n < 64; ++n)
                phazite::test::addMidiNote (song, clip, 48 + (n * 7) % 24, phazite::BeatPosition::fromBeats (n * 0.25),
                                            phazite::BeatDuration::fromBeats (0.2), 100);
        }
    }

    phazite::test::dispatchMessagesUntil ([] { return false; }, 1500);
    const auto scale = juce::Component::getApproximateScaleFactorForComponent (&screen.window);
    juce::String report;
    report << "\n| Frosted glass over the song, at " << juce::String (scale * 100.0f, 0)
           << " % | Taken as it opens | Painted |\n|---|---|---|\n";

    const std::pair<const char*, juce::Point<int>> popups[] = { { "A tooltip", { 220, 30 } },
                                                                { "A menu", { 220, 320 } },
                                                                { "The command search", { 530, 480 } } };

    for (const auto& [what, size] : popups)
    {
        juce::Component popup;
        popup.setLookAndFeel (&lookAndFeel);
        popup.setBounds (juce::Rectangle<int> (size.x, size.y).withCentre (screen.window.getLocalBounds().getCentre()));
        screen.window.addChildComponent (popup);
        auto& glass = phazite::ui::FrostedGlass::putBehind (popup, phazite::ui::FrostedGlass::Shape::rounded, false);
        juce::Image target (juce::Image::ARGB, size.x, size.y, true); // the screen's kind
        double taking = 0.0, painting = 0.0;
        constexpr int times = 10;

        for (int i = 0; i < times; ++i)
        {
            taking += millisecondsOf ([&] { popup.setVisible (true); });
            painting += millisecondsOf (
                [&]
                {
                    juce::Graphics g (target); // made and finished each time, as a repaint's context is
                    glass.paintEntireComponent (g, false);
                });
            popup.setVisible (false);
        }

        report << "| " << what << ", " << size.x << " x " << size.y << " | " << milliseconds (taking / times) << " | "
               << milliseconds (painting / times) << " |\n";
        screen.window.removeChildComponent (&popup);
        popup.setLookAndFeel (nullptr);
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: scrolling and zooming a big song" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // The app's look and feel lives as long as the app, and keeps the mouse pointers (ui/theme/LookAndFeel.h); not set as
    // the default here, so that the paint times compare with those of earlier runs.
    phazite::ui::LookAndFeel lookAndFeel;
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    auto* open = phazite::findSong (screen.session.getContext());
    REQUIRE (open != nullptr);
    auto& song = *open;

    // 100 tracks, each with 24 clips of 4 bars and 16 notes: 2,400 clips and 38,400 notes.
    const auto tracks = phazite::ensureTracks (song, 100);
    phazite::ClipID lastClip;
    int numClips = 0;

    for (const auto track : tracks)
    {
        for (int c = 0; c < 24; ++c)
        {
            const auto start = phazite::BeatPosition::fromBeats (c * 16.0);
            const auto clip = phazite::test::addMidiClip (song, track, { start, start + phazite::BeatDuration::fromBeats (16.0) }, "Part");

            for (int n = 0; n < 16; ++n)
                phazite::test::addMidiNote (song, clip, 48 + (n * 5) % 24, phazite::BeatPosition::fromBeats (n),
                                            phazite::BeatDuration::fromBeats (0.5), 100);

            lastClip = clip;
            ++numClips;
        }
    }

    REQUIRE (lastClip.isValid());
    phazite::test::dispatchMessagesUntil ([] { return false; }, 1500);
    auto* playlist = phazite::test::findAll<phazite::ui::Playlist> (screen.shell).getFirst();
    REQUIRE (playlist != nullptr);

    juce::String report;
    report << "\n| The playlist: " << (int) tracks.size() << " tracks, " << numClips << " clips, " << numClips * 16
           << " notes | The UI thread's work | The playlist painted |\n|---|---|---|\n";

    // Each step: the event, what it posts, and the playlist painted afterwards as the screen would.
    const auto measure = [&] (const juce::String& what, int steps, const std::function<void()>& step)
    {
        double work = 0.0, painting = 0.0;

        for (int i = 0; i < steps; ++i)
        {
            work += millisecondsOf (
                [&]
                {
                    step();
                    phazite::test::dispatchPendingMessages();
                });
            painting += paintTime (*playlist, playlist->getLocalBounds());
        }

        report << "| " << what << " | " << milliseconds (work / steps) << " | " << milliseconds (painting / steps) << " |\n";
    };

    phazite::test::Mouse mouse (*playlist);
    const auto centre = playlist->getLocalBounds().getCentre(); // over the tracks' lanes
    constexpr float notch = 60.0f / 256.0f;                     // a mouse wheel's notch, as JUCE reports it on Windows
    const auto ctrl = juce::ModifierKeys::ctrlModifier, shift = juce::ModifierKeys::shiftModifier;

    auto x = 0;
    measure ("The mouse moved a pixel over the tracks", 40, [&] { mouse.moveTo (centre.translated (++x % 2, 0)); });

    // Half a row apart: from a clip's upper half, which selects a range, to its lower half, which moves it, each with its
    // own pointer.
    auto y = 0;
    measure ("The mouse moved from one half of a clip to the other", 20, [&] { mouse.moveTo (centre.translated (0, ++y % 2 * 32)); });
    measure ("A wheel notch, scrolling down", 40, [&] { mouse.wheel (centre, -notch); });
    measure ("A wheel notch, scrolling up", 40, [&] { mouse.wheel (centre, notch); });
    measure ("Shift and a wheel notch, scrolling in time", 20, [&] { mouse.wheel (centre, -notch, shift); });
    measure ("Ctrl and a wheel notch, zooming in", 10, [&] { mouse.wheel (centre, notch, ctrl); });
    measure ("Ctrl and a wheel notch, zooming out", 10, [&] { mouse.wheel (centre, -notch, ctrl); });

    // A clip moved a beat at a time, as a drag moves it: what the views do with each change.
    auto beat = phazite::getClipRange (song, lastClip).getStart();
    measure ("A clip moved a beat", 20,
             [&]
             {
                 beat = beat + phazite::BeatDuration::fromBeats (1.0);
                 phazite::moveClip (song, lastClip, beat, phazite::getClipTrack (song, lastClip));
             });

    report << "\nClip components in the playlist: " << phazite::test::findAll<phazite::ui::ClipComponent> (*playlist).size() << "\n";
    MESSAGE (report);
}

TEST_CASE ("Benchmark: a song scrolling continuously as it plays, each frame painted as the window paints it"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Each frame painted as the window paints it on Windows: what it asked to repaint, into one native image (Direct2D's, its
    // device held as the window holds it), in the app's look and feel, with its audio clips' waveforms, at the owner's
    // screen size (docs/performance-audit.md, section 16, options 15 and 16).
    juce::Image frameImage (juce::Image::ARGB, 1920, 1080, true);
    {
        juce::Graphics g (frameImage);
    }

    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    phazite::ui::setClipWaveformMaker (phazite::getClipWaveformMaker());
    juce::String report;

    {
        phazite::test::AppScreen screen;
        screen.window.setSize (1920, 1080);
        screen.session.openAtStartup();
        REQUIRE (screen.registry.invoke ("help.example.houseGroove")); // 124 BPM, an automation lane, an audio loop
        phazite::test::dispatchMessagesUntil ([] { return false; }, 2000);
        auto* song = phazite::findSong (screen.session.getContext());
        auto* playlist = phazite::test::findAll<phazite::ui::Playlist> (screen.shell).getFirst();
        auto* lanes = playlist != nullptr ? phazite::test::findComponentWithTitle (*playlist, "Tracks") : nullptr;
        REQUIRE (song != nullptr);
        REQUIRE (lanes != nullptr);

        // As the owner played a song: scrolling continuously, at about 22 pixels a beat, so that the song moves about 0.75 of a
        // pixel a frame at 124 BPM and 60 Hz.
        playlist->setScrollingContinuously (true);
        playlist->zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (lanes->getWidth() / 22.0) });
        phazite::playOrStop (*song);
        REQUIRE (phazite::isSongPlaying (*song));
        auto* session = phazite::findPlayingSession (*song);
        REQUIRE (session != nullptr);

        // What each part takes to paint, as JUCE times it (ComponentListener::componentPainted()), summed over a frame.
        struct PartTimes : juce::ComponentListener
        {
            void componentPainted (juce::Component&, const juce::ComponentPaintDiagnostics& d) override
            {
                total += d.totalPaintDuration.get<juce::Milliseconds>();
                own += d.paintDuration.get<juce::Milliseconds>();
            }

            void reset() { total = own = 0.0; }
            double total = 0.0, own = 0.0;
        };

        PartTimes lanesTimes, automationTimes, barTimes, midiTimes, audioTimes;
        lanes->addComponentListener (&lanesTimes);
        std::vector<juce::Component*> automationLanes, timeBar;

        for (auto* each : phazite::test::findAll<juce::Component> (*lanes))
            if (each->getTitle().startsWith ("Automation of"))
                automationLanes.push_back (each);

        for (const auto* title : { "Timeline", "Sections", "Chords" })
            if (auto* part = phazite::test::findComponentWithTitle (*playlist, title))
                timeBar.push_back (part);

        for (auto* part : automationLanes)
            part->addComponentListener (&automationTimes);

        for (auto* part : timeBar)
            part->addComponentListener (&barTimes);

        struct Frame
        {
            double work, painted, lanes, background, automation, midi, audio, bar;
        };

        std::vector<Frame> frames;
        const auto beatsPerFrame = 124.0 / 60.0 / 60.0;
        auto beat = 64.0, time = juce::Time::getMillisecondCounterHiRes();
        playlist->showFrame (time / 1000.0);
        screen.window.takeRepaintedArea();

        for (int f = 0; f < 600; ++f)
        {
            beat += beatsPerFrame;
            time += 1000.0 / 60.0;
            // Where the song is at this frame: the tests' engine plays no blocks, so the song waits where it is put.
            session->locate (phazite::BeatPosition::fromBeats (beat));
            lanesTimes.reset();
            automationTimes.reset();
            barTimes.reset();
            midiTimes.reset();
            audioTimes.reset();

            // The clips in view, which come and go: each heard once.
            for (auto* clip : phazite::test::findAll<phazite::ui::ClipComponent> (*playlist))
            {
                auto& times = clip->getInfo().kind == phazite::ClipType::audio ? audioTimes : midiTimes;
                clip->removeComponentListener (&times);
                clip->addComponentListener (&times);
            }

            Frame frame {};
            frame.work = millisecondsOf (
                [&]
                {
                    playlist->showFrame (time / 1000.0);
                    phazite::test::dispatchPendingMessages();
                });

            const auto area = screen.window.takeRepaintedArea();
            frame.painted = millisecondsOf (
                [&]
                {
                    juce::Graphics g (frameImage);

                    if (g.reduceClipRegion (area))
                        screen.window.paintEntireComponent (g, false);
                });

            frame.lanes = lanesTimes.total;
            frame.background = lanesTimes.own;
            frame.automation = automationTimes.total;
            frame.midi = midiTimes.total;
            frame.audio = audioTimes.total;
            frame.bar = barTimes.total;
            frames.push_back (frame);
        }

        phazite::playOrStop (*song);
        lanes->removeComponentListener (&lanesTimes);

        for (auto* part : automationLanes)
            part->removeComponentListener (&automationTimes);

        for (auto* part : timeBar)
            part->removeComponentListener (&barTimes);

        const auto clips = phazite::test::findAll<phazite::ui::ClipComponent> (*playlist);
        auto kept = 0;

        for (auto* clip : clips)
        {
            clip->removeComponentListener (&midiTimes);
            clip->removeComponentListener (&audioTimes);
            kept += clip->getCachedComponentImage() != nullptr ? 1 : 0;
        }

        const auto row = [&frames, &report] (const juce::String& what, double Frame::* value)
        {
            std::vector<double> values;

            for (const auto& frame : frames)
                values.push_back (frame.*value);

            std::sort (values.begin(), values.end());
            const auto mean = std::accumulate (values.begin(), values.end(), 0.0) / (double) values.size();
            report << "| " << what << " | " << milliseconds (mean) << " | " << milliseconds (values[values.size() * 95 / 100]) << " | "
                   << milliseconds (values.back()) << " |\n";
        };

        report << "\n| Each of 600 frames, House Groove scrolling continuously at " << juce::String (playlist->getView().pixelsPerBeat, 1)
               << " pixels a beat, 1920 x 1080 | Mean | 95th percentile | Most |\n|---|---|---|---|\n";
        row ("The frame's work: the song's position shown, and what that posts", &Frame::work);
        row ("Its paint, through Direct2D, of what it asked to repaint", &Frame::painted);
        row ("... of which the lanes, with their clips and automation lanes", &Frame::lanes);
        row ("... ... the lanes' grid and sections (their own paint)", &Frame::background);
        row ("... ... the automation lanes", &Frame::automation);
        row ("... ... the MIDI clips", &Frame::midi);
        row ("... ... the audio clips", &Frame::audio);
        row ("... the ruler, the sections' strip and the chord lane", &Frame::bar);

        const auto slow = std::count_if (frames.begin(), frames.end(), [] (const Frame& f) { return f.work + f.painted > 8.0; });
        const auto slowest = std::max_element (frames.begin(), frames.end(),
                                               [] (const Frame& a, const Frame& b) { return a.work + a.painted < b.work + b.painted; });
        report << "\nFrames whose work and paint took more than half a frame (8 ms): " << (int) slow << " of " << (int) frames.size()
               << "; the slowest is frame " << (int) std::distance (frames.begin(), slowest) + 1
               << "\nAutomation lanes: " << (int) automationLanes.size() << "; clip components at the end: " << clips.size()
               << ", of which kept as pictures: " << kept << "\n";
    }

    phazite::ui::setClipWaveformMaker ({});
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    MESSAGE (report);
}

TEST_CASE ("Benchmark: the mouse moving over the playlist while the song scrolls continuously, at the display's pace"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // The owner, 2026-11-07: "when i move my mouse around on top of the playlist though, i can get visual stuttering". Frames
    // at 60 Hz in real time, so that timers run as in the app, with the mouse moving over the track headers and the lanes
    // between them as Windows reports it (four moves a frame) for two seconds, then resting for one, as a hand does; first
    // without the window's tooltips, then with them, on frosted glass as the main window has them (docs/performance-audit.md,
    // section 16).
    juce::Image frameImage (juce::Image::ARGB, 1920, 1080, true);
    {
        juce::Graphics g (frameImage);
    }

    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    juce::String report;

    {
        phazite::test::AppScreen screen;
        screen.window.setSize (1920, 1080);
        screen.session.openAtStartup();
        REQUIRE (screen.registry.invoke ("help.example.houseGroove"));
        phazite::test::dispatchMessagesUntil ([] { return false; }, 2000);
        auto* song = phazite::findSong (screen.session.getContext());
        auto* playlist = phazite::test::findAll<phazite::ui::Playlist> (screen.shell).getFirst();
        auto* lanes = playlist != nullptr ? phazite::test::findComponentWithTitle (*playlist, "Tracks") : nullptr;
        REQUIRE (song != nullptr);
        REQUIRE (lanes != nullptr);
        playlist->setScrollingContinuously (true);
        playlist->zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (lanes->getWidth() / 22.0) });
        phazite::test::dispatchPendingMessages();
        std::unique_ptr<phazite::ui::Tooltips> tips;

        // A tooltip shown over the lanes: what showing it takes, with the clips under it kept as pictures and without.
        const auto timeTips = [&]
        {
            std::vector<double> times;
            const auto centre = lanes->localPointToGlobal (lanes->getLocalBounds().getCentre());

            for (int i = 0; i < 20; ++i)
            {
                const auto at = centre + juce::Point<int> ((i % 5) * 120 - 240, (i % 4) * 48 - 96);
                times.push_back (millisecondsOf ([&] { tips->displayTip (at, "A tip over the clips, " + juce::String (i)); }));
                tips->hideTip();
            }

            std::sort (times.begin(), times.end());
            return times;
        };

        tips = std::make_unique<phazite::ui::Tooltips> (screen.window, true);
        const auto overPictures = timeTips();

        for (auto* clip : phazite::test::findAll<phazite::ui::ClipComponent> (*playlist))
            clip->setBufferedToImage (false); // until the view moves

        const auto overClips = timeTips();
        tips = nullptr;
        report << "\n| A tooltip shown over the lanes, on frosted glass (20 times) | Median | Most |\n|---|---|---|\n"
               << "| Over clips kept as pictures | " << milliseconds (overPictures[10]) << " | " << milliseconds (overPictures.back())
               << " |\n| Over clips painted afresh | " << milliseconds (overClips[10]) << " | " << milliseconds (overClips.back())
               << " |\n";

        // The frames, paced as the display's: a frame's work (the mouse's moves before it, the song's position shown and what
        // that posts) and its paint, and the frames missed while one took longer than a frame.
        const auto area = juce::Rectangle<int> (0, lanes->getY(), playlist->getWidth(), lanes->getHeight()).reduced (8);
        const auto pathAt = [area] (int step)
        {
            const auto t = step * 0.003;
            return juce::Point<int> (area.getCentreX() + juce::roundToInt (area.getWidth() * 0.5 * std::sin (t * 0.9)),
                                     area.getCentreY() + juce::roundToInt (area.getHeight() * 0.5 * std::sin (t * 0.37)));
        };

        const auto run = [&] (const juce::String& name, bool moving)
        {
            phazite::playOrStop (*song);
            REQUIRE (phazite::isSongPlaying (*song));
            auto* session = phazite::findPlayingSession (*song);
            REQUIRE (session != nullptr);
            phazite::test::Mouse mouse (*playlist);
            std::vector<double> work, paint, moves;
            auto missed = 0, shows = 0, over = 0;
            auto showing = false;
            auto beat = 64.0;
            constexpr double interval = 1000.0 / 60.0;
            auto due = juce::Time::getMillisecondCounterHiRes();

            for (int f = 0; f < 1200; ++f)
            {
                due += interval;

                while (juce::Time::getMillisecondCounterHiRes() < due)
                    std::this_thread::yield();

                beat += 124.0 / 60.0 / 60.0;
                session->locate (phazite::BeatPosition::fromBeats (beat));

                work.push_back (millisecondsOf (
                    [&]
                    {
                        if (moving && f % 180 < 120)
                            for (int m = 0; m < 4; ++m)
                                moves.push_back (millisecondsOf ([&] { mouse.moveTo (pathAt (f * 4 + m)); }));

                        playlist->showFrame (due / 1000.0);
                        phazite::test::dispatchPendingMessages();
                    }));

                const auto repainted = screen.window.takeRepaintedArea();
                paint.push_back (millisecondsOf (
                    [&]
                    {
                        juce::Graphics g (frameImage);

                        if (g.reduceClipRegion (repainted))
                            screen.window.paintEntireComponent (g, false);
                    }));

                over += work.back() + paint.back() > 8.0 ? 1 : 0;
                shows += tips != nullptr && tips->isVisible() && ! showing ? 1 : 0;
                showing = tips != nullptr && tips->isVisible();

                // The vertical blanks that went by meanwhile.
                for (const auto now = juce::Time::getMillisecondCounterHiRes(); due + interval < now; due += interval)
                    ++missed;
            }

            phazite::playOrStop (*song);
            const auto row = [&report] (const juce::String& what, std::vector<double> values)
            {
                std::sort (values.begin(), values.end());
                const auto mean = std::accumulate (values.begin(), values.end(), 0.0) / (double) values.size();
                report << " | " << what << " " << milliseconds (mean) << " / " << milliseconds (values[values.size() * 95 / 100]) << " / "
                       << milliseconds (values.back());
            };

            report << "| " << name;

            if (moves.empty())
                report << " | -";
            else
                row ("", moves);

            row ("", work);
            row ("", paint);
            report << " | " << over << " | " << missed << " | " << shows << " |\n";
        };

        report << "\n| 1,200 frames at 60 Hz, House Groove scrolling continuously, the mouse moving over the playlist | A move of the "
                  "mouse, mean / 95th percentile / most | A frame's work, with its moves | Its paint | Frames over 8 ms | Frames missed | "
                  "Tooltips shown |\n|---|---|---|---|---|---|---|\n";
        run ("The mouse still", false);
        run ("Without tooltips", true);
        tips = std::make_unique<phazite::ui::Tooltips> (screen.window, true);
        run ("With the window's tooltips", true);
        tips = nullptr;
    }

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    MESSAGE (report);
}

TEST_CASE ("Benchmark: starting up, and the window at rest" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Starting up as Main.cpp and MainWindow do, but for the process itself and the audio device, which the tests' engine
    // context opens once: the services, the commands and the window's content; the project the first start opens, shown;
    // and an example song opened from Help, shown.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::String report ("\n| Starting up, and at rest | |\n|---|---|\n");
    std::unique_ptr<phazite::test::AppScreen> screen;
    report << "| The services, the commands and the window's content | "
           << milliseconds (millisecondsOf ([&] { screen = std::make_unique<phazite::test::AppScreen>(); }), 1) << " |\n";
    report << "| The first start's project, open and the window painted | "
           << milliseconds (millisecondsOf (
                                [&]
                                {
                                    screen->session.openAtStartup();
                                    phazite::test::dispatchPendingMessages();
                                    paintTime (screen->shell, screen->shell.getLocalBounds());
                                }),
                            1)
           << " |\n";
    report << "| An example song (House Groove, 64 bars) opened from Help and the window painted | "
           << milliseconds (millisecondsOf (
                                [&]
                                {
                                    REQUIRE (screen->registry.invoke ("help.example.houseGroove"));
                                    phazite::test::dispatchPendingMessages();
                                    paintTime (screen->shell, screen->shell.getLocalBounds());
                                }),
                            1)
           << " |\n";

    // At rest with the song open and stopped: what the window asks to repaint, and the process's CPU (the tests' process,
    // with no audio device: the message thread's timers and the threads that wait). The message thread waits as the app's
    // message loop does, asleep until a message comes, then dispatches what there is.
    const auto rest = [] (double milliseconds)
    {
        const auto until = juce::Time::getMillisecondCounterHiRes() + milliseconds;

        for (auto now = juce::Time::getMillisecondCounterHiRes(); now < until; now = juce::Time::getMillisecondCounterHiRes())
            if (phazite::test::waitForMessages ((int) (until - now) + 1))
                phazite::test::dispatchWaitingMessages();
    };

    rest (2000.0);
    screen->window.takeRepaintedArea();
    const auto cpuBefore = phazite::test::getProcessCpuSeconds(), messageThreadBefore = phazite::test::getThreadCpuSeconds();
    const auto start = juce::Time::getMillisecondCounterHiRes();
    rest (5000.0);
    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
    const auto cpu = (phazite::test::getProcessCpuSeconds() - cpuBefore) / seconds;
    const auto messageThread = (phazite::test::getThreadCpuSeconds() - messageThreadBefore) / seconds;
    const auto repainted = screen->window.takeRepaintedArea().getBounds();
    report << "| At rest for 5 s: the process's CPU | " << juce::String (cpu * 100.0, 2) << " % of a core |\n";
    report << "| ... of which the message thread's | " << juce::String (messageThread * 100.0, 2) << " % of a core |\n";
    report << "| At rest for 5 s: asked to repaint | "
           << (repainted.isEmpty() ? juce::String ("nothing")
                                   : juce::String (repainted.getWidth()) + " x " + juce::String (repainted.getHeight()) + " px")
           << " |\n";
    MESSAGE (report);
    screen = nullptr;
}
