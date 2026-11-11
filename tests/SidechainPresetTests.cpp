// SPDX-License-Identifier: AGPL-3.0-or-later
// The sidechain presets as they duck (resources/presets/Compressor/Sidechain; docs/presets.md): Duck Under's own setting
// under every factory kit's kick, and Sidechain Full Pump and Deep Pump under Exodus's kick against the original song's two
// buses. Apart from FactoryPresetTests, which checks every preset, so that each file keeps within CTest's time limit in
// CI's slowest builds (linux-asan ran the two together past 600 s).
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dsp/Random.h"
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
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <array>
#include <cmath>
#include <string>

namespace
{
namespace devices = phazite::devices;
namespace keys = phazite::schema::preset;
constexpr double testRate = 48000.0;
constexpr int testBlockSize = 256;

juce::String toString (std::string_view text)
{
    return juce::String (text.data(), text.size());
}

juce::File getFactoryPresets()
{
    return phazite::test::getFactoryPresetsFolder();
}

// A preset's values: each parameter it names, the others at their defaults (as FactoryPresetTests reads them).
std::vector<float> readValues (const devices::DeviceInfo& info, const juce::var& device)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    // A choice by its name, any other value by its number, as Phazite reads a preset.
    if (const auto* set = device[keys::parameters].getDynamicObject())
        for (const auto& [id, value] : set->getProperties())
            if (const auto index = devices::findParameter (info, id.toString().toStdString()); index >= 0)
                values[(size_t) index] =
                    devices::constrain (info.parameters[(size_t) index], phazite::readSavedValue (info.parameters[(size_t) index], value));

    return values;
}

// A drum sampler prepared, playing a kit's sounds, as FactoryPresetTests loads a kit's preset (these tests load kits alone).
std::unique_ptr<devices::Processor> makeLoaded (const devices::DeviceInfo&, const juce::var& sounds)
{
    auto drums = std::make_unique<devices::DrumSampler>();
    drums->prepare (testRate, testBlockSize);

    for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
        if (const auto name = sounds[phazite::schema::getPadSound (pad)].toString(); name.isNotEmpty())
            drums->setSound (pad, devices::getFactoryDrum (devices::findFactoryDrum (name.toStdString())));

    return drums;
}
} // namespace

namespace
{
// A drum sampler with a kit's sounds playing a pad on every beat at 120 bpm, at velocity 100, for some beats.
juce::AudioBuffer<float> playOnBeats (const devices::DeviceInfo& info, const std::vector<float>& values, const juce::var& sounds, int pad,
                                      int beats)
{
    auto processor = makeLoaded (info, sounds);
    const auto beatSamples = (int) (0.5 * testRate), noteSamples = (int) (0.1 * testRate), numSamples = beats * beatSamples;
    juce::AudioBuffer<float> played (2, numSamples), buffer (2, testBlockSize);

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);
        std::vector<devices::NoteEvent> events;

        for (int beat = 0; beat < beats; ++beat)
        {
            if (const auto on = beat * beatSamples; on >= done && on < done + length)
                events.push_back ({ devices::NoteEvent::Type::noteOn, on - done, pad, 100.0f / 127.0f });

            if (const auto off = beat * beatSamples + noteSamples; off >= done && off < done + length)
                events.push_back ({ devices::NoteEvent::Type::noteOff, off - done, pad, 0.0f });
        }

        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        block.events = events;
        block.isPlaying = true;
        processor->process (block);

        for (int channel = 0; channel < 2; ++channel)
            played.copyFrom (channel, done, buffer, channel, 0, length);
    }

    return played;
}

// A steady tone through Phazite's compressor at its values, under a key on every beat at 120 bpm: after each beat but the
// first, how far it went down at the deepest and how long after the beat it was back within 3 dB (a beat, 500 ms, if it
// wasn't), in steps of 10 ms; the average depth and the longest time.
struct Ducked
{
    float depth = 0.0f, backWithin3 = 0.0f;
};

Ducked duckUnderKey (const std::vector<float>& values, const juce::AudioBuffer<float>& key)
{
    const auto numSamples = key.getNumSamples();
    juce::AudioBuffer<float> tone (2, numSamples), out (2, numSamples);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            tone.setSample (channel, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / testRate));

    out.makeCopyOf (tone);
    devices::Compressor compressor;
    compressor.prepare (testRate, testBlockSize);

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        float* channels[] = { out.getWritePointer (0, done), out.getWritePointer (1, done) };
        const float* sidechain[] = { key.getReadPointer (0, done), key.getReadPointer (1, done) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = std::min (testBlockSize, numSamples - done);
        block.sidechain = sidechain;
        block.numSidechainChannels = 2;
        block.parameters = values;
        block.isPlaying = true;
        compressor.process (block);
    }

    constexpr int hop = 480, hopsPerBeat = 50;
    const auto reductionAt = [&] (int index)
    {
        const auto in = tone.getRMSLevel (0, index * hop, hop), heard = out.getRMSLevel (0, index * hop, hop);
        return juce::Decibels::gainToDecibels (in) - juce::Decibels::gainToDecibels (heard, -200.0f);
    };

    Ducked ducked;
    const auto beats = numSamples / (hop * hopsPerBeat);

    for (int beat = 1; beat < beats; ++beat)
    {
        auto deepest = 0.0f;
        auto deepestAt = 0;

        for (int step = 0; step < hopsPerBeat; ++step)
        {
            if (const auto down = reductionAt (beat * hopsPerBeat + step); down > deepest)
            {
                deepest = down;
                deepestAt = step;
            }
        }

        auto back = 500.0f;

        for (auto step = deepestAt; step < hopsPerBeat; ++step)
        {
            if (reductionAt (beat * hopsPerBeat + step) <= 3.0f)
            {
                back = (float) step * 10.0f;
                break;
            }
        }

        ducked.depth += deepest / (float) (beats - 1);
        ducked.backWithin3 = std::max (ducked.backWithin3, deepest > 3.0f ? back : 0.0f);
    }

    return ducked;
}
} // namespace

TEST_CASE ("Duck Under pumps under every factory kit's kick at 120 BPM, and no kit's snare, clap, hats or cymbals move it")
{
    // What one click on Duck Under sets a compressor to under a drum sampler's track, as the song holds it.
    const auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    REQUIRE (phazite::setInstrument (*song, tracks[0], phazite::makeOwnDeviceState (*song, devices::DrumSampler::typeID)).isValid());
    const auto ducker = phazite::duckUnder (*song, tracks[1], tracks[0]);
    REQUIRE (ducker.isValid());
    std::vector<float> duck;

    for (const auto& parameter : devices::Compressor::info.parameters)
        duck.push_back (phazite::getValueSetByHand (*song, phazite::ParameterRef { ducker, toString (parameter.id) }));

    REQUIRE (devices::Compressor::getMode (duck[devices::Compressor::mode]) == devices::Compressor::Mode::duck);

    // Each kit: the default kit, as a new drum sampler plays it, then every kit that comes with Phazite.
    struct Kit
    {
        juce::String name;
        const devices::DeviceInfo* info;
        std::vector<float> values;
        juce::var sounds;
    };

    std::vector<Kit> kits { { "Default Kit", &devices::DrumSampler::info, readValues (devices::DrumSampler::info, {}), {} } };
    auto files = getFactoryPresets().getChildFile ("Drum Sampler/Kits").findChildFiles (juce::File::findFiles, false, "*.phazitepreset");
    files.sort();

    for (const auto& file : files)
    {
        const auto device = phazite::readPreset (file)[keys::device];
        kits.push_back ({ file.getFileNameWithoutExtension(), &devices::DrumSampler::info, readValues (devices::DrumSampler::info, device),
                          device[keys::sounds] });
    }

    REQUIRE (kits.size() == 13);
    constexpr int beats = 5;

    for (const auto& kit : kits)
    {
        CAPTURE (kit.name.toStdString());
        const auto pumped = duckUnderKey (duck, playOnBeats (*kit.info, kit.values, kit.sounds, devices::DrumSampler::kitFirstNote, beats));
        MESSAGE (kit.name << ": " << pumped.depth << " dB down, back within 3 dB in " << pumped.backWithin3 << " ms");

        // A kick's pump, back well before the next beat, about as deep under each (but for the percussion kit's conga, which no
        // kick is): the deepest under the kits with the loudest kicks, the least under those whose kicks are quietest in the key's
        // band, and over 10 dB under the default kit's, the 808's and the 909's, which the defaults favour.
        if (kit.name != "Percussion Kit")
        {
            CHECK (pumped.depth >= 5.0f);
            CHECK (pumped.depth <= 15.0f);
            CHECK (pumped.backWithin3 >= 150.0f);
            CHECK (pumped.backWithin3 <= 300.0f);

            if (kit.name == "Default Kit" || kit.name == "808 Kit" || kit.name == "909 Kit")
                CHECK (pumped.depth >= 10.0f);
        }

        // The rest of a whole kit on the same track doesn't move it: its snares, clap, hats and cymbals.
        for (const auto pad : { 38, 39, 40, 42, 44, 46, 49, 51 })
        {
            CAPTURE (pad);
            CHECK (duckUnderKey (duck, playOnBeats (*kit.info, kit.values, kit.sounds, pad, beats)).depth < 0.5f);
        }
    }
}

TEST_CASE ("Sidechain Full Pump and Deep Pump duck a sound under Exodus's kick as the original song's two buses do")
{
    // The original Exodus's pumps, measured on its recording's buses: how far down at the deepest and when, and when back within
    // 12, 6, 3 and 1 dB, in milliseconds from the kick. The Exodus example song keys both by its Kick track, the Dark
    // Synthwave Kit's kick at velocity 100, at 114 BPM.
    struct Pump
    {
        std::string preset;
        double deepestAt;
        float deepest;
        std::array<double, 4> backAt;
    };

    for (const auto& pump : { Pump { "Sidechain Full Pump", 23.0, -33.9f, { 175.0, 209.0, 226.0, 244.0 } },
                              Pump { "Sidechain Deep Pump", 22.0, -28.1f, { 149.0, 189.0, 206.0, 220.0 } } })
    {
        CAPTURE (pump.preset);

        // A steady 500 Hz tone, whose level a window of 2 ms (a cycle at 48 kHz) reads alike wherever it starts, through the
        // preset, under the kick on every beat from the third, muted as a sidechain still hears it, its fader as Exodus sets it.
        phazite::test::TestSong song;
        song.setTempo (114.0);
        const auto tone = song.firstTrack;
        song.rename (tone, "Tone");
        song.addAudioClip (tone, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 8.0, 500.0, 0.25f), 0.0);
        const auto kick = song.addTrack ("Kick");
        const auto drums = song.setInstrument (kick, devices::DrumSampler::typeID);
        REQUIRE (phazite::loadPresetInto (song.context, drums,
                                          getFactoryPresets().getChildFile ("Drum Sampler/Kits/Dark Synthwave Kit.phazitepreset")));
        constexpr int firstKick = 2, kicks = 8;
        const auto clip = song.addMidiClip (kick, 0.0, firstKick + kicks + 1.0);

        for (int beat = firstKick; beat < firstKick + kicks; ++beat)
            song.addNote (clip, devices::DrumSampler::kitFirstNote, beat, 0.25, 100);

        song.setVolume (kick, 0.1f);
        phazite::setTrackMuted (*phazite::findSong (song.context), kick, true);
        const auto ducker = phazite::duckUnder (song.context, tone, kick);
        REQUIRE (ducker.isValid());
        REQUIRE (phazite::loadPresetInto (song.context, ducker,
                                          getFactoryPresets()
                                              .getChildFile ("Compressor/Sidechain")
                                              .getChildFile (juce::String (pump.preset) + phazite::presetExtension)));
        REQUIRE (phazite::getDeviceHandle (song.context, ducker).getParameter ("mode").getValue()
                 == doctest::Approx ((float) devices::Compressor::Mode::duck));

        phazite::test::EnginePlayback playback (phazite::test::EnginePlayback::Input::silence,
                                                phazite::test::EnginePlayback::Timing::asRender);
        phazite::test::EnginePlayback::waitForFiles();
        song.run ("transport.playStop");
        const auto beatSeconds = 60.0 / 114.0;
        const auto played = playback.play ((firstKick + kicks + 0.5) * beatSeconds);
        song.run ("transport.playStop");

        // The tone's level in 2 ms around a time, in decibels.
        constexpr auto rate = phazite::test::EnginePlayback::sampleRate;
        const auto levelAt = [&played] (double seconds)
        {
            const auto from = (int) std::lround ((seconds - 0.001) * rate);
            return juce::Decibels::gainToDecibels (played.getRMSLevel (0, from, (int) std::lround (0.002 * rate)), -200.0f);
        };

        // Each kick after the first, settled: its gain curve against the tone's level just before it, averaged.
        std::array<double, 5> gains {};
        auto deepest = 0.0, deepestAt = 0.0;

        for (int beat = firstKick + 1; beat < firstKick + kicks; ++beat)
        {
            const auto at = beat * beatSeconds;
            const auto before = levelAt (at - 0.01);
            const std::array times { pump.deepestAt, pump.backAt[0], pump.backAt[1], pump.backAt[2], pump.backAt[3] };

            for (size_t n = 0; n < times.size(); ++n)
                gains[n] += (levelAt (at + times[n] / 1000.0) - before) / (kicks - 1);

            for (auto ms = 1.0; ms < 300.0; ms += 1.0)
            {
                if (const auto gain = levelAt (at + ms / 1000.0) - before; gain < deepest)
                {
                    deepest = gain;
                    deepestAt = ms;
                }
            }
        }

        // Within 1.5 dB, RMS, of the original's numbers.
        const std::array expected { (double) pump.deepest, -12.0, -6.0, -3.0, -1.0 };
        auto squares = 0.0;

        for (size_t n = 0; n < expected.size(); ++n)
            squares += (gains[n] - expected[n]) * (gains[n] - expected[n]);

        const auto error = std::sqrt (squares / (double) expected.size());
        MESSAGE (pump.preset << ": deepest " << deepest << " dB at " << deepestAt << " ms; at " << pump.deepestAt << " ms " << gains[0]
                             << " dB; at " << pump.backAt[0] << ", " << pump.backAt[1] << ", " << pump.backAt[2] << " and "
                             << pump.backAt[3] << " ms " << gains[1] << ", " << gains[2] << ", " << gains[3] << " and " << gains[4]
                             << " dB: " << error << " dB RMS from the original");
        CHECK (error < 1.5);
        CHECK (deepest == doctest::Approx (pump.deepest).epsilon (0.05));
    }
}
