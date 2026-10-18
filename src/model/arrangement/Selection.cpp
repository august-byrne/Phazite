// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Selection.h"
#include "internal/Arrangement.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>
#include <vector>

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

namespace
{
    // RangeSelection::setInUndoStep()'s part of an undo step: performed or redone, it sets the range; undone, it puts
    // back the range there was. It holds the range weakly, since a range can go before its Edit's undo history does
    // (tests declare the range after the Edit), and the tracks by ID, since Undo gives a deleted track back as a new
    // object.
    class SetRange : public juce::UndoableAction
    {
    public:
        SetRange (RangeSelection& selection, te::Edit& e, te::BeatRange newRange, const juce::Array<te::Track*>& newTracks)
            : target (&selection),
              edit (e),
              before (selection.getRange()),
              after (newRange),
              tracksBefore (getIDs (selection.getTracks())),
              tracksAfter (getIDs (newTracks))
        {
        }

        bool perform() override { return apply (after, tracksAfter); }
        bool undo() override { return apply (before, tracksBefore); }

    private:
        juce::WeakReference<RangeSelection> target;
        te::Edit& edit;
        te::BeatRange before, after;
        std::vector<te::EditItemID> tracksBefore, tracksAfter;

        static std::vector<te::EditItemID> getIDs (const juce::Array<te::Track*>& tracks)
        {
            std::vector<te::EditItemID> ids;

            for (auto* track : tracks)
                ids.push_back (track->itemID);

            return ids;
        }

        // Always true: JUCE stops undoing a step at an action that fails, and clears the undo history.
        bool apply (te::BeatRange newRange, const std::vector<te::EditItemID>& ids)
        {
            if (auto* selection = target.get())
            {
                juce::Array<te::Track*> tracks;

                for (auto id : ids)
                    if (auto* track = te::findTrackForID (edit, id))
                        tracks.add (track);

                selection->set (newRange, tracks);
            }

            return true;
        }
    };
} // namespace

void RangeSelection::setInUndoStep (te::Edit& edit, te::BeatRange newRange, const juce::Array<te::Track*>& newTracks)
{
    edit.getUndoManager().perform (new SetRange (*this, edit, newRange, newTracks)); // the undo manager owns it
}

//==============================================================================
void deleteSelection (te::SelectionManager& selection, const RangeSelection& range, bool closeGap)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return;

    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
    {
        deleteNotes (notes);
        return;
    }

    if (const auto points = getSelectedPoints (selection))
    {
        deletePoints (*points->parameter, points->indices);
        selection.deselectAll(); // their indices name other points now
        return;
    }

    if (! range.isEmpty())
    {
        deleteTimeRange (*edit, range.getRange(), range.getTracks(), closeGap);
        return;
    }

    // Sections go with what is in them, and the rest of the song closes up.
    if (const auto sections = getSelectedSections (selection); ! sections.isEmpty())
    {
        deleteSections (sections);
        return;
    }

    if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
    {
        deleteClips (clips, closeGap);
        return;
    }

    if (! closeGap)
        deleteTracks (getSelectedOutermostTracks (selection), &selection);
}

void duplicateSelection (te::SelectionManager& selection, RangeSelection& range, bool asAliases)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return;

    // Notes: the copies go right after them, and are selected instead.
    if (const auto notes = getSelectedNotes (selection); ! notes.empty() && ! asAliases)
    {
        auto* events = getSelectedMidiEvents (selection); // before the copies change the clips
        events->setSelected (selection, toMidiNotes (duplicateNotes (notes)), false);
        return;
    }

    // Automation points: the copies go right after them (a beat after one point alone), and are selected instead.
    if (const auto points = getSelectedPoints (selection); points.has_value() && ! asAliases)
    {
        const auto copies = duplicatePoints (*points->parameter, points->indices, BeatDuration::fromBeats (1.0));
        selectPoints (selection, *points->parameter, copies);
        return;
    }

    // A time range, before sections, clips and tracks (the order Delete and the command's name take): what lies in
    // it goes again right after it, on its tracks, and the copy becomes the range in the same undo step, so that
    // Duplicate again repeats it and Undo puts the range back. What else is selected stays, as Delete leaves it.
    if (! range.isEmpty() && ! asAliases)
    {
        const auto tracks = range.getTracks();
        const auto copied = duplicateTimeRange (*edit, range.getRange(), tracks);
        range.setInUndoStep (*edit, copied, tracks);
        return;
    }

    // Sections, with what is in them, and clips go right after themselves.
    if (const auto sections = getSelectedSections (selection); ! sections.isEmpty())
    {
        duplicateSections (sections, asAliases, &selection);
        return;
    }

    if (const auto clips = getSelectedClips (selection); ! clips.isEmpty())
    {
        duplicateClips (clips, asAliases, &selection);
        return;
    }

    if (! asAliases)
        duplicateTracks (getSelectedOutermostTracks (selection), &selection);
}

void splitSelection (const te::SelectionManager& selection, const RangeSelection& range)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return;

    if (! range.isEmpty())
        splitClipsAtEdges (range.getTracks(), range.getRange());
    else
        splitClips (getSelectedClips (selection), edit->tempoSequence.toBeats (edit->getTransport().getPosition()));
}

void selectAllClipsOrTracks (te::SelectionManager& selection, RangeSelection& range)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return;

    // Every track while tracks are selected; otherwise every clip, or every track if there are none.
    const auto tracksSelected = hasSelectedTracks (selection);
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

void clearSelection (te::SelectionManager& selection, RangeSelection& range)
{
    range.clear();
    selection.deselectAll();
}

void toggleSelectionMuted (const te::SelectionManager& selection)
{
    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        toggleNotesMuted (notes);
    else
        toggleClipsMuted (getSelectedClips (selection));
}

bool isSelectionMuted (const te::SelectionManager& selection)
{
    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        return areAllNotesMuted (notes);

    return areAllClipsMuted (getSelectedClips (selection));
}

bool canDeleteSelection (const te::SelectionManager& selection, const RangeSelection& range)
{
    return hasSelectedNotes (selection) || getSelectedPoints (selection).has_value() || ! range.isEmpty() || hasSelectedSections (selection)
           || hasSelectedClips (selection) || hasSelectedTracks (selection);
}

bool hasSelectedRangeOrClips (const te::SelectionManager& selection, const RangeSelection& range)
{
    return ! range.isEmpty() || hasSelectedClips (selection);
}

bool canDuplicateAsAliases (const te::SelectionManager& selection)
{
    return hasSelectedClips (selection) || hasSelectedSections (selection);
}

bool canMuteSelection (const te::SelectionManager& selection)
{
    return hasSelectedNotes (selection) || hasSelectedClips (selection);
}

bool isAnythingSelected (const te::SelectionManager& selection, const RangeSelection& range)
{
    return selection.getNumObjectsSelected() > 0 || ! range.isEmpty();
}

juce::String describeSelection (const te::SelectionManager& selection, const RangeSelection& range, const juce::String& verb)
{
    const auto describe = [&verb] (int count, const char* noun)
    { return verb + " " + (count == 1 ? juce::String (noun) : juce::String (count) + " " + noun + "s"); };

    if (const auto notes = getSelectedNotes (selection).size(); notes > 0)
        return describe ((int) notes, "Note");

    if (const auto points = getSelectedPoints (selection))
        return describe ((int) points->indices.size(), "Automation Point");

    if (! range.isEmpty())
        return verb + " Range";

    if (const auto sections = getSelectedSections (selection).size(); sections > 0)
        return describe (sections, "Section");

    if (const auto clips = getSelectedClips (selection).size(); clips > 0)
        return describe (clips, "Clip");

    if (const auto tracks = getSelectedTracks (selection).size(); tracks > 0)
        return describe (tracks, "Track");

    return verb;
}

void addSelectionCommands (CommandRegistry& registry, te::SelectionManager& selection, RangeSelection& range)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "edit.delete",
                    .name = "Delete",
                    .category = "Edit",
                    .description = "Deletes the selected notes, automation points, time range, sections, clips or tracks. A section "
                                   "goes with what is in it.",
                    .synonyms = { "remove", "erase" },
                    .perform = [&selection, &range] { deleteSelection (selection, range, false); },
                    .isEnabled = [&selection, &range] { return canDeleteSelection (selection, range); },
                    .undoTarget = undoTarget,
                    .currentName = [&selection, &range] { return describeSelection (selection, range, "Delete"); } });

    registry.add ({ .id = "edit.deleteAndCloseGap",
                    .name = "Delete and Close Gap",
                    .category = "Edit",
                    .description = "Deletes the selected time range or clips, and moves what comes after them back to close the gap.",
                    .synonyms = { "ripple delete", "shuffle", "remove gap" },
                    .perform = [&selection, &range] { deleteSelection (selection, range, true); },
                    .isEnabled = [&selection, &range] { return hasSelectedRangeOrClips (selection, range); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "edit.duplicate",
                    .name = "Duplicate",
                    .category = "Edit",
                    .description =
                        "Duplicates the selected notes, automation points, time range, sections or clips right after them, or the "
                        "selected tracks, each below its original. A time range's copy replaces what it lands on, and the range "
                        "moves to it.",
                    .synonyms = { "copy", "clone", "repeat" },
                    .perform = [&selection, &range] { duplicateSelection (selection, range, false); },
                    .isEnabled = [&selection, &range] { return canDeleteSelection (selection, range); },
                    .undoTarget = undoTarget,
                    .currentName = [&selection, &range] { return describeSelection (selection, range, "Duplicate"); } });

    registry.add ({ .id = "edit.duplicateAsAlias",
                    .name = "Duplicate as Alias",
                    .category = "Edit",
                    .description = "Duplicates the selected sections or clips right after them, as aliases: editing the notes of one edits "
                                   "them all. A section is copied with what is in it.",
                    .synonyms = { "linked copy", "ghost", "pattern", "clone", "repeat section" },
                    .perform = [&selection, &range] { duplicateSelection (selection, range, true); },
                    .isEnabled = [&selection] { return canDuplicateAsAliases (selection); },
                    .undoTarget = undoTarget,
                    .currentName = [&selection]
                    { return hasSelectedSections (selection) ? juce::String ("Duplicate Section as Aliases") : juce::String(); } });

    registry.add ({ .id = "clip.makeUnique",
                    .name = "Make Unique",
                    .category = "Clip",
                    .description = "Turns the selected aliases into separate clips, which can be edited on their own.",
                    .synonyms = { "unlink", "independent" },
                    .perform = [&selection] { makeClipsUnique (getSelectedClips (selection)); },
                    .isEnabled = [&selection] { return hasAliases (getSelectedClips (selection)); },
                    .undoTarget = undoTarget });

    registry.add ({ .id = "edit.mute",
                    .name = "Mute",
                    .category = "Edit",
                    .description = "Silences the selected notes or clips, or unmutes them.",
                    .synonyms = { "deactivate", "disable", "silence" },
                    .perform = [&selection] { toggleSelectionMuted (selection); },
                    .isEnabled = [&selection] { return canMuteSelection (selection); },
                    .undoTarget = undoTarget,
                    .isTicked = [&selection] { return isSelectionMuted (selection); } });

    // Fade shapes, for the selected audio clips' fade-ins and fade-outs.
    struct FadeShape
    {
        te::AudioFadeCurve::Type type;
        const char* id;
        const char* name;
        const char* description;
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
                        .isEnabled = [&selection] { return hasAudioClips (getSelectedClips (selection)); },
                        .undoTarget = undoTarget,
                        .isTicked = [&selection, type] { return hasFadeShape (getSelectedClips (selection), type); } });
    }

    // What the selected audio clips play: backwards, and stretched to the song's tempo (as the audio clip editor sets them).
    registry.add ({ .id = "clip.reverse",
                    .name = "Reverse",
                    .category = "Clip",
                    .description = "Plays the selected audio clips backwards, or forwards again.",
                    .synonyms = { "backwards", "reverse audio", "play backwards" },
                    .perform = [&selection] { toggleClipsReversed (getSelectedClips (selection)); },
                    .isEnabled = [&selection] { return hasAudioClips (getSelectedClips (selection)); },
                    .undoTarget = undoTarget,
                    .isTicked = [&selection] { return areAllClipsReversed (getSelectedClips (selection)); } });

    registry.add ({ .id = "clip.followTempo",
                    .name = "Follow Tempo",
                    .category = "Clip",
                    .description = "Stretches the selected audio clips to follow the song's tempo, keeping their pitch, or plays them at "
                                   "their own speed again.",
                    .synonyms = { "warp", "stretch", "time stretch", "sync to tempo", "tempo" },
                    .perform = [&selection] { toggleClipsFollowTempo (getSelectedClips (selection)); },
                    .isEnabled = [&selection] { return hasAudioClips (getSelectedClips (selection)); },
                    .undoTarget = undoTarget,
                    .isTicked = [&selection] { return doAllClipsFollowTempo (getSelectedClips (selection)); } });

    registry.add ({ .id = "edit.split",
                    .name = "Split",
                    .category = "Edit",
                    .description = "Splits the clips in the selected time range at its edges, or the selected clips at the playhead.",
                    .synonyms = { "cut", "slice", "divide", "razor" },
                    .perform = [&selection, &range] { splitSelection (selection, range); },
                    .isEnabled = [&selection, &range] { return hasSelectedRangeOrClips (selection, range); },
                    .undoTarget = undoTarget });

    registry.add (
        { .id = "edit.insertTime",
          .name = "Insert Time",
          .category = "Edit",
          .description =
              "Inserts empty time, as long as the selected time range, at its start, on every track. A section it falls inside gets longer.",
          .synonyms = { "insert silence", "add bars", "make room" },
          .perform = withEdit (selection,
                               [&range] (te::Edit& edit) { insertTime (edit, range.getRange().getStart(), range.getRange().getLength()); }),
          .isEnabled = [&range] { return ! range.isEmpty(); },
          .undoTarget = undoTarget });

    registry.add (
        { .id = "edit.removeTime",
          .name = "Remove Time",
          .category = "Edit",
          .description =
              "Removes the selected time range from every track, and moves what comes after it back. Sections that reach into it get shorter.",
          .synonyms = { "delete time", "remove bars", "cut time" },
          .perform = withEdit (selection, [&range] (te::Edit& edit) { removeTime (edit, range.getRange()); }),
          .isEnabled = [&range] { return ! range.isEmpty(); },
          .undoTarget = undoTarget });

    registry.add ({ .id = "edit.selectAll",
                    .name = "Select All",
                    .category = "Edit",
                    .description = "Selects every clip, or every track while tracks are selected.",
                    .synonyms = { "everything" },
                    .perform = [&selection, &range] { selectAllClipsOrTracks (selection, range); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "edit.deselectAll",
                    .name = "Deselect All",
                    .category = "Edit",
                    .description = "Clears the selection.",
                    .synonyms = { "select none", "clear selection" },
                    .perform = [&selection, &range] { clearSelection (selection, range); },
                    .isEnabled = [&selection, &range] { return isAnythingSelected (selection, range); } });
}
} // namespace phazite
