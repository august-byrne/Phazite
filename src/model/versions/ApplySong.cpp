// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ApplySong.h"
#include "internal/SongDiff.h"
#include "internal/SongPieces.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesOutside.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"
#include "model/chords/internal/Chords.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/NoteValues.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/DeviceStore.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // An element's counted children, each named by its type and key, or, without a key, by its type and its place among
    // those of its type (a curve's points).
    struct Child
    {
        juce::ValueTree tree;
        juce::String name;
    };

    // Which of an element's parts making it like another takes: what is counted, or everything (switching in place).
    enum class Taken
    {
        counted,
        everything
    };

    bool isTaken (Taken taken, const juce::ValueTree& element, const juce::Identifier& property)
    {
        return taken == Taken::everything || ! schema::isUncountedProperty (element, property);
    }

    bool isTaken (Taken taken, const juce::ValueTree& element, const juce::ValueTree& child)
    {
        return taken == Taken::everything || ! schema::isUncountedElement (element, child);
    }

    // An element of a version's song whose piece, or part of it, couldn't be read (SongPieces.h: joinSong()): the song keeps
    // its own as it is now, wherever it is.
    bool isStandIn (const juce::ValueTree& element)
    {
        return element.hasProperty (schema::versions::missing);
    }

    juce::String nameOf (const juce::ValueTree& element)
    {
        return element.getType().toString() + ":" + schema::getElementKey (element);
    }

    // The stand-ins in the other song, by their names, as getCountedChildren() names them.
    using StandIns = std::set<juce::String>;

    StandIns findStandIns (const juce::ValueTree& other)
    {
        StandIns names;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (isStandIn (child))
                    names.insert (nameOf (child));
                else
                    visit (child);
            }
        };

        visit (other);
        return names;
    }

    const StandIns& noStandIns()
    {
        static const StandIns none;
        return none;
    }

    // Whether an element is the tempo map's: the song's TEMPOMAP, or an element in it. The song holds its tempo map as a value
    // (model/base/internal/SongValues.h), its TEMPOMAP an empty place for whole copies, which the tempo pass makes like the
    // other's (makeTempoMapLike(), below), never the passes over elements.
    bool isTempoMaps (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::TEMPOMAP) || (child.hasType (schema::TEMPOMAP) && parent.hasType (schema::PHAZITE));
    }

    // Whether an element is the chord lane's: the song's CHORDLANE, or an element in it. The song holds its lane as a value
    // (model/base/internal/SongValues.h), its CHORDLANE an empty placeholder that says whether it has one, which the lane pass
    // makes like the other's, the placeholder too (makeChordLaneLike(), below), never the passes over elements.
    bool isChordLanes (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::CHORDLANE) || (child.hasType (schema::CHORDLANE) && parent.hasType (schema::PHAZITE));
    }

    // Whether an element is the MIDI links': the song's MIDILINKS, or an element in it. The song holds its links as a value
    // (model/base/internal/SongValues.h), its MIDILINKS an empty placeholder written into its file, which the links pass
    // makes like the other's, the placeholder too (makeMidiLinksLike(), below), never the passes over elements.
    bool isMidiLinks (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::MIDILINKS) || (child.hasType (schema::MIDILINKS) && parent.hasType (schema::PHAZITE));
    }

    // Whether an element is the curves': the song's AUTOMATION, or an element in it. The song holds its curves as a value
    // (model/base/internal/SongValues.h), its AUTOMATION an empty place every song that holds its values has, whatever the
    // other has; the curves pass makes the curves like the other's (makeCurvesLike(), below), through automation's functions
    // by their parameters, never the passes over elements.
    bool isCurves (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::AUTOMATION) || (child.hasType (schema::AUTOMATION) && parent.hasType (schema::PHAZITE));
    }

    // Whether an element is the sections': the song's SECTIONS, or an element in it. The song holds its sections as a value
    // (model/base/internal/SongValues.h), its SECTIONS an empty placeholder written into its file, which the sections pass
    // makes like the other's, the placeholder too (makeSectionsLike(), below), never the passes over elements.
    bool isSections (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::SECTIONS) || (child.hasType (schema::SECTIONS) && parent.hasType (schema::PHAZITE));
    }

    // Whether an element is modulation's: a MODULATOR or a MACRO (of a channel or a container), with what is in it. The
    // modulation pass makes the song's modulators and macros like the other's (makeModulationLike(), below), through
    // modulation's setters by their IDs (model/modulation/internal/Modulation.h), never the passes over elements.
    bool isModulation (const juce::ValueTree& element)
    {
        return element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO);
    }

    // Whether an element is a device's, a container's or a chain's (with what is in it: a device's VALUES, SOUNDS and VALUEs,
    // a container's chains, a chain's devices), which the devices pass makes like the other's (makeDevicesLike(), below),
    // through the devices' setters by their IDs (model/mixer/internal/Mixer.h) and the store's functions
    // (model/plugins/internal/DeviceStore.h), never the passes over elements, though the song's tree holds them still.
    bool isDeviceItem (const juce::ValueTree& element)
    {
        return element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::CHAIN);
    }

    // Whether an element is an entry of an area the song holds beside its tree under an element that stays in it, which its
    // area's pass makes like the other's, never the passes over elements: modulation's (isModulation()), a track's SEND,
    // which the sends pass makes like the other's (makeSendsLike(), below), through the sends' setters by their IDs
    // (model/mixer/internal/Mixer.h), a device, a container or a chain (isDeviceItem()), which the devices pass alone makes
    // like the other's, and a track's or the master's LANES, the view's (makeTrackLeavesLike(), below).
    bool isKeptBeside (const juce::ValueTree& element)
    {
        return isModulation (element) || element.hasType (schema::SEND) || isDeviceItem (element) || element.hasType (schema::LANES);
    }

    // Whether an element is the song's track list (TRACKS) or its MASTER, whose tracks and own fields the tracks' pass makes
    // like the other's (makeTracksLike(), below), through the tracks' setters (model/arrangement/internal/Tracks.h), never the
    // passes over elements: they keep their places among the song's elements, which every song has, and are never added, taken
    // away or looked into by those passes.
    bool isTrackPlace (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::PHAZITE) && (child.hasType (schema::TRACKS) || child.hasType (schema::MASTER));
    }

    // A copy of an element of the other song, to be added to the song, without what in it the other's version couldn't read
    // (the stand-ins, and the elements that stand in for themselves), and without what of an area's it would hold (a clip,
    // the tempo map's entries, the chord lane's, the MIDI links, the curves, the sections, a modulator, a send, a device), which
    // the areas' passes give the song: one of the song's own elements that no area holds (Schema.h: LASTKEY, or one Phazite
    // doesn't know), never a track (the tracks' pass's: makeTracksLike()).
    // The song hasn't the stand-ins, so what they held stays as it is now: not there. A stand-in is never part of a song
    // (Schema.h: schema::versions::missing).
    juce::ValueTree copyForSong (const juce::ValueTree& element)
    {
        auto copy = element.createCopy();
        const std::function<void (juce::ValueTree)> strip = [&] (juce::ValueTree tree)
        {
            for (int i = tree.getNumChildren(); --i >= 0;)
            {
                if (auto child = tree.getChild (i); isStandIn (child) || child.hasType (schema::CLIP) || isKeptBeside (child)
                                                    || tree.hasType (schema::TEMPOMAP) || tree.hasType (schema::CHORDLANE)
                                                    || tree.hasType (schema::MIDILINKS) || tree.hasType (schema::AUTOMATION)
                                                    || tree.hasType (schema::SECTIONS))
                    tree.removeChild (i, nullptr);
                else
                    strip (child);
            }
        };

        strip (copy);
        return copy;
    }

    // Whether makeLike() places a child among the element's: taken, never a clip (a track's clips are the clip pass's:
    // ClipPass, below), nor the song's TEMPOMAP or what is in it (the tempo pass's: makeTempoMapLike()), nor
    // its CHORDLANE or what is in it (the lane pass's: makeChordLaneLike()), nor its MIDILINKS or what is in it (the links
    // pass's: makeMidiLinksLike()), nor its AUTOMATION or what is in it (the curves pass's: makeCurvesLike()), nor its
    // SECTIONS or what is in it (the sections pass's: makeSectionsLike()), nor a modulator or a macro (the modulation pass's:
    // makeModulationLike()) nor a send (the sends pass's: makeSendsLike()), which the song's tree holds none of (their areas'
    // whole state writes them in a copy), nor a device, a container or a chain (the devices pass's: makeDevicesLike()). The
    // song's TRACKS and MASTER are placed (they keep their places among the song's elements), and nothing in them is
    // (isTrackPlace()). The whole song's placeholders and places are put in the other's places after the areas' passes
    // (placePlaceholdersLike()).
    bool isPlaced (const juce::ValueTree& element, const juce::ValueTree& child, Taken taken)
    {
        return isTaken (taken, element, child) && ! child.hasType (schema::CLIP) && ! isKeptBeside (child) && ! isTempoMaps (element, child)
               && ! isChordLanes (element, child) && ! isMidiLinks (element, child) && ! isSections (element, child)
               && ! isCurves (element, child);
    }

    std::vector<Child> getCountedChildren (const juce::ValueTree& element, Taken taken)
    {
        std::vector<Child> children;
        std::map<juce::String, int> numUnkeyed;
        std::set<juce::String> names;

        for (const auto& child : element)
        {
            if (! isPlaced (element, child, taken))
                continue;

            const auto type = child.getType().toString();
            const auto key = schema::getElementKey (child);
            auto name = type + ":" + key;

            if (key.isEmpty() || names.count (name) > 0)
                name = type + "#" + juce::String (numUnkeyed[type]++);

            names.insert (name);
            children.push_back ({ child, name });
        }

        return children;
    }

    // The element made like the other, its children too (the song's own: its properties, and the elements no area holds,
    // LASTKEY, PIANOROLL where everything is taken, MERGING, and those Phazite doesn't know), in the same history (the
    // song's, or none while the song is written outside it). Its TRACKS and its MASTER keep their places among its elements
    // and are left as they are, never added, taken away or looked into (isTrackPlace()): the tracks' pass makes them like the
    // other's (makeTracksLike(), below).
    void makeLike (Song& song, juce::ValueTree live, const juce::ValueTree& other, juce::UndoManager* undo, Taken taken,
                   const StandIns& standIns)
    {
        jassert ((undo == nullptr) == song.isWritingOutsideHistory());

        for (int i = live.getNumProperties(); --i >= 0;)
            if (const auto name = live.getPropertyName (i); isTaken (taken, live, name) && ! other.hasProperty (name))
                live.removeProperty (name, undo);

        for (int i = 0; i < other.getNumProperties(); ++i)
        {
            const auto name = other.getPropertyName (i);

            if (! isTaken (taken, other, name))
                continue;

            if (const auto* value = live.getPropertyPointer (name); value == nullptr || ! isSameValue (*value, other[name]))
                live.setProperty (name, other[name], undo);
        }

        // The children it has that the other hasn't go; then, in the other's order, each it has made like the other's and
        // each it hasn't added, after the children it doesn't place before them (the song's own uncounted ones, as its
        // PIANOROLL).
        const auto wanted = getCountedChildren (other, taken);
        std::set<juce::String> wantedNames;

        for (const auto& child : wanted)
            wantedNames.insert (child.name);

        std::map<juce::String, juce::ValueTree> kept;
        std::vector<juce::ValueTree> gone;

        for (const auto& child : getCountedChildren (live, taken))
        {
            if (wantedNames.count (child.name) == 0 && standIns.count (child.name) == 0)
            {
                if (! isTrackPlace (live, child.tree))
                    gone.push_back (child.tree);
            }
            else if (wantedNames.count (child.name) > 0)
            {
                kept.emplace (child.name, child.tree);
            }
        }

        for (const auto& child : gone)
            live.removeChild (child, undo);

        int place = 0;

        for (const auto& child : wanted)
        {
            while (place < live.getNumChildren() && ! isPlaced (live, live.getChild (place), taken))
                ++place;

            if (const auto found = kept.find (child.name); found != kept.end())
            {
                // What the other's version couldn't read stays as it is, in its place; the track list and the master, the
                // tracks' pass's, in theirs.
                auto tree = found->second;

                if (! isStandIn (child.tree) && ! isTrackPlace (live, tree))
                    makeLike (song, tree, child.tree, undo, taken, standIns);

                if (const auto index = live.indexOf (tree); index != place)
                    live.moveChild (index, place, undo);
            }
            else if (! isStandIn (child.tree) && ! isTrackPlace (other, child.tree))
            {
                live.addChild (copyForSong (child.tree), place, undo);
            }
            else
            {
                continue; // a stand-in for what the song hasn't, or a place every song has: nothing to put there
            }

            ++place;
        }
    }

    //==============================================================================
    // A track's element in the other song's whole copy by its ID (the master's for its): what the passes read the other's by.
    juce::ValueTree findTrackIn (const juce::ValueTree& other, std::uint64_t id)
    {
        if (const auto master = other.getChildWithName (schema::MASTER); master.isValid() && readID (master[schema::trackID]) == id)
            return master;

        for (const auto& track : getTrackElements (other))
            if (readID (track[schema::trackID]) == id)
                return track;

        return {};
    }

    // The folder a track's element is in, in a whole copy: the TRACK it is in, none for the track list.
    TrackID getFolderThere (const juce::ValueTree& element)
    {
        const auto parent = element.getParent();
        return parent.hasType (schema::TRACK) ? TrackID::fromRaw (readID (parent[schema::trackID])) : TrackID();
    }

    // Whether the song holds a track (a folder, a return; not the master) of this ID, and whether it holds a folder of it, as
    // the tracks' keeper keeps them (Tracks.h: findHeldTrack(): no element read).
    bool holdsTrack (const Song& song, TrackID id)
    {
        const auto held = findHeldTrack (song, id);
        return held.has_value() && ! held->isMaster();
    }

    bool holdsFolder (const Song& song, TrackID id)
    {
        const auto held = findHeldTrack (song, id);
        return held.has_value() && held->isFolder();
    }

    // A track's index in a folder's list of the song's (the top list for none), -1 where the list holds none of it.
    int findIndexIn (const SongTracks& tracks, TrackID folder, TrackID id)
    {
        if (const auto* list = tracks.findList (folder))
            for (size_t i = 0; i < list->size(); ++i)
                if ((*list)[i] == id)
                    return (int) i;

        return -1;
    }

    // The place a track goes in a folder's list of the song's (the top list for none: Tracks.h, setTrackPlace()), to be where
    // the other song has it among the tracks there (`there`: its element in the other's whole copy): right after the nearest
    // track before it there that the list holds, the track itself left out of the list; else first.
    int findTrackPlaceLike (const SongTracks& tracks, TrackID folder, TrackID track, const juce::ValueTree& there)
    {
        const auto* list = tracks.findList (folder);

        if (list == nullptr)
            return 0;

        for (auto before = there.getSibling (-1); before.isValid(); before = before.getSibling (-1))
        {
            if (! before.hasType (schema::TRACK))
                continue;

            const auto id = TrackID::fromRaw (readID (before[schema::trackID]));
            auto index = 0;

            for (const auto each : *list)
            {
                if (each == track)
                    continue;

                if (each == id)
                    return index + 1;

                ++index;
            }
        }

        return 0;
    }

    // A track's own fields (TracksValue.h: what writeTrackElement() writes of a TRACK's, or of the MASTER's) whose description
    // says they count as no change (Schema.h: isUncountedProperty()): what a pass of what is counted leaves as the song holds
    // them (keepUncounted()). Read from the descriptions, once: the properties a value with every field set writes, each asked.
    const std::vector<juce::Identifier>& getUncountedFields (bool master)
    {
        static const auto read = [] (bool ofMaster)
        {
            auto full = ofMaster ? TrackValue::makeMaster() : TrackValue {};
            full.id = TrackID::fromRaw (1);
            full.kind = ofMaster ? TrackValue::Kind::master : TrackValue::Kind::folder;
            full.name = juce::String ("Name");
            full.colour = 0xff102030u;
            full.bus = true;
            full.volume = -1.0f;
            full.pan = 0.5f;
            full.fader = DeviceID::fromRaw (2);
            full.vca = DeviceID::fromRaw (3);
            full.key = Key {};
            full.input = "1";
            full.monitoring = TrackValue::Monitoring::always;
            full.padDevice = DeviceID::fromRaw (4);
            full.pad = 36;

            const auto element = writeTrackElement (full);
            std::vector<juce::Identifier> names;

            for (int i = 0; i < element.getNumProperties(); ++i)
                if (schema::isUncountedProperty (element, element.getPropertyName (i)))
                    names.push_back (element.getPropertyName (i));

            return names;
        };

        static const auto ofTracks = read (false), ofMaster = read (true);
        return master ? ofMaster : ofTracks;
    }

    // The other's value of a track with what it doesn't count (getUncountedFields(): none, as the descriptions stand) as the
    // song holds it, where what is counted is taken.
    void keepUncounted (TrackValue& theirs, const TrackValue& held, Taken taken)
    {
        if (taken != Taken::counted)
            return;

        const auto& names = getUncountedFields (theirs.isMaster());

        if (names.empty())
            return;

        const auto written = writeTrackElement (held);

        for (const auto& name : names)
            setTrackProperty (theirs, name, written[name]);
    }

    //==============================================================================
    // Whether two numbers write the same text in the song (SongText.h: isSameValue()): alike but past the digits written,
    // which makeLike() takes for the same, so that what arithmetic leaves past them (a length of 4.000000000000001 against
    // the other's 4) is no change.
    bool writeAlike (double a, double b)
    {
        return isSameValue (juce::var (a), juce::var (b));
    }

    // What an audio clip plays as the other song holds it, each number that writes as the song's clip writes it kept as the
    // song holds it (writeAlike()), its markers too, each by its place.
    ClipSound keepWhatWritesAlike (const ClipSound& held, ClipSound sound)
    {
        if (writeAlike (held.tempo, sound.tempo))
            sound.tempo = held.tempo;

        if (writeAlike (held.pitch, sound.pitch))
            sound.pitch = held.pitch;

        auto warps = immer::flex_vector<WarpValue>().transient();

        for (size_t i = 0; i < sound.warps.size(); ++i)
        {
            auto warp = sound.warps[i];

            if (i < held.warps.size())
            {
                if (writeAlike (held.warps[i].time, warp.time))
                    warp.time = held.warps[i].time;

                if (writeAlike (held.warps[i].playsAt, warp.playsAt))
                    warp.playsAt = held.warps[i].playsAt;
            }

            warps.push_back (warp);
        }

        sound.warps = warps.persistent();

        if (sound.warps == held.warps)
            sound.warps = held.warps; // the song's own, shared

        return sound;
    }

    // A clip's value as the other song holds it, each number that writes as the song's clip writes it kept as the song holds
    // it (writeAlike()).
    ClipValue keepWhatWritesAlike (const ClipValue& held, ClipValue value)
    {
        const auto keepBeats = [] (const auto& heldBeats, auto& beats)
        {
            if (writeAlike (heldBeats.inBeats(), beats.inBeats()))
                beats = heldBeats;
        };

        const auto keep = [] (auto heldNumber, auto& number)
        {
            if (writeAlike ((double) heldNumber, (double) number))
                number = heldNumber;
        };

        keepBeats (held.start, value.start);
        keepBeats (held.length, value.length);
        keepBeats (held.offset, value.offset);
        keep (held.fileStart, value.fileStart);
        keep (held.fileEnd, value.fileEnd);
        keep (held.gain, value.gain);
        keep (held.fadeIn.seconds, value.fadeIn.seconds);
        keep (held.fadeIn.bend, value.fadeIn.bend);
        keep (held.fadeOut.seconds, value.fadeOut.seconds);
        keep (held.fadeOut.bend, value.fadeOut.bend);
        keep (held.takeLength, value.takeLength);
        value.sound = keepWhatWritesAlike (held.sound, value.sound);
        return value;
    }

    //==============================================================================
    // The song's tracks' clips made like the other's in a pass of their own, once the rest of what is made like the other's
    // is (the tracks there, each where the other has it), through the clips' functions (model/arrangement/internal/Clips.h)
    // by the song and the clips' IDs and values (the song's clips' area: model/base/internal/SongValues.h). For each track it
    // makes like the other's: the clips the track holds that the other hasn't there taken away (or left for the track the
    // other has them on, or sent there); each the other holds there set as the other holds it (one on another track now is
    // moved, keeping what it is: its selection too) or added with its ID; and the track's clips put in the other's order.
    // Then each alias group given its notes as one value (Notes.h: setClipNotesValue()) and its audio clips' sound, its clips
    // on tracks the pass doesn't make like the other's too (known bug 234). What the other's version couldn't read (a stand-in)
    // stays as the song has it. Each write only where the song differs, in the history's step in progress, or outside it while
    // the song is written outside it (Song::OutsideHistory), as the clips' functions write.
    class ClipPass
    {
    public:
        // What the pass does with a clip of a track it makes like the other's that the other holds on another track: leaves it
        // for that track, which the pass makes like the other's too (the whole song made like the other), or sends it there as
        // it is now, among that track's clips where the other has it (one track's line, which leaves the other tracks as they
        // are). Where the song hasn't that track, or the pass doesn't make it like the other's, it goes.
        enum class Elsewhere
        {
            leftForItsTrack,
            sentThere
        };

        // standInsStay: whether a clip the other holds as a stand-in on another track stays where the song has it (the whole
        // song made like a version, as its other parts' stand-ins stay), or goes (another branch's song, a copy of the song).
        ClipPass (Song& s, const juce::ValueTree& other, Elsewhere e, bool standInsStay)
            : song (s),
              elsewhere (e),
              standInsStayWhereTheyAre (standInsStay)
        {
            // The other's tracks and clips, read once.
            for (const auto& track : getTrackElements (other))
            {
                const auto id = TrackID::fromRaw (readID (track[schema::trackID]));
                otherTracks.emplace (id.getRaw(), track);
                otherOrder.push_back (id);
                auto& clipsThere = otherTrackClips[id.getRaw()];

                for (const auto& child : track)
                {
                    if (child.hasType (schema::CLIP))
                    {
                        const auto clip = readID (child[schema::clipID]);
                        otherClips.emplace (clip, OtherClip { child, id, isStandIn (child) });
                        clipsThere.push_back (ClipID::fromRaw (clip));
                    }
                }
            }
        }

        // A track whose clips the pass makes like the other's: one both have, which the other's version could read.
        void addTrack (TrackID track)
        {
            const auto there = otherTracks.find (track.getRaw());

            if (there != otherTracks.end() && ! isStandIn (there->second) && hasTrack (song, track)
                && passed.insert (track.getRaw()).second)
                tracks.emplace_back (track, there->second);
        }

        // Every track both have, in the other's order.
        void addEveryTrack()
        {
            for (const auto track : otherOrder)
                addTrack (track);
        }

        // The tracks' clips made like the other's, as the class says. How many writes it made.
        int run()
        {
            for (const auto& track : tracks)
                takeAway (track.first);

            putBackLinks();

            for (const auto& [track, there] : tracks)
                makeTrackClipsLike (track, there);

            giveGroupsTheirNotes();
            giveGroupsTheirSounds();
            return writes;
        }

        // A track that is to go: its clips that the other holds on another track that the song has sent there, as they are
        // now; the rest go with it. How many writes it made.
        int sendHome (TrackID track)
        {
            for (const auto& clip : getClipValues (song, track))
                if (const auto found = otherClips.find (clip.id.getRaw());
                    found != otherClips.end() && found->second.track != track && holdsClips (found->second.track))
                    sendTo (clip, found->second.track);

            return writes;
        }

    private:
        // A clip of the other song: its element there, the track it is on there, and whether it stands in for what the other's
        // version couldn't read.
        struct OtherClip
        {
            juce::ValueTree element;
            TrackID track;
            bool standIn = false;
        };

        // A clip the pass gave the other's value: its element there, and that value.
        struct Given
        {
            juce::ValueTree element;
            ClipValue value;
        };

        Song& song;
        const Elsewhere elsewhere;
        const bool standInsStayWhereTheyAre;
        std::map<std::uint64_t, juce::ValueTree> otherTracks;         // by their IDs
        std::vector<TrackID> otherOrder;                              // the other's tracks, as getTrackElements() gives them
        std::map<std::uint64_t, OtherClip> otherClips;                // by their IDs
        std::map<std::uint64_t, std::vector<ClipID>> otherTrackClips; // each of the other's tracks' clips, in its order there
        std::vector<std::pair<TrackID, juce::ValueTree>> tracks;
        std::set<std::uint64_t> passed; // the tracks' IDs
        std::vector<std::pair<ClipID, std::uint64_t>> unlinked;
        std::map<std::uint64_t, ClipID> groupMembers; // a clip of the song in each alias group the pass has seen, by its link
        std::vector<Given> given;
        int writes = 0;

        // Whether a track of the song holds clips: a track's, not a folder's, a return's or the master's (as the tracks'
        // keeper keeps it: no element read).
        bool holdsClips (TrackID track) const
        {
            const auto held = findHeldTrack (song, track);
            return held.has_value() && held->holdsClips();
        }

        // Whether the pass gives a clip the other's value: one the other holds, which its version could read, on a track the
        // pass makes like the other's.
        bool isMadeLikeTheOthers (ClipID clip) const
        {
            const auto found = otherClips.find (clip.getRaw());
            return found != otherClips.end() && ! found->second.standIn && passed.count (found->second.track.getRaw()) > 0;
        }

        void setOrder (TrackID track, const std::vector<ClipID>& order)
        {
            if (order != getClips (song, track))
            {
                setTrackClipOrder (song, track, order);
                ++writes;
            }
        }

        // The track's clips that the other hasn't there: gone, unless the other holds one as a stand-in (which stays where the
        // song has it, where the other's version could read the rest: standInsStayWhereTheyAre) or on another track (where it
        // goes: Elsewhere). Before any clip is set, so that one the pass adds to an alias group never leaves it again as the
        // last of the group but one goes.
        void takeAway (TrackID track)
        {
            for (const auto& clip : getClipValues (song, track))
            {
                const auto found = otherClips.find (clip.id.getRaw());

                if (found != otherClips.end() && found->second.track == track)
                {
                    noteMember (clip);
                    continue;
                }

                if (found == otherClips.end())
                    erase (clip);
                else if (elsewhere == Elsewhere::sentThere && holdsClips (found->second.track))
                    sendTo (clip, found->second.track);
                else if (elsewhere == Elsewhere::sentThere)
                    erase (clip);
                else if (found->second.standIn ? ! standInsStayWhereTheyAre : passed.count (found->second.track.getRaw()) == 0)
                    erase (clip);
                else
                    noteMember (clip);
            }
        }

        void noteMember (const ClipValue& clip)
        {
            if (clip.link != 0)
                groupMembers.emplace (clip.link, clip.id);
        }

        // A clip taken out of the song. The last clip but this one of its alias group leaves the group as it goes (eraseClip()),
        // which the other may not say: put back after, unless the pass gives that clip the other's value (putBackLinks()).
        void erase (const ClipValue& clip)
        {
            if (clip.link != 0)
                if (const auto group = getAliasClips (song, clip.id); group.size() == 2)
                    unlinked.emplace_back (group[1], clip.link);

            eraseClip (song, clip.id);
            ++writes;
        }

        void putBackLinks()
        {
            for (const auto& [clip, link] : unlinked)
            {
                if (isMadeLikeTheOthers (clip))
                    continue;

                if (const auto held = getClipValue (song, clip); held.has_value() && held->link != link)
                {
                    setClipLink (song, clip, link);
                    ++writes;
                }
            }
        }

        // A clip sent to the track the other holds it on, as it is now, among that track's clips in the other's order, whatever
        // else the track holds and wherever among its elements: after the nearest clip before it there that the track holds;
        // where there is none, before the nearest clip after it there that the track holds; where there is none either, after
        // them all.
        void sendTo (const ClipValue& clip, TrackID track)
        {
            auto moved = clip;
            moved.track = track;
            setClip (song, clip, moved);
            ++writes;

            auto order = getClips (song, track);
            order.erase (std::remove (order.begin(), order.end(), clip.id), order.end());
            std::map<ClipID, std::size_t> held; // the track's clips, by their places in its order
            auto at = order.size();

            for (std::size_t i = 0; i < order.size(); ++i)
                held.emplace (order[i], i);

            if (const auto found = otherTrackClips.find (track.getRaw()); found != otherTrackClips.end())
            {
                const auto& there = found->second;
                const auto self = std::find (there.begin(), there.end(), clip.id);
                auto placed = false;

                for (auto before = self; before != there.begin() && ! placed;)
                {
                    if (const auto heldBefore = held.find (*--before); heldBefore != held.end())
                    {
                        at = heldBefore->second + 1;
                        placed = true;
                    }
                }

                if (self != there.end())
                {
                    for (auto after = std::next (self); after != there.end() && ! placed; ++after)
                    {
                        if (const auto heldAfter = held.find (*after); heldAfter != held.end())
                        {
                            at = heldAfter->second;
                            placed = true;
                        }
                    }
                }
            }

            order.insert (order.begin() + (std::ptrdiff_t) at, clip.id);
            setOrder (track, order);
        }

        // The track's clips as the other holds them there, in its order: what its version couldn't read where the song has it,
        // and a clip the song holds as the other kind under the other's ID taken away and the other's added in its place.
        void makeTrackClipsLike (TrackID track, const juce::ValueTree& otherTrack)
        {
            std::vector<ClipID> theirs; // in the other's order

            for (const auto& element : otherTrack)
            {
                if (! element.hasType (schema::CLIP))
                    continue;

                const auto id = ClipID::fromRaw (readID (element[schema::clipID]));
                theirs.push_back (id);

                if (isStandIn (element))
                    continue;

                auto value = readClipValue (element);
                value.track = track;
                jassert (value.id == id);

                if (const auto held = getClipValue (song, id); held.has_value() && held->kind != value.kind)
                    eraseForAnotherKind (*held);

                if (const auto held = getClipValue (song, id))
                {
                    value = keepWhatWritesAlike (*held, value);

                    if (! (*held == value))
                    {
                        setClip (song, *held, value); // on another track: moved here
                        ++writes;
                    }
                }
                else
                {
                    add (element, value);
                }

                if (value.link != 0)
                    groupMembers[value.link] = id;

                given.push_back ({ element, value });
            }

            // The other's order, then what else the track holds (what the other's version couldn't read of another track, or
            // what the pass moves to another track after) in the order it holds it.
            const auto now = getClips (song, track);
            const std::set<ClipID> holds (now.begin(), now.end());
            std::vector<ClipID> order;
            std::set<ClipID> placed;

            for (const auto id : theirs)
                if (holds.count (id) > 0 && placed.insert (id).second)
                    order.push_back (id);

            for (const auto id : now)
                if (placed.count (id) == 0)
                    order.push_back (id);

            setOrder (track, order);
        }

        // A clip of one kind in the song that the other holds as the other kind under its ID (MIDI and audio: two computers can
        // make the same ID), taken out of the song for the other's to be added in its place, with nothing of the kind it was
        // left on it. The clip of its alias group that is left alone keeps the group, as the other's clip would leave it.
        void eraseForAnotherKind (const ClipValue& clip)
        {
            std::optional<std::pair<ClipID, std::uint64_t>> left; // the clip left alone in its group, and the link it holds

            if (clip.link != 0)
                if (const auto group = getAliasClips (song, clip.id); group.size() == 2)
                    if (const auto other = getClipValue (song, group[1]))
                        left.emplace (other->id, other->link);

            eraseClip (song, clip.id);
            ++writes;

            if (left.has_value())
            {
                setClipLink (song, left->first, left->second);
                ++writes;
            }
        }

        // A clip the song hasn't, added with its ID; a MIDI clip with its notes and controllers' values, those of its alias
        // group where a clip of the group in the song holds them alike, as one value.
        void add (const juce::ValueTree& element, const ClipValue& value)
        {
            ClipNotes notes;

            if (value.isMidi())
            {
                notes = readClipNotes (element);

                if (const auto member = groupMembers.find (value.link); value.link != 0 && member != groupMembers.end())
                    if (const auto held = getClipValue (song, member->second);
                        held.has_value() && held->link == value.link && held->isMidi())
                        if (const auto groups = getClipNotesValue (song, member->second); groups == notes)
                            notes = groups;
            }

            [[maybe_unused]] const auto added = addClip (song, value, value.isMidi() ? &notes : nullptr);
            jassert (added == value.id); // the song had no clip of its ID
            ++writes;
        }

        // Each alias group's notes, and each MIDI clip's in none, the other's: one value for the group, its clips on other
        // tracks too, the one a clip of it holds where it is the other's already (Notes.h: makeClipNotesLike() for one clip).
        void giveGroupsTheirNotes()
        {
            std::set<std::uint64_t> groups;

            for (const auto& [element, value] : given)
            {
                if (! value.isMidi() || (value.link != 0 && ! groups.insert (value.link).second))
                    continue;

                const auto notes = readClipNotes (element);
                std::vector<ClipNotes> held;

                for (const auto member : getAliasClips (song, value.id))
                    if (isMidiClip (song, member))
                        held.push_back (getClipNotesValue (song, member));

                auto one = notes;

                for (const auto& each : held)
                {
                    if (each == notes)
                    {
                        one = each;
                        break;
                    }
                }

                if (std::any_of (held.begin(), held.end(), [&one] (const ClipNotes& each) { return ! each.isSameAs (one); }))
                {
                    setClipNotesValue (song, value.id, one);
                    ++writes;
                }
            }
        }

        // What each audio alias group's clips share (AudioClips.h: shareTheSound()), as a clip of it the pass gave the other's
        // value holds it, given to every clip of the group in the song, those on other tracks too.
        void giveGroupsTheirSounds()
        {
            std::set<std::uint64_t> groups;

            for (const auto& [element, value] : given)
            {
                if (! value.isAudio() || value.link == 0)
                    continue;

                const auto* values = song.getValues();
                const auto* stored = values != nullptr ? values->findClip (value.id) : nullptr; // until the clips change: copied

                if (stored == nullptr || ! groups.insert (value.link).second)
                    continue;

                // The very sound the clip holds, so that the group holds one (ClipsValue.h: ClipSound).
                const auto sound = stored->sound;

                for (const auto member : getAliasClips (song, value.id))
                {
                    if (member == value.id)
                        continue;

                    const auto held = getClipValue (song, member);

                    if (! held.has_value() || ! held->isAudio())
                        continue;

                    auto next = *held;
                    next.sound = sound;

                    if (! next.sound.isSameAs (held->sound))
                    {
                        setClip (song, *held, next);
                        ++writes;
                    }
                }
            }
        }
    };

    // What gives the song's tracks their clips here is the clip pass alone, the song its tempo map the tempo pass alone, and
    // the song its chord lane the lane pass alone, never the keeper of the song's values, which takes in the clips' elements
    // of a track that comes into the song's tree with them, the tempo map's entries that come into its TEMPOMAP, and the
    // chord lane's that come into its CHORDLANE (SongValues.h: a safety net): every track the tracks' pass adds is a value,
    // its element holding nothing but its own fields (makeTracksLike(); Tracks.h: setNewTracks()), every other copy added to
    // the song is without an area's entries (copyForSong()), and the passes over elements leave the song's TEMPOMAP and
    // CHORDLANE to the areas' passes (isPlaced()). Checked in Debug builds, as each of the functions
    // below ends: what the keeper took in counted for every area that counts it, a line each, in the order of the areas.
    struct NothingTakenIn
    {
        explicit NothingTakenIn (const Song& s)
            : song (s),
              before (count (s))
        {
        }

        ~NothingTakenIn() { jassert (count (song) == before); }

        static int count (const Song& s)
        {
            const auto* values = s.getValues();

            if (values == nullptr)
                return 0;

            return values->countAdopted()              // the clips', with their notes
                   + values->countTempoMapAdopted()    // the tempo map's
                   + values->countChordLaneAdopted()   // the chord lane's
                   + values->countMidiLinksAdopted()   // the MIDI links'
                   + values->countCurvesAdopted()      // the curves'
                   + values->countSectionsAdopted()    // the sections'
                   + values->countModulationAdopted()  // modulation's: no track or copy the passes add holds one
                   + values->countSendsAdopted()       // the sends': nor a send
                   + values->countDeviceStoreAdopted() // the store's: the devices pass sets an item's entries in the store
                   + values->countDevicesAdopted()     // the devices': no track or copy the passes add holds a device, a
                                                       // container or a chain, which the devices pass alone sets
                                                       // (makeDevicesLike())
                   + values->countTrackLeavesAdopted() // the tracks' switches and view: nor a switch, a view property or a
                                                       // LANES, which their pass sets (makeTrackLeavesLike())
                   + values->countTracksAdopted();     // the tracks': nor a TRACK or the MASTER's own, which their pass
                                                       // sets (makeTracksLike())
        }

        const Song& song;
        const int before;

        JUCE_DECLARE_NON_COPYABLE (NothingTakenIn)
    };

    // The song's tempo map made the other song's (its TEMPOMAP's, read as the song holds a map, as everything that enters the
    // tempo map's area is: TempoMapValue.h, readHeldTempoMap(); the map a song without one plays, 120 BPM in 4/4, where it has
    // none), through the tempo map's one setter, in the mode of the moment (SongValues.h: setTempoMap(), which writes nothing for the map the song holds already). The rule for what the
    // other's version couldn't read (a stand-in: the TEMPOMAP or an entry of it, which only a piece that isn't Phazite's can
    // make): a map that holds what could not be read is left as the song has it, whole. One stand-in entry keeps the whole
    // map from being applied, where the generic pass kept the stand-in elements and applied the rest: a map's entries have no
    // identity of their own (two at one beat are told apart only by their order), so there is no entry of the song's to keep
    // in the stand-in's place, nor a place to put the rest around it. Whether it wrote.
    bool makeTempoMapLike (Song& song, const juce::ValueTree& other)
    {
        auto* values = song.getValues();
        const auto then = other.getChildWithName (schema::TEMPOMAP);

        if (values == nullptr || (then.isValid() && (isStandIn (then) || ! findStandIns (then).empty())))
            return false;

        const auto before = values->tempoMap.get();
        values->setTempoMap (song, readHeldTempoMap (then));
        return ! values->tempoMap.get().isSameAs (before);
    }

    // The song's chord lane made the other song's: a lane or none, as the other has one (its CHORDLANE, read as the song holds
    // a lane: ChordLaneValue.h, readHeldChordLane()), through the chord lane's code, which adds or takes away the song's
    // CHORDLANE and sets its value, in the mode of the moment (model/chords/internal/Chords.h: makeChordLaneAs(), which writes
    // nothing for the lane the song holds already). The rule for what the other's version couldn't read, as the tempo map's
    // (makeTempoMapLike()): a lane that holds what could not be read is left as the song has it, whole, its being there too
    // (a lane's chords and keys have no identity of their own to keep in the stand-in's place). Whether it wrote.
    bool makeChordLaneLike (Song& song, const juce::ValueTree& other)
    {
        const auto then = other.getChildWithName (schema::CHORDLANE);

        if (song.getValues() == nullptr || (then.isValid() && (isStandIn (then) || ! findStandIns (then).empty())))
            return false;

        return makeChordLaneAs (song, then);
    }

    // The song's MIDI links made the other song's: links or none, as the other has a MIDILINKS (read as the song holds links:
    // MidiLinksValue.h, readHeldMidiLinks()), through the MIDI links' code, which adds or takes away the song's MIDILINKS and
    // sets its value, in the mode of the moment (model/midi/internal/MidiLinks.h: makeMidiLinksAs(), which writes nothing for
    // the links the song holds already). The rule for what the other's version couldn't read, as the passes over elements had
    // it: a MIDILINKS that stands in for what could not be read is left as the song has it, whole, its being there too; a link
    // that does keeps the song's link of its controller (a link's key) in its place, or none where the song has none, and the
    // rest are the other's. Whether it wrote. Only for the whole song (makeSongLike(), makeSongTheSame(), makeSongAsCopied()):
    // no line's restore gives the song another's links (makeSongOwnLikeIn() and makeTrackLikeIn() never did).
    bool makeMidiLinksLike (Song& song, const juce::ValueTree& other)
    {
        auto then = other.getChildWithName (schema::MIDILINKS);

        if (song.getValues() == nullptr || (then.isValid() && isStandIn (then)))
            return false;

        if (then.isValid() && ! findStandIns (then).empty())
        {
            const auto live = copyWhole (song, song.getState().getChildWithName (schema::MIDILINKS));
            then = then.createCopy();

            for (int i = then.getNumChildren(); --i >= 0;)
            {
                const auto child = then.getChild (i);

                if (! isStandIn (child))
                    continue;

                const auto name = nameOf (child);
                then.removeChild (i, nullptr);

                for (const auto& link : live)
                {
                    if (nameOf (link) == name)
                    {
                        then.addChild (link.createCopy(), i, nullptr);
                        break;
                    }
                }
            }
        }

        return makeMidiLinksAs (song, then);
    }

    // The song's sections made the other song's: sections or none, as the other has a SECTIONS (read as the song holds them:
    // SectionsValue.h, readHeldSections()), through the sections' code, which adds or takes away the song's SECTIONS and sets
    // its value, in the mode of the moment (model/arrangement/internal/Sections.h: makeSectionsAs(), which writes nothing for
    // the sections the song holds already). The rule for what the other's version couldn't read, as the passes over elements
    // had it: a SECTIONS that stands in for what could not be read is left as the song has it, whole, its being there too; a
    // section that does keeps the song's section of its ID (a section's key) in its place, or none where the song has none,
    // and the rest are the other's. Whether it wrote. For the whole song (makeSongLike(), makeSongTheSame(),
    // makeSongAsCopied()) and the song's own line (makeSongOwnLikeIn()), whose restore gave the song another's sections as it
    // gave its tempo map.
    bool makeSectionsLike (Song& song, const juce::ValueTree& other)
    {
        auto then = other.getChildWithName (schema::SECTIONS);

        if (song.getValues() == nullptr || (then.isValid() && isStandIn (then)))
            return false;

        if (then.isValid() && ! findStandIns (then).empty())
        {
            const auto live = copyWhole (song, song.getState().getChildWithName (schema::SECTIONS));
            then = then.createCopy();

            for (int i = then.getNumChildren(); --i >= 0;)
            {
                const auto child = then.getChild (i);

                if (! isStandIn (child))
                    continue;

                const auto name = nameOf (child);
                then.removeChild (i, nullptr);

                for (const auto& section : live)
                {
                    if (nameOf (section) == name)
                    {
                        then.addChild (section.createCopy(), i, nullptr);
                        break;
                    }
                }
            }
        }

        return makeSectionsAs (song, then);
    }

    // Which of the song's curves a curves pass makes like the other's, by the parameters they automate.
    using CurvesTaken = std::function<bool (const ParameterRef&)>;

    // The song's curves that which takes made like the other song's, by their parameters, in one set of the curves' area, in
    // the mode of the moment (SongValues.h: setCurves()): each the song has that the other hasn't taken out (all of them where
    // the other has no AUTOMATION); each the other has made the song's (read as the song holds a curve: CurveValues.h,
    // readHeldCurve()), one the song has keeping its place and its points' keys by their places, one it hasn't added after
    // the others; what the other's version couldn't read (a curve that stands in for itself, a run of its points lost, or the
    // other's AUTOMATION a stand-in) left as the song has it, there or not. The whole song's (wholeSong): then its curves in
    // the other's order. A line's: the curves the song has keep their places, those it hasn't are added in the order of their
    // key texts (Schema.h: getElementKey(): "1042/cutoff"), as they always were. How many writes it made: one, or none for
    // curves alike to the song's.
    int makeCurvesLike (Song& song, const juce::ValueTree& other, const CurvesTaken& which, bool wholeSong)
    {
        const auto otherAutomation = other.getChildWithName (schema::AUTOMATION);

        if (otherAutomation.isValid() && isStandIn (otherAutomation))
            return 0;

        // The other's curves which takes, each parameter's once (the first), in its order.
        std::vector<std::pair<ParameterRef, juce::ValueTree>> theirs;
        const auto isTheirs = [&theirs] (const ParameterRef& parameter)
        { return std::any_of (theirs.begin(), theirs.end(), [&parameter] (const auto& each) { return each.first == parameter; }); };

        for (const auto& curve : otherAutomation)
            if (curve.hasType (schema::CURVE))
                if (const auto parameter = readCurveParameter (curve); which (parameter) && ! isTheirs (parameter))
                    theirs.emplace_back (parameter, curve);

        // The song's curves worked out whole, then set once (SongValues.h: setCurves(), which writes nothing for curves alike to
        // those the song holds): those it has that the other hasn't taken out; the other's given the keys of the song's points
        // by their places (keyByPlace()), in place or after the others; for the whole song, in the other's order.
        auto* values = song.getValues();

        if (values == nullptr)
            return 0;

        const auto before = values->curves.get();
        auto next = before;

        for (const auto& parameter : before.order)
            if (which (parameter) && ! isTheirs (parameter))
                next = withoutCurve (next, parameter);

        auto given = theirs;

        if (! wholeSong)
            std::stable_sort (given.begin(), given.end(), [] (const auto& a, const auto& b)
                              { return schema::getElementKey (a.second) < schema::getElementKey (b.second); });

        // A curve alike to the song's (keys aside) left as the song holds it, so that what hears the curves change hears of
        // those that changed alone.
        for (const auto& [parameter, curve] : given)
        {
            if (isStandIn (curve))
                continue;

            const auto* held = before.find (parameter);

            if (auto read = readHeldCurve (curve); held == nullptr || ! (read == *held))
                next = withCurve (next, parameter, values->keyByPlace (held, std::move (read)));
        }

        if (wholeSong)
        {
            std::vector<ParameterRef> order;

            for (const auto& each : theirs)
                order.push_back (each.first);

            next = withCurveOrder (next, order);
        }

        values->setCurves (song, std::move (next));
        return values->curves.get().isSameAs (before) ? 0 : 1;
    }

    // Every curve of the song made like the other's (makeCurvesLike()), once the rest of the song is: how many writes it made.
    int makeAllCurvesLike (Song& song, const juce::ValueTree& other)
    {
        return makeCurvesLike (song, other, [] (const ParameterRef&) { return true; }, true);
    }

    //==============================================================================
    // An owner of modulators and macros (ModulationValue.h: ModulatorPlace) as a key: a container's by its ID, a track's by
    // its own, the master's by none.
    using OwnerKey = std::pair<int, std::uint64_t>;

    OwnerKey getOwnerKey (const ModulatorPlace& place)
    {
        if (place.container.isValid())
            return { 2, place.container.getRaw() };

        return place.onMaster ? OwnerKey { 1, 0 } : OwnerKey { 0, place.track.getRaw() };
    }

    // The other song's modulation as the modulation pass reads it: each owner's modulators and macros in its order (each
    // channel's own, then its containers', the tracks' in the other's order, then the master's), each read as the song holds
    // one (ModulationValue.h: readHeldModulator()), which everything that enters modulation's area reads; and what the other's
    // version couldn't read: each owner that stands in for it (a track's, a container's), and each modulator or macro that
    // holds a stand-in (SongPieces.h: holdsStandIn()), which an owner's order names in its place, unread.
    struct OtherModulation
    {
        struct Owner
        {
            ModulatorPlace place {};
            immer::flex_vector<ModulatorID> order {};
        };

        std::vector<Owner> owners {};
        std::map<std::uint64_t, ModulatorValue> modulators {}; // by their IDs, those read
        std::set<OwnerKey> standInOwners {};
        std::set<std::uint64_t> standInModulators {};

        explicit OtherModulation (const juce::ValueTree& other)
        {
            auto channels = getTrackElements (other);
            channels.push_back (other.getChildWithName (schema::MASTER));

            for (const auto& channel : channels)
            {
                if (! channel.isValid())
                    continue;

                const auto place = channel.hasType (schema::MASTER)
                                       ? ModulatorPlace { .onMaster = true }
                                       : ModulatorPlace { .track = TrackID::fromRaw (readID (channel[schema::trackID])) };

                if (isStandIn (channel))
                {
                    standInOwners.insert (getOwnerKey (place));
                    continue;
                }

                read (channel, place);

                for (const auto& child : channel)
                {
                    if (! child.hasType (schema::CONTAINER))
                        continue;

                    const ModulatorPlace inside { .track = place.track,
                                                  .onMaster = place.onMaster,
                                                  .container = DeviceID::fromRaw (readID (child[schema::deviceID])) };

                    if (isStandIn (child))
                        standInOwners.insert (getOwnerKey (inside));
                    else
                        read (child, inside);
                }
            }
        }

        void read (const juce::ValueTree& owner, const ModulatorPlace& place)
        {
            auto order = immer::flex_vector<ModulatorID>().transient();

            for (const auto& child : owner)
            {
                if (! isModulation (child))
                    continue;

                const auto id = readID (child[schema::modulatorID]);

                if (holdsStandIn (child))
                {
                    standInModulators.insert (id);
                    order.push_back (ModulatorID::fromRaw (id));
                }
                else if (auto value = readHeldModulator (child, place); value.has_value() && modulators.count (id) == 0)
                {
                    order.push_back (value->id);
                    modulators.emplace (id, std::move (*value));
                }
            }

            owners.push_back ({ place, order.persistent() });
        }

        // Whether the song's modulator or macro of this value is one the other's version couldn't read: its owner there (or its
        // container's channel) a stand-in, or its own element there holding one.
        bool standsIn (const ModulatorValue& value) const
        {
            const ModulatorPlace channel { .track = value.place.track, .onMaster = value.place.onMaster };
            return standInModulators.count (value.id.getRaw()) > 0 || standInOwners.count (getOwnerKey (value.place)) > 0
                   || standInOwners.count (getOwnerKey (channel)) > 0;
        }
    };

    // Which of the song's modulators and macros a modulation pass makes like the other's, by its value (the song's, and the
    // other's).
    using ModulatorsTaken = std::function<bool (const ModulatorValue&)>;

    // The song's modulators and macros that which takes made like the other song's, once the passes over elements put each
    // owner (a track, the master, a container) where the other has it, through modulation's setters by their IDs, in the mode
    // of the moment (model/modulation/internal/Modulation.h: setModulator(), eraseModulator(), setModulatorOrder(), each of
    // which writes nothing for what the song holds already): each the song has that the other hasn't taken out; each the other
    // has (read as the song holds one: OtherModulation) set as it holds it, at its place in its owner's order there (one the
    // song has keeping its place, one it hasn't added there), on the owner it names there; then each owner's in the other's
    // order. What the other's version couldn't read (an owner that stands in for it, a modulator or a macro that holds a
    // stand-in) is left as the song has it, there or not, in its place. The whole song's: every modulator and macro. A line's:
    // those of its channel and of the containers on it (in the song now, or in the other). How many writes it made.
    int makeModulationLike (Song& song, const juce::ValueTree& other, const ModulatorsTaken& which)
    {
        // The song's next modulation worked out whole, then set once (modulation's area tells it once).
        const OtherModulation theirs (other);
        const auto ours = getModulationValue (song);
        auto next = ours;
        auto writes = 0;

        for (const auto& [id, value] : ours.modulators)
        {
            if (which (value) && theirs.modulators.count (id) == 0 && ! theirs.standsIn (value))
            {
                next = withoutModulator (next, value.id);
                ++writes;
            }
        }

        for (const auto& owner : theirs.owners)
        {
            // The place the owner's take in the song: a container's macros on the channel the song's container is on, as
            // setModulator() gives them (the passes put it where the other has it, but where they leave it elsewhere, its
            // macros stay with it).
            const auto place = findOwnerPlace (song, owner.place);

            if (! place.has_value())
                continue;

            auto taken = false;

            for (size_t i = 0; i < owner.order.size(); ++i)
            {
                const auto found = theirs.modulators.find (owner.order[i].getRaw());

                if (found == theirs.modulators.end() || ! which (found->second))
                    continue;

                taken = true;
                auto value = found->second;
                value.place = *place;

                // One held as the other kind replaced whole at its place, as setModulator() replaces it.
                const auto* held = next.find (value.id);

                if (held != nullptr && held->isMacro() != value.isMacro())
                    next = withoutModulator (next, value.id);
                else if (held != nullptr && *held == value)
                    continue;

                next = withHeldModulator (next, std::move (value), (int) i);
                ++writes;
            }

            if (! taken)
                continue;

            // Each owner's in the other's order: those named first, in their order, the others after them in theirs.
            if (const auto* list = next.findList (owner.place))
            {
                std::vector<ModulatorID> wanted;
                const auto isWanted = [&wanted] (ModulatorID id) { return std::find (wanted.begin(), wanted.end(), id) != wanted.end(); };

                for (const auto id : owner.order)
                    if (std::find (list->begin(), list->end(), id) != list->end() && ! isWanted (id))
                        wanted.push_back (id);

                for (const auto id : *list)
                    if (! isWanted (id))
                        wanted.push_back (id);

                if (const immer::flex_vector<ModulatorID> order (wanted.begin(), wanted.end()); ! (order == *list))
                {
                    next = withOwnerOrder (next, owner.place, order);
                    ++writes;
                }
            }
        }

#if JUCE_DEBUG
        // Every container's macro on the channel its container is on in the song.
        for (const auto& [id, value] : next.modulators)
            if (value.place.container.isValid())
                if (const auto place = findOwnerPlace (song, value.place))
                    jassert (*place == value.place);
#endif

        setAllModulation (song, std::move (next));
        return writes;
    }

    // Every modulator and macro of the song made like the other's (makeModulationLike()), once the rest of the song is: how
    // many writes it made.
    int makeAllModulationLike (Song& song, const juce::ValueTree& other)
    {
        return makeModulationLike (song, other, [] (const ModulatorValue&) { return true; });
    }

    //==============================================================================
    // The other song's sends as the sends pass reads them: each track's sends in its order (the tracks' in the other's order),
    // each read as the song holds one (SendsValue.h: readHeldSend()), which everything that enters the sends' area reads, the
    // first of an ID; and what the other's version couldn't read: each track that stands in for it, and each send that does,
    // which its track's order names in its place, unread.
    struct OtherSends
    {
        struct Track
        {
            TrackID id {};
            std::vector<DeviceID> order {};
        };

        std::vector<Track> tracks {};
        std::map<std::uint64_t, SendValue> sends {}; // by their IDs, those read
        std::set<std::uint64_t> standInTracks {}, standInSends {};

        explicit OtherSends (const juce::ValueTree& other)
        {
            for (const auto& track : getTrackElements (other))
            {
                const auto id = TrackID::fromRaw (readID (track[schema::trackID]));

                if (isStandIn (track))
                {
                    standInTracks.insert (id.getRaw());
                    continue;
                }

                Track listed { id, {} };

                for (const auto& child : track)
                {
                    const auto send = readHeldSendID (child);

                    if (! send.isValid())
                        continue;

                    if (isStandIn (child))
                    {
                        standInSends.insert (send.getRaw());
                        listed.order.push_back (send);
                    }
                    else if (auto value = readHeldSend (child, id); value.has_value() && sends.count (send.getRaw()) == 0)
                    {
                        listed.order.push_back (send);
                        sends.emplace (send.getRaw(), std::move (*value));
                    }
                }

                tracks.push_back (std::move (listed));
            }
        }

        const Track* findTrack (TrackID id) const
        {
            const auto found = std::find_if (tracks.begin(), tracks.end(), [id] (const Track& each) { return each.id == id; });
            return found != tracks.end() ? &*found : nullptr;
        }

        // Whether the song's send of this value is one the other's version couldn't read: its track there a stand-in, or its
        // own element there one.
        bool standsIn (const SendValue& ours) const
        {
            return standInSends.count (ours.id.getRaw()) > 0 || standInTracks.count (ours.from.getRaw()) > 0;
        }
    };

    // Where a send goes among a track's sends to be where the other song has it there, as the devices pass places an item in
    // a list (findDevicePlaceLike()): after the one before it there that the track holds; else before the track's others (a
    // send's order group); else after them, the first.
    int findSendPlaceLike (const SongSends& sends, const OtherSends& theirs, TrackID track, DeviceID send)
    {
        const auto* list = sends.tracks.find (track.getRaw());

        if (list == nullptr)
            return -1;

        if (const auto* there = theirs.findTrack (track))
        {
            const auto at = std::find (there->order.begin(), there->order.end(), send);

            for (auto before = at; before != there->order.begin();)
            {
                --before;

                if (const auto found = std::find (list->begin(), list->end(), *before); found != list->end())
                    return (int) std::distance (list->begin(), found) + 1;
            }
        }

        return 0;
    }

    // Which of the song's sends a sends pass makes like the other's, by the track it is on (the song's, and the other's).
    using SendTracksTaken = std::function<bool (TrackID)>;

    // The song's sends that which takes made like the other song's, once the passes over elements put each track where the
    // other has it, in one set of the sends' area, in the mode of the moment (SongValues.h: setAllSends(), which writes
    // nothing for sends alike to those the song holds), as the passes over elements made a track's SEND elements like the
    // other's: each the song has on a track taken that the other holds on no track taken, taken out, or sent, as it is now, to
    // the track the other holds it on where the song has that track (placed as the other has it: findSendPlaceLike()); each the
    // other has on a track taken that the song has (read as the song holds one: OtherSends) set as it holds it, on that track,
    // from wherever the song holds it now; then each track taken's in the other's order, those it holds that the other doesn't
    // name after them. A send to a return the song hasn't is set too, as the passes over elements set it, and goes as what
    // names the gone does (removeBrokenReferences()). What the other's version couldn't read (a track that stands in for it,
    // a send that does) is left as the song has it, there or not, in its place. The whole song's: every track. A line's: its
    // track, and those that come back with it (a folder's, gone since). How many writes it made.
    int makeSendsLike (Song& song, const juce::ValueTree& other, const SendTracksTaken& which)
    {
        auto* values = song.getValues();

        if (values == nullptr)
            return 0;

        const OtherSends theirs (other);
        const auto ours = values->sends.get();
        auto next = ours;
        auto writes = 0;
        const auto isSongTrack = [&song] (TrackID track) { return holdsTrack (song, track); };

        for (const auto& [key, send] : ours.sends)
        {
            if (! which (send.from) || theirs.standsIn (send))
                continue;

            const auto found = theirs.sends.find (key);
            const auto there = found != theirs.sends.end() ? found->second.from : TrackID();

            if (there.isValid() && isSongTrack (there))
            {
                if (which (there))
                    continue; // set below, from there

                auto moved = send;
                moved.from = there;
                next = withSend (next, std::move (moved), findSendPlaceLike (next, theirs, there, send.id));
                ++writes;
                continue;
            }

            next = withoutSend (next, send.id);
            ++writes;
        }

        for (const auto& track : theirs.tracks)
        {
            if (! which (track.id) || ! isSongTrack (track.id))
                continue;

            for (const auto id : track.order)
            {
                const auto found = theirs.sends.find (id.getRaw());

                if (found == theirs.sends.end())
                    continue; // a stand-in: the song's of its ID left as it is

                if (const auto* held = next.find (id); held != nullptr && *held == found->second)
                    continue;

                next = withSend (next, found->second);
                ++writes;
            }

            if (const auto* list = next.tracks.find (track.id.getRaw()))
            {
                std::vector<DeviceID> wanted;
                const auto isWanted = [&wanted] (DeviceID id) { return std::find (wanted.begin(), wanted.end(), id) != wanted.end(); };

                for (const auto id : track.order)
                    if (std::find (list->begin(), list->end(), id) != list->end() && ! isWanted (id))
                        wanted.push_back (id);

                for (const auto id : *list)
                    if (! isWanted (id))
                        wanted.push_back (id);

                if (const immer::flex_vector<DeviceID> order (wanted.begin(), wanted.end()); ! (order == *list))
                {
                    next = withTrackSendOrder (next, track.id, order);
                    ++writes;
                }
            }
        }

        values->setAllSends (song, std::move (next));
        return writes;
    }

    // Every send of the song made like the other's (makeSendsLike()), once the rest of the song is: how many writes it made.
    int makeAllSendsLike (Song& song, const juce::ValueTree& other)
    {
        return makeSendsLike (song, other, [] (TrackID) { return true; });
    }

    //==============================================================================
    // A channel by a place on it (DevicePlace): the master's, or a track's by its ID.
    using ChannelKey = std::pair<bool, std::uint64_t>;

    ChannelKey getChannelKey (const DevicePlace& place)
    {
        return { place.onMaster, place.onMaster ? 0 : place.track.getRaw() };
    }

    // The place of the list of what an item holds: a container's chains, a chain's devices.
    DevicePlace getInsidePlace (const DeviceValue& item)
    {
        auto place = item.place;

        if (item.form == DeviceValue::Form::container)
            place.container = item.id;
        else
            place.chain = item.id;

        return place;
    }

    bool holdsItems (const DeviceValue& item)
    {
        return item.form == DeviceValue::Form::container || item.form == DeviceValue::Form::chain;
    }

    // Whether two forms are of one kind of item: a device (one of Phazite's own, a plug-in), a container, a chain.
    bool isOfOneKind (DeviceValue::Form a, DeviceValue::Form b)
    {
        const auto kind = [] (DeviceValue::Form form) { return form == DeviceValue::Form::plugin ? DeviceValue::Form::own : form; };
        return kind (a) == kind (b);
    }

    // An item's name as the passes over elements named its element (nameOf()): "DEVICE:44", "CONTAINER:40", "CHAIN:41".
    juce::String nameOfItem (const DeviceValue& item)
    {
        const auto& type = item.form == DeviceValue::Form::container ? schema::CONTAINER
                           : item.form == DeviceValue::Form::chain   ? schema::CHAIN
                                                                     : schema::DEVICE;
        return type.getName().toString() + ":" + juce::String ((juce::int64) item.id.getRaw());
    }

    // Whether a child of an owner's element is in the owner's list: a channel's devices and containers, a container's chains,
    // a chain's devices (DevicesValue.h: readDeviceList() lists the same).
    bool isListedIn (const juce::ValueTree& owner, const juce::ValueTree& child)
    {
        if (owner.hasType (schema::TRACK) || owner.hasType (schema::MASTER))
            return child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER);

        if (owner.hasType (schema::CONTAINER))
            return child.hasType (schema::CHAIN);

        return owner.hasType (schema::CHAIN) && child.hasType (schema::DEVICE);
    }

    // Which channels' devices a devices pass makes like the other's, by a place on one (the song's, and the other's).
    using DeviceChannelsTaken = std::function<bool (const DevicePlace&)>;

    // The other song's devices as the devices pass reads them: each channel's devices and containers, each container's
    // chains and each chain's devices, the first of an ID, each owner before what it holds (a channel's list, then its
    // containers' chains, then their devices), the tracks' in the other's order, then the master's; each owner's list in its
    // order; and what the other's version couldn't read: each channel that stands in for it (a track's piece), and each item
    // that does (a device's or a container's piece, or a plug-in's state lost), which its owner's list names in its place,
    // unread. Of the channels the pass takes (which: every one for the whole song; a line's own), each item read whole, as the
    // song holds one (DevicesValue.h: readHeldDevice()), which everything that enters the devices' area reads, with what the
    // store holds of it (DeviceStoreValue.h: readHeldStoreOf()), a plug-in's values' order and a window's place. Of the
    // others, only what the pass may need, without a device read whole: each item's own fields (readHeldDeviceFields(),
    // which reads the same items), its form kept by its ID (holds()), and, where the song holds its ID on a channel the pass
    // takes (held: the song's devices), the item itself and its owner's list, for the pass to send the song's there as it is
    // now.
    struct OtherDevices
    {
        struct List
        {
            DevicePlace owner {};
            std::vector<DeviceID> order {};
        };

        std::vector<DeviceValue> items {};
        std::map<std::uint64_t, DeviceValue::Form> forms {}; // the items' forms, by their IDs
        std::vector<List> lists {};
        std::map<std::uint64_t, DevicePlace> standIns {}; // each item that stands in, with the place of the list naming it
        std::set<ChannelKey> standInChannels {};
        DeviceStore store {};
        std::map<std::uint64_t, WindowPlace> windows {};

        OtherDevices (const juce::ValueTree& other, const DeviceChannelsTaken& which, const SongDevices& held)
        {
            auto channels = getTrackElements (other);
            channels.push_back (other.getChildWithName (schema::MASTER));

            // Whether the song holds an item of this ID on a channel the pass takes.
            const std::function<bool (DeviceID)> isHeldThere = [&which, &held] (DeviceID id)
            {
                const auto* found = held.find (id);
                return found != nullptr && which (found->place);
            };

            for (const auto& channel : channels)
            {
                if (! channel.isValid())
                    continue;

                const auto place = channel.hasType (schema::MASTER)
                                       ? DevicePlace { .onMaster = true }
                                       : DevicePlace { .track = TrackID::fromRaw (readID (channel[schema::trackID])) };

                if (isStandIn (channel))
                    standInChannels.insert (getChannelKey (place));
                else
                    readList (channel, place, which (place), isHeldThere);
            }
        }

        // An owner's list, and those of what it holds: whole, of a channel taken; else each item's own fields alone.
        void readList (const juce::ValueTree& owner, const DevicePlace& place, bool whole,
                       const std::function<bool (DeviceID)>& isHeldThere)
        {
            List list;
            list.owner = place;
            auto kept = whole; // a list of a channel not taken is kept only where it names an item the song holds on one taken
            std::vector<std::pair<juce::ValueTree, DevicePlace>> inside;

            for (const auto& child : owner)
            {
                if (! isListedIn (owner, child))
                    continue;

                const auto id = readDeviceElementID (child);

                if (! id.isValid() || forms.count (id.getRaw()) > 0 || standIns.count (id.getRaw()) > 0)
                    continue;

                if (isStandIn (child))
                {
                    standIns.emplace (id.getRaw(), place);
                    list.order.push_back (id);
                    continue;
                }

                auto value =
                    whole ? readHeldDevice (child, readDevicePlace (child)) : readHeldDeviceFields (child, readDevicePlace (child));

                if (! value.has_value())
                    continue;

                forms.emplace (id.getRaw(), value->form);
                list.order.push_back (id);

                if (holdsItems (*value))
                    inside.emplace_back (child, getInsidePlace (*value));

                if (! whole)
                {
                    if (isHeldThere (id))
                    {
                        kept = true;
                        items.push_back (std::move (*value));
                    }

                    continue;
                }

                readHeldStoreOf (child, store);

                if (const auto window = readWindowPlaceOf (child))
                    windows.emplace (id.getRaw(), *window);

                items.push_back (std::move (*value));
            }

            if (kept && ! list.order.empty())
                lists.push_back (std::move (list));

            for (const auto& [element, at] : inside)
                readList (element, at, whole, isHeldThere);
        }

        const List* findList (const DevicePlace& owner) const
        {
            const auto found = std::find_if (lists.begin(), lists.end(), [&owner] (const List& each) { return each.owner == owner; });
            return found != lists.end() ? &*found : nullptr;
        }

        // Whether the other holds an item of this ID, of this kind, read.
        bool holds (DeviceID id, DeviceValue::Form form) const
        {
            const auto found = forms.find (id.getRaw());
            return found != forms.end() && isOfOneKind (found->second, form);
        }

        // Whether the song's item of this ID at a place is one the other's version couldn't read: on a channel that stands in
        // for it, in a container or a chain that does, or one the list it is in there names as one that does.
        bool standsIn (DeviceID id, const DevicePlace& place) const
        {
            if (standInChannels.count (getChannelKey (place)) > 0)
                return true;

            for (const auto owner : { place.container, place.chain })
                if (owner.isValid() && standIns.count (owner.getRaw()) > 0)
                    return true;

            const auto found = standIns.find (id.getRaw());
            return found != standIns.end() && found->second == place;
        }
    };

    // Whether the song holds what holds what is at a place: its chain (in its container, on its channel), its container (on
    // its channel), or its channel (a track's, or the master).
    bool holdsPlace (const Song& song, const SongDevices& now, const DevicePlace& place)
    {
        const auto isOnChannel = [&place] (const DeviceValue* item)
        { return item != nullptr && getChannelKey (item->place) == getChannelKey (place); };

        if (place.chain.isValid())
        {
            const auto* chain = now.find (place.chain);
            return isOnChannel (chain) && chain->form == DeviceValue::Form::chain && chain->place.container == place.container;
        }

        if (place.container.isValid())
        {
            const auto* container = now.find (place.container);
            return isOnChannel (container) && container->form == DeviceValue::Form::container;
        }

        return place.onMaster || holdsTrack (song, place.track);
    }

    // Where an item goes in its list to be where the other song has it there: after the one before it there that the song's
    // list holds; else first; -1 (after the others) for a list the song hasn't.
    int findDevicePlaceLike (const SongDevices& now, const OtherDevices& theirs, const DeviceValue& item)
    {
        const auto* list = now.findList (item.place);
        const auto* there = theirs.findList (item.place);

        if (list == nullptr)
            return -1;

        if (there != nullptr)
        {
            const auto at = std::find (there->order.begin(), there->order.end(), item.id);

            for (auto before = at; before != there->order.begin();)
            {
                --before;

                for (size_t i = 0; i < list->size(); ++i)
                    if ((*list)[i] == *before)
                        return (int) i + 1;
            }
        }

        return 0;
    }

    // The song's devices, containers and chains that which takes made like the other's (OtherDevices), through the devices'
    // setters by their IDs and places, in the mode of the moment (model/mixer/internal/Mixer.h: setDevice(),
    // setNewDevices(), eraseDevice(), setDeviceListOrder(), each of which writes nothing for what the song holds already),
    // once the passes over elements put each channel where the other has it; now: the song's devices as the pass leaves
    // them. In three rounds, so that what moves is the same element, a plug-in keeping its instance, its values, its state and
    // its window's place (a device moved into a container on another track by a version comes home without being loaded
    // again; a container taken apart since gets its devices back from the channel, one made since gives them back to it):
    //   - each the other holds (its owners before what they hold), on a channel taken there or held by the song on one taken
    //     here: set as the other holds it, at its place there (after the one before it there that the song's list holds),
    //     which moves one the song holds in another list as the same element with all it holds; one the song hasn't (or holds
    //     as another form of a device: a plug-in for one of Phazite's own) made with what the store holds of it, its state and
    //     values, and its window's place, in the one action that puts it in (setNewDevices()), as the passes over elements
    //     added the other's element whole; one the song holds as another kind of item under its ID (a device for a chain:
    //     two computers can make the same ID) taken out first, wherever the song holds it: on a line's restore, even on a
    //     track outside the line (a known limit, not a fault: it needs two computers that made the same ID, and a merge
    //     gives what it adds IDs of its own). One the song holds on a channel taken that the other holds on
    //     one not taken (a line's: what the line's track holds that the other holds on another track) sent there as it is now
    //     where the song holds what holds it there, else taken out; one whose place there the song holds nothing to hold
    //     (a track the song hasn't) left where it is;
    //   - each the song holds on a channel taken that the other holds none of (of its kind) taken out with what it holds,
    //     but one the other's version couldn't read (OtherDevices::standsIn(), or one the other holds as a stand-in somewhere
    //     else that standIns names): left as the song has it;
    //   - each list the other holds on a channel taken put in its order there, those it doesn't name (what the other's version
    //     couldn't read) after them in theirs.
    // What the store holds of each the other holds is its pass's (makeDeviceStoreLike()). The whole song's: every channel. A
    // line's: its track's, and those of the tracks that come back with it (a folder's, gone since); the master's line the
    // master's. A setter's refusal is none the pass can meet: it writes only where holdsPlace() found what holds the item,
    // and erases only what the song holds where now says, so that each is asserted (the assertion net and the fuzz fail on
    // a song that reaches one). How many writes it made.
    int makeDeviceItemsLike (Song& song, const OtherDevices& theirs, const DeviceChannelsTaken& which, const StandIns& standIns,
                             SongDevices& now)
    {
        auto writes = 0;
        std::set<std::uint64_t> going; // sent where the song holds nothing to hold it: taken out

        for (const auto& wanted : theirs.items)
        {
            const auto id = wanted.id;
            const auto* found = now.find (id);
            auto held = found != nullptr ? std::optional (*found) : std::nullopt;
            const auto takenThere = which (wanted.place);

            if (! takenThere && ! (held.has_value() && which (held->place)))
                continue;

            if (held.has_value() && ! isOfOneKind (held->form, wanted.form))
            {
                if (! takenThere)
                    continue; // the song's, which the other holds none of: taken out below

                const auto erased = eraseDevice (song, id, held->place);
                jassert (erased); // held where now says
                writes += erased ? 1 : 0;
                now = withoutDevice (now, id);
                held.reset();
            }

            auto next = wanted;

            if (! takenThere)
            {
                next = *held; // sent home as it is now
                next.place = wanted.place;
            }

            if (! holdsPlace (song, now, next.place))
            {
                if (! takenThere)
                    going.insert (id.getRaw());

                continue;
            }

            const auto index = findDevicePlaceLike (now, theirs, next);

            if (held.has_value() && held->form == next.form)
            {
                if (*held == next)
                    continue;

                // Refused only where the song holds the ID as another kind (taken out above) or hasn't what holds its place
                // (holdsPlace()); and what differs is written.
                const auto set = setDevice (song, next, index);
                jassert (set);
                writes += set ? 1 : 0;
                now = withDevice (now, std::move (next), index);
                continue;
            }

            if (held.has_value())
            {
                const auto erased = eraseDevice (song, id, held->place);
                jassert (erased); // held where now says
                writes += erased ? 1 : 0;
                now = withoutDevice (now, id);
            }

            MadeDevices made;
            made.devices.push_back (next);
            made.store = takeEntriesOf (theirs.store, id);

            // Refused only for nothing made or a place the song hasn't what holds (holdsPlace()).
            const auto added = setNewDevices (song, made, index);
            jassert (added);

            if (! added)
                continue;

            ++writes;
            now = withDevice (now, std::move (next), index);

            if (const auto window = theirs.windows.find (id.getRaw()); window != theirs.windows.end())
                setWindowPlace (song, id, window->second);
        }

        // Each the song holds on a channel taken that the other holds none of, top down: a container with what it holds.
        const std::function<void (const DevicePlace&)> takeOutIn = [&] (const DevicePlace& owner)
        {
            const auto* list = now.findList (owner);

            if (list == nullptr)
                return;

            const std::vector<DeviceID> ids (list->begin(), list->end());

            for (const auto id : ids)
            {
                const auto* found = now.find (id);

                if (found == nullptr)
                    continue;

                const auto item = *found;

                if (going.count (id.getRaw()) > 0
                    || (which (item.place) && ! theirs.holds (id, item.form) && ! theirs.standsIn (id, item.place)
                        && standIns.count (nameOfItem (item)) == 0))
                {
                    const auto erased = eraseDevice (song, id, item.place);
                    jassert (erased); // held where now says
                    writes += erased ? 1 : 0;
                    now = withoutDevice (now, id);
                }
                else if (holdsItems (item))
                {
                    takeOutIn (getInsidePlace (item));
                }
            }
        };

        std::vector<DevicePlace> channels;

        for (const auto& channel : now.channels)
            channels.push_back (channel.first == 0 ? DevicePlace { .onMaster = true }
                                                   : DevicePlace { .track = TrackID::fromRaw (channel.first) });

        for (const auto& channel : channels)
            takeOutIn (channel);

        // Each list the other holds on a channel taken in its order there: those it names first, in their order, the others
        // after them in theirs.
        for (const auto& list : theirs.lists)
        {
            const auto* held = which (list.owner) ? now.findList (list.owner) : nullptr;

            if (held == nullptr)
                continue;

            std::vector<DeviceID> wanted;
            const auto isWanted = [&wanted] (DeviceID id) { return std::find (wanted.begin(), wanted.end(), id) != wanted.end(); };

            for (const auto id : list.order)
                if (std::find (held->begin(), held->end(), id) != held->end() && ! isWanted (id))
                    wanted.push_back (id);

            for (const auto id : *held)
                if (! isWanted (id))
                    wanted.push_back (id);

            if (const immer::flex_vector<DeviceID> order (wanted.begin(), wanted.end()); ! (order == *held))
            {
                // The list's own IDs, each once, in another order: refused never, and moved.
                const auto ordered = setDeviceListOrder (song, list.owner, order);
                jassert (ordered);
                writes += ordered ? 1 : 0;
                now = withListOrder (now, list.owner, order);
            }
        }

        return writes;
    }

    // What the store holds of the devices and chains the other holds on a channel which takes, made what the other holds of
    // them, once the devices pass put each where the other has it (now: the song's devices as it left them), through the
    // store's functions by their IDs, in the mode of the moment (model/plugins/internal/DeviceStore.h: setStoreEntries(),
    // which writes nothing where the song holds the same): a plug-in's state, program, format ID and version and its values
    // set by hand, and a chain's mute and solo; and with taken everything, a window's place too (view state, which a version
    // applied in the history leaves as it is). Each item's entries a look in the store by its ID (getStoreOf()), compared
    // before anything is written, so that the store's writer is asked only where something differs. What the other's version
    // couldn't read (a stand-in) left as the song has it. How many writes it made.
    int makeDeviceStoreLike (Song& song, const OtherDevices& theirs, const DeviceChannelsTaken& which, Taken taken, const SongDevices& now)
    {
        auto writes = 0;

        for (const auto& item : theirs.items)
        {
            const auto* ours = now.find (item.id);
            const auto isStored = item.form == DeviceValue::Form::plugin || item.form == DeviceValue::Form::chain;

            if (! which (item.place) || ours == nullptr || ! (ours->place == item.place) || (! isStored && taken == Taken::counted))
                continue;

            if (isStored)
            {
                const auto wanted = takeEntriesOf (theirs.store, item.id);

                if (! (getStoreOf (song, item.id) == wanted) && setStoreEntries (song, item.id, wanted))
                    ++writes;
            }

            if (taken != Taken::everything)
                continue;

            const auto window = getWindowPlace (song, item.id);
            std::optional<WindowPlace> windowThere;

            if (const auto there = theirs.windows.find (item.id.getRaw()); there != theirs.windows.end())
                windowThere = there->second;

            if (window == windowThere)
                continue;

            if (windowThere.has_value())
                setWindowPlace (song, item.id, *windowThere);
            else
                forgetWindowPlace (song, item.id);

            ++writes;
        }

        return writes;
    }

    // The song's devices as the devices' area holds them (SongValues.h: devices; none read), a copy the pass keeps up to date
    // as it writes; read from its tree only for a song that holds no values.
    SongDevices getSongDevices (const Song& song)
    {
        return getDevicesValue (song);
    }

    // The song's devices, containers and chains, and what the store holds of them, made like the other song's, through the
    // devices' pass (makeDeviceItemsLike()) and then the store's (makeDeviceStoreLike()): the song's devices as its keeper
    // holds them (getSongDevices(): none read), the other's read once, whole of the channels which takes alone
    // (OtherDevices); for those channels, in the mode of the moment, what the other's version couldn't read left as the song
    // has it. After the passes over elements, which leave the devices alone (isKeptBeside()), and before modulation's pass (a
    // container's macros need their container) and the curves'. How many writes each made: the devices' (written in the undo
    // history alone, but as a song opens), and the store's (written outside it too: a plug-in's own window, its state stored
    // as the song saves, a chain soloed, a window moved).
    struct DeviceWrites
    {
        int items = 0;
        int store = 0;
    };

    std::atomic<std::uint64_t> storePassWrites { 0 }; // countDeviceStorePassWrites(): a count alone, which orders nothing

    DeviceWrites makeDevicesLike (Song& song, const juce::ValueTree& other, const DeviceChannelsTaken& which, const StandIns& standIns,
                                  Taken taken)
    {
        auto now = getSongDevices (song);
        const OtherDevices theirs (other, which, now);
        DeviceWrites writes;
        writes.items = makeDeviceItemsLike (song, theirs, which, standIns, now);
        writes.store = makeDeviceStoreLike (song, theirs, which, taken, now);
        storePassWrites.fetch_add ((std::uint64_t) writes.store, std::memory_order_relaxed);
        return writes;
    }

    // Every track's clips made like the other's, once the rest of the song is: how many writes it made.
    int makeClipsLike (Song& song, const juce::ValueTree& other, bool standInsStay)
    {
        ClipPass clips (song, other, ClipPass::Elsewhere::leftForItsTrack, standInsStay);
        clips.addEveryTrack();
        return clips.run();
    }

    // The areas the song holds as values beside its tree made like the other's, once the rest of the song is (its tracks
    // first: makeTracksLike(), whose tracks the clip pass gives their clips), each through its pass, a line each, in this
    // order: the clips with their notes (makeClipsLike()), then the devices with what the
    // store holds of them (makeDevicesLike(), every channel's: before modulation's pass, which gives a container's macros
    // their container, and the curves'), then the tempo map (makeTempoMapLike()), then the chord lane (makeChordLaneLike()),
    // then the MIDI links (makeMidiLinksLike()), then the curves (makeAllCurvesLike()), then the sections
    // (makeSectionsLike()), then modulation (makeAllModulationLike()), then the sends (makeAllSendsLike()), none of which
    // reads another's but modulation's the devices'. Taken and standIns as the passes over elements had them (the devices'
    // store and a window's place; what the other's version couldn't read that stays where the song has it). How many writes
    // they made (the tempo map's one, where it wrote; the chord lane's one, where it wrote), of what the undo history alone
    // writes: not the devices' store's, which is written outside it too, and which a song made as copied takes back
    // (makeSongAsCopied()), as the passes over elements took back a plug-in's state and a chain's solo. A channel's devices
    // stand before its other children in a whole copy, always (DevicesArea.h: writeDevicesIn()), so that a folder's tracks have
    // no place among its devices to be given.
    int makeValuesLike (Song& song, const juce::ValueTree& other, bool standInsStay, Taken taken, const StandIns& standIns)
    {
        auto writes = makeClipsLike (song, other, standInsStay);
        writes += makeDevicesLike (song, other, [] (const DevicePlace&) { return true; }, standIns, taken).items;
        writes += makeTempoMapLike (song, other) ? 1 : 0;
        writes += makeChordLaneLike (song, other) ? 1 : 0;
        writes += makeMidiLinksLike (song, other) ? 1 : 0; // the MIDI links' one, where it wrote
        writes += makeAllCurvesLike (song, other);
        writes += makeSectionsLike (song, other) ? 1 : 0; // the sections' one, where it wrote
        writes += makeAllModulationLike (song, other);
        writes += makeAllSendsLike (song, other);
        return writes;
    }

    // Whether an element is the placeholder or the place of an area the song holds as a value, which the passes over elements
    // leave where it is (isPlaced()): the song's CHORDLANE, MIDILINKS or SECTIONS, which the area's pass adds or takes away
    // (makeChordLaneLike(), makeMidiLinksLike(), makeSectionsLike()), and its TEMPOMAP and AUTOMATION, places every song that
    // holds its values has (a whole copy writes the AUTOMATION only while the song has a curve: CurvesArea.h).
    bool isPlaceholder (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        return parent.hasType (schema::PHAZITE)
               && (child.hasType (schema::TEMPOMAP) || child.hasType (schema::CHORDLANE) || child.hasType (schema::MIDILINKS)
                   || child.hasType (schema::AUTOMATION) || child.hasType (schema::SECTIONS));
    }

    // The song's placeholders and places (isPlaceholder()) put where the other song has them among its elements, once the
    // pass over the song's elements (makeLike(), which leaves them where they are) and the areas' passes (makeValuesLike(),
    // which add one after the others) are done, in the same history (the song's step, or none while the song is written
    // outside it), so that the song's file writes its elements in the other's order: by makeLike()'s rule, with the
    // placeholders and places among what it places. In the other's order, each of its elements that makeLike() places, and
    // each placeholder or place, is put at the next place among the song's that are such and that the other has, past those
    // that aren't (its uncounted ones) and those the other hasn't (its AUTOMATION where the other, a whole copy of a song
    // without curves, has none: left where it is), the song's own of that name (as getCountedChildren() names them); one the
    // song hasn't (a stand-in's) takes no place. The elements makeLike() placed are in that order already, so only the
    // placeholders and places move, and a second pass moves nothing. Only for the whole song (makeSongLike(),
    // makeSongTheSame(), makeSongAsCopied()): the song's own line adds its SECTIONS after the others, as it always did.
    void placePlaceholdersLike (Song& song, const juce::ValueTree& other, juce::UndoManager* undo, Taken taken)
    {
        jassert ((undo == nullptr) == song.isWritingOutsideHistory());
        auto live = song.getState();
        const auto isOrdered = [taken] (const juce::ValueTree& parent, const juce::ValueTree& child)
        { return isPlaced (parent, child, taken) || isPlaceholder (parent, child); };

        const auto getOrdered = [&isOrdered] (const juce::ValueTree& element)
        {
            std::vector<Child> children;
            std::map<juce::String, int> numUnkeyed;
            std::set<juce::String> names;

            for (const auto& child : element)
            {
                if (! isOrdered (element, child))
                    continue;

                const auto type = child.getType().toString();
                const auto key = schema::getElementKey (child);
                auto name = type + ":" + key;

                if (key.isEmpty() || names.count (name) > 0)
                    name = type + "#" + juce::String (numUnkeyed[type]++);

                names.insert (name);
                children.push_back ({ child, name });
            }

            return children;
        };

        const auto wanted = getOrdered (other);
        std::set<juce::String> wantedNames;

        for (const auto& child : wanted)
            wantedNames.insert (child.name);

        // The song's own of each name the other has; those of a name the other hasn't take no place, left where they are.
        std::map<juce::String, juce::ValueTree> held;
        std::vector<juce::ValueTree> notWanted;

        for (const auto& child : getOrdered (live))
        {
            if (wantedNames.count (child.name) > 0)
                held.emplace (child.name, child.tree);
            else
                notWanted.push_back (child.tree);
        }

        const auto takesPlace = [&isOrdered, &live, &notWanted] (const juce::ValueTree& child)
        { return isOrdered (live, child) && std::find (notWanted.begin(), notWanted.end(), child) == notWanted.end(); };

        int place = 0;

        for (const auto& child : wanted)
        {
            while (place < live.getNumChildren() && ! takesPlace (live.getChild (place)))
                ++place;

            const auto found = held.find (child.name);

            if (found == held.end())
                continue;

            if (const auto index = live.indexOf (found->second); index != place)
                live.moveChild (index, place, undo);

            ++place;
        }
    }

    // The song's count of IDs brought past every ID it holds now and past the other's count: what it took from the other
    // can hold IDs past its own count (a song opened again as it was saved before they were given), and the next item it
    // makes gets an ID that nothing of either has. The other's IDs counted in its whole tree, its clips' too. Not an undo
    // step, as the count never is.
    void countIDsPast (Song& song, const juce::ValueTree& other)
    {
        countIDsFrom (song, std::max (findNextID (song), findNextID (other)));
    }

    // The tracks both songs have put where the other has them: each in the folder the other has it in, where the song holds
    // that folder (else in the track list), right after the track before it there that the song holds there
    // (findTrackPlaceLike()), in the other's order (depth first: a folder put in its place before the tracks in it, so that
    // none goes into a folder in itself), each as the same element (Tracks.h: setTrackPlace(), which writes nothing for one
    // where it goes already; the other areas' keepers carry what it holds while the song's tree is the tracks' storage). One
    // the other's version couldn't read (a stand-in) too: the song's, in its place among the other's. How many it moved. The
    // song's own line's (makeSongOwnLikeIn()), and the whole song's (makeTracksLike()).
    int makeTrackOrderLike (Song& song, const juce::ValueTree& other)
    {
        auto writes = 0;

        for (const auto& there : getTrackElements (other))
        {
            const auto id = TrackID::fromRaw (readID (there[schema::trackID]));

            if (! holdsTrack (song, id))
                continue;

            auto folder = getFolderThere (there);

            if (folder.isValid() && ! holdsFolder (song, folder))
                folder = {};

            const auto tracks = getHeldTracks (song);

            if (setTrackPlace (song, id, folder, findTrackPlaceLike (tracks, folder, id, there)))
                ++writes;
        }

        return writes;
    }

    // The tracks that came into the song in an apply: each a track of the other's that the song didn't hold (an ID it didn't
    // hold before the apply: newToSong), put in the song as an element of its own (elements), which the switches' and the
    // view's pass gives the other's switches and view (makeTrackLeavesLike()). Since the tracks' pass writes through the
    // tracks' setters (roadmap step S5.20), two kinds of track the passes over elements counted as come in are not: a track
    // a version moves to another folder, and the tracks inside a folder a version moves, which the song held and which stay
    // the same elements. Where the song kept no row for one, it keeps the default view; before, they took the version's.
    struct TracksCameIn
    {
        std::set<std::uint64_t> elements {};
        std::set<std::uint64_t> newToSong {};
    };

    // What the tracks' pass did: how many writes it made, and the tracks it brought into the song; and what the song's tracks
    // are to be once the whole song's passes are done (holdsTracksAsRead()): the other's as the pass read them (the read, in
    // their order, and the master), but those it couldn't read (standingIn: a stand-in's ID, the master's a stand-in).
    struct TracksMade
    {
        int writes = 0;
        TracksCameIn cameIn {};
        std::vector<TrackValue> read {};
        std::optional<TrackValue> master {};
        std::set<std::uint64_t> standingIn {};
    };

    // The IDs a whole copy holds of items that aren't tracks: each element's own ID (Schema.h: holdsID()) but a TRACK's and
    // the MASTER's (theirs and their faders' and VCAs' are the tracks'), a note's (its clip's notes count their own) and an
    // alias group's (clipLink: its clips share it). What the tracks' pass asks before it puts a track in the song whose ID the
    // song holds as another area's item (makeTracksLike()).
    std::set<std::uint64_t> findItemIDsButTracks (const juce::ValueTree& whole)
    {
        std::set<std::uint64_t> ids;
        const std::function<void (const juce::ValueTree&)> visit = [&ids, &visit] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::NOTE))
                    continue;

                if (! child.hasType (schema::TRACK) && ! child.hasType (schema::MASTER))
                    for (int i = 0; i < child.getNumProperties(); ++i)
                        if (const auto name = child.getPropertyName (i); name != schema::clipLink && schema::holdsID (child, name))
                            ids.insert (readID (child[name]));

                visit (child);
            }
        };

        visit (whole);
        return ids;
    }

    // The whole song's tracks, folders, returns and master made like the other's (their own fields, which folder each is in
    // and its place there: TracksValue.h, TrackValue), through the tracks' setters (model/arrangement/internal/Tracks.h:
    // setTrack(), setNewTracks(), setTrackPlace(), eraseTrack()), in the mode of the moment, each writing nothing where the
    // song holds what it would set: the first of the passes of what the song holds beside its tree, as a track must be in the
    // song before the clip pass gives it its clips (ClipPass: addTrack()). The song's tracks as its area holds them
    // (findHeldTrack(), getHeldTracks(): none read), the other's read once each, as the song holds one (readHeldTrack()). In
    // this order, so that what goes frees its IDs before what comes takes them (a merge gives IDs anew):
    //   1. a track of both made a folder, where the other has it as one;
    //   2. a track of both in a folder the other hasn't put at the end of the track list, as the same element, so that it
    //      stays as the folder goes (it goes where the other has it in step 6);
    //   3. each the other hasn't (a folder with what is left in it) taken away, what it holds in the song's areas erased first
    //      (Tracks.h: eraseTrackHoldings()), so that nothing is left to a keeper; but one in a folder the other's version
    //      couldn't read (a stand-in), whose piece held the tracks in it, which stays as it is;
    //   4. each of both given the other's fields (setTrack()), in the folder it is in now, and the master its fields, its ID
    //      too (a version may give the master another: makeLike() gave the MASTER the other's ID as the same element by its
    //      kind);
    //   5. each the song hasn't added at its place (findTrackPlaceLike()), a new folder with the new tracks in it, in one
    //      action (setNewTracks()), bare: its clips, devices, modulators, sends, switches and view are the later passes' to
    //      give. One whose ID the song holds as another area's item (a device of the same number, which two lines that
    //      counted their IDs on from the same place can each have made) is put in all the same (IDsHeldElsewhere::
    //      goWithTheirAreas) where the other holds no item but tracks under any of the group's IDs (findItemIDsButTracks()):
    //      that area's pass, later, makes it the other's, which hasn't that item, so the song holds the ID twice only
    //      meanwhile. What the passes in between look up by an ID takes no item of one kind for another's: the clips' pass
    //      finds a clip by its ID among the clips and a track among the tracks; the devices' pass a device among the devices
    //      and its channel among the tracks, and the store's entries by a device's ID; the curves', the MIDI links' and the
    //      sections' passes set their values whole from the other's, finding no parameter's owner; modulation's an owner by
    //      its kind and ID (OwnerKey); and the switches' and the view's keys, the tracks', are read after every pass;
    //   6. the tracks both have put where the other has them (makeTrackOrderLike()), each as the same element, with all it
    //      holds: a plug-in on it keeps its instance;
    //   7. step 4 again for what the song couldn't hold until now (a folder made a track once the tracks in it went).
    // What the other's version couldn't read (a stand-in: the element, or its name among standIns) stays as the song holds it,
    // in its place among the other's (step 6); and where what is counted is taken, a field the descriptions count as no change
    // (getUncountedFields(): none) stays the song's. What it wrote, and the tracks it brought in, each new to the song.
    TracksMade makeTracksLike (Song& song, const juce::ValueTree& other, Taken taken, const StandIns& standIns)
    {
        struct Theirs
        {
            TrackID id {};
            juce::ValueTree element {};
            std::optional<TrackValue> value {}; // none for a stand-in, and one that isn't read
        };

        std::vector<Theirs> theirs;
        std::set<std::uint64_t> inOther, standingIn;

        for (const auto& element : getTrackElements (other))
        {
            const auto id = TrackID::fromRaw (readID (element[schema::trackID]));

            if (! id.isValid() || ! inOther.insert (id.getRaw()).second)
                continue;

            Theirs read { .id = id, .element = element };

            if (isStandIn (element) || standIns.count (nameOf (element)) > 0)
                standingIn.insert (id.getRaw());
            else if (auto value = readHeldTrack (element); value.has_value() && value->id == id)
                read.value = std::move (value);

            theirs.push_back (std::move (read));
        }

        TracksMade made;
        const auto held = getHeldTracks (song);

        // Step 3's tracks, as the song holds them now: each the other hasn't in the track list or in a folder the other has (a
        // folder the other hasn't takes what is left in it); and step 2's, each of both in a folder the other hasn't: none in a
        // stand-in.
        std::vector<TrackID> gone, inGone;

        for (const auto& [raw, value] : held.tracks)
        {
            auto inStandIn = false;

            for (auto folder = value.parent; folder.isValid() && ! inStandIn;)
            {
                inStandIn = standingIn.count (folder.getRaw()) > 0;
                const auto* above = held.tracks.find (folder.getRaw());
                folder = above != nullptr ? above->parent : TrackID();
            }

            const auto inFolderGone = value.parent.isValid() && inOther.count (value.parent.getRaw()) == 0;

            if (inStandIn)
                continue;

            if (inOther.count (raw) == 0 && ! inFolderGone)
                gone.push_back (value.id);
            else if (inOther.count (raw) > 0 && inFolderGone)
                inGone.push_back (value.id);
        }

        const auto byID = [] (TrackID a, TrackID b) { return a.getRaw() < b.getRaw(); };
        std::sort (gone.begin(), gone.end(), byID);
        std::sort (inGone.begin(), inGone.end(), byID);

        // Each of both given the other's fields where they differ, in the folder it is in now; of those alone that the other has
        // as a folder and the song hasn't (kindsAlone: step 1). How many it wrote.
        const auto setFields = [&song, &theirs, taken] (bool kindsAlone)
        {
            auto writes = 0;

            for (auto& each : theirs)
            {
                const auto now = each.value.has_value() ? findHeldTrack (song, each.id) : std::nullopt;

                if (! now.has_value() || now->isMaster() || (kindsAlone && (! each.value->isFolder() || now->isFolder())))
                    continue;

                keepUncounted (*each.value, *now, taken);
                auto value = *each.value;
                value.parent = now->parent;

                if (setTrack (song, std::move (value)))
                    ++writes;
            }

            return writes;
        };

        // 1. A track of both the other has as a folder.
        made.writes += setFields (true);

        // 2. A track of both in a folder the other hasn't, out of it.
        for (const auto id : inGone)
            if (setTrackPlace (song, id, {}, -1))
                ++made.writes;

        // 3. Each the other hasn't.
        eraseTrackHoldings (song, gone);

        for (const auto id : gone)
            if (eraseTrack (song, id))
                ++made.writes;

        // 4. Each of both given the other's fields; and the master.
        made.writes += setFields (false);

        if (const auto then = other.getChildWithName (schema::MASTER); then.isValid() && ! isStandIn (then))
        {
            if (auto value = readHeldTrack (then); value.has_value() && value->isMaster())
            {
                keepUncounted (*value, getHeldTracks (song).master, taken);
                made.master = *value;

                if (setTrack (song, std::move (*value)))
                    ++made.writes;
            }
        }

        // 5. Each the song hasn't, at its place, with the tracks in it that the song hasn't either; put in where the song holds
        // one of their IDs as another area's item, where the other holds none of them but as tracks (the other's IDs read once,
        // as the first group comes).
        std::optional<std::set<std::uint64_t>> otherItems;

        const auto whereHeldElsewhere = [&otherItems, &other] (const std::vector<TrackValue>& group)
        {
            if (! otherItems.has_value())
                otherItems = findItemIDsButTracks (other);

            for (const auto& value : group)
                for (const auto id : { value.id.getRaw(), value.fader.getRaw(), value.vca.getRaw() })
                    if (id != 0 && otherItems->count (id) > 0)
                        return IDsHeldElsewhere::refused;

            return IDsHeldElsewhere::goWithTheirAreas;
        };

        for (size_t i = 0; i < theirs.size(); ++i)
        {
            const auto& each = theirs[i];

            if (! each.value.has_value() || made.cameIn.newToSong.count (each.id.getRaw()) > 0 || hasTrack (song, each.id))
                continue;

            const auto folder = each.value->parent;

            if (folder.isValid() && ! holdsTrack (song, folder))
                continue; // in a folder the song couldn't hold: not there

            std::vector<TrackValue> group { *each.value };
            std::set<std::uint64_t> inGroup { each.id.getRaw() };

            for (auto j = i + 1; j < theirs.size(); ++j)
            {
                if (const auto& inside = theirs[j];
                    inside.value.has_value() && inGroup.count (inside.value->parent.getRaw()) > 0 && ! hasTrack (song, inside.id))
                {
                    group.push_back (*inside.value);
                    inGroup.insert (inside.id.getRaw());
                }
            }

            const auto tracks = getHeldTracks (song);

            if (! setNewTracks (song, group, findTrackPlaceLike (tracks, folder, each.id, each.element), whereHeldElsewhere (group)))
                continue;

            ++made.writes;

            for (const auto id : inGroup)
            {
                made.cameIn.elements.insert (id);
                made.cameIn.newToSong.insert (id);
            }
        }

        // 6. Where the other has each; 7. what the song couldn't hold before.
        made.writes += makeTrackOrderLike (song, other);
        made.writes += setFields (false);

        // What the song's tracks are to be once the passes are done.
        made.standingIn = std::move (standingIn);

        for (auto& each : theirs)
        {
            if (each.value.has_value())
                made.read.push_back (std::move (*each.value));
            else
                made.standingIn.insert (each.id.getRaw()); // and one it couldn't read
        }

        return made;
    }

    // Whether the song holds the tracks the tracks' pass read of the other (TracksMade), once the whole song's passes are done:
    // each the song holds as it read it (its folder too), the master too, and no other but one the other's version couldn't
    // read (a stand-in), or one in a folder that is one. What the whole song's passes assert, so that no track the setters
    // refused is left unseen (makeSongLike(), makeSongTheSame(), makeSongAsCopied()).
    [[maybe_unused]] bool holdsTracksAsRead (const Song& song, const TracksMade& made)
    {
        const auto tracks = getHeldTracks (song);

        if (made.master.has_value() && ! (tracks.master == *made.master))
            return false;

        std::set<std::uint64_t> read;

        for (const auto& value : made.read)
        {
            const auto* held = tracks.find (value.id);

            if (held == nullptr || ! (*held == value))
                return false;

            read.insert (value.id.getRaw());
        }

        for (const auto& [raw, value] : tracks.tracks)
        {
            auto accounted = read.count (raw) > 0;

            for (auto each = value.id; each.isValid() && ! accounted;)
            {
                accounted = made.standingIn.count (each.getRaw()) > 0;
                const auto* above = tracks.find (each);
                each = above != nullptr ? above->parent : TrackID();
            }

            if (! accounted)
                return false;
        }

        return true;
    }
    // The tracks' switches and view made like the other's (TrackSwitchesValue.h, TracksOutside.h), once the tracks' pass put
    // the tracks where the other has them (makeTracksLike()), for the tracks a pass takes (which: by their IDs) and the master's
    // where a pass takes it: each of them the song holds and the other holds, not a stand-in (what the other's version
    // couldn't read stays as the song has it). The other's element read as the song holds it (readHeldSwitchesOf(),
    // readHeldViewOf()), its entries replacing the song's of its key; through their setters, each value set once, nothing
    // where they hold the same. Where everything is taken (switching in place, the song as copied), the switches in the mode
    // of the moment (outside the history: the song is written outside it) and the view whole, the master's too. Where what
    // is counted is taken (a version applied in the history, a line put back):
    //   - the switches in the step (its Undo puts back what the song held), but those of a track new to the song (cameIn:
    //     its ID not in the song before the apply) that the switches hold no entry of (TrackSwitchesValue.h:
    //     holdsSwitchesOf()), which are written outside the history, as a maker gives a new item's (a duplicate's copy:
    //     model/arrangement/Tracks.cpp, copyTrackLeaves()): the step's Undo takes the track away and its Redo brings it back
    //     with what was written for it since (unmuted by hand). A track a step took away that kept an entry gets them in the
    //     step: written outside, that step's Undo would bring the old track back with the version's switches.
    //   - the view of a track that came into the song (cameIn: a track the pass put in as an element of its own) where the
    //     song keeps none of its ID; one it keeps (the track was in the song before: its row as it last was) stays, and so
    //     does every other track's and the master's.
    // Whether it wrote (no write the history alone holds: the switches are written outside it too).
    bool makeTrackLeavesLike (Song& song, const juce::ValueTree& other, const std::function<bool (std::uint64_t)>& which, bool master,
                              Taken taken, const TracksCameIn& cameIn)
    {
        auto* values = song.getValues();

        if (values == nullptr)
            return false;

        // The elements of the tracks it takes, each new one without an entry in the switches first, outside the history.
        std::vector<juce::ValueTree> inStep, outsideHistory;
        const auto held = values->trackSwitches.get();
        auto outside = values->tracksOutside.get();

        for (const auto& then : getTrackElements (other))
        {
            const auto id = readID (then[schema::trackID]);

            if (! which (id) || isStandIn (then) || ! holdsTrack (song, TrackID::fromRaw (id)))
                continue;

            const auto isNew = taken == Taken::counted && cameIn.newToSong.count (id) > 0 && ! holdsSwitchesOf (held, id);
            (isNew ? outsideHistory : inStep).push_back (then);

            if (taken == Taken::everything || (cameIn.elements.count (id) > 0 && values->tracksOutside.get().tracks.find (id) == nullptr))
                outside = withTrackView (outside, id, readHeldViewOf (then));
        }

        if (const auto then = other.getChildWithName (schema::MASTER); master && then.isValid() && ! isStandIn (then))
        {
            inStep.push_back (then);

            if (taken == Taken::everything)
                outside = withMasterView (outside, readHeldViewOf (then));
        }

        auto newSwitches = held;

        for (const auto& then : outsideHistory)
            readHeldSwitchesOf (then, newSwitches);

        const auto wroteNew = values->setSwitchesOutsideHistory (song, std::move (newSwitches));
        auto switches = values->trackSwitches.get();

        for (const auto& then : inStep)
            readHeldSwitchesOf (then, switches);

        const auto wroteSwitches = values->setSwitches (song, std::move (switches));
        const auto wroteView = values->setTracksOutside (std::move (outside));
        return wroteNew || wroteSwitches || wroteView;
    }

    bool isEveryTrack (std::uint64_t)
    {
        return true;
    }
} // namespace

void makeSongLike (Song& song, const juce::ValueTree& other)
{
    jassert (other != song.getState()); // a whole copy (WholeState.h), never the song's live tree, which holds no track
    const NothingTakenIn byThePassesAlone (song);
    auto* undo = &song.getUndoManager();
    const auto standIns = findStandIns (other);
    const auto tracks = makeTracksLike (song, other, Taken::counted, standIns);
    makeLike (song, song.getState(), other, undo, Taken::counted, standIns);
    makeValuesLike (song, other, true, Taken::counted, standIns);
    makeTrackLeavesLike (song, other, isEveryTrack, true, Taken::counted, tracks.cameIn);
    placePlaceholdersLike (song, other, undo, Taken::counted);
    countIDsPast (song, other);

    // The song's tracks the other's, and no ID held twice: what the tracks' pass put in beside another area's item of its ID
    // (makeTracksLike()), that area's pass took out.
    jassert (holdsTracksAsRead (song, tracks));
    jassert (findIDHeldTwice (song) == 0);
}

void makeSongTheSame (Song& song, const juce::ValueTree& other)
{
    jassert (other != song.getState()); // as makeSongLike()'s
    const NothingTakenIn byThePassesAlone (song);

    // Outside the history, as what the song keeps beside its tree is written too (Song::OutsideHistory).
    const Song::OutsideHistory outside (song);

    // Its count of IDs the larger of the two: what either branch made keeps an ID of its own.
    const auto next = std::max (findNextID (song), findNextID (other));
    [[maybe_unused]] const auto tracks = makeTracksLike (song, other, Taken::everything, noStandIns());
    makeLike (song, song.getState(), other, nullptr, Taken::everything, noStandIns());
    makeValuesLike (song, other, false, Taken::everything, noStandIns());
    makeTrackLeavesLike (song, other, isEveryTrack, true, Taken::everything, {});
    placePlaceholdersLike (song, other, nullptr, Taken::everything);
    countIDsFrom (song, next);
    song.getUndoManager().clearUndoHistory();
    jassert (holdsTracksAsRead (song, tracks)); // as makeSongLike()'s
    jassert (findIDHeldTwice (song) == 0);
}

void makeSongAsCopied (Song& song, const juce::ValueTree& copy)
{
    jassert (copy != song.getState()); // as makeSongLike()'s
    const NothingTakenIn byThePassesAlone (song);
    const Song::OutsideHistory outside (song);

    // Everything, outside the history, and its count of IDs as the copy's: what the history holds is the copy's already, so
    // that only what changed without it differs (a property set without a step, view state, the count of IDs). Its clips, its
    // tempo map, its chord lane, its curves and its devices among what the history holds: every change of one is an undo step,
    // so none differs (an Undo of a step below would lose what was written of one here: ValueArea.h); the passes write nothing
    // of them, nor of the tracks' own fields and places, which only a step changes. What the devices' store holds (a plug-in's
    // state stored, a chain soloed) and a window's place are written outside the history too, and taken back here as they
    // differ.
    [[maybe_unused]] const auto tracks = makeTracksLike (song, copy, Taken::everything, noStandIns());
    makeLike (song, song.getState(), copy, nullptr, Taken::everything, noStandIns());
    [[maybe_unused]] const auto valueWrites = makeValuesLike (song, copy, false, Taken::everything, noStandIns());
    jassert (tracks.writes + valueWrites == 0);
    makeTrackLeavesLike (song, copy, isEveryTrack, true, Taken::everything, {}); // written outside the history too
    placePlaceholdersLike (song, copy, nullptr, Taken::everything);
    song.getState().setProperty (schema::nextID, copy[schema::nextID], nullptr);
    jassert (holdsTracksAsRead (song, tracks)); // as makeSongLike()'s
    jassert (findIDHeldTwice (song) == 0);
}

namespace
{
    // A line's track as each song holds it, and what the line gives the song of the other's tracks, worked out before anything
    // is written (makeTrackLike()), each read once as the song holds one (readHeldTrack()): the line's track where both hold it
    // (its value, the master's for the master's line), or where the song hasn't it, the line's track with the tracks in it the
    // song holds nowhere, in the other's order, each with its element (elements); nothing where the other hasn't it, or
    // couldn't read it (a stand-in).
    struct LineTracks
    {
        TrackID id {};
        std::optional<TrackValue> live {};
        juce::ValueTree then {};
        std::vector<TrackValue> given {};
        std::vector<juce::ValueTree> elements {};
    };

    LineTracks readLineTracks (const Song& song, TrackID id, const juce::ValueTree& other)
    {
        // The song's as the tracks' keeper holds it (no element read), the other's element in its whole copy.
        LineTracks line { .id = id, .live = findHeldTrack (song, id), .then = findTrackIn (other, id.getRaw()) };

        // The master's line by its kind: the master in each song, whichever of their IDs for it the line names (a version may
        // give the master another ID), never taken for one the other hasn't, added since and taken away, or gone since and
        // added beside it.
        if ((line.live.has_value() && line.live->isMaster()) || line.then.hasType (schema::MASTER))
        {
            line.live = getHeldTracks (song).master;
            line.then = other.getChildWithName (schema::MASTER);
            line.id = line.live->id;
        }

        if (! line.then.isValid() || isStandIn (line.then))
            return line;

        if (line.live.has_value())
        {
            if (auto value = readHeldTrack (line.then); value.has_value() && value->isMaster() == line.live->isMaster())
                line.given.push_back (std::move (*value));

            return line;
        }

        // Gone since: the tracks it held that the song holds nowhere now come back with it (one it holds stays where it is,
        // with what is in it); not what the other's version couldn't read (a stand-in), which isn't in the song now either.
        const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& element)
        {
            auto value = readHeldTrack (element);

            if (! value.has_value() || value->isMaster())
                return;

            line.given.push_back (std::move (*value));
            line.elements.push_back (element);

            for (const auto& child : element)
                if (child.hasType (schema::TRACK) && ! isStandIn (child)
                    && ! hasTrack (song, TrackID::fromRaw (readID (child[schema::trackID]))))
                    collect (child);
        };

        collect (line.then);
        return line;
    }

    // Whether a line would give a track an ID (its own, its fader's or its VCA's) that the song holds now for another item than
    // the line's track as it is: another track, a fader or a VCA, a device, a container or a chain, a send, a modulator or a
    // macro, a clip, a section (IDsHeld.h: forEachItemIDHeld()). Such an item stays where the line's passes don't reach it (a
    // device on another track), and the song would hold the ID twice; refused whether the line's passes would take that item
    // away or not, which only the whole song's passes can tell (makeTracksLike()). Two lines that counted their IDs on from the
    // same place make it (another computer's branch switched to or restored). Nothing walked where the line gives the song's
    // own IDs alone (a track of both, its fader and VCA as they are).
    bool givesIDsHeldElsewhere (const Song& song, const LineTracks& line)
    {
        std::set<std::uint64_t> own, wanted;

        if (line.live.has_value())
            own = { line.live->id.getRaw(), line.live->fader.getRaw(), line.live->vca.getRaw() };

        for (const auto& value : line.given)
            for (const auto id : { value.id.getRaw(), value.fader.getRaw(), value.vca.getRaw() })
                if (id != 0 && own.count (id) == 0)
                    wanted.insert (id);

        if (wanted.empty())
            return false;

        auto held = false;
        forEachItemIDHeld (song, [&wanted, &held] (std::uint64_t id) { held = held || wanted.count (id) > 0; });
        return held;
    }

    bool makeTrackLikeIn (Song& song, LineTracks line, const juce::ValueTree& other)
    {
        const auto id = line.id;
        const auto& live = line.live;
        const auto& then = line.then;

        if (! live.has_value() && ! then.isValid())
            return false;

        // A track the other's version couldn't read stays as it is.
        if (then.isValid() && isStandIn (then))
            return true;

        // Its devices' automation, of what either of them holds: its fader's and VCA's, its devices', its sends' levels', and its
        // modulators' and macros' (its containers' macros included), the song's read by its ID from where the song keeps each
        // (Tracks.h: getAutomatableIDs()), the other's from its whole copy's elements; the master's line the master's.
        const auto isMaster = live.has_value() ? live->isMaster() : then.hasType (schema::MASTER);
        auto devices = getAutomatableIDs (song, id);
        devices.merge (getAutomatableIDs (then));

        // Its clips, and those of the tracks that come back with it, in a pass of their own once the rest is done: what the
        // track holds that the other song holds on another track goes there, as it is now. And the tracks whose devices,
        // modulators, macros and sends the line carries (the devices pass's, the modulation pass's and the sends pass's,
        // below): its own, and those that come back with it; the master's line the master's.
        ClipPass clips (song, other, ClipPass::Elsewhere::sentThere, true);
        std::set<std::uint64_t> lineTracks { id.getRaw() };
        const auto isLineTrack = [&lineTracks] (TrackID track) { return lineTracks.count (track.getRaw()) > 0; };
        const auto isLineChannel = [isMaster, &lineTracks] (const DevicePlace& place)
        { return isMaster ? place.onMaster : ! place.onMaster && lineTracks.count (place.track.getRaw()) > 0; };
        const auto standIns = then.isValid() ? findStandIns (then) : StandIns();
        auto devicesDone = false;
        TracksCameIn cameBack; // the tracks that came back with it, new to the song: their switches and view from the other

        if (live.has_value() && then.isValid())
        {
            // Its own fields, the other's, read as the song holds them (the master's ID too), through the tracks' setter
            // (Tracks.h: setTrack()), which writes nothing where they are the song's already; where it is (its folder, its place
            // there) the song's own line's (makeSongOwnLikeIn()), as a line always left it. What it held then that is
            // somewhere else now (a device moved into a container on another track) comes back from there, and what it holds
            // now that the other song holds somewhere else goes there, by the devices pass (below).
            if (! line.given.empty())
            {
                auto value = std::move (line.given.front());
                value.parent = live->parent;
                keepUncounted (value, *live, Taken::counted);
                setTrack (song, std::move (value));
            }

            clips.addTrack (id);
        }
        else if (live.has_value())
        {
            // Added since: taken away. The tracks in it (a folder's) are things of their own, which stay: where it was, in
            // their order, as ungrouping a folder leaves them, each as the same element (Tracks.h: setTrackPlace()). And what it
            // holds that the other song holds somewhere else (a clip moved onto it, a device moved into a container on another
            // track) goes there first; the rest of its devices with it.
            clips.sendHome (id);
            makeDevicesLike (song, other, isLineChannel, standIns, Taken::counted);
            devicesDone = true;
            makeSendsLike (song, other, isLineTrack); // its sends the other holds on another track, sent there first

            // The song's tracks as they are now, a value of the pass's own: the list walked is its, which the moves below
            // don't free (each makes the song's next value, not this one).
            const auto tracks = getHeldTracks (song);
            auto place = findIndexIn (tracks, live->parent, id) + 1;

            if (const auto* inside = tracks.findList (id))
                for (const auto track : *inside)
                    setTrackPlace (song, track, live->parent, place++);

            // What it holds itself in the song's areas, its tracks out of it now, erased before it goes (Tracks.h:
            // eraseTrackHoldings()): the keeper of the song's values finds nothing to remember. Then a clip on another track
            // left the only one of its alias group (an alias of one of its clips) leaves the group, as deleteTracks() unlinks it.
            eraseTrackHoldings (song, { id });
            eraseTrack (song, id);
            unlinkLoneClips (song);
        }
        else
        {
            // Gone since: back where it was, after the track before it there, with the tracks it held that the song holds
            // nowhere now (one it holds stays where it is, with what is in it), in one action, bare (Tracks.h: setNewTracks()),
            // each read once as the song holds one (readLineTracks()); and what they held that is elsewhere now back from there
            // (their devices by the devices pass, below); without what the other's version couldn't read (a stand-in), which
            // isn't in the song now either.
            auto& made = line.given;
            const auto& elements = line.elements;

            if (! made.empty())
            {
                // In the folder the other has it in, where the song holds that folder; else in the track list.
                auto folder = getFolderThere (then);

                if (folder.isValid() && ! holdsFolder (song, folder))
                    folder = {};

                made.front().parent = folder;
                const auto tracks = getHeldTracks (song);
                setNewTracks (song, made, findTrackPlaceLike (tracks, folder, id, then));
            }

            // Its clips, and those of the tracks in it that came back with it; and their devices, modulators and macros, which
            // the values put in hold none of; and their automation, as the other song's elements of them hold what it is of.
            for (const auto& element : elements)
            {
                const auto track = TrackID::fromRaw (readID (element[schema::trackID]));
                clips.addTrack (track);
                lineTracks.insert (track.getRaw());
                devices.merge (getAutomatableIDs (element));
            }

            cameBack.elements = lineTracks;
            cameBack.newToSong = lineTracks;
        }

        // Its devices, containers and chains and what the store holds of them, and those of the tracks that came back with it,
        // once the tracks are where the other has them, and before the curves' and modulation's passes (a container's macros
        // need their container): as the line always carried a channel's DEVICE and CONTAINER elements, what it holds that the
        // other holds on another track sent there as it is now.
        if (! devicesDone)
            makeDevicesLike (song, other, isLineChannel, standIns, Taken::counted);

        clips.run();
        makeCurvesLike (
            song, other, [&devices] (const ParameterRef& parameter) { return devices.count (parameter.device.getRaw()) > 0; }, false);

        // Its modulators and macros, and those of the containers on it, as the line always carried them (its own and its
        // containers' children), and those of the tracks that came back with it (a folder's, gone since): the master's line
        // the master's.
        makeModulationLike (
            song, other, [isMaster, &lineTracks] (const ModulatorValue& value)
            { return isMaster ? value.place.onMaster : ! value.place.onMaster && lineTracks.count (value.place.track.getRaw()) > 0; });

        // Its sends, and those of the tracks that came back with it, as the line always carried a track's SEND elements: the
        // master's line none.
        if (! isMaster)
            makeSendsLike (song, other, isLineTrack);

        // Its switches, and those of the tracks that came back with it, with their view where the song keeps none: the
        // master's line the master's mode.
        makeTrackLeavesLike (
            song, other, [isMaster, &lineTracks] (std::uint64_t track) { return ! isMaster && lineTracks.count (track) > 0; }, isMaster,
            Taken::counted, cameBack);

        return true;
    }

    void makeSongOwnLikeIn (Song& song, juce::ValueTree state, const juce::ValueTree& other, juce::UndoManager* undo)
    {
        // Its own properties.
        for (int i = state.getNumProperties(); --i >= 0;)
            if (const auto name = state.getPropertyName (i); ! schema::isUncountedProperty (state, name) && ! other.hasProperty (name))
                state.removeProperty (name, undo);

        for (int i = 0; i < other.getNumProperties(); ++i)
            if (const auto name = other.getPropertyName (i); ! schema::isUncountedProperty (other, name))
                if (const auto* value = state.getPropertyPointer (name); value == nullptr || ! isSameValue (*value, other[name]))
                    state.setProperty (name, other[name], undo);

        // Its tempo map, through the tempo pass; its chord lane, through the lane pass; its sections, through the sections pass;
        // the key set last: each as the other has it, or none.
        makeTempoMapLike (song, other);
        makeChordLaneLike (song, other);
        makeSectionsLike (song, other);

        for (const auto* type : { &schema::LASTKEY })
        {
            auto live = state.getChildWithName (*type);
            const auto then = other.getChildWithName (*type);

            // One the other's version couldn't read (a stand-in, which only a piece that isn't Phazite's can make of these)
            // stays as it is.
            if (then.isValid() && isStandIn (then))
                continue;

            if (live.isValid() && then.isValid())
                makeLike (song, live, then, undo, Taken::counted, findStandIns (then));
            else if (live.isValid())
                state.removeChild (live, undo);
            else if (then.isValid())
                state.appendChild (copyForSong (then), undo);
        }

        // The tracks both have, in the other's order: in its folders, and in its order there, each as the same element.
        makeTrackOrderLike (song, other);
    }
} // namespace

bool makeTrackLike (Song& song, TrackID id, const juce::ValueTree& other)
{
    const NothingTakenIn byThePassesAlone (song);
    auto line = readLineTracks (song, id, other);

    // Refused before anything is written, the song's count of IDs too.
    if (givesIDsHeldElsewhere (song, line))
        return false;

    const auto done = makeTrackLikeIn (song, std::move (line), other);
    countIDsPast (song, other);
    return done;
}

bool givesTrackIDsHeldElsewhere (const Song& song, TrackID id, const juce::ValueTree& other)
{
    return givesIDsHeldElsewhere (song, readLineTracks (song, id, other));
}

void makeSongOwnLike (Song& song, const juce::ValueTree& other)
{
    const NothingTakenIn byThePassesAlone (song);
    makeSongOwnLikeIn (song, song.getState(), other, &song.getUndoManager());
    countIDsPast (song, other);
}

int removeBrokenReferences (Song& song)
{
    // The IDs of what the song holds that something can name: its tracks' (each track's, its fader's and its VCA's, and the
    // master's), from what the tracks' keeper holds (no element read), and those its other areas hold (IDsHeld.h:
    // forEachNameableIDHeld(): a modulator's, a macro's, a send's and a device's).
    const auto held = getHeldTracks (song);
    std::set<std::uint64_t> items, tracks;
    forEachIDHeld (held, [&items] (std::uint64_t id) { items.insert (id); });

    for (const auto& entry : held.tracks)
        tracks.insert (entry.first);

    forEachNameableIDHeld (song, [&items] (std::uint64_t id) { items.insert (id); });
    std::vector<DeviceID> sidechains;

    for (const auto& [id, device] : getDevicesValue (song).devices)
        if (device.sidechain.isValid() && tracks.count (device.sidechain.getRaw()) == 0)
            sidechains.push_back (device.id);

    // A modulation of a parameter of an item the song hasn't, through modulation's code.
    const auto modulations = (size_t) forgetModulationsOf (song, [&items] (DeviceID device) { return items.count (device.getRaw()) == 0; });

    // A sidechain that listens to a track the song hasn't turned off through the devices' setter, by its device's ID
    // (Mixer.h: setDeviceFields()).
    for (const auto device : sidechains)
        setDeviceFields (song, device, [] (DeviceValue& value) { value.sidechain = {}; });

    // And what an edit takes away with what it names (model/mixer/internal/Mixer.h): a send to a return that is gone, with
    // what modulated it, and a pad's track whose drum sampler is gone; so that the song is as makeConsistent() leaves it.
    // Then a sidechain that would hear the device's own sound round a loop, which two lines' changes can make together (one
    // a sidechain, the other a send), as an edit that makes one lets it go (Sidechain.h: forgetSidechainLoops()).
    // The song's sends, as its area holds them (SendsValue.h: getSendsValue()).
    const auto countSends = [&song] { return (int) getSendsValue (song).sends.size(); };

    // The devices that listen to a sidechain, as the devices' area holds them.
    const auto countSidechains = [&song]
    {
        const auto& devices = getDevicesValue (song).devices;
        return (int) std::count_if (devices.begin(), devices.end(), [] (const auto& each) { return each.second.sidechain.isValid(); });
    };

    const auto sendsBefore = countSends();
    forgetWhatNamesTheGone (song);
    const auto sidechainsBefore = countSidechains();
    forgetSidechainLoops (song);

    return (int) (modulations + sidechains.size()) + sendsBefore - countSends() + sidechainsBefore - countSidechains();
}

std::uint64_t countDeviceStorePassWrites() noexcept
{
    return storePassWrites.load (std::memory_order_relaxed);
}
} // namespace phazite
