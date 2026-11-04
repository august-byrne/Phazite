// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's parameters through its updates: the song refers to each by the plug-in's own ID for it (a VST3's number),
// which an update keeps wherever it moves the parameter in the plug-in's list. A project saved with one version of the
// stand-in plug-in, opened with another.
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::test::TestPlugin;

// A project with the stand-in on its track, in its first version (Gain, then Tone): Gain set by hand to 0.3, then
// automated from 0.2 to 0.8 and shown in a lane, and Tone moved by an LFO.
struct Saved
{
    Saved()
    {
        auto& song = getSong();
        plugin = TestPlugin::add (song, track);
        REQUIRE (phazite::findPluginInstance (song, plugin) != nullptr);
        gain = { plugin, TestPlugin::gainID };
        tone = { plugin, TestPlugin::toneID };

        phazite::setValueSetByHand (song, gain, 0.3f);
        phazite::addPoint (song, gain, { phazite::BeatPosition::fromBeats (0.0), 0.2f });
        phazite::addPoint (song, gain, { phazite::BeatPosition::fromBeats (4.0), 0.8f });
        phazite::showLane (song, track, gain);
        const auto lfo = phazite::addModulator (song, track, phazite::ModulatorType::lfo);
        REQUIRE (lfo.isValid());
        phazite::setModulation (song, lfo, tone, 0.5f);
    }

    phazite::Song& getSong() const { return *phazite::findSong (test.context); }

    // What Phazite's engine plays of the song now.
    const phazite::SongPlayer& play() const
    {
        auto* session = phazite::getSession (test.context);
        REQUIRE (session != nullptr);
        session->update();
        return session->getPlayer();
    }

    TestPlugin testPlugin; // known while the song is
    phazite::test::TestSong test;
    const phazite::TrackID track = test.firstTrack;
    phazite::DeviceID plugin;
    phazite::ParameterRef gain, tone;
};
} // namespace

TEST_CASE ("A song refers to a plug-in's parameters by the plug-in's own IDs, which its update keeps")
{
    namespace schema = phazite::schema;
    Saved saved;

    // What refers to Gain names it by the plug-in's "1": its value set by hand and its automation.
    const auto element = phazite::findDeviceState (saved.getSong(), saved.plugin);
    CHECK (element.getChildWithName (schema::VALUE)[schema::valueParameter].toString() == juce::String (TestPlugin::gainID));
    CHECK (phazite::getCurveParameter (phazite::findCurveState (saved.getSong(), saved.gain)) == saved.gain);

    // Version 2 has Tone first, then Gain, then a new Drive: Gain is second in its list now, under the same ID.
    saved.testPlugin.setVersion (2);
    saved.test.reopen();
    auto& song = saved.getSong();
    auto* instance = phazite::findPluginInstance (song, saved.plugin);
    REQUIRE (instance != nullptr);
    REQUIRE (instance->findParameter (TestPlugin::gainID) == 1);
    const phazite::ParameterRef drive { saved.plugin, "2" };
    REQUIRE (phazite::describeParameter (song, saved.gain).has_value());
    REQUIRE (phazite::describeParameter (song, drive).has_value());
    CHECK (phazite::describeParameter (song, saved.gain)->name == juce::String ("Gain"));
    CHECK (phazite::describeParameter (song, saved.tone)->name == juce::String ("Tone"));
    CHECK (phazite::describeParameter (song, drive)->name == juce::String ("Drive"));

    // Gain keeps its automation, its value set by hand and its lane; Tone its LFO; Drive gets nothing.
    CHECK (phazite::getPoints (song, saved.gain).size() == 2);
    CHECK (phazite::getValueSetByHand (song, saved.gain) == doctest::Approx (0.3f));
    CHECK (instance->getParameterValue (1) == doctest::Approx (0.3f));
    CHECK (phazite::getModulations (song, saved.gain).empty());
    CHECK (phazite::getPoints (song, saved.tone).empty());
    CHECK (phazite::getModulations (song, saved.tone).size() == 1);
    CHECK (phazite::getPoints (song, drive).empty());
    CHECK (phazite::getModulations (song, drive).empty());

    const auto lanes = phazite::getLanes (song, saved.track);
    REQUIRE (lanes.size() == 1);
    CHECK (lanes[0].parameter == saved.gain);

    // What Phazite's engine plays moves Gain by its automation and Tone by its LFO, and leaves Drive alone.
    const auto& player = saved.play();
    CHECK (player.getPlayedValue (saved.gain, false).has_value());
    CHECK (player.isMoved (saved.gain));
    CHECK (player.isMoved (saved.tone));
    CHECK_FALSE (player.isMoved (drive));
}

TEST_CASE ("A parameter that a plug-in's update removed plays nothing, and the song keeps what refers to it")
{
    Saved saved;

    // Version 3 has Tone alone, first as before: Gain is gone.
    saved.testPlugin.setVersion (3);
    saved.test.reopen();
    auto& song = saved.getSong();
    auto* instance = phazite::findPluginInstance (song, saved.plugin);
    REQUIRE (instance != nullptr);
    REQUIRE (instance->getParameterInfos().size() == 1);
    CHECK (instance->findParameter (TestPlugin::gainID) < 0);
    CHECK (instance->findParameter (TestPlugin::toneID) == 0);

    // Gain's automation and value set by hand stay the song's, for a version that has it again, but land on nothing:
    // Tone keeps its own value and its LFO, and none of Gain's automation.
    CHECK (phazite::getPoints (song, saved.gain).size() == 2);
    CHECK (phazite::getValueSetByHand (song, saved.gain) == doctest::Approx (0.3f));
    CHECK (phazite::getPoints (song, saved.tone).empty());
    CHECK (instance->getParameterValue (0) == doctest::Approx (0.5f));
    CHECK (phazite::getModulations (song, saved.tone).size() == 1);

    // What Phazite's engine plays has no Gain to move.
    const auto& player = saved.play();
    CHECK_FALSE (player.getPlayedValue (saved.gain, false).has_value());
    CHECK (player.getPlayedValue (saved.tone, false).has_value());
    CHECK (player.isMoved (saved.tone));
}
