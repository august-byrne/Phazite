// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Selection.h"
#include "Clips.h"
#include "Tracks.h"

namespace phazite
{
namespace
{
    void deleteSelection (te::SelectionManager& selection)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return;

        if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
        {
            for (auto* clip : clips)
                clip->removeFromParent();

            return;
        }

        const auto tracks = getSelectedOutermostTracks (selection);

        if (tracks.isEmpty())
            return;

        const auto row = getPlaylistTracks (*edit).indexOf (tracks.getFirst());

        for (auto* track : tracks)
            edit->deleteTrack (track);

        // The track that takes the first one's place is selected, as in Ableton Live, so the keyboard
        // can carry on from there.
        if (const auto remaining = getPlaylistTracks (*edit); ! remaining.isEmpty())
            selection.selectOnly (remaining[juce::jmin (row, remaining.size() - 1)]);
    }

    void duplicateSelection (te::SelectionManager& selection)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return;

        // Clips: the copies go right after the selection, as FL Studio's Ctrl+B does.
        if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
        {
            auto start = clips.getFirst()->getStartBeat(), end = clips.getFirst()->getEndBeat();

            for (auto* clip : clips)
            {
                start = std::min (start, clip->getStartBeat());
                end = std::max (end, clip->getEndBeat());
            }

            selection.deselectAll();

            for (auto* copy : copyClips (clips, false)) // each starts where its original is
            {
                if (auto* track = copy->getClipTrack())
                    moveClip (*copy, copy->getStartBeat() + (end - start), *track);

                selection.addToSelection (copy);
            }

            return;
        }

        // Tracks: each copy goes right after its original.
        juce::Array<te::Track*> copies;

        for (auto* track : getSelectedOutermostTracks (selection))
            if (auto copy = edit->copyTrack (track, getInsertPointAfter (*track)))
                copies.add (copy.get());

        selection.deselectAll();

        for (auto* copy : copies)
            selection.addToSelection (copy);
    }

    void selectAll (te::SelectionManager& selection)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return;

        // Every track while tracks are selected; otherwise every clip, or every track if there are none.
        const auto tracksSelected = ! getSelectedTracks (selection).isEmpty();
        juce::Array<te::Selectable*> items;

        for (auto* track : getPlaylistTracks (*edit))
        {
            if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track); clipTrack != nullptr && ! tracksSelected)
                for (auto* clip : clipTrack->getClips())
                    items.add (clip);
        }

        if (items.isEmpty())
            for (auto* track : getPlaylistTracks (*edit))
                items.add (track);

        selection.deselectAll();

        for (auto* item : items)
            selection.addToSelection (item);
    }
} // namespace

juce::String describeSelection (const te::SelectionManager& selection, const juce::String& verb)
{
    const auto describe = [&verb] (int count, const char* noun)
    { return verb + " " + (count == 1 ? juce::String (noun) : juce::String (count) + " " + noun + "s"); };

    if (const auto clips = getSelectedClips (selection).size(); clips > 0)
        return describe (clips, "Clip");

    if (const auto tracks = getSelectedTracks (selection).size(); tracks > 0)
        return describe (tracks, "Track");

    return verb;
}

void addSelectionCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto getEdit = [&selection] { return selection.getEdit(); };
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };
    const auto hasSelection = [&selection]
    { return ! getSelectedClips (selection).isEmpty() || ! getSelectedTracks (selection).isEmpty(); };

    registry.add ({ .id = "edit.delete",
                    .name = "Delete",
                    .category = "Edit",
                    .description = "Deletes the selected clips or tracks.",
                    .synonyms = { "remove", "erase" },
                    .perform = [&selection] { deleteSelection (selection); },
                    .isEnabled = hasSelection,
                    .undoTarget = getEdit,
                    .currentName = [&selection] { return describeSelection (selection, "Delete"); } });

    registry.add ({ .id = "edit.duplicate",
                    .name = "Duplicate",
                    .category = "Edit",
                    .description = "Duplicates the selected clips, right after them, or the selected tracks, each below its original.",
                    .synonyms = { "copy", "clone", "repeat" },
                    .perform = [&selection] { duplicateSelection (selection); },
                    .isEnabled = hasSelection,
                    .undoTarget = getEdit,
                    .currentName = [&selection] { return describeSelection (selection, "Duplicate"); } });

    registry.add ({ .id = "edit.selectAll",
                    .name = "Select All",
                    .category = "Edit",
                    .description = "Selects every clip, or every track while tracks are selected.",
                    .synonyms = { "everything" },
                    .perform = [&selection] { selectAll (selection); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "edit.deselectAll",
                    .name = "Deselect All",
                    .category = "Edit",
                    .description = "Clears the selection.",
                    .synonyms = { "select none", "clear selection" },
                    .perform = [&selection] { selection.deselectAll(); },
                    .isEnabled = [&selection] { return selection.getNumObjectsSelected() > 0; } });
}
} // namespace phazite
