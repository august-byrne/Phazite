// SPDX-License-Identifier: AGPL-3.0-or-later
// The presets that come with Phazite (resources/presets; docs/design.md, section 9): each one a device's, in Phazite's own
// format (model/base/internal/Schema.h: schema::preset), in its device's folder, naming every parameter the device has
// and no other, within their ranges, and tables it has, written as Save Preset writes them; and each one playing: an
// instrument a chord and a bass note, an effect a beat and a tone, coming out with sound, without anything that isn't a
// number, and without going far over full scale; and the instruments leveled alike, none of their notes or chords
// peaking above -1 dBFS.
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
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
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
#include "model/playback/internal/PlaybackSession.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>

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
// (a drum sampler each of its pads in turn, a 32nd note at 120 bpm apart, as a busy beat plays them), an effect a beat of
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
            for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
                if (const auto at = pad * padSpacing; at >= done && at < done + testBlockSize)
                    events.push_back ({ devices::NoteEvent::Type::noteOn, at - done, devices::DrumSampler::firstNote + pad, 0.8f });
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
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = testBlockSize;
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

        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
        {
            const auto name = device[keys::sounds][phazite::schema::getPadSound (pad)].toString();
            CAPTURE (name.toStdString());
            CHECK (devices::findFactoryDrum (name.toStdString()) >= devices::DrumSampler::numPads); // a kit's, not the default's
            CHECK (kit.contains (name.upToFirstOccurrenceOf (" ", false, false)));                  // "808 Kick" in "808 Kit"
            CHECK (kitOf.count (name) == 0);
            kitOf[name] = kit;
        }
    }

    // A kit for every sound past the default kit's.
    CHECK (kits == (int) names.size() / devices::DrumSampler::numPads - 1);

    for (size_t i = devices::DrumSampler::numPads; i < names.size(); ++i)
        CHECK (kitOf.count (toString (names[i])) == 1);
}

TEST_CASE ("A kit's preset puts its sounds on a drum sampler's pads, which the song keeps, under any file put on a pad")
{
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, devices::DrumSampler::typeID);
    const auto device = phazite::getDeviceHandle (song.context, drums);
    const auto kitsFolder = getFactoryPresets().getChildFile ("Drum Sampler").getChildFile ("Kits");
    CHECK (device.getPadName (0) == juce::String ("Kick"));

    // The 808's on every pad, and in the sampler the engine plays once it has the song again.
    REQUIRE (phazite::loadPresetInto (song.context, drums, kitsFolder.getChildFile ("808 Kit.phazitepreset")));
    CHECK (device.getPadName (0) == juce::String ("808 Kick"));
    CHECK (device.getPadName (15) == juce::String ("808 Cowbell"));
    phazite::getSession (song.context)->update();
    auto* sampler = device.getProcessor<devices::DrumSampler>();
    REQUIRE (sampler != nullptr);
    CHECK (&sampler->getPadSound (0) == devices::getFactoryDrum (devices::findFactoryDrum ("808 Kick")).get());

    // The song keeps them by their names, and a project's file is the song.
    const auto sounds = phazite::findDeviceState (*phazite::findSong (song.context), drums).getChildWithName (phazite::schema::SOUNDS);
    CHECK (sounds[phazite::schema::getPadSound (0)].toString() == juce::String ("808 Kick"));

    // A file put on a pad plays instead, until Reset brings the kit's sound back.
    REQUIRE (device.setPadSound (0, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Boom.wav"), 0.2)));
    CHECK (device.getPadName (0) == juce::String ("Boom"));
    device.clearPadSound (0);
    CHECK (device.getPadName (0) == juce::String ("808 Kick"));

    // Another kit in its place, and Undo step by step back to the default kit.
    REQUIRE (phazite::loadPresetInto (song.context, drums, kitsFolder.getChildFile ("909 Kit.phazitepreset")));
    CHECK (device.getPadName (6) == juce::String ("909 Closed Hat"));

    for (const auto* expected : { "808 Kick", "Boom", "808 Kick", "Kick" })
    {
        song.run ("edit.undo");
        CHECK (device.getPadName (0) == juce::String (expected));
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
