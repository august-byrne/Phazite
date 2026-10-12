// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Selection.h"
#include "Clips.h"
#include "Tracks.h"

namespace phazite
{
//==============================================================================
void RangeSelection::set (te::BeatRange newRange, const juce::Array<te::Track*>& newTracks)
{
    range = newRange;
    tracks.clear();

    for (auto* track : newTracks)
        tracks.emplace_back (track);

    sendChangeMessage();
}

void RangeSelection::clear()
{
    if (tracks.empty())
        return;

    range = {};
    tracks.clear();
    sendChangeMessage();
}

bool RangeSelection::isEmpty() const
{
    return range.getLength() <= te::BeatDuration() || getTracks().isEmpty();
}

juce::Array<te::Track*> RangeSelection::getTracks() const
{
    juce::Array<te::Track*> result;

    for (const auto& track : tracks)
        if (auto* t = track.get())
            result.add (t);

    return result;
}

//==============================================================================
namespace
{
    void deleteSelection (te::SelectionManager& selection, const RangeSelection& range, bool closeGap)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return;

        if (! range.isEmpty())
        {
            deleteTimeRange (*edit, range.getRange(), range.getTracks(), closeGap);
            return;
        }

        if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
        {
            deleteClips (clips, closeGap);
            return;
        }

        const auto tracks = getSelectedOutermostTracks (selection);

        if (tracks.isEmpty() || closeGap)
            return;

        const auto row = getPlaylistTracks (*edit).indexOf (tracks.getFirst());

        for (auto* track : tracks)
            edit->deleteTrack (track);

        // The track that takes the first one's place is selected, as in Ableton Live, so the keyboard
        // can carry on from there.
        if (const auto remaining = getPlaylistTracks (*edit); ! remaining.isEmpty())
            selection.selectOnly (remaining[juce::jmin (row, remaining.size() - 1)]);
    }

    void duplicateSelection (te::SelectionManager& selection, bool asAliases)
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

            for (auto* copy : copyClips (clips, asAliases)) // each starts where its original is
            {
                if (auto* track = copy->getClipTrack())
                    moveClip (*copy, copy->getStartBeat() + (end - start), *track);

                selection.addToSelection (copy);
            }

            return;
        }

        if (asAliases)
            return;

        // Tracks: each copy goes right after its original.
        juce::Array<te::Track*> copies;

        for (auto* track : getSelectedOutermostTracks (selection))
            if (auto copy = edit->copyTrack (track, getInsertPointAfter (*track)))
                copies.add (copy.get());

        selection.deselectAll();

        for (auto* copy : copies)
            selection.addToSelection (copy);
    }

    // Splits the clips in the range at its edges, or else the selected clips at the playhead.
    void split (te::SelectionManager& selection, const RangeSelection& range)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return;

        if (! range.isEmpty())
        {
            juce::Array<te::Clip*> clips;

            for (auto* track : range.getTracks())
                if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
                    clips.addArray (clipTrack->getClips());

            const auto parts = splitClips (clips, range.getRange().getStart());
            auto all = clips;
            all.addArray (parts);
            splitClips (all, range.getRange().getEnd());
            return;
        }

        splitClips (getSelectedClips (selection), edit->tempoSequence.toBeats (edit->getTransport().getPosition()));
    }

    void selectAll (te::SelectionManager& selection, RangeSelection& range)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return;

        // Every track while tracks are selected; otherwise every clip, or every track if there are none.
        const auto tracksSelected = ! getSelectedTracks (selection).isEmpty();
        juce::Array<te::Selectable*> items;

        for (auto* track : getPlaylistTracks (*edit))
            if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track); clipTrack != nullptr && ! tracksSelected)
                for (auto* clip : clipTrack->getClips())
                    items.add (clip);

        if (items.isEmpty())
            for (auto* track : getPlaylistTracks (*edit))
                items.add (track);

        range.clear();
        selection.deselectAll();

        for (auto* item : items)
            selection.addToSelection (item);
    }
} // namespace

juce::String describeSelection (const te::SelectionManager& selection, const RangeSelection& range, const juce::String& verb)
{
    const auto describe = [&verb] (int count, const char* noun)
    { return verb + " " + (count == 1 ? juce::String (noun) : juce::String (count) + " " + noun + "s"); };

    if (! range.isEmpty())
        return verb + " Range";

    if (const auto clips = getSelectedClips (selection).size(); clips > 0)
        return describe (clips, "Clip");

    if (const auto tracks = getSelectedTracks (selection).size(); tracks > 0)
        return describe (tracks, "Track");

    return verb;
}

void addSelectionCommands (CommandRegistry& registry, te::SelectionManager& selection, RangeSelection& range)
{
    const auto getEdit = [&selection] { return selection.getEdit(); };
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };
    const auto hasClips = [&selection] { return ! getSelectedClips (selection).isEmpty(); };
    const auto hasRange = [&range] { return ! range.isEmpty(); };
    const auto hasRangeOrClips = [&selection, &range] { return ! range.isEmpty() || ! getSelectedClips (selection).isEmpty(); };
    const auto hasAnything = [&selection, &range]
    { return ! range.isEmpty() || ! getSelectedClips (selection).isEmpty() || ! getSelectedTracks (selection).isEmpty(); };

    registry.add ({ .id = "edit.delete",
                    .name = "Delete",
                    .category = "Edit",
                    .description = "Deletes the selected time range, clips or tracks.",
                    .synonyms = { "remove", "erase" },
                    .perform = [&selection, &range] { deleteSelection (selection, range, false); },
                    .isEnabled = hasAnything,
                    .undoTarget = getEdit,
                    .currentName = [&selection, &range] { return describeSelection (selection, range, "Delete"); } });

    registry.add ({ .id = "edit.deleteAndCloseGap",
                    .name = "Delete and Close Gap",
                    .category = "Edit",
                    .description = "Deletes the selected time range or clips, and moves what comes after them back to close the gap.",
                    .synonyms = { "ripple delete", "shuffle", "remove gap" },
                    .perform = [&selection, &range] { deleteSelection (selection, range, true); },
                    .isEnabled = hasRangeOrClips,
                    .undoTarget = getEdit });

    registry.add (
        { .id = "edit.duplicate",
          .name = "Duplicate",
          .category = "Edit",
          .description = "Duplicates the selected clips, right after them, or the selected tracks, each below its original.",
          .synonyms = { "copy", "clone", "repeat" },
          .perform = [&selection] { duplicateSelection (selection, false); },
          .isEnabled = [&selection] { return ! getSelectedClips (selection).isEmpty() || ! getSelectedTracks (selection).isEmpty(); },
          .undoTarget = getEdit,
          .currentName = [&selection, &range] { return describeSelection (selection, range, "Duplicate"); } });

    registry.add ({ .id = "clip.duplicateAsAlias",
                    .name = "Duplicate as Alias",
                    .category = "Clip",
                    .description = "Duplicates the selected clips right after them, as aliases: editing the notes of one edits them all.",
                    .synonyms = { "linked copy", "ghost", "pattern", "clone" },
                    .perform = [&selection] { duplicateSelection (selection, true); },
                    .isEnabled = hasClips,
                    .undoTarget = getEdit });

    registry.add ({ .id = "clip.makeUnique",
                    .name = "Make Unique",
                    .category = "Clip",
                    .description = "Turns the selected aliases into separate clips, which can be edited on their own.",
                    .synonyms = { "unlink", "independent" },
                    .perform = [&selection] { makeClipsUnique (getSelectedClips (selection)); },
                    .isEnabled =
                        [&selection]
                    {
                        const auto clips = getSelectedClips (selection);
                        return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return clip->isLinked(); });
                    },
                    .undoTarget = getEdit });

    registry.add ({ .id = "clip.mute",
                    .name = "Mute Clips",
                    .category = "Clip",
                    .description = "Silences the selected clips, or unmutes them.",
                    .synonyms = { "deactivate", "disable", "silence" },
                    .perform = [&selection] { toggleClipsMuted (getSelectedClips (selection)); },
                    .isEnabled = hasClips,
                    .undoTarget = getEdit,
                    .isTicked = [&selection] { return areAllClipsMuted (getSelectedClips (selection)); } });

    // Fade shapes, for the selected audio clips' fade-ins and fade-outs.
    struct FadeShape
    {
        te::AudioFadeCurve::Type type;
        const char* id;
        const char* name;
        const char* description;
    };

    const auto hasAudioClips = [&selection]
    {
        const auto clips = getSelectedClips (selection);
        return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return dynamic_cast<te::AudioClipBase*> (clip) != nullptr; });
    };

    for (const auto& shape :
         { FadeShape { te::AudioFadeCurve::linear, "clip.fadeLinear", "Linear Fades", "Fades in a straight line." },
           FadeShape { te::AudioFadeCurve::convex, "clip.fadeConvex", "Convex Fades",
                       "Fades that change fast near silence and slow near full volume." },
           FadeShape { te::AudioFadeCurve::concave, "clip.fadeConcave", "Concave Fades",
                       "Fades that change slowly near silence and fast near full volume." },
           FadeShape { te::AudioFadeCurve::sCurve, "clip.fadeSCurve", "S-Curve Fades", "Fades that start and end gently, like an S." } })
    {
        const auto type = shape.type;
        registry.add ({ .id = shape.id,
                        .name = shape.name,
                        .category = "Clip",
                        .description = juce::String (shape.description) + " For the selected audio clips.",
                        .synonyms = { "fade shape", "fade curve", "crossfade" },
                        .perform = [&selection, type] { setFadeShape (getSelectedClips (selection), type); },
                        .isEnabled = hasAudioClips,
                        .undoTarget = getEdit,
                        .isTicked =
                            [&selection, type]
                        {
                            const auto clips = getSelectedClips (selection);
                            auto any = false;

                            for (auto* clip : clips)
                            {
                                if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
                                {
                                    if (audio->getFadeInType() != type || audio->getFadeOutType() != type)
                                        return false;

                                    any = true;
                                }
                            }

                            return any;
                        } });
    }

    registry.add ({ .id = "edit.split",
                    .name = "Split",
                    .category = "Edit",
                    .description = "Splits the clips in the selected time range at its edges, or the selected clips at the playhead.",
                    .synonyms = { "cut", "slice", "divide", "razor" },
                    .perform = [&selection, &range] { split (selection, range); },
                    .isEnabled = hasRangeOrClips,
                    .undoTarget = getEdit });

    registry.add ({ .id = "edit.insertTime",
                    .name = "Insert Time",
                    .category = "Edit",
                    .description = "Inserts empty time, as long as the selected time range, at its start, on every track.",
                    .synonyms = { "insert silence", "add bars", "make room" },
                    .perform =
                        [&selection, &range]
                    {
                        if (auto* edit = selection.getEdit())
                            insertTime (*edit, range.getRange());
                    },
                    .isEnabled = hasRange,
                    .undoTarget = getEdit });

    registry.add ({ .id = "edit.removeTime",
                    .name = "Remove Time",
                    .category = "Edit",
                    .description = "Removes the selected time range from every track, and moves what comes after it back.",
                    .synonyms = { "delete time", "remove bars", "cut time" },
                    .perform =
                        [&selection, &range]
                    {
                        if (auto* edit = selection.getEdit())
                            deleteTimeRange (*edit, range.getRange(), getPlaylistTracks (*edit), true);
                    },
                    .isEnabled = hasRange,
                    .undoTarget = getEdit });

    registry.add ({ .id = "edit.selectAll",
                    .name = "Select All",
                    .category = "Edit",
                    .description = "Selects every clip, or every track while tracks are selected.",
                    .synonyms = { "everything" },
                    .perform = [&selection, &range] { selectAll (selection, range); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "edit.deselectAll",
                    .name = "Deselect All",
                    .category = "Edit",
                    .description = "Clears the selection.",
                    .synonyms = { "select none", "clear selection" },
                    .perform =
                        [&selection, &range]
                    {
                        range.clear();
                        selection.deselectAll();
                    },
                    .isEnabled = [&selection, &range] { return selection.getNumObjectsSelected() > 0 || ! range.isEmpty(); } });
}
} // namespace phazite
