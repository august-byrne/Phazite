// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Selection.h"
#include "AudioClipModel.h"
#include "internal/Arrangement.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <vector>

namespace phazite
{
//==============================================================================
RangeSelection::RangeSelection (const Selection& s)
    : selection (s)
{
}

void RangeSelection::set (BeatRange newRange, const std::vector<TrackID>& newTracks)
{
    range = newRange;
    tracks = newTracks;
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
    return range.getLength() <= BeatDuration() || getTracks().empty();
}

std::vector<TrackID> RangeSelection::getTracks() const
{
    std::vector<TrackID> there;

    if (auto* song = selection.getSong())
        for (const auto track : tracks)
            if (findTrackState (*song, track).isValid())
                there.push_back (track);

    return there;
}

namespace
{
    // RangeSelection::setInUndoStep()'s part of an undo step: performed or redone, it sets the range; undone, it puts
    // back the range there was. It holds the range weakly, since a range can go before its song's undo history does
    // (tests declare the range after the song).
    class SetRange : public juce::UndoableAction
    {
    public:
        SetRange (RangeSelection& selection, BeatRange newRange, const std::vector<TrackID>& newTracks)
            : target (&selection),
              before (selection.getRange()),
              after (newRange),
              tracksBefore (selection.getTracks()),
              tracksAfter (newTracks)
        {
        }

        bool perform() override { return apply (after, tracksAfter); }
        bool undo() override { return apply (before, tracksBefore); }

    private:
        juce::WeakReference<RangeSelection> target;
        BeatRange before, after;
        std::vector<TrackID> tracksBefore, tracksAfter;

        // Always true: JUCE stops undoing a step at an action that fails, and clears the undo history.
        bool apply (BeatRange newRange, const std::vector<TrackID>& ids)
        {
            if (auto* selection = target.get())
                selection->set (newRange, ids);

            return true;
        }
    };
} // namespace

void RangeSelection::setInUndoStep (Song& song, BeatRange newRange, const std::vector<TrackID>& newTracks)
{
    song.getUndoManager().perform (new SetRange (*this, newRange, newTracks)); // the undo manager owns it
}

//==============================================================================
void deleteSelection (Selection& selection, const RangeSelection& range, bool closeGap)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
    {
        deleteNotes (*song, notes);
        return;
    }

    if (const auto points = getSelectedPoints (selection))
    {
        deletePoints (*song, points->parameter, points->indices);
        selection.deselectAll(); // their indices name other points now
        return;
    }

    if (! range.isEmpty())
    {
        deleteTimeRange (*song, range.getRange(), range.getTracks(), closeGap);
        return;
    }

    // Sections go with what is in them, and the rest of the song closes up.
    if (const auto sections = getSelectedSections (selection); ! sections.empty())
    {
        deleteSections (*song, getIDs (sections));
        return;
    }

    if (const auto clips = getSelectedClips (selection); ! clips.empty())
    {
        deleteClips (*song, clips, closeGap);
        return;
    }

    if (! closeGap)
        deleteTracks (*song, getSelectedOutermostTracks (selection), &selection);
}

void duplicateSelection (Selection& selection, RangeSelection& range, bool asAliases)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    // Notes: the copies go right after them, and are selected instead.
    if (const auto notes = getSelectedNotes (selection); ! notes.empty() && ! asAliases)
    {
        selectNotes (selection, duplicateNotes (*song, notes));
        return;
    }

    // Automation points: the copies go right after them (a beat after one point alone), and are selected instead.
    if (const auto points = getSelectedPoints (selection); points.has_value() && ! asAliases)
    {
        const auto copies = duplicatePoints (*song, points->parameter, points->indices, BeatDuration::fromBeats (1.0));
        selectPoints (selection, points->parameter, copies);
        return;
    }

    // A time range, before sections, clips and tracks (the order Delete and the command's name take): what lies in
    // it goes again right after it, on its tracks, and the copy becomes the range in the same undo step, so that
    // Duplicate again repeats it and Undo puts the range back. What else is selected stays, as Delete leaves it.
    if (! range.isEmpty() && ! asAliases)
    {
        const auto tracks = range.getTracks();
        const auto copied = duplicateTimeRange (*song, range.getRange(), tracks);
        range.setInUndoStep (*song, copied, tracks);
        return;
    }

    // Sections, with what is in them, and clips go right after themselves.
    if (const auto sections = getSelectedSections (selection); ! sections.empty())
    {
        duplicateSections (*song, getIDs (sections), asAliases, &selection);
        return;
    }

    if (const auto clips = getSelectedClips (selection); ! clips.empty())
    {
        duplicateClips (*song, clips, asAliases, &selection);
        return;
    }

    if (! asAliases)
        duplicateTracks (*song, getSelectedOutermostTracks (selection), &selection);
}

void splitSelection (const Selection& selection, const RangeSelection& range)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    if (! range.isEmpty())
        splitClipsAtEdges (*song, range.getTracks(), range.getRange());
    else
        splitClips (*song, getSelectedClips (selection), getPlayheadBeat (*song));
}

void selectAllClipsOrTracks (Selection& selection, RangeSelection& range)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    // Every track while tracks are selected; otherwise every clip, track by track, each track's in time order as the playlist
    // shows them, or every track if there are none.
    std::vector<ClipID> clips;

    if (! hasSelectedTracks (selection))
    {
        for (const auto track : getTrackIDs (*song))
        {
            auto onTrack = getClips (*song, track);
            std::stable_sort (onTrack.begin(), onTrack.end(), [song] (ClipID a, ClipID b)
                              { return getClipRange (*song, a).getStart() < getClipRange (*song, b).getStart(); });
            clips.insert (clips.end(), onTrack.begin(), onTrack.end());
        }
    }

    range.clear();
    selection.deselectAll();

    if (clips.empty())
        for (const auto track : getTrackIDs (*song))
            selection.addToSelection (track);

    for (const auto clip : clips)
        selection.addToSelection (clip);
}

void clearSelection (Selection& selection, RangeSelection& range)
{
    range.clear();
    selection.deselectAll();
}

void toggleSelectionMuted (const Selection& selection)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        toggleNotesMuted (*song, notes);
    else
        toggleClipsMuted (*song, getSelectedClips (selection));
}

bool isSelectionMuted (const Selection& selection)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return false;

    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
        return areAllNotesMuted (*song, notes);

    return areAllClipsMuted (*song, getSelectedClips (selection));
}

bool canDeleteSelection (const Selection& selection, const RangeSelection& range)
{
    return hasSelectedNotes (selection) || getSelectedPoints (selection).has_value() || ! range.isEmpty() || hasSelectedSections (selection)
           || hasSelectedClips (selection) || hasSelectedTracks (selection);
}

bool hasSelectedRangeOrClips (const Selection& selection, const RangeSelection& range)
{
    return ! range.isEmpty() || hasSelectedClips (selection);
}

bool canDuplicateAsAliases (const Selection& selection)
{
    return hasSelectedClips (selection) || hasSelectedSections (selection);
}

bool canMuteSelection (const Selection& selection)
{
    return hasSelectedNotes (selection) || hasSelectedClips (selection);
}

bool isAnythingSelected (const Selection& selection, const RangeSelection& range)
{
    return selection.getNumObjectsSelected() > 0 || ! range.isEmpty();
}

juce::String describeSelection (const Selection& selection, const RangeSelection& range, const juce::String& verb)
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
        return describe ((int) sections, "Section");

    if (const auto clips = (int) getSelectedClips (selection).size(); clips > 0)
        return describe (clips, "Clip");

    if (const auto tracks = (int) getSelectedTracks (selection).size(); tracks > 0)
        return describe (tracks, "Track");

    return verb;
}

void addSelectionCommands (CommandRegistry& registry, Selection& selection, RangeSelection& range)
{
    const auto undoTarget = undoTargetOf (selection);
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };

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

    registry.add (
        { .id = "clip.makeUnique",
          .name = "Make Unique",
          .category = "Clip",
          .description = "Turns the selected aliases into separate clips, which can be edited on their own.",
          .synonyms = { "unlink", "independent" },
          .perform = withSong (selection, [&selection] (Song& song) { makeClipsUnique (song, getSelectedClips (selection)); }),
          .isEnabled = ifSong (selection, [&selection] (const Song& song) { return hasAliases (song, getSelectedClips (selection)); }),
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
    struct NamedFade
    {
        FadeCurve curve;
        const char* id;
        const char* name;
        const char* description;
    };

    for (const auto& shape :
         { NamedFade { linearFade, "clip.fadeLinear", "Linear Fades", "Fades in a straight line." },
           NamedFade { convexFade, "clip.fadeConvex", "Convex Fades", "Fades that change fast near silence and slow near full volume." },
           NamedFade { concaveFade, "clip.fadeConcave", "Concave Fades",
                       "Fades that change slowly near silence and fast near full volume." },
           NamedFade { sCurveFade, "clip.fadeSCurve", "S-Curve Fades", "Fades that start and end gently, like an S." } })
    {
        const auto type = shape.curve;
        registry.add (
            { .id = shape.id,
              .name = shape.name,
              .category = "Clip",
              .description = juce::String (shape.description) + " For the selected audio clips.",
              .synonyms = { "fade shape", "fade curve", "crossfade" },
              .perform = withSong (selection, [&selection, type] (Song& song) { setFadeShape (song, getSelectedClips (selection), type); }),
              .isEnabled =
                  ifSong (selection, [&selection] (const Song& song) { return hasAudioClips (song, getSelectedClips (selection)); }),
              .undoTarget = undoTarget,
              .isTicked = ifSong (selection, [&selection, type] (const Song& song)
                                  { return hasFadeShape (song, getSelectedClips (selection), type); }) });
    }

    // What the selected audio clips play: backwards, and stretched to the song's tempo (as the audio clip editor sets them).
    registry.add (
        { .id = "clip.reverse",
          .name = "Reverse",
          .category = "Clip",
          .description = "Plays the selected audio clips backwards, or forwards again.",
          .synonyms = { "backwards", "reverse audio", "play backwards" },
          .perform = withSong (selection, [&selection] (Song& song) { toggleClipsReversed (song, getSelectedClips (selection)); }),
          .isEnabled = ifSong (selection, [&selection] (const Song& song) { return hasAudioClips (song, getSelectedClips (selection)); }),
          .undoTarget = undoTarget,
          .isTicked =
              ifSong (selection, [&selection] (const Song& song) { return areAllClipsReversed (song, getSelectedClips (selection)); }) });

    registry.add (
        { .id = "clip.followTempo",
          .name = "Follow Tempo",
          .category = "Clip",
          .description = "Stretches the selected audio clips to follow the song's tempo, keeping their pitch, or plays them at "
                         "their own speed again.",
          .synonyms = { "warp", "stretch", "time stretch", "sync to tempo", "tempo" },
          .perform = withSong (selection, [&selection] (Song& song) { toggleClipsFollowTempo (song, getSelectedClips (selection)); }),
          .isEnabled = ifSong (selection, [&selection] (const Song& song) { return hasAudioClips (song, getSelectedClips (selection)); }),
          .undoTarget = undoTarget,
          .isTicked =
              ifSong (selection, [&selection] (const Song& song) { return doAllClipsFollowTempo (song, getSelectedClips (selection)); }) });

    // Each selected audio clip's gain, from its loudest peak (as the audio clip editor's button sets one clip's).
    registry.add (
        { .id = "clip.normalize",
          .name = "Normalize",
          .category = "Clip",
          .description = "Sets each selected audio clip's gain so that its loudest peak plays at " + juce::String (normalizedPeak, 0)
                         + " dB. Their audio files don't change.",
          .synonyms = { "normalise", "maximize", "peak", "level", "louder", "gain staging" },
          .perform = withSong (selection, [&selection] (Song& song) { normalizeClips (song, getSelectedClips (selection)); }),
          .isEnabled = ifSong (selection, [&selection] (const Song& song) { return hasAudioClips (song, getSelectedClips (selection)); }),
          .undoTarget = undoTarget });

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
          .perform =
              withSong (selection, [&range] (Song& song) { insertTime (song, range.getRange().getStart(), range.getRange().getLength()); }),
          .isEnabled = [&range] { return ! range.isEmpty(); },
          .undoTarget = undoTarget });

    registry.add (
        { .id = "edit.removeTime",
          .name = "Remove Time",
          .category = "Edit",
          .description =
              "Removes the selected time range from every track, and moves what comes after it back. Sections that reach into it get shorter.",
          .synonyms = { "delete time", "remove bars", "cut time" },
          .perform = withSong (selection, [&range] (Song& song) { removeTime (song, range.getRange()); }),
          .isEnabled = [&range] { return ! range.isEmpty(); },
          .undoTarget = undoTarget });

    registry.add ({ .id = "edit.selectAll",
                    .name = "Select All",
                    .category = "Edit",
                    .description = "Selects every clip, or every track while tracks are selected.",
                    .synonyms = { "everything" },
                    .perform = [&selection, &range] { selectAllClipsOrTracks (selection, range); },
                    .isEnabled = hasSong });

    registry.add ({ .id = "edit.deselectAll",
                    .name = "Deselect All",
                    .category = "Edit",
                    .description = "Clears the selection.",
                    .synonyms = { "select none", "clear selection" },
                    .perform = [&selection, &range] { clearSelection (selection, range); },
                    .isEnabled = [&selection, &range] { return isAnythingSelected (selection, range); } });
}
} // namespace phazite
