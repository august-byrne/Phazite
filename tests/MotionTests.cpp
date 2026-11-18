// SPDX-License-Identifier: AGPL-3.0-or-later
// Motion (src/devices/motion/Motion.h) and its editor (src/ui/devices/MotionEditor.h): what it is; each mode playing what its
// engine plays, through its runner, as the device it was before played (the guard of the mapping from Motion's values to
// each engine's, and of the one meaning of Mix); a change of mode faded without a click, the same in blocks of any size;
// its tail; and its editor's tabs, each mode's page and picture, their dots and wheels, and the default dock.
#include "MotionTestHelpers.h"
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
// playing from its start at 120 beats a minute in 4/4.
juce::AudioBuffer<float> playStereo (const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> processor,
                                     std::span<const float> values, const juce::AudioBuffer<float>& sound, int blockSize, bool songPlays)
{
    engine::DeviceRunner runner (info, std::move (processor));
    runner.prepare (usualRate, blockSize);
    auto out = sound;
    const auto beatsPerSample = 120.0 / 60.0 / usualRate;

    for (int start = 0; start < out.getNumSamples(); start += blockSize)
    {
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

// The largest difference between two sounds, sample for sample, on either side.
float largestDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    auto largest = 0.0f;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < a.getNumSamples(); ++i)
            largest = std::max (largest, std::abs (a.getSample (channel, i) - b.getSample (channel, i)));

    return largest;
}

// A Motion on a track of its own song, open in a context of its own whose session plays it.
struct Song
{
    Song()
    {
        phazite::setSong (context, song.get());
        device = phazite::insertDeviceState (*song, phazite::getTrackIDs (*song)[0], phazite::makeOwnDeviceState (*song, Motion::typeID));
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

struct Setting
{
    std::string name;
    std::function<void (std::vector<float>&)> set; // on Motion's values, from its defaults in the mode
    bool songPlays = false;
};

// Plays Motion with its values, and the mode's engine alone with them mapped to its own, each through a runner of its own,
// and checks that the two agree sample for sample.
template <typename Engine, typename Map>
void checkMode (Motion::Mode mode, const Setting& setting, Map&& map)
{
    CAPTURE (setting.name);
    auto values = getMotionDefaults (mode);
    setting.set (values);
    const auto mapped = map (values);
    const std::vector<float> engineValues (mapped.begin(), mapped.end());
    REQUIRE (engineValues[(size_t) Engine::mix] <= 1.0f); // what the device it was could be set to

    const auto sound = makeStereoSound ((int) usualRate);

    for (const auto blockSize : { 256, 37 })
    {
        CAPTURE (blockSize);
        const auto before = playStereo (Engine::info, std::make_unique<Engine>(), engineValues, sound, blockSize, setting.songPlays);
        const auto now = playStereo (Motion::info, std::make_unique<Motion>(), values, sound, blockSize, setting.songPlays);
        CHECK (largestDifference (before, now) < 1.0e-5f);

        // It plays something: not the sound as it came, nor silence.
        CHECK (largestDifference (now, sound) > 1.0e-3f);
    }
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
    const auto& info = Motion::info;

    SUBCASE ("Chorus")
    {
        const std::vector<Setting> settings {
            { "its defaults", [] (std::vector<float>&) {} },
            { "Solid Bass Chorus",
              [&] (std::vector<float>& v)
              {
                  v[Motion::voices] = choiceOf (info, Motion::voices, "3");
                  v[Motion::delay] = 5.0f;
                  v[Motion::rate] = 0.6f;
                  v[Motion::spread] = 0.4f;
                  v[Motion::highCut] = 12000.0f;
                  v[Motion::safeBass] = 150.0f;
              } },
            { "feedback at its most, eight voices with shimmer",
              [] (std::vector<float>& v)
              {
                  v[Motion::voices] = 7.0f;
                  v[Motion::feedback] = 1.0f;
                  v[Motion::shimmer] = 1.0f;
                  v[Motion::delay] = 12.0f;
                  v[Motion::depth] = 1.0f;
              } },
            { "feedback at its least",
              [] (std::vector<float>& v)
              {
                  v[Motion::voices] = 2.0f;
                  v[Motion::feedback] = -1.0f;
                  v[Motion::highCut] = 4000.0f;
              } },
            { "in time with the song, Safe Bass on, a Mix below full",
              [] (std::vector<float>& v)
              {
                  v[Motion::sync] = 1.0f;
                  v[Motion::division] = (float) devices::dsp::barNoteValue - 2.0f;
                  v[Motion::safeBass] = 300.0f;
                  v[Motion::mix] = 0.3f;
                  v[Motion::stereo] = 0.4f;
              },
              true },
        };

        for (const auto& setting : settings)
            checkMode<devices::Chorus> (Motion::Mode::chorus, setting, Motion::getChorusValues);
    }

    SUBCASE ("Flanger")
    {
        const std::vector<Setting> settings {
            { "its defaults", [] (std::vector<float>&) {} },
            { "Jet Sweep",
              [&] (std::vector<float>& v)
              {
                  v[Motion::delay] = 5.0f;
                  v[Motion::depth] = 0.9f;
                  v[Motion::sync] = 1.0f;
                  v[Motion::division] = choiceOf (info, Motion::division, "2 bars");
                  v[Motion::rate] = 0.25f;
                  v[Motion::feedback] = 0.7f;
                  v[Motion::stereo] = 0.25f;
              },
              true },
            { "feedback at its most, the longest delay",
              [] (std::vector<float>& v)
              {
                  v[Motion::feedback] = 1.0f;
                  v[Motion::delay] = 25.0f;
                  v[Motion::rate] = 3.0f;
              } },
            { "feedback at its least, a sine",
              [] (std::vector<float>& v)
              {
                  v[Motion::feedback] = -1.0f;
                  v[Motion::shape] = 0.0f;
                  v[Motion::delay] = 0.5f;
              } },
            { "Safe Bass on, a Mix below full",
              [] (std::vector<float>& v)
              {
                  v[Motion::safeBass] = 200.0f;
                  v[Motion::feedback] = 0.5f;
                  v[Motion::mix] = 0.2f;
              } },
        };

        for (const auto& setting : settings)
            checkMode<devices::Flanger> (Motion::Mode::flanger, setting, Motion::getFlangerValues);
    }

    SUBCASE ("Phaser")
    {
        const std::vector<Setting> settings {
            { "its defaults", [] (std::vector<float>&) {} },
            { "Classic Phase",
              [&] (std::vector<float>& v)
              {
                  v[Motion::frequency] = 700.0f;
                  v[Motion::depth] = 0.75f;
                  v[Motion::sync] = 1.0f;
                  v[Motion::division] = choiceOf (info, Motion::division, "1/2");
                  v[Motion::shape] = 0.0f;
                  v[Motion::feedback] = 0.35f / Motion::phaserFeedback;
                  v[Motion::stereo] = 0.0f;
              },
              true },
            { "feedback at its most, six notches",
              [] (std::vector<float>& v)
              {
                  v[Motion::feedback] = 1.0f;
                  v[Motion::notches] = 5.0f;
                  v[Motion::spacing] = 0.8f;
              } },
            { "feedback at its least, one notch",
              [] (std::vector<float>& v)
              {
                  v[Motion::feedback] = -1.0f;
                  v[Motion::notches] = 0.0f;
                  v[Motion::spacing] = 0.2f;
                  v[Motion::rate] = 4.0f;
              } },
            { "Safe Bass on, a Mix below full",
              [] (std::vector<float>& v)
              {
                  v[Motion::safeBass] = 150.0f;
                  v[Motion::feedback] = 0.4f;
                  v[Motion::mix] = 0.35f;
              } },
        };

        for (const auto& setting : settings)
            checkMode<devices::Phaser> (Motion::Mode::phaser, setting, Motion::getPhaserValues);
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
