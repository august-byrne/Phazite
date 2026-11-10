// SPDX-License-Identifier: AGPL-3.0-or-later
// What plug-ins keep of their own, stored into the song outside the undo history (model/plugins/internal/PluginInstances.h):
// their settings, their programs and the values set by hand of their parameters that automation moves, as saves,
// duplicates, containers and presets store them.
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
namespace schema = phazite::schema;
using phazite::test::TestPlugin;

// The stand-in's gain, as its settings stored in base 64 hold it.
float getStoredGain (const juce::String& base64)
{
    const auto settings = phazite::readBytes (base64);
    REQUIRE (settings.has_value());
    juce::MemoryInputStream stream (*settings, false);
    return stream.readFloat();
}

// The first plug-in on a track, not in a container.
phazite::DeviceID findStandIn (const phazite::Song& song, phazite::TrackID track)
{
    for (const auto& state : phazite::getDeviceStates (phazite::findTrackState (song, track)))
        if (phazite::isPluginState (state))
            return phazite::getDeviceID (state);

    return {};
}

// A track with the stand-in plug-in, whose gain its window turned, and another in a container. Nothing to undo.
struct Devices
{
    Devices()
    {
        auto& open = getSong();
        plugin = TestPlugin::add (open, track);
        inside = TestPlugin::add (open, track);
        container = phazite::groupIntoContainer (open, { inside });
        REQUIRE (container.isValid());
        open.getUndoManager().clearUndoHistory();

        TestPlugin::setGain (open, plugin, 0.25f);
        TestPlugin::setGain (open, inside, 0.5f);
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }
    phazite::PluginInstances& getPlugins() const { return phazite::getSession (song.context)->getPlugins(); }
    juce::ValueTree findState (phazite::DeviceID id) const { return phazite::findDeviceState (getSong(), id); }

    TestPlugin testPlugin; // known while the song is
    phazite::test::TestSong song;
    const phazite::TrackID track = song.firstTrack;
    phazite::DeviceID plugin;    // on the track
    phazite::DeviceID inside;    // in a container on it
    phazite::DeviceID container; // the container
};
} // namespace

TEST_CASE ("A save stores the plug-ins' settings, programs and values moved by automation into the song, without an undo step")
{
    Devices devices;
    auto& song = devices.getSong();

    // The plug-in's second program, chosen in its window, and then its gain turned there; its gain's automation, which moves
    // it away from its value set by hand as the song plays.
    auto* instance = phazite::findPluginInstance (song, devices.plugin);
    REQUIRE (instance != nullptr);
    instance->setCurrentProgram (1);
    REQUIRE (instance->getCurrentProgram() == 1);
    TestPlugin::setGain (song, devices.plugin, 0.25f);

    const phazite::ParameterRef gain { devices.plugin, TestPlugin::gainID };
    phazite::addPoint (song, gain, { phazite::BeatPosition(), 0.8f, 0.0f });
    song.getUndoManager().clearUndoHistory();
    phazite::getSession (devices.song.context)->update(); // a plan that moves it

    REQUIRE (devices.song.getProject().save().wasOk());
    CHECK_FALSE (song.getUndoManager().canUndo());

    // Its settings and its program, and the one in the container's settings.
    const auto element = devices.findState (devices.plugin);
    CHECK (getStoredGain (element[schema::pluginState].toString()) == doctest::Approx (0.25f));
    CHECK ((int) element[schema::pluginProgram] == 1);
    CHECK (getStoredGain (devices.findState (devices.inside)[schema::pluginState].toString()) == doctest::Approx (0.5f));

    // The gain's value set by hand, which its settings don't hold as set by hand while automation moves it, by the
    // plug-in's own ID for it. The tone, which nothing moves, has none.
    const auto value = element.getChildWithProperty (schema::valueParameter, juce::String (TestPlugin::gainID));
    REQUIRE (value.isValid());
    CHECK (value.hasType (schema::VALUE));
    CHECK ((float) value[schema::valueSetByHand] == doctest::Approx (0.25f));
    CHECK_FALSE (element.getChildWithProperty (schema::valueParameter, juce::String (TestPlugin::toneID)).isValid());
}

TEST_CASE ("A plug-in on the master keeps its instance as the song changes, and a save stores what it keeps of its own")
{
    // The master isn't among the tracks: its plug-ins were made anew from their stored state at each change of the song, and
    // a save never stored them.
    TestPlugin testPlugin;
    phazite::test::TestSong song;
    auto& open = *phazite::findSong (song.context);
    const auto plugin = TestPlugin::add (open, phazite::TrackID());
    auto* session = phazite::getSession (song.context);
    REQUIRE (session != nullptr);
    session->update();
    auto* instance = phazite::findPluginInstance (open, plugin);
    REQUIRE (instance != nullptr);

    // Its gain turned in its window, then a track added: the same instance plays on, its gain as it was.
    TestPlugin::setGain (open, plugin, 0.25f);
    song.addTrack ("Another");
    session->update();
    CHECK (phazite::findPluginInstance (open, plugin) == instance);
    CHECK (TestPlugin::getGain (open, plugin) == doctest::Approx (0.25f));

    REQUIRE (song.getProject().save().wasOk());
    CHECK (getStoredGain (phazite::findDeviceState (open, plugin)[schema::pluginState].toString()) == doctest::Approx (0.25f));
}

TEST_CASE ("Storing a plug-in's, or a track's plug-ins' states, writes them into the song without an undo step")
{
    Devices devices;
    auto& song = devices.getSong();
    auto& plugins = devices.getPlugins();
    const auto storedGain = [&devices] (phazite::DeviceID id)
    { return getStoredGain (devices.findState (id)[schema::pluginState].toString()); };

    // One plug-in: its own settings alone.
    plugins.store (devices.plugin);
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK (storedGain (devices.plugin) == doctest::Approx (0.25f));

    // A track's: those in its containers too.
    TestPlugin::setGain (song, devices.plugin, 0.75f);
    TestPlugin::setGain (song, devices.inside, 0.6f);
    plugins.storeTrack (devices.track);
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK (storedGain (devices.plugin) == doctest::Approx (0.75f));
    CHECK (storedGain (devices.inside) == doctest::Approx (0.6f));

    // The song's store, which a copy of the track asks for, is the same.
    TestPlugin::setGain (song, devices.plugin, 0.1f);
    TestPlugin::setGain (song, devices.inside, 0.2f);
    song.storeDeviceStates (devices.track.getRaw());
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK (storedGain (devices.plugin) == doctest::Approx (0.1f));
    CHECK (storedGain (devices.inside) == doctest::Approx (0.2f));

    // And every device's.
    TestPlugin::setGain (song, devices.plugin, 0.3f);
    plugins.storeAll();
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK (storedGain (devices.plugin) == doctest::Approx (0.3f));
}

TEST_CASE ("Duplicates, containers and presets take devices' states as they are, and only their own steps are undone")
{
    Devices devices;
    auto& song = devices.getSong();
    auto& undoManager = song.getUndoManager();
    const auto pluginID = devices.plugin;
    const auto storedGain = [&devices, pluginID] { return getStoredGain (devices.findState (pluginID)[schema::pluginState].toString()); };

    // A duplicated track's copy of the plug-in plays its settings as they are now. Undo takes the copy away and leaves the
    // original's stored settings as they are, with nothing else to undo.
    TestPlugin::setGain (song, pluginID, 0.75f);
    {
        const phazite::UndoStep step (song, "Duplicate Track");
        const auto copies = phazite::duplicateTracks (song, { devices.track }, nullptr);
        REQUIRE (copies.size() == 1);
        const auto copied = findStandIn (song, copies[0]);
        REQUIRE (copied.isValid());
        CHECK (copied != pluginID);
        CHECK (TestPlugin::getGain (song, copied) == doctest::Approx (0.75f));
    }

    phazite::undo (song);
    CHECK_FALSE (undoManager.canUndo());
    CHECK (phazite::getTrackIDs (song).size() == 1);
    CHECK (storedGain() == doctest::Approx (0.75f));

    // Grouped into a container, the plug-in keeps its settings as they are now; undo puts it back on the track.
    TestPlugin::setGain (song, pluginID, 0.6f);
    {
        const phazite::UndoStep step (song, "Group into Container");
        REQUIRE (phazite::groupIntoContainer (song, { pluginID }).isValid());
    }

    phazite::undo (song);
    CHECK_FALSE (undoManager.canUndo());
    CHECK_FALSE (phazite::findContainerOf (song, pluginID).isValid());
    CHECK (storedGain() == doctest::Approx (0.6f));

    // A preset of the plug-in, or of its track, holds its settings as they are now; making one is no undo step.
    TestPlugin::setGain (song, pluginID, 0.4f);
    const auto devicePreset = phazite::makeDevicePreset (song, pluginID);
    CHECK (getStoredGain (devicePreset[schema::preset::device][schema::preset::state].toString()) == doctest::Approx (0.4f));
    TestPlugin::setGain (song, pluginID, 0.3f);
    phazite::makeTrackPreset (song, devices.track);
    CHECK (storedGain() == doctest::Approx (0.3f));
    CHECK_FALSE (undoManager.canUndo());
}
