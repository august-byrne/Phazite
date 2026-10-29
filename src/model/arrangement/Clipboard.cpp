// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clipboard.h"
#include "SongSelection.h"
#include "internal/Clips.h"
#include "internal/Selection.h"
#include "internal/Tracks.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <optional>

namespace phazite
{
namespace
{
    // The tracks that hold clips, in the playlist's order, by which pasted clips keep their tracks' places.
    juce::Array<te::AudioTrack*> getTracksForClips (const te::Edit& edit)
    {
        juce::Array<te::AudioTrack*> tracks;

        for (auto* track : getPlaylistTracks (edit))
            if (auto* audio = dynamic_cast<te::AudioTrack*> (track))
                tracks.add (audio);

        return tracks;
    }

    te::BeatPosition getPlayhead (const te::Edit& edit)
    {
        return edit.tempoSequence.toBeats (edit.getTransport().getPosition());
    }

    // Keeps the clips, each with its track's place from the first of them, and the time from the earliest start to the latest
    // end (or the range's, given one).
    void keepClips (Clipboard& clipboard, const te::Edit& edit, const juce::Array<te::Clip*>& clips, std::optional<te::BeatRange> range)
    {
        const auto tracks = getTracksForClips (edit);
        auto first = tracks.size();

        for (auto* clip : clips)
            if (const auto index = tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip->getTrack())); index >= 0)
                first = std::min (first, index);

        for (auto* clip : clips)
            if (const auto index = tracks.indexOf (dynamic_cast<te::AudioTrack*> (clip->getTrack())); index >= 0)
                clipboard.clips.push_back ({ clip->state.createCopy(), clip->getEditBeatRange(), index - first });

        if (range.has_value())
        {
            clipboard.range = *range;
        }
        else if (! clips.isEmpty())
        {
            auto start = clips.getFirst()->getStartBeat(), end = clips.getFirst()->getEndBeat();

            for (auto* clip : clips)
            {
                start = std::min (start, clip->getStartBeat());
                end = std::max (end, clip->getEndBeat());
            }

            clipboard.range = { start, end };
        }

        if (juce::isPositiveAndBelow (first, tracks.size()))
            clipboard.track = tracks[first]->itemID;
    }

    // Where Paste puts clips: from the selected track down, else the first selected clip's, else where they came from.
    te::AudioTrack* getPasteTrack (const Clipboard& clipboard, const te::SelectionManager& selection)
    {
        auto* edit = selection.getEdit();

        if (edit == nullptr)
            return nullptr;

        for (auto* track : getSelectedTracks (selection))
            if (auto* audio = dynamic_cast<te::AudioTrack*> (track))
                return audio;

        for (auto* clip : getSelectedClips (selection))
            if (auto* audio = dynamic_cast<te::AudioTrack*> (clip->getTrack()))
                return audio;

        return dynamic_cast<te::AudioTrack*> (te::findTrackForID (*edit, clipboard.track));
    }

    // The clips the piano roll shows, which notes are pasted into.
    juce::Array<te::MidiClip*> getNoteClips (ProjectContext& context)
    {
        juce::Array<te::MidiClip*> clips;

        if (auto* edit = getEdit (context))
            for (const auto id : context.getSongSelection().getNoteClips())
                if (auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (*edit, toTracktion (id))))
                    clips.add (clip);

        return clips;
    }

    void pasteNotes (const Clipboard& clipboard, ProjectContext& context)
    {
        auto* edit = getEdit (context);
        const auto clips = getNoteClips (context);

        if (edit == nullptr || clips.isEmpty())
            return;

        // At the playhead in the clip there, or as far into the first clip as they were into theirs.
        const auto playhead = getPlayhead (*edit);
        auto* target = clips.getFirst();
        auto at = fromTracktion (target->getStartBeat()) + clipboard.intoClip;

        for (auto* clip : clips)
        {
            if (clip->getEditBeatRange().contains (playhead))
            {
                target = clip;
                at = fromTracktion (playhead);
                break;
            }
        }

        auto notes = clipboard.notes;

        for (auto& note : notes)
        {
            note.clip = getClipID (*target);
            note.start = at + (note.start - BeatPosition());
        }

        std::vector<NoteRef> pasted;

        for (const auto& added : replaceNotes (*edit, {}, notes))
            pasted.push_back (getNoteRef (*added.clip, *added.note));

        context.getSongSelection().selectNotes (pasted);
    }

    void pasteClips (const Clipboard& clipboard, ProjectContext& context)
    {
        auto& selection = getSelection (context);
        auto* edit = getEdit (context);
        auto* target = getPasteTrack (clipboard, selection);

        if (edit == nullptr || target == nullptr)
            return;

        const auto tracks = getTracksForClips (*edit);
        const auto first = tracks.indexOf (target);
        const auto distance = getPlayhead (*edit) - clipboard.range.getStart();
        const auto isRange = clipboard.kind == Clipboard::Kind::range;

        // Where each lands (a range's clips cut to it): cleared first, so that no pasted clip cuts another.
        const auto landingOf = [&clipboard, isRange, distance] (const Clipboard::CopiedClip& copied)
        {
            const auto beats = isRange ? copied.beats.getIntersectionWith (clipboard.range) : copied.beats;
            return te::BeatRange (beats.getStart() + distance, beats.getEnd() + distance);
        };

        for (const auto& copied : clipboard.clips)
            if (auto* track = tracks[first + copied.trackOffset])
                clearClipsIn (*track, landingOf (copied));

        // Each a copy, not an alias, as Duplicate makes them: made where it was, cut to the range, and moved to where it lands.
        juce::Array<te::Clip*> pasted;

        for (const auto& copied : clipboard.clips)
        {
            auto* track = tracks[first + copied.trackOffset];

            if (track == nullptr)
                continue;

            auto state = copied.state.createCopy();
            te::EditItemID::remapIDs (state, nullptr, *edit);
            auto* clip = te::insertClipCopy (*track, te::ClipCopy::fromClipboardState (state, false));

            if (clip == nullptr)
                continue;

            clip->setLinkGroupID ({});

            if (isRange)
            {
                if (const auto rest = splitClips ({ clip }, clipboard.range.getStart()); ! rest.isEmpty())
                {
                    clip->removeFromParent(); // what lay before the range
                    clip = rest.getFirst();
                }

                for (auto* after : splitClips ({ clip }, clipboard.range.getEnd()))
                    after->removeFromParent();
            }

            moveClip (*clip, landingOf (copied).getStart(), *track);
            pasted.add (clip);
        }

        // The automation, back onto its own tracks.
        if (target->itemID == clipboard.track)
            pasteAutomation (*edit, clipboard.automation, fromTracktion (distance));

        // What was pasted is selected: a range's becomes the range, as a duplicated range does.
        if (isRange)
        {
            juce::Array<te::Track*> landed;

            for (int i = 0; i < tracks.size(); ++i)
                if (std::any_of (clipboard.clips.begin(), clipboard.clips.end(),
                                 [i, first] (const Clipboard::CopiedClip& copied) { return copied.trackOffset == i - first; }))
                    landed.add (tracks[i]);

            if (landed.isEmpty())
                landed.add (target);

            getRangeSelection (context).setInUndoStep (*edit, clipboard.range + distance, landed);
            return;
        }

        selection.deselectAll();

        for (auto* clip : pasted)
            selection.addToSelection (clip);
    }
} // namespace

bool canCopy (const te::SelectionManager& selection, const RangeSelection& range)
{
    return hasSelectedNotes (selection) || ! range.isEmpty() || hasSelectedClips (selection);
}

void copySelection (Clipboard& clipboard, const te::SelectionManager& selection, const RangeSelection& range)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr || ! canCopy (selection, range))
        return;

    Clipboard copy;

    // Notes, as far apart as they are, and as far into its clip as the earliest is.
    if (const auto notes = getSelectedNotes (selection); ! notes.empty())
    {
        copy.kind = Clipboard::Kind::notes;
        copy.notes = getNoteData (notes);
        auto earliest = copy.notes.front().start;
        auto* earliestClip = notes.front().clip;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (copy.notes[i].start < earliest)
            {
                earliest = copy.notes[i].start;
                earliestClip = notes[i].clip;
            }
        }

        copy.intoClip = earliest - fromTracktion (earliestClip->getStartBeat());

        for (auto& note : copy.notes)
        {
            note.start = BeatPosition() + (note.start - earliest);
            note.clip = {};
            note.id = {};
        }
    }
    // A time range: the clips across it, whole (Paste cuts them to it), and its tracks' automation in it.
    else if (! range.isEmpty())
    {
        copy.kind = Clipboard::Kind::range;
        const auto tracks = range.getTracks();
        juce::Array<te::Clip*> clips;

        for (auto* track : tracks)
            if (auto* clipTrack = dynamic_cast<te::AudioTrack*> (track))
                for (auto* clip : clipTrack->getClips())
                    if (clip->getEditBeatRange().intersects (range.getRange()))
                        clips.add (clip);

        keepClips (copy, *edit, clips, range.getRange());

        // A range without clips pastes from its first track that holds them.
        if (! copy.track.isValid())
            for (auto* track : getTracksForClips (*edit))
                if (tracks.contains (track))
                {
                    copy.track = track->itemID;
                    break;
                }

        std::vector<AutomationStretch> stretches;

        for (auto* track : tracks)
            stretches.push_back ({ track, fromTracktion (range.getRange()) });

        copy.automation = copyAutomation (stretches);
    }
    // Clips, with the automation under each.
    else
    {
        copy.kind = Clipboard::Kind::clips;
        const auto clips = getSelectedClips (selection);
        keepClips (copy, *edit, clips, std::nullopt);
        std::vector<AutomationStretch> stretches;

        for (auto* clip : clips)
            stretches.push_back ({ clip->getTrack(), fromTracktion (clip->getEditBeatRange()) });

        copy.automation = copyAutomation (stretches);
    }

    clipboard = std::move (copy);
}

void cutSelection (Clipboard& clipboard, te::SelectionManager& selection, RangeSelection& range)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr || ! canCopy (selection, range))
        return;

    copySelection (clipboard, selection, range);

    switch (clipboard.kind)
    {
        case Clipboard::Kind::notes:
            deleteNotes (getSelectedNotes (selection));
            break;

        case Clipboard::Kind::range:
            deleteTimeRange (*edit, range.getRange(), range.getTracks(), false);
            break;

        case Clipboard::Kind::clips:
            deleteClips (getSelectedClips (selection), false);
            break;

        case Clipboard::Kind::nothing:
            break;
    }
}

juce::String describeCopy (const te::SelectionManager& selection, const RangeSelection& range, const juce::String& verb)
{
    return canCopy (selection, range) ? describeSelection (selection, range, verb) : verb;
}

bool canPaste (const Clipboard& clipboard, ProjectContext& context)
{
    switch (clipboard.kind)
    {
        case Clipboard::Kind::notes:
            return ! getNoteClips (context).isEmpty();

        case Clipboard::Kind::clips:
        case Clipboard::Kind::range:
            return getPasteTrack (clipboard, getSelection (context)) != nullptr;

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
