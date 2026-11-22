// SPDX-License-Identifier: AGPL-3.0-or-later
// Motion (src/devices/motion/Motion.h) and its editor (src/ui/devices/MotionEditor.h): what it is; each mode playing what its
// engine plays, through its runner, as the device it was before played (the guard of the mapping from Motion's values to
// each engine's, and of the one meaning of Mix); a change of mode faded without a click, the same in blocks of any size;
// its tail; and its editor's tabs, each mode's page and picture, their dots and wheels, and the default dock.
#include "MotionTestHelpers.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dsp/NoteValues.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Motion.h"
#include "devices/motion/Phaser.h"
#include "engine/nodes/DeviceRunner.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/components/DisplayTicker.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/MotionEditor.h"
#include "ui/devices/ParameterControls.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace engine = phazite::engine;
using devices::Motion;

constexpr double usualRate = 48000.0;

// A fixed stereo sound: on each side a chord of sines and a little noise, other on the right than on the left.
juce::AudioBuffer<float> makeStereoSound (int numSamples)
{
    juce::AudioBuffer<float> sound (2, numSamples);
    juce::Random random (17);
    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = (double) i / usualRate;
        sound.setSample (
            0, i, (float) (0.25 * std::sin (twoPi * 110.0 * t) + 0.15 * std::sin (twoPi * 660.0 * t)) + 0.1f * (random.nextFloat() - 0.5f));
        sound.setSample (
            1, i, (float) (0.2 * std::sin (twoPi * 82.5 * t) + 0.2 * std::sin (twoPi * 1250.0 * t)) + 0.1f * (random.nextFloat() - 0.5f));
    }

    return sound;
}

// Plays a stereo sound through a device's runner in blocks of a size, as Phazite's engine plays it: the song stopped, or
// playing from its start at 120 beats a minute in 4/4. Before each block, `beforeBlock` is called with the sample it starts
// on and may change the values.
juce::AudioBuffer<float> playStereo (const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> processor,
                                     std::vector<float> values, const juce::AudioBuffer<float>& sound, int blockSize, bool songPlays,
                                     const std::function<void (int, std::vector<float>&)>& beforeBlock = {})
{
    engine::DeviceRunner runner (info, std::move (processor));
    runner.prepare (usualRate, blockSize);
    auto out = sound;
    const auto beatsPerSample = 120.0 / 60.0 / usualRate;

    for (int start = 0; start < out.getNumSamples(); start += blockSize)
    {
        if (beforeBlock)
            beforeBlock (start, values);

        engine::DeviceRunner::Block block;
        block.values = values;
        block.isPlaying = songPlays;
        block.isRendering = true;

        if (songPlays)
        {
            block.songSample = start;
            block.sample = start;
            block.beat = start * beatsPerSample;
            block.bars = block.beat / 4.0;
            block.barStart = 4.0 * std::floor (block.bars);
            block.beatsPerSample = beatsPerSample;
            block.barsPerSample = beatsPerSample / 4.0;
        }

        runner.process (out, start, std::min (blockSize, out.getNumSamples() - start), block);
    }

    return out;
}

std::vector<float> getMotionDefaults (Motion::Mode mode)
{
    auto values = phazite::test::getDefaultValues (Motion::info);
    values[Motion::mode] = (float) mode;
    return values;
}

float choiceOf (const devices::DeviceInfo& info, int parameter, std::string_view name)
{
    const auto& described = info.parameters[(size_t) parameter];
    const auto index = devices::findChoice (described, name);
    REQUIRE (index.has_value());
    return described.minimum + (float) *index;
}

// The largest difference between two sounds, sample for sample, on either side, from a sample on.
float largestDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from = 0)
{
    auto largest = 0.0f;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = from; i < a.getNumSamples(); ++i)
            largest = std::max (largest, std::abs (a.getSample (channel, i) - b.getSample (channel, i)));

    return largest;
}

// A Motion on a track of its own song, open in a context of its own whose session plays it.
struct Song
{
    Song()
    {
        phazite::setSong (context, song.get());
        device = phazite::test::addOwnDevice (*song, phazite::getTrackIDs (*song)[0], Motion::typeID);
        REQUIRE (device.isValid());
        phazite::getSession (context)->update();
        song->getUndoManager().clearUndoHistory();
    }

    ~Song() { phazite::setSong (context, nullptr); }

    phazite::DeviceHandle getDevice() const { return phazite::makeDeviceHandle (*song, device); }
    float valueOf (const char* id) const { return phazite::getValueSetByHand (*song, phazite::ParameterRef { device, id }); }
    void set (const char* id, float value) const { phazite::setValueSetByHand (*song, phazite::ParameterRef { device, id }, value); }
    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::DeviceID device;
};

void tick()
{
    juce::SharedResourcePointer<phazite::ui::DisplayTicker>()->tickNow();
}

// Its mode's tab clicked.
void clickMode (phazite::ui::MotionEditor& editor, Motion::Mode mode)
{
    auto& tab = editor.getModeTabs()->getTab ((int) mode);
    phazite::test::Mouse mouse (tab);
    mouse.click (tab.getLocalBounds().getCentre());
}

bool shows (const phazite::ui::MotionEditor& editor, const char* id)
{
    const auto* control = editor.findControl (id);
    return control != nullptr && control->isShowing();
}

// A value of the device a mode was, by its parameter's ID: a number, or a choice by its name.
struct OldValue
{
    std::string_view id;
    std::variant<float, std::string_view> value;
};

// A setting of the device a mode was, every one of its parameters by its ID: one of its presets, or one written for the test.
struct OldSetting
{
    std::string name;
    std::vector<OldValue> values;
};

void setByID (const devices::DeviceInfo& info, std::vector<float>& values, std::string_view id, std::variant<float, std::string_view> value)
{
    CAPTURE (std::string (id));
    const auto index = devices::findParameter (info, id);
    REQUIRE (index >= 0);

    if (const auto* number = std::get_if<float> (&value))
        values[(size_t) index] = *number;
    else
        values[(size_t) index] = choiceOf (info, index, std::get<std::string_view> (value));
}

// Plays the mode's engine alone with the old device's values, by the engine's own IDs, and Motion with them converted by
// hand to its own, each through a runner of its own, and checks that the two agree sample for sample. The conversion is
// written out here, not taken from Motion's mapping (Motion::getChorusValues() and the others), which is what this guards:
// the mode; the Feedback a part of as far as the mode's engine goes (0.9, 1 or 0.85 at 100 %); a flanger's or a phaser's
// Mix half the old one (mixFactor), as Motion puts out the effect alone.
template <typename Engine>
void checkMode (Motion::Mode mode, float mostFeedback, float mixFactor, const OldSetting& setting)
{
    CAPTURE (setting.name);
    auto engineValues = phazite::test::getDefaultValues (Engine::info);
    auto values = getMotionDefaults (mode);
    auto songPlays = false;

    for (const auto& [id, value] : setting.values)
    {
        setByID (Engine::info, engineValues, id, value);
        auto converted = value;

        if (auto* number = std::get_if<float> (&converted))
        {
            if (id == "feedback")
                *number /= mostFeedback;
            else if (id == devices::mixParameterID)
                *number *= mixFactor;
            else if (id == "sync")
                songPlays = *number > 0.5f; // in time with the song while it plays: the LFO's phase from its place
        }

        setByID (Motion::info, values, id, converted);
    }

    // Every parameter of the device it was named once, so that nothing is left at a default that differs between the two.
    REQUIRE (setting.values.size() == Engine::info.parameters.size());

    for (const auto& parameter : Engine::info.parameters)
        CHECK (std::count_if (setting.values.begin(), setting.values.end(), [&] (const OldValue& v) { return v.id == parameter.id; }) == 1);

    const auto sound = makeStereoSound ((int) usualRate);

    for (const auto blockSize : { 256, 37 })
    {
        CAPTURE (blockSize);
        const auto before = playStereo (Engine::info, std::make_unique<Engine>(), engineValues, sound, blockSize, songPlays);
        const auto now = playStereo (Motion::info, std::make_unique<Motion>(), values, sound, blockSize, songPlays);
        CHECK (largestDifference (before, now) < 1.0e-5f);

        // It plays something: not the sound as it came, nor silence.
        CHECK (largestDifference (now, sound) > 1.0e-3f);
    }
}

// Each mode's settings, and each of its engine's parameters away from its default in at least one of them: so that a line
// of the mapping that reads the wrong value of Motion's, or none, shows (two values that trade places are each away from
// where the other is in one of the settings).
template <typename Engine>
void checkModeSettings (Motion::Mode mode, float mostFeedback, float mixFactor, const std::vector<OldSetting>& settings)
{
    for (const auto& parameter : Engine::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        const auto moved = std::any_of (settings.begin(), settings.end(),
                                        [&] (const OldSetting& setting)
                                        {
                                            auto engineValues = phazite::test::getDefaultValues (Engine::info);

                                            for (const auto& [id, value] : setting.values)
                                                if (id == parameter.id)
                                                    setByID (Engine::info, engineValues, id, value);

                                            const auto index = (size_t) devices::findParameter (Engine::info, parameter.id);
                                            return ! juce::exactlyEqual (engineValues[index], parameter.defaultValue);
                                        });
        CHECK (moved);
    }

    for (const auto& setting : settings)
        checkMode<Engine> (mode, mostFeedback, mixFactor, setting);
}
} // namespace

TEST_CASE ("Motion describes itself: three modes, a chorus at first, half and half with the sound, its engines no devices of their own")
{
    CHECK (devices::isValid (Motion::info));
    CHECK (Motion::info.name == std::string_view ("Motion"));
    CHECK (Motion::info.kind == devices::DeviceKind::effect);

    const auto& mode = Motion::info.parameters[Motion::mode];
    CHECK (mode.id == std::string_view ("mode"));
    REQUIRE (mode.choices.size() == 3);
    CHECK (mode.choices[0] == std::string_view ("Chorus"));
    CHECK (mode.choices[1] == std::string_view ("Flanger"));
    CHECK (mode.choices[2] == std::string_view ("Phaser"));

    // The chorus's defaults, so that a new Motion plays what a new Chorus played: the Juno-60's mode I, a copy a side swept
    // from 1.75 to 5.25 ms, half and half with the sound.
    const auto values = getMotionDefaults (Motion::Mode::chorus);
    CHECK (Motion::getMode (values) == Motion::Mode::chorus);
    CHECK (values[(size_t) devices::findParameter (Motion::info, devices::mixParameterID)] == doctest::Approx (0.5f));
    const auto chorus = Motion::getChorusValues (values);
    CHECK (devices::Chorus::getNumVoices (chorus) == 1);
    CHECK (devices::Chorus::getVoiceDelay (chorus, devices::Chorus::getVoicePhase (chorus, 0.0, 0, 0), 0) == doctest::Approx (1.75));
    CHECK (devices::Chorus::getVoiceDelay (chorus, devices::Chorus::getVoicePhase (chorus, 0.0, 1, 0), 0) == doctest::Approx (5.25));

    // Feedback from -100 to +100 % in every mode, as far as each engine goes; the flanger's and the phaser's mix twice it.
    const auto& feedback = Motion::info.parameters[Motion::feedback];
    CHECK (feedback.minimum == doctest::Approx (-1.0f));
    CHECK (feedback.maximum == doctest::Approx (1.0f));
    auto most = values;
    most[Motion::feedback] = -1.0f;
    CHECK (Motion::getChorusValues (most)[devices::Chorus::feedback] == doctest::Approx (-0.9f));
    CHECK (Motion::getFlangerValues (most)[devices::Flanger::feedback] == doctest::Approx (-1.0f));
    CHECK (Motion::getPhaserValues (most)[devices::Phaser::feedback] == doctest::Approx (-0.85f));
    CHECK (Motion::getFlangerValues (values)[devices::Flanger::mix] == doctest::Approx (1.0f));
    CHECK (Motion::getPhaserValues (values)[devices::Phaser::mix] == doctest::Approx (1.0f));

    // The engines' type IDs are their runners' alone: no device of their own, which nothing lists, adds or saves.
    for (const auto* engineInfo : { &devices::Chorus::info, &devices::Flanger::info, &devices::Phaser::info })
    {
        CHECK (devices::isValid (*engineInfo));
        CHECK (phazite::findOwnDevice (engineInfo->typeID) == nullptr);

        for (const auto* listed : phazite::getOwnDevices())
            CHECK (listed->typeID != engineInfo->typeID);
    }

    // Their own ranges take every value Motion maps to them.
    for (const auto& [id, engineInfo] : { std::pair<const char*, const devices::DeviceInfo*> { "delay", &devices::Chorus::info },
                                          { "delay", &devices::Flanger::info },
                                          { "feedback", &devices::Chorus::info } })
    {
        CAPTURE (id);
        const auto& own = engineInfo->parameters[(size_t) devices::findParameter (*engineInfo, id)];
        const auto& motion = Motion::info.parameters[(size_t) devices::findParameter (Motion::info, id)];
        const auto factor = std::string_view (id) == "feedback" ? Motion::chorusFeedback : 1.0f;
        CHECK (own.minimum == doctest::Approx (factor * motion.minimum));
        CHECK (own.maximum == doctest::Approx (factor * motion.maximum));
    }

    // Phazite lists Motion among its effects, and none of the three it was made from.
    CHECK (phazite::findOwnDevice (Motion::typeID) == &Motion::info);

    for (const auto* gone : { "phaziteChorus", "phaziteFlanger", "phazitePhaser" })
        CHECK (phazite::findOwnDevice (gone) == nullptr);
}

TEST_CASE ("Motion: each mode plays what its device played")
{
    // The old devices' presets (resources/presets/Chorus/, Flanger/ and Phaser/ before roadmap step S6.10), by their own
    // parameters' IDs, and settings written for the test where the presets leave a parameter at its default or a value at an
    // end: in time with the song, running free, the feedback at its ends.
    SUBCASE ("Chorus")
    {
        const std::vector<OldSetting> settings {
            { "Solid Bass Chorus",
              { { "voices", "3" },
                { "delay", 5.0f },
                { "depth", 0.5f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 0.6f },
                { "shape", "Triangle" },
                { "shimmer", 0.0f },
                { "voicePhase", 1.0f },
                { "spread", 0.4f },
                { "feedback", 0.0f },
                { "stereo", 1.0f },
                { "highCut", 12000.0f },
                { "safeBass", 150.0f },
                { "mix", 0.5f } } },
            { "Deep Swirl",
              { { "voices", "4" },
                { "delay", 8.19f },
                { "depth", 0.61f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 0.7f },
                { "shape", "Sine" },
                { "shimmer", 0.0f },
                { "voicePhase", 0.524f },
                { "spread", 0.62f },
                { "feedback", 0.0f },
                { "stereo", 1.0f },
                { "highCut", 13000.0f },
                { "safeBass", 20.0f },
                { "mix", 0.7f } } },
            { "Lush Ensemble",
              { { "voices", "3" },
                { "delay", 12.0f },
                { "depth", 0.2f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 0.35f },
                { "shape", "Sine" },
                { "shimmer", 1.0f },
                { "voicePhase", 1.0f },
                { "spread", 0.0f },
                { "feedback", 0.0f },
                { "stereo", 1.0f },
                { "highCut", 20000.0f },
                { "safeBass", 20.0f },
                { "mix", 0.6f } } },
            { "Metallic Chorus",
              { { "voices", "1" },
                { "delay", 2.5f },
                { "depth", 0.4f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 0.3f },
                { "shape", "Triangle" },
                { "shimmer", 0.0f },
                { "voicePhase", 1.0f },
                { "spread", 0.0f },
                { "feedback", 0.6f },
                { "stereo", 1.0f },
                { "highCut", 20000.0f },
                { "safeBass", 20.0f },
                { "mix", 0.5f } } },
            { "Tape Wobble",
              { { "voices", "1" },
                { "delay", 10.0f },
                { "depth", 0.4f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 0.6f },
                { "shape", "Sine" },
                { "shimmer", 0.0f },
                { "voicePhase", 1.0f },
                { "spread", 0.0f },
                { "feedback", 0.0f },
                { "stereo", 0.0f },
                { "highCut", 6000.0f },
                { "safeBass", 20.0f },
                { "mix", 1.0f } } },
            { "in time with the song, eight voices, the feedback at its least",
              { { "voices", "8" },
                { "delay", 12.0f },
                { "depth", 1.0f },
                { "sync", 1.0f },
                { "division", "1/2" },
                { "rate", 0.5f },
                { "shape", "Sine" },
                { "shimmer", 1.0f },
                { "voicePhase", 0.3f },
                { "spread", 0.8f },
                { "feedback", -0.9f },
                { "stereo", 0.4f },
                { "highCut", 4000.0f },
                { "safeBass", 300.0f },
                { "mix", 0.3f } } },
            { "the feedback at its most",
              { { "voices", "2" },
                { "delay", 3.0f },
                { "depth", 0.3f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 2.0f },
                { "shape", "Triangle" },
                { "shimmer", 0.0f },
                { "voicePhase", 0.7f },
                { "spread", 0.2f },
                { "feedback", 0.9f },
                { "stereo", 0.6f },
                { "highCut", 9000.0f },
                { "safeBass", 20.0f },
                { "mix", 0.5f } } },
        };

        checkModeSettings<devices::Chorus> (Motion::Mode::chorus, 0.9f, 1.0f, settings);
    }

    SUBCASE ("Flanger")
    {
        const std::vector<OldSetting> settings {
            { "Jet Sweep",
              { { "delay", 5.0f },
                { "depth", 0.9f },
                { "sync", 1.0f },
                { "division", "2 bars" },
                { "rate", 0.25f },
                { "shape", "Triangle" },
                { "feedback", 0.7f },
                { "stereo", 0.25f },
                { "safeBass", 20.0f },
                { "mix", 1.0f } } },
            { "Negative Flange",
              { { "delay", 3.0f },
                { "depth", 0.9f },
                { "sync", 1.0f },
                { "division", "1 bar" },
                { "rate", 0.25f },
                { "shape", "Triangle" },
                { "feedback", -0.7f },
                { "stereo", 0.25f },
                { "safeBass", 150.0f },
                { "mix", 1.0f } } },
            { "Metallic Ring",
              { { "delay", 1.5f },
                { "depth", 0.4f },
                { "sync", 1.0f },
                { "division", "4 bars" },
                { "rate", 0.25f },
                { "shape", "Sine" },
                { "feedback", 0.85f },
                { "stereo", 0.3f },
                { "safeBass", 190.0f },
                { "mix", 1.0f } } },
            { "running free, the longest delay, the feedback at its most, a Mix below full",
              { { "delay", 25.0f },
                { "depth", 0.6f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 3.0f },
                { "shape", "Triangle" },
                { "feedback", 1.0f },
                { "stereo", 0.8f },
                { "safeBass", 20.0f },
                { "mix", 0.4f } } },
            { "a sine, a short delay, the feedback at its least",
              { { "delay", 0.5f },
                { "depth", 0.3f },
                { "sync", 0.0f },
                { "division", "1/4" },
                { "rate", 1.2f },
                { "shape", "Sine" },
                { "feedback", -1.0f },
                { "stereo", 0.0f },
                { "safeBass", 60.0f },
                { "mix", 0.7f } } },
        };

        checkModeSettings<devices::Flanger> (Motion::Mode::flanger, 1.0f, 0.5f, settings);
    }

    SUBCASE ("Phaser")
    {
        const std::vector<OldSetting> settings {
            { "Classic Phase",
              { { "notches", "2" },
                { "frequency", 700.0f },
                { "depth", 0.75f },
                { "sync", 1.0f },
                { "division", "1/2" },
                { "rate", 0.5f },
                { "shape", "Sine" },
                { "feedback", 0.35f },
                { "spacing", 0.5f },
                { "stereo", 0.0f },
                { "safeBass", 20.0f },
                { "mix", 1.0f } } },
            { "Deep Phase",
              { { "notches", "4" },
                { "frequency", 900.0f },
                { "depth", 0.8f },
                { "sync", 1.0f },
                { "division", "1 bar" },
                { "rate", 0.5f },
                { "shape", "Sine" },
                { "feedback", 0.4f },
                { "spacing", 0.6f },
                { "stereo", 0.5f },
                { "safeBass", 20.0f },
                { "mix", 1.0f } } },
            { "Resonant Jet",
              { { "notches", "6" },
                { "frequency", 1200.0f },
                { "depth", 1.0f },
                { "sync", 1.0f },
                { "division", "2 bars" },
                { "rate", 0.5f },
                { "shape", "Triangle" },
                { "feedback", 0.55f },
                { "spacing", 0.5f },
                { "stereo", 0.3f },
                { "safeBass", 120.0f },
                { "mix", 1.0f } } },
            { "Slow Swirl",
              { { "notches", "3" },
                { "frequency", 600.0f },
                { "depth", 0.9f },
                { "sync", 1.0f },
                { "division", "4 bars" },
                { "rate", 0.5f },
                { "shape", "Sine" },
                { "feedback", 0.2f },
                { "spacing", 0.5f },
                { "stereo", 0.5f },
                { "safeBass", 20.0f },
                { "mix", 0.8f } } },
            { "Stereo Spin",
              { { "notches", "2" },
                { "frequency", 1000.0f },
                { "depth", 0.7f },
                { "sync", 1.0f },
                { "division", "1 bar" },
                { "rate", 0.5f },
                { "shape", "Sine" },
                { "feedback", 0.25f },
                { "spacing", 0.5f },
                { "stereo", 1.0f },
                { "safeBass", 20.0f },
                { "mix", 1.0f } } },
            { "running free, one notch, the feedback at its least, a Mix below full",
              { { "notches", "1" },
                { "frequency", 2500.0f },
                { "depth", 0.4f },
                { "sync", 0.0f },
                { "division", "1 bar" },
                { "rate", 4.0f },
                { "shape", "Triangle" },
                { "feedback", -0.85f },
                { "spacing", 0.2f },
                { "stereo", 0.7f },
                { "safeBass", 20.0f },
                { "mix", 0.6f } } },
        };

        checkModeSettings<devices::Phaser> (Motion::Mode::phaser, 0.85f, 0.5f, settings);
    }
}

TEST_CASE ("Motion: a change of mode fades out and in over 10 ms without a click, the same in blocks of any size")
{
    // A sine through its runner, the mode changed by hand at blocks' starts: Flanger, Phaser, Chorus again, and Phaser and
    // straight on to Flanger as Phaser fades in.
    constexpr int numSamples = 72000, fadeLength = 480; // 10 ms
    constexpr int changes[] = { 12288, 24576, 36864, 49152, 49152 + 512 };
    constexpr Motion::Mode modes[] = { Motion::Mode::flanger, Motion::Mode::phaser, Motion::Mode::chorus, Motion::Mode::phaser,
                                       Motion::Mode::flanger };
    constexpr float amplitude = 0.5f;
    const auto sound = phazite::test::makeTone (330.0, numSamples, usualRate, amplitude);

    const auto play = [&] (int blockSize)
    {
        auto values = getMotionDefaults (Motion::Mode::chorus);
        values[Motion::feedback] = 0.5f;

        return phazite::test::playThroughRunner (Motion::info, std::make_unique<Motion>(), values, sound, usualRate, blockSize,
                                                 [&] (int block, std::vector<float>& v)
                                                 {
                                                     for (size_t i = 0; i < std::size (changes); ++i)
                                                         if (block * blockSize == changes[i])
                                                             v[Motion::mode] = (float) modes[i];
                                                 });
    };

    const auto small = play (32), large = play (512);
    CHECK (largestDifference (small, large) < 1.0e-6f);

    // From one sample to the next: no more than the steadiest of the modes and the sound move, and what a 10 ms fade adds
    // to that (the fade's steepest step, 1.5 / its length, times the most the two it fades between can differ).
    const auto largestStep = [&large] (int from, int to)
    {
        auto largest = 0.0f;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = from + 1; i < to; ++i)
                largest = std::max (largest, std::abs (large.getSample (channel, i) - large.getSample (channel, i - 1)));

        return largest;
    };

    auto steady =
        std::max (2.0f * juce::MathConstants<float>::pi * 330.0f / (float) usualRate * amplitude, largestStep (2 * fadeLength, changes[0]));

    for (int i = 0; i < 3; ++i)
        steady = std::max (steady, largestStep (changes[i] + 3 * fadeLength, changes[i + 1]));

    const auto fadeStep = 1.5f / (float) fadeLength * 4.0f * amplitude;

    for (const auto change : changes)
    {
        CAPTURE (change);
        CHECK (largestStep (change - 1, change + 3 * fadeLength) <= steady + fadeStep);
    }

    // Halfway through the fade-out, the sound comes half from what played; at its bottom, it is the sound as it came, at any
    // mix; then the next mode fades in.
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int i = changes[0] + fadeLength - 1; i < changes[0] + fadeLength + 1; ++i)
            CHECK (large.getSample (channel, i) == doctest::Approx (sound[(size_t) i]).epsilon (1.0e-4));
    }
}

TEST_CASE ("Motion: the level holds through a change of mode, within 1 dB where the effect is in phase with the sound")
{
    // A low sine through Chorus mode at a Mix of 50 %, its copy 0.2 ms behind (a few degrees of the sine's cycle, so in phase
    // with it), changed to Flanger, whose copy is as close: the level, read over each cycle of the sine through the change,
    // within 1 dB of where it was before. Fed the sound faded by the fade's gain and then faded by it again, the effect would
    // play 1 - g + g squared of it, 2.5 dB down at the fade's middle.
    constexpr double hertz = 200.0;
    constexpr int cycle = 240, changeAt = 12288, fadeLength = 480, blockSize = 512;
    const auto sound = phazite::test::makeTone (hertz, changeAt + 8 * fadeLength, usualRate);
    auto values = getMotionDefaults (Motion::Mode::chorus);
    values[Motion::delay] = 0.2f;
    values[Motion::depth] = 0.0f;
    REQUIRE (values[Motion::mix] == doctest::Approx (0.5f));

    const auto out = phazite::test::playThroughRunner (Motion::info, std::make_unique<Motion>(), values, sound, usualRate, blockSize,
                                                       [] (int block, std::vector<float>& v)
                                                       {
                                                           if (block * blockSize == changeAt)
                                                               v[Motion::mode] = (float) Motion::Mode::flanger;
                                                       });

    // A sine's amplitude from its RMS over one cycle from a sample, on the left.
    const auto levelAt = [&out] (int from)
    {
        auto energy = 0.0;

        for (int i = from; i < from + cycle; ++i)
            energy += (double) out.getSample (0, i) * (double) out.getSample (0, i);

        return std::sqrt (2.0 * energy / cycle);
    };

    const auto before = levelAt (changeAt - 4 * cycle);
    REQUIRE (before > 0.4); // the sound and its copy in phase: about as loud as the sound (0.5)
    auto furthest = 0.0;
    auto where = 0;

    for (int from = changeAt - cycle; from < changeAt + 4 * fadeLength; from += 4)
    {
        if (const auto off = std::abs (phazite::test::inDecibels (levelAt (from) / before)); off > furthest)
        {
            furthest = off;
            where = from;
        }
    }

    INFO ("the level " << furthest << " dB off what it was, over the cycle from sample " << where);
    CHECK (furthest < 1.0);
}

TEST_CASE (
    "Motion: a change of mode while the song plays, in time with it, is the same in any blocks, the new mode's LFO where the song puts it")
{
    // Sync on and the song playing: the LFO's phase comes from where the song is. The mode changes at a block's start in blocks
    // of 512, so that the fade-out (480 samples) ends in the middle of one, where Motion plays the rest of the block as a
    // block of its own with the song moved on to where it starts; in blocks of 32 the fade-out ends at a block's end.
    constexpr int changeAt = 24576, settled = changeAt + 4800; // 100 ms after the change
    const auto sound = makeStereoSound ((int) usualRate);

    for (const auto newMode : { Motion::Mode::flanger, Motion::Mode::phaser })
    {
        CAPTURE ((int) newMode);
        auto values = getMotionDefaults (Motion::Mode::chorus);
        values[Motion::sync] = 1.0f;
        values[Motion::division] = choiceOf (Motion::info, Motion::division, "1/4");
        values[Motion::depth] = 0.8f;
        const auto change = [newMode] (int start, std::vector<float>& v)
        {
            if (start == changeAt)
                v[Motion::mode] = (float) newMode;
        };

        const auto small = playStereo (Motion::info, std::make_unique<Motion>(), values, sound, 32, true, change);
        const auto large = playStereo (Motion::info, std::make_unique<Motion>(), values, sound, 512, true, change);
        CHECK (largestDifference (small, large) < 1.0e-6f);
        CHECK (largestDifference (large, sound, changeAt) > 1.0e-3f); // the new mode plays

        // As a Motion in the new mode all along plays, once the fade is over and the new engine's lines have filled: its
        // sweep where the song puts it, not where it would be had it started with the change.
        auto allAlong = values;
        allAlong[Motion::mode] = (float) newMode;
        const auto along = playStereo (Motion::info, std::make_unique<Motion>(), allAlong, sound, 512, true);
        CHECK (largestDifference (large, along, settled) < 1.0e-4f);
    }
}

TEST_CASE ("Motion's Flanger and Phaser pictures draw what it plays at the Mix set, and repaint when the Mix moves")
{
    constexpr double rate = 48000.0;
    const auto lowestAndHighest = [] (const std::function<double (double)>& magnitude)
    {
        auto lowest = 1.0e9, highest = 0.0;

        for (auto hertz = 20.0; hertz < 20000.0; hertz *= 1.01)
        {
            lowest = std::min (lowest, magnitude (hertz));
            highest = std::max (highest, magnitude (hertz));
        }

        return std::pair { phazite::test::inDecibels (lowest), phazite::test::inDecibels (highest) };
    };

    // The phaser without feedback at a Mix of 100 %: its all-pass sections alone, flat. At 50 % its engine's response, with
    // its notches.
    auto values = getMotionDefaults (Motion::Mode::phaser);
    REQUIRE (values[Motion::feedback] == doctest::Approx (0.0f));
    values[Motion::mix] = 1.0f;
    const auto [flatLowest, flatHighest] =
        lowestAndHighest ([&] (double hertz) { return Motion::getPhaserMagnitude (values, 800.0, hertz, rate); });
    CHECK (std::abs (flatLowest) < 0.1);
    CHECK (std::abs (flatHighest) < 0.1);

    values[Motion::mix] = 0.5f;
    CHECK (lowestAndHighest ([&] (double hertz) { return Motion::getPhaserMagnitude (values, 800.0, hertz, rate); }).first < -30.0);

    for (const auto hertz : { 100.0, 800.0, 3000.0 })
        CHECK (Motion::getPhaserMagnitude (values, 800.0, hertz, rate)
               == doctest::Approx (devices::Phaser::getMagnitude (Motion::getPhaserValues (values), 800.0, hertz, rate)));

    // The flanger's notches, as deep as they go at 50 %, shallower at 25 %: a quarter of the copy against three of the sound.
    auto flanger = getMotionDefaults (Motion::Mode::flanger);
    const auto deepest = [&] (float mix)
    {
        flanger[Motion::mix] = mix;
        return lowestAndHighest ([&] (double hertz) { return Motion::getFlangerMagnitude (flanger, 2.0, hertz, rate); }).first;
    };

    const auto atHalf = deepest (0.5f), atQuarter = deepest (0.25f);
    CHECK (atHalf < -30.0);
    CHECK (atQuarter == doctest::Approx (phazite::test::inDecibels (0.5)).epsilon (0.05));
    CHECK (atQuarter > atHalf + 20.0);

    // Each picture repaints when the Mix moves, nothing else turned.
    Song song;
    phazite::ui::MotionEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    for (const auto mode : { Motion::Mode::flanger, Motion::Mode::phaser })
    {
        CAPTURE ((int) mode);
        clickMode (editor, mode);
        tick();
        phazite::test::dispatchPendingMessages();
        window.takeRepaintedArea();
        song.set ("mix", mode == Motion::Mode::flanger ? 0.8f : 0.3f);
        tick();
        CHECK_FALSE (window.takeRepaintedArea().isEmpty());
    }
}

TEST_CASE ("Motion's Flanger and Phaser pictures at a Mix of 50 and 100 % are what a sine through it measures; the flanger's teeth "
           "as deep as its notches")
{
    // The sweep held still (Depth 0), a sine through Motion on its runner, read once it has settled, against the curve its
    // picture draws there (Motion::getFlangerMagnitude(), getPhaserMagnitude()): at 50 % the engine's response, its notches
    // whole; at 100 % the effect alone, which for the flanger is the copy alone, flat without feedback, with no notches.
    constexpr int length = 24000, measured = 4800; // 100 ms: whole cycles of each tone
    const auto levelOf = [] (const juce::AudioBuffer<float>& out)
    {
        auto energy = 0.0;

        for (int i = length - measured; i < length; ++i)
            energy += (double) out.getSample (0, i) * (double) out.getSample (0, i);

        return std::sqrt (2.0 * energy / measured);
    };

    for (const auto mode : { Motion::Mode::flanger, Motion::Mode::phaser })
    {
        for (const auto feedback : { 0.0f, 0.5f })
        {
            for (const auto mix : { 0.5f, 1.0f })
            {
                CAPTURE ((int) mode);
                CAPTURE (feedback);
                CAPTURE (mix);
                auto values = getMotionDefaults (mode);
                values[Motion::depth] = 0.0f;
                values[Motion::delay] = 2.0f; // the flanger's notches at 250, 750, 1250 Hz...
                values[Motion::frequency] = 1000.0f;
                values[Motion::feedback] = feedback;
                values[Motion::mix] = mix;

                for (const auto hertz : { 500.0, 750.0, 1000.0, 1250.0 })
                {
                    CAPTURE (hertz);
                    juce::AudioBuffer<float> sine (2, length);

                    for (int i = 0; i < length; ++i)
                        for (int channel = 0; channel < 2; ++channel)
                            sine.setSample (channel, i,
                                            0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * hertz * i / usualRate));

                    const auto out = playStereo (Motion::info, std::make_unique<Motion>(), values, sine, 480, false);
                    const auto heard = phazite::test::inDecibels (levelOf (out) / 0.25);
                    const auto drawn = phazite::test::inDecibels (
                        mode == Motion::Mode::flanger
                            ? Motion::getFlangerMagnitude (values, 2.0, hertz, usualRate)
                            : Motion::getPhaserMagnitude (
                                  values, devices::Phaser::getSweptCentre (Motion::getPhaserValues (values), 0.0f, usualRate), hertz,
                                  usualRate));
                    CAPTURE (heard);
                    CAPTURE (drawn);

                    // A notch heard whole is at the floor of both: deep enough is the same.
                    if (drawn > -40.0)
                        CHECK (std::abs (heard - drawn) < 0.3);
                    else
                        CHECK (heard < -30.0);
                }
            }
        }
    }

    // The flanger's teeth, a line at each notch, as deep as the notches are heard: whole at 50 %, half at 25 and 75 %, none at
    // 100 %, where a tone on a notch comes through at its level.
    Song song;
    phazite::ui::MotionEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    clickMode (editor, Motion::Mode::flanger);

    for (const auto& [mix, teeth] : { std::pair (0.5f, 1.0f), std::pair (0.75f, 0.5f), std::pair (1.0f, 0.0f), std::pair (0.25f, 0.5f) })
    {
        CAPTURE (mix);
        song.set ("mix", mix);
        tick();
        CHECK (editor.getShownTeeth() == doctest::Approx (teeth));
    }

    auto values = getMotionDefaults (Motion::Mode::flanger);
    values[Motion::depth] = 0.0f;
    values[Motion::delay] = 2.0f;
    values[Motion::mix] = 1.0f;
    CHECK (std::abs (phazite::test::inDecibels (Motion::getFlangerMagnitude (values, 2.0, 750.0, usualRate))) < 0.1);
}

TEST_CASE ("Motion: the tail it reports is the tail of the mode that plays")
{
    Motion motion;
    motion.prepare (usualRate, 512);
    auto values = getMotionDefaults (Motion::Mode::phaser);
    values[Motion::feedback] = 1.0f;
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
    devices::ProcessContext block;
    block.channels = channels;
    block.numChannels = 2;
    block.numSamples = 512;
    block.parameters = values;
    motion.process (block);

    devices::Phaser phaser;
    phaser.prepare (usualRate, 512);
    const auto phaserValues = Motion::getPhaserValues (values);
    block.parameters = phaserValues;
    phaser.process (block);
    CHECK (motion.getTailSeconds() == doctest::Approx (phaser.getTailSeconds()));

    // A chorus at its longest delay: its tail, once the phaser has faded out.
    values[Motion::mode] = (float) Motion::Mode::chorus;
    values[Motion::delay] = 25.0f;
    block.parameters = values;

    for (int i = 0; i < 4; ++i)
        motion.process (block);

    devices::Chorus chorus;
    chorus.prepare (usualRate, 512);
    const auto chorusValues = Motion::getChorusValues (values);
    block.parameters = chorusValues;
    chorus.process (block);
    CHECK (motion.getTailSeconds() == doctest::Approx (chorus.getTailSeconds()));
}

//==============================================================================
TEST_CASE ("Motion's editor: a tab for each mode, whose click sets the mode as one undo step and shows its parts, at one size")
{
    Song song;
    const auto device = song.getDevice();
    CHECK (dynamic_cast<phazite::ui::MotionEditor*> (phazite::ui::createDeviceEditor (device).get()) != nullptr);

    phazite::ui::MotionEditor editor (device);
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    const auto size = editor.getBounds();
    auto* tabs = editor.getModeTabs();
    REQUIRE (tabs != nullptr);

    // A control for every parameter but the mix, each with its title.
    for (const auto& parameter : Motion::info.parameters)
    {
        CAPTURE (std::string (parameter.id));
        const auto* control = editor.findControl (parameter.id);
        CHECK ((control != nullptr) == (parameter.id != devices::mixParameterID));

        if (control != nullptr)
            CHECK (control->getTitle().isNotEmpty());
    }

    // Chorus at first: its tab lit, each tab saying what its mode is, the chorus's picture and its parts showing.
    const char* names[] = { "Chorus", "Flanger", "Phaser" };

    for (int mode = 0; mode < 3; ++mode)
    {
        CHECK (tabs->getTab (mode).getButtonText() == juce::String (names[mode]));
        CHECK (tabs->getTab (mode).getTooltip().startsWith (juce::String (names[mode]) + ": "));
        CHECK (tabs->getTab (mode).getToggleState() == (mode == 0));
    }

    struct Page
    {
        Motion::Mode mode;
        const char* pictureTitle;
        std::vector<const char*> shown, hidden;
    };

    const Page pages[] = {
        { Motion::Mode::chorus,
          "Chorus copies",
          { "delay", "depth", "sync", "division", "rate", "shape", "voices", "voicePhase", "spread", "shimmer", "feedback", "highCut",
            "stereo", "safeBass" },
          { "frequency", "notches", "spacing" } },
        { Motion::Mode::flanger,
          "Flanger comb",
          { "delay", "depth", "sync", "division", "rate", "shape", "feedback", "stereo", "safeBass" },
          { "frequency", "notches", "spacing", "highCut", "voices", "voicePhase", "spread", "shimmer" } },
        { Motion::Mode::phaser,
          "Phaser notches",
          { "frequency", "depth", "sync", "division", "rate", "shape", "notches", "feedback", "spacing", "stereo", "safeBass" },
          { "delay", "highCut", "voices", "voicePhase", "spread", "shimmer" } },
    };

    const auto checkPage = [&] (const Page& page)
    {
        CAPTURE ((int) page.mode);
        CHECK (editor.getShownMode() == (int) page.mode);
        CHECK (editor.getPicture().isShowing());
        CHECK (editor.getPicture().getTitle() == juce::String (page.pictureTitle));
        CHECK (editor.getBounds() == size);

        for (const auto* id : page.shown)
        {
            CAPTURE (id);
            CHECK (shows (editor, id));
        }

        for (const auto* id : page.hidden)
        {
            CAPTURE (id);
            CHECK_FALSE (shows (editor, id));
        }

        phazite::test::checkChildrenApart (editor);
    };

    checkPage (pages[0]);

    // Each tab's click: the mode, as one undo step, and its page.
    for (const auto mode : { Motion::Mode::flanger, Motion::Mode::phaser, Motion::Mode::chorus })
    {
        clickMode (editor, mode);
        CHECK (song.valueOf ("mode") == doctest::Approx ((float) mode));
        CHECK (song.describeUndo() == juce::String ("Undo Change Mode"));
        CHECK (tabs->getTab ((int) mode).getToggleState());
        checkPage (pages[(size_t) mode]);
    }

    // Undone, the page follows as the ticker refreshes the tabs.
    phazite::undo (*song.song);
    CHECK (song.valueOf ("mode") == doctest::Approx ((float) Motion::Mode::phaser));
    tick();
    checkPage (pages[2]);
}

TEST_CASE ("Motion's editor shows each mode whole in the default dock")
{
    Song song;
    phazite::ui::MotionEditor editor (song.getDevice());
    constexpr int dockWidth = 552, dockHeight = 268; // beside the Devices view's column
    CHECK (editor.getMinimumHeightAtWidth (dockWidth) <= dockHeight);
    phazite::test::TestWindow window (editor, dockWidth, dockHeight);

    for (const auto mode : { Motion::Mode::chorus, Motion::Mode::flanger, Motion::Mode::phaser })
    {
        CAPTURE ((int) mode);
        clickMode (editor, mode);
        CHECK (editor.getPicture().isShowing());
        CHECK (editor.getLocalBounds().contains (editor.getLocalArea (&editor.getPicture(), editor.getPicture().getLocalBounds())));
        CHECK (editor.getModeTabs()->isShowing());
    }
}

TEST_CASE ("Motion's Chorus page shows how far each copy lags, three a side as an ensemble, and its dot drags the delay and depth")
{
    Song song;
    phazite::ui::MotionEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // Its triangle at the start of its cycle: the left side's copy at the bottom of its sweep (3.5 ms, half of it shorter),
    // the right side's half a cycle ahead, at the top.
    tick();
    CHECK (editor.getShownVoices() == 1);
    CHECK (editor.getShownDelay (0, 0) == doctest::Approx (1.75).epsilon (0.001));
    CHECK (editor.getShownDelay (1, 0) == doctest::Approx (5.25).epsilon (0.001));

    // As the string machines' ensemble, three copies a side, each a third of a cycle after the one before, with the fast
    // sine on it.
    song.set ("voices", 2.0f);
    song.set ("shimmer", 1.0f);
    tick();
    CHECK (editor.getShownVoices() == 3);
    auto values = getMotionDefaults (Motion::Mode::chorus);
    values[Motion::voices] = 2.0f;
    values[Motion::shimmer] = 1.0f;
    const auto engine = Motion::getChorusValues (values);

    for (int voice = 0; voice < 3; ++voice)
        CHECK (editor.getShownDelay (0, voice)
               == doctest::Approx (devices::Chorus::getVoiceDelay (engine, voice / 3.0, voice)).epsilon (0.001));

    song.set ("voices", 0.0f);
    song.set ("shimmer", 0.0f);
    tick();

    // The dot at the delay's frequency (286 Hz, for 3.5 ms) dragged right and up: a shorter delay, more depth, one step.
    auto& picture = editor.getPicture();
    phazite::test::Mouse mouse (picture);
    const auto dot = editor.getDotPosition().roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot + juce::Point<int> (30, 0));
    mouse.dragTo (dot + juce::Point<int> (50, -30));
    mouse.release();
    CHECK (song.valueOf ("delay") < 3.0f);
    CHECK (song.valueOf ("depth") > 0.6f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Delay"));

    // The wheel turns the rate faster.
    mouse.pause();
    mouse.wheel (picture.getLocalBounds().getCentre(), 0.5f);
    CHECK (song.valueOf ("rate") > 0.5f);

    // The chorus's Feedback knob has 0 in the middle, as the others' do.
    auto* feedback = dynamic_cast<juce::Slider*> (editor.findControl ("feedback"));
    REQUIRE (feedback != nullptr);
    CHECK (feedback->isShowing());
    CHECK (feedback->getMinimum() == doctest::Approx (-1.0));
    CHECK (feedback->getMaximum() == doctest::Approx (1.0));
}

TEST_CASE ("Motion's Chorus page shows each voice in its part of the sweep, and Phase and Spread faint while it has one voice")
{
    Song song;
    phazite::ui::MotionEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    auto& ticker = *juce::SharedResourcePointer<phazite::ui::DisplayTicker>();

    // A control for each of the voices' parameters, each with its title: the count a list, the others knobs.
    for (const auto& [id, title] : { std::pair<const char*, const char*> { "voices", "Voices" },
                                     { "voicePhase", "Voice Phase" },
                                     { "spread", "Spread" },
                                     { "shimmer", "Shimmer" } })
    {
        CAPTURE (id);
        auto* control = editor.findControl (id);
        REQUIRE (control != nullptr);
        CHECK (control->isShowing());
        CHECK (control->getTitle() == juce::String (title));
    }

    CHECK (dynamic_cast<juce::ComboBox*> (editor.findControl ("voices")) != nullptr);
    CHECK (dynamic_cast<juce::Slider*> (editor.findControl ("spread")) != nullptr);

    // One voice a side has no other to keep a phase from or to share the sweep with: those two faint, the others not.
    ticker.tickNow();
    CHECK (editor.findControl ("voicePhase")->getAlpha() < 0.9f);
    CHECK (editor.findControl ("spread")->getAlpha() < 0.9f);
    CHECK (editor.findControl ("shimmer")->getAlpha() == doctest::Approx (1.0f));
    CHECK (editor.findControl ("voices")->getAlpha() == doctest::Approx (1.0f));

    // Four voices a side: Phase and Spread lit, four copies shown each side, all on one line while they share the sweep.
    song.set ("voices", 3.0f);
    ticker.tickNow();
    CHECK (editor.findControl ("voicePhase")->getAlpha() == doctest::Approx (1.0f));
    CHECK (editor.findControl ("spread")->getAlpha() == doctest::Approx (1.0f));
    CHECK (editor.getShownVoices() == 4);
    CHECK (editor.getHeadPosition (0, 0).y == doctest::Approx (editor.getHeadPosition (0, 3).y));

    // Spread gives each its part of the sweep: each copy shown within its own part, the later copies' longer, each on a line
    // of its own under the one before, all within the picture.
    song.set ("spread", 0.62f);
    song.set ("delay", 8.2f);
    song.set ("depth", 0.61f);
    ticker.tickNow();
    std::vector<float> motionValues;

    for (const auto& parameter : Motion::info.parameters)
        motionValues.push_back (song.valueOf (std::string (parameter.id).c_str()));

    const auto values = Motion::getChorusValues (motionValues);

    for (int channel = 0; channel < 2; ++channel)
    {
        for (int voice = 0; voice < 4; ++voice)
        {
            CAPTURE (channel);
            CAPTURE (voice);
            const auto reach = devices::Chorus::getVoiceReach (values, voice);
            CHECK (editor.getShownDelay (channel, voice) >= reach.shortest - 0.001);
            CHECK (editor.getShownDelay (channel, voice) <= reach.longest + 0.001);
            CHECK (editor.getPicture().getLocalBounds().toFloat().contains (editor.getHeadPosition (channel, voice)));

            if (voice > 0)
            {
                CHECK (editor.getHeadPosition (channel, voice).y > editor.getHeadPosition (channel, voice - 1).y + 1.0f);
                CHECK (devices::Chorus::getVoiceReach (values, voice).shortest
                       > devices::Chorus::getVoiceReach (values, voice - 1).shortest);
            }
        }
    }

    // Eight a side, the most: all shown, still within the picture; and the editor is as wide as with one, so that it fits
    // the dock the same.
    const auto width = editor.getWidth();
    song.set ("voices", 7.0f);
    ticker.tickNow();
    CHECK (editor.getShownVoices() == devices::Chorus::mostVoices);

    for (int voice = 0; voice < devices::Chorus::mostVoices; ++voice)
        CHECK (editor.getPicture().getLocalBounds().toFloat().contains (editor.getHeadPosition (1, voice)));

    CHECK (editor.getWidth() == width);
    phazite::test::checkChildrenApart (editor);

    // The picture repaints when a voice's control turns, without sound.
    phazite::test::dispatchPendingMessages();
    window.takeRepaintedArea();
    song.set ("voicePhase", 0.3f);
    ticker.tickNow();
    CHECK_FALSE (window.takeRepaintedArea().isEmpty());
}

TEST_CASE ("Motion's Flanger page shows how far each side's copy lags, and its dot drags the delay and feedback")
{
    Song song;
    song.set ("mode", (float) Motion::Mode::flanger);
    phazite::ui::MotionEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    REQUIRE (editor.getShownMode() == (int) Motion::Mode::flanger);

    // Its triangle at the start of its cycle: the left at the bottom of its sweep (3.5 ms, 2.5 octaves shorter at 50 %), the
    // right half a cycle ahead (Stereo at 100 %), at the top.
    tick();
    CHECK (editor.getShownDelay (0) == doctest::Approx (3.5 * std::exp2 (-2.5)).epsilon (0.001));
    CHECK (editor.getShownDelay (1) == doctest::Approx (3.5).epsilon (0.001));

    // The dot at the delay's frequency (286 Hz, for 3.5 ms), in the middle (no feedback), dragged right and up: a shorter
    // delay, more feedback, one step.
    auto& picture = editor.getPicture();
    phazite::test::Mouse mouse (picture);
    const auto dot = editor.getDotPosition().roundToInt();
    CHECK (std::abs (editor.getDotPosition().y - (float) picture.getHeight() / 2.0f) < (float) picture.getHeight() / 4.0f);
    mouse.press (dot);
    mouse.dragTo (dot + juce::Point<int> (30, 0));
    mouse.dragTo (dot + juce::Point<int> (50, -30));
    mouse.release();
    CHECK (song.valueOf ("delay") < 3.0f);
    CHECK (song.valueOf ("feedback") > 0.1f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Delay"));

    // Dragged to the bottom: the feedback's sign turned, as far as it goes.
    mouse.pause();
    mouse.press (editor.getDotPosition().roundToInt());
    mouse.dragTo ({ editor.getDotPosition().roundToInt().x, picture.getHeight() });
    mouse.release();
    CHECK (song.valueOf ("feedback") == doctest::Approx (-1.0f));

    // The wheel turns the depth.
    mouse.pause();
    mouse.wheel (picture.getLocalBounds().getCentre(), 0.5f);
    CHECK (song.valueOf ("depth") > 0.5f);
}

TEST_CASE ("Motion's Phaser page shows where the LFO has each side's notches, and its dot drags the frequency and feedback")
{
    Song song;
    song.set ("mode", (float) Motion::Mode::phaser);
    phazite::ui::MotionEditor editor (song.getDevice());
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());
    REQUIRE (editor.getShownMode() == (int) Motion::Mode::phaser);

    // Before it plays, its triangle at the start of its cycle: the left at the bottom of its sweep (800 Hz, two octaves down
    // at full depth, so one at 50 %), the right half a cycle ahead, at the top.
    tick();
    CHECK (editor.getShownCentre (0) == doctest::Approx (400.0).epsilon (0.001));
    CHECK (editor.getShownCentre (1) == doctest::Approx (1600.0).epsilon (0.001));

    // The dot dragged left and up: a lower frequency and more feedback, one step named after the frequency.
    auto& picture = editor.getPicture();
    phazite::test::Mouse mouse (picture);
    const auto dot = editor.getDotPosition().roundToInt();
    mouse.press (dot);
    mouse.dragTo (dot - juce::Point<int> (40, 0));
    mouse.dragTo (dot - juce::Point<int> (60, 30));
    mouse.release();
    CHECK (song.valueOf ("frequency") < 600.0f);
    CHECK (song.valueOf ("feedback") > 0.1f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Frequency"));
    phazite::undo (*song.song);
    CHECK (song.valueOf ("frequency") == doctest::Approx (800.0f));
    CHECK (song.valueOf ("feedback") == doctest::Approx (0.0f));

    // The wheel turns the depth.
    mouse.pause();
    mouse.wheel (picture.getLocalBounds().getCentre(), 0.5f);
    CHECK (song.valueOf ("depth") > 0.5f);
}
