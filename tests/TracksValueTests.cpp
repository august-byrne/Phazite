// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tracks as values: a track's fields written in the undo history
// alone (src/model/base/internal/TracksValue.h: TrackValue, SongTracks, TrackOrder), its switches written in the history and
// outside it alike, keyed (TrackSwitchesValue.h), and its view, outside the history alone (TracksOutside.h); their
// conversion from and to TRACK and MASTER elements; the loader's rules (keepTrackAsHeld(), keepSwitchesAsHeld(),
// keepViewAsHeld()); and the readers by ID that read the song's tree through them (model/arrangement/internal/Tracks.h:
// findTrack(), getTracksValue(), getTrackOrder(); the area's own getters). The golden project's tracks, damaged ones and the
// benchmark song's 200, read the same both ways once the song has opened and written back as the file holds them; the
// lenient reader against the getters it replaced, for what a file can hold; the helpers; the switches' changes and the
// sequences of design J.1 on a keyed area; and the gates: a playlist's rows read each track whole at most once, a paint, a
// tick, a mute's click and a rename none, and the loader's rules allocate nothing for a song Phazite wrote.
//   phazite_tests.exe --test-case="Tracks' values*"
#include "BenchmarkSong.h"
#include "GoldenFiles.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongState.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/ValueArea.h"
#include "model/base/internal/Visits.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::ChangeDirection;
using phazite::DeviceID;
using phazite::SongTracks;
using phazite::TrackID;
using phazite::TrackSwitches;
using phazite::TrackValue;
using Kind = TrackValue::Kind;

static_assert (phazite::KeyedAreaValue<TrackSwitches>);
static_assert (phazite::detail::GivesKeyedChanges<TrackSwitches>);
static_assert (! phazite::KeyedAreaValue<SongTracks>);

// A lookup kept to the statement that asked for it (Tracks.h: TrackLookup): never copied nor moved, and what it holds
// reached through a named one alone.
static_assert (! std::is_copy_constructible_v<phazite::TrackLookup>);
static_assert (! std::is_move_constructible_v<phazite::TrackLookup>);
static_assert (! std::is_copy_assignable_v<phazite::TrackLookup>);

template <typename Lookup>
concept ReachesThrough = requires (Lookup&& lookup) { std::forward<Lookup> (lookup).operator->(); };

static_assert (ReachesThrough<const phazite::TrackLookup&>);
static_assert (! ReachesThrough<phazite::TrackLookup>); // a call's lookup: `findTrack (...)->name` doesn't compile

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

// Each TRACK of a song's tree in the song's order (each folder's before those in it), then the MASTER.
std::vector<juce::ValueTree> findTrackElements (const juce::ValueTree& song)
{
    std::vector<juce::ValueTree> found;
    const std::function<void (const juce::ValueTree&)> visit = [&found, &visit] (const juce::ValueTree& parent)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
            {
                found.push_back (child);
                visit (child);
            }
        }
    };

    visit (song.getChildWithName (schema::TRACKS));

    if (const auto master = song.getChildWithName (schema::MASTER); master.isValid())
        found.push_back (master);

    return found;
}

// Every track's and the master's switches and view of a tree as it is written, no loader between: each element's read as
// Phazite's code read them (TrackSwitchesValue.h: readSwitchesOf(); TracksOutside.h: readViewOf()).
std::pair<phazite::TrackSwitches, phazite::TracksOutside> readSwitchesAndViewAsWritten (const juce::ValueTree& song)
{
    phazite::TrackSwitches switches;
    phazite::TracksOutside outside;

    for (const auto& element : findTrackElements (song))
    {
        phazite::readSwitchesOf (element, switches);

        if (element.hasType (schema::MASTER))
            outside.master = phazite::readViewOf (element);
        else
            outside.tracks = outside.tracks.set (phazite::readID (element[schema::trackID]), phazite::readViewOf (element));
    }

    return { switches, outside };
}

// Whether a property of an element is one of the tracks' three values (their conversions read and write it).
bool isCovered (const juce::ValueTree& element, const juce::Identifier& property)
{
    static const std::vector<const schema::PropertyDescription*> properties {
        &schema::trackID,        &schema::trackKind,   &schema::trackName,       &schema::trackColour,         &schema::folderBus,
        &schema::channelVolume,  &schema::channelPan,  &schema::channelFader,    &schema::channelVCA,          &schema::keyRoot,
        &schema::keyScale,       &schema::trackInput,  &schema::trackMonitoring, &schema::trackPadDevice,      &schema::trackPad,
        &schema::trackMute,      &schema::trackSolo,   &schema::trackFrozen,     &schema::trackAutomationMode, &schema::trackHeight,
        &schema::trackCollapsed, &schema::trackPinned, &schema::drumMode,        &schema::drumOrder,           &schema::drumHidden
    };

    for (const auto* each : properties)
        if (each->getName() == property && each->isOn (element.getType()))
            return true;

    return false;
}

// A LANE's properties, as text.
juce::StringPairArray describeLane (const juce::ValueTree& lane)
{
    juce::StringPairArray described;

    for (int i = 0; i < lane.getNumProperties(); ++i)
        described.set (lane.getPropertyName (i).toString(), phazite::writeValue (lane[lane.getPropertyName (i)]));

    return described;
}

// A track's or the master's element of a song that went through the loader: read both ways the same (its value, its
// switches, its view), written back from the three values into an element of its type with the same properties as the same
// text and the same lanes (an empty LANES none), the same read back from that, and each of the loader's rules leaving it as
// it is.
void checkRoundTrip (const juce::ValueTree& element)
{
    INFO (element.toXmlString().toStdString());
    const auto lenient = phazite::readTrackValue (element);
    const auto held = phazite::readHeldTrack (element);
    REQUIRE (lenient.has_value());
    REQUIRE (held.has_value());
    CHECK (*lenient == *held);

    TrackSwitches lenientSwitches, heldSwitches;
    phazite::readSwitchesOf (element, lenientSwitches);
    phazite::readHeldSwitchesOf (element, heldSwitches);
    CHECK (lenientSwitches == heldSwitches);

    const auto view = phazite::readViewOf (element);
    const auto heldView = phazite::readHeldViewOf (element);
    CHECK (view == heldView);

    auto written = phazite::writeTrackElement (*held);
    phazite::writeSwitchesInto (written, heldSwitches);
    phazite::writeViewInto (written, heldView);

    for (int i = 0; i < element.getNumProperties(); ++i)
    {
        const auto property = element.getPropertyName (i);
        INFO (property.toString().toStdString());

        if (isCovered (element, property))
            CHECK ((written.hasProperty (property) && phazite::isSameValue (written[property], element[property])));
    }

    for (int i = 0; i < written.getNumProperties(); ++i)
        CHECK (element.hasProperty (written.getPropertyName (i)));

    // Its lanes, each the same, in its order; an empty LANES as none.
    const auto lanes = element.getChildWithName (schema::LANES), writtenLanes = written.getChildWithName (schema::LANES);
    REQUIRE (writtenLanes.isValid() == (lanes.getNumChildren() > 0));

    for (int i = 0; i < lanes.getNumChildren(); ++i)
        CHECK (describeLane (lanes.getChild (i)) == describeLane (writtenLanes.getChild (i)));

    // Read back from what was written, the same (but where it is, which an element alone doesn't say).
    auto again = phazite::readHeldTrack (written);
    REQUIRE (again.has_value());
    again->parent = held->parent;
    CHECK (*again == *held);

    // The loader's rules leave it as it is.
    auto copy = element.createCopy();
    CHECK_FALSE (phazite::keepTrackAsHeld (copy));
    CHECK_FALSE (phazite::keepSwitchesAsHeld (copy));
    CHECK_FALSE (phazite::keepViewAsHeld (copy));
    CHECK (copy.isEquivalentTo (element));
}

// The readers by ID read what the conversion reads of each track and the master.
void checkReaders (const phazite::Song& song)
{
    const auto tracks = phazite::getTracksValue (song);
    const auto read = phazite::readSongTree (phazite::test::copyWholeSong (song)); // the switches and the view as a file holds them
    const auto& switches = read.trackSwitches;
    const auto& outside = read.tracksOutside;
    const auto order = phazite::makeTrackOrder (tracks);

    for (const auto id : order.order)
    {
        INFO (id.getRaw());
        const auto* value = tracks.find (id);
        REQUIRE (value != nullptr);
        const auto row = order.getPlace (id).index;

        if (const auto track = phazite::findTrack (song, id))
            CHECK (*track == *value);
        else
            FAIL ("findTrack() finds no track the value holds");

        CHECK (phazite::hasTrack (song, id));
        CHECK (phazite::isFolder (song, id) == value->isFolder());
        CHECK (phazite::isReturnTrack (song, id) == value->isReturn());
        CHECK (phazite::getParentFolder (song, id) == value->parent);
        CHECK (juce::exactlyEqual (phazite::getChannelVolume (song, id), value->volume));
        CHECK (juce::exactlyEqual (phazite::getChannelPan (song, id), value->pan));
        CHECK (phazite::getFader (song, id) == value->getVolumeDevice());
        CHECK (phazite::isBus (song, id) == value->isBus());
        CHECK (phazite::getTrackColour (song, id, row) == phazite::getTrackColour (value->colour, row));
        CHECK (phazite::getTrackKey (song, id) == value->key);
        CHECK (phazite::getTrackName (song, id)
               == (value->getOwnName().isNotEmpty() || value->isFolder() ? value->getOwnName()
                                                                         : "Track " + juce::String (order.getPlace (id).number)));
        CHECK (phazite::isTrackMuted (song, id, false) == (switches.mutes.find (id.getRaw()) != nullptr));
        CHECK (phazite::isTrackSoloed (song, id) == (switches.solos.find (id.getRaw()) != nullptr));

        // A row the song holds no view of (its file writes none) reads as the default view.
        const auto* held = outside.tracks.find (id.getRaw());
        const auto view = held != nullptr ? *held : phazite::TrackView {};
        CHECK (phazite::getSavedTrackHeight (song, id) == view.height);
        CHECK (phazite::isCollapsed (song, id) == view.collapsed.value_or (false));
        CHECK (phazite::isPinned (song, id) == view.pinned.value_or (false));
    }

    const auto master = phazite::getMasterID (song);
    CHECK (master == tracks.master.id);
    CHECK (phazite::isMaster (song, master));
    CHECK (juce::exactlyEqual (phazite::getChannelVolume (song, master), tracks.master.volume));
    CHECK (juce::exactlyEqual (phazite::getChannelPan (song, master), tracks.master.pan));
    CHECK (phazite::getFader (song, master) == tracks.master.getVolumeDevice());
    CHECK (phazite::getTrackName (song, master) == "Master");

    if (const auto track = phazite::findTrack (song, master))
        CHECK (*track == tracks.master);
    else
        FAIL ("findTrack() finds no master");

    // The song's order as the index keeps it, the same as the value's.
    const auto indexed = phazite::getTrackOrder (song);
    CHECK (indexed.order == order.order);
    CHECK (indexed.places == order.places);
    CHECK (phazite::getTrackIDs (song) == order.order);
}

TrackValue makeTrack (std::uint64_t id, Kind kind, std::uint64_t parent = 0)
{
    TrackValue track;
    track.id = TrackID::fromRaw (id);
    track.kind = kind;
    track.parent = TrackID::fromRaw (parent);
    track.fader = DeviceID::fromRaw (1000 + id);
    return track;
}

juce::String describeOrder (const SongTracks& tracks)
{
    juce::StringArray words;

    for (const auto id : phazite::makeTrackOrder (tracks).order)
        words.add (juce::String ((juce::int64) id.getRaw()));

    return words.joinIntoString (" ");
}

// The switches as text, each map by its keys in order: "mutes 1 | solos 2 | frozen | modes 0=touch 3=latch".
juce::String describe (const TrackSwitches& switches)
{
    const auto keys = [] (const immer::map<std::uint64_t, bool>& map)
    {
        std::map<std::uint64_t, bool> sorted (map.begin(), map.end());
        juce::StringArray words;

        for (const auto& [key, on] : sorted)
            words.add (juce::String ((juce::int64) key));

        return words.joinIntoString (" ");
    };

    std::map<std::uint64_t, phazite::SavedAutomationMode> modes (switches.modes.begin(), switches.modes.end());
    juce::StringArray words;

    for (const auto& [key, mode] : modes)
        words.add (juce::String ((juce::int64) key) + "="
                   + (mode == phazite::SavedAutomationMode::touch   ? "touch"
                      : mode == phazite::SavedAutomationMode::latch ? "latch"
                                                                    : "write"));

    return ("mutes " + keys (switches.mutes) + " | solos " + keys (switches.solos) + " | frozen " + keys (switches.frozen) + " | modes "
            + words.joinIntoString (" "))
        .replace ("  ", " ")
        .trim();
}

TrackSwitches withSwitch (TrackSwitches switches, int map, std::uint64_t key, bool on)
{
    auto& switched = map == 0 ? switches.mutes : map == 1 ? switches.solos : switches.frozen;
    switched = on ? switched.set (key, true) : switched.erase (key);
    return switches;
}

TrackSwitches withMode (TrackSwitches switches, std::uint64_t key, std::optional<phazite::SavedAutomationMode> mode)
{
    switches.modes = mode.has_value() ? switches.modes.set (key, *mode) : switches.modes.erase (key);
    return switches;
}

constexpr int mutes = 0, solos = 1, frozen = 2;

// The switches' keyed area on an undo manager of its own, as a song's history holds an area: the changes a test makes, in a
// step (set()) or outside the history.
struct SwitchesHistory
{
    juce::String now() const { return describe (area.get()); }

    void inStep (const juce::String& name, const std::function<TrackSwitches (TrackSwitches)>& change)
    {
        history.beginNewTransaction (name);
        area.set (change (area.get()));
    }

    void outside (const std::function<TrackSwitches (TrackSwitches)>& change) { area.setOutsideHistory (change (area.get())); }

    juce::UndoManager history;
    phazite::ValueArea<TrackSwitches> area { history, TrackSwitches() };
};

struct Counted
{
    std::uint64_t visits = 0, whole = 0;
    int allocations = -1;
};

// A song's file whose tracks and master hold what Phazite doesn't write, as another program could: of its first track, a
// volume in more digits than its float, a pan of 0, a colour in capitals, a root without a scale, a switch written off, an
// empty input, monitoring of no word, an empty name and drum order, an element Phazite doesn't know; of a folder, numbers off
// their ranges, a colour that is no colour, a property Phazite doesn't know; of the track in it, a volume of 0, a scale of
// no word, a frozen switch written off, a height in more digits; of the master, its default volume, a write mode of no word,
// a lane's switch written off; and a track with a scale and no root.
struct DamagedFile
{
    juce::ValueTree file {};
    std::vector<TrackID> tracks {};
    TrackID folder {}, inside {};
};

DamagedFile makeDamagedFile()
{
    auto made = phazite::createSong();
    DamagedFile damaged;
    damaged.tracks = phazite::ensureTracks (*made, 2);
    const auto& tracks = damaged.tracks;
    const auto folder = damaged.folder = phazite::groupIntoFolder (*made, { tracks[1] }, nullptr);
    const auto inside = damaged.inside = tracks[1];
    const auto master = phazite::getMasterID (*made);
    const auto masterFader = phazite::getFader (*made, master);
    phazite::showLane (*made, master, { .device = masterFader, .parameter = schema::masterVolume });

    auto file = damaged.file = phazite::test::copyWholeSong (*made);
    auto a = phazite::test::findTrackIn (file, tracks[0]);
    a.setProperty (schema::channelVolume, "-6.0000001", nullptr);
    a.setProperty (schema::channelPan, "0", nullptr);
    a.setProperty (schema::trackColour, "FF00FF00", nullptr);
    a.setProperty (schema::keyRoot, "3", nullptr);
    a.setProperty (schema::trackMute, "0", nullptr);
    a.setProperty (schema::trackInput, "", nullptr);
    a.setProperty (schema::trackMonitoring, "sometimes", nullptr);
    a.setProperty (schema::folderBus, "0", nullptr);
    a.setProperty (schema::trackName, "", nullptr);
    a.setProperty (schema::drumOrder, "", nullptr);
    a.appendChild (juce::ValueTree ("FUTURE"), nullptr);

    auto f = phazite::test::findTrackIn (file, folder);
    f.setProperty (schema::channelVolume, "-200", nullptr);
    f.setProperty (schema::channelPan, "2", nullptr);
    f.setProperty (schema::trackColour, "red", nullptr);
    f.setProperty ("shade", "deep", nullptr); // a property Phazite doesn't know

    auto c = phazite::test::findTrackIn (file, inside);
    c.setProperty (schema::channelVolume, "0", nullptr);
    c.setProperty (schema::keyRoot, "5", nullptr);
    c.setProperty (schema::keyScale, "bebop", nullptr);
    c.setProperty (schema::trackFrozen, "0", nullptr);
    c.setProperty (schema::trackHeight, "64.0", nullptr);

    auto m = file.getChildWithName (schema::MASTER);
    m.setProperty (schema::channelVolume, "-3", nullptr);
    m.setProperty (schema::trackAutomationMode, "skip", nullptr);
    auto lane = m.getChildWithName (schema::LANES).getChild (0);
    lane.setProperty (schema::laneCollapsed, "0", nullptr);
    lane.setProperty (schema::laneHeight, "48", nullptr);

    // A second track with a scale and no root.
    auto b = phazite::test::findTrackIn (file, tracks[0]).createCopy();
    b.removeAllChildren (nullptr);
    b.removeAllProperties (nullptr);
    b.setProperty (schema::trackID, 990001, nullptr);
    b.setProperty (schema::trackKind, "track", nullptr);
    b.setProperty (schema::channelFader, 990002, nullptr);
    b.setProperty (schema::keyScale, "minor", nullptr);
    file.getChildWithName (schema::TRACKS).appendChild (b, nullptr);
    return damaged;
}

// A song's file whose MASTER holds what only a track has: a kind, a name, a colour, a key, an input, monitoring, a pad, the
// switches of a track, pinned and drum rows; and a property Phazite doesn't know.
juce::ValueTree makeMasterFile()
{
    auto made = phazite::createSong();
    REQUIRE_FALSE (phazite::ensureTracks (*made, 1).empty());
    auto file = phazite::test::copyWholeSong (*made);
    auto m = file.getChildWithName (schema::MASTER);
    m.setProperty (schema::trackKind, "folder", nullptr);
    m.setProperty (schema::trackName, "Main", nullptr);
    m.setProperty (schema::trackColour, "ff00ff00", nullptr);
    m.setProperty (schema::keyRoot, 3, nullptr);
    m.setProperty (schema::keyScale, "minor", nullptr);
    m.setProperty (schema::trackInput, "1+2", nullptr);
    m.setProperty (schema::trackMonitoring, "always", nullptr);
    m.setProperty (schema::trackPad, 36, nullptr);
    m.setProperty (schema::trackMute, true, nullptr);
    m.setProperty (schema::trackSolo, true, nullptr);
    m.setProperty (schema::trackPinned, true, nullptr);
    m.setProperty (schema::drumMode, true, nullptr);
    m.setProperty (schema::drumOrder, "38,36", nullptr);
    m.setProperty (schema::drumHidden, "42", nullptr);
    m.setProperty ("shade", "deep", nullptr);
    return file;
}

// A song just opened: consistent, its tracks' round trip holding (tests/SongCheck.cpp: checkSong()), and a versions' pass
// against a version saved from it at once (its text, read back) making no undo step.
void checkOpenedAsVersion (phazite::Song& song)
{
    phazite::test::checkSong (song);

    for (const auto& element : findTrackElements (phazite::test::copyWholeSong (song)))
        checkRoundTrip (element);

    const auto version = parse (writeText (phazite::test::copyWholeSong (song)));
    song.getUndoManager().clearUndoHistory();

    {
        const phazite::UndoStep step (song, "Make Song Like");
        phazite::makeSongLike (song, version);
    }

    CHECK_FALSE (song.getUndoManager().canUndo());
}

// What a piece of work looks at (Visits.h), reads of its tracks whole, and allocates (the second time it runs).
Counted count (const std::function<void()>& work)
{
    Counted counted;
    const auto visits = phazite::countElementsVisited();
    const auto whole = phazite::countTracksReadAsValues();
    work();
    counted.visits = phazite::countElementsVisited() - visits;
    counted.whole = phazite::countTracksReadAsValues() - whole;
    counted.allocations = phazite::test::countAllocations (work).value_or (-1);
    return counted;
}
} // namespace

//==============================================================================
TEST_CASE ("Tracks' values: the golden project's tracks and master read the same both ways, written back as the file holds them")
{
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto song = phazite::makeSong (juce::ValueTree::fromXml (*xml));
    REQUIRE (song != nullptr);
    const auto whole = phazite::test::copyWholeSong (*song);
    const auto elements = findTrackElements (whole);
    CHECK (elements.size() == 13);

    for (const auto& element : elements)
        checkRoundTrip (element);

    const auto tracks = phazite::getTracksValue (*song);
    CHECK (phazite::holdsItsLists (tracks));
    CHECK (tracks.tracks.size() == 12);
    CHECK (tracks == phazite::readTracksValue (whole));

    // What the file holds of the master and a track or two.
    CHECK (tracks.master.id.getRaw() == 1);
    CHECK (tracks.master.fader.getRaw() == 2);
    CHECK (juce::exactlyEqual (tracks.master.volume, -6.0f));
    CHECK (juce::exactlyEqual (tracks.master.pan, 0.125f));
    REQUIRE (tracks.find (TrackID::fromRaw (91)) != nullptr);
    const auto& snare = *tracks.find (TrackID::fromRaw (91));
    CHECK (snare.padDevice.getRaw() == 90);
    CHECK (snare.pad == std::optional (38));
    CHECK (snare.key == std::optional (phazite::Key { .root = 2, .scale = phazite::Scale::minor }));
    CHECK (snare.colour == std::optional<juce::uint32> (0xff56b4e9));

    const auto read = phazite::readSongTree (whole);
    const auto& switches = read.trackSwitches;
    CHECK (switches.mutes.find (3) != nullptr);
    CHECK (*switches.modes.find (0) == phazite::SavedAutomationMode::touch);

    const auto& outside = read.tracksOutside;
    CHECK (outside.master.lanes.size() == 2);
    CHECK (outside.master.collapsed == std::optional (true));
    REQUIRE (outside.tracks.find (3) != nullptr);
    CHECK (outside.tracks.find (3)->drumOrder == std::optional<juce::String> ("38,36,37,39,42,46,41,45,48,49,51"));
    CHECK (outside.tracks.find (3)->pinned == std::optional (true));

    // Its IDs: the largest the tracks hold, each held, each pad's device named.
    std::uint64_t named = 0;
    phazite::forEachIDNamed (tracks, [&named] (std::uint64_t id) { named = id; });
    CHECK (named == 90);
    auto held = 0;
    phazite::forEachIDHeld (tracks, [&held] (std::uint64_t) { ++held; });
    CHECK (held >= 26); // each track's ID and its fader or VCA, and the master's two
    CHECK (phazite::findLargestIDIn (tracks) > 0);
    std::uint64_t lanes = 0;
    phazite::forEachIDNamed (outside, [&lanes] (std::uint64_t) { ++lanes; });
    CHECK (lanes >= 4);

    checkReaders (*song);
}

TEST_CASE ("Tracks' values: the benchmark song's 200 tracks, which Phazite's own writers wrote, read the same both ways and need no "
           "change, and the value's order is the index's")
{
    phazite::test::TestSong s;
    phazite::test::buildBenchmarkSong (s, 200, 16.0);
    auto& song = *phazite::findSong (s.context);

    // A track of each kind of view and switch set by the model's own writers too.
    const auto tracks = phazite::getTrackIDs (song);
    REQUIRE (tracks.size() > 200);
    phazite::setSavedTrackHeight (song, tracks[1], 96);
    phazite::setCollapsed (song, tracks[2], false);
    phazite::setPinned (song, tracks[3], true);
    phazite::setTrackMuted (song, tracks[4], true);
    phazite::setTrackSoloed (song, tracks[5], true);
    phazite::renameTrack (song, tracks[6], "");
    phazite::setDrumRowOrder (song, tracks[7], {});
    phazite::setAutomationMode (song, tracks[8], phazite::AutomationMode::latch);
    phazite::setTrackKey (song, tracks[9], phazite::Key { .root = 4, .scale = phazite::Scale::dorian });
    phazite::setChannelVolume (song, phazite::getMasterID (song), 0.0f);

    const auto whole = phazite::test::copyWholeSong (song);

    for (const auto& element : findTrackElements (whole))
        checkRoundTrip (element);

    const auto value = phazite::getTracksValue (song);
    CHECK (phazite::holdsItsLists (value));
    checkReaders (song);

    // And so opened from its text, the same text again.
    const auto text = writeText (whole);
    const auto opened = phazite::makeSong (parse (text));
    REQUIRE (opened != nullptr);
    CHECK (writeText (phazite::test::copyWholeSong (*opened)) == text);
}

TEST_CASE ("Tracks' values: the lenient reader reads what the getters it replaced read, for what a file can hold")
{
    // A song's tree as another program could write it, read as it is (no loader): a number as text, out of its range, a scale
    // and a kind of no word of theirs, a colour that is no colour and one with a sign before it, a folder with a fader, a track
    // with a VCA, a MASTER in a folder, and the master's view of a track's.
    const auto tree = parse (R"(<PHAZITE nextID="100"><TRACKS>)"
                             R"(<TRACK id="10" kind="track" volume="loud" pan="-3" colour="not a colour" keyRoot="14" keyScale="bebop")"
                             R"( fader="11" vca="12" mute="1" input="1+2" monitoring="always" pad="61.9" padDevice="abc"/>)"
                             R"(<TRACK id="20" kind="folder" fader="21" pan="0.5" collapsed="1">)"
                             R"(<TRACK id="30" kind="return" volume="7.5" fader="31" solo="1" name="" height="500"/>)"
                             R"(<MASTER id="40" fader="41"/></TRACK>)"
                             R"(<TRACK id="50" kind="whatever" fader="51" colour="#FF336699" automationMode="skip"/>)"
                             R"(</TRACKS><MASTER id="1" fader="2" volume="0" automationMode="latch" pinned="1" drumMode="1"/></PHAZITE>)");

    // The getters read a song's values: a song given, outside the history, what the lenient readers read of the tree as it is
    // (TracksValue.h: readTracksValue(); readSwitchesAndViewAsWritten(), above).
    const auto song = phazite::createSong();
    const auto [switchesWritten, viewWritten] = readSwitchesAndViewAsWritten (tree);

    {
        const phazite::Song::OutsideHistory outside (*song);
        auto& values = *song->getValues();
        values.setAllTracks (*song, phazite::readTracksValue (tree));
        values.setSwitchesOutsideHistory (*song, switchesWritten);
        values.setTracksOutside (viewWritten);
    }

    const auto id = [] (std::uint64_t raw) { return TrackID::fromRaw (raw); };
    const auto& palette = phazite::getTrackColours();

    // As Phazite's getters always read them (each expected value worked out from the code before the tracks' values).
    CHECK (juce::exactlyEqual (phazite::getChannelVolume (*song, id (10)), 0.0f)); // a text's leading number: none
    CHECK (juce::exactlyEqual (phazite::getChannelPan (*song, id (10)), -3.0f));   // as it is, out of range
    CHECK (phazite::getTrackColour (*song, id (10), 0) == palette[0].colour);      // no colour: its row's
    CHECK (phazite::getTrackKey (*song, id (10)) == std::optional (phazite::Key { .root = 11, .scale = phazite::Scale::major }));
    CHECK (phazite::getFader (*song, id (10)).getRaw() == 12); // its VCA
    CHECK (phazite::getVolumeParameterID (*song, id (10)) == schema::vcaVolume);
    CHECK (phazite::getPanParameterID (*song, id (10)).isEmpty());
    CHECK_FALSE (phazite::isBus (*song, id (10)));
    CHECK (phazite::isTrackMuted (*song, id (10), false));
    CHECK (phazite::getTrackName (*song, id (10)) == "Track 1");

    CHECK (phazite::isBus (*song, id (20))); // a folder with a fader
    CHECK (phazite::getFader (*song, id (20)).getRaw() == 21);
    CHECK (phazite::getVolumeParameterID (*song, id (20)) == schema::faderVolume);
    CHECK (phazite::getPanParameterID (*song, id (20)) == schema::faderPan);
    CHECK (juce::exactlyEqual (phazite::getChannelPan (*song, id (20)), 0.5f));
    CHECK (phazite::isCollapsed (*song, id (20)));
    CHECK (phazite::getTracksInFolder (*song, id (20)) == std::vector { id (30) });

    CHECK (juce::exactlyEqual (phazite::getChannelVolume (*song, id (30)), 7.5f)); // as it is, out of range
    CHECK (phazite::isReturnTrack (*song, id (30)));
    CHECK (phazite::isTrackSoloed (*song, id (30)));
    CHECK (phazite::getTrackName (*song, id (30)) == "Track 2"); // an empty name: its number
    CHECK (phazite::getSavedTrackHeight (*song, id (30)) == std::optional (500));
    CHECK (phazite::getParentFolder (*song, id (30)) == id (20));
    CHECK (phazite::isHiddenInCollapsedFolder (*song, id (30)));
    CHECK_FALSE (phazite::isSilencedBySolo (*song, id (10))); // muted
    CHECK (phazite::isSilencedBySolo (*song, id (50)));
    CHECK_FALSE (phazite::isSilencedBySolo (*song, id (30)));
    CHECK (phazite::areAnyTracksSoloed (*song));

    CHECK_FALSE (phazite::isFolder (*song, id (50))); // a kind of no word: a track
    CHECK (phazite::getTrackColour (*song, id (50), 3) == juce::Colour (0xff336699));
    CHECK (phazite::getAutomationMode (*song, id (50)) == phazite::AutomationMode::read);

    const auto master = phazite::getMasterID (*song);
    CHECK (master == id (1));
    CHECK (juce::exactlyEqual (phazite::getChannelVolume (*song, master), 0.0f));
    CHECK (phazite::getAutomationMode (*song, master) == phazite::AutomationMode::latch);
    CHECK (phazite::isPinned (*song, master)); // as written, though no master is pinned
    CHECK_FALSE (phazite::hasTrack (*song, id (40)));

    // The lenient reader reads the same.
    const auto tracks = phazite::readTracksValue (tree);
    REQUIRE (tracks.find (id (10)) != nullptr);
    const auto& ten = *tracks.find (id (10));
    CHECK (juce::exactlyEqual (ten.volume, 0.0f));
    CHECK (juce::exactlyEqual (ten.pan, -3.0f));
    CHECK_FALSE (ten.colour.has_value());
    CHECK (ten.key == std::optional (phazite::Key { .root = 11, .scale = phazite::Scale::major }));
    CHECK (ten.getVolumeDevice().getRaw() == 12);
    CHECK (ten.input == "1+2");
    CHECK (ten.monitoring == TrackValue::Monitoring::always);
    CHECK (ten.pad == std::optional (61));
    CHECK_FALSE (ten.padDevice.isValid());
    CHECK (tracks.find (id (20))->isBus());
    CHECK (juce::exactlyEqual (tracks.find (id (30))->volume, 7.5f));
    CHECK (tracks.find (id (30))->name == std::optional<juce::String> (""));
    CHECK (tracks.find (id (30))->parent == id (20));
    CHECK (tracks.find (id (50))->kind == Kind::track);
    CHECK (tracks.find (id (50))->colour == std::optional<juce::uint32> (0xff336699));
    CHECK (juce::exactlyEqual (tracks.master.volume, 0.0f));
    CHECK (tracks.find (id (40)) == nullptr);
    CHECK (describeOrder (tracks) == "10 20 30 50");

    // A MASTER in a folder: the master, in no folder.
    const auto misplaced = tree.getChildWithName (schema::TRACKS).getChild (1).getChildWithName (schema::MASTER);
    const auto read = phazite::readTrackValue (misplaced);
    REQUIRE (read.has_value());
    CHECK (read->isMaster());
    CHECK_FALSE (read->parent.isValid());
    CHECK (juce::exactlyEqual (read->volume, -3.0f));

    // As the song holds them: within their ranges, by their words.
    const auto held = phazite::readHeldTrack (tree.getChildWithName (schema::TRACKS).getChild (0));
    REQUIRE (held.has_value());
    CHECK (juce::exactlyEqual (held->volume, 0.0f)); // no number: its default
    CHECK (juce::exactlyEqual (held->pan, -1.0f));
    CHECK (held->key == std::optional (phazite::Key { .root = 11, .scale = phazite::Scale::major }));
    CHECK (held->pad == std::optional (62));
    CHECK (phazite::readHeldTrack (tree.getChildWithName (schema::TRACKS).getChild (2))->kind == Kind::track);
    CHECK (juce::exactlyEqual (phazite::readHeldTrack (tree.getChildWithName (schema::TRACKS).getChild (1).getChild (0))->volume, 6.0f));
    CHECK_FALSE (phazite::readHeldTrack (parse (R"(<TRACK kind="track" fader="3"/>)")).has_value()); // no ID

    // The switches and the view as the getters read them.
    const auto& switches = switchesWritten;
    CHECK (describe (switches) == "mutes 10 | solos 30 | frozen | modes 0=latch");
    const auto& outside = viewWritten;
    CHECK (outside.master.pinned == std::optional (true));
    CHECK (outside.master.drumMode == std::optional (true));
    CHECK_FALSE (phazite::readHeldViewOf (tree.getChildWithName (schema::MASTER)).pinned.has_value()); // not described there
    CHECK (outside.tracks.find (30)->height == std::optional (500));
    CHECK (phazite::readHeldViewOf (tree.getChildWithName (schema::TRACKS).getChild (1).getChild (0)).height == std::optional (320));
    CHECK (phazite::defaultMasterVolume == doctest::Approx (TrackValue::defaultVolume (Kind::master)));
}

TEST_CASE ("Tracks' values: the helpers keep each track in one list, a folder before its tracks, and refuse a folder into itself")
{
    SongTracks tracks;
    tracks = phazite::withTrack (tracks, makeTrack (1, Kind::track));
    tracks = phazite::withTrack (tracks, makeTrack (2, Kind::folder));
    tracks = phazite::withTrack (tracks, makeTrack (3, Kind::track, 2));
    tracks = phazite::withTrack (tracks, makeTrack (4, Kind::folder, 2));
    tracks = phazite::withTrack (tracks, makeTrack (5, Kind::track, 4));
    tracks = phazite::withTrack (tracks, makeTrack (6, Kind::returnTrack));
    CHECK (phazite::holdsItsLists (tracks));
    CHECK (describeOrder (tracks) == "1 2 3 4 5 6");

    const auto order = phazite::makeTrackOrder (tracks);
    CHECK (order.getPlace (TrackID::fromRaw (2)).number == 0);
    CHECK (order.getPlace (TrackID::fromRaw (3)).number == 2);
    CHECK (order.getPlace (TrackID::fromRaw (6)).number == 4);
    CHECK (order.getPlace (TrackID::fromRaw (5)).index == 4);
    CHECK (order.getPlace (TrackID::fromRaw (9)).index == -1);

    // Changed in place: its place kept. Added at a place.
    auto renamed = makeTrack (1, Kind::track);
    renamed.name = "One";
    CHECK (describeOrder (phazite::withTrack (tracks, renamed)) == "1 2 3 4 5 6");
    CHECK (phazite::withTrack (tracks, renamed).find (TrackID::fromRaw (1))->name == std::optional<juce::String> ("One"));
    CHECK (describeOrder (phazite::withTrack (tracks, makeTrack (7, Kind::track), 1)) == "1 7 2 3 4 5 6");
    CHECK (describeOrder (phazite::withTrack (tracks, makeTrack (7, Kind::track, 2), 0)) == "1 2 7 3 4 5 6");

    // Moved into another folder, with the tracks in it.
    const auto moved = phazite::withTrack (tracks, makeTrack (4, Kind::folder), 0);
    CHECK (describeOrder (moved) == "4 5 1 2 3 6");
    CHECK (phazite::holdsItsLists (moved));
    CHECK (moved.find (TrackID::fromRaw (5))->parent.getRaw() == 4);

    // Refused: a folder into itself, into a folder in it, into a track that is no folder, into one it doesn't hold; one
    // without an ID.
    CHECK (phazite::withTrack (tracks, makeTrack (2, Kind::folder, 2)).isSameAs (tracks));
    CHECK (phazite::withTrack (tracks, makeTrack (2, Kind::folder, 4)).isSameAs (tracks));
    CHECK (phazite::withTrack (tracks, makeTrack (5, Kind::track, 1)).isSameAs (tracks));
    CHECK (phazite::withTrack (tracks, makeTrack (5, Kind::track, 99)).isSameAs (tracks));
    CHECK (phazite::withTrack (tracks, makeTrack (0, Kind::track)).isSameAs (tracks));

    // Taken out with the tracks in it, their lists too.
    const auto without = phazite::withoutTrack (tracks, TrackID::fromRaw (2));
    CHECK (describeOrder (without) == "1 6");
    CHECK (without.tracks.size() == 2);
    CHECK (without.findList (TrackID::fromRaw (4)) == nullptr);
    CHECK (phazite::holdsItsLists (without));
    CHECK (phazite::withoutTrack (tracks, TrackID::fromRaw (99)).isSameAs (tracks));

    // A list in another order: the same IDs alone.
    CHECK (describeOrder (phazite::withListOrder (tracks, {}, { TrackID::fromRaw (6), TrackID::fromRaw (1), TrackID::fromRaw (2) }))
           == "6 1 2 3 4 5");
    CHECK (phazite::withListOrder (tracks, {}, { TrackID::fromRaw (6), TrackID::fromRaw (1) }).isSameAs (tracks));
    CHECK (phazite::withListOrder (tracks, {}, { TrackID::fromRaw (6), TrackID::fromRaw (1), TrackID::fromRaw (1) }).isSameAs (tracks));

    // The master: a member, by its ID, in no list.
    auto master = TrackValue::makeMaster();
    master.id = TrackID::fromRaw (100);
    master.parent = TrackID::fromRaw (2);
    const auto withMaster = phazite::withTrack (tracks, master);
    CHECK (withMaster.find (TrackID::fromRaw (100)) == &withMaster.master);
    CHECK_FALSE (withMaster.master.parent.isValid());
    CHECK (describeOrder (withMaster) == "1 2 3 4 5 6");
    CHECK (phazite::holdsItsLists (withMaster));

    // Refused, the value given back as it was (isSameAs()): a track of the master's ID; a folder made a track or a return
    // while it holds tracks. A folder that holds none may change its kind, and one that holds tracks stay a folder.
    CHECK (phazite::withTrack (withMaster, makeTrack (100, Kind::track)).isSameAs (withMaster));
    CHECK (phazite::withTrack (tracks, makeTrack (4, Kind::track, 2)).isSameAs (tracks));
    CHECK (phazite::withTrack (tracks, makeTrack (2, Kind::returnTrack)).isSameAs (tracks));
    CHECK_FALSE (phazite::withTrack (tracks, makeTrack (4, Kind::folder, 2)).isSameAs (tracks));
    auto emptied = phazite::withoutTrack (tracks, TrackID::fromRaw (5));
    const auto plain = phazite::withTrack (emptied, makeTrack (4, Kind::track, 2));
    CHECK_FALSE (plain.isSameAs (emptied));
    CHECK (plain.find (TrackID::fromRaw (4))->kind == Kind::track);
    CHECK (phazite::holdsItsLists (plain));

    // What holdsItsLists() finds: a track in two lists, the master in a list, a list of a track that is no folder, an empty
    // list, a track in a list its folder doesn't name, an ID twice among the tracks' and the faders', folders round in a ring.
    auto twice = tracks;
    twice.lists = twice.lists.set (4, twice.findList (TrackID::fromRaw (4))->push_back (TrackID::fromRaw (1)));
    CHECK_FALSE (phazite::holdsItsLists (twice));

    auto masterInList = withMaster;
    masterInList.lists = masterInList.lists.set (0, masterInList.findList ({})->push_back (TrackID::fromRaw (100)));
    CHECK_FALSE (phazite::holdsItsLists (masterInList));

    auto ofTrack = tracks;
    ofTrack.lists = ofTrack.lists.set (1, { TrackID::fromRaw (3) });
    CHECK_FALSE (phazite::holdsItsLists (ofTrack));

    auto empty = tracks;
    empty.lists = empty.lists.set (6, {});
    CHECK_FALSE (phazite::holdsItsLists (empty));

    auto elsewhere = tracks;
    auto three = *elsewhere.find (TrackID::fromRaw (3));
    three.parent = TrackID::fromRaw (4);
    elsewhere.tracks = elsewhere.tracks.set (3, three);
    CHECK_FALSE (phazite::holdsItsLists (elsewhere));

    auto sameFader = tracks;
    auto six = *sameFader.find (TrackID::fromRaw (6));
    six.fader = DeviceID::fromRaw (1001); // track 1's fader
    sameFader.tracks = sameFader.tracks.set (6, six);
    CHECK_FALSE (phazite::holdsItsLists (sameFader));

    auto ring = tracks;
    auto two = *ring.find (TrackID::fromRaw (2));
    two.parent = TrackID::fromRaw (4);
    ring.tracks = ring.tracks.set (2, two);
    ring.lists = ring.lists.set (0, { TrackID::fromRaw (1), TrackID::fromRaw (6) }).set (4, { TrackID::fromRaw (5), TrackID::fromRaw (2) });
    CHECK_FALSE (phazite::holdsItsLists (ring));
}

TEST_CASE ("Tracks' values: the switches' changes over every map, moved back and forward, onto switches that gained others")
{
    using Mode = phazite::SavedAutomationMode;
    TrackSwitches before;
    before = withSwitch (withSwitch (before, mutes, 1, true), mutes, 2, true);
    before = withSwitch (withSwitch (before, solos, 1, true), solos, 2, true);
    before = withSwitch (withSwitch (before, frozen, 1, true), frozen, 2, true);
    before = withMode (withMode (before, 0, Mode::touch), 2, Mode::latch);

    // In every map an entry added and one taken away; a mode changed too.
    auto after = withSwitch (withSwitch (before, mutes, 3, true), mutes, 2, false);
    after = withSwitch (withSwitch (after, solos, 3, true), solos, 1, false);
    after = withSwitch (withSwitch (after, frozen, 3, true), frozen, 2, false);
    after = withMode (withMode (withMode (after, 3, Mode::write), 0, Mode::latch), 2, std::nullopt);

    const auto changes = phazite::findChanges (before, after);
    CHECK (changes.mutes.size() == 2);
    CHECK (changes.solos.size() == 2);
    CHECK (changes.frozen.size() == 2);
    CHECK (changes.modes.size() == 3);

    CHECK (phazite::withChanges (before, changes, ChangeDirection::forward) == after);
    CHECK (phazite::withChanges (after, changes, ChangeDirection::back) == before);
    CHECK (phazite::withChanges (after, changes, ChangeDirection::forward).isSameAs (after));

    const auto gain = [] (TrackSwitches switches)
    { return withMode (withSwitch (withSwitch (withSwitch (switches, mutes, 9, true), solos, 9, true), frozen, 9, true), 9, Mode::touch); };

    CHECK (describe (phazite::withChanges (gain (after), changes, ChangeDirection::back)) == describe (gain (before)));
    CHECK (describe (phazite::withChanges (gain (before), changes, ChangeDirection::forward)) == describe (gain (after)));

    // Written as Phazite writes them, a track's by its ID, the master's mode by 0; read back the same.
    juce::ValueTree track (schema::TRACK, { { schema::trackID, 1 }, { schema::trackKind, "track" } });
    juce::ValueTree master (schema::MASTER, { { schema::trackID, 7 } });
    phazite::writeSwitchesInto (track, before);
    phazite::writeSwitchesInto (master, before);
    CHECK (track[schema::trackMute].toString() == "1");
    CHECK (track[schema::trackSolo].toString() == "1");
    CHECK (track[schema::trackFrozen].toString() == "1");
    CHECK_FALSE (track.hasProperty (schema::trackAutomationMode));
    CHECK (master[schema::trackAutomationMode].toString() == schema::automationTouch);
    TrackSwitches read;
    phazite::readHeldSwitchesOf (track, read);
    phazite::readHeldSwitchesOf (master, read);
    CHECK (describe (read) == "mutes 1 | solos 1 | frozen 1 | modes 0=touch");

    // A switch written off reads as none and is written as none; a mode of no word too.
    juce::ValueTree off (
        schema::TRACK,
        { { schema::trackID, 5 }, { schema::trackKind, "track" }, { schema::trackMute, "0" }, { schema::trackAutomationMode, "skip" } });
    CHECK_FALSE (phazite::readTrackMute (off));
    CHECK_FALSE (phazite::readSavedAutomationMode (off).has_value());
    CHECK (phazite::keepSwitchesAsHeld (off));
    CHECK (off.getNumProperties() == 2);
}

//==============================================================================
// The sequences of design J.1 on the keyed switches, each beside the tree's result today where a real song can show it.
TEST_CASE ("Tracks' values, J.1 a: the Undo of a step that mutes and freezes a track (Bounce in Place) puts both back, while a "
           "solo set outside the history since survives")
{
    SwitchesHistory switches;
    switches.inStep ("Bounce in Place", [] (TrackSwitches s) { return withSwitch (withSwitch (s, mutes, 1, true), frozen, 1, true); });
    switches.outside ([] (TrackSwitches s) { return withSwitch (s, solos, 2, true); });
    CHECK (switches.now() == "mutes 1 | solos 2 | frozen 1 | modes");
    switches.history.undo();
    CHECK (switches.now() == "mutes | solos 2 | frozen | modes");
    switches.history.redo();
    CHECK (switches.now() == "mutes 1 | solos 2 | frozen 1 | modes");

    // The tree today, on a real song: the mute in the step back by its Undo, the solo set by hand after it kept.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    song->getUndoManager().clearUndoHistory();

    {
        const phazite::UndoStep step (*song, "Bounce in Place");
        phazite::setTrackMuted (*song, tracks[0], true, &song->getUndoManager());
    }

    phazite::setTrackSoloed (*song, tracks[1], true);
    phazite::undo (*song);
    CHECK_FALSE (phazite::isTrackMuted (*song, tracks[0], false));
    CHECK (phazite::isTrackSoloed (*song, tracks[1]));
    phazite::redo (*song);
    CHECK (phazite::isTrackMuted (*song, tracks[0], false));
    CHECK (phazite::isTrackSoloed (*song, tracks[1]));
}

TEST_CASE ("Tracks' values, J.1 b: a switch set outside the history survives the Undo of an unrelated step, and is lost by the Undo "
           "and the Redo of a step that changed it")
{
    using Mode = phazite::SavedAutomationMode;
    SwitchesHistory switches;
    switches.outside ([] (TrackSwitches s) { return withSwitch (s, mutes, 1, true); });
    switches.inStep ("A Version Applied", [] (TrackSwitches s) { return withMode (s, 3, Mode::latch); });
    switches.outside ([] (TrackSwitches s) { return withSwitch (s, solos, 1, true); });
    switches.history.undo();
    CHECK (switches.now() == "mutes 1 | solos 1 | frozen | modes");
    switches.history.redo();
    CHECK (switches.now() == "mutes 1 | solos 1 | frozen | modes 3=latch");

    // A step that changed the mode of track 5, then the mode set by hand: its Undo gives back the mode before the step, its
    // Redo the step's, the one set by hand lost either way.
    switches.outside ([] (TrackSwitches s) { return withMode (s, 5, Mode::touch); });
    switches.inStep ("A Merge's Choice", [] (TrackSwitches s) { return withSwitch (withMode (s, 5, Mode::write), mutes, 5, true); });
    switches.outside ([] (TrackSwitches s) { return withSwitch (withMode (s, 5, Mode::latch), mutes, 5, false); });
    CHECK (switches.now() == "mutes 1 | solos 1 | frozen | modes 3=latch 5=latch");
    switches.history.undo();
    CHECK (switches.now() == "mutes 1 | solos 1 | frozen | modes 3=latch 5=touch");
    switches.history.redo();
    CHECK (switches.now() == "mutes 1 5 | solos 1 | frozen | modes 3=latch 5=write");

    // The tree today: a mode set by hand survives the Undo of a rename; a mute set in a step and taken off by hand after it
    // is on again after the step's Undo and Redo.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    song->getUndoManager().clearUndoHistory();
    phazite::setAutomationMode (*song, tracks[0], phazite::AutomationMode::touch);
    phazite::renameTrack (*song, tracks[1], "Renamed");
    phazite::undo (*song);
    CHECK (phazite::getAutomationMode (*song, tracks[0]) == phazite::AutomationMode::touch);

    {
        const phazite::UndoStep step (*song, "A Merge's Choice");
        phazite::setTrackMuted (*song, tracks[1], true, &song->getUndoManager());
    }

    phazite::setTrackMuted (*song, tracks[1], false);
    phazite::undo (*song);
    CHECK_FALSE (phazite::isTrackMuted (*song, tracks[1], false));
    phazite::redo (*song);
    CHECK (phazite::isTrackMuted (*song, tracks[1], false));
}

TEST_CASE ("Tracks' values, J.1 c: a version's switches applied in a step and undone, and redone")
{
    using Mode = phazite::SavedAutomationMode;
    SwitchesHistory switches;
    switches.outside ([] (TrackSwitches s)
                      { return withMode (withSwitch (withSwitch (s, mutes, 1, true), solos, 2, true), 0, Mode::touch); });
    const auto before = switches.area.get();

    switches.inStep ("Bring Back",
                     [] (TrackSwitches s)
                     {
                         s = withSwitch (withSwitch (withSwitch (s, mutes, 1, false), mutes, 3, true), solos, 2, false);
                         return withMode (withMode (withSwitch (s, frozen, 4, true), 0, Mode::write), 3, Mode::latch);
                     });

    const auto after = switches.area.get();
    CHECK (switches.now() == "mutes 3 | solos | frozen 4 | modes 0=write 3=latch");
    switches.history.undo();
    CHECK (switches.area.get().isSameAs (before)); // the very value, nothing written since
    switches.history.redo();
    CHECK (switches.area.get().isSameAs (after));
}

//==============================================================================
TEST_CASE ("Tracks' values: damaged tracks open as the song holds them, written as Phazite writes that, the same at every opening")
{
    // The loader makes each track and the master hold what the song holds of them (keepTrackAsHeld(), keepSwitchesAsHeld(),
    // keepViewAsHeld()), so that both readers read one song and what is written from the values is what the file holds. What
    // a file Phazite didn't write loses at its first save: a volume at its default (0 dB, the master's -3) and a pan of 0;
    // a number in more digits than its float; a colour written otherwise, as Phazite writes it, and one that is no colour; a
    // switch written off; an empty input; a scale of no word (major where it has a root), a scale without a root; a lane's
    // switch written off. What Phazite doesn't know on a TRACK or the MASTER goes, since the tracks' switch (roadmap step
    // S5.20: the song's values hold the tracks; it stayed before).
    const auto damaged = makeDamagedFile();
    const auto& file = damaged.file;
    const auto& tracks = damaged.tracks;
    const auto folder = damaged.folder, inside = damaged.inside;

    const auto opened = phazite::makeSong (parse (writeText (file)));
    REQUIRE (opened != nullptr);
    phazite::test::checkSong (*opened);
    const auto whole = phazite::test::copyWholeSong (*opened);

    const auto first = phazite::test::findTrackIn (whole, tracks[0]);
    CHECK (phazite::writeValue (first[schema::channelVolume]) == "-6");
    CHECK_FALSE (first.hasProperty (schema::channelPan));
    CHECK (first[schema::trackColour].toString() == "ff00ff00");
    CHECK (first[schema::keyScale].toString() == "major"); // a root without its scale
    CHECK_FALSE (first.hasProperty (schema::trackMute));
    CHECK_FALSE (first.hasProperty (schema::trackInput));
    CHECK_FALSE (first.hasProperty (schema::trackMonitoring));
    CHECK_FALSE (first.hasProperty (schema::folderBus));
    CHECK (first[schema::trackName].toString().isEmpty()); // an empty name, as renaming writes it, stays
    CHECK (first.hasProperty (schema::trackName));
    CHECK (first.hasProperty (schema::drumOrder));
    CHECK_FALSE (first.getChildWithName ("FUTURE").isValid()); // what Phazite doesn't know goes

    const auto group = phazite::test::findTrackIn (whole, folder);
    CHECK (phazite::writeValue (group[schema::channelVolume]) == "-100");
    CHECK (phazite::writeValue (group[schema::channelPan]) == "1");
    CHECK_FALSE (group.hasProperty (schema::trackColour));
    CHECK_FALSE (group.hasProperty ("shade"));

    const auto in = phazite::test::findTrackIn (whole, inside);
    CHECK_FALSE (in.hasProperty (schema::channelVolume));
    CHECK (in[schema::keyScale].toString() == "major"); // a scale of no word
    CHECK_FALSE (in.hasProperty (schema::trackFrozen));
    CHECK (phazite::writeValue (in[schema::trackHeight]) == "64");

    const auto second = phazite::test::findTrackIn (whole, TrackID::fromRaw (990001));
    CHECK_FALSE (second.hasProperty (schema::keyScale)); // a scale without a root

    const auto masterThere = whole.getChildWithName (schema::MASTER);
    CHECK_FALSE (masterThere.hasProperty (schema::channelVolume));
    CHECK_FALSE (masterThere.hasProperty (schema::trackAutomationMode));
    const auto laneThere = masterThere.getChildWithName (schema::LANES).getChild (0);
    CHECK_FALSE (laneThere.hasProperty (schema::laneCollapsed));
    CHECK (phazite::writeValue (laneThere[schema::laneHeight]) == "48");

    for (const auto& element : findTrackElements (whole))
        checkRoundTrip (element);

    checkReaders (*opened);

    // Saved, and opened and saved again, twice: the same text each time.
    const auto saved = writeText (whole);

    for (int opening = 0; opening < 2; ++opening)
    {
        const auto again = phazite::makeSong (parse (saved));
        REQUIRE (again != nullptr);
        CHECK (writeText (phazite::test::copyWholeSong (*again)) == saved);
    }
}

TEST_CASE ("Tracks' values: what only a track has goes from a MASTER as the song opens, so that both readers read the master alike")
{
    // The loader's one rule (keepTrackAsHeld()): of what the tracks' values cover, what isn't described on the element's type
    // goes. Before it, the lenient readers read a master's name, key, input and the rest, and the held readers don't.
    const auto file = makeMasterFile();
    const auto before = file.getChildWithName (schema::MASTER);
    CHECK_FALSE (*phazite::readTrackValue (before) == *phazite::readHeldTrack (before));
    CHECK_FALSE (phazite::readViewOf (before) == phazite::readHeldViewOf (before));

    const auto text = writeText (file);
    const auto opened = phazite::makeSong (parse (text));
    REQUIRE (opened != nullptr);
    const auto whole = phazite::test::copyWholeSong (*opened);
    const auto master = whole.getChildWithName (schema::MASTER);

    for (const auto* property : std::initializer_list<const schema::PropertyDescription*> {
             &schema::trackKind, &schema::trackName, &schema::trackColour, &schema::keyRoot, &schema::keyScale, &schema::trackInput,
             &schema::trackMonitoring, &schema::trackPad, &schema::trackMute, &schema::trackSolo, &schema::trackPinned, &schema::drumMode,
             &schema::drumOrder, &schema::drumHidden })
    {
        INFO (property->toString());
        CHECK_FALSE (master.hasProperty (property->getName()));
    }

    CHECK_FALSE (master.hasProperty ("shade")); // what Phazite doesn't know goes, since the tracks' switch
    CHECK (master.hasProperty (schema::channelFader));

    // Both readers read it alike, and the readers by ID too.
    CHECK (*phazite::readTrackValue (master) == *phazite::readHeldTrack (master));
    CHECK (phazite::readViewOf (master) == phazite::readHeldViewOf (master));
    checkRoundTrip (master);
    CHECK_FALSE (phazite::isPinned (*opened, phazite::getMasterID (*opened)));
    CHECK_FALSE (phazite::getTrackKey (*opened, phazite::getMasterID (*opened)).has_value());

    // Saved once changed, then the same text at each later opening.
    const auto saved = writeText (whole);
    CHECK (saved != text);

    for (int opening = 0; opening < 2; ++opening)
    {
        const auto again = phazite::makeSong (parse (saved));
        REQUIRE (again != nullptr);
        CHECK (writeText (phazite::test::copyWholeSong (*again)) == saved);
    }
}

TEST_CASE ("Tracks' values: after opening a file the loader changed, a versions' pass against a version saved at once writes "
           "nothing, and the round trip holds")
{
    // As the loader leaves a song, its tracks read the same both ways and write back as they are (checkSong(): the tracks'
    // round trip), so that a version saved from it holds what the song holds, and a pass makes nothing to change: no write to
    // the tree, no step. The golden project, Phazite's own; a file with damaged tracks; one whose master holds a track's.
    const auto golden = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (golden != nullptr);

    const std::vector<std::pair<const char*, juce::ValueTree>> files {
        { "the golden project", juce::ValueTree::fromXml (*golden) },
        { "damaged tracks", parse (writeText (makeDamagedFile().file)) },
        { "a master with a track's", parse (writeText (makeMasterFile())) },
    };

    for (const auto& [name, file] : files)
    {
        INFO (name);
        const auto opened = phazite::makeSong (file.createCopy());
        REQUIRE (opened != nullptr);
        checkOpenedAsVersion (*opened);
    }
}

//==============================================================================
// The sequences of design J.1 on a real song through the model, as the tree holds the switches today (what the switches' area
// must keep at the leaf switch; Bounce in Place's: tests/FreezeTests.cpp).
TEST_CASE ("Tracks' values, J.1 on a song: a mute set outside the history survives the Undo and the Redo of a rename")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    song->getUndoManager().clearUndoHistory();
    phazite::setTrackMuted (*song, tracks[0], true); // outside the history, as a mute's click
    phazite::renameTrack (*song, tracks[0], "Renamed");
    REQUIRE (phazite::canUndo (*song));
    phazite::undo (*song);
    CHECK (phazite::getTrackName (*song, tracks[0]) != "Renamed");
    CHECK (phazite::isTrackMuted (*song, tracks[0], false));
    phazite::redo (*song);
    CHECK (phazite::getTrackName (*song, tracks[0]) == "Renamed");
    CHECK (phazite::isTrackMuted (*song, tracks[0], false));
}

TEST_CASE ("Tracks' values, J.1 on a song: a version applied in a step that mutes a track, then a solo outside the history: Undo "
           "puts the mute back and keeps the solo, Redo mutes again and keeps it")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::setTrackMuted (*song, tracks[0], true);
    const auto version = phazite::test::copyWholeSong (*song); // the version: the first track muted
    phazite::setTrackMuted (*song, tracks[0], false);
    song->getUndoManager().clearUndoHistory();

    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, version);
    }

    REQUIRE (phazite::isTrackMuted (*song, tracks[0], false));
    phazite::setTrackSoloed (*song, tracks[1], true); // outside the history, as a solo's click
    phazite::undo (*song);
    CHECK_FALSE (phazite::isTrackMuted (*song, tracks[0], false));
    CHECK (phazite::isTrackSoloed (*song, tracks[1]));
    phazite::redo (*song);
    CHECK (phazite::isTrackMuted (*song, tracks[0], false));
    CHECK (phazite::isTrackSoloed (*song, tracks[1]));
}

TEST_CASE ("Tracks' values: the scales' saved names are the words the description gives a key's scale")
{
    const auto& words = schema::keyScale.getWords();
    CHECK (words.size() == phazite::getKeyScales().size());

    for (const auto scale : phazite::getKeyScales())
    {
        const auto name = phazite::getSavedScaleName (scale);
        CHECK (std::any_of (words.begin(), words.end(), [&name] (const char* word) { return name == word; }));
        CHECK (phazite::findScale (name) == std::optional (scale));
    }
}

//==============================================================================
TEST_CASE ("Tracks' values gate: the loader's rules allocate nothing for the tracks of a song Phazite wrote")
{
    // A song of 40 tracks, a folder, a return and the master, each with what Phazite's writers write of a track (names,
    // colours, keys, volumes and pans, mutes, solos, automation modes, heights, collapsed, pinned, drum rows, lanes), opened
    // once and saved: the rules over every track and the master of it as it is read from its text.
    phazite::test::TestSong s;
    phazite::test::buildBenchmarkSong (s, 40, 4.0);
    auto& made = *phazite::findSong (s.context);
    const auto tracks = phazite::getTrackIDs (made);

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto track = tracks[i];
        phazite::setSavedTrackHeight (made, track, 40 + (int) i);
        phazite::setCollapsed (made, track, i % 2 == 0);
        phazite::setPinned (made, track, i % 3 == 0);
        phazite::setTrackMuted (made, track, i % 4 == 0);
        phazite::setTrackSoloed (made, track, i % 5 == 0);
        phazite::setAutomationMode (made, track, i % 3 == 0 ? phazite::AutomationMode::latch : phazite::AutomationMode::read);
        phazite::setChannelPan (made, track, 0.25f);
        phazite::setDrumRowOrder (made, track, { 36, 38 });
        phazite::setTrackKey (made, track, phazite::Key { .root = (int) i % 12, .scale = phazite::Scale::minor });
        phazite::showLane (made, track,
                           { .device = phazite::getFader (made, track), .parameter = phazite::getVolumeParameterID (made, track) });
    }

    const auto opened = phazite::makeSong (phazite::test::copyWholeSong (made));
    REQUIRE (opened != nullptr);
    const auto state = parse (writeText (phazite::test::copyWholeSong (*opened)));
    const auto elements = findTrackElements (state);
    REQUIRE (elements.size() > 40);

    auto changed = false;
    const auto whole = phazite::countTracksReadAsValues();
    const auto allocations = phazite::test::countAllocations (
        [&elements, &changed]
        {
            for (const auto& element : elements)
            {
                changed = phazite::keepTrackAsHeld (element) || changed;
                changed = phazite::keepSwitchesAsHeld (element) || changed;
                changed = phazite::keepViewAsHeld (element) || changed;
            }
        });

    CHECK_FALSE (changed);
    CHECK (phazite::countTracksReadAsValues() == whole); // none read whole

    if (! allocations.has_value())
        return; // nothing counted on this build

    MESSAGE ("The loader's rules over " << elements.size() << " tracks: " << *allocations << " allocations");
    CHECK (*allocations == 0);
}

TEST_CASE ("Tracks' values gate: a playlist's rows, a row, a paint, a tick, a mute's click and a rename read no track whole, "
           "however many tracks the song has")
{
    // While the song's tree holds its tracks, a reader of one field of one track reads it from the track's element alone
    // (TracksValue.h: the readers of one field), and so does a row of the playlist (PlaylistModel.cpp: makeRow()), which the
    // audio clip editor makes at every paint and a drag at every event. On songs of 20 tracks and of 200, each with a clip
    // and a folder of two: the playlist's rows, a row, a strip's channel and a folder's, a switch's mute and solo, a track's clips as a paint shows them, a mute's click and a
    // rename. Counted: the tracks read whole (countTracksReadAsValues()), the elements looked at
    // (countElementsVisited(), Visits.h) and the allocations. Measured before the tracks' values (2026-11-23), elements
    // looked at / allocations: rows 230 / 407 at 20 tracks, 2,210 / 4,670 at 200; a row 12 / 25, 12 / 35; a strip's channel
    // 55 / 37, 415 / 52; a folder's 52 / 24, 412 / 34; mute and solo 47 / 12, 407 / 17; clips 7 / 28, 7 / 38; a mute's
    // click 88 / 40, 808 / 50; a rename 44 / 30, 404 / 35. After (2026-11-23): the rows 146 and 1,406, a row 8, the others
    // the same, none of them reading a track whole; allocations the same, which none may pass.
    struct Measured
    {
        Counted rows {}, row {}, channel {}, folderChannel {}, muteSolo {}, clips {}, mute {}, rename {};
        size_t shown = 0;
    };

    const auto measure = [] (int numTracks)
    {
        auto song = phazite::createSong();
        auto tracks = phazite::ensureTracks (*song, numTracks);
        const auto folder = phazite::groupIntoFolder (*song, { tracks[2], tracks[3] }, nullptr);

        for (const auto track : tracks)
            REQUIRE (phazite::insertMidiClip (*song, track, { phazite::test::at (0.0), phazite::test::at (4.0) }, nullptr).isValid());

        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setSong (context, song.get());
        const auto track = tracks[5];
        Measured measured;
        measured.shown = phazite::getPlaylistRows (context).size();
        measured.rows = count ([&] { (void) phazite::getPlaylistRows (context); });
        measured.row = count ([&] { (void) phazite::getTrackRow (context, track); });
        measured.channel = count ([&] { (void) phazite::getChannel (context, track); });
        measured.folderChannel = count ([&] { (void) phazite::getChannel (context, folder); });
        measured.muteSolo = count ([&] { (void) phazite::getMuteSolo (context, track); });
        measured.clips = count ([&] { (void) phazite::getClips (context, track); });
        measured.mute = count ([&] { phazite::toggleMute (context, track); });
        int names = 0;
        measured.rename = count ([&] { phazite::renameTrack (*song, track, "Name " + juce::String (++names)); });
        setSong (context, nullptr);
        return measured;
    };

    const auto small = measure (20), big = measure (200);
    const auto describeCost = [] (const char* what, const Counted& a, const Counted& b)
    {
        MESSAGE (std::string (what) << ": read whole " << a.whole << " (20 tracks), " << b.whole << " (200); elements looked at "
                                    << a.visits << ", " << b.visits << "; allocations " << a.allocations << ", " << b.allocations);
    };

    describeCost ("the playlist's rows", small.rows, big.rows);
    describeCost ("a row", small.row, big.row);
    describeCost ("a strip's channel", small.channel, big.channel);
    describeCost ("a folder's channel", small.folderChannel, big.folderChannel);
    describeCost ("mute and solo", small.muteSolo, big.muteSolo);
    describeCost ("a track's clips", small.clips, big.clips);
    describeCost ("a mute's click", small.mute, big.mute);
    describeCost ("a rename", small.rename, big.rename);

    // None read whole: not the rows', a row's, a paint's, a tick's, a click's nor a rename's.
    CHECK (small.shown > 20);
    CHECK (big.shown > 200);

    for (const auto* measured : { &small, &big })
    {
        CHECK (measured->rows.whole == 0);
        CHECK (measured->row.whole == 0);
        CHECK (measured->channel.whole == 0);
        CHECK (measured->folderChannel.whole == 0);
        CHECK (measured->muteSolo.whole == 0);
        CHECK (measured->clips.whole == 0);
        CHECK (measured->mute.whole == 0);
        CHECK (measured->rename.whole == 0);
    }

    // No more elements looked at than before the tracks' values.
    CHECK (small.rows.visits <= 146);
    CHECK (big.rows.visits <= 1406);
    CHECK (small.row.visits <= 8);
    CHECK (big.row.visits <= 8);
    CHECK (small.channel.visits <= 55);
    CHECK (big.channel.visits <= 415);
    CHECK (small.folderChannel.visits <= 52);
    CHECK (big.folderChannel.visits <= 412);
    CHECK (small.muteSolo.visits <= 47);
    CHECK (big.muteSolo.visits <= 407);
    CHECK (small.clips.visits <= 7);
    CHECK (big.clips.visits <= 7);
    CHECK (small.mute.visits <= 88);
    CHECK (big.mute.visits <= 808);
    CHECK (small.rename.visits <= 44);
    CHECK (big.rename.visits <= 404);

    // No more allocations than before the tracks' values, for the rows and a row (measured above).
    if (small.rows.allocations < 0 || big.rows.allocations < 0)
        return; // nothing counted on this build

    CHECK (small.rows.allocations <= 407);
    CHECK (big.rows.allocations <= 4670);
    CHECK (small.row.allocations <= 25);
    CHECK (big.row.allocations <= 35);
}
