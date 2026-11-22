// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ApplySong.h"
#include "internal/SongDiff.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/IDsHeld.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/chords/internal/Chords.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>
#include <cstddef>
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

    // A copy of an element of the other song, to be added to the song, without what in it the other's version couldn't read
    // (the stand-ins, and the elements that stand in for themselves: a clip a run of whose notes was lost), and without its
    // tracks' clips, which the clip pass gives them (ClipPass, below), nor the tempo map's entries, which the tempo pass
    // gives the song (makeTempoMapLike()), nor the chord lane's, which the lane pass gives it (makeChordLaneLike()), nor
    // the MIDI links, which the links pass gives it (makeMidiLinksLike()), nor its AUTOMATION's curves, which the curves
    // pass gives it (makeCurvesLike()), nor its SECTIONS' sections, which the sections pass gives it (makeSectionsLike()).
    // The song hasn't the stand-ins, so what they held stays as it is now: not there. A stand-in is never part of a song
    // (Schema.h: schema::versions::missing).
    juce::ValueTree copyForSong (const juce::ValueTree& element)
    {
        auto copy = element.createCopy();
        const std::function<void (juce::ValueTree)> strip = [&] (juce::ValueTree tree)
        {
            for (int i = tree.getNumChildren(); --i >= 0;)
            {
                if (auto child = tree.getChild (i); isStandIn (child) || child.hasType (schema::CLIP) || tree.hasType (schema::TEMPOMAP)
                                                    || tree.hasType (schema::CHORDLANE) || tree.hasType (schema::MIDILINKS)
                                                    || tree.hasType (schema::AUTOMATION) || tree.hasType (schema::SECTIONS))
                    tree.removeChild (i, nullptr);
                else
                    strip (child);
            }
        };

        strip (copy);
        return copy;
    }

    // Whether makeLike() places a child among the element's: taken, a track only withTracks, never a clip (a track's clips are
    // the clip pass's: ClipPass, below), nor the song's TEMPOMAP or what is in it (the tempo pass's: makeTempoMapLike()), nor
    // its CHORDLANE or what is in it (the lane pass's: makeChordLaneLike()), nor its MIDILINKS or what is in it (the links
    // pass's: makeMidiLinksLike()), nor its AUTOMATION or what is in it (the curves pass's: makeCurvesLike()), nor its
    // SECTIONS or what is in it (the sections pass's: makeSectionsLike()). The whole song's placeholders and places are put
    // in the other's places after the areas' passes (placePlaceholdersLike()).
    bool isPlaced (const juce::ValueTree& element, const juce::ValueTree& child, bool withTracks, Taken taken)
    {
        return isTaken (taken, element, child) && (withTracks || ! child.hasType (schema::TRACK)) && ! child.hasType (schema::CLIP)
               && ! isTempoMaps (element, child) && ! isChordLanes (element, child) && ! isMidiLinks (element, child)
               && ! isSections (element, child) && ! isCurves (element, child);
    }

    std::vector<Child> getCountedChildren (const juce::ValueTree& element, bool withTracks = true, Taken taken = Taken::counted)
    {
        std::vector<Child> children;
        std::map<juce::String, int> numUnkeyed;
        std::set<juce::String> names;

        for (const auto& child : element)
        {
            if (! isPlaced (element, child, withTracks, taken))
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

    // The element made like the other, its children too, but for the tracks in it while withTracks is off (a folder's,
    // which are things of their own), and a track's clips, which the clip pass makes like the other's once this is done
    // (ClipPass, below), in the same history (the song's, or none while the song is written outside it).
    void makeLike (Song& song, juce::ValueTree live, const juce::ValueTree& other, juce::UndoManager* undo, bool withTracks = true,
                   Taken taken = Taken::counted, const StandIns& standIns = noStandIns())
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
        // each it hasn't added, after the uncounted children before them (its lanes, which are view state).
        const auto wanted = getCountedChildren (other, withTracks, taken);
        std::set<juce::String> wantedNames;

        for (const auto& child : wanted)
            wantedNames.insert (child.name);

        std::map<juce::String, juce::ValueTree> kept;

        for (const auto& child : getCountedChildren (live, withTracks, taken))
        {
            if (wantedNames.count (child.name) == 0 && standIns.count (child.name) == 0)
                live.removeChild (child.tree, undo);
            else if (wantedNames.count (child.name) > 0)
            {
                kept.emplace (child.name, child.tree);
            }
        }

        int place = 0;

        for (const auto& child : wanted)
        {
            while (place < live.getNumChildren() && ! isPlaced (live, live.getChild (place), withTracks, taken))
                ++place;

            if (const auto found = kept.find (child.name); found != kept.end())
            {
                // What the other's version couldn't read stays as it is, in its place.
                auto tree = found->second;

                if (! isStandIn (child.tree))
                    makeLike (song, tree, child.tree, undo, true, taken, standIns);

                if (const auto index = live.indexOf (tree); index != place)
                    live.moveChild (index, place, undo);
            }
            else if (! isStandIn (child.tree))
            {
                live.addChild (copyForSong (child.tree), place, undo);
            }
            else
            {
                continue; // a stand-in for what the song hasn't: nothing to put there
            }

            ++place;
        }
    }

    // Where each item is in a song, or in a part of it (its parent's type and key), by its own type and ID: notes and clips
    // aside (the clip pass moves clips: ClipPass, below), and, while withTracks is off, the tracks in it (a folder's, things
    // of their own).
    using Places = std::map<juce::String, juce::String>;

    Places findPlaces (const juce::ValueTree& tree, bool withTracks = true)
    {
        Places places;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if ((! withTracks && child.hasType (schema::TRACK)) || child.hasType (schema::CLIP) || child.hasType (schema::SECTIONS))
                    continue; // a clip, and the song's sections (the sections pass's: makeSectionsLike())

                if (! child.hasType (schema::NOTE) && schema::holdsID (child, schema::trackID))
                    places[child.getType().toString() + ":" + child[schema::trackID].toString()] =
                        element.getType().toString() + ":" + schema::getElementKey (element);

                visit (child);
            }
        };

        visit (tree);
        return places;
    }

    // The song's items that are somewhere else in the other song, or in the part of it about to be taken (a track moved
    // into a folder, a device into a container), taken away first, to be put where the other has them: no ID is in the
    // song twice, meanwhile or after. Not clips, which the clip pass moves (ClipPass, below).
    void removeMovedItems (juce::ValueTree live, const Places& otherPlaces, juce::UndoManager* undo,
                           const StandIns& standIns = noStandIns())
    {
        std::vector<juce::ValueTree> moved;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::CLIP))
                    continue;

                if (! child.hasType (schema::NOTE) && schema::holdsID (child, schema::trackID) && standIns.count (nameOf (child)) == 0)
                {
                    const auto found = otherPlaces.find (child.getType().toString() + ":" + child[schema::trackID].toString());

                    if (found != otherPlaces.end() && found->second != element.getType().toString() + ":" + schema::getElementKey (element))
                    {
                        moved.push_back (child);
                        continue;
                    }
                }

                visit (child);
            }
        };

        visit (live);

        for (auto& tree : moved)
            tree.getParent().removeChild (tree, undo);
    }

    // What holds items in a song, by its type and key as findPlaces() names it ("TRACK:12", "CHAIN:40"): a track, the
    // master, a chain; invalid if the song has none.
    juce::ValueTree findHolder (const juce::ValueTree& song, const juce::String& place)
    {
        for (const auto& child : song)
        {
            if (child.hasType (schema::NOTE) || child.hasType (schema::CLIP))
                continue;

            if (schema::holdsID (child, schema::trackID) && nameOf (child) == place)
                return child;

            if (const auto found = findHolder (child, place); found.isValid())
                return found;
        }

        return {};
    }

    // Where an item goes among a holder's children to be where the other song has it among its holder's there: after the
    // one before it there that this holder has; else before the others of its order (a chain's devices), or last.
    int findIndexLike (const juce::ValueTree& home, const juce::ValueTree& item, const juce::ValueTree& homeThere)
    {
        std::map<juce::String, int> held;

        for (int i = 0; i < home.getNumChildren(); ++i)
            if (const auto child = home.getChild (i); schema::getElementKey (child).isNotEmpty())
                held.emplace (nameOf (child), i);

        juce::ValueTree there;

        for (const auto& child : homeThere)
            if (nameOf (child) == nameOf (item))
                there = child;

        for (auto before = there.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            if (const auto found = held.find (nameOf (before)); found != held.end())
                return found->second + 1;

        if (const auto group = schema::getOrderGroup (item); group.isNotEmpty())
            for (int i = 0; i < home.getNumChildren(); ++i)
                if (schema::getOrderGroup (home.getChild (i)) == group)
                    return i;

        return -1;
    }

    // What a track holds now that the other song holds somewhere else (a device moved since into a container on another
    // track), sent there, as it is now, before the track is made like the other's or taken away: it would be on neither track
    // otherwise. Where the song has nothing to hold it now (the track it was on deleted since), it goes with the rest, and
    // that track's own line brings it back. Not its clips, which the clip pass sends (ClipPass::sendHome()).
    void sendItemsHome (juce::ValueTree state, const juce::ValueTree& track, const juce::ValueTree& other, juce::UndoManager* undo)
    {
        const auto otherPlaces = findPlaces (other);
        std::vector<std::pair<juce::ValueTree, juce::ValueTree>> going; // each with what holds it there, in the song
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::TRACK) || child.hasType (schema::NOTE) || child.hasType (schema::CLIP))
                    continue;

                if (schema::holdsID (child, schema::trackID))
                {
                    if (const auto found = otherPlaces.find (nameOf (child));
                        found != otherPlaces.end() && found->second != nameOf (element))
                    {
                        if (const auto home = findHolder (state, found->second); home.isValid() && ! home.isAChildOf (child))
                            going.emplace_back (child, home);

                        continue;
                    }
                }

                visit (child);
            }
        };

        visit (track);

        for (auto& [item, home] : going)
        {
            const auto index = findIndexLike (home, item, findHolder (other, nameOf (home)));
            item.getParent().removeChild (item, undo);
            home.addChild (item, index, undo);
        }
    }

    juce::ValueTree findTrack (const juce::ValueTree& song, std::uint64_t id)
    {
        if (const auto master = song.getChildWithName (schema::MASTER); master.isValid() && readID (master[schema::trackID]) == id)
            return master;

        for (const auto& track : getTrackElements (song))
            if (readID (track[schema::trackID]) == id)
                return track;

        return {};
    }

    // The element that holds a track there: the song's track list, or a folder's element.
    juce::ValueTree findTrackParent (const juce::ValueTree& song, const juce::ValueTree& parentThere)
    {
        if (parentThere.hasType (schema::TRACK))
            if (const auto folder = findTrack (song, readID (parentThere[schema::trackID])); folder.isValid())
                return folder;

        return song.getChildWithName (schema::TRACKS);
    }

    // A copy of a track of the other song's, a folder's, without the tracks in it, however deep, that the song has
    // somewhere now: each track is in the song once.
    void removeTracksHeld (juce::ValueTree copy, const juce::ValueTree& song)
    {
        for (int i = copy.getNumChildren(); --i >= 0;)
        {
            auto child = copy.getChild (i);

            if (! child.hasType (schema::TRACK))
                continue;

            if (findTrack (song, readID (child[schema::trackID])).isValid())
                copy.removeChild (i, nullptr);
            else
                removeTracksHeld (child, song);
        }
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

            if (there != otherTracks.end() && ! isStandIn (there->second) && findTrackState (song, track).isValid()
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

        // Whether a track of the song holds clips: a track's, not a folder's or a return's.
        bool holdsClips (TrackID track) const
        {
            const auto trackState = findTrackState (song, track);
            return trackState.hasType (schema::TRACK) && ! isFolderState (trackState) && ! isReturnState (trackState);
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
    // chord lane's that come into its CHORDLANE (SongValues.h: a safety net): every copy added to the song is without its
    // tracks' clips, the tempo map's entries and the chord lane's (copyForSong()), and the passes over elements leave the
    // song's TEMPOMAP and CHORDLANE to the areas' passes (isPlaced()). Checked in Debug builds, as each of the functions
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

            return values->countAdopted()            // the clips', with their notes
                   + values->countTempoMapAdopted()  // the tempo map's
                   + values->countChordLaneAdopted() // the chord lane's
                   + values->countMidiLinksAdopted() // the MIDI links'
                   + values->countCurvesAdopted()    // the curves'
                   + values->countSectionsAdopted(); // the sections'
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

    // Every track's clips made like the other's, once the rest of the song is: how many writes it made.
    int makeClipsLike (Song& song, const juce::ValueTree& other, bool standInsStay)
    {
        ClipPass clips (song, other, ClipPass::Elsewhere::leftForItsTrack, standInsStay);
        clips.addEveryTrack();
        return clips.run();
    }

    // The areas the song holds as values beside its tree made like the other's, once the rest of the song is, each through
    // its pass, a line each, in this order: the clips with their notes (makeClipsLike()), then the tempo map
    // (makeTempoMapLike()), then the chord lane (makeChordLaneLike()), then the MIDI links (makeMidiLinksLike()), then the
    // curves (makeAllCurvesLike()), then the sections (makeSectionsLike()), none of which reads another's. How many writes
    // they made (the tempo map's one, where it wrote; the chord lane's one, where it wrote).
    int makeValuesLike (Song& song, const juce::ValueTree& other, bool standInsStay)
    {
        auto writes = makeClipsLike (song, other, standInsStay);
        writes += makeTempoMapLike (song, other) ? 1 : 0;
        writes += makeChordLaneLike (song, other) ? 1 : 0;
        writes += makeMidiLinksLike (song, other) ? 1 : 0; // the MIDI links' one, where it wrote
        writes += makeAllCurvesLike (song, other);
        writes += makeSectionsLike (song, other) ? 1 : 0; // the sections' one, where it wrote
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
        { return isPlaced (parent, child, true, taken) || isPlaceholder (parent, child); };

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
        auto state = song.getState();
        const auto next = std::max (findNextID (song), findNextID (other));

        if ((std::uint64_t) (juce::int64) state.getProperty (schema::nextID, (juce::int64) 0) != next)
            state.setProperty (schema::nextID, (juce::int64) next, nullptr);
    }

    // The tracks a song holds by their IDs, each with its parent's ID (0 for the track list) and its place there.
    struct TrackPlace
    {
        std::uint64_t parent = 0;
        int index = 0;
    };

    std::map<std::uint64_t, TrackPlace> findTrackPlaces (const juce::ValueTree& song)
    {
        std::map<std::uint64_t, TrackPlace> places;

        for (const auto& track : getTrackElements (song))
            places[readID (track[schema::trackID])] = { track.getParent().hasType (schema::TRACK)
                                                            ? readID (track.getParent()[schema::trackID])
                                                            : 0,
                                                        track.getParent().indexOf (track) };

        return places;
    }
} // namespace

void makeSongLike (Song& song, const juce::ValueTree& other)
{
    const NothingTakenIn byThePassesAlone (song);
    auto* undo = &song.getUndoManager();
    const auto standIns = findStandIns (other);
    removeMovedItems (song.getState(), findPlaces (other), undo, standIns);
    makeLike (song, song.getState(), other, undo, true, Taken::counted, standIns);
    makeValuesLike (song, other, true);
    placePlaceholdersLike (song, other, undo, Taken::counted);
    countIDsPast (song, other);
}

void makeSongTheSame (Song& song, const juce::ValueTree& other)
{
    const NothingTakenIn byThePassesAlone (song);

    // Outside the history, as what the song keeps beside its tree is written too (Song::OutsideHistory).
    const Song::OutsideHistory outside (song);

    // Its count of IDs the larger of the two: what either branch made keeps an ID of its own.
    const auto next = std::max (findNextID (song), findNextID (other));
    removeMovedItems (song.getState(), findPlaces (other), nullptr);
    makeLike (song, song.getState(), other, nullptr, true, Taken::everything);
    makeValuesLike (song, other, false);
    placePlaceholdersLike (song, other, nullptr, Taken::everything);
    song.getState().setProperty (schema::nextID, (juce::int64) next, nullptr);
    song.getUndoManager().clearUndoHistory();
}

void makeSongAsCopied (Song& song, const juce::ValueTree& copy)
{
    const NothingTakenIn byThePassesAlone (song);
    const Song::OutsideHistory outside (song);

    // Everything, outside the history, and its count of IDs as the copy's: what the history holds is the copy's already, so
    // that only what changed without it differs (a property set without a step, view state, the count of IDs). Its clips, its
    // tempo map, its chord lane and its curves among what the history holds: every change of one is an undo step, so none
    // differs (an Undo of a step below would lose what was written of one here: ValueArea.h); the passes write nothing.
    makeLike (song, song.getState(), copy, nullptr, true, Taken::everything);
    [[maybe_unused]] const auto valueWrites = makeValuesLike (song, copy, false);
    jassert (valueWrites == 0);
    placePlaceholdersLike (song, copy, nullptr, Taken::everything);
    song.getState().setProperty (schema::nextID, copy[schema::nextID], nullptr);
}

namespace
{
    bool makeTrackLikeIn (Song& song, juce::ValueTree state, TrackID id, const juce::ValueTree& other, juce::UndoManager* undo)
    {
        auto live = findTrack (state, id.getRaw());
        const auto then = findTrack (other, id.getRaw());

        if (! live.isValid() && ! then.isValid())
            return false;

        // A track the other's version couldn't read stays as it is.
        if (then.isValid() && isStandIn (then))
            return true;

        // Its devices' automation, of what either of them holds.
        auto devices = getAutomatableIDs (live);
        devices.merge (getAutomatableIDs (then));

        // Its clips, and those of the tracks that come back with it, in a pass of their own once the rest is done: what the
        // track holds that the other song holds on another track goes there, as it is now.
        ClipPass clips (song, other, ClipPass::Elsewhere::sentThere, true);

        if (live.isValid() && then.isValid())
        {
            // What it held then that is somewhere else now (a device moved into a container on another track) comes back from
            // there, and what it holds now that the other song holds somewhere else goes there.
            const auto standIns = findStandIns (then);
            removeMovedItems (state, findPlaces (then, false), undo, standIns);
            sendItemsHome (state, live, other, undo);
            makeLike (song, live, then, undo, false, Taken::counted, standIns);
            clips.addTrack (id);
        }
        else if (live.isValid())
        {
            // Added since: taken away. The tracks in it (a folder's) are things of their own, which stay: where it was, in
            // their order, as ungrouping a folder leaves them. And what it holds that the other song holds somewhere else (a
            // clip moved onto it) goes there.
            clips.sendHome (id);
            sendItemsHome (state, live, other, undo);
            auto parent = live.getParent();
            auto place = parent.indexOf (live) + 1;

            for (int i = 0; i < live.getNumChildren();)
            {
                if (auto child = live.getChild (i); child.hasType (schema::TRACK))
                {
                    live.removeChild (child, undo);
                    parent.addChild (child, place++, undo);
                }
                else
                {
                    ++i;
                }
            }

            parent.removeChild (live, undo);
        }
        else
        {
            // Gone since: back where it was, after the track before it there, with the tracks it held that aren't elsewhere
            // now, and what they held that is elsewhere now back from there; without what the other's version couldn't read
            // (a clip of it), which isn't in the song now either.
            auto parent = findTrackParent (state, then.getParent());
            auto copy = copyForSong (then);
            removeTracksHeld (copy, state);
            removeMovedItems (state, findPlaces (copy), undo);

            int index = 0;

            for (auto before = then.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto found = parent.indexOf (findTrack (state, readID (before[schema::trackID]))); found >= 0)
                {
                    index = found + 1;
                    break;
                }
            }

            parent.addChild (copy, index, undo);

            // Its clips, and those of the tracks in it that came back with it.
            const std::function<void (const juce::ValueTree&)> addTracks = [&] (const juce::ValueTree& track)
            {
                clips.addTrack (TrackID::fromRaw (readID (track[schema::trackID])));

                for (const auto& child : track)
                    if (child.hasType (schema::TRACK))
                        addTracks (child);
            };

            addTracks (copy);
        }

        clips.run();
        makeCurvesLike (
            song, other, [&devices] (const ParameterRef& parameter) { return devices.count (parameter.device.getRaw()) > 0; }, false);
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
                makeLike (song, live, then, undo, true, Taken::counted, findStandIns (then));
            else if (live.isValid())
                state.removeChild (live, undo);
            else if (then.isValid())
                state.appendChild (copyForSong (then), undo);
        }

        // The tracks both have, in the other's order: in its folders, and in its order there.
        const auto wanted = findTrackPlaces (other);

        for (const auto& track : getTrackElements (other))
        {
            const auto id = readID (track[schema::trackID]);
            auto live = findTrack (state, id);

            if (! live.isValid())
                continue;

            auto parent = findTrackParent (state, track.getParent());

            if (live.getParent() != parent)
            {
                live.getParent().removeChild (live, undo);
                parent.addChild (live, -1, undo);
            }

            // After the track before it there that both have.
            int index = 0;

            for (auto before = track.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto found = parent.indexOf (findTrack (state, readID (before[schema::trackID]))); found >= 0)
                {
                    index = found + 1;
                    break;
                }
            }

            if (const auto current = parent.indexOf (live); current != index && current != index - 1)
                parent.moveChild (current, current < index ? index - 1 : index, undo);
        }

        juce::ignoreUnused (wanted);
    }
} // namespace

bool makeTrackLike (Song& song, TrackID id, const juce::ValueTree& other)
{
    const NothingTakenIn byThePassesAlone (song);
    const auto done = makeTrackLikeIn (song, song.getState(), id, other, &song.getUndoManager());
    countIDsPast (song, other);
    return done;
}

void makeSongOwnLike (Song& song, const juce::ValueTree& other)
{
    const NothingTakenIn byThePassesAlone (song);
    makeSongOwnLikeIn (song, song.getState(), other, &song.getUndoManager());
    countIDsPast (song, other);
}

int removeBrokenReferences (Song& song)
{
    auto* undo = &song.getUndoManager();
    const auto state = song.getState();
    std::set<std::uint64_t> items, tracks;
    const std::function<void (const juce::ValueTree&)> collect = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (schema::holdsID (child, schema::trackID) && ! child.hasType (schema::NOTE))
                items.insert (readID (child[schema::trackID]));

            for (const auto* property : { &schema::channelFader, &schema::channelVCA })
                if (child.hasProperty (*property))
                    items.insert (readID (child[*property]));

            if (child.hasType (schema::TRACK))
                tracks.insert (readID (child[schema::trackID]));

            collect (child);
        }
    };

    collect (state);

    // And those the song's values hold of items that something can name, which its tree may not (IDsHeld.h:
    // forEachNameableIDHeld(), a modulator's and a macro's once they leave the tree).
    forEachNameableIDHeld (song, [&items] (std::uint64_t id) { items.insert (id); });
    std::vector<juce::ValueTree> modulations;
    std::vector<juce::ValueTree> sidechains;
    const std::function<void (const juce::ValueTree&)> find = [&] (const juce::ValueTree& element)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::MODULATES) && items.count (readID (child[schema::modulatesDevice])) == 0)
                modulations.push_back (child);

            if (child.hasType (schema::DEVICE) && child.hasProperty (schema::deviceSidechain)
                && tracks.count (readID (child[schema::deviceSidechain])) == 0)
                sidechains.push_back (child);

            find (child);
        }
    };

    find (state);

    for (auto& modulation : modulations)
        modulation.getParent().removeChild (modulation, undo);

    for (auto& device : sidechains)
        device.removeProperty (schema::deviceSidechain, undo);

    // And what an edit takes away with what it names (model/mixer/internal/Mixer.h): a send to a return that is gone, with
    // what modulated it, and a pad's track whose drum sampler is gone; so that the song is as makeConsistent() leaves it.
    // Then a sidechain that would hear the device's own sound round a loop, which two lines' changes can make together (one
    // a sidechain, the other a send), as an edit that makes one lets it go (Sidechain.h: forgetSidechainLoops()).
    const auto count = [&state] (const std::function<bool (const juce::ValueTree&)>& counts)
    {
        auto found = 0;
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                found += counts (child) ? 1 : 0;
                visit (child);
            }
        };

        visit (state);
        return found;
    };

    const auto isSend = [] (const juce::ValueTree& element) { return element.hasType (schema::SEND); };
    const auto hasSidechain = [] (const juce::ValueTree& element)
    { return element.hasType (schema::DEVICE) && element.hasProperty (schema::deviceSidechain); };

    const auto sendsBefore = count (isSend);
    forgetWhatNamesTheGone (song);
    const auto sidechainsBefore = count (hasSidechain);
    forgetSidechainLoops (song);

    return (int) (modulations.size() + sidechains.size()) + sendsBefore - count (isSend) + sidechainsBefore - count (hasSidechain);
}
} // namespace phazite
