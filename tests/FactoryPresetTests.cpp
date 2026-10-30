// SPDX-License-Identifier: AGPL-3.0-or-later
// The presets that come with Phazite (resources/presets; docs/design.md, section 9): each one a device's, in Phazite's own
// format (model/base/internal/Schema.h: schema::preset), in its device's folder, naming only parameters the device has,
// within their ranges, and tables it has; and each one playing: an instrument a chord and a bass note, an effect a beat
// and a tone, coming out with sound, without anything that isn't a number, and without going far over full scale.
#include "TestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dsp/Random.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Maximizer.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"

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

// resources/presets in the source tree, above the test program, as the golden tests find tests/.
juce::File getFactoryPresets()
{
    auto folder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    while (! folder.getChildFile ("resources").getChildFile ("presets").isDirectory() && ! folder.isRoot())
        folder = folder.getParentDirectory();

    return folder.getChildFile ("resources").getChildFile ("presets");
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
    if (type == devices::DrumSampler::typeID)
        return std::make_unique<devices::DrumSampler>();
    if (type == devices::Equalizer::typeID)
        return std::make_unique<devices::Equalizer>();
    if (type == devices::Compressor::typeID)
        return std::make_unique<devices::Compressor>();
    if (type == devices::Limiter::typeID)
        return std::make_unique<devices::Limiter>();
    if (type == devices::Maximizer::typeID)
        return std::make_unique<devices::Maximizer>();
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

// Plays a device with a preset's values for a second and a half: an instrument a chord and a bass note for half a second
// (a drum sampler each of its pads in turn, a 32nd note at 120 bpm apart, as a busy beat plays them), an effect a beat of
// noise bursts over a low tone, with what went in mixed back as its mix says, as its thin plug-in does it
// (model/devices/DevicePlugin.cpp).
Played play (const devices::DeviceInfo& info, const std::vector<float>& values, const juce::var& sounds)
{
    auto processor = makeProcessor (info);
    REQUIRE (processor != nullptr);
    processor->prepare (testRate, testBlockSize);

    if (auto* wavetable = dynamic_cast<devices::WavetableSynth*> (processor.get()))
        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
            if (const auto name = sounds[phazite::schema::getOscillatorWavetable (osc)].toString(); name.isNotEmpty())
                wavetable->setTable (osc, devices::getFactoryWavetable (devices::findFactoryWavetable (name.toStdString())));

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
            CHECK ((value.isDouble() || value.isInt() || value.isInt64()));
            CHECK ((float) value >= parameter.minimum);
            CHECK ((float) value <= parameter.maximum);
            CHECK (devices::constrain (parameter, (float) value) == doctest::Approx ((float) value));
        }

        // A wavetable synth's tables by their names; no sound files, which aren't in Phazite's repository.
        if (const auto* sounds = device[keys::sounds].getDynamicObject())
        {
            CHECK (toString (info->typeID) == devices::WavetableSynth::typeID);

            for (const auto& [property, name] : sounds->getProperties())
            {
                CHECK (
                    (property == phazite::schema::getOscillatorWavetable (0) || property == phazite::schema::getOscillatorWavetable (1)));
                CHECK (devices::findFactoryWavetable (name.toString().toStdString()) >= 0);
            }
        }
    }

    // A good few for each instrument, and some for each effect.
    for (const auto* info : phazite::getOwnDevices())
    {
        CAPTURE (std::string (info->name));
        CHECK (perDevice[toString (info->name)]
               >= (info->kind == devices::DeviceKind::instrument && toString (info->typeID) != devices::DrumSampler::typeID ? 12 : 3));
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

        std::vector<float> values;

        for (const auto& parameter : info->parameters)
            values.push_back (parameter.defaultValue);

        if (const auto* set = device[keys::parameters].getDynamicObject())
            for (const auto& [id, value] : set->getProperties())
                if (const auto index = devices::findParameter (*info, id.toString().toStdString()); index >= 0)
                    values[(size_t) index] = devices::constrain (info->parameters[(size_t) index], (float) value);

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
