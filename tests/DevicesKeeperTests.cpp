// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' part of the keeper (roadmap step S5.20, the devices' switch: model/base/internal/DevicesArea.h), now that the
// song's devices, containers and chains are its values' (SongValues.h: devices) and its tree holds none: a track's devices go
// with it and come back with it, a removal remembered once and forgotten at the next, a container's macros go with the
// container (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()), and what is made, copied or chosen gives the song its
// devices through the devices' setters, never through the keeper's safety net (countDevicesAdopted()), which takes in a
// track that comes in with them and refuses a lone one in a step. Each change one exact undo step (UndoCheck.h).
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
#include "model/base/internal/Schema.h"
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

// Whether the song's tree holds a device's element anywhere: it holds none (the devices' area does).
bool holdsDeviceElement (const juce::ValueTree& element)
{
    for (const auto& child : element)
        if (child.hasType (phazite::schema::DEVICE) || child.hasType (phazite::schema::CONTAINER) || child.hasType (phazite::schema::CHAIN)
            || holdsDeviceElement (child))
            return true;

    return false;
}
} // namespace

TEST_CASE ("Devices' keeper: a track's devices go with its deletion and come back with its Undo, stay through a folder, a "
           "container removed takes its macros, a duplicate and a preset's container are set through the setters, each one exact "
           "undo step")
{
    const phazite::test::TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& context = s.context;
    const auto& values = *song.getValues();
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
    const auto adopted = values.countDevicesAdopted();
    CHECK_FALSE (holdsDeviceElement (song.getState()));
    phazite::test::UndoCheck undo (context);

    // (a) The track deleted: its devices, its container with its chain and its devices, and its plug-in go with it, erased as
    // it goes (Tracks.h: eraseTrackHoldings()), the other track's stay; its Undo brings them back as they were.
    undo.checkSeveral ("Delete Track", [&] { phazite::deleteTracks (song, { track }, nullptr); });
    CHECK (phazite::getDevices (song, track).empty());

    for (const auto id : { equalizer, container, delay, plugin })
        CHECK_FALSE (phazite::hasDevice (song, id));

    CHECK (phazite::getContainerMacros (song, container).empty());
    CHECK (values.countHoldingsLeftWithKeepers() == 0);
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK (phazite::getDevicesValue (song) == before);
    CHECK (phazite::getModulationValue (song) == modulationBefore);

    // (a) Moved into a folder and out: the same devices on the same track, the same element given them back each time (JUCE
    // takes a track out and puts it in again).
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
    CHECK (values.countDevicesAdopted() == adopted);
    CHECK (values.countModulationAdopted() == 0);
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
    CHECK (values.countDevicesAdopted() == adopted);
    CHECK_FALSE (holdsDeviceElement (song.getState()));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Devices' keeper: a track deleted and brought back by Undo, whose device is then removed, is moved into a folder "
           "without it")
{
    // The keeper remembers a deleted track's devices with its element (none, as the deletion erased them first: Tracks.h,
    // eraseTrackHoldings()), and the Undo (which it leaves alone) brings them back itself: what it remembered must not come
    // back with the next move of the same element, once the device is gone.
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

TEST_CASE ("Devices' keeper: a lone device added to the master in a step is refused and one outside the history read in; a track "
           "that comes in with devices outside the history has them taken in by the tracks' safety net, one of an ID the song "
           "holds dropped")
{
    // The master's element, the one channel the song's tree still holds (a place: the tracks are values, roadmap step S5.20,
    // the tracks' switch), takes the lone device; a TRACK that comes in is the tracks' part of the keeper's (TracksArea.h),
    // which reads its devices through the devices' reader of a channel (DevicesArea.h: takeChannelDevicesIn()), outside the
    // history alone (in a step it is refused: SongValuesTracksKeeperTests.cpp).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto& values = *song.getValues();
    const auto track = s.firstTrack;
    const auto held = s.addEffect (track, "phaziteEq");

    // (c) A lone one in a step: refused (the element can't be taken out in the step that adds it: JUCE workaround W62,
    // docs/juce-workarounds.md), the song's devices as they were; its step's Undo takes the element out again.
    auto masterState = song.getState().getChildWithName (phazite::schema::MASTER);
    const auto loneID = DeviceID::fromRaw (song.makeID());
    phazite::DeviceValue loneValue;
    loneValue.id = loneID;
    loneValue.form = phazite::DeviceValue::Form::own;
    loneValue.type = dev::Delay::typeID;
    const auto lone = phazite::writeDeviceElement (loneValue);
    const auto before = phazite::getDevicesValue (song);
    const auto onMaster = phazite::getDevices (song, {});

    {
        const phazite::test::ExpectedAssertion refused ("DevicesArea.cpp");
        const phazite::UndoStep step (song, "A Lone Device");
        masterState.appendChild (lone.createCopy(), &song.getUndoManager());
#if JUCE_DEBUG
        CHECK (refused.getCount() == 1);
#endif
    }

    CHECK (phazite::getDevicesValue (song) == before);
    phazite::undo (song);
    CHECK_FALSE (holdsDeviceElement (song.getState()));

    // Outside the history: read in, as the song holds one, after the master's others, and taken out.
    const auto adoptedBefore = values.countDevicesAdopted();

    {
        const phazite::Song::OutsideHistory outside (song);
        masterState.appendChild (lone.createCopy(), nullptr);
    }

    auto expected = onMaster;
    expected.push_back (loneID);
    CHECK (values.countDevicesAdopted() == adoptedBefore + 1);
    CHECK (phazite::getDevices (song, {}) == expected);
    CHECK_FALSE (holdsDeviceElement (song.getState()));

    // (b) A track that comes in whole with its devices' elements, outside the history: the tracks' safety net takes it in with
    // its own device (counted, by the devices' count and as a track taken in), the one under an ID the song holds dropped (the
    // song's kept, on its track), none left in the tree. The track given a second device first (the lone one went to the
    // master).
    s.addEffect (track, "phaziteDelay");
    auto incoming = phazite::test::copyTrack (song, track);
    phazite::giveNewIDs (song, incoming);
    auto devicesIn = 0;

    for (auto child : incoming)
        if (child.hasType (phazite::schema::DEVICE) && devicesIn++ == 0)
            child.setProperty (phazite::schema::deviceID, (juce::int64) held.getRaw(), nullptr); // the song's device's ID

    REQUIRE (devicesIn == 2);
    const auto came = phazite::readTrackElementID (incoming);
    const auto adoptedThen = values.countDevicesAdopted();
    const auto tracksThen = values.countTracksAdopted();

    {
        const phazite::Song::OutsideHistory outside (song);
        song.getState().getChildWithName (phazite::schema::TRACKS).appendChild (incoming, nullptr);
    }

    CHECK (values.countDevicesAdopted() == adoptedThen + 1);
    CHECK (values.countTracksAdopted() == tracksThen + 1);
    REQUIRE (phazite::getDevices (song, came).size() == 1);
    CHECK (phazite::getDeviceChannel (song, held) == track);
    CHECK_FALSE (holdsDeviceElement (song.getState()));
    phazite::test::checkSong (song);
}
