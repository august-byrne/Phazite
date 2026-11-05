// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SongDiff.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"

#include <cmath>
#include <map>

namespace phazite
{
namespace
{
    // The same value as a file holds it: the model may hold a number where a version holds its text, with more digits than
    // the text keeps (a clip at 12.000000000000002 beats is at "12.0").
    bool isSameValue (const juce::var& a, const juce::var& b)
    {
        return (a == b && b == a) || a.toString() == b.toString();
    }

    // The counted properties that two elements don't hold the same of.
    juce::Array<juce::Identifier> findChangedProperties (const juce::ValueTree& a, const juce::ValueTree& b)
    {
        juce::Array<juce::Identifier> changed;

        for (int i = 0; i < a.getNumProperties(); ++i)
        {
            const auto name = a.getPropertyName (i);

            if (schema::isUncountedProperty (a, name))
                continue;

            if (const auto* other = b.getPropertyPointer (name); other == nullptr || ! isSameValue (a[name], *other))
                changed.add (name);
        }

        for (int i = 0; i < b.getNumProperties(); ++i)
            if (const auto name = b.getPropertyName (i); ! schema::isUncountedProperty (b, name) && ! a.hasProperty (name))
                changed.add (name);

        return changed;
    }

    // An element's counted children: those with keys by their types and keys, the keys of those in an order in that order,
    // and the others, by their types, in their order.
    struct Children
    {
        std::map<juce::String, juce::ValueTree> keyed;
        std::map<juce::String, juce::StringArray> orders;
        std::map<juce::String, std::vector<juce::ValueTree>> unkeyed;
    };

    Children getChildren (const juce::ValueTree& element)
    {
        Children children;

        for (const auto& child : element)
        {
            if (schema::isUncountedElement (element, child))
                continue;

            const auto type = child.getType().toString();
            const auto key = schema::getElementKey (child);
            const auto name = type + ":" + key;

            if (key.isEmpty() || children.keyed.count (name) > 0)
            {
                children.unkeyed[type].push_back (child);
                continue;
            }

            children.keyed.emplace (name, child);

            if (const auto group = schema::getOrderGroup (child); group.isNotEmpty())
                children.orders[group].add (name);
        }

        return children;
    }

    bool holdTheSameChildren (const Children& a, const Children& b)
    {
        if (a.keyed.size() != b.keyed.size() || a.unkeyed.size() != b.unkeyed.size() || a.orders != b.orders)
            return false;

        for (const auto& [name, child] : a.keyed)
            if (const auto found = b.keyed.find (name); found == b.keyed.end() || ! holdTheSame (child, found->second))
                return false;

        for (const auto& [type, list] : a.unkeyed)
        {
            const auto found = b.unkeyed.find (type);

            if (found == b.unkeyed.end() || found->second.size() != list.size())
                return false;

            for (size_t i = 0; i < list.size(); ++i)
                if (! holdTheSame (list[i], found->second[i]))
                    return false;
        }

        return true;
    }

    // Whether two elements of the song's, either of which it may not have, hold the same: one it doesn't have holds
    // nothing.
    bool holdTheSameOrNothing (const juce::ValueTree& a, const juce::ValueTree& b)
    {
        if (a.isValid() && b.isValid())
            return holdTheSame (a, b);

        const auto& other = a.isValid() ? a : b;
        return ! other.isValid()
               || (findChangedProperties (other, juce::ValueTree (other.getType())).isEmpty() && getChildren (other).keyed.empty()
                   && getChildren (other).unkeyed.empty());
    }

    bool hasTypeOf (const juce::String& name, std::initializer_list<const juce::Identifier*> types)
    {
        for (const auto* type : types)
            if (name.startsWith (type->toString() + ":"))
                return true;

        return false;
    }

    juce::String count (int number, const char* one, const char* many)
    {
        return juce::String (number) + " " + (number == 1 ? one : many);
    }

    // The words for a track's (or the master's) own properties that changed.
    void describeTrackProperties (const juce::ValueTree& before, const juce::ValueTree& after, juce::StringArray& what)
    {
        for (const auto& property : findChangedProperties (before, after))
        {
            const auto word = property == schema::trackName                                           ? "name"
                              : property == schema::trackColour                                       ? "color"
                              : property == schema::trackMute                                         ? "mute"
                              : property == schema::trackSolo                                         ? "solo"
                              : property == schema::channelVolume                                     ? "volume"
                              : property == schema::channelPan                                        ? "pan"
                              : property == schema::trackAutomationMode                               ? "automation mode"
                              : property == schema::keyRoot || property == schema::keyScale           ? "key"
                              : property == schema::trackInput || property == schema::trackMonitoring ? "input"
                              : property == schema::trackFrozen                                       ? "freeze"
                              : property == schema::folderBus || property == schema::channelFader || property == schema::channelVCA
                                  ? "bus"
                                  : "settings";
            what.addIfNotAlreadyThere (word);
        }
    }

    // How many notes of a clip were added, taken away or changed.
    int countNoteChanges (const Children& before, const Children& after)
    {
        int changed = 0;

        for (const auto& [name, note] : after.keyed)
            if (name.startsWith (schema::NOTE.toString() + ":"))
                if (const auto found = before.keyed.find (name); found == before.keyed.end() || ! holdTheSame (found->second, note))
                    ++changed;

        for (const auto& [name, note] : before.keyed)
            if (name.startsWith (schema::NOTE.toString() + ":") && after.keyed.count (name) == 0)
                ++changed;

        return changed;
    }

    void describeClips (const Children& before, const Children& after, juce::StringArray& what)
    {
        int added = 0, removed = 0, changedClips = 0, notes = 0;
        bool controls = false;
        std::set<juce::String> notesCounted; // alias groups, whose clips hold the same notes, counted once

        for (const auto& [name, clip] : after.keyed)
        {
            if (! name.startsWith (schema::CLIP.toString() + ":"))
                continue;

            const auto found = before.keyed.find (name);

            if (found == before.keyed.end())
            {
                ++added;
                continue;
            }

            if (holdTheSame (found->second, clip))
                continue;

            if (! findChangedProperties (found->second, clip).isEmpty())
                ++changedClips;

            const auto clipBefore = getChildren (found->second), clipAfter = getChildren (clip);

            if (const auto link = clip[schema::clipLink].toString(); link.isEmpty() || notesCounted.insert (link).second)
                notes += countNoteChanges (clipBefore, clipAfter);

            controls = controls || ! holdTheSameChildren ({ {}, {}, clipBefore.unkeyed }, { {}, {}, clipAfter.unkeyed });
        }

        for (const auto& [name, clip] : before.keyed)
            if (name.startsWith (schema::CLIP.toString() + ":") && after.keyed.count (name) == 0)
                ++removed;

        if (added > 0)
            what.add (count (added, "clip added", "clips added"));

        if (removed > 0)
            what.add (count (removed, "clip removed", "clips removed"));

        if (changedClips > 0)
            what.add (count (changedClips, "clip changed", "clips changed"));

        if (notes > 0)
            what.add (count (notes, "note", "notes"));

        if (controls)
            what.add ("controller events");
    }

    juce::String describeValue (float value)
    {
        return juce::String (juce::roundToInt (value * 100.0f)) + "%";
    }

    // The knobs of a plug-in that moved, by their names: "Cutoff 40% → 55%", and how many more; nothing for none, or
    // without both its values then and now.
    juce::String describeMovedParameters (std::uint64_t device, const PluginValues& values)
    {
        if (values.before == nullptr || values.after == nullptr)
            return {};

        const auto before = values.before->find (device), after = values.after->find (device);

        if (before == values.before->end() || after == values.after->end())
            return {};

        std::map<juce::String, float> then;

        for (const auto& parameter : before->second)
            then.emplace (parameter.id, parameter.value);

        juce::StringArray moved;

        for (const auto& parameter : after->second)
            if (const auto found = then.find (parameter.id); found != then.end() && std::abs (found->second - parameter.value) > 0.0005f)
                moved.add (parameter.name + " " + describeValue (found->second) + juce::String::fromUTF8 (" \xe2\x86\x92 ")
                           + describeValue (parameter.value));

        if (moved.size() <= 1)
            return moved.joinIntoString ({});

        return moved[0] + " and " + juce::String (moved.size() - 1) + " more";
    }

    void describeTrackChildren (const juce::ValueTree& before, const juce::ValueTree& after, const PluginValues& values,
                                juce::StringArray& what)
    {
        const auto a = getChildren (before), b = getChildren (after);
        const auto chainBefore = a.orders.find ("chain"), chainAfter = b.orders.find ("chain");

        // Its devices, by their names, in their order: added, taken away or changed (a plug-in's moved knobs named), and
        // their order.
        if (chainAfter != b.orders.end())
        {
            for (const auto& name : chainAfter->second)
            {
                const auto& device = b.keyed.at (name);

                if (const auto found = a.keyed.find (name); found == a.keyed.end())
                {
                    what.add ("+ " + getDeviceName (device));
                }
                else if (const auto moved = describeMovedParameters (readID (device[schema::deviceID]), values);
                         moved.isNotEmpty() || ! holdTheSame (found->second, device))
                {
                    what.add (getDeviceName (device) + (moved.isNotEmpty() ? ": " + moved : juce::String()));
                }
            }
        }

        if (chainBefore != a.orders.end())
            for (const auto& name : chainBefore->second)
                if (b.keyed.count (name) == 0)
                    what.add ("- " + getDeviceName (a.keyed.at (name)));

        if (chainBefore != a.orders.end() && chainAfter != b.orders.end() && chainBefore->second.size() == chainAfter->second.size()
            && chainBefore->second != chainAfter->second)
            what.add ("device order");

        // Its sends, and its modulators and macros, each as a whole.
        const auto differ = [&a, &b] (std::initializer_list<const juce::Identifier*> types, const char* order)
        {
            for (const auto* children : { &a, &b })
            {
                const auto& other = children == &a ? b : a;

                for (const auto& [name, child] : children->keyed)
                    if (hasTypeOf (name, types))
                        if (const auto found = other.keyed.find (name); found == other.keyed.end() || ! holdTheSame (found->second, child))
                            return true;
            }

            const auto orderBefore = a.orders.find (order), orderAfter = b.orders.find (order);
            return (orderBefore != a.orders.end()) != (orderAfter != b.orders.end())
                   || (orderBefore != a.orders.end() && orderBefore->second != orderAfter->second);
        };

        if (differ ({ &schema::SEND }, "SEND"))
            what.add ("sends");

        if (differ ({ &schema::MODULATOR }, "MODULATOR") || differ ({ &schema::MACRO }, "MACRO"))
            what.add ("modulation");

        describeClips (a, b, what);

        // Anything else it holds, but its tracks (lines of their own).
        const auto isOther = [] (const juce::String& name)
        {
            return ! hasTypeOf (name, { &schema::TRACK, &schema::DEVICE, &schema::CONTAINER, &schema::SEND, &schema::MODULATOR,
                                        &schema::MACRO, &schema::CLIP });
        };

        for (const auto* children : { &a, &b })
        {
            const auto& other = children == &a ? b : a;

            for (const auto& [name, child] : children->keyed)
            {
                if (isOther (name))
                {
                    if (const auto found = other.keyed.find (name); found == other.keyed.end() || ! holdTheSame (found->second, child))
                    {
                        what.addIfNotAlreadyThere ("settings");
                        return;
                    }
                }
            }
        }
    }

    void addTracks (const juce::ValueTree& parent, std::vector<juce::ValueTree>& tracks)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
            {
                tracks.push_back (child);
                addTracks (child, tracks);
            }
        }
    }

    void addAutomatableIDs (const juce::ValueTree& element, std::set<std::uint64_t>& ids)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::TRACK) || child.hasType (schema::CLIP))
                continue;

            for (const auto* type : { &schema::DEVICE, &schema::CONTAINER, &schema::SEND, &schema::MODULATOR, &schema::MACRO })
                if (child.hasType (*type))
                    ids.insert (readID (child[schema::deviceID]));

            addAutomatableIDs (child, ids);
        }
    }

    // The order of the tracks two songs both have: each one's folder and place, top to bottom.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> getTrackOrder (const juce::ValueTree& song, const std::set<std::uint64_t>& both)
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> order;

        for (const auto& track : getTrackElements (song))
            if (const auto id = readID (track[schema::trackID]); both.count (id) > 0)
                order.emplace_back (track.getParent().hasType (schema::TRACK) ? readID (track.getParent()[schema::trackID]) : 0, id);

        return order;
    }
} // namespace

bool holdTheSame (const juce::ValueTree& a, const juce::ValueTree& b)
{
    return a.getType() == b.getType() && findChangedProperties (a, b).isEmpty() && holdTheSameChildren (getChildren (a), getChildren (b));
}

std::vector<juce::ValueTree> getTrackElements (const juce::ValueTree& song)
{
    std::vector<juce::ValueTree> tracks;
    addTracks (song.getChildWithName (schema::TRACKS), tracks);
    return tracks;
}

juce::String getTrackNameIn (const juce::ValueTree& song, const juce::ValueTree& track)
{
    if (track.hasType (schema::MASTER))
        return "Master";

    const auto isFolder = [] (const juce::ValueTree& t) { return t[schema::trackKind].toString() == schema::trackIsFolder; };

    if (const auto name = track[schema::trackName].toString(); name.isNotEmpty())
        return name;

    if (isFolder (track))
        return "Folder";

    auto number = 1;

    for (const auto& other : getTrackElements (song))
    {
        if (other == track)
            break;

        if (! isFolder (other))
            ++number;
    }

    return "Track " + juce::String (number);
}

std::set<std::uint64_t> getAutomatableIDs (const juce::ValueTree& track)
{
    std::set<std::uint64_t> ids;

    for (const auto* property : { &schema::channelFader, &schema::channelVCA })
        if (track.hasProperty (*property))
            ids.insert (readID (track[*property]));

    addAutomatableIDs (track, ids);
    return ids;
}

SongChange findTrackChange (const juce::ValueTree& before, const juce::ValueTree& after, const juce::ValueTree& songBefore,
                            const juce::ValueTree& songAfter, const PluginValues& values)
{
    SongChange change;
    const auto& element = after.isValid() ? after : before;
    change.where = element.hasType (schema::MASTER) ? SongChange::Where::master : SongChange::Where::track;
    change.trackID = readID (element[schema::trackID]);
    change.name = getTrackNameIn (after.isValid() ? songAfter : songBefore, element);

    if (! before.isValid() || ! after.isValid())
    {
        change.kind = before.isValid() ? SongChange::Kind::removed : SongChange::Kind::added;
        return change;
    }

    describeTrackProperties (before, after, change.what);
    describeTrackChildren (before, after, values, change.what);
    return change;
}

SongChange findSongChange (const juce::ValueTree& songBefore, const juce::ValueTree& songAfter)
{
    SongChange change;
    change.name = "Song";
    auto& what = change.what;

    for (const auto& property : findChangedProperties (songBefore, songAfter))
        what.addIfNotAlreadyThere (property == schema::outline                  ? "song outline"
                                   : property == schema::patternsKeepTheirSpeed ? "pattern speed"
                                                                                : "settings");

    // The tempo map's tempos and time signatures.
    const auto mapBefore = getChildren (songBefore.getChildWithName (schema::TEMPOMAP));
    const auto mapAfter = getChildren (songAfter.getChildWithName (schema::TEMPOMAP));
    const auto mapDiffers = [&mapBefore, &mapAfter] (const juce::Identifier& type)
    {
        const auto take = [&type] (const Children& children)
        {
            Children only;

            for (const auto& [name, child] : children.keyed)
                if (name.startsWith (type.toString() + ":"))
                    only.keyed.emplace (name, child);

            return only;
        };

        return ! holdTheSameChildren (take (mapBefore), take (mapAfter));
    };

    if (mapDiffers (schema::TEMPO))
        what.add ("tempo");

    if (mapDiffers (schema::TIMESIGNATURE))
        what.add ("time signature");

    if (! holdTheSameOrNothing (songBefore.getChildWithName (schema::SECTIONS), songAfter.getChildWithName (schema::SECTIONS)))
        what.add ("sections");

    if (! holdTheSameOrNothing (songBefore.getChildWithName (schema::CHORDLANE), songAfter.getChildWithName (schema::CHORDLANE)))
        what.add ("chords");

    // The order of the tracks both have.
    std::set<std::uint64_t> inBefore, both;

    for (const auto& track : getTrackElements (songBefore))
        inBefore.insert (readID (track[schema::trackID]));

    for (const auto& track : getTrackElements (songAfter))
        if (const auto id = readID (track[schema::trackID]); inBefore.count (id) > 0)
            both.insert (id);

    if (getTrackOrder (songBefore, both) != getTrackOrder (songAfter, both))
        what.add ("track order");

    return change;
}

std::set<std::uint64_t> findAutomatedDevicesChanged (const juce::ValueTree& songBefore, const juce::ValueTree& songAfter)
{
    const auto before = getChildren (songBefore.getChildWithName (schema::AUTOMATION));
    const auto after = getChildren (songAfter.getChildWithName (schema::AUTOMATION));
    std::set<std::uint64_t> devices;

    for (const auto& [name, curve] : after.keyed)
        if (const auto found = before.keyed.find (name); found == before.keyed.end() || ! holdTheSame (found->second, curve))
            devices.insert (readID (curve[schema::curveDevice]));

    for (const auto& [name, curve] : before.keyed)
        if (after.keyed.count (name) == 0)
            devices.insert (readID (curve[schema::curveDevice]));

    return devices;
}

std::vector<SongChange> findChanges (const juce::ValueTree& songBefore, const juce::ValueTree& songAfter, const PluginValues& values)
{
    std::vector<SongChange> changes;
    const auto automated = findAutomatedDevicesChanged (songBefore, songAfter);

    const auto add = [&] (SongChange change, const juce::ValueTree& before, const juce::ValueTree& after)
    {
        if (change.kind == SongChange::Kind::changed)
        {
            auto ids = getAutomatableIDs (before);
            ids.merge (getAutomatableIDs (after));

            for (const auto id : ids)
            {
                if (automated.count (id) > 0)
                {
                    change.what.add ("automation");
                    break;
                }
            }
        }

        if (change.kind != SongChange::Kind::changed || ! change.what.isEmpty())
            changes.push_back (std::move (change));
    };

    std::map<std::uint64_t, juce::ValueTree> tracksBefore;

    for (const auto& track : getTrackElements (songBefore))
        tracksBefore.emplace (readID (track[schema::trackID]), track);

    std::set<std::uint64_t> seen;

    for (const auto& track : getTrackElements (songAfter))
    {
        const auto id = readID (track[schema::trackID]);
        const auto found = tracksBefore.find (id);
        const auto before = found != tracksBefore.end() ? found->second : juce::ValueTree();
        seen.insert (id);
        add (findTrackChange (before, track, songBefore, songAfter, values), before, track);
    }

    for (const auto& track : getTrackElements (songBefore))
        if (seen.count (readID (track[schema::trackID])) == 0)
            add (findTrackChange (track, {}, songBefore, songAfter), track, {});

    if (const auto masterBefore = songBefore.getChildWithName (schema::MASTER), masterAfter = songAfter.getChildWithName (schema::MASTER);
        masterBefore.isValid() && masterAfter.isValid())
        add (findTrackChange (masterBefore, masterAfter, songBefore, songAfter, values), masterBefore, masterAfter);

    if (auto song = findSongChange (songBefore, songAfter); ! song.what.isEmpty())
        changes.push_back (std::move (song));

    return changes;
}

juce::String summarise (const std::vector<SongChange>& changes)
{
    // As a commit's message is written: in the imperative, what was added, updated and removed, a few names each.
    juce::StringArray added, updated, removed;

    for (const auto& change : changes)
    {
        if (change.where == SongChange::Where::song)
            updated.addArray (change.what); // "tempo", "sections"
        else
            (change.kind == SongChange::Kind::added     ? added
             : change.kind == SongChange::Kind::removed ? removed
                                                        : updated)
                .add (change.name);
    }

    if (added.isEmpty() && updated.isEmpty() && removed.isEmpty())
        return "No changes";

    // At most a few, then how many more; the last after "and".
    const auto list = [] (juce::StringArray names)
    {
        constexpr int mostShown = 3;

        if (names.size() > mostShown + 1)
        {
            const auto more = names.size() - mostShown;
            names.removeRange (mostShown, more);
            names.add (juce::String (more) + " more");
        }

        const auto last = names[names.size() - 1];
        names.remove (names.size() - 1);
        return names.isEmpty() ? last : names.joinIntoString (", ") + " and " + last;
    };

    juce::StringArray parts;

    if (! added.isEmpty())
        parts.add ("add " + list (added));

    if (! updated.isEmpty())
        parts.add ("update " + list (updated));

    if (! removed.isEmpty())
        parts.add ("remove " + list (removed));

    const auto message = parts.joinIntoString ("; ");
    return message.substring (0, 1).toUpperCase() + message.substring (1);
}
} // namespace phazite
