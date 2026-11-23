// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongMerge.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace phazite
{
namespace
{
    // The same value as a file holds it (model/base/internal/SongText.h), none the same as none only.
    bool isSameValueOrNone (const juce::var& a, const juce::var& b)
    {
        if (a.isVoid() || b.isVoid())
            return a.isVoid() && b.isVoid();

        return isSameValue (a, b);
    }

    juce::String keyOf (const juce::ValueTree& element)
    {
        return element.getType().toString() + ":" + schema::getElementKey (element);
    }

    // An element's place from the song down, as a clash names it.
    juce::StringArray getPath (const juce::ValueTree& element)
    {
        juce::StringArray path;

        for (auto e = element; e.isValid() && e.getParent().isValid(); e = e.getParent())
            path.insert (0, keyOf (e));

        return path;
    }

    // What merges whole: a curve with its points, the chord lane, the key set last, and a MIDI link (a controller's, which
    // both sides linked to different parameters, is this side's: never one side's device with the other's parameter).
    bool mergesWhole (const juce::ValueTree& element)
    {
        return element.hasType (schema::CURVE) || element.hasType (schema::CHORDLANE) || element.hasType (schema::LASTKEY)
               || element.hasType (schema::MIDILINK);
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

    juce::ValueTree findTrackParent (const juce::ValueTree& song, const juce::ValueTree& parentThere)
    {
        if (parentThere.hasType (schema::TRACK))
            if (const auto folder = findTrack (song, readID (parentThere[schema::trackID])); folder.isValid())
                return folder;

        return song.getChildWithName (schema::TRACKS);
    }

    // Whether two tracks hold the same but for the tracks in them (a folder's, lines of their own).
    bool holdTheSameOwn (const juce::ValueTree& a, const juce::ValueTree& b)
    {
        const auto withoutTracks = [] (const juce::ValueTree& track)
        {
            auto copy = track.createCopy();

            for (int i = copy.getNumChildren(); --i >= 0;)
                if (copy.getChild (i).hasType (schema::TRACK))
                    copy.removeChild (i, nullptr);

            return copy;
        };

        return holdTheSame (withoutTracks (a), withoutTracks (b));
    }

    // A copy of one of the other side's tracks, a folder's, without the tracks in it, however deep, that the song has
    // somewhere already: each track is in the song once.
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

    // Puts a copy of one of the other side's tracks into the song where it is there: in its folder there, if the song has
    // it, after the track before it there that the song has; with the tracks in it that the song hasn't.
    juce::ValueTree addTrackCopy (juce::ValueTree song, const juce::ValueTree& theirs)
    {
        auto copy = theirs.createCopy();
        removeTracksHeld (copy, song);
        auto parent = findTrackParent (song, theirs.getParent());
        auto index = 0;

        for (auto before = theirs.getSibling (-1); before.isValid(); before = before.getSibling (-1))
        {
            if (const auto place = parent.indexOf (findTrack (song, readID (before[schema::trackID]))); place >= 0)
            {
                index = place + 1;
                break;
            }
        }

        parent.addChild (copy, index, nullptr);
        return copy;
    }

    // Takes a track out of the song, a folder without the tracks in it: lines of their own, they stay where it was, in
    // their order, as ungrouping a folder leaves them.
    void removeTrackOwn (juce::ValueTree track)
    {
        auto parent = track.getParent();
        auto place = parent.indexOf (track) + 1;

        for (int i = 0; i < track.getNumChildren();)
        {
            if (auto child = track.getChild (i); child.hasType (schema::TRACK))
            {
                track.removeChild (child, nullptr);
                parent.addChild (child, place++, nullptr);
            }
            else
            {
                ++i;
            }
        }

        parent.removeChild (track, nullptr);
    }

    // What can be somewhere else on one side than in the base: an item among its parent's (a clip on another track, a device
    // in a container), by its type and ID. Not a track, which the tracks' order places, nor a note, whose ID is its clip's own.
    using Items = std::map<juce::String, juce::ValueTree>;

    bool canMove (const juce::ValueTree& element)
    {
        return ! element.hasType (schema::NOTE) && ! element.hasType (schema::TRACK) && ! element.hasType (schema::MASTER)
               && element.hasProperty (schema::trackID) && schema::holdsID (element, schema::trackID);
    }

    void collectMovable (const juce::ValueTree& tree, Items& items)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::NOTE))
                continue;

            if (canMove (child))
                items.emplace (keyOf (child), child);

            collectMovable (child, items);
        }
    }

    juce::ValueTree findItem (const Items& items, const juce::String& name)
    {
        const auto found = items.find (name);
        return found != items.end() ? found->second : juce::ValueTree();
    }

    //==============================================================================
    // A copy of the other side's song, what it added since the base given IDs anew, and what names them renamed to match.
    struct Renumbered
    {
        juce::ValueTree song;
        CopiedIDs ids;                                                           // old to new
        std::map<std::pair<juce::String, std::uint64_t>, std::uint64_t> noteIDs; // by clip (its group) and old, the new
    };

    // A clip's notes' group, by which their IDs given anew are kept: its alias group's, or its own.
    juce::String getNoteGroup (const juce::ValueTree& clip)
    {
        return clip.hasProperty (schema::clipLink) ? "link " + clip[schema::clipLink].toString()
                                                   : "clip " + clip[schema::clipID].toString();
    }

    void collectIDs (const juce::ValueTree& tree, std::set<std::uint64_t>& ids)
    {
        for (const auto& child : tree)
        {
            if (! child.hasType (schema::NOTE))
                for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA, &schema::clipLink })
                    if (child.hasProperty (*property) && schema::holdsID (child, *property))
                        ids.insert (readID (child[*property]));

            collectIDs (child, ids);
        }
    }

    void collectClips (const juce::ValueTree& tree, std::map<std::uint64_t, juce::ValueTree>& clips)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::CLIP))
                clips.emplace (readID (child[schema::clipID]), child);
            else
                collectClips (child, clips);
        }
    }

    void collectItems (const juce::ValueTree& tree, std::map<std::uint64_t, juce::ValueTree>& items)
    {
        for (const auto& child : tree)
        {
            if (! child.hasType (schema::NOTE) && child.hasProperty (schema::trackID) && schema::holdsID (child, schema::trackID))
                items.emplace (readID (child[schema::trackID]), child);

            collectItems (child, items);
        }
    }

    Renumbered renumberAdditions (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                                  const std::function<std::uint64_t()>& makeID)
    {
        Renumbered result { theirs.createCopy(), {}, {} };
        std::set<std::uint64_t> inBase, inOurs;
        collectIDs (base, inBase);
        collectIDs (ours, inOurs);
        std::map<std::uint64_t, juce::ValueTree> ourItems;
        collectItems (ours, ourItems);

        const auto newID = [&result, &makeID] (std::uint64_t old)
        {
            auto [found, isNew] = result.ids.emplace (old, 0);

            if (isNew)
                found->second = makeID();

            return found->second;
        };

        // What the other side added since the base (items, channels' faders and VCAs, alias groups) keeps its ID where this side
        // has nothing with it; where this side has something else with it (both counted on from the same place), it gets a new
        // one. An item this side has too, the same and in the same place, is the same: added on both sides, or taken into both.
        // Inside what the other side added, nothing is this side's.
        const std::function<void (juce::ValueTree, bool)> renumber = [&] (juce::ValueTree tree, bool insideAdded)
        {
            for (auto child : tree)
            {
                if (child.hasType (schema::NOTE))
                    continue;

                auto added = insideAdded;

                if (child.hasProperty (schema::trackID) && schema::holdsID (child, schema::trackID))
                {
                    const auto id = readID (child[schema::trackID]);

                    if (inBase.count (id) == 0)
                    {
                        if (const auto found = ourItems.find (id);
                            ! insideAdded && found != ourItems.end() && found->second.getType() == child.getType()
                            && keyOf (found->second.getParent()) == keyOf (tree) && holdTheSame (found->second, child))
                            continue;

                        added = true;
                    }
                }

                for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA, &schema::clipLink })
                    if (child.hasProperty (*property) && schema::holdsID (child, *property))
                        if (const auto old = readID (child[*property]); inBase.count (old) == 0 && inOurs.count (old) > 0)
                            child.setProperty (*property, (juce::int64) newID (old), nullptr);

                renumber (child, added);
            }
        };

        renumber (result.song, false);

        // Notes added to clips the base has: a new ID where this side's clip has another note with it, the same in each clip of an
        // alias group. A clip the other side added keeps its notes' IDs, its own.
        std::map<std::uint64_t, juce::ValueTree> baseClips, ourClips, theirClips;
        collectClips (base, baseClips);
        collectClips (ours, ourClips);
        collectClips (result.song, theirClips);
        auto& noteIDs = result.noteIDs;

        for (auto& [id, clip] : theirClips)
        {
            const auto inBaseClip = baseClips.find (id);

            if (inBaseClip == baseClips.end())
                continue;

            std::set<std::uint64_t> baseNotes;

            for (const auto& note : inBaseClip->second)
                if (note.hasType (schema::NOTE))
                    baseNotes.insert (readID (note[schema::noteID]));

            const auto group = getNoteGroup (clip);

            std::map<std::uint64_t, juce::ValueTree> ourNotes;

            if (const auto ourClip = ourClips.find (id); ourClip != ourClips.end())
                for (const auto& note : ourClip->second)
                    if (note.hasType (schema::NOTE))
                        ourNotes.emplace (readID (note[schema::noteID]), note);

            for (auto note : clip)
            {
                if (! note.hasType (schema::NOTE) || baseNotes.count (readID (note[schema::noteID])) > 0)
                    continue;

                if (const auto ourNote = ourNotes.find (readID (note[schema::noteID]));
                    ourNote == ourNotes.end() || holdTheSame (ourNote->second, note))
                    continue;

                auto [found, isNew] = noteIDs.emplace (std::make_pair (group, readID (note[schema::noteID])), 0);

                if (isNew)
                    found->second = makeID();

                note.setProperty (schema::noteID, (juce::int64) found->second, nullptr);
            }
        }

        // What names an item given a new ID: a send's return, a sidechain, a lane's, a curve's and a modulation's device.
        const std::function<void (juce::ValueTree)> rename = [&] (juce::ValueTree tree)
        {
            for (auto child : tree)
            {
                for (int i = 0; i < child.getNumProperties(); ++i)
                    if (const auto property = child.getPropertyName (i); schema::namesID (child, property))
                        if (const auto found = result.ids.find (readID (child[property])); found != result.ids.end())
                            child.setProperty (property, writeIDLike (child[property], found->second), nullptr);

                rename (child);
            }
        };

        rename (result.song);
        return result;
    }

    //==============================================================================
    juce::String describeTrackProperty (const juce::Identifier& property)
    {
        return property == schema::trackName                                           ? "name"
               : property == schema::trackColour                                       ? "color"
               : property == schema::trackMute                                         ? "mute"
               : property == schema::trackSolo                                         ? "solo"
               : property == schema::channelVolume                                     ? "volume"
               : property == schema::channelPan                                        ? "pan"
               : property == schema::trackAutomationMode                               ? "automation mode"
               : property == schema::keyRoot || property == schema::keyScale           ? "key"
               : property == schema::trackInput || property == schema::trackMonitoring ? "input"
               : property == schema::trackPadDevice || property == schema::trackPad    ? "pad"
                                                                                       : property.toString();
    }

    // What a clash or a note is about, in Phazite's own words: an element, or one of its properties.
    juce::String describeClash (const juce::ValueTree& element, const juce::Identifier& property)
    {
        const auto withProperty = [&property] (const juce::String& thing)
        { return property.isValid() ? thing + ": " + property.toString() : thing; };

        if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
            return property.isValid() ? describeTrackProperty (property) : juce::String ("the track");

        if (element.hasType (schema::VALUES))
            return withProperty (getDeviceName (element.getParent()));

        if (element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER))
            return property == schema::pluginState ? getDeviceName (element) + " settings" : withProperty (getDeviceName (element));

        // A container's chain, by its container's name and its own ("Container: Chain 2").
        if (element.hasType (schema::CHAIN))
            return withProperty (getDeviceName (element.getParent()) + ": " + element[schema::chainName].toString());

        if (element.hasType (schema::VALUE))
            return getDeviceName (element.getParent()) + ": " + element[schema::valueParameter].toString();

        if (element.hasType (schema::SEND))
            return property == schema::sendLevel ? juce::String ("send level") : withProperty ("send");

        if (element.hasType (schema::CLIP))
        {
            // A clip without a name of its own is called after its track.
            auto name = element[schema::clipName].toString();
            name = name.isNotEmpty() ? name : element.getParent()[schema::trackName].toString();
            return withProperty (name.isNotEmpty() ? "clip \"" + name + "\"" : juce::String ("a clip"));
        }

        if (element.hasType (schema::NOTE))
            return property.isValid() ? "a note's " + property.toString() : juce::String ("a note");

        if (element.hasType (schema::CURVE))
            return "automation of " + element[schema::curveParameter].toString();

        if (element.hasType (schema::CHORDLANE))
            return "chords";

        if (element.hasType (schema::LASTKEY))
            return "key";

        if (element.hasType (schema::TEMPO))
            return "tempo";

        if (element.hasType (schema::TIMESIGNATURE))
            return "time signature";

        if (element.hasType (schema::SECTION))
            return withProperty ("section \"" + element[schema::sectionName].toString() + "\"");

        if (element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO) || element.hasType (schema::MODULATES))
            return "modulation";

        if (element.hasType (schema::MIDILINK))
            return "MIDI link of CC " + element[schema::midiLinkNumber].toString();

        return property.isValid() ? property.toString() : element.getType().toString().toLowerCase();
    }

    //==============================================================================
    struct Merger
    {
        MergeResult& result;
        juce::String line;
        std::uint64_t trackID = 0;

        // What can be somewhere else on one side (canMove()): the base's, the other side's, and this side's as the merge
        // began, each of which is in the result where the merge has it now, unless the merge took it away.
        const Items& baseItems;
        const Items& theirItems;
        const Items& ourItems;

        void note (const juce::String& text) { result.notes.addIfNotAlreadyThere (line + ": " + text); }

        // This side's item of that type and ID, wherever it is in the result now; invalid for none.
        juce::ValueTree findOurs (const juce::ValueTree& item) const
        {
            if (! canMove (item))
                return {};

            const auto mine = findItem (ourItems, keyOf (item));
            return mine.isAChildOf (result.song) ? mine : juce::ValueTree();
        }

        // Whether this side left an item where the base has it (was): among the same parent's.
        static bool isWhereItWas (const juce::ValueTree& mine, const juce::ValueTree& was)
        {
            return was.isValid() && keyOf (mine.getParent()) == keyOf (was.getParent());
        }

        // What a copy of the other side's, just put into the result, holds that this side has somewhere else: what the other
        // side moved into something it added (a device into a new container, a clip onto a new track), or what this side
        // moved out of something it took away. This side's takes its place in the copy where this side left it where the
        // base has it, and stays where this side put it otherwise; either way with the other side's changes, and in the
        // result once.
        void takeOurs (juce::ValueTree copy)
        {
            for (int i = 0; i < copy.getNumChildren(); ++i)
            {
                auto child = copy.getChild (i);

                if (child.hasType (schema::NOTE))
                    continue;

                auto mine = findOurs (child);

                if (! mine.isValid())
                {
                    // Moved there into what this copy is, and taken away here: back, where the other side moved it.
                    if (const auto was = canMove (child) ? findItem (baseItems, keyOf (child)) : juce::ValueTree();
                        was.isValid() && keyOf (copy) != keyOf (was.getParent()) && ! findItem (ourItems, keyOf (child)).isValid())
                        note (describeClash (child, {}) + " taken away here and moved there: kept");

                    takeOurs (child);
                    continue;
                }

                const auto was = findItem (baseItems, keyOf (child));
                copy.removeChild (i, nullptr);

                if (isWhereItWas (mine, was))
                {
                    mine.getParent().removeChild (mine, nullptr);
                    copy.addChild (mine, i, nullptr);
                }
                else
                {
                    if (was.isValid() && keyOf (copy) != keyOf (was.getParent()))
                        note (describeClash (mine, {}) + " moved on both sides: this side's place kept");

                    --i;
                }

                merge (was, mine, child);
            }
        }

        void clashOn (const juce::ValueTree& element, const juce::Identifier& property, const juce::var& base, const juce::var& ours,
                      const juce::var& theirs)
        {
            MergeClash clash;
            clash.line = line;
            clash.trackID = trackID;
            clash.path = getPath (element);
            clash.property = property;
            clash.base = base;
            clash.ours = ours;
            clash.theirs = theirs;
            clash.what = describeClash (element, property);
            result.clashes.push_back (std::move (clash));
        }

        void clashOnElement (const juce::ValueTree& parent, const juce::ValueTree& base, const juce::ValueTree& ours,
                             const juce::ValueTree& theirs)
        {
            MergeClash clash;
            clash.line = line;
            clash.trackID = trackID;
            const auto& any = ours.isValid() ? ours : theirs.isValid() ? theirs : base;
            clash.path = getPath (parent);
            clash.path.add (keyOf (any));
            clash.baseElement = base.createCopy();
            clash.oursElement = ours.createCopy();
            clash.theirsElement = theirs.createCopy();
            clash.what = describeClash (any, {});
            result.clashes.push_back (std::move (clash));
        }

        // The base's element, this side's in the result (r) and the other side's: their changes into r. A track's own leaves out
        // the tracks in it (withTracks off).
        void merge (const juce::ValueTree& b, juce::ValueTree r, const juce::ValueTree& t, bool withTracks = true)
        {
            mergeProperties (b, r, t);
            mergeChildren (b, r, t, withTracks);
        }

        void mergeProperties (const juce::ValueTree& b, juce::ValueTree r, const juce::ValueTree& t)
        {
            juce::StringArray names;
            const juce::ValueTree& ours = r;

            for (const auto* tree : { &b, &ours, &t })
                if (tree->isValid())
                    for (int i = 0; i < tree->getNumProperties(); ++i)
                        if (const auto name = tree->getPropertyName (i); ! schema::isUncountedProperty (r, name))
                            names.addIfNotAlreadyThere (name.toString());

            for (const auto& text : names)
            {
                const juce::Identifier name (text);
                const auto vb = b.isValid() ? b[name] : juce::var();
                const auto vo = r[name];
                const auto vt = t[name];

                if (isSameValueOrNone (vt, vb) || isSameValueOrNone (vo, vt))
                    continue;

                if (isSameValueOrNone (vo, vb))
                {
                    if (vt.isVoid())
                        r.removeProperty (name, nullptr);
                    else
                        r.setProperty (name, vt, nullptr);

                    continue;
                }

                clashOn (r, name, vb, vo, vt);
            }
        }

        // An element's counted children with keys, by their types and keys; and those without, by their types, in their order.
        struct Children
        {
            std::map<juce::String, juce::ValueTree> keyed;
            std::map<juce::String, std::vector<juce::ValueTree>> lists;
        };

        static Children getChildren (const juce::ValueTree& element, bool withTracks)
        {
            Children children;

            if (! element.isValid())
                return children;

            for (const auto& child : element)
            {
                if (schema::isUncountedElement (element, child) || (! withTracks && child.hasType (schema::TRACK)))
                    continue;

                if (schema::getElementKey (child).isEmpty() || children.keyed.count (keyOf (child)) > 0)
                    children.lists[child.getType().toString()].push_back (child);
                else
                    children.keyed.emplace (keyOf (child), child);
            }

            return children;
        }

        static bool listsHoldTheSame (const std::vector<juce::ValueTree>& a, const std::vector<juce::ValueTree>& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
                if (! holdTheSame (a[i], b[i]))
                    return false;

            return true;
        }

        // Puts an element in r where the other side's child is there: after the child before it there that r has.
        static void placeAfterBefore (juce::ValueTree r, juce::ValueTree element, const juce::ValueTree& theirs,
                                      const std::map<juce::String, juce::ValueTree>& keyed)
        {
            auto index = -1;

            for (auto before = theirs.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto found = keyed.find (keyOf (before)); found != keyed.end() && found->second.getParent() == r)
                {
                    index = r.indexOf (found->second) + 1;
                    break;
                }
            }

            // The first of an order goes before the others of it; anything else after them all.
            if (index < 0 && schema::getOrderGroup (theirs).isNotEmpty())
            {
                for (int i = 0; i < r.getNumChildren(); ++i)
                {
                    if (schema::getOrderGroup (r.getChild (i)) == schema::getOrderGroup (theirs))
                    {
                        index = i;
                        break;
                    }
                }
            }

            r.addChild (element, index, nullptr);
        }

        // Puts a copy of the other side's child in r, where it is there; what it holds that this side has somewhere else is
        // this side's (takeOurs()).
        juce::ValueTree addTheirs (juce::ValueTree r, const juce::ValueTree& theirs, const std::map<juce::String, juce::ValueTree>& keyed)
        {
            auto copy = theirs.createCopy();
            placeAfterBefore (r, copy, theirs, keyed);
            takeOurs (copy);
            return copy;
        }

        void mergeWhole (juce::ValueTree r, const juce::ValueTree& cb, const juce::ValueTree& co, const juce::ValueTree& ct,
                         std::map<juce::String, juce::ValueTree>& keyed)
        {
            const auto same = [] (const juce::ValueTree& a, const juce::ValueTree& b)
            { return a.isValid() == b.isValid() && (! a.isValid() || holdTheSame (a, b)); };

            if (same (ct, cb) || same (co, ct))
                return;

            if (same (co, cb))
            {
                // Theirs, in the place of this side's.
                const auto index = co.isValid() ? r.indexOf (co) : -1;

                if (co.isValid())
                    r.removeChild (co, nullptr);

                if (ct.isValid())
                {
                    auto copy = ct.createCopy();
                    r.addChild (copy, index, nullptr);
                    keyed[keyOf (ct)] = copy;
                }

                return;
            }

            clashOnElement (r, cb, co, ct);
        }

        void mergeChildren (const juce::ValueTree& b, juce::ValueTree r, const juce::ValueTree& t, bool withTracks)
        {
            const auto cb = getChildren (b, withTracks), cr = getChildren (r, withTracks), ct = getChildren (t, withTracks);
            auto keyed = cr.keyed;

            // The other side's children in its order first, for what it added to go after what it followed, then the others;
            // each once, known by a set (a clip's thousands of notes made this quadratic).
            std::vector<juce::String> names;
            std::set<juce::String> listed;

            for (const auto& child : t)
                if (const auto name = keyOf (child); ct.keyed.count (name) > 0 && listed.insert (name).second)
                    names.push_back (name);

            for (const auto* children : { &cb, &cr })
                for (const auto& [name, child] : children->keyed)
                    if (listed.insert (name).second)
                        names.push_back (name);

            for (const auto& name : names)
            {
                const auto findIn = [&name] (const Children& children)
                {
                    const auto found = children.keyed.find (name);
                    return found != children.keyed.end() ? found->second : juce::ValueTree();
                };

                const auto inBase = findIn (cb), theirs = findIn (ct);
                auto ours = findIn (cr);

                // Not here any more, where the merge took it to where the other side moved it (takeOurs()).
                if (ours.isValid() && ours.getParent() != r)
                    ours = {};

                const auto& any = theirs.isValid() ? theirs : ours.isValid() ? ours : inBase;

                if (mergesWhole (any))
                {
                    mergeWhole (r, inBase, ours, theirs, keyed);
                    continue;
                }

                if (! inBase.isValid())
                {
                    // Not here in the base: added; or, an item the base has somewhere else, moved here.
                    const auto was = canMove (any) ? findItem (baseItems, name) : juce::ValueTree();

                    if (theirs.isValid() && ours.isValid())
                    {
                        // Moved here on both sides; or added on both under one key: a parameter's value, a tempo at a beat.
                        if (! holdTheSame (ours, theirs))
                            merge (was, ours, theirs);
                    }
                    else if (auto mine = theirs.isValid() ? findOurs (theirs) : juce::ValueTree(); mine.isValid())
                    {
                        // Moved here there, and this side has it somewhere else: it comes here where this side left it where
                        // the base has it, and stays where this side moved it otherwise, with the other side's changes.
                        if (isWhereItWas (mine, was))
                        {
                            mine.getParent().removeChild (mine, nullptr);
                            placeAfterBefore (r, mine, theirs, keyed);
                            keyed[name] = mine;
                        }
                        else
                        {
                            note (describeClash (mine, {}) + " moved on both sides: this side's place kept");
                        }

                        merge (was, mine, theirs);
                    }
                    else if (theirs.isValid())
                    {
                        keyed[name] = addTheirs (r, theirs, keyed); // theirs, added

                        // Or moved here there from where the base has it, and taken away here: back, as a removal on one
                        // side and a change on the other always is, with a note.
                        if (was.isValid() && ! findItem (ourItems, name).isValid())
                            note (describeClash (theirs, {}) + " taken away here and moved there: kept");
                    }

                    continue;
                }

                if (ours.isValid() && theirs.isValid())
                {
                    merge (inBase, ours, theirs);

                    // Values set back to their defaults, some on each side, can leave none: the song keeps no VALUES without
                    // values (model/mixer/internal/Mixer.h: setDeviceValue(), and modulation's writers of its own values).
                    if (ours.hasType (schema::VALUES) && ours.getNumProperties() == 0 && ours.getNumChildren() == 0)
                    {
                        r.removeChild (ours, nullptr);
                        keyed.erase (name);
                    }
                }
                else if (ours.isValid())
                {
                    // Somewhere else there (a clip moved onto another track), it isn't taken away: the line it went to there
                    // takes it from here, as it is here, and without that line it stays, rather than be on neither. Taken
                    // away there: it goes, unless it changed here.
                    const auto movedThere = canMove (ours) && findItem (theirItems, name).isValid();

                    if (! movedThere && holdTheSame (inBase, ours))
                    {
                        r.removeChild (ours, nullptr);
                        keyed.erase (name);
                    }
                    else if (! movedThere)
                    {
                        note (describeClash (ours, {}) + " changed here and taken away there: kept");
                    }
                }
                else if (auto mine = theirs.isValid() ? findOurs (theirs) : juce::ValueTree(); mine.isValid())
                {
                    // Somewhere else here, where this side moved it: what changed there goes to it there.
                    if (! holdTheSame (inBase, theirs))
                        merge (inBase, mine, theirs);
                }
                else if (theirs.isValid() && ! holdTheSame (inBase, theirs))
                {
                    // Taken away here, changed there: back, as it changed.
                    keyed[name] = addTheirs (r, theirs, keyed);
                    note (describeClash (theirs, {}) + " taken away here and changed there: kept");
                }
                else if (! theirs.isValid() && canMove (inBase) && findOurs (inBase).isValid() && ! findItem (theirItems, name).isValid())
                {
                    // Moved somewhere else here, and taken away there: it stays where this side moved it, with a note.
                    note (describeClash (inBase, {}) + " moved here and taken away there: kept");
                }
            }

            // Children without keys (a clip's controller events), as a whole, kind by kind.
            for (const auto& [type, theirList] : ct.lists)
            {
                const auto empty = std::vector<juce::ValueTree>();
                const auto& baseList = cb.lists.count (type) > 0 ? cb.lists.at (type) : empty;
                const auto& ourList = cr.lists.count (type) > 0 ? cr.lists.at (type) : empty;

                if (listsHoldTheSame (theirList, baseList) || listsHoldTheSame (ourList, theirList))
                    continue;

                if (listsHoldTheSame (ourList, baseList))
                {
                    for (const auto& child : ourList)
                        r.removeChild (child, nullptr);

                    for (const auto& child : theirList)
                        r.appendChild (child.createCopy(), nullptr);
                }
                else
                {
                    note (type.toLowerCase() + " changed on both sides: this side's kept");
                }
            }

            mergeOrders (b, r, t, keyed, withTracks);
        }

        // Each order (a chain's devices, sends, modulators...): the other side's moves, where this side made none.
        static void mergeOrders (const juce::ValueTree& b, juce::ValueTree r, const juce::ValueTree& t,
                                 const std::map<juce::String, juce::ValueTree>& keyed, bool withTracks)
        {
            std::set<juce::String> groups;

            for (const auto& child : t)
                if (const auto group = schema::getOrderGroup (child); group.isNotEmpty() && (withTracks || ! child.hasType (schema::TRACK)))
                    groups.insert (group);

            for (const auto& group : groups)
            {
                const auto orderOf = [&group] (const juce::ValueTree& tree, const std::function<bool (const juce::String&)>& include)
                {
                    juce::StringArray order;

                    for (const auto& child : tree)
                        if (schema::getOrderGroup (child) == group && include (keyOf (child)))
                            order.add (keyOf (child));

                    return order;
                };

                // The children all three have, in each one's order.
                const auto inAll = [&] (const juce::String& name)
                {
                    const auto has = [&name] (const juce::ValueTree& tree)
                    {
                        for (const auto& child : tree)
                            if (keyOf (child) == name)
                                return true;

                        return false;
                    };

                    return has (b) && has (r) && has (t);
                };

                const auto orderBase = orderOf (b, inAll), orderOurs = orderOf (r, inAll), orderTheirs = orderOf (t, inAll);

                if (orderTheirs == orderBase || orderOurs != orderBase)
                    continue;

                // Theirs, with what it added: each in turn after the one before it there.
                const auto all = orderOf (t, [&keyed] (const juce::String& name) { return keyed.count (name) > 0; });

                for (int i = 1; i < all.size(); ++i)
                {
                    const auto previous = keyed.find (all[i - 1]), child = keyed.find (all[i]);

                    if (previous == keyed.end() || child == keyed.end() || previous->second.getParent() != r
                        || child->second.getParent() != r)
                        continue;

                    auto from = r.indexOf (child->second), to = r.indexOf (previous->second) + 1;

                    if (from < to)
                        --to;

                    if (from != to)
                        r.moveChild (from, to, nullptr);
                }
            }
        }
    };

    // The automation of a line's devices, curve by curve, each a whole.
    void mergeCurves (const juce::ValueTree& base, juce::ValueTree song, const juce::ValueTree& theirs,
                      const std::set<std::uint64_t>& devices, Merger& merger)
    {
        const auto curvesOf = [&devices] (const juce::ValueTree& tree)
        {
            std::map<juce::String, juce::ValueTree> curves;

            for (const auto& curve : tree.getChildWithName (schema::AUTOMATION))
                if (curve.hasType (schema::CURVE) && devices.count (readID (curve[schema::curveDevice])) > 0)
                    curves.emplace (keyOf (curve), curve);

            return curves;
        };

        const auto baseCurves = curvesOf (base), ourCurves = curvesOf (song), theirCurves = curvesOf (theirs);
        std::set<juce::String> names;

        for (const auto* curves : { &baseCurves, &ourCurves, &theirCurves })
            for (const auto& [name, curve] : *curves)
                names.insert (name);

        if (names.empty())
            return;

        auto automation = song.getChildWithName (schema::AUTOMATION);

        if (! automation.isValid())
        {
            automation = juce::ValueTree (schema::AUTOMATION);
            song.appendChild (automation, nullptr);
        }

        auto keyed = ourCurves;

        for (const auto& name : names)
        {
            const auto find = [&name] (const std::map<juce::String, juce::ValueTree>& curves)
            {
                const auto found = curves.find (name);
                return found != curves.end() ? found->second : juce::ValueTree();
            };

            merger.mergeWhole (automation, find (baseCurves), find (ourCurves), find (theirCurves), keyed);
        }
    }

    // Where the other side's tracks went (into another folder, another place), where this side moved none of them: the tracks
    // all three have, by their folders and places. Whether this side moved any is told by its own song (ours), as the merge
    // itself moves tracks in the result (song): out of a folder the other side took away.
    void mergeTrackOrder (const juce::ValueTree& base, const juce::ValueTree& ours, juce::ValueTree song, const juce::ValueTree& theirs)
    {
        const auto placesOf = [] (const juce::ValueTree& tree, const std::set<std::uint64_t>& only)
        {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> places; // folder (0 for none), track

            for (const auto& track : getTrackElements (tree))
                if (const auto id = readID (track[schema::trackID]); only.count (id) > 0)
                    places.emplace_back (track.getParent().hasType (schema::TRACK) ? readID (track.getParent()[schema::trackID]) : 0, id);

            return places;
        };

        std::set<std::uint64_t> inBase, inAll, inOurs;

        for (const auto& track : getTrackElements (base))
            inBase.insert (readID (track[schema::trackID]));

        for (const auto& track : getTrackElements (song))
            if (const auto id = readID (track[schema::trackID]); inBase.count (id) > 0 && findTrack (theirs, id).isValid())
                inAll.insert (id);

        for (const auto& track : getTrackElements (ours))
            if (const auto id = readID (track[schema::trackID]); inAll.count (id) > 0)
                inOurs.insert (id);

        if (placesOf (theirs, inAll) == placesOf (base, inAll) || placesOf (ours, inOurs) != placesOf (base, inOurs))
            return;

        // Each track the other side placed, after the track before it there, in its folder there.
        for (const auto& track : getTrackElements (theirs))
        {
            const auto id = readID (track[schema::trackID]);

            if (inAll.count (id) == 0)
                continue;

            auto live = findTrack (song, id);
            auto parent = findTrackParent (song, track.getParent());

            if (live.getParent() != parent)
            {
                live.getParent().removeChild (live, nullptr);
                parent.addChild (live, -1, nullptr);
            }

            auto index = 0;

            for (auto before = track.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto found = parent.indexOf (findTrack (song, readID (before[schema::trackID]))); found >= 0)
                {
                    index = found + 1;
                    break;
                }
            }

            if (const auto current = parent.indexOf (live); current != index && current != index - 1)
                parent.moveChild (current, current < index ? index - 1 : index, nullptr);
        }
    }

    // Where both sides changed clips of a track: their beats, for a note that a merge can't hear whether they go together.
    juce::String findClipsChangedOnBoth (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs)
    {
        const auto changedRanges = [&base] (const juce::ValueTree& side)
        {
            std::vector<juce::Range<double>> ranges;
            std::map<juce::String, juce::ValueTree> baseClips;

            for (const auto& clip : base)
                if (clip.hasType (schema::CLIP))
                    baseClips.emplace (keyOf (clip), clip);

            const auto add = [&ranges] (const juce::ValueTree& clip)
            {
                const auto start = (double) clip[schema::clipStart];
                ranges.push_back ({ start, start + juce::jmax (1.0, (double) clip[schema::clipLength]) });
            };

            for (const auto& clip : side)
            {
                if (! clip.hasType (schema::CLIP))
                    continue;

                const auto found = baseClips.find (keyOf (clip));

                if (found == baseClips.end() || ! holdTheSame (found->second, clip))
                    add (clip);

                if (found != baseClips.end())
                    baseClips.erase (found);
            }

            for (const auto& [name, clip] : baseClips) // taken away
                add (clip);

            return ranges;
        };

        for (const auto& a : changedRanges (ours))
            for (const auto& b : changedRanges (theirs))
                if (const auto both = a.getIntersectionWith (b); ! both.isEmpty())
                    return "both sides changed its clips from beat " + juce::String (both.getStart() + 1.0, 0) + " to "
                           + juce::String (both.getEnd() + 1.0, 0) + ": listen to them together";

        return {};
    }

    // A song's clips in alias groups (schema::clipLink), by their groups, each group's in the song's order.
    void collectLinkedClips (const juce::ValueTree& tree, std::map<std::uint64_t, std::vector<juce::ValueTree>>& groups)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::CLIP))
            {
                if (child.hasProperty (schema::clipLink))
                    groups[readID (child[schema::clipLink])].push_back (child);
            }
            else if (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK))
            {
                collectLinkedClips (child, groups);
            }
        }
    }

    // An audio clip of an alias group given what its group's keeper shares with it (AudioClips.h: shareTheSound(): its file,
    // tempo, pitch, direction and warp markers), as it is given the keeper's notes: an alias one side added, which kept the
    // file the group played there, plays the one the other side located anew; and a clash in one of those properties of its
    // own is the keeper's, which has it too (on each side the group held the same), and is listed once, for the keeper.
    void keepSharedSound (MergeResult& result, const juce::ValueTree& keeper, juce::ValueTree clip)
    {
        if (! isAudioState (keeper) || ! isAudioState (clip))
            return;

        const auto path = getPath (clip);
        std::erase_if (result.clashes,
                       [&path] (const MergeClash& clash) { return clash.path == path && isSharedInAliasGroup (clash.property); });

        giveSharedSound (keeper, clip, nullptr);
    }

    // Each alias group of the merged song holding together: its clips' notes and controllers' values are one thing, which
    // the merge treats as one, and so is what audio clips of a group share (keepSharedSound()). They are its keeper's:
    // the first clip of the group, in the song's order, that the parting and
    // both sides all hold in that group, whose own were merged three ways, each side's edits in (on each side the group held
    // the same, so an edit made there through any of its clips reached the keeper too); failing that, the first both sides
    // hold in it, the first both sides hold at all (a clip one side linked to a new alias), or the first. Every other clip of
    // the group takes the keeper's (an alias one side added gets what the other side changed), and a clash in them is the
    // keeper's alone. A group left with one clip (its other clips deleted on one side) loses its link, as deleting a clip
    // unlinks the last one of its group. The IDs given anew to notes (renumbered, by the clip's group) stay right: the
    // keeper's notes are every clip's.
    void keepAliasGroups (MergeResult& result, const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs)
    {
        std::map<std::uint64_t, std::vector<juce::ValueTree>> groups;
        collectLinkedClips (result.song, groups);

        if (groups.empty())
            return;

        std::map<std::uint64_t, juce::ValueTree> baseClips, ourClips, theirClips;
        collectClips (base, baseClips);
        collectClips (ours, ourClips);
        collectClips (theirs, theirClips);

        for (auto& group : groups)
        {
            const auto groupLink = group.first;
            auto& clips = group.second;

            if (clips.size() == 1)
            {
                clips.front().removeProperty (schema::clipLink, nullptr);
                continue;
            }

            // Whether a song holds the clip (its ID), in this group or at all.
            const auto holds =
                [groupLink] (const std::map<std::uint64_t, juce::ValueTree>& songClips, const juce::ValueTree& clip, bool inGroup)
            {
                const auto found = songClips.find (readID (clip[schema::clipID]));

                return found != songClips.end()
                       && (! inGroup
                           || (found->second.hasProperty (schema::clipLink) && readID (found->second[schema::clipLink]) == groupLink));
            };

            const auto findFirst = [&clips] (const std::function<bool (const juce::ValueTree&)>& is)
            {
                for (const auto& clip : clips)
                    if (is (clip))
                        return clip;

                return juce::ValueTree();
            };

            auto keeper =
                findFirst ([&] (const juce::ValueTree& clip)
                           { return holds (baseClips, clip, true) && holds (ourClips, clip, true) && holds (theirClips, clip, true); });

            if (! keeper.isValid())
                keeper = findFirst ([&] (const juce::ValueTree& clip)
                                    { return holds (ourClips, clip, true) && holds (theirClips, clip, true); });

            if (! keeper.isValid())
                keeper = findFirst ([&] (const juce::ValueTree& clip)
                                    { return holds (ourClips, clip, false) && holds (theirClips, clip, false); });

            if (! keeper.isValid())
                keeper = clips.front();

            for (auto& clip : clips)
            {
                if (clip == keeper)
                    continue;

                // A clash in what the clip holds (a note's value) is the keeper's, whose events it now holds.
                const auto step = keyOf (clip);
                std::erase_if (result.clashes,
                               [&step] (const MergeClash& clash)
                               {
                                   const auto at = clash.path.indexOf (step);
                                   return at >= 0 && at < clash.path.size() - 1;
                               });

                giveClipEvents (keeper, clip, nullptr);
                keepSharedSound (result, keeper, clip);
            }
        }
    }
} // namespace

MergeResult mergeSongs (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                        const std::function<std::uint64_t()>& makeID)
{
    return mergeSongs (base, ours, theirs, findChanges (base, theirs), makeID);
}

MergeResult mergeSongs (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                        const std::vector<SongChange>& lines, const std::function<std::uint64_t()>& makeID)
{
    // Every line the other side changed: its tracks (the other side's IDs, and the master's) and the song's own.
    std::set<std::uint64_t> lineTracks;
    auto songOwn = false;

    for (const auto& line : lines)
    {
        if (line.where == SongChange::Where::song)
            songOwn = true;
        else
            lineTracks.insert (line.trackID);
    }

    MergeResult result;
    result.song = ours.createCopy();
    // New IDs past every ID either side holds: the other side may have counted further than this one.
    const auto floor = juce::jmax (findNextID (theirs), findNextID (ours));
    const std::function<std::uint64_t()> makeNewID = [&makeID, floor]
    {
        auto id = makeID();

        while (id < floor)
            id = makeID();

        return id;
    };

    const auto renumbered = renumberAdditions (base, ours, theirs, makeNewID);
    const auto& other = renumbered.song;

    // What it gave anew, which its commit keeps.
    for (const auto& [from, to] : renumbered.ids)
        result.renumbered.push_back ({ {}, from, to });

    for (const auto& [note, to] : renumbered.noteIDs)
        result.renumbered.push_back ({ note.first, note.second, to });

    // What can be somewhere else on one side than in the base, for each side's moves to be told from what it took away and
    // added: no item is in the result twice.
    Items baseItems, theirItems, ourItems;
    collectMovable (base, baseItems);
    collectMovable (other, theirItems);
    collectMovable (result.song, ourItems);

    // The lines' tracks in the other side's order, top to bottom, so that a folder it added is here before the tracks it
    // added in it, which go into it; then those it took away, and the master.
    std::set<std::uint64_t> wanted;

    for (const auto requested : lineTracks)
    {
        const auto found = renumbered.ids.find (requested);
        wanted.insert (found != renumbered.ids.end() ? found->second : requested);
    }

    std::vector<std::uint64_t> tracks;

    for (const auto& track : getTrackElements (other))
        if (const auto id = readID (track[schema::trackID]); wanted.erase (id) > 0)
            tracks.push_back (id);

    tracks.insert (tracks.end(), wanted.begin(), wanted.end());

    for (const auto id : tracks)
    {
        const auto b = findTrack (base, id), t = findTrack (other, id);
        auto o = findTrack (result.song, id);
        Merger merger { result, getTrackNameIn (t.isValid() ? other : base, t.isValid() ? t : b), id, baseItems, theirItems, ourItems };

        auto devices = getAutomatableIDs (b);
        devices.merge (getAutomatableIDs (t));
        devices.merge (getAutomatableIDs (o));

        if (! b.isValid() && t.isValid() && ! o.isValid())
        {
            // Added there: here too, after the track before it there, with the tracks in it this side hasn't; what it holds
            // that this side has somewhere else (a clip moved onto it there) is this side's.
            merger.takeOurs (addTrackCopy (result.song, t));
        }
        else if (b.isValid() && ! t.isValid() && o.isValid())
        {
            // Taken away there: it goes, unless it changed here, by this side's own song (the merge may have taken from it
            // already what went somewhere else there). The tracks in it, a folder's, are lines of their own, and stay.
            const auto mine = findTrack (ours, id);

            if (holdTheSameOwn (b, mine.isValid() ? mine : o))
                removeTrackOwn (o);
            else
                merger.note ("changed here and taken away there: kept");
        }
        else if (b.isValid() && t.isValid() && o.isValid())
        {
            merger.merge (b, o, t, false);

            if (const auto both = findClipsChangedOnBoth (b, findTrack (ours, id), t); both.isNotEmpty())
                merger.note (both);
        }
        else if (b.isValid() && t.isValid() && ! holdTheSameOwn (b, t))
        {
            // Taken away here, changed there: back, where it is there, without the tracks in it that this side has somewhere
            // (a folder ungrouped here), and without what this side moved out of it, which stays where this side put it.
            merger.takeOurs (addTrackCopy (result.song, t));
            merger.note ("taken away here and changed there: kept");
        }

        mergeCurves (base, result.song, other, devices, merger);
    }

    if (songOwn)
    {
        Merger merger { result, "Song", 0, baseItems, theirItems, ourItems };
        merger.mergeProperties (base, result.song, other);

        // Its tempo map's tempos and time signatures, its sections, its chord lane, the key set last and its MIDI links, by
        // their controllers, each whole (a second link of a parameter, one from each side, goes as the merge is applied:
        // forgetWhatNamesTheGone()).
        for (const auto* type : { &schema::TEMPOMAP, &schema::SECTIONS, &schema::CHORDLANE, &schema::LASTKEY, &schema::MIDILINKS })
        {
            const auto b = base.getChildWithName (*type), t = other.getChildWithName (*type);
            auto o = result.song.getChildWithName (*type);
            auto keyed = std::map<juce::String, juce::ValueTree> {};

            if (mergesWhole (b.isValid() ? b : t.isValid() ? t : o))
            {
                if (o.isValid())
                    keyed.emplace (keyOf (o), o);

                merger.mergeWhole (result.song, b, o, t, keyed);
            }
            else if (t.isValid() && ! o.isValid())
            {
                result.song.appendChild (t.createCopy(), nullptr);
            }
            else if (t.isValid() && o.isValid())
            {
                merger.merge (b.isValid() ? b : juce::ValueTree (*type), o, t);
            }
        }

        mergeTrackOrder (base, ours, result.song, other);
    }

    // Last, once every line's clips are where the merge puts them and each clip's notes were merged: a group's notes are its
    // keeper's, by the other side's song as renumbered (its clips' IDs and links as this side's), whose notes' IDs given anew
    // (result.renumbered, recorded above by the clip's group) the keeper holds and every clip of its group takes.
    keepAliasGroups (result, base, ours, other);
    return result;
}

void translateIDs (juce::ValueTree song, const std::vector<RenumberedID>& renumbered)
{
    CopiedIDs items;
    std::map<std::pair<juce::String, std::uint64_t>, std::uint64_t> notes;

    for (const auto& id : renumbered)
    {
        if (id.clip.isEmpty())
            items[id.from] = id.to;
        else
            notes[{ id.clip, id.from }] = id.to;
    }

    if (items.empty() && notes.empty())
        return;

    // Items first, what holds an ID and what names one, so that a clip's alias group is numbered as this side's; then each
    // clip's notes, by that group.
    const std::function<void (juce::ValueTree)> translateItems = [&] (juce::ValueTree tree)
    {
        for (auto child : tree)
        {
            for (int i = 0; i < child.getNumProperties(); ++i)
            {
                const auto property = child.getPropertyName (i);

                if ((schema::holdsID (child, property) && ! child.hasType (schema::NOTE)) || schema::namesID (child, property))
                    if (const auto found = items.find (readID (child[property])); found != items.end())
                        child.setProperty (property, writeIDLike (child[property], found->second), nullptr);
            }

            translateItems (child);
        }
    };

    const std::function<void (juce::ValueTree)> translateNotes = [&] (juce::ValueTree tree)
    {
        for (auto child : tree)
        {
            if (! child.hasType (schema::CLIP))
            {
                translateNotes (child);
                continue;
            }

            const auto group = getNoteGroup (child);

            for (auto note : child)
                if (note.hasType (schema::NOTE))
                    if (const auto found = notes.find ({ group, readID (note[schema::noteID]) }); found != notes.end())
                        note.setProperty (schema::noteID, (juce::int64) found->second, nullptr);
        }
    };

    translateItems (song);
    translateNotes (song);
}

std::vector<RenumberedID> findLatestWords (const std::vector<RenumberedID>& recorded)
{
    // The later record's word for each ID; then, left out, what isn't one to one: an ID 0, and every ID two IDs were given.
    std::map<std::pair<juce::String, std::uint64_t>, std::uint64_t> latest;

    for (const auto& id : recorded)
        latest[{ id.clip, id.from }] = id.to;

    std::map<std::pair<juce::String, std::uint64_t>, int> targets;

    for (const auto& [from, to] : latest)
        ++targets[{ from.first, to }];

    std::vector<RenumberedID> words;

    for (const auto& [from, to] : latest)
        if (from.second != 0 && to != 0 && targets[{ from.first, to }] == 1)
            words.push_back ({ from.first, from.second, to });

    return words;
}

bool areOneToOne (const std::vector<RenumberedID>& ids)
{
    std::set<std::pair<juce::String, std::uint64_t>> froms, tos;

    for (const auto& id : ids)
        if (id.from == 0 || id.to == 0 || ! froms.insert ({ id.clip, id.from }).second || ! tos.insert ({ id.clip, id.to }).second)
            return false;

    return true;
}

PluginParameterValues translateKnobs (PluginParameterValues knobs, const std::vector<RenumberedID>& renumbered)
{
    CopiedIDs items;

    for (const auto& id : renumbered)
        if (id.clip.isEmpty())
            items[id.from] = id.to;

    PluginParameterValues translated;

    for (auto& [device, values] : knobs)
    {
        const auto to = items.find (device);
        translated[to != items.end() ? to->second : device] = std::move (values);
    }

    return translated;
}

Translation findTranslation (const juce::ValueTree& base, const juce::ValueTree& theirs, const std::vector<RenumberedID>& recorded,
                             const std::function<std::uint64_t()>& makeID, const std::vector<RenumberedID>& crossed)
{
    // What was recorded, the later merge's word for an ID over an earlier's, without what isn't one to one (a record of a
    // history another wrote, damaged), which would make two items one.
    CopiedIDs items;
    std::map<std::pair<juce::String, std::uint64_t>, std::uint64_t> notes;

    for (const auto& id : findLatestWords (recorded))
    {
        if (id.clip.isEmpty())
            items[id.from] = id.to;
        else
            notes[{ id.clip, id.from }] = id.to;
    }

    // The items' IDs the parting holds, alone: what the other line made since is no item a merge numbered. Of the other
    // side's, not those it was translated to first (crossed), each the item this side has under that ID already.
    std::set<std::uint64_t> held, inEither, theirIDs;
    collectIDs (base, held);
    collectIDs (base, inEither);
    collectIDs (theirs, theirIDs);
    std::set<std::pair<juce::String, std::uint64_t>> crossedTo;

    for (const auto& id : crossed)
        crossedTo.insert ({ id.clip, id.to });

    for (const auto id : theirIDs)
        if (crossedTo.count ({ {}, id }) == 0)
            inEither.insert (id);

    std::erase_if (items, [&held] (const auto& entry) { return held.count (entry.first) == 0; });

    Translation result;
    std::set<std::uint64_t> targets;

    for (const auto& [from, to] : items)
        targets.insert (to);

    for (const auto id : inEither)
        if (targets.count (id) > 0 && items.count (id) == 0)
            result.givenAnew.push_back ({ {}, id, makeID() });

    for (const auto& id : result.givenAnew)
        items[id.from] = id.to;

    // The notes, by their clips' groups as the songs are numbered once their items are translated (getNoteGroup()).
    using NoteKey = std::pair<juce::String, std::uint64_t>;

    const auto collectNotes = [&items] (const juce::ValueTree& song, std::set<NoteKey>& found)
    {
        const auto translated = [&items] (const juce::var& value)
        {
            const auto id = readID (value);
            const auto to = items.find (id);
            return juce::String ((juce::uint64) (to != items.end() ? to->second : id));
        };

        std::map<std::uint64_t, juce::ValueTree> clips;
        collectClips (song, clips);

        for (const auto& [id, clip] : clips)
        {
            const auto group = clip.hasProperty (schema::clipLink) ? "link " + translated (clip[schema::clipLink])
                                                                   : "clip " + translated (clip[schema::clipID]);

            for (const auto& note : clip)
                if (note.hasType (schema::NOTE))
                    found.insert ({ group, readID (note[schema::noteID]) });
        }
    };

    std::set<NoteKey> heldNotes, notesInEither, theirNotes;
    collectNotes (base, heldNotes);
    collectNotes (base, notesInEither);
    collectNotes (theirs, theirNotes);

    // The notes the other side's song was translated to first, keyed by their clips as these songs are numbered then: as
    // translated here.
    std::set<NoteKey> notesCrossedTo;

    for (const auto& id : crossed)
    {
        if (const auto space = id.clip.lastIndexOfChar (' '); space >= 0)
        {
            const auto to = items.find ((std::uint64_t) id.clip.substring (space + 1).getLargeIntValue());
            notesCrossedTo.insert (
                { to != items.end() ? id.clip.substring (0, space + 1) + juce::String ((juce::uint64) to->second) : id.clip, id.to });
        }
    }

    for (const auto& note : theirNotes)
        if (notesCrossedTo.count (note) == 0)
            notesInEither.insert (note);
    std::erase_if (notes, [&heldNotes] (const auto& entry) { return heldNotes.count (entry.first) == 0; });

    std::set<NoteKey> noteTargets;

    for (const auto& [note, to] : notes)
        noteTargets.insert ({ note.first, to });

    std::vector<RenumberedID> notesGivenAnew;

    for (const auto& note : notesInEither)
        if (noteTargets.count (note) > 0 && notes.count (note) == 0)
            notesGivenAnew.push_back ({ note.first, note.second, makeID() });

    for (const auto& id : notesGivenAnew)
        notes[{ id.clip, id.from }] = id.to;

    result.givenAnew.insert (result.givenAnew.end(), notesGivenAnew.begin(), notesGivenAnew.end());

    for (const auto& [from, to] : items)
        result.all.push_back ({ {}, from, to });

    for (const auto& [note, to] : notes)
        result.all.push_back ({ note.first, note.second, to });

    return result;
}

juce::ValueTree findByPath (const juce::ValueTree& song, const juce::StringArray& path)
{
    auto element = song;

    for (const auto& step : path)
    {
        juce::ValueTree next;

        for (const auto& child : element)
        {
            if (keyOf (child) == step)
            {
                next = child;
                break;
            }
        }

        if (! next.isValid())
            return {};

        element = next;
    }

    return element;
}
} // namespace phazite
