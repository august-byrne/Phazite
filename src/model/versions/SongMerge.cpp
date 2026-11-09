// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongMerge.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/mixer/internal/Mixer.h"

#include <cmath>
#include <map>

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

    // What merges whole: a curve with its points, the chord lane, the key set last.
    bool mergesWhole (const juce::ValueTree& element)
    {
        return element.hasType (schema::CURVE) || element.hasType (schema::CHORDLANE) || element.hasType (schema::LASTKEY);
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

        return property.isValid() ? property.toString() : element.getType().toString().toLowerCase();
    }

    //==============================================================================
    struct Merger
    {
        MergeResult& result;
        juce::String line;
        std::uint64_t trackID = 0;

        void note (const juce::String& text) { result.notes.addIfNotAlreadyThere (line + ": " + text); }

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

        // Puts a copy of the other side's child in r after the child before it there that r has.
        static juce::ValueTree insertAfterBefore (juce::ValueTree r, const juce::ValueTree& theirs,
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

            auto copy = theirs.createCopy();
            r.addChild (copy, index, nullptr);
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

            // The other side's children in its order first, for what it added to go after what it followed, then the others.
            std::vector<juce::String> names;

            for (const auto& child : t)
                if (const auto name = keyOf (child); ct.keyed.count (name) > 0)
                    names.push_back (name);

            for (const auto* children : { &cb, &cr })
                for (const auto& [name, child] : children->keyed)
                    if (std::find (names.begin(), names.end(), name) == names.end())
                        names.push_back (name);

            for (const auto& name : names)
            {
                const auto findIn = [&name] (const Children& children)
                {
                    const auto found = children.keyed.find (name);
                    return found != children.keyed.end() ? found->second : juce::ValueTree();
                };

                const auto inBase = findIn (cb), ours = findIn (cr), theirs = findIn (ct);
                const auto& any = theirs.isValid() ? theirs : ours.isValid() ? ours : inBase;

                if (mergesWhole (any))
                {
                    mergeWhole (r, inBase, ours, theirs, keyed);
                    continue;
                }

                if (! inBase.isValid())
                {
                    if (theirs.isValid() && ! ours.isValid())
                        keyed[name] = insertAfterBefore (r, theirs, keyed); // theirs, added
                    else if (theirs.isValid() && ours.isValid() && ! holdTheSame (ours, theirs))
                        merge ({}, ours, theirs); // added on both sides under one key: a parameter's value, a tempo at a beat

                    continue;
                }

                if (ours.isValid() && theirs.isValid())
                {
                    merge (inBase, ours, theirs);

                    // Values set back to their defaults, some on each side, can leave none: the song keeps no VALUES without
                    // values (model/devices/internal/Parameters.h: keepValueSetByHand()).
                    if (ours.hasType (schema::VALUES) && ours.getNumProperties() == 0 && ours.getNumChildren() == 0)
                    {
                        r.removeChild (ours, nullptr);
                        keyed.erase (name);
                    }
                }
                else if (ours.isValid())
                {
                    // Taken away there: it goes, unless it changed here.
                    if (holdTheSame (inBase, ours))
                    {
                        r.removeChild (ours, nullptr);
                        keyed.erase (name);
                    }
                    else
                    {
                        note (describeClash (ours, {}) + " changed here and taken away there: kept");
                    }
                }
                else if (theirs.isValid() && ! holdTheSame (inBase, theirs))
                {
                    // Taken away here, changed there: back, as it changed.
                    keyed[name] = insertAfterBefore (r, theirs, keyed);
                    note (describeClash (theirs, {}) + " taken away here and changed there: kept");
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

                    if (previous == keyed.end() || child == keyed.end())
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
    // all three have, by their folders and places.
    void mergeTrackOrder (const juce::ValueTree& base, juce::ValueTree song, const juce::ValueTree& theirs)
    {
        const auto placesOf = [] (const juce::ValueTree& tree, const std::set<std::uint64_t>& only)
        {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> places; // folder (0 for none), track

            for (const auto& track : getTrackElements (tree))
                if (const auto id = readID (track[schema::trackID]); only.count (id) > 0)
                    places.emplace_back (track.getParent().hasType (schema::TRACK) ? readID (track.getParent()[schema::trackID]) : 0, id);

            return places;
        };

        std::set<std::uint64_t> inBase, inAll;

        for (const auto& track : getTrackElements (base))
            inBase.insert (readID (track[schema::trackID]));

        for (const auto& track : getTrackElements (song))
            if (const auto id = readID (track[schema::trackID]); inBase.count (id) > 0 && findTrack (theirs, id).isValid())
                inAll.insert (id);

        const auto baseOrder = placesOf (base, inAll), ourOrder = placesOf (song, inAll), theirOrder = placesOf (theirs, inAll);

        if (theirOrder == baseOrder || ourOrder != baseOrder)
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
} // namespace

MergeResult mergeSongs (const juce::ValueTree& base, const juce::ValueTree& ours, const juce::ValueTree& theirs, const MergeLines& lines,
                        const std::function<std::uint64_t()>& makeID)
{
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

    for (const auto requested : lines.tracks)
    {
        const auto found = renumbered.ids.find (requested);
        const auto id = found != renumbered.ids.end() ? found->second : requested;
        const auto b = findTrack (base, id), t = findTrack (other, id);
        auto o = findTrack (result.song, id);
        Merger merger { result, getTrackNameIn (t.isValid() ? other : base, t.isValid() ? t : b), id };

        auto devices = getAutomatableIDs (b);
        devices.merge (getAutomatableIDs (t));
        devices.merge (getAutomatableIDs (o));

        if (! b.isValid() && t.isValid() && ! o.isValid())
        {
            // Added there: here too, after the track before it there, with the tracks in it this side hasn't.
            auto copy = t.createCopy();

            for (int i = copy.getNumChildren(); --i >= 0;)
                if (const auto child = copy.getChild (i);
                    child.hasType (schema::TRACK) && findTrack (result.song, readID (child[schema::trackID])).isValid())
                    copy.removeChild (i, nullptr);

            auto parent = findTrackParent (result.song, t.getParent());
            auto index = 0;

            for (auto before = t.getSibling (-1); before.isValid(); before = before.getSibling (-1))
            {
                if (const auto place = parent.indexOf (findTrack (result.song, readID (before[schema::trackID]))); place >= 0)
                {
                    index = place + 1;
                    break;
                }
            }

            parent.addChild (copy, index, nullptr);
        }
        else if (b.isValid() && ! t.isValid() && o.isValid())
        {
            // Taken away there: it goes, unless it changed here.
            if (holdTheSameOwn (b, o))
                o.getParent().removeChild (o, nullptr);
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
            // Taken away here, changed there: back.
            findTrackParent (result.song, t.getParent()).appendChild (t.createCopy(), nullptr);
            merger.note ("taken away here and changed there: kept");
        }

        mergeCurves (base, result.song, other, devices, merger);
    }

    if (lines.songOwn)
    {
        Merger merger { result, "Song", 0 };
        merger.mergeProperties (base, result.song, other);

        // Its tempo map's tempos and time signatures, its sections, its chord lane and the key set last.
        for (const auto* type : { &schema::TEMPOMAP, &schema::SECTIONS, &schema::CHORDLANE, &schema::LASTKEY })
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

        mergeTrackOrder (base, result.song, other);
    }

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
