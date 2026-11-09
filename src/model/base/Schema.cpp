// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Schema.h"

#include <algorithm>
#include <functional>

namespace phazite
{
namespace
{
    // The largest ID in a part of the song: its items' (each kind's is named "id": trackID, clipID, noteID, deviceID,
    // sendID, modulatorID, sectionID), its channels' faders and VCAs, and its alias groups (clipLink).
    std::uint64_t findLargestID (const juce::ValueTree& tree)
    {
        std::uint64_t largest = 0;

        for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA, &schema::clipLink })
            largest = std::max (largest, (std::uint64_t) (juce::int64) tree.getProperty (*property, (juce::int64) 0));

        for (const auto& child : tree)
            largest = std::max (largest, findLargestID (child));

        return largest;
    }

    std::uint64_t readNextID (const juce::ValueTree& song)
    {
        return (std::uint64_t) (juce::int64) song.getProperty (schema::nextID, (juce::int64) 0);
    }

} // namespace

// Here, with the other counting of IDs and the name of the count (schema::nextID), rather than in Song.cpp.
std::uint64_t Song::makeID()
{
    // From 1: no item's ID is 0.
    const auto id = std::max ((std::uint64_t) 1, readNextID (state));
    state.setProperty (schema::nextID, (juce::int64) (id + 1), nullptr);
    return id;
}

std::uint64_t findNextID (const juce::ValueTree& song)
{
    return std::max (readNextID (song), findLargestID (song) + 1);
}

void countIDs (Song& song)
{
    auto state = song.getState();

    if (const auto next = findNextID (state); next != readNextID (state))
        state.setProperty (schema::nextID, (juce::int64) next, nullptr);
}

CopiedIDs giveNewIDs (Song& song, juce::ValueTree& copy)
{
    CopiedIDs copied;

    // What holds an ID (schema::holdsID()), but a note's, which a copy of its clip keeps, and an alias group's, which the
    // copies join.
    const std::function<void (juce::ValueTree)> renumber = [&] (juce::ValueTree tree)
    {
        if (tree.hasType (schema::NOTE))
            return;

        for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA }) // in the order they are given
        {
            if (const auto* old = tree.getPropertyPointer (*property); old != nullptr && schema::holdsID (tree, *property))
            {
                const auto id = song.makeID();
                copied[readID (*old)] = id;
                tree.setProperty (*property, (juce::int64) id, nullptr);
            }
        }

        for (const auto& child : tree)
            renumber (child);
    };

    // What names one (schema::namesID()): a lane, a modulation and a sidechain name what they show, modulate or listen to,
    // the copy's own if it holds it.
    const std::function<void (juce::ValueTree)> rename = [&] (juce::ValueTree tree)
    {
        for (int i = 0; i < tree.getNumProperties(); ++i)
        {
            const auto property = tree.getPropertyName (i);

            if (schema::namesID (tree, property))
                if (const auto found = copied.find (readID (tree[property])); found != copied.end())
                    tree.setProperty (property, writeIDLike (tree[property], found->second), nullptr);
        }

        for (const auto& child : tree)
            rename (child);
    };

    renumber (copy);
    rename (copy);
    return copied;
}

std::uint64_t readID (const juce::var& value)
{
    return (std::uint64_t) value.toString().getLargeIntValue();
}

juce::var writeIDLike (const juce::var& old, std::uint64_t id)
{
    // A device named by a lane, a curve or a modulation is text ("1042"); the other IDs are numbers.
    return old.isString() ? juce::var (juce::String ((juce::int64) id)) : juce::var ((juce::int64) id);
}

namespace schema
{
    bool isUncountedProperty (const juce::ValueTree& tree, const juce::Identifier& property)
    {
        return ((tree.hasType (TRACK) || tree.hasType (MASTER)) && isTrackViewProperty (property))
               || (tree.hasType (DEVICE) && isDeviceViewProperty (property))
               || (tree.hasType (PHAZITE)
                   && (isGridProperty (property) || property == nextID || property == loopFollowsSong || property == punch
                       || property == lastInput || property == loop || property == loopStart || property == loopEnd || property == playStart
                       || property == metronome));
    }

    bool isUncountedElement (const juce::ValueTree& parent, const juce::ValueTree& element)
    {
        return (parent.hasType (PHAZITE) && element.hasType (PIANOROLL))
               || ((parent.hasType (TRACK) || parent.hasType (MASTER)) && isTrackViewElement (element));
    }

    bool isCounted (const juce::ValueTree& tree, const juce::Identifier& property)
    {
        if (isUncountedProperty (tree, property))
            return false;

        for (auto element = tree; element.isValid(); element = element.getParent())
            if (isUncountedElement (element.getParent(), element))
                return false;

        return true;
    }

    namespace
    {
        // Where something starts, in beats, as a key: the same beat however it was written ("4", "4.0").
        juce::String beatKey (const juce::var& beat)
        {
            return juce::String ((double) beat, 6);
        }

        bool isItem (const juce::ValueTree& element)
        {
            for (const auto* type : { &TRACK, &DEVICE, &CONTAINER, &SEND, &MODULATOR, &MACRO, &CLIP, &NOTE, &SECTION })
                if (element.hasType (*type))
                    return true;

            return false;
        }
    } // namespace

    juce::String getElementKey (const juce::ValueTree& element)
    {
        if (isItem (element))
            return element[trackID].toString(); // each kind's ID is named "id"

        if (element.hasType (CURVE))
            return element[curveDevice].toString() + "/" + element[curveParameter].toString();

        if (element.hasType (LANE))
            return element[laneDevice].toString() + "/" + element[laneParameter].toString();

        if (element.hasType (MODULATES))
            return element[modulatesDevice].toString() + "/" + element[modulatesParameter].toString();

        if (element.hasType (VALUE))
            return element[valueParameter].toString();

        if (element.hasType (TEMPO) || element.hasType (TIMESIGNATURE))
            return beatKey (element[mapBeat]);

        if (element.hasType (CHORD) || element.hasType (CHORDKEY))
            return beatKey (element[chordStart]);

        if (element.hasType (POINT) || element.hasType (CONTROL) || element.hasType (WARP))
            return {};

        return element.getType().toString(); // one of its kind in its parent: MASTER, TEMPOMAP, TRACKS, VALUES...
    }

    juce::String getOrderGroup (const juce::ValueTree& element)
    {
        if (element.hasType (TRACK))
            return "tracks";

        if (element.hasType (DEVICE) || element.hasType (CONTAINER))
            return "chain";

        for (const auto* type : { &SEND, &MODULATOR, &MACRO, &MODULATES, &POINT, &CONTROL, &WARP, &LANE })
            if (element.hasType (*type))
                return type->toString();

        return {};
    }

    bool holdsID (const juce::ValueTree& element, const juce::Identifier& property)
    {
        if (property == trackID) // each kind's ID is named "id"
            return isItem (element) || element.hasType (MASTER);

        if (property == channelFader || property == channelVCA)
            return element.hasType (TRACK) || element.hasType (MASTER);

        return property == clipLink && element.hasType (CLIP);
    }

    bool namesID (const juce::ValueTree& element, const juce::Identifier& property)
    {
        return (element.hasType (SEND) && property == sendTo) || (element.hasType (DEVICE) && property == deviceSidechain)
               || (element.hasType (LANE) && property == laneDevice) || (element.hasType (CURVE) && property == curveDevice)
               || (element.hasType (MODULATES) && property == modulatesDevice);
    }
} // namespace schema
} // namespace phazite
