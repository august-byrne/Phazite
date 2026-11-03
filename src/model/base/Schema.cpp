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

    // Where the Edit's state holds its song, on its root, which is never saved (a project file is the song alone).
    const juce::Identifier songHolder { "phaziteSong" };
} // namespace

Song& getSong (const te::Edit& edit)
{
    auto root = edit.state;

    if (auto* held = dynamic_cast<Song*> (root[songHolder].getObject()))
        return *held;

    auto* made = new Song (root.getOrCreateChildWithName (schema::PHAZITE, nullptr));

    // Tracktion's items take their IDs from the same numbers while it mirrors the song (Song::makeID()).
    made->setTracktionIDs ([&mutableEdit = const_cast<te::Edit&> (edit)] { return mutableEdit.createNewItemID().getRawID(); });
    root.setProperty (songHolder, juce::var (made), nullptr);
    return *made;
}

juce::ValueTree findPhaziteState (const te::Edit& edit)
{
    return edit.state.getChildWithName (schema::PHAZITE);
}

// Here, with the other counting of IDs and the name of the count (schema::nextID), rather than in Song.cpp.
std::uint64_t Song::makeID()
{
    // From 1: no item's ID is 0.
    auto id = std::max ((std::uint64_t) 1, readNextID (state));

    // While Tracktion mirrors the song, its next ID, brought past the song's (a project whose last items went before it was
    // saved), so that Tracktion never gives what the song has given.
    if (takeTracktionID != nullptr)
    {
        auto tracktions = takeTracktionID();

        while (tracktions < id)
            tracktions = takeTracktionID();

        id = tracktions;
    }

    state.setProperty (schema::nextID, (juce::int64) (id + 1), nullptr);
    return id;
}

std::uint64_t findNextID (const juce::ValueTree& song)
{
    return std::max (readNextID (song), findLargestID (song) + 1);
}

void countIDs (te::Edit& edit)
{
    auto song = getSong (edit).getState();
    const auto next = findNextID (song);

    if (next != readNextID (song))
        song.setProperty (schema::nextID, (juce::int64) next, nullptr);

    // Tracktion's next is past every ID in its state (it counts from the largest it finds, and the song's come from it), so
    // only a song that counted further needs it brought on.
    const auto ids = te::EditItemID::findAllIDs (edit.state);
    const auto largest = ids.empty() ? std::uint64_t (0) : std::max_element (ids.begin(), ids.end())->getRawID();

    if (largest + 1 < next)
        for (auto id = edit.createNewItemID().getRawID(); id + 1 < next;)
            id = edit.createNewItemID().getRawID();
}

CopiedIDs giveNewIDs (Song& song, juce::ValueTree& copy)
{
    CopiedIDs copied;

    // Each kind of item's ID is named "id" (schema::trackID, clipID, deviceID, sendID, modulatorID), as a note's is, which
    // keeps its own; a channel's fader and VCA are named after them.
    const std::function<void (juce::ValueTree)> renumber = [&] (juce::ValueTree tree)
    {
        if (tree.hasType (schema::NOTE))
            return;

        for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
        {
            if (const auto* old = tree.getPropertyPointer (*property))
            {
                const auto id = song.makeID();
                copied[(std::uint64_t) (juce::int64) *old] = id;
                tree.setProperty (*property, (juce::int64) id, nullptr);
            }
        }

        for (const auto& child : tree)
            renumber (child);
    };

    // A lane, a modulation and a sidechain name what they show, modulate or listen to: the copy's own if it holds it.
    const std::function<void (juce::ValueTree)> rename = [&] (juce::ValueTree tree)
    {
        const auto renameDevice = [&] (const juce::Identifier& property)
        {
            if (const auto found = copied.find ((std::uint64_t) tree[property].toString().getLargeIntValue()); found != copied.end())
                tree.setProperty (property, juce::String ((juce::int64) found->second), nullptr);
        };

        if (tree.hasType (schema::LANE))
            renameDevice (schema::laneDevice);
        else if (tree.hasType (schema::MODULATES))
            renameDevice (schema::modulatesDevice);

        if (const auto* source = tree.getPropertyPointer (schema::deviceSidechain))
            if (const auto found = copied.find ((std::uint64_t) (juce::int64) *source); found != copied.end())
                tree.setProperty (schema::deviceSidechain, (juce::int64) found->second, nullptr);

        for (const auto& child : tree)
            rename (child);
    };

    renumber (copy);
    rename (copy);
    return copied;
}
} // namespace phazite
