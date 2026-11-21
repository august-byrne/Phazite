// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clipboard.h"
#include "SongSelection.h"
#include "internal/AudioClips.h"
#include "internal/Clips.h"
#include "internal/Selection.h"
#include "internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <map>
#include <optional>

namespace phazite
{
namespace
{
    // The tracks that hold clips (not folders or returns: Schema.h, trackKind), in the playlist's order, by which pasted
    // clips keep their tracks' places.
    std::vector<TrackID> getTracksForClips (const Song& song)
    {
        std::vector<TrackID> tracks;

        for (const auto& state : getTrackStates (song))
            if (! isFolderState (state) && ! isReturnState (state))
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
            const auto value = getClipValue (song, clip);
            const auto beats = value.has_value() ? getLiveClipRange (song, *value) : BeatRange();
            span = span.has_value() ? span->getUnionWith (beats) : beats;

            if (const auto index = value.has_value() ? indexOf (tracks, value->track) : -1; index >= 0)
                clipboard.clips.push_back ({ copyWhole (song, findClipState (song, clip)), beats, index - first }); // with its notes
        }

        if (range.has_value())
            clipboard.range = *range;
        else if (span.has_value())
            clipboard.range = *span;

        if (juce::isPositiveAndBelow (first, (int) tracks.size()))
            clipboard.track = tracks[(size_t) first];
    }

    // Where Paste puts clips: from the selected track down (from the first track below a selected folder or return, which
    // hold no clips, or from a track added for them when none is below), else from the first selected clip's track, else
    // from where they came from.
    struct PasteTarget
    {
        TrackID track;
        bool newTrack = false;

        bool isValid() const noexcept { return track.isValid() || newTrack; }
    };

    PasteTarget getPasteTarget (const Clipboard& clipboard, const Selection& selection)
    {
        auto* song = selection.getSong();

        if (song == nullptr)
            return {};

        const auto tracks = getTracksForClips (*song);

        if (const auto selected = getSelectedTracks (selection); ! selected.empty())
        {
            for (const auto track : selected)
                if (indexOf (tracks, track) >= 0)
                    return { track };

            const auto states = getTrackStates (*song);
            const auto from = std::find (states.begin(), states.end(), findTrackState (*song, selected.front()));

            for (auto state = from; state != states.end(); ++state)
                if (const auto track = getTrackID (*state); indexOf (tracks, track) >= 0)
                    return { track };

            if (from != states.end())
                return { {}, true };
        }

        for (const auto clip : getSelectedClips (selection))
            if (const auto track = getClipTrack (*song, clip); indexOf (tracks, track) >= 0)
                return { track };

        return { indexOf (tracks, clipboard.track) >= 0 ? clipboard.track : TrackID() };
    }

    // A track added for pasted clips: after the last of the list's own (not in a folder, where a collapsed folder would hide
    // it), before the returns.
    TrackID addTrackForPaste (Song& song)
    {
        TrackID last;
        TrackID firstReturn;

        for (const auto& state : getTrackList (song))
        {
            if (! state.hasType (schema::TRACK))
                continue;

            if (isReturnState (state))
            {
                if (! firstReturn.isValid())
                    firstReturn = getTrackID (state);
            }
            else
            {
                last = getTrackID (state);
            }
        }

        const auto added = insertTrack (song, last, nullptr); // at the end with no track before it

        if (added.isValid() && ! last.isValid() && firstReturn.isValid())
            moveTracksTo (song, { added }, firstReturn, TrackDrop::before);

        return added;
    }

    // The MIDI clips the piano roll shows, which notes are pasted into.
    std::vector<ClipID> getNoteClips (ProjectContext& context)
    {
        std::vector<ClipID> clips;

        if (auto* song = findSong (context))
            for (const auto id : context.getSongSelection().getNoteClips())
                if (isMidiClip (*song, id))
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

        const auto made = replaceNotes (*song, {}, notes);
        tellNotesMade (*song, describePaste (clipboard), "Paste", notes.size(), made.size());
        context.getSongSelection().selectNotes (made);
    }

    void pasteClips (const Clipboard& clipboard, ProjectContext& context)
    {
        auto& selection = getSelection (context);
        auto* song = findSong (context);
        const auto pasteTarget = getPasteTarget (clipboard, selection);

        if (song == nullptr || ! pasteTarget.isValid())
            return;

        const auto target = pasteTarget.newTrack ? addTrackForPaste (*song) : pasteTarget.track;
        auto tracks = getTracksForClips (*song);
        const auto first = indexOf (tracks, target);
        const auto distance = getPlayheadBeat (*song) - clipboard.range.getStart();

        // Clips from more tracks than there are from the target down: empty tracks are added after the last, as REAPER,
        // Studio One and Logic Pro add them, rather than leave clips out.
        auto needed = 0;

        for (const auto& copied : clipboard.clips)
            needed = std::max (needed, first + copied.trackOffset + 1);

        while (first >= 0 && needed > (int) tracks.size())
        {
            const auto added = addTrackForPaste (*song);

            if (! added.isValid())
                break;

            tracks.push_back (added);
        }

        const auto isRange = clipboard.kind == Clipboard::Kind::range;

        // Each clip's track: its own place from the target down, or the next below that can hold its kind (audio and notes
        // don't share a track: Clips.h, canTrackHold()), counting what this paste puts on each; or a track added for it.
        std::vector<TrackID> destinations;
        std::map<TrackID, bool> given; // whether each track this paste uses takes audio clips

        for (const auto& copied : clipboard.clips)
        {
            const auto audio = isAudioState (copied.state);
            const auto canTake = [&] (TrackID track)
            {
                const auto found = given.find (track);
                return found != given.end() ? found->second == audio : canTrackHold (*song, track, ! audio);
            };

            TrackID track;

            for (auto index = first + copied.trackOffset; first >= 0 && index < (int) tracks.size() && ! track.isValid(); ++index)
                if (index >= 0 && canTake (tracks[(size_t) index]))
                    track = tracks[(size_t) index];

            if (! track.isValid() && first >= 0)
                if ((track = addTrackForPaste (*song)).isValid())
                    tracks.push_back (track);

            if (track.isValid())
                given[track] = audio;

            destinations.push_back (track);
        }

        // Where each lands (a range's clips cut to it): cleared first, so that no pasted clip cuts another.
        const auto landingOf = [&clipboard, isRange, distance] (const Clipboard::CopiedClip& copied)
        { return (isRange ? copied.beats.getIntersectionWith (clipboard.range) : copied.beats).movedBy (distance); };

        for (size_t i = 0; i < clipboard.clips.size(); ++i)
            if (destinations[i].isValid())
                clearClipsIn (*song, destinations[i], landingOf (clipboard.clips[i]));

        // Each a copy, not an alias, as Duplicate makes them: made where it was, cut to the range, and moved to where it lands.
        std::vector<ClipID> pasted;

        for (size_t i = 0; i < clipboard.clips.size(); ++i)
        {
            const auto& copied = clipboard.clips[i];
            const auto track = destinations[i];

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

            for (const auto track : tracks)
                if (std::find (destinations.begin(), destinations.end(), track) != destinations.end())
                    landed.push_back (track);

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
            return getPasteTarget (clipboard, getSelection (context)).isValid();

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
