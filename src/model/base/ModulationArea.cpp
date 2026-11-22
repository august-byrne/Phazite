// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ModulationArea.h"
#include "Song.h"
#include "internal/Schema.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    using Change = SongValues::ModulationChange;

    bool isModulatorElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO);
    }

    // Whether the song's readers read one (ModulationValue.h: readModulatorValue()): a macro, or a modulator of a kind Phazite
    // plays.
    bool isRead (const juce::ValueTree& element)
    {
        return element.hasType (schema::MACRO) || readModulatorKind (element).has_value();
    }

    ModulatorID readModulatorID (const juce::ValueTree& element)
    {
        return ModulatorID::fromRaw (readID (element[schema::modulatorID]));
    }

    bool isFolder (const juce::ValueTree& track)
    {
        return track[schema::trackKind].toString() == schema::trackIsFolder;
    }

    // A channel of the song's, as readModulationValue() reads them: the MASTER of its PHAZITE element, a TRACK of its track
    // list, or a TRACK in a folder that is one.
    bool isSongChannel (const juce::ValueTree& element)
    {
        if (element.hasType (schema::MASTER))
            return element.getParent().hasType (schema::PHAZITE);

        if (! element.hasType (schema::TRACK))
            return false;

        const auto parent = element.getParent();

        if (parent.hasType (schema::TRACKS))
            return parent.getParent().hasType (schema::PHAZITE);

        return parent.hasType (schema::TRACK) && isFolder (parent) && isSongChannel (parent);
    }

    // What holds the song's modulators and macros: a channel of the song's, or a CONTAINER on one.
    bool isSongOwner (const juce::ValueTree& element)
    {
        return isSongChannel (element) || (element.hasType (schema::CONTAINER) && isSongChannel (element.getParent()));
    }

    // The song's MODULATOR or MACRO an element is, or is in (its VALUES, a MODULATES); an invalid tree for one in none. Looked
    // for upwards only as far as what holds no modulator inside it, as a fader's change is heard often.
    juce::ValueTree findSongModulator (const juce::ValueTree& element)
    {
        for (auto each = element; each.isValid(); each = each.getParent())
        {
            if (isModulatorElement (each))
                return isSongOwner (each.getParent()) ? each : juce::ValueTree();

            if (each.hasType (schema::TRACK) || each.hasType (schema::MASTER) || each.hasType (schema::CONTAINER)
                || each.hasType (schema::DEVICE) || each.hasType (schema::CHAIN) || each.hasType (schema::TRACKS)
                || each.hasType (schema::PHAZITE))
                return {};
        }

        return {};
    }

    // An owner's place, as readModulatorPlace() reads its modulators': a channel's, or a container's on its channel.
    ModulatorPlace readOwnerPlace (const juce::ValueTree& owner)
    {
        ModulatorPlace place;
        auto channel = owner;

        if (owner.hasType (schema::CONTAINER))
        {
            place.container = DeviceID::fromRaw (readID (owner[schema::deviceID]));
            channel = owner.getParent();
        }

        if (channel.hasType (schema::MASTER))
            place.onMaster = true;
        else if (channel.hasType (schema::TRACK))
            place.track = TrackID::fromRaw (readID (channel[schema::trackID]));

        return place;
    }

    // The list an owner's element names, without its parent (one taken out of the song): a container's by its ID, a
    // channel's by its track's, the master's by its type.
    ModulatorPlace readOwnerKey (const juce::ValueTree& owner)
    {
        ModulatorPlace key;

        if (owner.hasType (schema::CONTAINER))
            key.container = DeviceID::fromRaw (readID (owner[schema::deviceID]));
        else if (owner.hasType (schema::MASTER))
            key.onMaster = true;
        else
            key.track = TrackID::fromRaw (readID (owner[schema::trackID]));

        return key;
    }

    // An owner's modulators and macros that the song's readers read, each ID once (the first), in its element's order.
    std::vector<std::pair<ModulatorID, juce::ValueTree>> readOwned (const juce::ValueTree& owner)
    {
        std::vector<std::pair<ModulatorID, juce::ValueTree>> owned;

        for (const auto& child : owner)
            if (isModulatorElement (child) && isRead (child))
                if (const auto id = readModulatorID (child);
                    std::none_of (owned.begin(), owned.end(), [id] (const auto& each) { return each.first == id; }))
                    owned.emplace_back (id, child);

        return owned;
    }

    bool holdSameValues (const ModulatorValue& a, const ModulatorValue& b)
    {
        if (a.values.size() != b.values.size())
            return false;

        for (const auto& [index, held] : a.values)
            if (const auto* other = b.values.find (index); other == nullptr || ! juce::exactlyEqual (held, *other))
                return false;

        return true;
    }

    bool holdSameSteps (const ModulatorValue& a, const ModulatorValue& b)
    {
        if (a.steps.size() != b.steps.size())
            return false;

        for (size_t i = 0; i < a.steps.size(); ++i)
            if (! juce::exactlyEqual (a.steps[i], b.steps[i]))
                return false;

        return true;
    }

    // What of a modulator or a macro differs between two of its values (the same ID).
    std::uint16_t findParts (const ModulatorValue& was, const ModulatorValue& now)
    {
        std::uint16_t parts = 0;
        const auto add = [&parts] (std::uint16_t part) { parts = (std::uint16_t) (parts | part); };

        if (! (was.place == now.place))
            add (Change::made);

        if (was.kind != now.kind)
            add (Change::kind);

        if (was.name != now.name)
            add (Change::name);

        if (was.off != now.off)
            add (Change::off);

        if (! holdSameValues (was, now))
        {
            add (Change::valuesSetByHand);

            if (was.values.empty() != now.values.empty())
                add (Change::valuesAppeared);
        }

        if (! holdSameSteps (was, now))
            add (Change::steps);

        if (! juce::exactlyEqual (was.value, now.value))
            add (Change::macroValue);

        if (! (was.modulations == now.modulations))
            add (Change::modulations);

        return parts;
    }

    // Each owner in a song's tree (PHAZITE) as readModulationValue() reads them: each channel of its track list (a folder's
    // tracks after it), then the master, each followed by its containers; until `each` returns true, whether it did.
    bool findOwner (const juce::ValueTree& song, const std::function<bool (const juce::ValueTree&)>& each)
    {
        const auto inChannel = [&each] (const juce::ValueTree& channel)
        {
            if (each (channel))
                return true;

            for (const auto& child : channel)
                if (child.hasType (schema::CONTAINER) && each (child))
                    return true;

            return false;
        };

        const std::function<bool (const juce::ValueTree&)> inTracks = [&inChannel, &inTracks] (const juce::ValueTree& parent)
        {
            for (const auto& child : parent)
                if (child.hasType (schema::TRACK) && (inChannel (child) || (isFolder (child) && inTracks (child))))
                    return true;

            return false;
        };

        if (inTracks (song.getChildWithName (schema::TRACKS)))
            return true;

        const auto master = song.getChildWithName (schema::MASTER);
        return master.isValid() && inChannel (master);
    }
} // namespace

//==============================================================================
SongValues::ModulationKeeper::ModulationKeeper (SongValues& v, Song& s)
    : values (v),
      told (readModulationValue (s.getState()))
{
}

void SongValues::ModulationKeeper::propertyChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    Heard heard;

    if (const auto modulator = findSongModulator (tree); modulator.isValid())
    {
        // Given another ID: its owner's list. Anything else (its type too, which reread() takes to its owner's list where it
        // comes to be read or not): itself.
        if (modulator == tree && property == schema::modulatorID)
            resync (modulator.getParent(), heard);
        else
            reread (modulator, heard);
    }
    else if (tree.hasType (schema::TRACK) && property == schema::trackID && isSongChannel (tree))
    {
        // A channel given another ID (a merge's renumbering): its own and its containers' come to another place.
        resync (tree, heard);

        for (const auto& child : tree)
            if (child.hasType (schema::CONTAINER))
                resync (child, heard);
    }
    else if (tree.hasType (schema::CONTAINER) && property == schema::deviceID)
    {
        resync (tree, heard); // a container given another ID: its macros
    }

    tell (heard);
}

void SongValues::ModulationKeeper::childAdded (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    Heard heard;

    if (isModulatorElement (child))
        resync (parent, heard); // one added to its owner
    else if (const auto modulator = findSongModulator (parent); modulator.isValid())
        reread (modulator, heard); // its VALUES, a MODULATES, or what is in one
    else if (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS) || child.hasType (schema::MASTER)
             || child.hasType (schema::CONTAINER))
        resyncAllIn (child, heard); // owners that came with theirs

    tell (heard);
}

void SongValues::ModulationKeeper::childRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    Heard heard;

    if (isModulatorElement (child))
        resync (parent, heard);
    else if (const auto modulator = findSongModulator (parent); modulator.isValid())
        reread (modulator, heard);
    else if (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS) || child.hasType (schema::MASTER)
             || child.hasType (schema::CONTAINER))
        forgetAllIn (child, heard); // owners that went with theirs

    tell (heard);
}

void SongValues::ModulationKeeper::childOrderChanged (const juce::ValueTree& parent)
{
    Heard heard;

    if (isSongOwner (parent))
        resync (parent, heard); // its modulators and macros in another order, or something else of it (only the IDs read)
    else if (const auto modulator = findSongModulator (parent); modulator.isValid())
        reread (modulator, heard); // its modulations in another order

    tell (heard);
}

void SongValues::ModulationKeeper::reread (const juce::ValueTree& modulator, Heard& heard)
{
    const auto id = readModulatorID (modulator);
    const auto* was = told.find (id);
    auto now = readModulatorValue (modulator, readModulatorPlace (modulator));

    // One the listeners didn't know, or that isn't read now: what its owner holds.
    if (was == nullptr || ! now.has_value())
        return resync (modulator.getParent(), heard);

    if (const auto parts = findParts (*was, *now); parts != 0)
    {
        heard.changes.push_back (
            { .id = id, .before = was->place, .after = now->place, .wasThere = true, .isThere = true, .parts = parts });
        told = withModulator (told, std::move (*now));
    }
}

void SongValues::ModulationKeeper::resync (const juce::ValueTree& owner, Heard& heard)
{
    if (! isSongOwner (owner))
        return;

    const auto place = readOwnerPlace (owner);
    const auto owned = readOwned (owner);
    const auto holds = [&owned] (ModulatorID id)
    { return std::any_of (owned.begin(), owned.end(), [id] (const auto& each) { return each.first == id; }); };

    std::vector<ModulatorID> before;

    if (const auto* listed = told.findList (place))
        before.assign (listed->begin(), listed->end());

    // Those it held that it holds no longer: taken out.
    for (const auto id : before)
    {
        if (holds (id))
            continue;

        if (const auto* was = told.find (id))
            heard.changes.push_back ({ .id = id, .before = was->place, .wasThere = true, .parts = Change::made });

        told = withoutModulator (told, id);
    }

    // Those that came, from nowhere or from another place: read, each.
    for (const auto& [id, element] : owned)
    {
        const auto* was = told.find (id);

        if (was != nullptr && was->place == place)
            continue;

        auto value = readModulatorValue (element, place);

        if (! value.has_value())
            continue;

        heard.changes.push_back ({ .id = id,
                                   .before = was != nullptr ? was->place : ModulatorPlace(),
                                   .after = place,
                                   .wasThere = was != nullptr,
                                   .isThere = true,
                                   .parts = Change::made });
        told = withModulator (told, std::move (*value));
    }

    // Its list in its element's order; reordered where those it held before are in another order.
    const auto* listed = told.findList (place);

    if (listed == nullptr)
        return;

    std::vector<ModulatorID> order;

    for (const auto& each : owned)
        if (const auto* value = told.find (each.first); value != nullptr && value->place == place)
            order.push_back (each.first);

    if (order == std::vector<ModulatorID> (listed->begin(), listed->end()))
        return;

    told = withOwnerOrder (told, place, immer::flex_vector<ModulatorID> (order.begin(), order.end()));

    std::vector<ModulatorID> keptBefore, keptNow;
    std::copy_if (before.begin(), before.end(), std::back_inserter (keptBefore), holds);
    std::copy_if (order.begin(), order.end(), std::back_inserter (keptNow),
                  [&before] (ModulatorID id) { return std::find (before.begin(), before.end(), id) != before.end(); });

    if (keptBefore != keptNow)
        heard.reordered.push_back (place);
}

void SongValues::ModulationKeeper::resyncAllIn (const juce::ValueTree& element, Heard& heard)
{
    // The track list: its tracks.
    if (element.hasType (schema::TRACKS))
    {
        for (const auto& child : element)
            resyncAllIn (child, heard);

        return;
    }

    // A channel: its own, its containers' and, a folder's, its tracks'.
    if (isSongChannel (element))
    {
        resync (element, heard);

        for (const auto& child : element)
        {
            if (child.hasType (schema::CONTAINER))
                resync (child, heard);
            else if (child.hasType (schema::TRACK))
                resyncAllIn (child, heard);
        }

        return;
    }

    if (element.hasType (schema::CONTAINER))
        resync (element, heard);
}

void SongValues::ModulationKeeper::forgetAllIn (const juce::ValueTree& element, Heard& heard)
{
    // An owner taken out of the song: each of its list the listeners knew, taken out, without a read (its element has no
    // parent now to say where it was: its list says).
    const auto forgetOwner = [this, &heard] (const juce::ValueTree& owner)
    {
        const auto* listed = told.findList (readOwnerKey (owner));

        if (listed == nullptr)
            return;

        const std::vector<ModulatorID> ids (listed->begin(), listed->end());

        for (const auto id : ids)
        {
            if (const auto* was = told.find (id))
                heard.changes.push_back ({ .id = id, .before = was->place, .wasThere = true, .parts = Change::made });

            told = withoutModulator (told, id);
        }
    };

    if (element.hasType (schema::CONTAINER))
        return forgetOwner (element);

    if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER))
        forgetOwner (element);

    if (element.hasType (schema::TRACK) || element.hasType (schema::MASTER) || element.hasType (schema::TRACKS))
        for (const auto& child : element)
            if (child.hasType (schema::CONTAINER) || child.hasType (schema::TRACK))
                forgetAllIn (child, heard);
}

void SongValues::ModulationKeeper::tell (Heard& heard) const
{
    if (heard.changes.empty() && heard.reordered.empty())
        return;

    // Each value after, in a copy of what the listeners are told now, which stays while they are told whatever one of them
    // changes meanwhile (cheap: it shares what it holds).
    const auto now = told;

    for (auto& change : heard.changes)
        change.now = change.isThere ? now.find (change.id) : nullptr;

    values.listeners.call ([&heard] (SongValues::Listener& listener) { listener.modulationChanged (heard.changes, heard.reordered); });
}

//==============================================================================
bool isHeardAsModulation (const juce::ValueTree& element)
{
    return findSongModulator (element).isValid();
}

bool isHeardAsModulation (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    return (isModulatorElement (child) && isSongOwner (parent)) || isHeardAsModulation (parent);
}

//==============================================================================
juce::ValueTree findModulatorElement (const juce::ValueTree& song, ModulatorID id)
{
    juce::ValueTree found;

    findOwner (song,
               [&found, id] (const juce::ValueTree& owner)
               {
                   for (const auto& child : owner)
                   {
                       if (isModulatorElement (child) && isRead (child) && readModulatorID (child) == id)
                       {
                           found = child;
                           return true;
                       }
                   }

                   return false;
               });

    return found;
}

std::vector<ModulatorID> readOwnerOrder (const juce::ValueTree& song, const ModulatorPlace& owner)
{
    std::vector<ModulatorID> order;

    findOwner (song,
               [&order, &owner] (const juce::ValueTree& candidate)
               {
                   if (! (readOwnerPlace (candidate) == owner))
                       return false;

                   for (const auto& each : readOwned (candidate))
                       order.push_back (each.first);

                   return true;
               });

    return order;
}
} // namespace phazite
