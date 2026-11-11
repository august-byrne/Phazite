// SPDX-License-Identifier: AGPL-3.0-or-later
// The presets that come with Phazite (resources/presets; docs/design.md, section 9): each one a device's, in Phazite's own
// format (model/base/internal/Schema.h: schema::preset), in its device's folder, naming every parameter the device has
// and no other, within their ranges, and tables it has, written as Save Preset writes them; and each one playing: an
// instrument a chord and a bass note, an effect a beat and a tone, coming out with sound, without anything that isn't a
// number, and without going far over full scale; and the instruments leveled alike, none of their notes or chords
// peaking above -1 dBFS.
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

juce::Array<juce::File> getPresetFiles()
{
    auto files = getFactoryPresets().findChildFiles (juce::File::findFiles, true, juce::String ("*") + phazite::presetExtension);
    files.sort();
    return files;
}

const devices::DeviceInfo* findDevice (const juce::String& type)
{
    for (const auto* info : phazite::getOwnDevices())
        if (toString (info->typeID) == type)
            return info;

    return nullptr;
}

std::unique_ptr<devices::Processor> makeProcessor (const devices::DeviceInfo& info)
{
    const auto type = toString (info.typeID);

    if (type == devices::Synth::typeID)
        return std::make_unique<devices::Synth>();
    if (type == devices::WavetableSynth::typeID)
        return std::make_unique<devices::WavetableSynth>();
    if (type == devices::FmSynth::typeID)
        return std::make_unique<devices::FmSynth>();
    if (type == devices::Sampler::typeID)
        return std::make_unique<devices::Sampler>();
    if (type == devices::DrumSampler::typeID)
        return std::make_unique<devices::DrumSampler>();
    if (type == devices::Equalizer::typeID)
        return std::make_unique<devices::Equalizer>();
    if (type == devices::Compressor::typeID)
        return std::make_unique<devices::Compressor>();
    if (type == devices::Limiter::typeID)
        return std::make_unique<devices::Limiter>();
    if (type == devices::Multiband::typeID)
        return std::make_unique<devices::Multiband>();
    if (type == devices::Reverb::typeID)
        return std::make_unique<devices::Reverb>();
    if (type == devices::Delay::typeID)
        return std::make_unique<devices::Delay>();
    if (type == devices::Saturator::typeID)
        return std::make_unique<devices::Saturator>();
    if (type == devices::Exciter::typeID)
        return std::make_unique<devices::Exciter>();
    if (type == devices::Vinyl::typeID)
        return std::make_unique<devices::Vinyl>();
    if (type == devices::FilterEffect::typeID)
        return std::make_unique<devices::FilterEffect>();
    if (type == devices::Phaser::typeID)
        return std::make_unique<devices::Phaser>();
    if (type == devices::Flanger::typeID)
        return std::make_unique<devices::Flanger>();
    if (type == devices::Chorus::typeID)
        return std::make_unique<devices::Chorus>();
    if (type == devices::StereoExpander::typeID)
        return std::make_unique<devices::StereoExpander>();
    if (type == devices::Vocoder::typeID)
        return std::make_unique<devices::Vocoder>();

    return {};
}

struct Played
{
    float peak = 0.0f;
    double rms = 0.0; // over the first second
    bool finite = true;
};

// A device for a preset, prepared, playing the preset's tables and sounds.
std::unique_ptr<devices::Processor> makeLoaded (const devices::DeviceInfo& info, const juce::var& sounds)
{
    auto processor = makeProcessor (info);
    REQUIRE (processor != nullptr);
    processor->prepare (testRate, testBlockSize);

    if (auto* wavetable = dynamic_cast<devices::WavetableSynth*> (processor.get()))
        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
            if (const auto name = sounds[phazite::schema::getOscillatorWavetable (osc)].toString(); name.isNotEmpty())
                wavetable->setTable (osc, devices::getFactoryWavetable (devices::findFactoryWavetable (name.toStdString())));

    if (auto* drums = dynamic_cast<devices::DrumSampler*> (processor.get()))
        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            if (const auto name = sounds[phazite::schema::getPadSound (pad)].toString(); name.isNotEmpty())
                drums->setSound (pad, devices::getFactoryDrum (devices::findFactoryDrum (name.toStdString())));

    if (auto* sampler = dynamic_cast<devices::Sampler*> (processor.get()))
        if (const auto name = sounds[phazite::schema::samplerSample].toString(); name.isNotEmpty())
            sampler->setSound (devices::getFactorySample (devices::findFactorySample (name.toStdString())));

    return processor;
}

// Plays a device with a preset's values for a second and a half: an instrument a chord and a bass note for half a second
// (a drum sampler each of its kit's pads in turn, a 32nd note at 120 bpm apart, as a busy beat plays them), an effect a beat of
// noise bursts over a low tone, with what went in mixed back as its mix says, as Phazite's engine plays it
// (engine/nodes/DeviceRunner.h).
Played play (const devices::DeviceInfo& info, const std::vector<float>& values, const juce::var& sounds)
{
    auto processor = makeLoaded (info, sounds);
    const auto isInstrument = info.kind == devices::DeviceKind::instrument;
    const auto isDrums = toString (info.typeID) == devices::DrumSampler::typeID;
    const auto numSamples = (int) (1.5 * testRate), release = (int) (0.5 * testRate);
    const auto padSpacing = (int) (testRate / 16.0);
    const auto mixIndex = devices::findParameter (info, devices::mixParameterID);
    const auto mix = mixIndex >= 0 ? values[(size_t) mixIndex] : 1.0f;
    juce::AudioBuffer<float> buffer (2, testBlockSize), dry (2, testBlockSize);
    devices::dsp::Random random (7);
    Played played;
    double energy = 0.0;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        std::vector<devices::NoteEvent> events;

        if (isDrums)
        {
            for (int index = 0; index < devices::DrumSampler::kitPads; ++index)
                if (const auto at = index * padSpacing; at >= done && at < done + testBlockSize)
                    events.push_back ({ devices::NoteEvent::Type::noteOn, at - done, devices::DrumSampler::kitFirstNote + index, 0.8f });
        }
        else if (isInstrument && done == 0)
        {
            for (const auto note : { 36, 48, 52, 55 })
                events.push_back ({ devices::NoteEvent::Type::noteOn, 0, note, 0.8f });
        }

        if (isInstrument && done <= release && done + testBlockSize > release)
            for (const auto note : { 36, 48, 52, 55 })
                events.push_back ({ devices::NoteEvent::Type::noteOff, release - done, note, 0.0f });

        for (int i = 0; i < testBlockSize; ++i)
        {
            const auto t = done + i;
            const auto burst = (t % 6000) < 600 ? 0.5f * random.nextBipolar() : 0.0f;
            const auto tone = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * t / testRate);

            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, isInstrument ? 0.0f : burst + tone);
        }

        dry.makeCopyOf (buffer, true);
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        float* dryChannels[] = { dry.getWritePointer (0), dry.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = testBlockSize;
        block.dry = mixIndex >= 0 ? dryChannels : nullptr;
        block.parameters = values;
        block.events = events;
        processor->process (block);

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < testBlockSize; ++i)
            {
                const auto sample = mix * buffer.getSample (channel, i) + (1.0f - mix) * dry.getSample (channel, i);
                played.finite = played.finite && std::isfinite (sample);
                played.peak = std::max (played.peak, std::abs (sample));

                if (done + i < (int) testRate)
                    energy += (double) sample * sample;
            }
        }
    }

    played.rms = std::sqrt (energy / (2.0 * testRate));
    return played;
}

// A preset's values: each parameter it names, the others at their defaults.
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

// An instrument playing notes at velocity 100 for a while, then three seconds more: how high it peaks, and its loudest
// momentary loudness (EBU R128, 400 ms), as the engine's meter finds it.
struct Heard
{
    float peak = 0.0f, loudness = -99.0f;
};

Heard playNotes (const devices::DeviceInfo& info, const std::vector<float>& values, const juce::var& sounds,
                 std::initializer_list<int> notes, double seconds)
{
    auto processor = makeLoaded (info, sounds);
    phazite::engine::LoudnessMeter meter;
    meter.prepare (testRate, testBlockSize);
    const auto release = (int) (seconds * testRate), numSamples = release + (int) (3.0 * testRate);
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    Heard heard;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        std::vector<devices::NoteEvent> events;

        for (const auto note : notes)
        {
            if (done == 0)
                events.push_back ({ devices::NoteEvent::Type::noteOn, 0, note, 0.8f });

            if (done <= release && done + testBlockSize > release)
                events.push_back ({ devices::NoteEvent::Type::noteOff, release - done, note, 0.0f });
        }

        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = testBlockSize;
        block.parameters = values;
        block.events = events;
        processor->process (block);
        heard.peak = std::max (heard.peak, buffer.getMagnitude (0, testBlockSize));
        const float* read[] = { buffer.getReadPointer (0), buffer.getReadPointer (1) };
        meter.process (read, 2, testBlockSize);

        if (const auto momentary = meter.getReadings().momentary)
            heard.loudness = std::max (heard.loudness, *momentary);
    }

    return heard;
}
} // namespace

TEST_CASE ("Phazite's presets: each a device's, in its folder, with parameters and tables the device has, in their ranges")
{
    const auto files = getPresetFiles();
    REQUIRE (files.size() > 50);
    std::map<juce::String, int> perDevice;

    for (const auto& file : files)
    {
        CAPTURE (file.getRelativePathFrom (getFactoryPresets()).toStdString());
        const auto preset = phazite::readPreset (file);
        REQUIRE (preset.isObject());
        CHECK (preset[keys::kind].toString() == juce::String (keys::deviceKind));

        const auto device = preset[keys::device];
        const auto* info = findDevice (device[keys::type].toString());
        REQUIRE (info != nullptr);
        CHECK (device[keys::name].toString() == toString (info->name));
        CHECK ((bool) device[keys::instrument] == (info->kind == devices::DeviceKind::instrument));
        ++perDevice[toString (info->name)];

        // In its device's folder, in one of a kind of sound: presets/<device>/<kind>/<name>.
        CHECK (file.getParentDirectory().getParentDirectory().getFileName() == toString (info->name));
        CHECK (file.getParentDirectory().getParentDirectory().getParentDirectory() == getFactoryPresets());

        // Only what the preset format has, and only parameters the device has, in their ranges.
        for (const auto& [key, value] : device.getDynamicObject()->getProperties())
            CHECK ((key == keys::type || key == keys::name || key == keys::instrument || key == keys::parameters || key == keys::sounds));

        const auto* values = device[keys::parameters].getDynamicObject();
        REQUIRE (values != nullptr);

        for (const auto& [id, value] : values->getProperties())
        {
            CAPTURE (id.toString().toStdString());
            const auto index = devices::findParameter (*info, id.toString().toStdString());
            REQUIRE (index >= 0);
            const auto& parameter = info->parameters[(size_t) index];

            // A choice by its name, one of the device's choices; any other value a number.
            if (parameter.unit == devices::Unit::choice)
            {
                CHECK (value.isString());
                CHECK (devices::findChoice (parameter, value.toString().toStdString()).has_value());
                continue;
            }

            CHECK ((value.isDouble() || value.isInt() || value.isInt64()));
            CHECK ((float) value >= parameter.minimum);
            CHECK ((float) value <= parameter.maximum);
            CHECK (devices::constrain (parameter, (float) value) == doctest::Approx ((float) value));
        }

        // And every parameter the device has, so that loading the preset sets the whole device, as a preset the user
        // saves does.
        CHECK (values->getProperties().size() == (int) info->parameters.size());

        // A wavetable synth's tables, a drum sampler's pads' sounds and a sampler's sound by their names; no sound files,
        // which aren't in Phazite's repository.
        if (const auto* sounds = device[keys::sounds].getDynamicObject())
        {
            const auto isDrums = toString (info->typeID) == devices::DrumSampler::typeID;
            const auto isSampler = toString (info->typeID) == devices::Sampler::typeID;
            CHECK ((isDrums || isSampler || toString (info->typeID) == devices::WavetableSynth::typeID));

            for (const auto& [property, name] : sounds->getProperties())
            {
                if (isDrums)
                {
                    CHECK (property.toString().startsWith ("pad"));
                    CHECK (property.toString().endsWith ("Sound"));
                    CHECK (devices::findFactoryDrum (name.toString().toStdString()) >= 0);
                }
                else if (isSampler)
                {
                    CHECK (property == phazite::schema::samplerSample);
                    CHECK (devices::findFactorySample (name.toString().toStdString()) >= 0);
                }
                else
                {
                    CHECK ((property == phazite::schema::getOscillatorWavetable (0)
                            || property == phazite::schema::getOscillatorWavetable (1)));
                    CHECK (devices::findFactoryWavetable (name.toString().toStdString()) >= 0);
                }
            }
        }
    }

    // A good few for each instrument, and some for each effect and for each sampler, whose presets set up the ways to play
    // the sounds that come with Phazite (the user's own sounds aren't in its repository).
    for (const auto* info : phazite::getOwnDevices())
    {
        CAPTURE (std::string (info->name));
        const auto isSampler =
            toString (info->typeID) == devices::DrumSampler::typeID || toString (info->typeID) == devices::Sampler::typeID;
        CHECK (perDevice[toString (info->name)] >= (info->kind == devices::DeviceKind::instrument && ! isSampler ? 12 : 3));
    }
}

TEST_CASE ("Phazite's presets play: sound comes out, only numbers, and never far over full scale")
{
    for (const auto& file : getPresetFiles())
    {
        CAPTURE (file.getRelativePathFrom (getFactoryPresets()).toStdString());
        const auto device = phazite::readPreset (file)[keys::device];
        const auto* info = findDevice (device[keys::type].toString());
        REQUIRE (info != nullptr);

        const auto values = readValues (*info, device);
        const auto played = play (*info, values, device[keys::sounds]);
        CHECK (played.finite);
        CHECK (played.peak < 2.0f); // 6 dB over full scale at most, for a chord at velocity 100 or a loud beat

        // Sound, unless it is a vocoder waiting for a track to carry it (its sidechain, which this doesn't play).
        const auto waitsForTrack = toString (info->typeID) == devices::Vocoder::typeID
                                   && juce::roundToInt (values[(size_t) devices::findParameter (*info, "carrier")]) == 3;

        if (! waitsForTrack)
            CHECK (played.rms > std::pow (10.0, -60.0 / 20.0));
    }
}

TEST_CASE ("Phazite's instrument presets are leveled alike, and none clips a note or a chord")
{
    // Each one's note at velocity 100, at its kind's pitch (a bass and a drum low, a pad in the middle, a sampler at its root
    // or on its first slice), and, for one that plays chords (polyphonic, and not a bass, a drum or an effect), the chord the
    // test above plays: none of them peaks above -1 dBFS, as factory libraries keep their presets (docs/presets.md); and each
    // note's loudness is within 12 LU of its device's presets' median, so that switching presets doesn't jump far in level.
    std::map<juce::String, std::vector<std::pair<juce::String, float>>> loudness; // each device's presets, by their names

    for (const auto& file : getPresetFiles())
    {
        const auto device = phazite::readPreset (file)[keys::device];
        const auto* info = findDevice (device[keys::type].toString());
        REQUIRE (info != nullptr);

        if (info->kind != devices::DeviceKind::instrument || toString (info->typeID) == devices::DrumSampler::typeID)
            continue;

        const auto name = file.getRelativePathFrom (getFactoryPresets());
        CAPTURE (name.toStdString());
        const auto values = readValues (*info, device);
        const auto value = [&] (std::string_view id) { return values[(size_t) devices::findParameter (*info, id)]; };
        const auto kind = file.getParentDirectory().getFileName();
        const auto isSampler = toString (info->typeID) == devices::Sampler::typeID;

        auto note = kind == "Bass" || kind == "Percussion" ? 36 : (kind == "Pad" || kind == "Strings" ? 48 : 60);

        if (isSampler)
            note = juce::roundToInt (value ("mode")) == (int) devices::Sampler::Mode::slice ? devices::Sampler::firstSliceNote
                                                                                            : juce::roundToInt (value ("root"));

        const auto heard = playNotes (*info, values, device[keys::sounds], { note }, 2.0);
        CHECK (heard.peak <= juce::Decibels::decibelsToGain (-1.0f));
        loudness[toString (info->name)].push_back ({ name, heard.loudness });

        const auto playsChords = juce::roundToInt (value ("voiceMode")) != 1 && kind != "Bass" && kind != "Percussion" && kind != "FX";

        if (playsChords)
            CHECK (playNotes (*info, values, device[keys::sounds], { 36, 48, 52, 55 }, 0.5).peak <= juce::Decibels::decibelsToGain (-1.0f));
    }

    for (auto& [device, presets] : loudness)
    {
        auto sorted = presets;
        std::sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.second < b.second; });
        const auto median = sorted[sorted.size() / 2].second;

        for (const auto& [name, each] : presets)
        {
            CAPTURE (name.toStdString());
            CAPTURE (median);
            CHECK (std::abs (each - median) <= 12.0f);
        }
    }
}

TEST_CASE ("Phazite's drum kits each play sounds of their own on every pad, named after the kit, none of them another kit's")
{
    const auto names = devices::getFactoryDrumNames();
    std::map<juce::String, juce::String> kitOf; // each sound, and the kit that plays it
    auto kits = 0;

    for (const auto& file : getPresetFiles())
    {
        const auto device = phazite::readPreset (file)[keys::device];

        if (device[keys::type].toString() != devices::DrumSampler::typeID)
            continue;

        const auto kit = file.getFileNameWithoutExtension();
        CAPTURE (kit.toStdString());
        ++kits;

        for (int pad = devices::DrumSampler::kitFirstNote; pad < devices::DrumSampler::kitFirstNote + devices::DrumSampler::kitPads; ++pad)
        {
            const auto name = device[keys::sounds][phazite::schema::getPadSound (pad)].toString();
            CAPTURE (name.toStdString());
            CHECK (devices::findFactoryDrum (name.toStdString()) >= devices::DrumSampler::kitPads); // a kit's, not the default's
            CHECK (kit.contains (name.upToFirstOccurrenceOf (" ", false, false)));                  // "808 Kick" in "808 Kit"
            CHECK (kitOf.count (name) == 0);
            kitOf[name] = kit;
        }
    }

    // A kit for every sound past the default kit's.
    CHECK (kits == (int) names.size() / devices::DrumSampler::kitPads - 1);

    for (size_t i = devices::DrumSampler::kitPads; i < names.size(); ++i)
        CHECK (kitOf.count (toString (names[i])) == 1);
}

TEST_CASE ("The Dark Synthwave Kit is balanced as a kit: its kick the loudest at about -16 LUFS, and no pad peaking above -1 dBFS")
{
    // Each pad hit alone at velocity 100, as the preset report measures them (docs/presets.md, "Leveling"): the snares and the
    // clap 3 to 5 LU under the kick, the toms and the perc 5 to 8, the hats and the cymbals 8 to 11.
    const auto file = getFactoryPresets().getChildFile ("Drum Sampler/Kits/Dark Synthwave Kit" + juce::String (phazite::presetExtension));
    REQUIRE (file.existsAsFile());
    const auto device = phazite::readPreset (file)[keys::device];
    const auto* info = findDevice (device[keys::type].toString());
    REQUIRE (info != nullptr);
    const auto values = readValues (*info, device);
    const auto kick = playNotes (*info, values, device[keys::sounds], { 36 }, 0.1);
    CHECK (kick.loudness == doctest::Approx (-16.0f).epsilon (0.05));
    CHECK (kick.peak <= juce::Decibels::decibelsToGain (-1.0f));

    struct Group
    {
        std::vector<int> pads;
        float nearest, furthest; // under the kick, in LU
    };

    for (const auto& group : { Group { { 38, 39, 40 }, 3.0f, 5.0f }, Group { { 37, 41, 43, 45, 47, 48, 50 }, 5.0f, 8.0f },
                               Group { { 42, 44, 46, 49, 51 }, 8.0f, 11.0f } })
    {
        for (const auto pad : group.pads)
        {
            CAPTURE (pad);
            const auto heard = playNotes (*info, values, device[keys::sounds], { pad }, 0.1);
            CHECK (heard.peak <= juce::Decibels::decibelsToGain (-1.0f));
            CHECK (kick.loudness - heard.loudness >= group.nearest - 0.5f);
            CHECK (kick.loudness - heard.loudness <= group.furthest + 0.5f);
        }
    }

    // The closed and the pedal hat choke the open one.
    for (const auto pad : { 42, 44, 46 })
        CHECK (values[(size_t) devices::DrumSampler::getParameterIndex (pad, devices::DrumSampler::choke)] == doctest::Approx (1.0f));
}

TEST_CASE ("A kit's preset puts its sounds on a drum sampler's pads, which the song keeps, under any file put on a pad")
{
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, devices::DrumSampler::typeID);
    const auto device = phazite::getDeviceHandle (song.context, drums);
    const auto kitsFolder = getFactoryPresets().getChildFile ("Drum Sampler").getChildFile ("Kits");
    CHECK (device.getPadName (36) == juce::String ("Kick"));

    // The 808's on every pad, and in the sampler the engine plays once it has the song again.
    REQUIRE (phazite::loadPresetInto (song.context, drums, kitsFolder.getChildFile ("808 Kit.phazitepreset")));
    CHECK (device.getPadName (36) == juce::String ("808 Kick"));
    CHECK (device.getPadName (51) == juce::String ("808 Cowbell"));
    phazite::getSession (song.context)->update();
    auto* sampler = device.getProcessor<devices::DrumSampler>();
    REQUIRE (sampler != nullptr);
    CHECK (sampler->getPadSound (36) == devices::getFactoryDrum (devices::findFactoryDrum ("808 Kick")).get());

    // The song keeps them by their names, and a project's file is the song.
    const auto sounds = phazite::findDeviceState (*phazite::findSong (song.context), drums).getChildWithName (phazite::schema::SOUNDS);
    CHECK (sounds[phazite::schema::getPadSound (36)].toString() == juce::String ("808 Kick"));

    // A file put on a pad plays instead, until Reset brings the kit's sound back.
    REQUIRE (device.setPadSound (36, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Boom.wav"), 0.2)));
    CHECK (device.getPadName (36) == juce::String ("Boom"));
    device.clearPadSound (36);
    CHECK (device.getPadName (36) == juce::String ("808 Kick"));

    // Another kit in its place, and Undo step by step back to the default kit.
    REQUIRE (phazite::loadPresetInto (song.context, drums, kitsFolder.getChildFile ("909 Kit.phazitepreset")));
    CHECK (device.getPadName (42) == juce::String ("909 Closed Hat"));

    for (const auto* expected : { "808 Kick", "Boom", "808 Kick", "Kick" })
    {
        song.run ("edit.undo");
        CHECK (device.getPadName (36) == juce::String (expected));
    }
}

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

TEST_SUITE ("golden")
{
    // Each preset file as Save Preset writes it for a device loaded from it: every parameter, in the device's order, with
    // its tables. A preset written by hand, or by an earlier Phazite, shows each line that differs. Rewritten with
    // PHAZITE_UPDATE_GOLDEN=1, as the golden files are (GoldenFiles.h); then read git diff resources/presets in full.
    TEST_CASE ("Golden presets: Phazite's presets are written as Save Preset writes them")
    {
        const auto song = phazite::createSong();
        const auto track = phazite::getTrackIDs (*song)[0];
        const auto& plugins = phazite::getPluginHost (phazite::test::getEngineContext());
        const auto update = phazite::test::isUpdatingGoldenFiles();

        for (const auto& file : getPresetFiles())
        {
            CAPTURE (file.getRelativePathFrom (getFactoryPresets()).toStdString());
            const auto device = phazite::addDeviceFromPreset (*song, plugins, track, phazite::readPreset (file), nullptr);
            REQUIRE (device.isValid());

            const auto saved = phazite::makeDevicePreset (*song, device);
            const auto expected = file.loadFileAsString().replace ("\r\n", "\n");
            const auto actual = juce::JSON::toString (saved, false) + "\n";
            const auto differences = phazite::test::compareLines (expected, actual, 10);

            if (update && ! differences.isEmpty())
                REQUIRE (phazite::writePreset (saved, file));
            else
                CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n").toStdString());

            phazite::removeDevice (*song, device);
        }
    }
}
