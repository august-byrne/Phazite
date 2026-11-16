// SPDX-License-Identifier: AGPL-3.0-or-later
// Benchmarks (docs/performance.md): what each device costs and how soon it sounds, how soon a live note sounds through
// Phazite's engine, what a project costs stopped and playing (and a song of 100 tracks at 128 and 512 samples), how long
// saving, opening and the UI's work take, what scrolling and zooming a big song cost, and the mixer's and the playlist's work
// on a song of 200 tracks.
// ctest and CI skip them; run them from an optimised build (windows-profile or windows-release) with
//   phazite_tests.exe --test-suite=benchmark --no-skip
// and record the numbers in docs/performance.md. Times depend on the machine: compare runs on the same one. What can be
// compared across machines is gated elsewhere: counts in PerformanceGateTests.cpp (ctest), ratios in BenchmarkGates.cpp.
#include "AppScreen.h"
#include "BenchmarkSong.h"
#include "EditorScreens.h"
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "HostedDevice.h"
#include "ProcessMemory.h"
#include "SandboxedPlugins.h"
#include "TestClips.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "app/ClipWaveforms.h"
#include "devices/core/Spectrum.h"
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
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/stretch/Stretch.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/audio/AudioRegion.h"
#include "engine/audio/Resampler.h"
#include "engine/nodes/DeviceRunner.h"
#include "engine/nodes/Strip.h"
#include "host/ThreadCpu.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/PreviewLoop.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/Export.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "ui/components/Controls.h"
#include "ui/devices/EqEditor.h"
#include "ui/devices/ReverbEditor.h"
#include "ui/devices/SamplerEditor.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/MixerView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TrackHeader.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
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
    bool playing = false;                             // whether the song plays (the vinyl's noise only plays then)
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
                                            .parameters = values,
                                            .beat = b * benchBlock * 2.0 / benchRate,
                                            .bars = b * benchBlock * 2.0 / benchRate / 4.0, // in 4/4
                                            .isPlaying = c.playing };
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
        { "Wavetable, 8 notes, full unison, stacked and drifting", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 8,
          [&] (std::vector<float>& v)
          {
              v[dev::WavetableSynth::unisonVoices] = maxOf (dev::WavetableSynth::info, dev::WavetableSynth::unisonVoices);
              v[dev::WavetableSynth::unisonStack] = (float) dev::WavetableSynth::Stack::twoOctaves;
              v[dev::WavetableSynth::unisonDrift] = 25.0f;
          } },
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
        { "Exciter", maker<dev::Exciter>(), &dev::Exciter::info },
        { "Exciter, its band from 1 kHz", maker<dev::Exciter>(), &dev::Exciter::info, 0,
          [] (std::vector<float>& v) { v[dev::Exciter::frequency] = 1000.0f; } },
        { "Vinyl", maker<dev::Vinyl>(), &dev::Vinyl::info },
        { "Vinyl, playing, everything on", maker<dev::Vinyl>(), &dev::Vinyl::info, 0,
          [] (std::vector<float>& v)
          {
              v[dev::Vinyl::wow] = 20.0f;
              v[dev::Vinyl::flutter] = 8.0f;
              v[dev::Vinyl::distortion] = 0.8f;
              v[dev::Vinyl::crackle] = -18.0f;
              v[dev::Vinyl::density] = 300.0f;
          },
          false, true },
        { "Vinyl, silence in, stopped", maker<dev::Vinyl>(), &dev::Vinyl::info, 0, {}, true },
        { "Delay", maker<dev::Delay>(), &dev::Delay::info },
        { "Reverb", maker<dev::Reverb>(), &dev::Reverb::info },
        { "Reverb, plate", maker<dev::Reverb>(), &dev::Reverb::info, 0,
          [] (std::vector<float>& v) { v[dev::Reverb::mode] = (float) dev::Reverb::plate; } },
        { "Stereo Expander, spreading", maker<dev::StereoExpander>(), &dev::StereoExpander::info, 0,
          [] (std::vector<float>& v) { v[dev::StereoExpander::spread] = 1.0f; } },
        { "Phaser, six notches", maker<dev::Phaser>(), &dev::Phaser::info, 0,
          [] (std::vector<float>& v) { v[dev::Phaser::notches] = 5.0f; } },
        { "Flanger", maker<dev::Flanger>(), &dev::Flanger::info },
        { "Chorus, three voices with shimmer", maker<dev::Chorus>(), &dev::Chorus::info, 0,
          [] (std::vector<float>& v)
          {
              v[dev::Chorus::voices] = 2.0f;
              v[dev::Chorus::shimmer] = 1.0f;
          } },
        { "Chorus, eight voices spread", maker<dev::Chorus>(), &dev::Chorus::info, 0,
          [] (std::vector<float>& v)
          {
              v[dev::Chorus::voices] = 7.0f;
              v[dev::Chorus::spread] = 0.6f;
          } },
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

TEST_CASE ("Benchmark: each device's reset, as Play and a jump reset every device in the block that starts or lands"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Known bug 85's review: what a reset costs each of Phazite's devices, through its runner as the engine resets it
    // (engine::Engine::flushStrip()), at 48 kHz in blocks of 128, after it has played a while (notes held, noise in), as
    // a share of a block's 2.67 ms. The median of 50.
    const std::vector<std::pair<const dev::DeviceInfo*, std::function<std::unique_ptr<dev::Processor>()>>> makers {
        { &dev::Synth::info, maker<dev::Synth>() },
        { &dev::WavetableSynth::info, maker<dev::WavetableSynth>() },
        { &dev::FmSynth::info, maker<dev::FmSynth>() },
        { &dev::DrumSampler::info, maker<dev::DrumSampler>() },
        { &dev::Sampler::info, maker<dev::Sampler>() },
        { &dev::Equalizer::info, maker<dev::Equalizer>() },
        { &dev::FilterEffect::info, maker<dev::FilterEffect>() },
        { &dev::Compressor::info, maker<dev::Compressor>() },
        { &dev::Multiband::info, maker<dev::Multiband>() },
        { &dev::Limiter::info, maker<dev::Limiter>() },
        { &dev::Saturator::info, maker<dev::Saturator>() },
        { &dev::Exciter::info, maker<dev::Exciter>() },
        { &dev::Vinyl::info, maker<dev::Vinyl>() },
        { &dev::Delay::info, maker<dev::Delay>() },
        { &dev::Reverb::info, maker<dev::Reverb>() },
        { &dev::StereoExpander::info, maker<dev::StereoExpander>() },
        { &dev::Phaser::info, maker<dev::Phaser>() },
        { &dev::Flanger::info, maker<dev::Flanger>() },
        { &dev::Chorus::info, maker<dev::Chorus>() },
        { &dev::Vocoder::info, maker<dev::Vocoder>() },
    };

    juce::String report ("\n| Device | Its reset (% of a block of 128 at 48 kHz) |\n|---|---|\n");
    juce::Random random (3);

    for (const auto& [infoOf, make] : makers)
    {
        const auto& info = *infoOf;
        phazite::engine::DeviceRunner runner (info, make());
        runner.prepare (benchRate, benchBlock);
        auto values = defaultsOf (info);
        juce::AudioBuffer<float> buffer (2, benchBlock);
        std::vector<dev::NoteEvent> notes;

        for (int n = 0; n < 8; ++n)
            notes.push_back ({ .type = dev::NoteEvent::Type::noteOn, .sample = 0, .number = 48 + n * 3, .value = 0.8f });

        std::vector<double> times;

        for (int round = 0; round < 50; ++round)
        {
            for (int b = 0; b < 20; ++b)
            {
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < benchBlock; ++i)
                        buffer.setSample (channel, i, info.kind == dev::DeviceKind::instrument ? 0.0f : 0.3f * (random.nextFloat() - 0.5f));

                const dev::ProcessContext context { .channels = buffer.getArrayOfWritePointers(),
                                                    .numChannels = 2,
                                                    .numSamples = benchBlock,
                                                    .events = b == 0 ? std::span<const dev::NoteEvent> (notes)
                                                                     : std::span<const dev::NoteEvent>(),
                                                    .parameters = values,
                                                    .isPlaying = true };
                runner.getProcessor().process (context);
            }

            times.push_back (millisecondsOf ([&runner] { runner.reset(); }));
        }

        report << "| " << juce::String (std::string (info.name)) << " | "
               << juce::String (100.0 * phazite::test::percentileOf (times, 0.5) * benchRate / 1000.0 / benchBlock, 2) << " % |\n";
    }

    // And the paths' rings, of which a song of 200 tracks sending to 8 returns has about 400, each a third of a second and
    // a block at 48 kHz: reset (writing nothing), against clearing them as a reset did before.
    std::vector<std::unique_ptr<phazite::engine::DelayLine>> rings;
    std::vector<juce::AudioBuffer<float>> cleared;

    for (int i = 0; i < 400; ++i)
    {
        rings.push_back (std::make_unique<phazite::engine::DelayLine> (16384 + 128));
        cleared.emplace_back (2, 16384 + 128);
    }

    std::vector<double> resets, clears;

    for (int round = 0; round < 20; ++round)
    {
        for (auto& buffer : cleared)
            buffer.setSample (0, 0, 1.0f); // not clear any more, as one that played isn't

        resets.push_back (millisecondsOf (
            [&rings]
            {
                for (auto& ring : rings)
                    ring->reset();
            }));
        clears.push_back (millisecondsOf (
            [&cleared]
            {
                for (auto& buffer : cleared)
                    buffer.clear();
            }));
    }

    const auto ofBlock = [] (double ms) { return juce::String (100.0 * ms * benchRate / 1000.0 / benchBlock, 1) + " %"; };
    report << "| 400 paths' rings, reset (each 16,512 samples) | " << ofBlock (phazite::test::percentileOf (resets, 0.5)) << " |\n"
           << "| The same, cleared as a reset did before | " << ofBlock (phazite::test::percentileOf (clears, 0.5)) << " |\n";

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

TEST_CASE ("Benchmark: a wavetable of 256 frames made, as a sound file of Serum's becomes one" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // The reviewer's finding 2 (2026-11-15).
    // 256 frames of 2048 samples, each a saw of its own phases (all 1024 harmonics), read back as a table: its 21 levels a
    // frame, and what its warps leave off centre (Wavetable::getArea(), getBentMean()). The best of three.
    std::vector<float> sound ((size_t) (256 * dev::Wavetable::frameSize));

    for (size_t i = 0; i < sound.size(); ++i)
    {
        const auto frame = (double) (i / (size_t) dev::Wavetable::frameSize);
        const auto t = (double) (i % (size_t) dev::Wavetable::frameSize) / dev::Wavetable::frameSize;
        sound[i] = (float) (std::fmod (t + 0.37 * frame / 256.0, 1.0) * 2.0 - 1.0);
    }

    auto best = 1.0e9;

    for (int run = 0; run < 3; ++run)
    {
        std::shared_ptr<const dev::Wavetable> table;
        best = std::min (best, millisecondsOf ([&] { table = dev::Wavetable::fromSound ("Bench", sound); }));
        REQUIRE (table->getNumFrames() == 256);
    }

    MESSAGE ("A table of 256 frames made: " << milliseconds (best, 1));
}

TEST_CASE ("Benchmark: an analyzer's work on a second of sound, as an EQ's editor shows it" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // The spectrum of a device's sound (devices/core/Spectrum.h), worked out on the message thread while an editor shows it:
    // ten seconds of noise taken in as the display's refreshes take it, each refresh showing it and reading the curve's
    // levels as a paint does, a point every 2 pixels across a 600-pixel graph from 20 Hz to 20 kHz.
    for (const auto rate : { 48000.0, 96000.0 })
    {
        dev::Spectrum spectrum;
        spectrum.setSampleRate (rate);
        std::vector<float> sound ((size_t) (10.0 * rate));
        juce::Random random (11);

        for (auto& sample : sound)
            sample = random.nextFloat() - 0.5f;

        const auto perRefresh = (int) (rate / 30.0);
        auto levels = 0.0f;
        const auto ms = millisecondsOf (
            [&]
            {
                for (size_t at = 0; at + (size_t) perRefresh <= sound.size(); at += (size_t) perRefresh)
                {
                    spectrum.add (sound.data() + at, perRefresh);
                    spectrum.update (1.3f);

                    for (int x = 0; x < 600; x += 2)
                        levels += spectrum.getLevelOver (20.0 * std::pow (1000.0, x / 600.0), 20.0 * std::pow (1000.0, (x + 2) / 600.0));
                }
            });

        CHECK (levels < 0.0f);
        MESSAGE ("At " << (int) rate << " Hz: " << milliseconds (ms / 10.0) << " a second of sound, " << juce::String (ms / 100.0, 2)
                       << "% of a core");
    }
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
        mutedSynths,
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

            // Muted, a track plays on, its channel silencing it (known bug 78): it costs what it costs heard.
            if (kind == Kind::mutedSynths)
                phazite::setTrackMuted (*song, track, true);

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

    for (const auto& [numTracks, kind] :
         { std::pair (20, Kind::empty), std::pair (20, Kind::synths), std::pair (20, Kind::mutedSynths), std::pair (20, Kind::drums) })
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
            report << "| " << numTracks << (kind == Kind::mutedSynths ? " muted" : "")
                   << (kind == Kind::synths || kind == Kind::mutedSynths ? " tracks, each a synth, an EQ, a compressor and a reverb, chords"
                       : kind == Kind::drums                             ? " tracks, each a drum sampler, a beat"
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

TEST_CASE ("Benchmark: a song of 100 tracks, its blocks at 128 and 512 samples, its plan and an edit" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // The engine's scale (docs/performance.md): the benchmark song (BenchmarkSong.h) of 100 tracks, each an instrument
    // playing all the way, an EQ and a compressor (and a saturator or a chorus on some), sends to a reverb and a delay on
    // returns, buses of ten with a compressor each, automation on a quarter of the faders and on every Synth's cutoff. Its
    // blocks played one after another on the calling thread as the audio device's thread plays them, at 48 kHz, each
    // block's time as a share of its length (100 % = a dropout); its plan's compile, and the message thread's work for an
    // edit (the model, the session's new plan; no window is open, so no view's work is in it).
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 100, 64.0);
    auto* open = phazite::findSong (s.context);
    REQUIRE (open != nullptr);
    auto& song = *open;
    REQUIRE (s.getProject().save().wasOk());

    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    const auto threads = engine.getNumThreads();
    juce::String report ("\n| 100 tracks, 48 kHz | Threads | Stopped: mean / 99th percentile / most | Playing: mean / 99th percentile / "
                         "most |\n|---|---|---|---|\n");

    const auto shares = [] (const std::vector<double>& times, int block)
    {
        const auto length = block / benchRate;
        return juce::String (100.0 * phazite::test::meanOf (times) / length, 1) + " / "
               + juce::String (100.0 * phazite::test::percentileOf (times, 0.99) / length, 1) + " / "
               + juce::String (100.0 * phazite::test::percentileOf (times, 1.0) / length, 1) + " %";
    };

    for (const auto block : { 128, 512 })
    {
        phazite::test::EnginePlayback device (phazite::test::EnginePlayback::Input::silence,
                                              phazite::test::EnginePlayback::Timing::realTime, benchRate, block);
        const auto count = (int) (10.0 * benchRate / block); // ten seconds

        for (const auto numThreads : { 1, 4 })
        {
            engine.setNumThreads (numThreads);
            phazite::test::timeEngineBlocks (100, block);
            const auto stopped = phazite::test::timeEngineBlocks (count, block);
            phazite::playOrStop (song);
            phazite::test::timeEngineBlocks (100, block);
            const auto playing = phazite::test::timeEngineBlocks (count, block);
            phazite::stopAndReturn (song);
            report << "| Blocks of " << block << " samples (" << milliseconds (block * 1000.0 / benchRate) << ") | " << numThreads << " | "
                   << shares (stopped, block) << " | " << shares (playing, block) << " |\n";
        }
    }

    engine.setNumThreads (threads);

    // The message thread's work.
    auto* session = phazite::findPlayingSession (song);
    REQUIRE (session != nullptr);
    std::vector<double> compiles, notes, moves;

    for (int i = 0; i < 20; ++i)
    {
        compiles.push_back (millisecondsOf ([session] { session->update(); }));
        notes.push_back (millisecondsOf (
            [&]
            {
                phazite::addNote (s.context, built.clips[(size_t) i], 72, phazite::BeatPosition::fromBeats (2.5),
                                  phazite::BeatDuration::fromBeats (0.25), 100);
                phazite::test::dispatchPendingMessages();
            }));
        moves.push_back (millisecondsOf (
            [&]
            {
                phazite::moveClip (s.context, built.clips[(size_t) i], phazite::BeatPosition::fromBeats (1.0), built.tracks[(size_t) i]);
                phazite::test::dispatchPendingMessages();
            }));
    }

    report << "\n| 100 tracks, the message thread | Median | Most |\n|---|---|---|\n";
    report << "| The plan's compile, given to the engine | " << milliseconds (phazite::test::percentileOf (compiles, 0.5)) << " | "
           << milliseconds (phazite::test::percentileOf (compiles, 1.0)) << " |\n";
    report << "| A note added, with its new plan | " << milliseconds (phazite::test::percentileOf (notes, 0.5)) << " | "
           << milliseconds (phazite::test::percentileOf (notes, 1.0)) << " |\n";
    report << "| A clip moved, with its new plan | " << milliseconds (phazite::test::percentileOf (moves, 0.5)) << " | "
           << milliseconds (phazite::test::percentileOf (moves, 1.0)) << " |\n";
    report << "| Save, after the song played and was edited | "
           << milliseconds (millisecondsOf ([&] { REQUIRE (s.getProject().save().wasOk()); }), 1) << " |  |\n";
    report << "| Save again, nothing changed | " << milliseconds (millisecondsOf ([&] { REQUIRE (s.getProject().save().wasOk()); }), 1)
           << " |  |\n";
    report << "\nThe project's file: " << juce::String (s.getProject().getFile().getSize() / 1024) << " KB\n";
    MESSAGE (report);
}

TEST_CASE ("Benchmark: an everyday edit's block while its paths fade, on the benchmark song" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // A send taken away and given back as the song plays (known bug 167's review, round 4): the leaving path fades out on the
    // plan played, which plays in place of the new one for those samples (never beside it), and the new one fades in
    // itself. The block in which the edit's plan is taken, against the blocks around it, at 48 kHz in blocks of 480.
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 100, 64.0);
    auto& song = *phazite::findSong (s.context);
    phazite::test::EnginePlayback device (phazite::test::EnginePlayback::Input::silence, phazite::test::EnginePlayback::Timing::realTime,
                                          benchRate, 480);
    phazite::playOrStop (song);
    phazite::test::timeEngineBlocks (200, 480);
    std::vector<double> steady, edited;

    for (int i = 0; i < 40; ++i)
    {
        const auto track = built.tracks[(size_t) i];
        const auto sends = phazite::getSends (s.context, track);
        REQUIRE (! sends.empty());
        const auto send = sends.front();

        for (const auto time : phazite::test::timeEngineBlocks (10, 480))
            steady.push_back (time);

        {
            const phazite::UndoStep step (song, "Remove Send");
            phazite::removeSend (song, send.id);
        }

        edited.push_back (phazite::test::timeEngineBlocks (1, 480).front());
        phazite::undo (song); // the send back: a path that comes
        edited.push_back (phazite::test::timeEngineBlocks (1, 480).front());
    }

    phazite::stopAndReturn (song);
    const auto length = 480 / benchRate;
    MESSAGE ("\n| 100 tracks, 48 kHz, blocks of 480 | Mean | 99th percentile |\n|---|---|---|\n| Blocks around | "
             << juce::String (100.0 * phazite::test::meanOf (steady) / length, 1) << " % | "
             << juce::String (100.0 * phazite::test::percentileOf (steady, 0.99) / length, 1)
             << " % |\n| A send taken away or given back | " << juce::String (100.0 * phazite::test::meanOf (edited) / length, 1) << " % | "
             << juce::String (100.0 * phazite::test::percentileOf (edited, 0.99) / length, 1) << " % |\n");
}

TEST_CASE ("Benchmark: a loop's wrap and a jump on a song of 200 tracks, against the blocks around them" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // Known bug 85: the block in which the loop goes round (every note before the playhead looked through on every track for
    // those sounding, and each audio clip of a file at another rate starting its resampler from nothing) and the block in
    // which a jump lands (every device reset too), against the blocks around them. The benchmark song of 200 tracks, 256
    // beats of notes (about 176,000), and 40 tracks of audio clips of a file at 44.1 kHz, which the engine at 48 kHz
    // resamples, looping two beats near the song's end; blocks of 128 samples, on one thread and on four.
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 200, 256.0);
    auto& song = *phazite::findSong (s.context);
    const auto sound = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Pad.wav"), 12.0);

    for (int i = 0; i < 40; ++i)
        s.addAudioClip (s.addTrack ("Audio " + juce::String (i + 1)), sound, 240.0);

    constexpr double loopStart = 248.0, loopEnd = 250.0;
    phazite::loopBeats (song, { phazite::BeatPosition::fromBeats (loopStart), phazite::BeatPosition::fromBeats (loopEnd) });
    constexpr int block = 128;
    phazite::test::EnginePlayback device (phazite::test::EnginePlayback::Input::silence, phazite::test::EnginePlayback::Timing::realTime,
                                          benchRate, block);
    s.waitForClipRenders();
    phazite::test::dispatchPendingMessages();

    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    const auto threads = engine.getNumThreads();
    const auto loopLength = (std::int64_t) std::llround ((loopEnd - loopStart) * 0.5 * benchRate); // at 120 BPM
    juce::String report ("\n| 200 tracks and 40 resampled audio tracks, 48 kHz, blocks of 128 | Threads | Blocks around: median / 99th "
                         "percentile | The wrap's block: mean / most | A jump's block: mean / most |\n|---|---|---|---|---|\n");

    for (const auto numThreads : { 1, 4 })
    {
        engine.setNumThreads (numThreads);
        phazite::movePlayhead (song, phazite::BeatPosition::fromBeats (loopStart));
        phazite::playOrStop (song);
        const auto times = phazite::test::timeEngineBlocks ((int) (12.0 * benchRate / block), block);
        std::vector<double> around, wraps, jumps;

        // The block the wrap's work falls in: the one the loop's start plays from again in (the first, from where playback
        // started, isn't one). The first second warms up.
        for (size_t k = (size_t) (benchRate / block); k < times.size(); ++k)
        {
            const auto first = (std::int64_t) k * block, last = first + block;
            const auto wrapsIn = (last - 1) / loopLength > (first - 1) / loopLength;
            (wrapsIn ? wraps : around).push_back (times[k]);
        }

        // A jump to half a beat into the loop, in the middle of a round, where the resampled clips aren't made ready yet: the
        // most of the eight blocks after it is asked for (its fade out over 144 samples, the landing held while they are made
        // ready, and the landing).
        for (int jump = 0; jump < 20; ++jump)
        {
            phazite::test::timeEngineBlocks (100, block);
            phazite::movePlayhead (song, phazite::BeatPosition::fromBeats (loopStart + 0.5));
            const auto after = phazite::test::timeEngineBlocks (8, block);
            jumps.push_back (*std::max_element (after.begin(), after.end()));
        }

        phazite::stopAndReturn (song);
        const auto share = [] (double seconds) { return juce::String (100.0 * seconds * benchRate / block, 1); };
        report << "| | " << numThreads << " | " << share (phazite::test::percentileOf (around, 0.5)) << " / "
               << share (phazite::test::percentileOf (around, 0.99)) << " % | " << share (phazite::test::meanOf (wraps)) << " / "
               << share (phazite::test::percentileOf (wraps, 1.0)) << " % | " << share (phazite::test::meanOf (jumps)) << " / "
               << share (phazite::test::percentileOf (jumps, 1.0)) << " % |\n";
    }

    engine.setNumThreads (threads);
    MESSAGE (report << "\n(" << built.tracks.size() << " tracks of notes; a block's share of its 2.67 ms)\n");
}

TEST_CASE ("Benchmark: a loop's wrap's own work, on 200 tracks of notes and 40 resampled clips: the notes sounding found, the clips started"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Known bug 85, the wrap's two parts measured alone, without the devices' work around them: the notes sounding at the
    // loop's start found on 200 tracks of 256 beats (a chord of four on every beat, 1,024 notes a track), by a look through
    // every note before it (as before) and by the sequences' index; and a block of 128 samples played from the loop's start
    // by 40 clips of a stereo file at 44.1 kHz on a 48 kHz engine, their resamplers started from nothing (as before) and
    // their spares made ready beforehand. Each the median of 50.
    namespace engine = phazite::engine;
    std::vector<std::shared_ptr<const engine::NoteSequence>> tracks;

    for (int t = 0; t < 200; ++t)
    {
        std::vector<engine::NoteSequence::Note> notes;

        for (int beat = 0; beat < 256; ++beat)
            for (const auto pitch : { 48, 52, 55, 59 })
                notes.push_back ({ (double) beat, beat + 0.9, pitch + t % 12, 0.8f });

        tracks.push_back (std::make_shared<const engine::NoteSequence> (std::move (notes), std::vector<engine::NoteSequence::Control>()));
    }

    constexpr double at = 248.5; // in the chord on beat 248
    auto found = 0;
    std::vector<double> walked, indexed;

    for (int round = 0; round < 50; ++round)
    {
        walked.push_back (millisecondsOf (
            [&]
            {
                for (const auto& track : tracks)
                    for (const auto& note : track->getNotes())
                    {
                        if (note.start >= at)
                            break;

                        found += note.end > at ? 1 : 0;
                    }
            }));

        indexed.push_back (millisecondsOf (
            [&]
            {
                for (const auto& track : tracks)
                    track->forEachSounding (at, at, [&found] (const auto&) { ++found; });
            }));
    }

    struct NoiseReader final : engine::SampleReader
    {
        int getNumChannels() const override { return 2; }
        double getSampleRate() const override { return 44100.0; }
        std::int64_t getLength() const override { return (std::int64_t) (20.0 * 44100.0); }

        bool read (juce::AudioBuffer<float>& buffer, std::int64_t, int numSamples) override
        {
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < numSamples; ++i)
                    buffer.setSample (channel, i, 0.3f * (random.nextFloat() * 2.0f - 1.0f));

            return true;
        }

        juce::Random random { 7 };
    };

    constexpr double engineRate = 48000.0;
    constexpr int block = 128;
    constexpr std::int64_t loopStart = 240000, playing = 400000;
    auto file = std::make_shared<engine::AudioFile> (std::make_unique<NoiseReader>(), nullptr);
    file->readNow (0, file->getLength());
    juce::AudioBuffer<float> out (2, block), scratch (2, 9 * block + 16);
    float* channels[] = { out.getWritePointer (0), out.getWritePointer (1) };
    const std::int64_t places[] = { loopStart };
    std::vector<double> fresh, warmed;

    for (int round = 0; round < 50; ++round)
    {
        std::vector<engine::AudioRegion> regions;

        for (int i = 0; i < 40; ++i)
        {
            engine::AudioRegion region;
            region.file = file;
            region.state = std::make_shared<engine::RegionState> (*file, engineRate);
            region.length = (std::int64_t) (19.0 * engineRate);
            region.play (channels, playing, block, engineRate, false, scratch); // playing elsewhere, as before the wrap
            regions.push_back (region);
        }

        fresh.push_back (millisecondsOf (
            [&]
            {
                for (const auto& region : regions)
                    region.play (channels, loopStart, block, engineRate, false, scratch);
            }));

        // Again, each made ready over the blocks before (as the engine does a little a block).
        for (auto& region : regions)
        {
            region.play (channels, playing, block, engineRate, false, scratch);

            for (int b = 0; b < 100; ++b)
                region.prepare (playing + block, true, places, engineRate, block);
        }

        warmed.push_back (millisecondsOf (
            [&]
            {
                for (const auto& region : regions)
                    region.play (channels, loopStart, block, engineRate, false, scratch);
            }));
    }

    // What a clip's resamplers hold: 400 clips' states made (each two resamplings of two channels, one of them the spare).
    const auto memoryBefore = phazite::test::getProcessMemoryBytes();
    std::vector<std::shared_ptr<engine::RegionState>> states;

    for (int i = 0; i < 400; ++i)
        states.push_back (std::make_shared<engine::RegionState> (*file, engineRate));

    const auto perClip = (double) (phazite::test::getProcessMemoryBytes() - memoryBefore) / 400.0;
    states.clear();

    // How much of the file the resampler takes before it gives out its first sample, as it starts from nothing.
    auto taken = 0;
    {
        phazite::engine::Resampler resampler (44100.0, engineRate, 1024);
        std::vector<double> zeros (64, 0.0);

        for (const double* given = nullptr; resampler.process (zeros.data(), 64, given) == 0;)
            taken += 64;
    }

    const auto share = [] (double ms) { return juce::String (100.0 * ms / (block * 1000.0 / engineRate), 1) + " %"; };
    MESSAGE ("\n| A loop's wrap, its own work, as a share of a block of 128 at 48 kHz | Before | Now |\n|---|---|---|\n"
             << "| The notes sounding at its start found on 200 tracks of 1,024 notes | "
             << share (phazite::test::percentileOf (walked, 0.5)) << " | " << share (phazite::test::percentileOf (indexed, 0.5)) << " |\n"
             << "| 40 clips at 44.1 kHz playing their first block from its start | " << share (phazite::test::percentileOf (fresh, 0.5))
             << " | " << share (phazite::test::percentileOf (warmed, 0.5)) << " |\n(" << found << " found)\n"
             << "A resampled clip's state, its two resamplings of two channels: " << juce::String (perClip / 1024.0, 0)
             << " KB; the resampler's start: " << taken << " samples of the file (44.1 to 48 kHz) before its first comes out ("
             << juce::String (taken * 1000.0 / 44100.0, 1) << " ms)\n");
}

TEST_CASE ("Benchmark: a long take dense with pressure: its compile after a note edited, what its sequence holds, the chase at a wrap"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Known bug 85's review: the index that the chase reads (NoteSequence::forEachControlAt()) is made with the track's
    // sequence at every compile of the track, and read at every start, jump and wrap of the loop.
    namespace engine = phazite::engine;
    using Type = dev::NoteEvent::Type;
    juce::String report ("\n");

    // A take of a minute, ten keys held with their pressure at 200 values a second each, in a clip; one note added, and
    // the session's compile of the song for it (the model's work, the sequence made again).
    {
        phazite::test::TestSong s;
        auto& song = *phazite::findSong (s.context);
        phazite::test::addTestDevice (song, s.firstTrack, phazite::test::TestTone::typeID);
        const auto clip = s.addMidiClip (s.firstTrack, 0.0, 120.0);
        std::vector<phazite::ControlValue> values;

        for (int key = 0; key < 10; ++key)
        {
            s.addNote (clip, 60 + key, 0.0, 120.0);

            for (int i = 0; i < 12000; ++i)
                values.push_back ({ phazite::BeatPosition::fromBeats (i * 0.01), phazite::ControlKind::aftertouch, 60 + key, i % 128 });
        }

        phazite::addControls (song, clip, values);
        phazite::test::EnginePlayback playback;
        playback.play (0.01);
        auto* session = phazite::findPlayingSession (song);
        std::vector<double> compiles;

        for (int i = 0; i < 10; ++i)
            compiles.push_back (millisecondsOf (
                [&]
                {
                    s.addNote (clip, 40 + i, 1.0 + i, 0.5);
                    session->update();
                }));

        const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
        REQUIRE (plan != nullptr);
        std::size_t bytes = 0;

        for (const auto& strip : plan->strips)
            if (strip.notes != nullptr)
                bytes += strip.notes->getBytes();

        report << "A take of a minute, ten keys' pressure at 200 values a second (120,000 values): a note added and the song compiled "
               << milliseconds (phazite::test::percentileOf (compiles, 0.5), 1) << " (median); its sequence holds "
               << juce::String ((double) bytes / (1024.0 * 1024.0), 2) << " MB\n";
    }

    // The bound a take keeps to (Take::controlSeconds): ten keys held for five minutes, each one's pressure a thousand
    // times a second, 3 million values: the sequence made, and what it holds.
    {
        std::vector<engine::NoteSequence::Note> notes;
        std::vector<engine::NoteSequence::Control> controls;

        for (int key = 0; key < 10; ++key)
        {
            notes.push_back ({ 0.0, 600.0, 60 + key, 0.8f });

            for (int i = 0; i < 300000; ++i)
                controls.push_back ({ i * 0.002, Type::aftertouch, 60 + key, (float) (i % 128) / 127.0f });
        }

        std::unique_ptr<engine::NoteSequence> sequence;
        const auto made = millisecondsOf ([&] { sequence = std::make_unique<engine::NoteSequence> (notes, controls); });
        report << "Five minutes of it at the bound (3 million values): the sequence made in " << milliseconds (made, 0) << ", holding "
               << juce::String ((double) sequence->getBytes() / (1024.0 * 1024.0), 1) << " MB\n";
    }

    // The chase at a wrap on a track dense with controllers: sixty of them, 2,000 changes each, and ten keys' pressure; its
    // values found where the loop starts, as a share of a block of 128 at 48 kHz (the median of 1,000).
    {
        std::vector<engine::NoteSequence::Note> notes;
        std::vector<engine::NoteSequence::Control> controls;

        for (int key = 0; key < 10; ++key)
            notes.push_back ({ 0.0, 600.0, 60 + key, 0.8f });

        for (int i = 0; i < 120000; ++i)
            controls.push_back ({ i * 0.005, Type::controller, i % 60, (float) (i % 128) / 127.0f });

        for (int i = 0; i < 20000; ++i)
            controls.push_back ({ i * 0.03, Type::aftertouch, 60 + i % 10, (float) (i % 128) / 127.0f });

        const engine::NoteSequence sequence (notes, controls);
        std::vector<double> chases;
        auto found = 0;

        for (int i = 0; i < 1000; ++i)
            chases.push_back (millisecondsOf (
                [&]
                {
                    const auto at = 100.0 + i * 0.37;
                    sequence.forEachControlAt (at, at, [&found] (const auto& control) { found += control.held ? 1 : 0; });
                }));

        report << "The chase at a wrap on a track of sixty controllers with 2,000 changes each and ten keys' pressure: "
               << juce::String (100.0 * phazite::test::percentileOf (chases, 0.5) * benchRate / 1000.0 / 128.0, 3)
               << " % of a block of 128 at 48 kHz (median; " << found << " found)\n";
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: what the message thread waits for as a new plan keeps an audio file's clip start, while a chunk is read"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Known bug 85: the reading thread held a file's lock across its read from the disk, which the message thread takes at
    // every new plan (AudioFile::keep()). A file whose reads each take 20 ms, as a cold read of a chunk from a slow or busy
    // drive can, read by a thread of its own; the message thread's keep() at each of 200 "plans" meanwhile.
    namespace engine = phazite::engine;

    struct SlowReader final : engine::SampleReader
    {
        int getNumChannels() const override { return 2; }
        double getSampleRate() const override { return 48000.0; }
        std::int64_t getLength() const override { return (std::int64_t) (600.0 * 48000.0); }

        bool read (juce::AudioBuffer<float>& buffer, std::int64_t, int numSamples) override
        {
            juce::Thread::sleep (20);
            buffer.clear (0, numSamples);
            return true;
        }
    };

    std::atomic<std::uint64_t> blocks { 0 };
    engine::AudioFile file (std::make_unique<SlowReader>(), &blocks);
    std::atomic<bool> done { false };
    std::thread reading (
        [&]
        {
            for (int chunk = 0; ! done.load(); ++chunk)
            {
                file.want ((std::int64_t) (chunk % 800) * engine::AudioFile::chunkSamples);
                file.readNext();
            }
        });

    std::vector<double> waits;

    for (int plan = 0; plan < 200; ++plan)
    {
        waits.push_back (millisecondsOf ([&] { file.keep ((std::int64_t) plan * 4800); }));
        juce::Thread::sleep (3);
    }

    done.store (true);
    reading.join();
    MESSAGE ("\nkeep() while chunks of 20 ms are read: median " << milliseconds (phazite::test::percentileOf (waits, 0.5), 3) << ", most "
                                                                << milliseconds (phazite::test::percentileOf (waits, 1.0), 3) << "\n");
}

TEST_CASE ("Benchmark: 200 tracks each sending to 8 returns, the rings' memory and a block's cost at 48 and 192 kHz"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Live, every path keeps a ring with a third of a second of room (known bug 167's review): one per path made 1,809
    // rings for this song; the paths from one point of a strip share one now (a track's sends after its fader, its way to
    // the master). 200 tracks, each a Synth holding a chord, sending after its fader to 8 returns (each an EQ); one track's
    // 64 samples of latency lines the others up. The rings the plan's paths read, and its blocks of 10 ms played one after
    // another on the calling thread, on one thread and on four.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    std::vector<phazite::TrackID> tracks { s.firstTrack };

    while (tracks.size() < 200)
        tracks.push_back (s.addTrack ("Track " + juce::String (tracks.size() + 1)));

    for (const auto track : tracks)
    {
        s.setInstrument (track, dev::Synth::typeID);
        s.addChord (s.addMidiClip (track, 0.0, 64.0), { 48, 55, 64 }, 0.0, 64.0, 60);
    }

    for (int r = 0; r < 8; ++r)
    {
        const auto back = s.addReturn (tracks.front(), dev::Equalizer::typeID, -12.0f);

        for (size_t t = 1; t < tracks.size(); ++t)
            s.addSend (tracks[t], back, -12.0f);
    }

    s.setParameter (phazite::test::addTestDevice (song, tracks.front(), phazite::test::TestLatency::typeID), "samples", 64.0f);
    phazite::test::dispatchPendingMessages();

    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    const auto threads = engine.getNumThreads();
    juce::String report ("\n| 200 tracks, 8 returns | Rings | Their memory | Threads | Playing: mean / 99th percentile (% of the block) |\n"
                         "|---|---|---|---|---|\n");

    for (const auto rate : { 48000.0, 192000.0 })
    {
        const auto block = (int) (rate / 100.0);
        phazite::test::EnginePlayback device (phazite::test::EnginePlayback::Input::silence,
                                              phazite::test::EnginePlayback::Timing::realTime, rate, block);
        phazite::test::dispatchPendingMessages();
        const auto* plan = engine.getPlan();
        REQUIRE (plan != nullptr);

        // Each ring the plan's paths read once: the strips' taps and the delays' own.
        std::set<const phazite::engine::DelayLine*> rings;

        for (const auto& strip : plan->strips)
        {
            for (const auto& tap : strip.taps)
                rings.insert (tap.ring.get());

            for (const auto& input : strip.inputs)
                if (input.delay != nullptr && input.delay->getLine() != nullptr)
                    rings.insert (input.delay->getLine().get());
        }

        std::int64_t bytes = 0;

        for (const auto* ring : rings)
            bytes += ring->isAllocated() ? (std::int64_t) ring->getCapacity() * 2 * (std::int64_t) sizeof (float) : 0;

        for (const auto numThreads : { 1, 4 })
        {
            engine.setNumThreads (numThreads);
            phazite::playOrStop (song);
            phazite::test::timeEngineBlocks (100, block);
            const auto playing = phazite::test::timeEngineBlocks ((int) (5.0 * rate / block), block);
            phazite::stopAndReturn (song);
            const auto length = block / rate;
            report << "| " << juce::String (rate / 1000.0, 0) << " kHz, blocks of " << block << " | " << (int) rings.size() << " | "
                   << juce::String ((double) bytes / (1024.0 * 1024.0), 1) << " MB | " << numThreads << " | "
                   << juce::String (100.0 * phazite::test::meanOf (playing) / length, 1) << " / "
                   << juce::String (100.0 * phazite::test::percentileOf (playing, 0.99) / length, 1) << " % |\n";
        }
    }

    engine.setNumThreads (threads);
    MESSAGE (report);
}

TEST_CASE ("Benchmark: the benchmark song's stems exported, as it is and with a quarter of its tracks muted"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Each stem a render of one channel as it reaches the master: a render leaves out what it can't hear (what doesn't reach
    // that channel, and a muted track that feeds nothing heard), which it played whole before (known bug 78's review).
    juce::String report ("\n| Stems of the benchmark song, 20 tracks of 16 beats, 48 kHz | Seconds |\n|---|---|\n");

    for (const auto muted : { false, true })
    {
        phazite::test::TestSong s;
        const auto built = phazite::test::buildBenchmarkSong (s, 20, 16.0);
        auto& song = *phazite::findSong (s.context);

        if (muted)
            for (size_t i = 0; i < built.tracks.size(); i += 4)
                phazite::setTrackMuted (song, built.tracks[i], true);

        phazite::test::dispatchPendingMessages();
        phazite::ExportSettings settings;
        settings.stems = true;
        settings.sampleRate = 48000.0;
        std::optional<phazite::ExportReport> done;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto running = phazite::startExport (song, s.getSoundsFolder().getChildFile ("Song.wav"), settings,
                                                   [&done] (const phazite::ExportReport& r) { done = r; });
        REQUIRE (phazite::test::dispatchMessagesUntil ([&done] { return done.has_value(); }, 3600000));
        REQUIRE (done->result.wasOk());
        report << "| " << (muted ? "A quarter of its tracks muted" : "As it is") << " (" << done->stems.size() << " stems) | "
               << juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 2) << " |\n";
    }

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

TEST_CASE ("Benchmark: a song of 100,000 notes made consistent as it opens, and joined from a version's pieces"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // What opening a song costs before anything of it plays (model/project/internal/Project.h: makeConsistent()), and what
    // joining a version's song costs (model/versions/internal/SongPieces.h: joinSong()): the reviewer of known bugs 170 and 179
    // asked what their steps add. 40 tracks of 2,500 notes each, as a project file holds them (its properties text).
    auto made = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*made, 40);

    for (const auto track : tracks)
    {
        const auto clip =
            phazite::test::addMidiClip (*made, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (640.0) }, "Notes");

        for (int step = 0; step < 2500; ++step)
            phazite::test::addMidiNote (*made, clip, 48 + step % 24, phazite::BeatPosition::fromBeats (step * 0.25),
                                        phazite::BeatDuration::fromBeats (0.2), 100);
    }

    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, made->getState());
    const auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    const auto asFile = juce::ValueTree::fromXml (*xml);

    std::vector<double> times;

    for (int i = 0; i < 5; ++i)
    {
        phazite::Song song (asFile.createCopy());
        times.push_back (millisecondsOf ([&] { phazite::makeConsistent (song); }));
    }

    std::sort (times.begin(), times.end());
    juce::String report ("\n");
    report << "makeConsistent() of 100,000 notes, the median of 5: " << milliseconds (times[2], 1) << "\n";

    const auto pieces = phazite::splitSong (asFile);
    const auto read = [&pieces] (const juce::String& name) -> std::optional<juce::MemoryBlock>
    {
        const auto found = pieces.pieces.find (name);
        return found != pieces.pieces.end() ? std::optional (found->second) : std::nullopt;
    };

    times.clear();

    for (int i = 0; i < 5; ++i)
        times.push_back (millisecondsOf ([&] { REQUIRE (phazite::joinSong (pieces.song, read).isValid()); }));

    std::sort (times.begin(), times.end());
    report << "joinSong() of it, the median of 5: " << milliseconds (times[2], 1) << "\n";
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
    // What a clip costs as it plays (docs/archive/dsp-audit.md, 3.1): 64 clips of a stereo file, in memory, on an engine at 48 kHz,
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
    constexpr auto notch = (float) phazite::ui::wheelNotch;     // a mouse wheel's notch, as JUCE reports it
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
    // screen size (docs/archive/performance-audit.md, section 16, options 15 and 16).
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
        playlist->zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (lanes->getParentComponent()->getWidth() / 22.0) });
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
    // without the window's tooltips, then with them, on frosted glass as the main window has them (docs/archive/performance-audit.md,
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
        playlist->zoomTo ({ phazite::BeatPosition(), phazite::BeatPosition::fromBeats (lanes->getParentComponent()->getWidth() / 22.0) });
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
        const auto area =
            juce::Rectangle<int> (0, lanes->getParentComponent()->getY(), playlist->getWidth(), lanes->getHeight()).reduced (8);
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

TEST_CASE ("Benchmark: a device's graph repainted, its curve kept or worked out again" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // Known bug 130: the EQ worked its curves out at each repaint (30 a second while its analyzer moves), the reverb its tail's
    // shape at each of its own. Each graph painted 200 times into a software image (paintTime()): with its values as they
    // were, as it repaints now that it keeps its curve, and with a value changed before each paint, as every repaint worked
    // it out before. And the sampler's sound drawn, a repaint as a voice plays makes it.
    phazite::test::TestSong song;
    constexpr int paints = 200;
    juce::String report = "| Graph | Kept | Worked out again |\n|---|---|---|\n";

    const auto measure = [&report] (const juce::String& name, juce::Component& graph, const std::function<void (int)>& change)
    {
        auto kept = 0.0, again = 0.0;

        for (int i = 0; i < paints; ++i)
        {
            change (-1); // as it was
            kept += paintTime (graph, graph.getLocalBounds());
        }

        for (int i = 0; i < paints; ++i)
        {
            change (i);
            again += paintTime (graph, graph.getLocalBounds());
        }

        report << "| " << name << " | " << milliseconds (kept / paints, 3) << " | " << milliseconds (again / paints, 3) << " |\n";
    };

    {
        const auto id = addDevice (*phazite::findSong (song.context), song.firstTrack, dev::Equalizer::typeID);
        phazite::ui::EqEditor editor (phazite::getDeviceHandle (song.context, id));
        phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto gain = phazite::getParameter (song.context, { id, "band4Gain" });
        measure ("EQ", editor.getGraph(),
                 [&] (int i)
                 {
                     gain.setValue (i < 0 ? 0.0f : (i % 2 == 0 ? 1.0f : 2.0f));
                     editor.refresh();
                 });
    }

    {
        const auto id = addDevice (*phazite::findSong (song.context), song.firstTrack, dev::Reverb::typeID);
        phazite::ui::ReverbEditor editor (phazite::getDeviceHandle (song.context, id));
        phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto decay = phazite::getParameter (song.context, { id, "decay" });
        measure ("Reverb", editor.getGraph(),
                 [&] (int i)
                 {
                     decay.setValue (i < 0 ? 2.0f : (i % 2 == 0 ? 2.5f : 3.0f));
                     editor.refresh();
                 });
    }

    {
        const auto id = song.setInstrument (song.firstTrack, dev::Sampler::typeID);
        phazite::ui::SamplerEditor editor (phazite::getDeviceHandle (song.context, id));
        phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
        auto start = phazite::getParameter (song.context, { id, "start" });
        measure ("Sampler's sound", editor.getSampleView(),
                 [&] (int i)
                 {
                     start.setValue (i < 0 ? 0.0f : (i % 2 == 0 ? 0.1f : 0.2f));
                     editor.refresh();
                 });
    }

    MESSAGE (report);
}

//==============================================================================
TEST_CASE ("Benchmark: what a branch switch and a merge spend on the message thread, by part" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    // What is left of known bug 126: a switch and the start of a merge read whole songs on the message thread. Their parts
    // timed on their own beside the whole: the song left on the branch split into pieces (and written: its pieces are the
    // store's already but its own), a commit's song read, and what is left of the whole (the song made the other's, and
    // what plays it compiled again; the merge's three-way merge).
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 32, 64.0);
    auto& song = *phazite::findSong (s.context);
    auto* history = phazite::getVersionHistory (s.context);
    const auto save = [] (std::function<void (bool)> then) { then (true); };
    const auto commit = [&]
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        std::optional<juce::Result> result;
        REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        return phazite::getLastVersion (s.context)->id;
    };

    commit();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    REQUIRE (phazite::newBranch (s.context, {}, save));
    const auto idea = phazite::getCurrentBranch (s.context)->id;
    phazite::setChannelVolume (song, built.tracks[1], -9.0f);
    const auto theirs = commit();
    REQUIRE (phazite::switchBranch (s.context, main, save));
    phazite::setChannelVolume (song, built.tracks[2], -3.0f);
    commit();
    phazite::setChannelVolume (song, built.tracks[3], -2.0f); // a change left on main as it switches

    const auto time = [] (const std::function<void()>& work)
    {
        const auto start = juce::Time::getMillisecondCounterHiRes();
        work();
        return juce::Time::getMillisecondCounterHiRes() - start;
    };

    const auto split = time ([&] { juce::ignoreUnused (phazite::splitSong (song.getState().createCopy())); });
    const auto read = time ([&] { juce::ignoreUnused (history->readSong (theirs)); });

    auto songsRead = phazite::VersionStore::countSongsRead();
    const auto toIdea = time ([&] { REQUIRE (phazite::switchBranch (s.context, idea, save)); });
    const auto switchReads = phazite::VersionStore::countSongsRead().onMessageThread - songsRead.onMessageThread;
    const auto back = time ([&] { REQUIRE (phazite::switchBranch (s.context, main, save)); });

    songsRead = phazite::VersionStore::countSongsRead();
    const auto merge = time ([&] { REQUIRE (phazite::startMerge (s.context, theirs)); });
    const auto mergeReads = phazite::VersionStore::countSongsRead().onMessageThread - songsRead.onMessageThread;

    MESSAGE ("32 tracks, 64 beats: a switch " << toIdea << " ms (back " << back << " ms), reading " << switchReads
                                              << " songs on the message thread; the song split into pieces " << split
                                              << " ms; a commit's song read " << read << " ms; the start of a merge " << merge
                                              << " ms, reading " << mergeReads << " songs");
}

namespace
{
// The message thread's work the next ticks of the views' timers make: after a timer's period (the mixer's strips tick eight
// times a second), what is due is run and timed, the wait apart.
double nextTicks (int milliseconds = 130)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (milliseconds));
    return millisecondsOf ([] { phazite::test::dispatchPendingMessages(); });
}

// The song's plans not compiled from here on (its session lent, as to a render: PlaybackSession::lendToRender()), so that
// what a benchmark of a view times is the view's work alone: a plan of 200 tracks takes tens of milliseconds of its own
// (known bug 145).
void withoutPlans (phazite::ProjectContext& context)
{
    auto* session = phazite::getSession (context);
    REQUIRE (session != nullptr);
    session->lendToRender (true);
}
} // namespace

TEST_CASE ("Benchmark: the mixer of 200 tracks" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // 200 tracks, each with a synth and an EQ, in a mixer as wide as a window of 1600 pixels: about a dozen strips in view.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 200);

    for (const auto track : tracks)
    {
        addDevice (*song, track, dev::Synth::typeID);
        addDevice (*song, track, dev::Equalizer::typeID);
    }

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, song.get());
    withoutPlans (context);
    phazite::test::dispatchPendingMessages();
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    std::unique_ptr<phazite::ui::MixerView> mixer;
    std::unique_ptr<phazite::test::TestWindow> window;
    juce::String report ("\n| The mixer, 200 tracks, a synth and an EQ on each | The message thread's work |\n|---|---|\n");

    report << "| Opened: its strips made and laid out | "
           << milliseconds (millisecondsOf (
                                [&]
                                {
                                    mixer = std::make_unique<phazite::ui::MixerView> (commands, context);
                                    window = std::make_unique<phazite::test::TestWindow> (*mixer, 1600, 800);
                                    phazite::test::dispatchPendingMessages();
                                }),
                            1)
           << " |\n";

    report << "| Strips made / components in the mixer | " << phazite::test::findAll<phazite::ui::ChannelStrip> (*mixer).size() << " / "
           << phazite::test::findAll<juce::Component> (*mixer).size() << " |\n";

    // At rest: the strips' ticks, eight times a second, which bring their controls' values up to date.
    nextTicks();
    double rest = 0.0;

    for (int i = 0; i < 10; ++i)
        rest += nextTicks();

    report << "| At rest: a tick of the strips (8 a second) | " << milliseconds (rest / 10) << " |\n";

    // A device added on a track in view, and on one out of view: the change heard, then shown at the strips' next tick, then
    // the mixer's outlines a quarter of a second later.
    for (const auto& [what, track] : { std::pair ("in view", tracks[1]), std::pair ("out of view", tracks[150]) })
    {
        nextTicks (400); // what is due before is done
        double heard = 0.0, shown = 0.0;

        for (int i = 0; i < 5; ++i)
        {
            heard += millisecondsOf (
                [&, track = track]
                {
                    addDevice (*song, track, dev::Delay::typeID);
                    phazite::test::dispatchPendingMessages();
                });
            shown += nextTicks();
            nextTicks (300);
        }

        report << "| A device added to a track " << what << ": the change heard | " << milliseconds (heard / 5) << " |\n";
        report << "| ... and shown at the strips' next tick | " << milliseconds (shown / 5) << " |\n";
    }

    // A fader's drag: a step changes the track's volume, which the strips' next tick shows.
    {
        auto volume = phazite::getVolume (context, tracks[1]);
        REQUIRE (volume.exists());
        volume.beginGesture();
        double step = 0.0, shown = 0.0;

        for (int i = 0; i < 10; ++i)
        {
            step += millisecondsOf (
                [&]
                {
                    volume.setValue (0.4f + 0.02f * (float) i);
                    phazite::test::dispatchPendingMessages();
                });
            shown += nextTicks();
        }

        volume.endGesture();
        report << "| A fader's drag: a step | " << milliseconds (step / 10) << " |\n";
        report << "| ... and the strips' next tick | " << milliseconds (shown / 10) << " |\n";
    }

    // Scrolled sideways a strip at a time, as the wheel scrolls it.
    {
        auto* channels = dynamic_cast<juce::Viewport*> (phazite::test::findComponentWithTitle (*mixer, "Channels"));
        REQUIRE (channels != nullptr);
        double scrolling = 0.0;

        for (int i = 1; i <= 20; ++i)
            scrolling += millisecondsOf (
                [&]
                {
                    channels->setViewPosition (i * phazite::ui::ChannelStrip::width, 0);
                    phazite::test::dispatchPendingMessages();
                });

        report << "| Scrolled a strip sideways | " << milliseconds (scrolling / 20) << " |\n";

        // The scroll bar's thumb dragged from one end to the other, in 60 of the mouse's events: each a jump of three strips
        // or more, a whole view's at the most.
        channels->setViewPosition (0, 0);
        phazite::test::dispatchPendingMessages();
        // Each event's own work (what the frame after it waits for), and with what it leaves for the next turns of the message
        // loop (the strips a jump can't make in a part of a frame).
        const auto widest = channels->getViewedComponent()->getWidth() - channels->getViewWidth();
        double total = 0.0, worst = 0.0, withRest = 0.0;

        for (int i = 1; i <= 60; ++i)
        {
            const auto event = millisecondsOf ([&] { channels->setViewPosition (widest * i / 60, 0); });
            withRest += event + millisecondsOf ([] { phazite::test::dispatchPendingMessages(); });
            total += event;
            worst = std::max (worst, event);
        }

        report << "| The scroll bar's thumb dragged end to end, 60 events: each / the worst / each with what it left for later | "
               << milliseconds (total / 60) << " / " << milliseconds (worst) << " / " << milliseconds (withRest / 60) << " |\n";
    }

    MESSAGE (report);
    window = nullptr;
    mixer = nullptr;
    setSong (context, nullptr);
}

TEST_CASE ("Benchmark: the playlist of 200 tracks and 4,800 clips" * doctest::test_suite ("benchmark") * doctest::skip())
{
    // 200 tracks, each with 24 clips of 4 bars and 16 notes: 4,800 clips and 76,800 notes, in a playlist 1600 by 900 pixels.
    phazite::test::PlaylistScreen screen;
    withoutPlans (screen.context);
    screen.window.setSize (1600, 900);
    const auto tracks = phazite::ensureTracks (*screen.song, 200);
    std::vector<std::vector<phazite::ClipID>> clips;

    for (const auto track : tracks)
    {
        auto& onTrack = clips.emplace_back();

        for (int c = 0; c < 24; ++c)
        {
            const auto start = phazite::BeatPosition::fromBeats (c * 16.0);
            const auto clip =
                phazite::test::addMidiClip (*screen.song, track, { start, start + phazite::BeatDuration::fromBeats (16.0) }, "Part");

            for (int n = 0; n < 16; ++n)
                phazite::test::addMidiNote (*screen.song, clip, 48 + (n * 5) % 24, phazite::BeatPosition::fromBeats (n),
                                            phazite::BeatDuration::fromBeats (0.5), 100);

            onTrack.push_back (clip);
        }
    }

    phazite::test::dispatchPendingMessages();
    auto& playlist = screen.playlist;
    juce::String report ("\n| The playlist, 200 tracks, 4,800 clips, 76,800 notes | The message thread's work | Asked to repaint |\n"
                         "|---|---|---|\n");

    const auto measure = [&] (const juce::String& what, int steps, const std::function<void (int)>& step)
    {
        double work = 0.0;
        juce::Rectangle<int> repainted;

        for (int i = 0; i < steps; ++i)
        {
            screen.window.takeRepaintedArea();
            work += millisecondsOf (
                [&]
                {
                    step (i);
                    phazite::test::dispatchPendingMessages();
                });
            repainted = screen.window.takeRepaintedArea().getBounds();
        }

        report << "| " << what << " | " << milliseconds (work / steps) << " | " << repainted.getWidth() << " x " << repainted.getHeight()
               << " px |\n";
    };

    const auto componentCounts = [&]
    {
        return juce::String (phazite::test::findAll<phazite::ui::TrackHeader> (playlist).size()) + " headers, "
               + juce::String (phazite::test::findAll<phazite::ui::ClipComponent> (playlist).size()) + " clips, "
               + juce::String (phazite::test::findAll<juce::Component> (playlist).size()) + " components";
    };

    const auto shown = componentCounts();
    const auto clip = clips[1][1]; // in view
    measure ("A note added to a clip in view", 20,
             [&] (int i)
             {
                 phazite::addNote (screen.context, clip, 72 + i % 12, phazite::BeatPosition::fromBeats (16.0 + 0.25 * i),
                                   phazite::BeatDuration::fromBeats (0.25), 100);
             });
    measure ("A note added to a clip out of view", 20,
             [&] (int i)
             {
                 phazite::addNote (screen.context, clips[150][20], 72 + i % 12, phazite::BeatPosition::fromBeats (320.0 + 0.25 * i),
                                   phazite::BeatDuration::fromBeats (0.25), 100);
             });

    auto beat = phazite::getClipRange (*screen.song, clips[2][0]).getStart();
    measure ("A clip moved a beat, as a drag's step moves it", 20,
             [&] (int)
             {
                 beat = beat + phazite::BeatDuration::fromBeats (1.0);
                 phazite::moveClip (*screen.song, clips[2][0], beat, tracks[2]);
             });

    phazite::test::Mouse mouse (*screen.lanes);
    const auto centre = screen.lanes->getLocalBounds().getCentre();
    constexpr auto notch = (float) phazite::ui::wheelNotch;
    measure ("A wheel notch, scrolling down", 40, [&] (int) { mouse.wheel (centre, -notch); });
    measure ("A wheel notch, scrolling up", 40, [&] (int) { mouse.wheel (centre, notch); });
    {
        // The rows' scroll bar's thumb dragged from the top to the bottom, in 60 of the mouse's events.
        juce::ScrollBar* rowsBar = nullptr;

        for (auto* bar : phazite::test::findAll<juce::ScrollBar> (playlist))
            if (bar->isVertical() && bar->getParentComponent() == &playlist)
                rowsBar = bar;

        REQUIRE (rowsBar != nullptr);
        rowsBar->setCurrentRangeStart (0.0, juce::sendNotificationSync);
        phazite::test::dispatchPendingMessages();
        const auto end = rowsBar->getMaximumRangeLimit() - rowsBar->getCurrentRangeSize();
        double total = 0.0, worst = 0.0, withRest = 0.0;

        for (int i = 1; i <= 60; ++i)
        {
            const auto event = millisecondsOf ([&] { rowsBar->setCurrentRangeStart (end * i / 60.0, juce::sendNotificationSync); });
            withRest += event + millisecondsOf ([] { phazite::test::dispatchPendingMessages(); });
            total += event;
            worst = std::max (worst, event);
        }

        report << "| The rows' scroll bar's thumb dragged top to bottom, 60 events: each / the worst / each with what it left for later | "
               << milliseconds (total / 60) << " / " << milliseconds (worst) << " / " << milliseconds (withRest / 60) << " | |\n";
        rowsBar->setCurrentRangeStart (0.0, juce::sendNotificationSync);
        phazite::test::dispatchPendingMessages();
    }

    measure ("A track renamed", 10, [&] (int i) { phazite::renameTrack (screen.context, tracks[3], "Renamed " + juce::String (i)); });
    measure ("A track added at the end", 10, [&] (int) { phazite::addTrackAtEnd (screen.context, false); });

    report << "\nShown: " << shown << "\n";
    MESSAGE (report);
}

//==============================================================================
TEST_CASE ("Benchmark: a clip of 5,000 notes edited in the piano roll, and what views read of a song of 4,800 clips"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // Known bug 122: notes and clips were found by looking through the song at every call, so that editing many notes cost
    // the square of them. Each made through the window, as the mouse and the keys make it, the song's changes delivered after.
    using phazite::test::at;
    using phazite::test::lasting;

    const auto averageOf = [] (int times, const std::function<void (int)>& work)
    {
        double total = 0.0;

        for (int i = 0; i < times; ++i)
            total += millisecondsOf ([&] { work (i); });

        return total / times;
    };

    juce::String report ("\n| A clip of 5,000 notes, with another track's 5,000 as ghosts | Time |\n|---|---|\n");

    {
        phazite::test::PianoRollScreen screen;
        auto& song = screen.getSong();
        phazite::addNoteCommands (screen.registry, screen.context, [] { return 1.0; });
        constexpr int count = 5000;
        const auto length = count * 0.25;
        const auto middle = screen.middlePitch();
        phazite::trimClipEnd (song, screen.clip, at (length));
        const auto ghostClip = phazite::insertMidiClip (song, screen.tracks[1], { at (0.0), at (length) }, nullptr);

        // Four a beat, just off the grid (for Quantize), within six rows of the middle.
        const auto notesOf = [&] (phazite::ClipID clip, int offset)
        {
            std::vector<phazite::NoteData> made;

            for (int i = 0; i < count; ++i)
                made.push_back (
                    { .clip = clip, .pitch = middle - 6 + (i * 7 + offset) % 12, .start = at (i * 0.25 + 0.03), .length = lasting (0.2) });

            return made;
        };

        phazite::setClipNotes (song, screen.clip, notesOf (screen.clip, 0));
        phazite::setClipNotes (song, ghostClip, notesOf (ghostClip, 5));
        song.getUndoManager().clearUndoHistory();
        phazite::test::deliverChanges();
        const auto notes = screen.getAll();
        REQUIRE (notes.size() == (size_t) count);

        const auto run = [&] (const char* id)
        {
            REQUIRE (screen.registry.invoke (id));
            phazite::test::deliverChanges();
        };

        // The mouse over the whole clip: on the notes' rows, and above them, where only ghosts could be.
        screen.pianoRoll.zoomTo ({ at (0.0), at (length) });
        phazite::test::deliverChanges();
        phazite::test::Mouse mouse (*screen.notes);
        const auto width = screen.notes->getWidth();
        const auto across = [width] (int i, int of) { return 10 + i * (width - 20) / of; };
        report << "| The mouse moved over the notes (hover) | "
               << milliseconds (averageOf (50, [&] (int i) { mouse.moveTo ({ across (i, 50), screen.yOf (middle) }); }), 3) << " |\n";
        report << "| ... over empty rows, where ghosts are looked for | "
               << milliseconds (averageOf (50, [&] (int i) { mouse.moveTo ({ across (i, 50), screen.yOf (middle + 9) }); }), 3) << " |\n";

        // A rectangle drawn across the clip (Ctrl+drag), from empty rows down over every note.
        mouse.press ({ 2, screen.yOf (middle + 9) }, juce::ModifierKeys::commandModifier);
        const auto band = averageOf (20,
                                     [&] (int i)
                                     {
                                         mouse.dragTo ({ across (i + 1, 20), screen.yOf (middle - 8) });
                                         phazite::test::deliverChanges();
                                     });
        report << "| A step of a rectangle selecting across the clip | " << milliseconds (band) << " |\n";
        mouse.release();
        phazite::test::deliverChanges();

        // 500 of them dragged a beat at a time, by the note in the middle row (the 7th).
        screen.pianoRoll.zoomTo ({ at (0.0), at (16.0) });
        phazite::test::deliverChanges();
        screen.selection().selectNotes (getRefs (std::vector<phazite::NoteData> (notes.begin(), notes.begin() + 500)));
        phazite::test::deliverChanges();
        const auto pressed = notes[6].getRef();
        REQUIRE (screen.get (pressed).pitch == middle);
        const auto from = screen.middleOf (pressed);
        mouse.press (from);
        mouse.dragTo (from.translated (5, 0)); // past the drag's threshold, by less than half a beat
        const auto step = averageOf (10,
                                     [&] (int i)
                                     {
                                         mouse.dragTo (from.translated (screen.widthOf (i + 1.0), 0));
                                         phazite::test::deliverChanges();
                                     });
        report << "| A step of a drag of 500 notes | " << milliseconds (step) << " |\n";
        mouse.release();
        phazite::test::deliverChanges();
        CHECK (screen.get (pressed).start == notes[6].start + lasting (10.0));
        report << "| ... its Undo | " << milliseconds (millisecondsOf ([&] { run ("edit.undo"); })) << " |\n";

        // Every note: selected, then each edit and its Undo.
        report << "| Select All | " << milliseconds (millisecondsOf ([&] { run ("note.selectAll"); })) << " |\n";
        REQUIRE (screen.getSelected().size() == (size_t) count);

        for (const auto& [id, what] :
             { std::pair { "note.transposeUp", "Transpose Up (the up arrow)" }, std::pair { "edit.quantize", "Quantize" },
               std::pair { "edit.duplicate", "Duplicate" }, std::pair { "edit.delete", "Delete" } })
        {
            run ("note.selectAll");
            report << "| " << what << ", every note | " << milliseconds (millisecondsOf ([&] { run (id); })) << " |\n";
            report << "| ... its Undo | " << milliseconds (millisecondsOf ([&] { run ("edit.undo"); })) << " |\n";
        }

        CHECK (screen.getAll().size() == (size_t) count);
    }

    // What the playlist reads of a song of 200 tracks of 24 clips of 16 notes each.
    {
        auto song = phazite::createSong();
        phazite::ProjectContext context (phazite::test::getEngineContext());
        const auto tracks = phazite::ensureTracks (*song, 200);
        std::vector<phazite::ClipID> clips;

        for (const auto track : tracks)
        {
            for (int c = 0; c < 24; ++c)
            {
                const auto clip = phazite::insertMidiClip (*song, track, { at (c * 16.0), at (c * 16.0 + 16.0) }, nullptr);
                std::vector<phazite::NoteData> clipNotes;

                for (int n = 0; n < 16; ++n)
                    clipNotes.push_back ({ .clip = clip, .pitch = 48 + n, .start = at (c * 16.0 + n), .length = lasting (0.5) });

                phazite::setClipNotes (*song, clip, clipNotes);
                clips.push_back (clip);
            }
        }

        song->getUndoManager().clearUndoHistory();
        phazite::setSong (context, song.get());
        phazite::test::dispatchPendingMessages();

        report << "\n| What views read of 200 tracks of 24 clips of 16 notes | Time |\n|---|---|\n";
        const auto everyTrack = millisecondsOf (
            [&]
            {
                for (const auto track : tracks)
                    REQUIRE (phazite::getClips (context, track).size() == 24);
            });
        report << "| Every track's clips (getClips()) | " << milliseconds (everyTrack) << " |\n";
        report << "| A clip's info by its ID (getClip()) | "
               << milliseconds (averageOf (480, [&] (int i) { REQUIRE (phazite::getClip (context, clips[(size_t) i * 10]).has_value()); }),
                                4)
               << " |\n";
        // A lookup alone, by the song's index, with its check of the answer against the tree (SongIndex.h).
        const auto lookups = millisecondsOf (
            [&]
            {
                for (int i = 0; i < 100000; ++i)
                    if (! phazite::findClipState (*song, clips[(size_t) (i * 7919) % clips.size()]).isValid())
                        FAIL ("a clip not found");
            });
        report << "| A clip found by its ID (findClipState()) | " << juce::String (lookups * 10.0, 1) << " ns |\n";
        report << "| A track's row (getTrackRow()) | "
               << milliseconds (averageOf (200, [&] (int i) { REQUIRE (phazite::getTrackRow (context, tracks[(size_t) i]).has_value()); }),
                                4)
               << " |\n";
        report << "| A clip's note shapes (getNoteShapes()) | "
               << milliseconds (
                      averageOf (480, [&] (int i) { REQUIRE (phazite::getNoteShapes (context, clips[(size_t) i * 10]).size() == 16); }), 4)
               << " |\n";
        report << "| A clip moved, as a drag's step moves it (the model alone) | "
               << milliseconds (
                      averageOf (20, [&] (int i) { phazite::moveClip (context, clips[2400], at (24 * 16.0 + i * 4.0), tracks[100]); }), 4)
               << " |\n";
        phazite::setSong (context, nullptr);
    }

    MESSAGE (report);
}
