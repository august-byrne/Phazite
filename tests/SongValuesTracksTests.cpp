// SPDX-License-Identifier: AGPL-3.0-or-later
// What hears the song's tracks, folders, returns and master change hears it from one place (model/base/internal/SongValues.h:
// Listener::tracksChanged(), trackSwitchesChanged(), tracksViewChanged()), by their IDs (roadmap step S5.20; while the song's
// tree is still their storage, the tracks' part of the keeper tells it from the tree: model/base/internal/TracksArea.h). For
// each edit of tracks made through the model, its Undo and its Redo, the listener is told every track that differs before
// and after, compared as values (getTracksValue(), readTrackSwitches(), readTracksOutside(): the readers, independent of
// what tells it), with its folder and its place before and after, at least the parts that differ and its value after as the
// readers read it; one told that doesn't differ was taken out and came back (a track JUCE moves); every list whose tracks came
// in another order; each switch that differs of a track the song holds before and after; each track whose view differs; and
// the tracks whole after as the readers read them. What a step told in all is checked, never how many calls (the tree's
// count until the tracks' switch). Another area's change tells nothing of the tracks. And what plays makes the plans it made
// per edit, and plays a volume and a mute at once; the unsaved check, the versions' lines and a learning fader follow them
// too.
//   phazite_tests.exe --test-case="Song values' tracks*"
#include "BenchmarkSong.h"
#include "EnginePlayback.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "devices/drums/DrumSampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/midi/MidiLinkModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace
{
namespace dev = phazite::devices;
using phazite::SongTracks;
using phazite::TrackChange;
using phazite::TrackID;
using phazite::TracksOutside;
using phazite::TrackSwitchChange;
using phazite::TrackSwitches;
using phazite::TrackValue;
using Field = TrackSwitchChange::Field;

// What a track's changes told, in all: whether it was there as the first told it and is as the last told it, its folder and
// its place then and now, the parts of all of them, and its value after the last.
struct Entry
{
    bool wasThere = false, isThere = false;
    TrackID parentBefore {}, parentAfter {};
    int indexBefore = -1, indexAfter = -1;
    std::uint16_t parts = 0;
    std::optional<TrackValue> now {};
};

// A track's place in its folder's list (the top list for none), or -1.
int findIndex (const SongTracks& tracks, TrackID parent, TrackID id)
{
    if (const auto* list = tracks.findList (parent))
        for (size_t i = 0; i < list->size(); ++i)
            if ((*list)[i] == id)
                return (int) i;

    return -1;
}

// An element's place from the song down, as a merge's clash names it ("TRACKS:TRACKS", "MASTER:MASTER").
juce::StringArray pathOf (const juce::ValueTree& element)
{
    juce::StringArray path;

    for (auto each = element; each.isValid() && each.getParent().isValid(); each = each.getParent())
        path.insert (0, each.getType().toString() + ":" + phazite::schema::getElementKey (each));

    return path;
}

// How many calls the values' listener heard, of each area, and what it was told of the tracks, each by its ID (the master's
// apart).
struct Told final : phazite::SongValues::Listener
{
    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++notes; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<TrackID>&) override { ++clips; }

    void modulationChanged (const std::vector<phazite::SongValues::ModulationChange>&, const std::vector<phazite::ModulatorPlace>&) override
    {
        ++modulation;
    }

    void sendsChanged (const std::vector<phazite::SongValues::SendChange>&, const std::vector<TrackID>&, const phazite::SongSends&) override
    {
        ++sends;
    }

    void devicesChanged (const std::vector<phazite::SongValues::DeviceChange>&, const std::vector<phazite::DevicePlace>&,
                         const phazite::SongDevices&) override
    {
        ++devices;
    }

    void tracksChanged (const std::vector<TrackChange>& changes, const std::vector<TrackID>& reorderedNow, const SongTracks& now) override
    {
        ++calls;
        CHECK_FALSE ((changes.empty() && reorderedNow.empty()));
        CHECK (phazite::holdsItsLists (now));

        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            CHECK (std::none_of (changes.begin(), changes.begin() + (std::ptrdiff_t) i, [&change] (const TrackChange& other)
                                 { return other.onMaster == change.onMaster && other.id == change.id; })); // each once in a call
            CHECK ((change.wasThere || change.isThere));
            CHECK ((change.now != nullptr) == change.isThere);
            CHECK (change.parts != 0);

            if (change.onMaster)
            {
                CHECK (change.now == &now.master);
                auto& entry = master.has_value() ? *master : master.emplace();
                entry.parts = (std::uint16_t) (entry.parts | change.parts);
                entry.now = now.master;
                continue;
            }

            if (change.now != nullptr)
            {
                CHECK (change.now->id == change.id);
                CHECK (change.now->parent == change.parentAfter);
                CHECK (now.tracks.find (change.id.getRaw()) == change.now); // the value after handed whole, the change's in it

                // Its place, for one that came or came to another folder alone (TracksArea.h).
                if (change.has (TrackChange::made) || change.has (TrackChange::place))
                    CHECK (change.indexAfter == findIndex (now, change.parentAfter, change.id));
                else
                    CHECK ((change.indexBefore == -1 && change.indexAfter == -1));
            }
            else
            {
                CHECK (now.tracks.find (change.id.getRaw()) == nullptr);
            }

            auto [found, first] = told.try_emplace (change.id.getRaw());
            auto& entry = found->second;

            if (first)
            {
                entry.wasThere = change.wasThere;
                entry.parentBefore = change.parentBefore;
                entry.indexBefore = change.indexBefore;
            }

            entry.isThere = change.isThere;
            entry.parentAfter = change.parentAfter;
            entry.indexAfter = change.indexAfter;
            entry.parts = (std::uint16_t) (entry.parts | change.parts);
            entry.now = change.now != nullptr ? std::optional (*change.now) : std::nullopt;
        }

        for (const auto folder : reorderedNow)
            reordered.insert (folder.getRaw());

        last = now;
    }

    void trackSwitchesChanged (const std::vector<TrackSwitchChange>& changes, const TrackSwitches&) override
    {
        ++switchCalls;
        CHECK_FALSE (changes.empty());

        for (const auto& change : changes)
            switches.insert ({ change.onMaster ? 0 : change.track.getRaw(), (int) change.field });
    }

    void tracksViewChanged (const std::vector<TrackID>& tracks, bool ofMaster) override
    {
        ++viewCalls;
        CHECK_FALSE ((tracks.empty() && ! ofMaster));

        for (const auto track : tracks)
            view.insert (track.getRaw());

        viewMaster = viewMaster || ofMaster;
    }

    int calls = 0, switchCalls = 0, viewCalls = 0, notes = 0, clips = 0, modulation = 0, sends = 0, devices = 0;
    std::map<std::uint64_t, Entry> told {};
    std::optional<Entry> master {};
    std::set<std::uint64_t> reordered {};
    std::set<std::pair<std::uint64_t, int>> switches {};
    std::set<std::uint64_t> view {};
    bool viewMaster = false;
    std::optional<SongTracks> last {};
};

// The parts in which a track differs between two values of the song's tracks, worked out here (independent of the keeper):
// none where it is the same, `made` where it comes or goes.
std::uint16_t findDifferingParts (const TrackValue* was, const TrackValue* is)
{
    if (was == nullptr || is == nullptr)
        return was == is ? 0 : (std::uint16_t) TrackChange::made;

    std::uint16_t parts = 0;
    const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

    if (was->parent != is->parent)
        add (TrackChange::place);

    if (was->kind != is->kind)
        add (TrackChange::kind);

    if (was->bus != is->bus)
        add (TrackChange::bus);

    if (was->fader != is->fader || was->vca != is->vca)
        add (TrackChange::fader);

    if (was->name != is->name)
        add (TrackChange::name);

    if (was->colour != is->colour)
        add (TrackChange::colour);

    if (! juce::exactlyEqual (was->volume, is->volume) || ! juce::exactlyEqual (was->pan, is->pan))
        add (TrackChange::mixer);

    if (was->key != is->key)
        add (TrackChange::key);

    if (was->input != is->input || was->monitoring != is->monitoring)
        add (TrackChange::recording);

    if (was->padDevice != is->padDevice || was->pad != is->pad)
        add (TrackChange::pad);

    if (was->id != is->id)
        add (TrackChange::newID);

    return parts;
}

// One switch of a track as a value of the switches holds it (the master's mode under 0).
std::optional<int> getSwitch (const TrackSwitches& switches, std::uint64_t key, Field field)
{
    const auto on = [key] (const immer::map<std::uint64_t, bool>& map)
    { return map.find (key) != nullptr ? std::optional (1) : std::nullopt; };

    switch (field)
    {
        case Field::mute:
            return on (switches.mutes);
        case Field::solo:
            return on (switches.solos);
        case Field::frozen:
            return on (switches.frozen);
        case Field::automationMode:
            if (const auto* mode = switches.modes.find (key))
                return (int) *mode;

            return std::nullopt;
    }

    return std::nullopt;
}

// A song in a project of its own, with its versions, the values' listener recording what it is told.
struct Watched
{
    Watched()
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        song.getValues()->addListener (&told);
    }

    ~Watched() { song.getValues()->removeListener (&told); }

    // What a change told, how far the project's count of changes went up meanwhile, and the song's tracks, their switches and
    // their view before and after, as the readers read them.
    struct Heard
    {
        Told told {};
        juce::int64 project = 0;
        SongTracks before {}, after {};
        TrackSwitches switchesBefore {}, switchesAfter {};
        TracksOutside viewBefore {}, viewAfter {};
    };

    Heard hear (const std::function<void()>& change)
    {
        told = {};
        Heard heard;
        const auto projectBefore = s.getProject().getChangeCount();
        heard.before = phazite::getTracksValue (song);
        heard.switchesBefore = phazite::readTrackSwitches (song.getState());
        heard.viewBefore = phazite::readTracksOutside (song.getState());
        change();
        heard.told = told;
        heard.project = s.getProject().getChangeCount() - projectBefore;
        heard.after = phazite::getTracksValue (song);
        heard.switchesAfter = phazite::readTrackSwitches (song.getState());
        heard.viewAfter = phazite::readTracksOutside (song.getState());
        return heard;
    }

    // A change as one undo step, then its Undo, then its Redo: what each told.
    std::vector<Heard> hearEach (const juce::String& name, const std::function<void()>& change)
    {
        return { hear (
                     [&]
                     {
                         const phazite::UndoStep step (song, name);
                         change();
                     }),
                 hear ([this] { phazite::undo (song); }), hear ([this] { phazite::redo (song); }) };
    }

    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    Told told;
};

// What a change and its Undo and Redo told, each against the song's tracks before and after: every track that differs told,
// as it was before (where) and is after (there or not, where, its value as the readers read it), with at least the parts
// that differ; one told that doesn't differ was taken out and put in again (a track JUCE moves); every list whose tracks came
// in another order told; the master likewise; each switch and view that differs of a track the song holds before and after;
// the tracks whole after as the readers read them; and nothing of another area unless othersToo.
void checkHeard (const juce::String& name, const std::vector<Watched::Heard>& heard, bool othersToo = false)
{
    INFO (name.toStdString());
    const std::array<const char*, 3> moments { "the step", "its Undo", "its Redo" };
    REQUIRE (heard.size() <= moments.size());

    for (size_t i = 0; i < heard.size(); ++i)
    {
        INFO (moments[i]);
        const auto& each = heard[i];
        std::set<std::uint64_t> ids;

        for (const auto* tracks : { &each.before, &each.after })
            for (const auto& [id, value] : tracks->tracks)
                ids.insert (id);

        auto differs = false;

        for (const auto id : ids)
        {
            INFO ("track " << id);
            const auto* was = each.before.tracks.find (id);
            const auto* is = each.after.tracks.find (id);
            const auto parts = findDifferingParts (was, is);
            const auto found = each.told.told.find (id);

            if (parts == 0 && found == each.told.told.end())
                continue;

            differs = differs || parts != 0;
            REQUIRE (found != each.told.told.end());
            const auto& entry = found->second;
            CHECK (((entry.parts & parts) == parts || (entry.parts & TrackChange::made) != 0));
            CHECK (entry.isThere == (is != nullptr));
            CHECK (entry.wasThere == (was != nullptr));

            if (parts == 0)
                CHECK ((entry.parts & TrackChange::made) != 0); // taken out and put in again

            if (was != nullptr && entry.wasThere)
            {
                CHECK (entry.parentBefore == was->parent);
            }

            if (is != nullptr)
            {
                CHECK (entry.parentAfter == is->parent);
                REQUIRE (entry.now.has_value());
                CHECK (*entry.now == *is);
            }
        }

        // Nothing told of a track the song had neither before nor after.
        for (const auto& [id, entry] : each.told.told)
            CHECK (ids.contains (id));

        // The master.
        if (const auto parts = findDifferingParts (&each.before.master, &each.after.master); parts != 0)
        {
            differs = true;
            REQUIRE (each.told.master.has_value());
            CHECK ((each.told.master->parts & parts) == parts);
        }

        if (each.told.master.has_value())
            CHECK (*each.told.master->now == each.after.master);

        // Each list that holds the tracks it held before in another order.
        for (const auto& [owner, list] : each.before.lists)
        {
            const auto* other = each.after.lists.find (owner);

            if (other == nullptr)
                continue;

            std::vector<TrackID> keptBefore, keptAfter;

            for (const auto id : list)
                if (std::find (other->begin(), other->end(), id) != other->end())
                    keptBefore.push_back (id);

            for (const auto id : *other)
                if (std::find (list.begin(), list.end(), id) != list.end())
                    keptAfter.push_back (id);

            if (keptBefore != keptAfter)
            {
                differs = true;
                CHECK_MESSAGE (each.told.reordered.contains (owner), "list " << (juce::int64) owner << " not told reordered");
            }
        }

        // The switches and the view of each track the song holds before and after (one that comes or goes is the tracks').
        const auto isHeldThrough = [&each] (std::uint64_t key)
        { return key == 0 || (each.before.tracks.find (key) != nullptr && each.after.tracks.find (key) != nullptr); };

        std::set<std::uint64_t> keys { 0 };

        for (const auto& [id, value] : each.after.tracks)
            keys.insert (id);

        for (const auto key : keys)
        {
            if (! isHeldThrough (key))
                continue;

            for (const auto field : { Field::mute, Field::solo, Field::frozen, Field::automationMode })
            {
                if (getSwitch (each.switchesBefore, key, field) != getSwitch (each.switchesAfter, key, field))
                {
                    differs = true;
                    CHECK_MESSAGE (each.told.switches.contains ({ key, (int) field }),
                                   "track " << (juce::int64) key << "'s switch " << (int) field << " not told");
                }
            }

            if (key == 0)
                continue;

            const auto* viewBefore = each.viewBefore.tracks.find (key);
            const auto* viewAfter = each.viewAfter.tracks.find (key);

            if (! ((viewBefore != nullptr ? *viewBefore : phazite::TrackView())
                   == (viewAfter != nullptr ? *viewAfter : phazite::TrackView())))
                CHECK_MESSAGE (each.told.view.contains (key), "track " << (juce::int64) key << "'s view not told");
        }

        // Nothing told of a switch that didn't change, but of a track that came.
        for (const auto& [key, field] : each.told.switches)
            if (isHeldThrough (key))
                CHECK (getSwitch (each.switchesBefore, key, (Field) field) != getSwitch (each.switchesAfter, key, (Field) field));

        if (! (each.viewBefore.master == each.viewAfter.master))
            CHECK (each.told.viewMaster);

        // The tracks whole after, as the readers read them.
        if (each.told.last.has_value())
            CHECK (*each.told.last == each.after);

        if (differs)
            CHECK ((each.told.calls > 0 || each.told.switchCalls > 0));

        if (! othersToo)
        {
            CHECK (each.told.clips == 0);
            CHECK (each.told.notes == 0);
            CHECK (each.told.modulation == 0);
            CHECK (each.told.sends == 0);
            CHECK (each.told.devices == 0);
        }
    }
}

bool toldOf (const Watched::Heard& heard, TrackID id)
{
    return heard.told.told.contains (id.getRaw());
}

bool toldAny (const Watched::Heard& heard, TrackID id, std::uint16_t parts)
{
    const auto found = heard.told.told.find (id.getRaw());
    return found != heard.told.told.end() && (found->second.parts & parts) != 0;
}

juce::ValueTree parse (const juce::String& xml)
{
    const auto parsed = juce::parseXML (xml);
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

juce::String writeText (const juce::ValueTree& song)
{
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, song);
    return text.toString();
}
} // namespace

TEST_CASE ("Song values' tracks: each edit of tracks, its Undo and its Redo tell each track that differs, and nothing else")
{
    Watched w;
    auto& song = w.song;
    auto& s = w.s;
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    const auto third = s.addTrack ("Third");

    checkHeard ("Add Track", w.hearEach ("Add Track", [&] { phazite::insertTrack (song, first, nullptr); }), true);

    const auto renamed = w.hearEach ("Rename", [&] { phazite::renameTrack (song, second, "Bass"); });
    checkHeard ("Rename", renamed);
    CHECK (toldAny (renamed[0], second, TrackChange::name));
    CHECK_FALSE (toldAny (renamed[0], second, TrackChange::made));

    checkHeard ("Colour", w.hearEach ("Colour", [&] { phazite::setTrackColours (song, { second, third }, juce::Colour (0xff336699)); }));
    checkHeard ("Key", w.hearEach ("Key", [&] { phazite::setTrackKey (song, second, phazite::Key { 2, phazite::Scale::minor }); }));
    checkHeard ("Volume", w.hearEach ("Volume", [&] { phazite::setChannelVolume (song, second, -6.0f); }));
    checkHeard ("Pan", w.hearEach ("Pan", [&] { phazite::setChannelPan (song, second, 0.25f); }));

    // A drag: one step from its start to its commit, its Undo and Redo.
    checkHeard ("Drag Volume", { w.hear (
                                     [&]
                                     {
                                         phazite::Gesture gesture (song, "Drag Volume");
                                         phazite::setChannelVolume (song, third, -1.0f);
                                         phazite::setChannelVolume (song, third, -2.0f);
                                         phazite::setChannelVolume (song, third, -3.0f);
                                         gesture.commit();
                                     }),
                                 w.hear ([&] { phazite::undo (song); }), w.hear ([&] { phazite::redo (song); }) });

    checkHeard ("Move", w.hearEach ("Move", [&] { phazite::moveTracksAfter (song, { first }, third); }), true);

    // Grouped into a folder, moved into it and out, the folder made a bus and plain again, its VCA dragged, ungrouped.
    auto folder = TrackID();
    checkHeard ("Group", w.hearEach ("Group", [&] { folder = phazite::groupIntoFolder (song, { second, third }, nullptr); }), true);
    REQUIRE (phazite::isFolder (song, folder));
    checkHeard ("Into Folder", w.hearEach ("Into Folder", [&] { phazite::moveTrackIntoFolder (song, first, folder); }), true);
    checkHeard ("Out of Folder", w.hearEach ("Out of Folder", [&] { phazite::moveTracksAfter (song, { first }, folder); }), true);
    checkHeard ("Bus", w.hearEach ("Bus", [&] { phazite::setMixedAsBus (song, folder, true); }), true);
    checkHeard ("Plain", w.hearEach ("Plain", [&] { phazite::setMixedAsBus (song, folder, false); }), true);
    checkHeard ("VCA", w.hearEach ("VCA", [&] { phazite::setChannelVolume (song, folder, -4.0f); }));
    checkHeard ("Ungroup", w.hearEach ("Ungroup", [&] { phazite::ungroupFolder (song, folder); }), true);

    checkHeard ("Duplicate", w.hearEach ("Duplicate", [&] { phazite::duplicateTracks (song, { second }, nullptr); }), true);
    checkHeard ("Return", w.hearEach ("Return", [&] { phazite::insertReturnTrack (song, "Reverb", nullptr); }), true);

    // A pad given a track of its own and brought back.
    const auto drums = s.setInstrument (first, dev::DrumSampler::typeID);
    checkHeard ("Pad", w.hearEach ("Pad", [&] { phazite::givePadItsOwnTrack (song, drums, 36); }), true);
    checkHeard ("Pad Back", w.hearEach ("Pad Back", [&] { phazite::bringPadBack (song, drums, 36); }), true);

    // Its input and monitoring (each a step of its own).
    checkHeard ("Input", { w.hear ([&] { phazite::setTrackInput (s.context, third, "1+2"); }), w.hear ([&] { phazite::undo (song); }),
                           w.hear ([&] { phazite::redo (song); }) });
    checkHeard ("Monitoring", { w.hear ([&] { phazite::setInputMonitoring (s.context, third, phazite::InputMonitoring::always); }),
                                w.hear ([&] { phazite::undo (song); }), w.hear ([&] { phazite::redo (song); }) });

    // A bounce's freeze of its source, in its step (what is frozen outside the history: the case below).
    const auto file = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Frozen.wav"), 0.1);
    checkHeard ("Bounce's Freeze", w.hearEach ("Bounce", [&] { phazite::freezeFromFile (song, third, file, &song.getUndoManager()); }),
                true);

    const auto deleted = w.hearEach ("Delete", [&] { phazite::deleteTracks (song, { second }, nullptr); });
    checkHeard ("Delete", deleted, true);
    CHECK_FALSE (deleted[0].told.told.at (second.getRaw()).isThere);
    CHECK (deleted[1].told.told.at (second.getRaw()).isThere);

    // A version applied: the song as it was at the start of this test, each way, and one track's line.
    const auto then = phazite::test::copyWholeSong (song);

    {
        const phazite::UndoStep step (song, "Change");
        phazite::renameTrack (song, third, "Changed");
        phazite::setChannelVolume (song, third, -12.0f);
        phazite::insertTrack (song, third, nullptr);
    }

    const auto now = phazite::test::copyWholeSong (song);
    checkHeard ("Make Song Like", w.hearEach ("Make Song Like", [&] { phazite::makeSongLike (song, then); }), true);
    checkHeard ("Make Song Like Again", { w.hear ([&] { phazite::makeSongTheSame (song, now); }) }, true);
    checkHeard ("Make Track Like", w.hearEach ("Make Track Like", [&] { phazite::makeTrackLike (song, third, then); }), true);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks: a mute, a solo, a solo alone, an automation mode and a freeze outside the history tell the switches "
           "alone, and the view the view alone")
{
    Watched w;
    auto& song = w.song;
    auto& s = w.s;
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");

    const auto expectSwitch = [&] (const char* what, const std::function<void()>& change, TrackID track, Field field)
    {
        INFO (what);
        const auto heard = w.hear (change);
        checkHeard (what, { heard });
        CHECK (heard.told.calls == 0);
        CHECK (heard.told.viewCalls == 0);
        CHECK (heard.told.switches.contains ({ track.getRaw(), (int) field }));
        CHECK (heard.project > 0); // counted as a change of the song's, as its property was
    };

    expectSwitch ("Mute", [&] { phazite::toggleTracksMuted (song, { first }); }, first, Field::mute);
    expectSwitch ("Unmute", [&] { phazite::toggleTracksMuted (song, { first }); }, first, Field::mute);
    expectSwitch ("Solo", [&] { phazite::toggleTracksSoloed (song, { second }); }, second, Field::solo);
    expectSwitch ("Solo Alone", [&] { phazite::toggleSoloAlone (song, first); }, first, Field::solo);
    expectSwitch ("Latch", [&] { phazite::setAutomationMode (song, first, phazite::AutomationMode::latch); }, first, Field::automationMode);

    const auto file = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Frozen.wav"), 0.1);
    expectSwitch ("Freeze", [&] { phazite::freezeFromFile (song, second, file); }, second, Field::frozen);
    expectSwitch ("Unfreeze", [&] { phazite::unfreeze (song, second); }, second, Field::frozen);

    // The view: the view alone, no change of the song's.
    const auto expectView = [&] (const char* what, const std::function<void()>& change, TrackID track)
    {
        INFO (what);
        const auto heard = w.hear (change);
        checkHeard (what, { heard });
        CHECK (heard.told.calls == 0);
        CHECK (heard.told.switchCalls == 0);
        CHECK (heard.told.view.contains (track.getRaw()));
        CHECK (heard.project == 0);
    };

    expectView ("Height", [&] { phazite::setSavedTrackHeight (song, first, 120); }, first);
    expectView ("Collapsed", [&] { phazite::setCollapsed (song, first, true); }, first);
    expectView ("Pinned", [&] { phazite::setPinned (song, second, true); }, second);
    expectView ("Drum Rows", [&] { phazite::setDrumRowOrder (song, second, { 38, 36 }); }, second);
    const auto volume = s.getVolumeParameter (first);
    expectView ("Lane Shown", [&] { phazite::showLane (song, first, volume); }, first);
    expectView ("Lane Resized", [&] { phazite::setLaneHeight (song, first, volume, 80); }, first);
    expectView ("Lane Removed", [&] { phazite::removeLane (song, first, volume); }, first);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks: the master's volume, automation mode and height, and an ID a version gives it anew")
{
    Watched w;
    auto& song = w.song;
    const auto master = phazite::getMasterID (song);

    const auto volume = w.hearEach ("Master Volume", [&] { phazite::setChannelVolume (song, master, -6.0f); });
    checkHeard ("Master Volume", volume);
    REQUIRE (volume[0].told.master.has_value());
    CHECK ((volume[0].told.master->parts & TrackChange::mixer) != 0);
    CHECK (volume[0].told.told.empty());

    const auto mode = w.hear ([&] { phazite::setAutomationMode (song, master, phazite::AutomationMode::touch); });
    checkHeard ("Master Mode", { mode });
    CHECK (mode.told.switches.contains ({ 0, (int) Field::automationMode }));

    const auto height = w.hear ([&] { phazite::setSavedTrackHeight (song, master, 100); });
    checkHeard ("Master Height", { height });
    CHECK (height.told.viewMaster);
    CHECK (height.told.calls == 0);

    // A version whose master has another ID: given in place, told as the master's change, never a track gone and another come.
    auto other = phazite::test::copyWholeSong (song);
    const auto newID = song.makeID();
    other.getChildWithName (phazite::schema::MASTER).setProperty (phazite::schema::trackID, (juce::int64) newID, nullptr);
    const auto given = w.hearEach ("Master ID", [&] { phazite::makeSongLike (song, other); });
    checkHeard ("Master ID", given, true);
    REQUIRE (given[0].told.master.has_value());
    CHECK ((given[0].told.master->parts & TrackChange::newID) != 0);
    CHECK (given[0].told.master->now->id.getRaw() == newID);
    CHECK_FALSE (given[0].told.told.contains (newID));
    CHECK_FALSE (given[0].told.told.contains (master.getRaw()));
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks: another area's change tells nothing of the tracks")
{
    Watched w;
    auto& s = w.s;
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    const auto reverb = s.addReturn (track, "phaziteReverb");

    const std::vector<std::pair<const char*, std::function<void()>>> edits {
        { "a clip", [&] { s.addMidiClip (track, 0.0, 4.0); } },
        { "an effect", [&] { s.addEffect (track, "phaziteDelay"); } },
        { "a send", [&] { s.addSend (track, reverb, -6.0f); } },
        { "a modulator", [&] { s.addModulator (track, phazite::ModulatorType::lfo); } },
        { "a curve", [&] { s.addAutomation (s.getPanParameter (track), { { phazite::BeatPosition(), 0.5f } }); } },
    };

    for (const auto& [what, edit] : edits)
    {
        INFO (what);
        const auto heard = w.hear (edit);
        CHECK (heard.told.calls == 0);
        CHECK (heard.told.switchCalls == 0);
        CHECK (heard.told.viewCalls == 0);
    }
}

TEST_CASE ("Song values' tracks: what plays makes the plans it made for each edit, and plays a volume, a pan, a mute and a solo at "
           "once")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    const auto other = s.addTrack ("Other");
    const auto folder = phazite::groupIntoFolder (song, { other }, nullptr);
    phazite::setMixedAsBus (song, folder, false);
    REQUIRE_FALSE (phazite::getChannelFader (song, { .track = folder }).isValid());
    phazite::test::PlanCounter plans (s.context);
    plans.take();

    const auto inStep = [&song] (const char* name, const std::function<void()>& change)
    {
        return [&song, name, change]
        {
            const phazite::UndoStep step (song, name);
            change();
        };
    };

    struct Edit
    {
        const char* what = "";
        std::function<void()> make {};
        int plans = 0;
    };

    // In the order they are made, each on the song as the ones before left it: as many as the tree's callbacks made.
    const std::vector<Edit> edits {
        { "a volume", inStep ("Volume", [&] { phazite::setChannelVolume (song, track, -6.0f); }), 0 },
        { "its Undo", [&] { phazite::undo (song); }, 0 },
        { "its Redo", [&] { phazite::redo (song); }, 0 },
        { "a pan", inStep ("Pan", [&] { phazite::setChannelPan (song, track, 0.5f); }), 0 },
        { "a plain folder's VCA", inStep ("VCA", [&] { phazite::setChannelVolume (song, folder, -3.0f); }), 0 },
        { "a mute", [&] { phazite::toggleTracksMuted (song, { track }); }, 0 },
        { "a solo", [&] { phazite::toggleTracksSoloed (song, { other }); }, 0 },
        { "a height", [&] { phazite::setSavedTrackHeight (song, track, 100); }, 0 },
        { "a rename", inStep ("Rename", [&] { phazite::renameTrack (song, track, "Lead"); }), 1 },
        { "a colour", inStep ("Colour", [&] { phazite::setTrackColours (song, { track }, juce::Colour (0xff112233)); }), 1 },
        { "an automation mode", [&] { phazite::setAutomationMode (song, track, phazite::AutomationMode::latch); }, 1 },
        { "a track added", inStep ("Add", [&] { phazite::insertTrack (song, track, nullptr); }), 1 },
        { "its Undo", [&] { phazite::undo (song); }, 1 },
        { "a track moved", inStep ("Move", [&] { phazite::moveTracksAfter (song, { track }, folder); }), 1 },
        { "the folder a bus", inStep ("Bus", [&] { phazite::setMixedAsBus (song, folder, true); }), 1 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
    }
}

TEST_CASE ("Song values' tracks: a volume, a plain folder's VCA, a mute and a solo set while the song plays are heard in the next "
           "block, from the values the song's values tell")
{
    phazite::test::TestSong s;
    s.setTempo (120.0);
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    s.addNote (s.addMidiClip (track, 0.0, 16.0), 60, 0.0, 16.0);
    const auto other = s.addTrack ("Other");
    s.setInstrument (other, dev::Synth::typeID);
    const auto folder = phazite::groupIntoFolder (song, { track }, nullptr);
    phazite::setMixedAsBus (song, folder, false);
    phazite::test::EnginePlayback playback;
    auto& player = phazite::getSession (s.context)->getPlayer();
    s.run ("transport.playStop");
    playback.play (0.05);
    phazite::test::PlanCounter plans (s.context);
    plans.take();

    auto* strip = player.findStrip ((std::int64_t) track.getRaw());
    REQUIRE (strip != nullptr);

    // Its volume, and the plain folder's VCA above it, at once.
    {
        const phazite::UndoStep step (song, "Volume");
        phazite::setChannelVolume (song, track, -6.0f);
    }

    CHECK (strip->getChannel().getVolume() == doctest::Approx (-6.0f));

    {
        const phazite::UndoStep step (song, "VCA");
        phazite::setChannelVolume (song, folder, -3.0f);
    }

    CHECK (strip->getChannel().getVolume() == doctest::Approx (-9.0f));
    phazite::undo (song);
    CHECK (strip->getChannel().getVolume() == doctest::Approx (-6.0f));

    // A mute, and the other track soloed: not heard, at once; and heard again.
    phazite::toggleTracksMuted (song, { track });
    CHECK_FALSE (strip->getChannel().isAudible());
    phazite::toggleTracksMuted (song, { track });
    CHECK (strip->getChannel().isAudible());
    phazite::toggleTracksSoloed (song, { other });
    CHECK_FALSE (strip->getChannel().isAudible());
    phazite::toggleTracksSoloed (song, { other });
    CHECK (strip->getChannel().isAudible());
    playback.play (0.05);
    CHECK (plans.take() == 0);
}

TEST_CASE ("Song values' tracks: a mute toggled outside the history is an unsaved change, and toggled back is as saved; the view is "
           "none")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto& project = s.getProject();
    const auto track = s.firstTrack;
    s.saveWithPluginStates();
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // The check of what notified since the save (hasUnsavedChanges()), and the whole song against the saved copy agreeing.
    const auto check = [&project] (bool unsaved)
    {
        CHECK (project.hasUnsavedChanges() == unsaved);
        CHECK (project.isWholeAsSaved() == ! unsaved);
    };

    phazite::toggleTracksMuted (song, { track });
    check (true);
    s.saveWithPluginStates();
    check (false);
    phazite::toggleTracksMuted (song, { track });
    phazite::toggleTracksMuted (song, { track });
    check (false);

    phazite::setSavedTrackHeight (song, track, 150);
    phazite::setCollapsed (song, phazite::getMasterID (song), true);
    check (false);

    // A track's own property set and set back, and the tracks moved and moved back, compared against the saved copy.
    const auto other = s.addTrack ("Other");
    s.saveWithPluginStates();
    phazite::renameTrack (song, other, "Renamed");
    phazite::renameTrack (song, other, "Other");
    check (false);
    phazite::moveTracksAfter (song, { track }, other);
    phazite::moveTracksAfter (song, { track }, TrackID());
    check (false);
    phazite::moveTracksAfter (song, { track }, other);
    check (true);
}

TEST_CASE ("Song values' tracks: MIDI learn of a fader ends as its track is taken out, or as it is given another fader")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Track");
    phazite::startMidiLearn (phazite::getVolume (s.context, track));
    REQUIRE (phazite::isLearningMidi (s.context));
    phazite::deleteTracks (song, { track }, nullptr);
    CHECK_FALSE (phazite::isLearningMidi (s.context));

    // A bus's fader, learning, the bus made plain: its fader gone, learning ends.
    const auto inBus = s.addTrack ("In Bus");
    const auto folder = phazite::groupIntoFolder (song, { inBus }, nullptr);
    phazite::setMixedAsBus (song, folder, true);
    REQUIRE (phazite::getChannelFader (song, { .track = folder }).isValid());
    phazite::startMidiLearn (phazite::getVolume (s.context, folder));
    REQUIRE (phazite::isLearningMidi (s.context));
    phazite::renameTrack (song, folder, "Still");
    CHECK (phazite::isLearningMidi (s.context)); // another part of the track: still learning
    phazite::setMixedAsBus (song, folder, false);
    CHECK_FALSE (phazite::isLearningMidi (s.context));
}

TEST_CASE ("Song values' tracks: MIDI learn of the master's fader ends as a merge's choice puts the whole MASTER in anew")
{
    // The tree's callback ended it as the MASTER was taken out; the master told made does now (MidiLinks.h).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto master = phazite::getMasterID (song);
    const auto version = phazite::test::copyWholeSong (song);
    const auto then = version.getChildWithName (phazite::schema::MASTER);
    REQUIRE (then.isValid());

    {
        const phazite::UndoStep step (song, "Change");
        phazite::setChannelVolume (song, master, -6.0f);
    }

    const auto now = phazite::test::copyWholeSong (song);
    phazite::startMidiLearn (phazite::getMasterVolume (s.context));
    REQUIRE (phazite::isLearningMidi (s.context));

    {
        const phazite::UndoStep step (song, "Change Again");
        phazite::setChannelVolume (song, master, -9.0f);
    }

    CHECK (phazite::isLearningMidi (s.context)); // its volume set: still learning

    phazite::MergeClash clash;
    clash.line = "Master";
    clash.trackID = master.getRaw();
    clash.path = pathOf (then);
    clash.baseElement = then;
    clash.oursElement = phazite::test::copyWholeSong (song).getChildWithName (phazite::schema::MASTER);
    clash.theirsElement = then;

    {
        const phazite::UndoStep step (song, "Take the Other Branch's");
        REQUIRE (phazite::chooseTrackClash (song, clash, 2, { version, now, version }) == phazite::AreaChoice::chosen);
    }

    CHECK_FALSE (phazite::isLearningMidi (s.context));
}

TEST_CASE ("Song values' tracks, benchmark: the 200-track benchmark song made and opened, its time and allocations, each track "
           "read once as it opens"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::TestSong s;
    const auto readBefore = phazite::countTracksReadAsValues();
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto allocations = phazite::test::countAllocations ([&s] { phazite::test::buildBenchmarkSong (s, 200, 16.0); });
    const auto made = juce::Time::getMillisecondCounterHiRes() - start;
    const auto readMaking = phazite::countTracksReadAsValues() - readBefore;

    auto& song = *phazite::findSong (s.context);
    const auto text = writeText (phazite::test::copyWholeSong (song));
    const auto tracks = phazite::getTrackIDs (song).size();

    const auto readOpening = phazite::countTracksReadAsValues();
    const auto openStart = juce::Time::getMillisecondCounterHiRes();
    phazite::Song::Ptr opened;
    const auto openAllocations = phazite::test::countAllocations ([&opened, &text] { opened = phazite::makeSong (parse (text)); });
    const auto openTime = juce::Time::getMillisecondCounterHiRes() - openStart;
    const auto readOpened = phazite::countTracksReadAsValues() - readOpening;
    REQUIRE (opened != nullptr);

    MESSAGE ("200-track song: " << tracks << " tracks; made in " << made << " ms, " << allocations.value_or (-1)
                                << " allocations, tracks read whole " << readMaking << "; opened in " << openTime << " ms, "
                                << openAllocations.value_or (-1) << " allocations, tracks read whole " << readOpened);

    // Each track and the master once, as the keeper takes them; none by the loader's rule (a song Phazite wrote).
    CHECK (readOpened == tracks + 1);
    CHECK (readMaking >= tracks);
    phazite::test::checkSong (*opened);
}
