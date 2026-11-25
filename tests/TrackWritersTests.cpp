// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks' writers on values (model/arrangement/internal/Tracks.h): every writer of
// the tracks' own code works its next value out and sets it through the tracks' setters (setTrack(), setNewTracks(),
// eraseTrack(), setTrackPlace(), setTrackListOrder(), setTrackFields(), and the channels' narrow setters, setChannelVolume()
// and setChannelPan()), which while the song's tree holds the tracks write it by what differs. Their own cases (a set equal to
// what the song holds writes nothing; each refusal; a write outside the history makes no undo action; a move is the same
// element, a plug-in on the moved track keeping its instance and a height set after the move kept through its Undo and Redo);
// and the writers moved onto them, each one exact undo step with its name (UndoCheck), the song checked (checkSong()), the
// track after as expected: a track and a folder added, renamed, coloured and moved; three tracks grouped into a folder and
// ungrouped; a track with clips, devices, sends and modulators moved into a folder, into a folder in it and out, keeping
// everything; a bus made plain and back with its fader automated; a folder of two tracks duplicated; a pad given its own
// track and brought back; a return added with an effect; a track added with the key set last, its Undo and Redo; an input,
// a monitoring and a volume; and a track's preset of a track without a colour, which loads without one. A track and a folder
// of two tracks duplicated keep their switches and their view (mute, solo, write mode, height, collapsed, pinned, drum mode
// and rows, lanes naming the copy's devices and fader), a frozen original's copy not frozen; given outside the history before
// the copies come in, so that a copy unmuted since stays unmuted through the step's Undo and Redo, none taken in by a
// safety net. A track "moved" to where it is makes no step, in a folder with lanes. The master set whole and refused without
// an ID; a track moved into a bus by its output and out; a track added for an input; tracks ensured; tracks and a folder deleted; the
// master's key, which writes nothing; and a removed drum sampler's pads' tracks, one deleted, one with a clip made plain.
//   phazite_tests.exe --source-file=*TrackWritersTests*
#include "EnginePlayback.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace schema = phazite::schema;
using phazite::TrackID;
using phazite::TrackValue;

// A step of the test's own, around what isn't a change of its own (a setter, a writer that joins its caller's step).
std::function<void()> inStep (phazite::Song& song, const juce::String& name, std::function<void()> change)
{
    return [&song, name, change = std::move (change)]
    {
        const phazite::UndoStep step (song, name);
        change();
    };
}

// What the song holds of a track, which must be there.
TrackValue held (const phazite::Song& song, TrackID id)
{
    const auto value = phazite::findHeldTrack (song, id);
    REQUIRE (value.has_value());
    return *value;
}

// The name of the step a change made last.
juce::String lastStep (phazite::Song& song)
{
    return song.getUndoManager().getUndoDescription();
}

// A track's lanes as its copy must show them: each of the original's devices' parameters named by the copy's device in its
// place among the copy's devices, and the original's fader by the copy's.
std::vector<phazite::AutomationLane> getLanesOfCopy (const phazite::Song& song, TrackID original, TrackID copy)
{
    const auto from = phazite::getDevices (song, original);
    const auto to = phazite::getDevices (song, copy);
    REQUIRE (from.size() == to.size());
    const auto originalFader = held (song, original).fader;
    const auto copyFader = held (song, copy).fader;
    auto lanes = phazite::getLanes (song, original);

    for (auto& lane : lanes)
    {
        if (lane.parameter.device == originalFader)
            lane.parameter.device = copyFader;

        for (size_t i = 0; i < from.size(); ++i)
            if (lane.parameter.device == from[i])
                lane.parameter.device = to[i];
    }

    return lanes;
}

// A copy's switches and view, read through the model by ID, as its original's: its mute, solo and write mode, its height,
// collapsed and pinned, its drum mode and rows, and its lanes (naming its own devices and fader); but never frozen (a copy
// plays live: the frozen file is the original's).
void checkKeepsSwitchesAndView (const phazite::Song& song, TrackID original, TrackID copy)
{
    const auto name = phazite::getTrackName (song, original);
    CAPTURE (name);
    CHECK (phazite::isTrackMuted (song, copy, false) == phazite::isTrackMuted (song, original, false));
    CHECK (phazite::isTrackSoloed (song, copy) == phazite::isTrackSoloed (song, original));
    CHECK (phazite::getAutomationMode (song, copy) == phazite::getAutomationMode (song, original));
    CHECK (phazite::getSavedTrackHeight (song, copy) == phazite::getSavedTrackHeight (song, original));
    CHECK (phazite::isCollapsed (song, copy) == phazite::isCollapsed (song, original));
    CHECK (phazite::isPinned (song, copy) == phazite::isPinned (song, original));
    CHECK (phazite::isDrumTrack (song, copy) == phazite::isDrumTrack (song, original));
    CHECK (phazite::getDrumRowOrder (song, copy) == phazite::getDrumRowOrder (song, original));
    CHECK (phazite::getHiddenDrumRows (song, copy) == phazite::getHiddenDrumRows (song, original));
    CHECK_FALSE (phazite::isTrackFrozen (song, copy));

    const auto expected = getLanesOfCopy (song, original, copy);
    const auto lanes = phazite::getLanes (song, copy);
    REQUIRE (lanes.size() == expected.size());

    for (size_t i = 0; i < lanes.size(); ++i)
    {
        CHECK (lanes[i].parameter == expected[i].parameter);
        CHECK (lanes[i].height == expected[i].height);
        CHECK (lanes[i].collapsed == expected[i].collapsed);
    }
}

} // namespace

TEST_CASE ("Track writers: the setters write nothing for what the song holds, refuse what it can't hold, and write outside the "
           "history without an undo action")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& undo = song.getUndoManager();
    const auto track = s.firstTrack;
    const auto folder = phazite::addTrackAtEnd (song, true, nullptr);
    const auto inner = phazite::addTrackAtEnd (song, true, nullptr);
    inStep (song, "Into", [&] { phazite::moveTrackIntoFolder (song, inner, folder); })();
    REQUIRE (phazite::getParentFolder (song, inner) == folder);

    // (a) A value the song holds: no write, no undo action.
    {
        const phazite::UndoStep step (song, "Nothing");
        CHECK_FALSE (phazite::setTrack (song, held (song, track)));
        CHECK_FALSE (phazite::setTrack (song, held (song, phazite::getMasterID (song))));
        CHECK_FALSE (phazite::setTrackFields (song, track, [] (TrackValue&) {}));
        CHECK_FALSE (phazite::setTrackPlace (song, inner, folder, 0));
        phazite::setChannelVolume (song, track, phazite::getChannelVolume (song, track));
        phazite::setChannelPan (song, track, phazite::getChannelPan (song, track));
        CHECK (undo.getNumActionsInCurrentTransaction() == 0);
    }

    // (b) What the song can't hold: refused, nothing written.
    {
        const phazite::UndoStep step (song, "Refused");
        CHECK_FALSE (phazite::setTrackPlace (song, folder, folder, 0)); // into itself
        CHECK_FALSE (phazite::setTrackPlace (song, folder, inner, 0));  // into a folder in it
        CHECK_FALSE (phazite::setTrackPlace (song, inner, track, 0));   // into a track that is no folder
        CHECK_FALSE (phazite::setTrack (song, TrackValue()));           // without an ID

        auto asMaster = held (song, track);
        asMaster.id = phazite::getMasterID (song);
        CHECK_FALSE (phazite::setTrack (song, asMaster)); // a track with the master's ID

        auto asTrack = held (song, folder);
        asTrack.kind = TrackValue::Kind::track;
        CHECK_FALSE (phazite::setTrack (song, asTrack)); // a folder made a track while it holds one

        auto asFader = held (song, track);
        asFader.id = TrackID::fromRaw (held (song, track).fader.getRaw());
        CHECK_FALSE (phazite::setTrack (song, asFader)); // a new track of an ID the song holds as a fader

        CHECK_FALSE (phazite::eraseTrack (song, phazite::getMasterID (song)));
        CHECK_FALSE (phazite::setTrackListOrder (song, folder, { track }));
        CHECK (undo.getNumActionsInCurrentTransaction() == 0);
    }

    // (c) Outside the history: written, no action.
    undo.clearUndoHistory();

    {
        const phazite::Song::OutsideHistory outside (song);
        CHECK (phazite::setTrackFields (song, track, [] (TrackValue& value) { value.name = juce::String ("Outside"); }));
        phazite::setChannelVolume (song, track, -6.0f);
        CHECK (phazite::setTrackPlace (song, track, {}, -1));
    }

    CHECK_FALSE (undo.canUndo());
    CHECK (phazite::getTrackName (song, track) == juce::String ("Outside"));
    CHECK (phazite::getChannelVolume (song, track) == doctest::Approx (-6.0f));
    CHECK (phazite::getTrackIDs (song).back() == track);
    phazite::test::checkSong (song);

    // (d) A list in another order, and a value set whole: what was given.
    const auto second = s.addTrack ("Second");
    const auto heldTracks = phazite::getHeldTracks (song);
    const auto* list = heldTracks.findList ({});
    REQUIRE (list != nullptr);
    auto reversed = immer::flex_vector<TrackID>();

    for (const auto id : *list)
        reversed = reversed.push_front (id);

    inStep (song, "Order", [&] { CHECK (phazite::setTrackListOrder (song, {}, reversed)); })();
    CHECK (*phazite::getHeldTracks (song).findList ({}) == reversed);

    auto changed = held (song, second);
    changed.name = juce::String ("Changed");
    changed.colour = 0xff112233u;
    changed.key = phazite::Key { 4, phazite::Scale::minor };
    changed.parent = folder;
    inStep (song, "Set", [&] { CHECK (phazite::setTrack (song, changed, 0)); })();
    CHECK (held (song, second) == changed);
    CHECK (phazite::getTracksInFolder (song, folder).front() == second);
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: a track moved into a folder and out is the same element: its plug-in keeps its instance, and a "
           "height set after the move stays through the move's Undo and Redo")
{
    using phazite::test::TestPlugin;
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto folder = phazite::addTrackAtEnd (song, true, nullptr);
    phazite::DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Add Device");
        plugin = TestPlugin::add (song, track);
    }

    s.waitForPlugins();
    auto* instance = phazite::findPluginInstance (song, plugin);
    REQUIRE (instance != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [instance] { return ! instance->isStateUnasked() && ! instance->isAskingState() && instance->getLastState().getSize() > 0; }));
    song.storeDeviceStates();

    inStep (song, "Move Track", [&] { phazite::moveTrackIntoFolder (song, track, folder); })();
    CHECK (phazite::getParentFolder (song, track) == folder);
    CHECK (phazite::findPluginInstance (song, plugin) == instance);

    // A height set after the move, outside the history (the view's, by the track's ID).
    phazite::setSavedTrackHeight (song, track, 120);
    phazite::undo (song);
    CHECK_FALSE (phazite::getParentFolder (song, track).isValid());
    CHECK (phazite::getSavedTrackHeight (song, track) == 120);
    CHECK (phazite::findPluginInstance (song, plugin) == instance);
    phazite::redo (song);
    CHECK (phazite::getParentFolder (song, track) == folder);
    CHECK (phazite::getSavedTrackHeight (song, track) == 120);
    CHECK (phazite::findPluginInstance (song, plugin) == instance);

    phazite::moveTracksAfter (song, { track }, folder);
    CHECK_FALSE (phazite::getParentFolder (song, track).isValid());
    CHECK (phazite::findPluginInstance (song, plugin) == instance);
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: tracks and folders added, renamed, coloured and moved, each one undo step with its name")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& selection = phazite::getSelection (s.context);
    const auto first = s.firstTrack;
    phazite::test::UndoCheck undo (s.context);

    TrackID added, folder;
    undo.check ("Add Track", [&] { added = phazite::addTrackAtEnd (song, false, &selection); });
    CHECK (lastStep (song) == juce::String ("Add Track"));
    CHECK (selection.isSelected (added));
    {
        const auto value = held (song, added);
        CHECK (value.kind == TrackValue::Kind::track);
        CHECK (value.colour.has_value());
        CHECK (value.fader.isValid());
        CHECK_FALSE (value.parent.isValid());
        CHECK (phazite::getTrackIDs (song).back() == added);
    }

    undo.check ("Add Folder", [&] { folder = phazite::addTrackAtEnd (song, true, &selection); });
    CHECK (lastStep (song) == juce::String ("Add Folder"));
    {
        const auto value = held (song, folder);
        CHECK (value.isFolder());
        CHECK (value.bus);
        CHECK (value.isBus());
        CHECK (value.getOwnName() == juce::String ("Folder"));
    }

    undo.check ("Rename Track", [&] { phazite::renameTrack (song, added, "  Bass  "); });
    CHECK (lastStep (song) == juce::String ("Rename Track"));
    CHECK (held (song, added).name == juce::String ("Bass"));

    undo.check ("Change Track Colors", [&] { phazite::setTrackColours (song, { first, added }, juce::Colour (0xff336699)); });
    CHECK (lastStep (song) == juce::String ("Change Track Colors"));
    CHECK (held (song, first).colour == 0xff336699u);
    CHECK (held (song, added).colour == 0xff336699u);

    undo.check ("Move Track", [&] { phazite::moveTracksAfter (song, { first }, added); });
    CHECK (lastStep (song) == juce::String ("Move Track"));
    CHECK (phazite::getTrackIDs (song) == std::vector { added, first, folder });

    undo.check ("Move Into", [&] { phazite::moveTracksTo (song, { added, first }, folder, phazite::TrackDrop::into); });
    CHECK (lastStep (song) == juce::String ("Move Tracks"));
    CHECK (phazite::getTrackIDs (song) == std::vector { folder, added, first });

    undo.check ("Move Before", [&] { phazite::moveTracksTo (song, { first }, folder, phazite::TrackDrop::before); });
    CHECK (phazite::getTrackIDs (song) == std::vector { first, folder, added });
    CHECK_FALSE (phazite::canMoveTracksTo (song, { folder }, added, phazite::TrackDrop::before)); // into itself

    selection.selectOnly (added);
    undo.command (s.commands, "track.moveUp");
    CHECK (phazite::getTrackIDs (song) == std::vector { first, folder, added }); // first in its folder already
    selection.selectOnly (folder);
    undo.command (s.commands, "track.moveUp");
    CHECK (phazite::getTrackIDs (song) == std::vector { folder, added, first });

    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: three tracks grouped into a folder and ungrouped, and a track with clips, devices, sends and "
           "modulators moved into a folder, into a folder in it and out, keeping everything")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& selection = phazite::getSelection (s.context);
    const auto a = s.firstTrack;
    const auto b = s.addTrack ("B");
    const auto c = s.addTrack ("C");
    const auto d = s.addTrack ("D");
    phazite::test::UndoCheck undo (s.context);

    // Grouped where the first was, in its colour, and ungrouped back into their places.
    TrackID folder;
    undo.check ("Group", inStep (song, "Group into Folder", [&] { folder = phazite::groupIntoFolder (song, { b, c, d }, &selection); }));
    CHECK (phazite::getTrackIDs (song) == std::vector { a, folder, b, c, d });
    CHECK (held (song, folder).colour == phazite::getTrackColour (song, b, 1).getARGB());
    CHECK (held (song, folder).bus);
    CHECK (selection.isSelected (folder));
    undo.check ("Ungroup", inStep (song, "Ungroup Folder", [&] { phazite::ungroupFolders (song, { folder }); }));
    CHECK (phazite::getTrackIDs (song) == std::vector { a, b, c, d });
    CHECK_FALSE (phazite::hasTrack (song, folder));

    // A track with what each area holds of it.
    s.setInstrument (a, dev::Synth::typeID);
    s.addEffect (a, dev::Equalizer::typeID);
    const auto clip = s.addMidiClip (a, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0);
    const auto reverb = s.addReturn (b, "phaziteReverb");
    s.addSend (a, reverb, -6.0f);
    s.addModulator (a, phazite::ModulatorType::lfo);

    const auto clips = phazite::getClips (song, a);
    const auto devices = phazite::getDevices (song, a);
    const auto sends = phazite::getSends (song, a);
    const auto modulators = phazite::getModulators (song, a);
    REQUIRE (clips.size() == 1);
    REQUIRE (devices.size() == 2);
    REQUIRE (sends.size() == 1);
    REQUIRE (modulators.size() == 1);

    const auto keeps = [&]
    {
        CHECK (phazite::getClips (song, a) == clips);
        CHECK (phazite::getDevices (song, a) == devices);
        CHECK (phazite::getSends (song, a) == sends);
        CHECK (phazite::getModulators (song, a) == modulators);
        phazite::test::checkSong (song);
    };

    TrackID outer, inner;
    undo.check ("Folders", inStep (song, "Folders",
                                   [&]
                                   {
                                       outer = phazite::groupIntoFolder (song, { c }, nullptr);
                                       inner = phazite::groupIntoFolder (song, { d }, nullptr);
                                       phazite::moveTrackIntoFolder (song, inner, outer);
                                   }));
    undo.check ("Into Folder", inStep (song, "Move", [&] { phazite::moveTrackIntoFolder (song, a, outer); }));
    CHECK (phazite::getParentFolder (song, a) == outer);
    keeps();
    undo.check ("Into Inner", inStep (song, "Move", [&] { phazite::moveTrackIntoFolder (song, a, inner); }));
    CHECK (phazite::getParentFolder (song, a) == inner);
    keeps();
    undo.check ("Out", [&] { phazite::moveTracksAfter (song, { a }, TrackID()); });
    CHECK_FALSE (phazite::getParentFolder (song, a).isValid());
    CHECK (phazite::getTrackIDs (song).front() == a);
    keeps();

    undo.walkBackAndForth();
    keeps();
}

TEST_CASE ("Track writers: a bus made plain and a bus again with its fader automated, and a folder of two tracks duplicated")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto one = s.firstTrack;
    const auto two = s.addTrack ("Two");
    const auto bus = s.addBus ("Bus", { one, two });
    s.addAutomation (s.getVolumeParameter (bus),
                     { { phazite::BeatPosition::fromBeats (0.0), 0.5f }, { phazite::BeatPosition::fromBeats (4.0), 0.8f } });
    phazite::test::UndoCheck undo (s.context);

    const auto fader = held (song, bus).fader;
    REQUIRE (fader.isValid());
    undo.check ("Plain", [&] { phazite::setMixedAsBus (song, bus, false); });
    CHECK (lastStep (song) == juce::String ("Make Folder Plain"));
    {
        const auto value = held (song, bus);
        CHECK_FALSE (value.bus);
        CHECK_FALSE (value.fader.isValid());
        CHECK (value.vca.isValid());
        CHECK (phazite::findCurve (song, s.getVolumeParameter (bus)) != nullptr); // its automation follows its VCA
    }

    undo.check ("Bus", [&] { phazite::setMixedAsBus (song, bus, true); });
    CHECK (lastStep (song) == juce::String ("Mix Folder as Bus"));
    {
        const auto value = held (song, bus);
        CHECK (value.bus);
        CHECK (value.fader.isValid());
        CHECK (value.fader != fader);
        CHECK_FALSE (value.vca.isValid());
        CHECK (phazite::findCurve (song, s.getVolumeParameter (bus)) != nullptr);
    }

    // The folder duplicated: right after it, with copies of its two tracks in their order, each as its original but for its
    // IDs and its place.
    std::vector<TrackID> copies;
    undo.check ("Duplicate", inStep (song, "Duplicate", [&] { copies = phazite::duplicateTracks (song, { bus }, nullptr); }));
    REQUIRE (copies.size() == 1);
    const auto copy = copies.front();
    const auto inCopy = phazite::getTracksInFolder (song, copy);
    REQUIRE (inCopy.size() == 2);
    CHECK (phazite::getTrackIDs (song) == std::vector { bus, one, two, copy, inCopy[0], inCopy[1] });

    for (const auto& [original, made] : { std::pair { bus, copy }, std::pair { one, inCopy[0] }, std::pair { two, inCopy[1] } })
    {
        auto expected = held (song, original);
        const auto value = held (song, made);
        CHECK (value.id != original);
        CHECK (value.fader != expected.fader);
        expected.id = value.id;
        expected.parent = value.parent;
        expected.fader = value.fader;
        expected.vca = value.vca;
        CHECK (value == expected);
    }

    CHECK (phazite::findCurve (song, s.getVolumeParameter (copy)) != nullptr); // its automation copied

    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: a pad given its own track and brought back, a return added with an effect, a track added with the key "
           "set last, its input, monitoring, volume and pan, and a track's preset of a track without a colour")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto drums = s.firstTrack;
    const auto sampler = s.setInstrument (drums, dev::DrumSampler::typeID);
    inStep (song, "Colour", [&] { phazite::setTrackColour (song, drums, juce::Colour (0xff445566)); })();
    phazite::test::UndoCheck undo (s.context);

    // A pad's own track: after the sampler's, named, in its colour, playing the pad.
    TrackID pad;
    undo.check ("Pad", [&] { pad = phazite::givePadItsOwnTrack (song, sampler, 36); });
    REQUIRE (pad.isValid());
    {
        const auto value = held (song, pad);
        CHECK (value.padDevice == sampler);
        CHECK (value.pad == 36);
        CHECK (value.colour == 0xff445566u);
        CHECK (value.getOwnName().isNotEmpty());
        CHECK (phazite::getTrackIDs (song) == std::vector { drums, pad });
    }

    undo.check ("Pad Back", [&] { phazite::bringPadBack (song, sampler, 36); });
    CHECK_FALSE (phazite::hasTrack (song, pad));

    // A return with an effect.
    TrackID reverb;
    undo.check ("Return", [&] { reverb = phazite::addReturnWith (song, drums, s.findDeviceType ("phaziteReverb")); });
    REQUIRE (reverb.isValid());
    CHECK (held (song, reverb).kind == TrackValue::Kind::returnTrack);
    CHECK (phazite::getDevices (song, reverb).size() == 1);
    CHECK (phazite::getSends (song, drums).size() == 1);

    // The key set last: a track added has it, in the step that adds it, through its Undo and Redo.
    const phazite::Key key { 7, phazite::Scale::minor };
    undo.check ("Set Key", [&] { phazite::chooseTrackKey (song, drums, key); });
    CHECK (phazite::getLastKey (song) == key);
    TrackID keyed;
    undo.check ("Add Track", [&] { keyed = phazite::addTrackAtEnd (song, false, nullptr); });
    CHECK (held (song, keyed).key == key);
    undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
    CHECK_FALSE (phazite::hasTrack (song, keyed));
    undo.stepBackOrForth ("Redo", [&] { phazite::redo (song); });
    CHECK (held (song, keyed).key == key);
    CHECK (phazite::getTrackKey (song, keyed) == key);

    // Its input and monitoring, each a step of its own; its volume and pan in a step.
    undo.check ("Input", [&] { phazite::setTrackInput (s.context, keyed, "1+2"); });
    CHECK (lastStep (song) == juce::String ("Change Input"));
    CHECK (held (song, keyed).input == juce::String ("1+2"));
    undo.check ("Monitoring", [&] { phazite::setInputMonitoring (s.context, keyed, phazite::InputMonitoring::always); });
    CHECK (held (song, keyed).monitoring == TrackValue::Monitoring::always);
    undo.check ("Mixer", inStep (song, "Mixer",
                                 [&]
                                 {
                                     phazite::setChannelVolume (song, keyed, -6.00001f);
                                     phazite::setChannelPan (song, keyed, 0.25f);
                                     phazite::setChannelVolume (song, {}, -9.0f);
                                 }));
    CHECK (held (song, keyed).volume == doctest::Approx (-6.0f));
    CHECK (held (song, keyed).pan == doctest::Approx (0.25f));
    CHECK (held (song, phazite::getMasterID (song)).volume == doctest::Approx (-9.0f));
    undo.check ("Back", inStep (song, "Mixer", [&] { phazite::setChannelVolume (song, keyed, 0.00001f); }));
    CHECK (phazite::test::getTrackProperty (song, keyed, schema::channelVolume).isVoid()); // at its default: none

    // A track without a colour of its own, its preset loaded: none written (its preset holds "0", which reads as none).
    TrackID plain;
    inStep (song, "Plain", [&] { plain = phazite::insertPlainTrack (song, {}); })();
    REQUIRE (plain.isValid());
    CHECK_FALSE (held (song, plain).colour.has_value());
    const auto preset = phazite::makeTrackPreset (song, plain);
    TrackID loaded;
    undo.check ("Preset", inStep (song, "Add Track",
                                  [&]
                                  {
                                      loaded = phazite::addTrackFromPreset (
                                          song, phazite::getPluginHost (phazite::test::getEngineContext()), plain, preset, nullptr);
                                  }));
    REQUIRE (loaded.isValid());
    CHECK_FALSE (held (song, loaded).colour.has_value());
    CHECK_FALSE (phazite::test::copyTrack (song, loaded).hasProperty (schema::trackColour));

    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: a track and a folder of two tracks duplicated keep their switches and their view, the frozen track's "
           "copy playing live")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);

    // A drum track muted, soloed, writing in latch, 120 high, collapsed, pinned, its drum mode off and its rows set, two
    // lanes shown (its delay's mix 100 high, its volume collapsed), and frozen.
    const auto drums = s.firstTrack;
    s.setInstrument (drums, dev::DrumSampler::typeID);
    const auto delay = s.addEffect (drums, dev::Delay::typeID);
    s.addMidiClip (drums, 0.0, 4.0);
    const phazite::ParameterRef mix { delay, "mix" };
    const auto volume = s.getVolumeParameter (drums);
    phazite::setTrackMuted (song, drums, true);
    phazite::setTrackSoloed (song, drums, true);
    phazite::setAutomationMode (song, drums, phazite::AutomationMode::latch);
    phazite::setSavedTrackHeight (song, drums, 120);
    phazite::setCollapsed (song, drums, true);
    phazite::setPinned (song, drums, true);
    phazite::setDrumTrack (song, drums, false);
    phazite::setDrumRowOrder (song, drums, { 38, 36, 42 });
    phazite::hideDrumRow (song, drums, 42);
    phazite::showLane (song, drums, mix);
    phazite::showLane (song, drums, volume);
    phazite::setLaneHeight (song, drums, mix, 100);
    phazite::setLaneCollapsed (song, drums, volume, true);
    phazite::freezeFromFile (song, drums, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Frozen.wav"), 0.5));

    // A bus folder collapsed, writing in touch, its volume's lane shown; its first track muted and 90 high, its second
    // soloed, pinned and in drum mode without a drum sampler.
    const auto one = s.addTrack ("One");
    const auto two = s.addTrack ("Two");
    const auto bus = s.addBus ("Bus", { one, two });
    phazite::setCollapsed (song, bus, true);
    phazite::setAutomationMode (song, bus, phazite::AutomationMode::touch);
    phazite::showLane (song, bus, s.getVolumeParameter (bus));
    phazite::setTrackMuted (song, one, true);
    phazite::setSavedTrackHeight (song, one, 90);
    phazite::setTrackSoloed (song, two, true);
    phazite::setPinned (song, two, true);
    phazite::setDrumTrack (song, two, true);

    REQUIRE (phazite::isTrackFrozen (song, drums));
    REQUIRE_FALSE (phazite::isDrumTrack (song, drums));
    REQUIRE (phazite::isDrumTrack (song, two));
    REQUIRE (phazite::getLanes (song, drums).size() == 2);
    REQUIRE (phazite::getLanes (song, bus).size() == 1);

    phazite::test::UndoCheck undo (s.context);
    const auto steps = undo.getNumSteps();
    std::vector<TrackID> copies;
    undo.check ("Duplicate", inStep (song, "Duplicate Tracks", [&] { copies = phazite::duplicateTracks (song, { drums, bus }, nullptr); }));
    CHECK (undo.getNumSteps() == steps + 1);
    CHECK (lastStep (song) == juce::String ("Duplicate Tracks"));
    phazite::test::checkSong (song);
    REQUIRE (copies.size() == 2);
    const auto inCopy = phazite::getTracksInFolder (song, copies[1]);
    REQUIRE (inCopy.size() == 2);

    const auto keepsAll = [&]
    {
        checkKeepsSwitchesAndView (song, drums, copies[0]);
        checkKeepsSwitchesAndView (song, bus, copies[1]);
        checkKeepsSwitchesAndView (song, one, inCopy[0]);
        checkKeepsSwitchesAndView (song, two, inCopy[1]);
        CHECK (phazite::isTrackFrozen (song, drums)); // the original still frozen
        CHECK (phazite::getLanes (song, copies[0]).front().parameter.device == phazite::getDevices (song, copies[0])[1]);
    };

    keepsAll();
    undo.walkBackAndForth();
    keepsAll();
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: a folder of two tracks duplicated gives each copy its original's switches and view outside the "
           "history, before the copies come in: a copy unmuted since stays unmuted through the step's Undo and Redo")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);

    // A folder of two tracks, each with a delay whose mix's lane it shows: the first muted, 80 high, collapsed and frozen; the
    // second soloed, writing in touch, 140 high and pinned.
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    const auto firstDelay = s.addEffect (first, dev::Delay::typeID);
    const auto secondDelay = s.addEffect (second, dev::Delay::typeID);
    const auto folder = s.addBus ("Folder", { first, second });
    phazite::setTrackMuted (song, first, true);
    phazite::setSavedTrackHeight (song, first, 80);
    phazite::setCollapsed (song, first, true);
    phazite::showLane (song, first, { firstDelay, "mix" });
    phazite::freezeFromFile (song, first, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Frozen.wav"), 0.5));
    phazite::setTrackSoloed (song, second, true);
    phazite::setAutomationMode (song, second, phazite::AutomationMode::touch);
    phazite::setSavedTrackHeight (song, second, 140);
    phazite::setPinned (song, second, true);
    phazite::showLane (song, second, { secondDelay, "mix" });
    REQUIRE (phazite::isTrackFrozen (song, first));

    phazite::test::UndoCheck undo (s.context);
    const auto steps = undo.getNumSteps();
    std::vector<TrackID> copies;
    undo.check ("Duplicate", inStep (song, "Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { folder }, nullptr); }));
    CHECK (undo.getNumSteps() == steps + 1);
    CHECK (lastStep (song) == juce::String ("Duplicate Track"));
    REQUIRE (copies.size() == 1);
    const auto inCopy = phazite::getTracksInFolder (song, copies.front());
    REQUIRE (inCopy.size() == 2);

    const auto keepsAll = [&]
    {
        checkKeepsSwitchesAndView (song, first, inCopy[0]); // the frozen switch not copied among them
        checkKeepsSwitchesAndView (song, second, inCopy[1]);
        CHECK (phazite::isTrackMuted (song, inCopy[0], false));
        CHECK (phazite::isTrackSoloed (song, inCopy[1]));
        CHECK (phazite::getAutomationMode (song, inCopy[1]) == phazite::AutomationMode::touch);
        CHECK (phazite::getSavedTrackHeight (song, inCopy[0]) == std::optional (80));
        CHECK (phazite::getSavedTrackHeight (song, inCopy[1]) == std::optional (140));
        CHECK (phazite::isCollapsed (song, inCopy[0]));
        CHECK (phazite::isPinned (song, inCopy[1]));
        CHECK (phazite::isTrackFrozen (song, first));

        // Each copy's lane names its own device's copy, not its original's.
        for (const auto& [original, copy] : { std::pair { first, inCopy[0] }, std::pair { second, inCopy[1] } })
        {
            const auto lanes = phazite::getLanes (song, copy);
            const auto devices = phazite::getDevices (song, copy);
            REQUIRE (lanes.size() == 1);
            REQUIRE (devices.size() == 1);
            CHECK (lanes.front().parameter.device == devices.front());
            CHECK (devices.front() != phazite::getDevices (song, original).front());
        }

        phazite::test::checkSong (song);
    };

    keepsAll();
    undo.walkBackAndForth();
    keepsAll();

    // The first's copy unmuted since, outside the history: the step's Undo takes the copies away, and its Redo brings them back
    // with the mute as it was set since, the rest as given.
    phazite::toggleTracksMuted (song, { inCopy[0] });
    REQUIRE_FALSE (phazite::isTrackMuted (song, inCopy[0], false));
    phazite::undo (song);
    CHECK_FALSE (phazite::hasTrack (song, copies.front()));
    CHECK_FALSE (phazite::hasTrack (song, inCopy[0]));
    phazite::redo (song);
    REQUIRE (phazite::hasTrack (song, inCopy[0]));
    CHECK_FALSE (phazite::isTrackMuted (song, inCopy[0], false));
    CHECK (phazite::isTrackMuted (song, first, false));
    CHECK (phazite::isTrackSoloed (song, inCopy[1]));
    CHECK (phazite::getAutomationMode (song, inCopy[1]) == phazite::AutomationMode::touch);
    CHECK (phazite::getSavedTrackHeight (song, inCopy[0]) == std::optional (80));
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: a track moved to where it is makes no undo step, in a folder with lanes, and at the top")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto a = s.firstTrack;
    const auto b = s.addTrack ("B");
    const auto c = s.addTrack ("C");

    // A bus holding A, its lanes shown while it held A alone, then B: whole state writes A, B, then its lanes (the song
    // holds no LANES element: a folder's lanes are its view's, with no place among its tracks).
    const auto bus = s.addBus ("Bus", { a });
    phazite::showLane (song, bus, s.getVolumeParameter (bus));
    inStep (song, "Into", [&] { phazite::moveTrackIntoFolder (song, b, bus); })();
    REQUIRE (phazite::getTracksInFolder (song, bus) == std::vector { a, b });
    {
        juce::StringArray types;

        for (const auto& child : phazite::test::copyTrack (song, bus))
            types.add (child.getType().toString());

        const auto track = schema::TRACK.getName().toString();
        REQUIRE (types == juce::StringArray { track, track, schema::LANES.getName().toString() });
    }

    phazite::test::UndoCheck undo (s.context);
    const auto steps = undo.getNumSteps();
    const auto before = phazite::test::writeSong (song, phazite::test::SongPart::whole);

    undo.check ("Before Itself", [&] { phazite::moveTracksTo (song, { a }, a, phazite::TrackDrop::before); });
    undo.check ("After Itself", [&] { phazite::moveTracksTo (song, { a }, a, phazite::TrackDrop::after); });
    undo.check ("At the Top", [&] { phazite::moveTracksTo (song, { c }, c, phazite::TrackDrop::before); });

    {
        const phazite::UndoStep step (song, "Where It Is");
        CHECK_FALSE (phazite::setTrackPlace (song, a, bus, 0));
        CHECK_FALSE (phazite::setTrackPlace (song, b, bus, 1));
        CHECK_FALSE (phazite::setTrackPlace (song, c, {}, 0));
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    CHECK (undo.getNumSteps() == steps);
    CHECK (phazite::test::writeSong (song, phazite::test::SongPart::whole) == before);
    CHECK (phazite::getTrackIDs (song) == std::vector { c, bus, a, b });
}

TEST_CASE ("Track writers: the master set whole, tracks ensured, a track's output chosen, the master's key, tracks deleted, and "
           "a removed drum sampler's pads' tracks")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& selection = phazite::getSelection (s.context);
    const auto master = phazite::getMasterID (song);
    const auto first = s.firstTrack;
    phazite::test::UndoCheck undo (s.context);

    // The master set whole: its volume written; without an ID, refused.
    {
        auto value = held (song, master);
        value.volume = -9.0f;
        undo.check ("Master", inStep (song, "Master Volume", [&] { CHECK (phazite::setTrack (song, value)); }));
        CHECK (lastStep (song) == juce::String ("Master Volume"));
        CHECK (held (song, master).volume == doctest::Approx (-9.0f));

        const phazite::UndoStep step (song, "Refused");
        value.id = {};
        value.volume = -3.0f;
        CHECK_FALSE (phazite::setTrack (song, value));
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
    }

    // Tracks ensured: as many as asked for, the first kept, in one step; none more where there are as many.
    std::vector<TrackID> tracks;
    undo.check ("Ensure", inStep (song, "Add Tracks", [&] { tracks = phazite::ensureTracks (song, 4); }));
    CHECK (lastStep (song) == juce::String ("Add Tracks"));
    REQUIRE (tracks.size() == 4);
    CHECK (tracks.front() == first);
    CHECK (phazite::getTrackIDs (song) == tracks);

    for (const auto track : tracks)
        CHECK (held (song, track).kind == TrackValue::Kind::track);

    const auto steps = undo.getNumSteps();
    undo.check ("Ensure Again", inStep (song, "Add Tracks", [&] { CHECK (phazite::ensureTracks (song, 2) == tracks); }));
    CHECK (undo.getNumSteps() == steps);
    phazite::test::checkSong (song);

    // A track's output chosen: a bus, at the end of its folder; then the master, out of it, after the bus.
    const auto bus = s.addBus ("Bus", { tracks[1] });
    undo.check ("To the Bus", [&] { phazite::setOutput (s.context, tracks[3], bus); });
    CHECK (lastStep (song) == juce::String ("Change Output"));
    CHECK (phazite::getTracksInFolder (song, bus) == std::vector { tracks[1], tracks[3] });
    CHECK (held (song, tracks[3]).parent == bus);
    phazite::test::checkSong (song);
    undo.check ("To the Master", [&] { phazite::setOutput (s.context, tracks[3], {}); });
    CHECK (lastStep (song) == juce::String ("Change Output"));
    CHECK_FALSE (held (song, tracks[3]).parent.isValid());
    CHECK (phazite::getTrackIDs (song) == std::vector { tracks[0], tracks[2], bus, tracks[1], tracks[3] });
    phazite::test::checkSong (song);

    // The master's key: nothing of the master written, in no undo action; the key set last is the key given.
    {
        const auto before = held (song, master);
        const phazite::Key key { .root = 2, .scale = phazite::Scale::dorian };
        const phazite::UndoStep step (song, "Master Key");
        phazite::setTrackKey (song, master, key);
        CHECK (song.getUndoManager().getNumActionsInCurrentTransaction() == 0);
        CHECK (held (song, master) == before);
        CHECK (phazite::getLastKey (song) == key);
    }

    // Deleted directly: a track, then two, then a folder with its track; each one step, the next track selected.
    undo.check ("Delete One", inStep (song, "Delete Track", [&] { phazite::deleteTracks (song, { tracks[0] }, &selection); }));
    CHECK (lastStep (song) == juce::String ("Delete Track"));
    CHECK_FALSE (phazite::hasTrack (song, tracks[0]));
    CHECK (selection.isSelected (tracks[2]));
    phazite::test::checkSong (song);

    const auto other = s.addTrack ("Other");
    undo.check ("Delete Two", inStep (song, "Delete Tracks", [&] { phazite::deleteTracks (song, { tracks[3], other }, &selection); }));
    CHECK (lastStep (song) == juce::String ("Delete Tracks"));
    CHECK (phazite::getTrackIDs (song) == std::vector { tracks[2], bus, tracks[1] });
    phazite::test::checkSong (song);

    undo.check ("Delete Folder", inStep (song, "Delete Track", [&] { phazite::deleteTracks (song, { bus }, nullptr); }));
    CHECK (lastStep (song) == juce::String ("Delete Track"));
    CHECK (phazite::getTrackIDs (song) == std::vector { tracks[2] });
    CHECK_FALSE (phazite::hasTrack (song, tracks[1]));
    phazite::test::checkSong (song);

    // A drum sampler removed whose pads play on tracks of their own: the pad's track without clips goes; the one with a clip
    // stays, a plain track playing no pad.
    const auto drums = tracks[2];
    const auto sampler = s.setInstrument (drums, dev::DrumSampler::typeID);
    TrackID kick, snare;
    inStep (song, "Pads",
            [&]
            {
                kick = phazite::givePadItsOwnTrack (song, sampler, 36);
                snare = phazite::givePadItsOwnTrack (song, sampler, 38);
            })();
    REQUIRE (kick.isValid());
    REQUIRE (snare.isValid());
    s.addMidiClip (snare, 0.0, 4.0);
    REQUIRE (phazite::getTrackPad (song, snare).has_value());

    undo.check ("Remove Sampler", [&] { phazite::removeDevice (song, sampler); });
    CHECK_FALSE (phazite::hasTrack (song, kick));
    REQUIRE (phazite::hasTrack (song, snare));
    CHECK_FALSE (phazite::getTrackPad (song, snare).has_value());
    CHECK_FALSE (held (song, snare).padDevice.isValid());
    CHECK_FALSE (held (song, snare).pad.has_value());
    CHECK (phazite::getClips (song, snare).size() == 1);
    phazite::test::checkSong (song);

    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Track writers: a track added for an input, named Audio, recording it, after the selected track, as one undo step")
{
    phazite::test::TestSong s;
    phazite::test::EnginePlayback playback; // a device with two inputs
    auto& song = *phazite::findSong (s.context);
    const auto second = s.addTrack ("Second");
    phazite::getSelection (s.context).selectOnly (s.firstTrack);
    phazite::test::UndoCheck undo (s.context);

    TrackID track;
    undo.check ("Input", [&] { track = phazite::addTrackForInput (s.context, "1+2"); });
    REQUIRE (track.isValid());
    CHECK (lastStep (song) == juce::String ("Add Audio Track"));
    CHECK (phazite::getTrackIDs (song) == std::vector { s.firstTrack, track, second });
    const auto value = held (song, track);
    CHECK (value.kind == TrackValue::Kind::track);
    CHECK (value.name == juce::String ("Audio"));
    CHECK (value.input == juce::String ("1+2"));
    phazite::test::checkSong (song);

    undo.walkBackAndForth();
    CHECK (held (song, track).input == juce::String ("1+2"));
}
