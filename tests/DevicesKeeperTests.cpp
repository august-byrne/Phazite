// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices as values of the song (model/base/internal/DevicesArea.h): a track's
// devices go with it and come back with it, a container's macros go with the container (model/mixer/internal/Mixer.h:
// forgetWhatNamesTheGone()), and what is made, copied or chosen gives the song its devices through the devices' setters.
// Each change one exact undo step (UndoCheck.h).
//   phazite_tests.exe --test-case="Devices' keeper*"
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/delay/Delay.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/ProjectContext.h"

#include <functional>
#include <vector>

namespace
{
namespace dev = phazite::devices;
using phazite::DeviceID;
using phazite::TrackID;
} // namespace

TEST_CASE ("Devices' keeper: a track's devices go with its deletion and come back with its Undo, stay through a folder, a "
           "container removed takes its macros, a duplicate and a preset's container are set through the setters, each one exact "
           "undo step")
{
    const phazite::test::TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto equalizer = s.addEffect (track, "phaziteEq");
    const auto container = phazite::addContainer (context, track);
    REQUIRE (container.isValid());
    const auto delay = phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), dev::Delay::typeID);
    DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Add Device");
        plugin = phazite::test::TestPlugin::add (song, track);
    }

    // The plug-in loaded and its first state in the song before the steps are compared: its helper's answer comes when it
    // comes, and one that arrived in the middle of the walk below was a state the walk did not expect on its device.
    s.waitForPlugins();
    auto* instance = phazite::findPluginInstance (song, plugin);
    REQUIRE (instance != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [instance] { return ! instance->isStateUnasked() && ! instance->isAskingState() && instance->getLastState().getSize() > 0; }));
    song.storeDeviceStates();

    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() == 8);
    s.setMacroValue (macros[0], 0.5f);
    const auto before = phazite::getDevicesValue (song);
    const auto modulationBefore = phazite::getModulationValue (song);
    phazite::test::UndoCheck undo (context);

    // (a) The track deleted: its devices, its container with its chain and its devices, and its plug-in go with it, erased as
    // it goes (Tracks.h: eraseTrackHoldings()), the other track's stay; its Undo brings them back as they were.
    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    CHECK (phazite::getDevices (song, track).empty());

    for (const auto id : { equalizer, container, delay, plugin })
        CHECK_FALSE (phazite::hasDevice (song, id));

    CHECK (phazite::getContainerMacros (song, container).empty());
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::getDevicesValue (song) == before);
    CHECK (phazite::getModulationValue (song) == modulationBefore);

    // (a) Moved into a folder and out: the same devices on the same track (a move changes the track's place alone).
    TrackID folder;
    undo.checkSeveral ("a folder", [&] { folder = phazite::insertFolder (song, other, nullptr); });
    undo.checkSeveral ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    REQUIRE (phazite::getParentFolder (song, track) == folder);
    CHECK (phazite::getDevicesValue (song) == before);
    CHECK (phazite::getModulationValue (song) == modulationBefore);
    undo.checkSeveral ("Ungroup Folder", [&] { phazite::ungroupFolder (song, folder); });
    CHECK (phazite::getDevicesValue (song) == before);
    CHECK (phazite::getModulationValue (song) == modulationBefore);

    // A container removed: its chain and its devices go with it, and its macros in that step (forgetWhatNamesTheGone());
    // its Undo brings all of them back.
    undo.checkSeveral ("Remove Device", [&] { phazite::removeDevice (song, container); });
    CHECK_FALSE (phazite::hasDevice (song, container));
    CHECK_FALSE (phazite::hasDevice (song, delay));
    CHECK (phazite::getContainerMacros (song, container).empty());
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::getDevicesValue (song) == before);
    CHECK (phazite::getModulationValue (song) == modulationBefore);

    // Duplicated: the copy's devices under new IDs, its container's macros with them, none taken in by the keeper.
    std::vector<TrackID> copies;
    undo.checkSeveral ("Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); });
    REQUIRE (copies.size() == 1);
    const auto copied = phazite::getDevices (song, copies.front());
    REQUIRE (copied.size() == phazite::getDevices (song, track).size());
    const auto copiedContainer = copied[1];
    CHECK (phazite::isContainer (song, copiedContainer));
    CHECK (copiedContainer != container);
    CHECK (phazite::getContainerMacros (song, copiedContainer).size() == 8);
    phazite::test::checkSong (song);

    // A preset of the container added to the other track: its chain and its devices set with it, its macros too, none taken in.
    const auto preset = phazite::makeDevicePreset (song, container);
    DeviceID made;
    undo.check ("Add Device",
                [&]
                {
                    const phazite::UndoStep step (song, "Add Device");
                    made = phazite::addDeviceFromPreset (song, phazite::getPluginHost (phazite::test::getEngineContext()), other, preset,
                                                         nullptr);
                });
    REQUIRE (made.isValid());
    CHECK (phazite::isContainer (song, made));
    CHECK (phazite::getContainerDevices (song, made).size() == 1);
    CHECK (phazite::getContainerMacros (song, made).size() == 8);
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Devices' keeper: a track deleted and brought back by Undo, whose device is then removed, is moved into a folder "
           "without it")
{
    // The deletion erases a deleted track's devices first (Tracks.h: eraseTrackHoldings()), and its Undo brings them back:
    // none must come back with the next move of the track, once the device is gone.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto equalizer = s.addEffect (track, "phaziteEq");
    phazite::test::UndoCheck undo (context);

    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    undo.stepBackOrForth ("Undo Delete Track", [&] { phazite::undo (song); });
    REQUIRE (phazite::hasDevice (song, equalizer));

    undo.checkSeveral ("Remove Device", [&] { phazite::removeDevice (song, equalizer); });
    REQUIRE_FALSE (phazite::hasDevice (song, equalizer));

    TrackID folder;
    undo.checkSeveral ("a folder", [&] { folder = phazite::insertFolder (song, other, nullptr); });
    undo.checkSeveral ("Into the Folder", [&] { phazite::moveTrackIntoFolder (song, track, folder); });
    REQUIRE (phazite::getParentFolder (song, track) == folder);
    CHECK_FALSE (phazite::hasDevice (song, equalizer));
    CHECK (phazite::getDevices (song, track).empty());
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    CHECK_FALSE (phazite::hasDevice (song, equalizer));
    phazite::test::checkSong (song);
}
