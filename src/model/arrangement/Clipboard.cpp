// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clipboard.h"
#include "SongSelection.h"
#include "internal/Clips.h"
#include "internal/Selection.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <optional>

namespace phazite
{
namespace
{
    // The tracks that hold clips (not folders), in the playlist's order, by which pasted clips keep their tracks' places.
    std::vector<TrackID> getTracksForClips (const Song& song)
    {
        std::vector<TrackID> tracks;

        for (const auto& state : getTrackStates (song))
            if (! isFolderState (state))
                tracks.push_back (getTrackID (state));

        return tracks;
    }

    int indexOf (const std::vector<TrackID>& tracks, TrackID track)
    {
        const auto found = std::find (tracks.begin(), tracks.end(), track);
        return found != tracks.end() ? (int) (found - tracks.begin()) : -1;
    }

    // A clip made from what Copy kept, where it was, on a track: a copy, in no alias group, with an ID of its own.
    ClipID pasteClip (Song& song, const juce::ValueTree& kept, TrackID track)
    {
        auto clipState = copyClipState (song, kept);
        clipState.removeProperty (schema::clipLink, nullptr);
        return placeClipState (song, clipState, track);
    }

    // Keeps the clips, each with its track's place from the first of them, and the time from the earliest start to the latest
    // end (or the range's, given one).
    void keepClips (Clipboard& clipboard, const Song& song, const std::vector<ClipID>& clips, std::optional<BeatRange> range)
    {
        const auto tracks = getTracksForClips (song);
        auto first = (int) tracks.size();

        for (const auto clip : clips)
            if (const auto index = indexOf (tracks, getClipTrack (song, clip)); index >= 0)
                first = std::min (first, index);

        std::optional<BeatRange> span;

        for (const auto clip : clips)
        {
            const auto clipState = findClipState (song, clip);
            const auto beats = getClipRange (song, clipState);
            span = span.has_value() ? span->getUnionWith (beats) : beats;

            if (const auto index = indexOf (tracks, getTrackID (clipState.getParent())); index >= 0)
                clipboard.clips.push_back ({ clipState.createCopy(), beats, index - first });
        }

        if (range.has_value())
            clipboard.range = *range;
        else if (span.has_value())
            clipboard.range = *span;

        if (juce::isPositiveAndBelow (first, (int) tracks.size()))
            clipboard.track = tracks[(size_t) first];
    }

    // Where Paste puts clips: from the selected track down, else the first selected clip's, else where they came from.
    TrackID getPasteTrack (const Clipboard& clipboard, const Selection& selection)
    {
        auto* song = selection.getSong();

        if (song == nullptr)
            return {};

        const auto tracks = getTracksForClips (*song);

        for (const auto track : getSelectedTracks (selection))
            if (indexOf (tracks, track) >= 0)
                return track;

        for (const auto clip : getSelectedClips (selection))
            if (const auto track = getClipTrack (*song, clip); indexOf (tracks, track) >= 0)
                return track;

        return indexOf (tracks, clipboard.track) >= 0 ? clipboard.track : TrackID();
    }

    // The MIDI clips the piano roll shows, which notes are pasted into.
    std::vector<ClipID> getNoteClips (ProjectContext& context)
    {
        std::vector<ClipID> clips;

        if (auto* song = findSong (context))
            for (const auto id : context.getSongSelection().getNoteClips())
                if (isMidiState (findClipState (*song, id)))
                    clips.push_back (id);

        return clips;
    }

    void pasteNotes (const Clipboard& clipboard, ProjectContext& context)
    {
        auto* song = findSong (context);
        const auto clips = getNoteClips (context);

        if (song == nullptr || clips.empty())
            return;

        // At the playhead in the clip there, or as far into the first clip as they were into theirs.
        const auto playhead = getPlayheadBeat (*song);
        auto target = clips.front();
        auto at = getClipRange (*song, target).getStart() + clipboard.intoClip;

        for (const auto clip : clips)
        {
            if (getClipRange (*song, clip).contains (playhead))
            {
                target = clip;
                at = playhead;
                break;
            }
        }

        auto notes = clipboard.notes;

        for (auto& note : notes)
        {
            note.clip = target;
            note.start = at + (note.start - BeatPosition());
        }

        context.getSongSelection().selectNotes (replaceNotes (*song, {}, notes));
    }

    void pasteClips (const Clipboard& clipboard, ProjectContext& context)
    {
        auto& selection = getSelection (context);
        auto* song = findSong (context);
        const auto target = getPasteTrack (clipboard, selection);

        if (song == nullptr || ! target.isValid())
            return;

        const auto tracks = getTracksForClips (*song);
        const auto first = indexOf (tracks, target);
        const auto distance = getPlayheadBeat (*song) - clipboard.range.getStart();
        const auto isRange = clipboard.kind == Clipboard::Kind::range;

        const auto trackAt = [&tracks] (int index)
        { return juce::isPositiveAndBelow (index, (int) tracks.size()) ? tracks[(size_t) index] : TrackID(); };

        // Where each lands (a range's clips cut to it): cleared first, so that no pasted clip cuts another.
        const auto landingOf = [&clipboard, isRange, distance] (const Clipboard::CopiedClip& copied)
        { return (isRange ? copied.beats.getIntersectionWith (clipboard.range) : copied.beats).movedBy (distance); };

        for (const auto& copied : clipboard.clips)
            if (const auto track = trackAt (first + copied.trackOffset); track.isValid())
                clearClipsIn (*song, track, landingOf (copied));

        // Each a copy, not an alias, as Duplicate makes them: made where it was, cut to the range, and moved to where it lands.
        std::vector<ClipID> pasted;

        for (const auto& copied : clipboard.clips)
        {
            const auto track = trackAt (first + copied.trackOffset);

            if (! track.isValid())
                continue;

            auto clip = pasteClip (*song, copied.state, track);

            if (! clip.isValid())
                continue;

            if (isRange)
            {
                if (const auto rest = splitClips (*song, { clip }, clipboard.range.getStart()); ! rest.empty())
                {
                    deleteClips (*song, { clip }, false); // what lay before the range
                    clip = rest.front();
                }

                deleteClips (*song, splitClips (*song, { clip }, clipboard.range.getEnd()), false);
            }

            moveClip (*song, clip, landingOf (copied).getStart(), track);
            pasted.push_back (clip);
        }

        // The automation, back onto its own tracks.
        if (target == clipboard.track)
            pasteAutomation (*song, clipboard.automation, distance);

        // What was pasted is selected: a range's becomes the range, as a duplicated range does.
        if (isRange)
        {
            std::vector<TrackID> landed;

            for (int i = 0; i < (int) tracks.size(); ++i)
                if (std::any_of (clipboard.clips.begin(), clipboard.clips.end(),
                                 [i, first] (const Clipboard::CopiedClip& copied) { return copied.trackOffset == i - first; }))
                    landed.push_back (tracks[(size_t) i]);

            if (landed.empty())
                landed.push_back (target);

            getRangeSelection (context).setInUndoStep (*song, clipboard.range.movedBy (distance), landed);
            return;
        }

        selection.deselectAll();

        for (const auto clip : pasted)
            selection.addToSelection (clip);
    }
} // namespace

bool canCopy (const Selection& selection, const RangeSelection& range)
{
    return hasSelectedNotes (selection) || ! range.isEmpty() || hasSelectedClips (selection);
}

void copySelection (Clipboard& clipboard, const Selection& selection, const RangeSelection& range)
{
    auto* song = selection.getSong();

    if (song == nullptr || ! canCopy (selection, range))
        return;

    Clipboard copy;

    // Notes, as far apart as they are, and as far into its clip as the earliest is.
    if (const auto notes = getNoteData (*song, getSelectedNotes (selection)); ! notes.empty())
    {
        copy.kind = Clipboard::Kind::notes;
        copy.notes = notes;
        auto earliest = notes.front();

        for (const auto& note : notes)
            if (note.start < earliest.start)
                earliest = note;

        copy.intoClip = earliest.start - getClipRange (*song, earliest.clip).getStart();

        for (auto& note : copy.notes)
        {
            note.start = BeatPosition() + (note.start - earliest.start);
            note.clip = {};
            note.id = {};
        }
    }
    // A time range: the clips across it, whole (Paste cuts them to it), and its tracks' automation in it.
    else if (! range.isEmpty())
    {
        copy.kind = Clipboard::Kind::range;
        const auto tracks = range.getTracks();
        std::vector<ClipID> clips;

        for (const auto track : tracks)
            for (const auto clip : getClips (*song, track))
                if (getClipRange (*song, clip).overlaps (range.getRange()))
                    clips.push_back (clip);

        keepClips (copy, *song, clips, range.getRange());

        // A range without clips pastes from its first track that holds them.
        if (! copy.track.isValid())
            for (const auto track : getTracksForClips (*song))
                if (std::find (tracks.begin(), tracks.end(), track) != tracks.end())
                {
                    copy.track = track;
                    break;
                }

        std::vector<AutomationStretch> stretches;

        for (const auto track : tracks)
            stretches.push_back ({ track, range.getRange() });

        copy.automation = copyAutomation (*song, stretches);
    }
    // Clips, with the automation under each.
    else
    {
        copy.kind = Clipboard::Kind::clips;
        const auto clips = getSelectedClips (selection);
        keepClips (copy, *song, clips, std::nullopt);
        std::vector<AutomationStretch> stretches;

        for (const auto clip : clips)
            stretches.push_back ({ getClipTrack (*song, clip), getClipRange (*song, clip) });

        copy.automation = copyAutomation (*song, stretches);
    }

    clipboard = std::move (copy);
}

void cutSelection (Clipboard& clipboard, Selection& selection, RangeSelection& range)
{
    auto* song = selection.getSong();

    if (song == nullptr || ! canCopy (selection, range))
        return;

    copySelection (clipboard, selection, range);

    switch (clipboard.kind)
    {
        case Clipboard::Kind::notes:
            deleteNotes (*song, getSelectedNotes (selection));
            break;

        case Clipboard::Kind::range:
            deleteTimeRange (*song, range.getRange(), range.getTracks(), false);
            break;

        case Clipboard::Kind::clips:
            deleteClips (*song, getSelectedClips (selection), false);
            break;

        case Clipboard::Kind::nothing:
            break;
    }
}

juce::String describeCopy (const Selection& selection, const RangeSelection& range, const juce::String& verb)
{
    return canCopy (selection, range) ? describeSelection (selection, range, verb) : verb;
}

bool canPaste (const Clipboard& clipboard, ProjectContext& context)
{
    switch (clipboard.kind)
    {
        case Clipboard::Kind::notes:
            return ! getNoteClips (context).empty();

        case Clipboard::Kind::clips:
        case Clipboard::Kind::range:
            return getPasteTrack (clipboard, getSelection (context)).isValid();

        case Clipboard::Kind::nothing:
            break;
    }

    return false;
}

void paste (const Clipboard& clipboard, ProjectContext& context)
{
    if (! canPaste (clipboard, context))
        return;

    if (clipboard.kind == Clipboard::Kind::notes)
        pasteNotes (clipboard, context);
    else
        pasteClips (clipboard, context);
}

juce::String describePaste (const Clipboard& clipboard)
{
    const auto describe = [] (size_t count, const char* noun)
    { return "Paste " + (count == 1 ? juce::String (noun) : juce::String ((int) count) + " " + noun + "s"); };

    switch (clipboard.kind)
    {
        case Clipboard::Kind::notes:
            return describe (clipboard.notes.size(), "Note");

        case Clipboard::Kind::clips:
            return describe (clipboard.clips.size(), "Clip");

        case Clipboard::Kind::range:
            return "Paste Range";

        case Clipboard::Kind::nothing:
            break;
    }

    return "Paste";
}

void addClipboardCommands (CommandRegistry& registry, ProjectContext& context)
{
    auto& selection = getSelection (context);
    auto& range = getRangeSelection (context);

    registry.add ({ .id = "edit.cut",
                    .name = "Cut",
                    .category = "Edit",
                    .description = "Copies the selected notes, time range or clips, with the automation under the clips, and deletes them, "
                                   "to paste elsewhere.",
                    .synonyms = { "clipboard", "move", "take out" },
                    .perform = [&context, &selection, &range] { cutSelection (getClipboard (context), selection, range); },
                    .isEnabled = [&selection, &range] { return canCopy (selection, range); },
                    .undoTarget = undoTargetOf (selection),
                    .currentName = [&selection, &range] { return describeCopy (selection, range, "Cut"); } });

    registry.add ({ .id = "edit.copy",
                    .name = "Copy",
                    .category = "Edit",
                    .description = "Copies the selected notes, time range or clips, with the automation under the clips, to paste "
                                   "elsewhere.",
                    .synonyms = { "clipboard", "copy notes", "copy clips" },
                    .perform = [&context, &selection, &range] { copySelection (getClipboard (context), selection, range); },
                    .isEnabled = [&selection, &range] { return canCopy (selection, range); },
                    .currentName = [&selection, &range] { return describeCopy (selection, range, "Copy"); } });

    registry.add ({ .id = "edit.paste",
                    .name = "Paste",
                    .category = "Edit",
                    .description = "Puts what was cut or copied at the playhead: notes into the clip the piano roll shows there, clips on "
                                   "the selected track, in place of what is there.",
                    .synonyms = { "clipboard", "insert", "put" },
                    .perform = [&context] { paste (getClipboard (context), context); },
                    .isEnabled = [&context] { return canPaste (getClipboard (context), context); },
                    .undoTarget = undoTargetOf (selection),
                    .currentName = [&context] { return describePaste (getClipboard (context)); } });
}
} // namespace phazite
