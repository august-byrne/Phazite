// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Schema.h"

#include <algorithm>

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

    // The song's undo history, as long as Tracktion keeps an Edit's: 30 steps at least.
    struct SongUndo final : juce::ReferenceCountedObject
    {
        SongUndo() { undoManager.setMaxNumberOfStoredUnits (30000, 30); }

        juce::UndoManager undoManager;
    };

    // Where the Edit's state holds it, on its root, which is never saved (a project file is the song alone).
    const juce::Identifier songUndo { "phaziteSongUndo" };
} // namespace

juce::ValueTree getPhaziteState (te::Edit& edit)
{
    return edit.state.getOrCreateChildWithName (schema::PHAZITE, nullptr);
}

juce::ValueTree findPhaziteState (const te::Edit& edit)
{
    return edit.state.getChildWithName (schema::PHAZITE);
}

juce::UndoManager& getSongUndoManager (const te::Edit& edit)
{
    auto root = edit.state;

    if (auto* held = dynamic_cast<SongUndo*> (root[songUndo].getObject()))
        return held->undoManager;

    auto* made = new SongUndo();
    root.setProperty (songUndo, juce::var (made), nullptr);
    return made->undoManager;
}

std::uint64_t makeID (te::Edit& edit)
{
    auto song = getPhaziteState (edit);
    const auto next = readNextID (song);
    auto id = edit.createNewItemID().getRawID();

    // Tracktion's behind the song's (a project whose last items went before it was saved): brought past it, so that
    // Tracktion never gives what the song has given.
    while (id < next)
        id = edit.createNewItemID().getRawID();

    song.setProperty (schema::nextID, (juce::int64) (id + 1), nullptr);
    return id;
}

std::uint64_t findNextID (const juce::ValueTree& song)
{
    return std::max (readNextID (song), findLargestID (song) + 1);
}

void countIDs (te::Edit& edit)
{
    auto song = getPhaziteState (edit);
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
} // namespace phazite
