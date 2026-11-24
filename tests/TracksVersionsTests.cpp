// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions write the song's tracks through the tracks' setters (roadmap step S5.20, the tracks' versions pass;
// model/versions/ApplySong.cpp: makeTracksLike(), a line's makeTrackLikeIn(), the song's own line's makeTrackOrderLike(); and a
// merge's choice: model/arrangement/Tracks.cpp, chooseTrackClash()): a version applied to the whole song in the history (one
// step, its Undo and its Redo) and outside it, the song as copied, and each line restored, after a song changed since in
// every way a track can (one added, one deleted, renamed, recoloured, its volume and pan moved, moved within its list, moved
// into a folder and out, tracks grouped into a folder, a folder ungrouped, a bus made plain and a plain folder a bus, a
// return added, a track duplicated, a drum sampler's pad given a track of its own), each pass leaving the song's tracks the
// version's, a second writing nothing, a plug-in on a track moved back out of a folder the same instance (and on a track of a
// folder the version hasn't), nothing left to the keepers; a version's track whose ID or fader's ID the song holds as another
// kind of item (a device, a clip, a send, a modulator, a section) taken by the whole version, and refused by the track's line,
// the song as it was; a folder gone since brought back with its tracks and their clips, devices, curves, modulators, sends,
// switches and view; a folder added since around a folder the version holds taken away with a track added since in that
// folder, the inner folder staying with its tracks; a track added since taken away while the version holds one of its clips
// and one of its devices on another track; a track the version couldn't read left as the song has it, in the version's
// place; the master given another ID by a version, then its line put back and taken again; and each kind of a merge's clash
// on a track or the master chosen through the tracks' setters, and those refused.
//   phazite_tests.exe --source-file=*TracksVersionsTests*
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/drums/DrumSampler.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/TracksValue.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::DeviceID;
using phazite::TrackID;
using phazite::TrackValue;
using phazite::test::TestPlugin;

// A step of the test's own, around what isn't a change of its own (a pass, a choice).
std::function<void()> inStep (phazite::Song& song, const juce::String& name, std::function<void()> change)
{
    return [&song, name, change = std::move (change)]
    {
        const phazite::UndoStep step (song, name);
        change();
    };
}

// Whether the song holds the tracks a whole copy holds, read as the song holds them, and the whole copy's song as text in
// what a version compares; the song checked, and nothing of a track out of the song left with a keeper.
void checkTracksAsIn (const phazite::Song& song, const juce::ValueTree& whole)
{
    CHECK (phazite::getTracksValue (song) == phazite::readHeldTracks (whole));
    CHECK (phazite::holdTheSame (phazite::test::copyWholeSong (song), whole));
    phazite::test::checkSong (song);
}

// The make of a version applied as Bring Back makes it: the song made like it, and what names what it hasn't forgotten.
void bringBack (phazite::Song& song, const juce::ValueTree& version)
{
    phazite::makeSongLike (song, version);
    phazite::removeBrokenReferences (song);
}

void restoreLine (phazite::Song& song, TrackID track, const juce::ValueTree& version)
{
    REQUIRE (phazite::makeTrackLike (song, track, version));
    phazite::removeBrokenReferences (song);
}

void restoreSongLine (phazite::Song& song, const juce::ValueTree& version)
{
    phazite::makeSongOwnLike (song, version);
    phazite::removeBrokenReferences (song);
}

// A whole copy's track element (or the MASTER) put in place of another, as a version's piece that couldn't be read leaves
// one (SongPieces.h: a stand-in, schema::versions::missing): its type and its key alone.
void putStandInFor (juce::ValueTree element)
{
    juce::ValueTree standIn (element.getType());

    for (const auto& key : schema::getKeyProperties (element.getType()))
        if (element.hasProperty (key))
            standIn.setProperty (key, element[key], nullptr);

    standIn.setProperty (schema::versions::missing, true, nullptr);
    auto parent = element.getParent();
    REQUIRE (parent.isValid());
    const auto index = parent.indexOf (element);
    parent.removeChild (index, nullptr);
    parent.addChild (standIn, index, nullptr);
}

// An element's place from the song down, as a merge's clash names it ("TRACKS:TRACKS", "TRACK:12").
juce::StringArray pathOf (const juce::ValueTree& element)
{
    juce::StringArray path;

    for (auto each = element; each.isValid() && each.getParent().isValid(); each = each.getParent())
        path.insert (0, each.getType().toString() + ":" + schema::getElementKey (each));

    return path;
}

// What the song holds of a track, which must be there.
TrackValue held (const phazite::Song& song, TrackID id)
{
    const auto value = phazite::findHeldTrack (song, id);
    REQUIRE (value.has_value());
    return *value;
}

// Whether a change put a track in the song anew rather than moving the one it holds (what a plug-in on it would be taken out
// and put in again with): whether the tracks' teller told it taken out or added (TracksArea.h: TrackChange::made) rather than
// come to another folder (TrackChange::place). Since the tracks' switch (roadmap step S5.20) no track has an element whose
// identity could tell it (named: this read the element's identity in the song's tree before).
class ElementKept final : private phazite::SongValues::Listener
{
public:
    ElementKept (const phazite::Song& s, TrackID id)
        : song (s),
          track (id)
    {
        song.getValues()->addListener (this);
    }

    ~ElementKept() override { song.getValues()->removeListener (this); }

    bool isPutInAnew() const noexcept { return anew; }

private:
    void tracksChanged (const std::vector<phazite::TrackChange>& changes, const std::vector<TrackID>&, const phazite::SongTracks&) override
    {
        for (const auto& change : changes)
            if (! change.onMaster && change.id == track && change.has (phazite::TrackChange::made))
                anew = true;
    }

    const phazite::Song& song;
    TrackID track {};
    bool anew = false;
};

// Every line of a version, as the versions list them: the tracks either song holds, the master, the song's own.
std::vector<TrackID> getLineTracks (const phazite::Song& song, const juce::ValueTree& version)
{
    std::vector<TrackID> lines;

    for (const auto id : phazite::getTrackIDs (song))
        lines.push_back (id);

    for (const auto& element : phazite::getTrackElements (version))
        if (const auto id = TrackID::fromRaw (phazite::readID (element[schema::trackID])); ! phazite::hasTrack (song, id))
            lines.push_back (id);

    return lines;
}
} // namespace

TEST_CASE ("The versions make the song's tracks a version's through the tracks' setters: the whole song in the history and "
           "outside it, each line put back, a second time writing nothing, a plug-in on a track moved out of a folder the same "
           "instance")
{
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);

    // The version: tracks, a bus with a track in it, a plain folder with a track in it, a folder to ungroup, two tracks to
    // group, a drum sampler's track, and a plug-in on a track.
    const auto first = s.firstTrack;
    const auto renamed = s.addTrack ("Renamed");
    const auto mixed = s.addTrack ("Mixed");
    const auto deleted = s.addTrack ("Deleted");
    const auto withPlugin = s.addTrack ("Plug-in");
    const auto groupedA = s.addTrack ("Grouped A");
    const auto groupedB = s.addTrack ("Grouped B");
    const auto inBus = s.addTrack ("In Bus");
    const auto bus = s.addBus ("Bus", { inBus });
    const auto inPlain = s.addTrack ("In Plain");
    const auto plain = s.addBus ("Plain", { inPlain });
    const auto inUngrouped = s.addTrack ("In Ungrouped");
    const auto ungrouped = s.addBus ("Ungrouped", { inUngrouped });
    const auto drumsTrack = s.addTrack ("Drums");
    const auto drums = s.setInstrument (drumsTrack, phazite::devices::DrumSampler::typeID);
    phazite::setMixedAsBus (song, plain, false);
    DeviceID plugin, groupedPlugin;

    {
        const phazite::UndoStep step (song, "Plug-in");
        plugin = TestPlugin::add (song, withPlugin);
        groupedPlugin = TestPlugin::add (song, groupedA); // on a track grouped into a folder since, which the version hasn't
    }

    s.waitForPlugins();
    session->update();
    auto* instance = phazite::findPluginInstance (song, plugin);
    auto* groupedInstance = phazite::findPluginInstance (song, groupedPlugin);
    REQUIRE (instance != nullptr);
    REQUIRE (groupedInstance != nullptr);

    for (auto* each : { instance, groupedInstance })
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [each] { return ! each->isStateUnasked() && ! each->isAskingState() && each->getLastState().getSize() > 0; }));

    song.storeDeviceStates();
    REQUIRE (held (song, plain).isFolder());
    REQUIRE_FALSE (held (song, plain).isBus());
    const auto version = phazite::test::copyWholeSong (song);

    // Since: every kind of change a track can have.
    const auto added = s.addTrack ("Added");
    phazite::deleteTracks (song, { deleted }, nullptr);
    phazite::renameTrack (song, renamed, "Renamed Since");
    phazite::setTrackColours (song, { mixed }, juce::Colours::orange);
    s.setVolume (mixed, -9.0f);
    s.setPan (mixed, 0.5f);
    phazite::moveTracksAfter (song, { mixed }, first);
    phazite::moveTracksTo (song, { withPlugin }, bus, phazite::TrackDrop::into);
    phazite::moveTracksAfter (song, { inPlain }, drumsTrack);
    const auto grouping = phazite::groupIntoFolder (song, { groupedA, groupedB }, nullptr);
    REQUIRE (grouping.isValid());
    phazite::ungroupFolders (song, { ungrouped });
    phazite::setMixedAsBus (song, bus, false);
    phazite::setMixedAsBus (song, plain, true);
    const auto returnTrack = s.addReturn (first, "phaziteReverb");
    const auto copies = phazite::duplicateTracks (song, { renamed }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto padTrack = phazite::givePadItsOwnTrack (song, drums, 36);
    REQUIRE (padTrack.isValid());
    REQUIRE (phazite::getParentFolder (song, withPlugin) == bus);
    session->update();
    REQUIRE (phazite::findPluginInstance (song, plugin) == instance);
    song.storeDeviceStates();
    const auto since = phazite::test::copyWholeSong (song);
    REQUIRE_FALSE (phazite::holdTheSame (since, version));
    phazite::test::checkSong (song);

    {
        phazite::test::UndoCheck undo (s.context);

        // In the history, as Bring Back applies it: one step, which UndoCheck undoes and redoes. The track grouped since comes
        // out of the folder the version hasn't as the same element (its plug-in not taken out with it and put in again).
        auto groupedAnew = true;
        undo.check ("Bring Back", inStep (song, "Bring Back",
                                          [&]
                                          {
                                              const ElementKept kept (song, groupedA);
                                              bringBack (song, version);
                                              groupedAnew = kept.isPutInAnew();
                                          }));
        checkTracksAsIn (song, version);
        CHECK (phazite::test::hasTrack (song, deleted));

        for (const auto gone : { added, grouping, returnTrack, copies.front(), padTrack })
            CHECK_FALSE (phazite::test::hasTrack (song, gone));

        CHECK (phazite::getParentFolder (song, withPlugin) == TrackID());
        CHECK (phazite::getParentFolder (song, inUngrouped) == ungrouped);
        CHECK (phazite::getParentFolder (song, groupedA) == TrackID());
        CHECK (held (song, bus).isBus());
        CHECK_FALSE (held (song, plain).isBus());

        // The plug-in's track moved out of the folder as the same element: the same instance. And the plug-in on a track of a
        // folder the version hasn't, which comes out of it before the folder goes (makeTracksLike()'s step 2), the same.
        session->update();
        CHECK (phazite::findPluginInstance (song, plugin) == instance);
        CHECK (phazite::findPluginInstance (song, groupedPlugin) == groupedInstance);
        CHECK_FALSE (groupedAnew);

        // A second time: nothing to write, no step.
        const auto steps = undo.getNumSteps();
        undo.check ("Again", inStep (song, "Again", [&] { bringBack (song, version); }));
        CHECK (undo.getNumSteps() == steps);

        // And back to the song since.
        undo.check ("Bring Back Again", inStep (song, "Bring Back Again", [&] { bringBack (song, since); }));
        checkTracksAsIn (song, since);
        session->update();
        CHECK (phazite::findPluginInstance (song, plugin) == instance);
        CHECK (phazite::findPluginInstance (song, groupedPlugin) == groupedInstance);
        undo.walkBackAndForth();
    }

    // Each line put back, one step each: the tracks the song holds and those the version holds that it hasn't, then the
    // master's and the song's own, which places the tracks both have.
    {
        phazite::test::UndoCheck undo (s.context);

        for (const auto line : getLineTracks (song, version))
            undo.check ("Restore " + juce::String ((juce::int64) line.getRaw()),
                        inStep (song, "Restore Line", [&song, line, &version] { restoreLine (song, line, version); }));

        undo.check ("Restore Master", inStep (song, "Restore Master", [&] { restoreLine (song, phazite::getMasterID (song), version); }));
        undo.check ("Restore Song", inStep (song, "Restore Song", [&] { restoreSongLine (song, version); }));
        checkTracksAsIn (song, version);
        session->update();
        CHECK (phazite::findPluginInstance (song, plugin) == instance);
        CHECK (phazite::findPluginInstance (song, groupedPlugin) == groupedInstance);

        // A second time: nothing to write.
        const auto steps = undo.getNumSteps();

        for (const auto line : getLineTracks (song, version))
            undo.check ("Again", inStep (song, "Again", [&song, line, &version] { restoreLine (song, line, version); }));

        undo.check ("Again", inStep (song, "Again", [&] { restoreSongLine (song, version); }));
        CHECK (undo.getNumSteps() == steps);
        undo.walkBackAndForth();
    }

    // Outside the history, as switching to another branch does: the song since, then the version again.
    phazite::makeSongTheSame (song, since);
    checkTracksAsIn (song, since);
    phazite::makeSongTheSame (song, version);
    checkTracksAsIn (song, version);
    session->update();
    CHECK (phazite::findPluginInstance (song, plugin) == instance);
    CHECK (phazite::findPluginInstance (song, groupedPlugin) == groupedInstance);

    // The song as copied after an edit undone: nothing of its tracks written (asserted in Debug builds: ApplySong.cpp).
    const auto copy = phazite::test::copyWholeSong (song);
    phazite::renameTrack (song, mixed, "Undone");
    phazite::undo (song);
    phazite::makeSongAsCopied (song, copy);
    checkTracksAsIn (song, copy);
}

TEST_CASE ("A folder gone since comes back with its tracks and their clips, devices, curves, modulators, sends, switches and "
           "view: by its line and by the whole version")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto inside = s.firstTrack;
    const auto deeper = s.addTrack ("Deeper");
    const auto synth = s.setInstrument (inside, "phaziteSynth");
    const auto clip = s.addMidiClip (inside, 0.0, 4.0);
    s.addNote (clip, 60, 0.0, 1.0);
    const auto returnTrack = s.addReturn (inside, "phaziteReverb", -6.0f);
    const auto innerFolder = s.addBus ("Inner", { deeper });
    const auto folder = s.addBus ("Folder", { inside, innerFolder });
    const auto onFolder = s.addEffect (folder, "phaziteEq");
    const auto lfo = s.addModulator (inside, phazite::ModulatorType::lfo);
    s.modulate (lfo, { .device = synth, .parameter = "cutoff" }, 0.5f);
    s.addAutomation ({ .device = synth, .parameter = "cutoff" }, { { phazite::BeatPosition(), 0.5f } });
    phazite::setTrackMuted (song, inside, true);
    phazite::setSavedTrackHeight (song, inside, 100);
    phazite::setAutomationMode (song, deeper, phazite::AutomationMode::latch);
    const auto sends = phazite::getSends (song, inside);
    REQUIRE (sends.size() == 1);
    const auto version = phazite::test::copyWholeSong (song);

    const auto checkBack = [&]
    {
        checkTracksAsIn (song, version);
        CHECK (phazite::getParentFolder (song, inside) == folder);
        CHECK (phazite::getParentFolder (song, deeper) == innerFolder);
        CHECK (phazite::getClips (song, inside) == std::vector { clip });
        CHECK (phazite::hasDevice (song, synth));
        CHECK (phazite::hasDevice (song, onFolder));
        CHECK (phazite::findCurve (song, { .device = synth, .parameter = "cutoff" }) != nullptr);
        REQUIRE (phazite::getModulatorValue (song, lfo).has_value());
        CHECK (phazite::getSends (song, inside) == sends);
        CHECK (phazite::test::hasTrack (song, returnTrack));
        CHECK (phazite::isTrackMuted (song, inside, false));
        CHECK (phazite::getSavedTrackHeight (song, inside) == std::optional (100));
        CHECK (phazite::getAutomationMode (song, deeper) == phazite::AutomationMode::latch);
    };

    phazite::test::UndoCheck undo (s.context);
    undo.check ("Delete Folder", inStep (song, "Delete Folder", [&] { phazite::deleteTracks (song, { folder }, nullptr); }));
    REQUIRE_FALSE (phazite::test::hasTrack (song, inside));
    REQUIRE_FALSE (phazite::hasDevice (song, synth));

    undo.check ("Restore Folder", inStep (song, "Restore Folder", [&] { restoreLine (song, folder, version); }));
    checkBack();

    phazite::undo (song);
    REQUIRE_FALSE (phazite::test::hasTrack (song, inside));
    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
    checkBack();
    undo.walkBackAndForth();
}

TEST_CASE ("A folder added since around a folder the version holds goes, and a track added since in that folder with it; the "
           "folder the version holds stays, with its tracks")
{
    // The random songs found it (tests/RandomSongTests.cpp, seed 20261355): the tracks of both were taken out of every folder
    // above them that the version hasn't, not only the one they are in, and a track added since stayed in the inner folder.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto kept = s.firstTrack;
    const auto inner = s.addBus ("Inner", { kept });
    const auto beside = s.addTrack ("Beside");
    const auto version = phazite::test::copyWholeSong (song);

    const auto added = s.addTrack ("Added");
    phazite::moveTrackIntoFolder (song, added, inner);
    const auto outer = phazite::groupIntoFolder (song, { inner, beside }, nullptr);
    REQUIRE (outer.isValid());
    REQUIRE (phazite::getParentFolder (song, inner) == outer);
    REQUIRE (phazite::getParentFolder (song, added) == inner);

    phazite::test::UndoCheck undo (s.context);
    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
    CHECK_FALSE (phazite::test::hasTrack (song, outer));
    CHECK_FALSE (phazite::test::hasTrack (song, added));
    CHECK (phazite::getParentFolder (song, kept) == inner);
    CHECK (phazite::getParentFolder (song, inner) == TrackID());
    CHECK (phazite::getTrackIDs (song) == std::vector { inner, kept, beside });
    checkTracksAsIn (song, version);
    undo.walkBackAndForth();
}

TEST_CASE ("A track added since goes away while the version holds one of its clips and one of its devices on another track: "
           "each sent home as it is now, by the added track's line and by the whole version")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto first = s.firstTrack;
    const auto clip = s.addMidiClip (first, 0.0, 4.0);
    s.addNote (clip, 64, 0.0, 1.0);
    DeviceID eq;

    {
        const phazite::UndoStep step (song, "Device");
        eq = phazite::test::addOwnDevice (song, first, "phaziteEq");
    }

    const auto version = phazite::test::copyWholeSong (song);
    const auto added = s.addTrack ("Added");

    {
        const phazite::UndoStep step (song, "Since");
        phazite::moveClip (song, clip, phazite::BeatPosition(), added);
        auto moved = *phazite::findDevice (song, eq);
        moved.place = { .track = added };
        REQUIRE (phazite::setDevice (song, moved));
    }

    REQUIRE (phazite::getClips (song, added) == std::vector { clip });
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Restore Added", inStep (song, "Restore Added", [&] { restoreLine (song, added, version); }));
    CHECK_FALSE (phazite::test::hasTrack (song, added));
    CHECK (phazite::getClips (song, first) == std::vector { clip });
    REQUIRE (phazite::findDevice (song, eq) != nullptr);
    CHECK (phazite::findDevice (song, eq)->place == phazite::DevicePlace { .track = first });
    checkTracksAsIn (song, version);

    phazite::undo (song);
    REQUIRE (phazite::test::hasTrack (song, added));
    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
    CHECK_FALSE (phazite::test::hasTrack (song, added));
    CHECK (phazite::getClips (song, first) == std::vector { clip });
    checkTracksAsIn (song, version);
    undo.walkBackAndForth();
}

TEST_CASE ("A track the version couldn't read keeps its value, its holdings and the version's place among the other tracks")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto first = s.firstTrack;
    const auto unread = s.addTrack ("Unread");
    const auto last = s.addTrack ("Last");
    auto version = phazite::test::copyWholeSong (song);
    putStandInFor (phazite::test::findTrackIn (version, unread));

    // Since: the track renamed, given a clip, and moved to the end; another renamed.
    phazite::renameTrack (song, unread, "Unread Since");
    const auto clip = s.addMidiClip (unread, 0.0, 4.0);
    phazite::moveTracksAfter (song, { unread }, last);
    phazite::renameTrack (song, last, "Last Since");

    phazite::test::UndoCheck undo (s.context);
    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, version); }));
    CHECK (phazite::getTrackName (song, unread) == "Unread Since");
    CHECK (phazite::getClips (song, unread) == std::vector { clip });
    CHECK (phazite::getTrackName (song, last) == "Last");
    CHECK (phazite::getTrackIDs (song) == std::vector { first, unread, last }); // in the version's place
    phazite::test::checkSong (song);

    // Its own line: nothing taken.
    phazite::renameTrack (song, unread, "Again");
    const auto steps = undo.getNumSteps();
    undo.check ("Restore Unread", inStep (song, "Restore Unread", [&] { restoreLine (song, unread, version); }));
    CHECK (undo.getNumSteps() == steps); // nothing taken: no step
    CHECK (phazite::getTrackName (song, unread) == "Again");
    undo.walkBackAndForth();
}

TEST_CASE ("A version gives the master another ID in place, its holdings kept; its line put back gives the old one back, and "
           "taken again the new one")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto master = phazite::getMasterID (song);
    DeviceID eq;

    {
        const phazite::UndoStep step (song, "Master");
        eq = phazite::test::addOwnDevice (song, master, "phaziteEq");
        phazite::setChannelVolume (song, master, -6.0f);
    }

    const auto version = phazite::test::copyWholeSong (song);
    auto other = phazite::test::copyWholeSong (song);
    const auto newID = TrackID::fromRaw (song.makeID());
    other.getChildWithName (schema::MASTER).setProperty (schema::trackID, (juce::int64) newID.getRaw(), nullptr);
    phazite::test::UndoCheck undo (s.context);

    undo.check ("Bring Back", inStep (song, "Bring Back", [&] { bringBack (song, other); }));
    CHECK (phazite::getMasterID (song) == newID);
    CHECK (phazite::findDevice (song, eq) != nullptr);
    CHECK (phazite::getChannelVolume (song, newID) == doctest::Approx (-6.0f));
    checkTracksAsIn (song, other);

    // The master's line from the version before: its ID that one's again, as its line by its kind; and from the other again.
    undo.check ("Restore Master", inStep (song, "Restore Master", [&] { restoreLine (song, phazite::getMasterID (song), version); }));
    CHECK (phazite::getMasterID (song) == master);
    CHECK (phazite::findDevice (song, eq) != nullptr);
    checkTracksAsIn (song, version);

    undo.check ("Take Master", inStep (song, "Take Master", [&] { restoreLine (song, phazite::getMasterID (song), other); }));
    CHECK (phazite::getMasterID (song) == newID);
    CHECK (phazite::findDevice (song, eq) != nullptr);
    checkTracksAsIn (song, other);
    undo.walkBackAndForth();
}

TEST_CASE ("A merge's clash on a track's or the master's own field is chosen through the tracks' setters, each way; a whole track "
           "chosen and taken away; what the song couldn't hold refused, nothing written")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto clip = s.addMidiClip (track, 0.0, 4.0);
    const auto master = phazite::getMasterID (song);
    const auto whole = phazite::test::copyWholeSong (song);
    const auto trackPath = pathOf (phazite::test::findTrackIn (whole, track));
    const auto masterPath = pathOf (whole.getChildWithName (schema::MASTER));
    const phazite::MergeSides sides;
    phazite::test::UndoCheck undo (s.context);

    const auto propertyClash =
        [] (const juce::StringArray& path, const juce::Identifier& property, juce::var base, juce::var ours, juce::var theirs)
    {
        phazite::MergeClash clash;
        clash.path = path;
        clash.property = property;
        clash.base = std::move (base);
        clash.ours = std::move (ours);
        clash.theirs = std::move (theirs);
        return clash;
    };

    const auto choose = [&] (const juce::String& what, const phazite::MergeClash& clash, int value)
    {
        auto choice = phazite::AreaChoice::notTheArea;
        undo.check (what, inStep (song, what, [&] { choice = phazite::chooseTrackClash (song, clash, value, sides); }));
        return choice;
    };

    // A track's name, each way: the other line's, what was before (none: its number), this line's.
    const auto name = propertyClash (trackPath, schema::trackName, {}, "Ours", "Theirs");
    CHECK (choose ("Theirs", name, 2) == phazite::AreaChoice::chosen);
    CHECK (held (song, track).name == std::optional (juce::String ("Theirs")));
    CHECK (choose ("Before", name, 0) == phazite::AreaChoice::chosen);
    CHECK_FALSE (held (song, track).name.has_value());
    CHECK (choose ("Ours", name, 1) == phazite::AreaChoice::chosen);
    CHECK (held (song, track).name == std::optional (juce::String ("Ours")));

    // A volume and a pan, a colour, a key, an input and its monitoring.
    CHECK (choose ("Volume", propertyClash (trackPath, schema::channelVolume, {}, -3.0f, -12.0f), 2) == phazite::AreaChoice::chosen);
    CHECK (held (song, track).volume == doctest::Approx (-12.0f));
    CHECK (choose ("Pan", propertyClash (trackPath, schema::channelPan, {}, 0.25f, -0.5f), 2) == phazite::AreaChoice::chosen);
    CHECK (held (song, track).pan == doctest::Approx (-0.5f));
    CHECK (choose ("Colour", propertyClash (trackPath, schema::trackColour, {}, "ff00ff00", "ffff0000"), 2) == phazite::AreaChoice::chosen);
    CHECK (held (song, track).colour == std::optional (juce::Colours::red.getARGB()));
    CHECK (choose ("Key", propertyClash (trackPath, schema::keyRoot, {}, 2, 7), 2) == phazite::AreaChoice::chosen);
    REQUIRE (held (song, track).key.has_value());
    CHECK (held (song, track).key->root == 7);
    CHECK (choose ("Input", propertyClash (trackPath, schema::trackInput, {}, "1", "1+2"), 2) == phazite::AreaChoice::chosen);
    CHECK (held (song, track).input == "1+2");
    CHECK (choose ("Monitoring", propertyClash (trackPath, schema::trackMonitoring, {}, schema::monitorNever, schema::monitorAlways), 2)
           == phazite::AreaChoice::chosen);
    CHECK (held (song, track).monitoring == TrackValue::Monitoring::always);

    // The master's volume, and its ID, which a version may give it anew.
    CHECK (choose ("Master Volume", propertyClash (masterPath, schema::channelVolume, {}, -6.0f, -1.0f), 2) == phazite::AreaChoice::chosen);
    CHECK (held (song, master).volume == doctest::Approx (-1.0f));
    const auto newMaster = (juce::int64) song.makeID();
    CHECK (choose ("Master ID",
                   propertyClash (masterPath, schema::trackID, (juce::int64) master.getRaw(), (juce::int64) master.getRaw(), newMaster), 2)
           == phazite::AreaChoice::chosen);
    CHECK ((juce::int64) phazite::getMasterID (song).getRaw() == newMaster);
    phazite::test::checkSong (song);

    // Refused, nothing written: a track that holds a clip made a folder; a track's ID; a property its value doesn't cover; a
    // part deeper in a track; a track the song hasn't there; a master taken away whole.
    const auto someID = (juce::int64) song.makeID();
    const auto text = phazite::test::writeSong (song, phazite::test::SongPart::whole);
    auto deeperPath = trackPath;
    deeperPath.add ("FUTURE:1");
    auto elsewhere = pathOf (phazite::test::findTrackIn (whole, other));
    elsewhere.insert (1, "TRACK:" + juce::String ((juce::int64) track.getRaw()));
    phazite::MergeClash noMaster;
    noMaster.path = masterPath;
    noMaster.baseElement = whole.getChildWithName (schema::MASTER);
    noMaster.oursElement = noMaster.baseElement;

    for (const auto& [what, clash] : std::vector<std::pair<juce::String, phazite::MergeClash>> {
             { "Kind", propertyClash (trackPath, schema::trackKind, {}, schema::trackIsTrack, schema::trackIsFolder) },
             { "ID", propertyClash (trackPath, schema::trackID, {}, {}, someID) },
             { "Unknown", propertyClash (trackPath, "future", {}, 1, 2) },
             { "Deeper", propertyClash (deeperPath, "future", {}, 1, 2) },
             { "Elsewhere", propertyClash (elsewhere, schema::trackName, {}, "A", "B") },
             { "No Master", noMaster } })
    {
        INFO (what);
        const phazite::UndoStep step (song, what);
        CHECK (phazite::chooseTrackClash (song, clash, 2, sides) == phazite::AreaChoice::refused);
    }

    CHECK (phazite::test::writeSong (song, phazite::test::SongPart::whole) == text);
    CHECK (phazite::getClips (song, track) == std::vector { clip });

    // A whole track: the other line's in its place (another name, without the clip), then taken away where that line has none.
    auto theirs = phazite::test::findTrackIn (whole, track).createCopy();
    theirs.setProperty (schema::trackName, "Whole", nullptr);

    for (int i = theirs.getNumChildren(); --i >= 0;)
        if (theirs.getChild (i).hasType (schema::CLIP))
            theirs.removeChild (i, nullptr);

    phazite::MergeClash wholeTrack;
    wholeTrack.path = trackPath;
    wholeTrack.baseElement = phazite::test::findTrackIn (whole, track);
    wholeTrack.oursElement = wholeTrack.baseElement;
    wholeTrack.theirsElement = theirs;
    CHECK (choose ("Whole Track", wholeTrack, 2) == phazite::AreaChoice::chosen);
    CHECK (phazite::getTrackName (song, track) == "Whole");
    CHECK (phazite::getClips (song, track).empty());
    CHECK (phazite::getTrackIDs (song) == std::vector { track, other }); // in its place
    phazite::test::checkSong (song);

    wholeTrack.theirsElement = {};
    CHECK (choose ("Whole Track Gone", wholeTrack, 2) == phazite::AreaChoice::chosen);
    CHECK_FALSE (phazite::test::hasTrack (song, track));
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

namespace
{
// A version that holds a track whose ID, or its fader's, the song holds as another kind of item, as two lines that counted their
// IDs on from the same place make one (another computer's branch switched to or restored: known bug 199's ground): the version
// taken with a track the song then made, that track deleted and the song's count set back to its ID (or its fader's), so
// that the item the song makes next takes it.
struct HeldElsewhere
{
    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    TrackID first = s.firstTrack;
    TrackID other {}, returnTrack {}, versionTrack {};
    juce::ValueTree version {};

    explicit HeldElsewhere (const juce::String& kind)
    {
        other = s.addTrack ("Other");
        returnTrack = s.addReturn (first, "phaziteReverb"); // a return, for a send of the other track's
        versionTrack = s.addTrack ("Version's");
        version = phazite::test::copyWholeSong (song);
        const auto fader = held (song, versionTrack).fader;
        REQUIRE (fader.isValid());
        phazite::deleteTracks (song, { versionTrack }, nullptr);
        REQUIRE_FALSE (phazite::test::hasTrack (song, versionTrack));

        const auto taken = kind == "a clip's (the fader's ID)" ? fader.getRaw() : versionTrack.getRaw();
        phazite::countIDsFrom (song, taken, phazite::IDCount::mayBeBehind);
        const phazite::UndoStep step (song, "Since");
        std::uint64_t made = 0;

        if (kind == "a device's")
            made = phazite::test::addOwnDevice (song, first, "phaziteEq").getRaw();
        else if (kind == "a clip's (the fader's ID)")
            made = s.addMidiClip (first, 0.0, 4.0).getRaw();
        else if (kind == "a send's")
            made = phazite::addSend (song, other, returnTrack, false).getRaw();
        else if (kind == "a modulator's")
            made = s.addModulator (first, phazite::ModulatorType::lfo).getRaw();
        else if (kind == "a section's")
            made = phazite::addSectionAt (song, phazite::BeatPosition(), nullptr).getRaw();

        REQUIRE (made == taken);
        REQUIRE (phazite::findIDHeldTwice (song) == 0);
    }
};

const std::vector<juce::String> heldKinds { "a device's", "a clip's (the fader's ID)", "a send's", "a modulator's", "a section's" };
} // namespace

TEST_CASE ("A version's track whose ID or fader's ID the song holds as another kind of item comes in with the whole version, in "
           "the history and outside it, the other item going with its area's pass")
{
    // Before, the tracks' setter refused the track (Tracks.h: IDsHeldElsewhere), as the item of its ID went only later, in its
    // own area's pass, and the song was left without it (a device of its ID).
    for (const auto& kind : heldKinds)
    {
        INFO (kind);

        {
            HeldElsewhere h (kind);
            phazite::test::UndoCheck undo (h.s.context);
            undo.check ("Bring Back", inStep (h.song, "Bring Back", [&h] { bringBack (h.song, h.version); }));
            CHECK (phazite::test::hasTrack (h.song, h.versionTrack));
            CHECK (phazite::findIDHeldTwice (h.song) == 0);
            checkTracksAsIn (h.song, h.version);
            undo.walkBackAndForth();
        }

        {
            HeldElsewhere h (kind);
            phazite::makeSongTheSame (h.song, h.version);
            CHECK (phazite::test::hasTrack (h.song, h.versionTrack));
            CHECK (phazite::findIDHeldTwice (h.song) == 0);
            checkTracksAsIn (h.song, h.version);
        }
    }
}

TEST_CASE ("A version's line whose track's ID the song holds as a device on another track is refused, the song as it was, no step")
{
    HeldElsewhere h ("a device's");
    const auto text = phazite::test::writeSong (h.song, phazite::test::SongPart::whole);
    CHECK (phazite::givesTrackIDsHeldElsewhere (h.song, h.versionTrack, h.version));
    CHECK_FALSE (phazite::givesTrackIDsHeldElsewhere (h.song, h.first, h.version)); // a track of both, its IDs its own

    phazite::test::UndoCheck undo (h.s.context);
    const auto steps = undo.getNumSteps();
    undo.check ("Restore", inStep (h.song, "Restore", [&h] { CHECK_FALSE (phazite::makeTrackLike (h.song, h.versionTrack, h.version)); }));
    CHECK (undo.getNumSteps() == steps);
    CHECK (phazite::test::writeSong (h.song, phazite::test::SongPart::whole) == text);
    CHECK_FALSE (phazite::test::hasTrack (h.song, h.versionTrack));
}
