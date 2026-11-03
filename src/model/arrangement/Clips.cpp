// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clips.h"
#include "internal/AudioClips.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mirror/internal/Mirror.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace phazite
{
namespace
{
    const auto minimumClipLength = te::BeatDuration::fromBeats (1.0 / 64.0);

    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);

    // How far inside a clip a cut must be, as Tracktion splits clips.
    const auto splitMargin = te::TimeDuration::fromSeconds (0.001);

    juce::UndoManager* getUndo (te::Edit& edit)
    {
        return &getSongUndoManager (edit);
    }

    // The latest a clip can end: where its audio does, for an audio clip that doesn't loop (Tracktion's longest takes no
    // account of what was trimmed off its start), or else as late as Tracktion lets it.
    te::TimePosition getLatestEnd (te::Clip& clip)
    {
        const auto latest = clip.getPosition().getStart() + clip.getMaximumLength();

        if (auto* audio = dynamic_cast<te::AudioClipBase*> (&clip); audio != nullptr && ! audio->isLooping())
            return std::min (latest, getAudioTimeRange (*audio).getEnd());

        return latest;
    }

    // Where clips are on their tracks, for the automation under them (Automation.h: moveAutomation()).
    std::vector<AutomationStretch> getStretchesOf (const juce::Array<te::Clip*>& clips)
    {
        std::vector<AutomationStretch> stretches;

        for (auto* clip : clips)
            stretches.push_back ({ clip->getClipTrack(), { fromTracktion (clip->getStartBeat()), fromTracktion (clip->getEndBeat()) } });

        return stretches;
    }

    std::vector<AutomationStretch> getStretchesOf (const juce::Array<te::Track*>& tracks, te::BeatRange range)
    {
        std::vector<AutomationStretch> stretches;

        for (auto* track : tracks)
            stretches.push_back ({ track, { fromTracktion (range.getStart()), fromTracktion (range.getEnd()) } });

        return stretches;
    }

    //==============================================================================
    // The song's clips' elements.

    juce::ValueTree findIn (const juce::ValueTree& trackState, ClipID id)
    {
        for (const auto& child : trackState)
            if (child.hasType (schema::CLIP) && getClipID (child) == id)
                return child;

        return {};
    }

    // The clips in a group, on the tracks under a parent (the track list, or a folder's element), but one.
    void collectLinked (const juce::ValueTree& parent, const juce::String& link, const juce::ValueTree& except,
                        std::vector<juce::ValueTree>& found)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (schema::TRACK))
                collectLinked (child, link, except, found);
            else if (child.hasType (schema::CLIP) && child != except && child[schema::clipLink].toString() == link)
                found.push_back (child);
        }
    }

    void removeClipState (te::Edit& edit, juce::ValueTree clipState)
    {
        const auto id = getClipID (clipState);
        clipState.getParent().removeChild (clipState, getUndo (edit));
        mirror::reconcileClip (edit, id);
    }

    // How far a move of an audio clip's start or end, from where it plays now to a beat, takes it into its file, in the
    // file's seconds as it plays them.
    double getFileSecondsTo (te::Edit& edit, const juce::ValueTree& clipState, te::TimePosition from, BeatPosition to)
    {
        return (edit.tempoSequence.toTime (toTracktion (to)) - from).inSeconds() / getClipStretch (edit, clipState).timeRatio;
    }

    // Moves a clip's start to a beat, its end staying where it is, and what it plays too.
    void trimStartTo (te::Edit& edit, juce::ValueTree clipState, BeatPosition start)
    {
        if (isAudioState (clipState))
        {
            const auto moved = getFileSecondsTo (edit, clipState, getPlayedPosition (edit, clipState).getStart(), start);
            auto part = getFilePart (clipState);

            if (getClipStretch (edit, clipState).reversed)
                part.end -= moved;
            else
                part.start += moved;

            setAudioPart (edit, clipState, start, part);
            return;
        }

        const auto range = getClipRange (clipState);
        const auto moved = start - range.getStart();
        setClipPlace (edit, clipState, start, range.getLength() - moved, getClipOffset (clipState) + moved);
    }

    void trimEndTo (te::Edit& edit, juce::ValueTree clipState, BeatPosition end)
    {
        const auto range = getClipRange (clipState);

        if (isAudioState (clipState))
        {
            const auto moved = getFileSecondsTo (edit, clipState, getPlayedPosition (edit, clipState).getEnd(), end);
            auto part = getFilePart (clipState);

            if (getClipStretch (edit, clipState).reversed)
                part.start -= moved;
            else
                part.end += moved;

            setAudioPart (edit, clipState, range.getStart(), part);
            return;
        }

        setClipPlace (edit, clipState, range.getStart(), end - range.getStart(), getClipOffset (clipState));
    }

    // Splits a clip of the song into two at a beat, if it is far enough inside it, as Tracktion splits clips: both halves
    // play what they did, and stay in its alias group if it is in one; an audio clip has no fade where it is cut. Returns
    // the right-hand half's element.
    juce::ValueTree splitClipState (te::Edit& edit, juce::ValueTree clipState, BeatPosition beat)
    {
        const auto& tempo = edit.tempoSequence;
        const auto range = getClipRange (edit, clipState);
        const auto time = tempo.toTime (toTracktion (beat));

        if (! tempo.toTime (toTracktion (range)).reduced (splitMargin).contains (time))
            return {};

        auto right = copyClipState (edit, clipState);
        right.setProperty (schema::clipStart, beat.inBeats(), nullptr);

        if (isAudioState (clipState))
        {
            const auto into = getFileSecondsTo (edit, clipState, getPlayedPosition (edit, clipState).getStart(), beat);
            auto left = getFilePart (clipState), rest = left;

            if (getClipStretch (edit, clipState).reversed) // the left half plays the end of the part, backwards
            {
                left.start = left.end - into;
                rest.end = left.start;
            }
            else
            {
                left.end = left.start + into;
                rest.start = left.end;
            }

            right.setProperty (schema::clipFileStart, rest.start, nullptr);
            right.setProperty (schema::clipFileEnd, rest.end, nullptr);
            right.removeProperty (schema::clipFadeIn, nullptr);
            clipState.getParent().appendChild (right, getUndo (edit));
            clipState.removeProperty (schema::clipFadeOut, getUndo (edit));
            setAudioPart (edit, clipState, range.getStart(), left);
        }
        else
        {
            right.setProperty (schema::clipLength, (range.getEnd() - beat).inBeats(), nullptr);
            right.setProperty (schema::clipOffset, (getClipOffset (clipState) + (beat - range.getStart())).inBeats(), nullptr);
            clipState.getParent().appendChild (right, getUndo (edit));
            trimEndTo (edit, clipState, beat);
        }

        mirror::reconcileClip (edit, getClipID (right));
        return right;
    }

    // What lies in a range of a clip of the song goes, as Tracktion's deleteRegionOfClip() deletes it from one of its
    // own: the whole clip, or its start or end, or its middle, splitting it in two.
    void deleteRegionOf (te::Edit& edit, juce::ValueTree clipState, BeatRange region)
    {
        const auto range = getClipRange (edit, clipState);

        if (region.getStart() <= range.getStart() && range.getEnd() <= region.getEnd())
        {
            removeClipState (edit, clipState);
        }
        else if (range.getStart() < region.getStart() && range.getEnd() > region.getEnd())
        {
            if (const auto right = splitClipState (edit, clipState, region.getStart()); right.isValid())
                trimStartTo (edit, right, region.getEnd());
        }
        else if (range.overlaps (region))
        {
            if (region.getEnd() < range.getEnd())
                trimStartTo (edit, clipState, region.getEnd());
            else if (range.getStart() < region.getStart())
                trimEndTo (edit, clipState, region.getStart());
        }
    }

    // Removes a clip from its track: the song's, or else one of Tracktion's alone.
    void removeClip (te::Clip& clip)
    {
        if (const auto clipState = findClipState (clip); clipState.isValid())
            removeClipState (clip.edit, clipState);
        else
            clip.removeFromParent();
    }

    // Moves a clip to start at a beat on its track, keeping what it plays.
    void moveClipStart (te::Clip& clip, BeatPosition start)
    {
        if (auto clipState = findClipState (clip); clipState.isValid())
            moveClipState (clip.edit, clipState, start);
        else
            clip.setStart (clip.edit.tempoSequence.toTime (toTracktion (start)), false, true);
    }

    bool isClipMuted (te::Clip& clip)
    {
        if (const auto clipState = findClipState (clip); clipState.isValid())
            return clipState[schema::clipMute];

        return clip.isMuted();
    }

    void setClipMuted (te::Clip& clip, bool muted)
    {
        if (auto clipState = findClipState (clip); clipState.isValid())
        {
            clipState.setProperty (schema::clipMute, muted, getUndo (clip.edit));
            mirror::reconcileClip (clip.edit, getClipID (clipState));
        }
        else
        {
            clip.setMuted (muted);
        }
    }

    // The clips in a range of a track, as deleteTimeRange() deletes them, without the automation there.
    void deleteClipsIn (te::ClipTrack& track, te::BeatRange range, bool closeGap)
    {
        auto& edit = track.edit;
        const auto time = edit.tempoSequence.toTime (range);

        for (auto* clip : juce::Array<te::Clip*> (track.getClips())) // a copy: deleting changes the list
        {
            if (const auto clipState = findClipState (*clip); clipState.isValid())
            {
                if (getClipRange (edit, clipState).overlaps (fromTracktion (range)))
                    deleteRegionOf (edit, clipState, fromTracktion (range));
            }
            else if (clip->getEditTimeRange().overlaps (time))
            {
                te::deleteRegionOfClip (*clip, time);
            }
        }

        if (closeGap)
            for (auto* clip : juce::Array<te::Clip*> (track.getClips()))
                if (clip->getPosition().getStart() >= time.getEnd())
                    moveClipStart (*clip, fromTracktion (clip->getStartBeat() - range.getLength()));
    }

    //==============================================================================
    // What Tracktion made itself, taken into the song.

    // One of Tracktion's notes, as a note of a song clip's element, with an ID, which Tracktion's note gets too.
    void adoptNote (juce::ValueTree clipState, te::MidiNote& note, juce::int64 id, juce::UndoManager* undo)
    {
        juce::ValueTree noteState (schema::NOTE);
        noteState.setProperty (schema::noteID, id, nullptr);
        noteState.setProperty (schema::notePitch, note.getNoteNumber(), nullptr);
        noteState.setProperty (schema::noteStart, note.getStartBeat().inBeats(), nullptr);
        noteState.setProperty (schema::noteLength, note.getLengthBeats().inBeats(), nullptr);
        noteState.setProperty (schema::noteVelocity, note.getVelocity(), nullptr);

        if (note.isMute())
            noteState.setProperty (schema::noteMute, true, nullptr);

        clipState.appendChild (noteState, undo);
        note.state.setProperty (schema::phaziteNoteID, id, undo);
    }

    // A new note's ID, the song's (makeID()): one no note has had before.
    juce::int64 makeNoteID (te::Edit& edit)
    {
        return (juce::int64) makeID (edit);
    }

    // A copy of the part of a clip that lies in a range, where that part is. The copy is cut at the range's edges as
    // Split cuts clips (an audio clip loses its fades where it is cut); the clip itself stays whole.
    te::Clip* copyPartOfClip (te::Clip& clip, te::BeatRange range)
    {
        const auto copies = copyClips ({ &clip }, false);

        if (copies.isEmpty())
            return nullptr;

        auto* part = copies.getFirst();

        if (const auto rest = splitClips ({ part }, range.getStart()); ! rest.isEmpty())
        {
            removeClip (*part); // what lies before the range
            part = rest.getFirst();
        }

        for (auto* after : splitClips ({ part }, range.getEnd()))
            removeClip (*after);

        return part;
    }
} // namespace

//==============================================================================
juce::ValueTree findClipState (const te::Edit& edit, ClipID id)
{
    if (id.isValid())
        for (const auto& trackState : getTrackStates (edit))
            if (auto found = findIn (trackState, id); found.isValid())
                return found;

    return {};
}

juce::ValueTree findClipState (const te::Clip& clip)
{
    // On its track's element, most likely.
    if (auto* track = clip.getTrack())
        if (auto found = findIn (findTrackState (*track), getClipID (clip)); found.isValid())
            return found;

    return findClipState (clip.edit, getClipID (clip));
}

std::vector<juce::ValueTree> getClipStates (const juce::ValueTree& trackState)
{
    std::vector<juce::ValueTree> clips;

    for (const auto& child : trackState)
        if (child.hasType (schema::CLIP))
            clips.push_back (child);

    return clips;
}

std::vector<juce::ValueTree> getClipStates (const te::Edit& edit)
{
    std::vector<juce::ValueTree> clips;

    for (const auto& trackState : getTrackStates (edit))
        for (const auto& child : trackState)
            if (child.hasType (schema::CLIP))
                clips.push_back (child);

    return clips;
}

ClipID getClipID (const juce::ValueTree& clipState)
{
    return ClipID::fromRaw ((std::uint64_t) (juce::int64) clipState[schema::clipID]);
}

BeatRange getClipRange (const juce::ValueTree& clipState)
{
    const auto start = BeatPosition::fromBeats (clipState[schema::clipStart]);
    return { start, start + BeatDuration::fromBeats (clipState[schema::clipLength]) };
}

BeatRange getClipRange (const te::Edit& edit, const juce::ValueTree& clipState)
{
    return isAudioState (clipState) ? getAudioClipRange (edit, clipState) : getClipRange (clipState);
}

void moveClipState (te::Edit& edit, juce::ValueTree clipState, BeatPosition start)
{
    if (isAudioState (clipState))
        setAudioPart (edit, clipState, start, getFilePart (clipState));
    else
        setClipPlace (edit, clipState, start, getClipRange (clipState).getLength(), getClipOffset (clipState));
}

BeatDuration getClipOffset (const juce::ValueTree& clipState)
{
    return BeatDuration::fromBeats (clipState.getProperty (schema::clipOffset, 0.0));
}

BeatPosition getContentStart (const juce::ValueTree& clipState)
{
    return getClipRange (clipState).getStart() - getClipOffset (clipState);
}

std::vector<juce::ValueTree> getAliasStates (const juce::ValueTree& clipState)
{
    std::vector<juce::ValueTree> aliases { clipState };

    if (const auto link = clipState[schema::clipLink].toString(); link.isNotEmpty())
    {
        // Every clip of the song in the same group: the clip's track's element is in the track list.
        auto list = clipState.getParent();

        while (list.isValid() && ! list.hasType (schema::TRACKS))
            list = list.getParent();

        collectLinked (list, link, clipState, aliases);
    }

    return aliases;
}

juce::ValueTree copyClipState (te::Edit& edit, const juce::ValueTree& clipState)
{
    auto copy = clipState.createCopy();
    copy.setProperty (schema::clipID, (juce::int64) makeID (edit), nullptr);
    return copy;
}

juce::ValueTree makeMidiClipState (te::Edit& edit, const juce::String& name, BeatRange range)
{
    const auto start = std::max (0.0, range.getStart().inBeats());
    juce::ValueTree clipState (schema::CLIP);
    clipState.setProperty (schema::clipID, (juce::int64) makeID (edit), nullptr);
    clipState.setProperty (schema::clipKind, schema::clipIsMidi, nullptr);
    clipState.setProperty (schema::clipName, name, nullptr);
    clipState.setProperty (schema::clipStart, start, nullptr);
    clipState.setProperty (schema::clipLength, std::max (range.getLength().inBeats(), minimumClipLength.inBeats()), nullptr);
    return clipState;
}

te::Clip* placeClipState (te::Edit& edit, juce::ValueTree clipState, te::ClipTrack& track)
{
    auto trackState = findTrackState (track);

    if (! trackState.isValid())
        return nullptr;

    if (clipState.getParent() != trackState)
    {
        if (auto parent = clipState.getParent(); parent.isValid())
            parent.removeChild (clipState, getUndo (edit));

        trackState.appendChild (clipState, getUndo (edit));
    }

    mirror::reconcileClip (edit, getClipID (clipState));
    return te::findClipForID (edit, toTracktion (getClipID (clipState)));
}

void setClipPlace (te::Edit& edit, juce::ValueTree clipState, BeatPosition start, BeatDuration length, BeatDuration offset)
{
    auto* undo = getUndo (edit);
    clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), undo);
    clipState.setProperty (schema::clipLength, std::max (length.inBeats(), minimumClipLength.inBeats()), undo);

    if (offset.inBeats() > 0.0)
        clipState.setProperty (schema::clipOffset, offset.inBeats(), undo);
    else
        clipState.removeProperty (schema::clipOffset, undo);

    mirror::reconcileClip (edit, getClipID (clipState));
}

TracktionControl toTracktionControl (const juce::ValueTree& control)
{
    const auto kind = control[schema::controlKind].toString();
    const auto value = (int) control[schema::controlValue];
    const auto number = (int) control[schema::controlNumber];
    TracktionControl event { te::BeatPosition::fromBeats ((double) control[schema::controlBeat]) };

    if (kind == schema::controlIsPitchWheel)
        event.type = te::MidiControllerEvent::pitchWheelType, event.value = value;
    else if (kind == schema::controlIsChannelPressure)
        event.type = te::MidiControllerEvent::channelPressureType, event.value = value << 7;
    else if (kind == schema::controlIsAftertouch)
        event.type = te::MidiControllerEvent::aftertouchType, event.value = value << 7, event.metadata = number;
    else if (kind == schema::controlIsProgramChange)
        event.type = te::MidiControllerEvent::programChangeType, event.value = value << 7;
    else
        event.type = number, event.value = value << 7;

    return event;
}

juce::ValueTree makeControlState (const te::MidiControllerEvent& event)
{
    juce::ValueTree control (schema::CONTROL);
    const auto type = event.getType();
    auto value = event.getControllerValue() >> 7;
    const char* kind = schema::controlIsController;

    if (type == te::MidiControllerEvent::pitchWheelType)
        kind = schema::controlIsPitchWheel, value = event.getControllerValue();
    else if (type == te::MidiControllerEvent::channelPressureType)
        kind = schema::controlIsChannelPressure;
    else if (type == te::MidiControllerEvent::aftertouchType)
        kind = schema::controlIsAftertouch, control.setProperty (schema::controlNumber, event.getMetadata(), nullptr);
    else if (type == te::MidiControllerEvent::programChangeType)
        kind = schema::controlIsProgramChange;
    else if (juce::isPositiveAndBelow (type, 128))
        control.setProperty (schema::controlNumber, type, nullptr);
    else
        return {};

    control.setProperty (schema::controlKind, kind, nullptr);
    control.setProperty (schema::controlBeat, event.getBeatPosition().inBeats(), nullptr);
    control.setProperty (schema::controlValue, value, nullptr);
    return control;
}

bool haveSameControls (const te::MidiClip& clip, const juce::ValueTree& clipState)
{
    const auto& events = clip.getSequence().getControllerEvents();
    int i = 0;

    for (const auto& child : clipState)
    {
        if (! child.hasType (schema::CONTROL))
            continue;

        const auto control = toTracktionControl (child);
        const auto* event = events[i++];

        if (event == nullptr || event->getBeatPosition() != control.beat || event->getType() != control.type
            || event->getControllerValue() != control.value || event->getMetadata() != control.metadata)
            return false;
    }

    return i == events.size();
}

juce::ValueTree adoptMidiClip (te::MidiClip& clip)
{
    auto& edit = clip.edit;
    auto trackState = clip.getTrack() != nullptr ? findTrackState (*clip.getTrack()) : juce::ValueTree();

    if (! trackState.isValid())
        return {};

    const auto range = fromTracktion (clip.getEditBeatRange());
    auto clipState = makeMidiClipState (edit, clip.getName(), range);
    clipState.setProperty (schema::clipID, (juce::int64) clip.itemID.getRawID(), nullptr);

    if (const auto offset = range.getStart() - fromTracktion (clip.getContentStartBeat()); offset.inBeats() > 0.0)
        clipState.setProperty (schema::clipOffset, offset.inBeats(), nullptr);

    if (clip.isMuted())
        clipState.setProperty (schema::clipMute, true, nullptr);

    trackState.appendChild (clipState, getUndo (edit));

    for (auto* note : juce::Array<te::MidiNote*> (clip.getSequence().getNotes()))
        adoptNote (clipState, *note, makeNoteID (edit), getUndo (edit));

    for (auto* event : clip.getSequence().getControllerEvents())
        if (const auto control = makeControlState (*event); control.isValid())
            clipState.appendChild (control, getUndo (edit));

    mirror::reconcileClip (edit, getClipID (clipState));
    return clipState;
}

//==============================================================================
juce::Array<te::Clip*> getSelectedClips (const Selection& selection)
{
    auto clips = selection.getItemsOfType<te::Clip>();
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return {};

    const auto tracks = getPlaylistTracks (*edit);
    std::stable_sort (clips.begin(), clips.end(),
                      [&tracks] (te::Clip* a, te::Clip* b)
                      {
                          const auto rowA = tracks.indexOf (a->getTrack()), rowB = tracks.indexOf (b->getTrack());
                          return rowA != rowB ? rowA < rowB : a->getPosition().getStart() < b->getPosition().getStart();
                      });
    return clips;
}

bool hasSelectedClips (const Selection& selection)
{
    return ! getSelectedClips (selection).isEmpty();
}

bool hasAliases (const juce::Array<te::Clip*>& clips)
{
    return std::any_of (clips.begin(), clips.end(),
                        [] (te::Clip* clip)
                        {
                            if (const auto clipState = findClipState (*clip); clipState.isValid())
                                return clipState.hasProperty (schema::clipLink);

                            return clip->isLinked();
                        });
}

bool hasAudioClips (const juce::Array<te::Clip*>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return dynamic_cast<te::AudioClipBase*> (clip) != nullptr; });
}

bool hasFadeShape (const juce::Array<te::Clip*>& clips, FadeCurve curve)
{
    auto any = false;

    for (auto* clip : clips)
    {
        if (const auto clipState = findClipState (*clip); isAudioState (clipState))
        {
            if (! (getClipFadeCurve (clipState, true) == curve && getClipFadeCurve (clipState, false) == curve))
                return false;

            any = true;
        }
    }

    return any;
}

std::optional<double> getLinkedPlayBeat (const juce::Array<te::MidiClip*>& shown, double songBeat)
{
    const auto beat = BeatPosition::fromBeats (songBeat);
    std::vector<juce::ValueTree> states;

    for (auto* clip : shown)
        if (const auto clipState = findClipState (*clip); clipState.isValid())
            states.push_back (clipState);

    if (std::any_of (states.begin(), states.end(), [beat] (const juce::ValueTree& s) { return getClipRange (s).contains (beat); }))
        return {};

    // The place in their shared notes, from where those count, in a clip linked to one of these that plays there.
    for (const auto& clipState : states)
        for (const auto& other : getAliasStates (clipState))
            if (other != clipState && getClipRange (other).contains (beat))
                return (getContentStart (clipState) + (beat - getContentStart (other))).inBeats();

    return {};
}

juce::Array<te::Clip*> copyClips (const juce::Array<te::Clip*>& clips, bool asAliases)
{
    juce::Array<te::Clip*> copies;

    for (auto* clip : clips)
    {
        auto clipState = findClipState (*clip);
        auto* track = clip->getClipTrack();

        if (! clipState.isValid() || track == nullptr)
            continue;

        auto& edit = clip->edit;

        // Aliases share an alias group, which the original gets if it is in none; a copy that isn't an alias leaves the
        // original's.
        if (asAliases && ! clipState.hasProperty (schema::clipLink))
            clipState.setProperty (schema::clipLink, (juce::int64) makeID (edit), getUndo (edit));

        auto copy = copyClipState (edit, clipState);

        if (! asAliases)
            copy.removeProperty (schema::clipLink, nullptr);

        if (auto* made = placeClipState (edit, copy, *track))
            copies.add (made);
    }

    return copies;
}

void moveClip (te::Clip& clip, te::BeatPosition start, te::ClipTrack& track)
{
    // To its new start (playing what it did) and track, which Tracktion's clip follows at once.
    if (auto clipState = findClipState (clip); clipState.isValid())
    {
        clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), getUndo (clip.edit));
        placeClipState (clip.edit, clipState, track);
    }
}

void trimClipStart (te::Clip& clip, te::BeatPosition beat)
{
    const auto clipState = findClipState (clip);

    if (isAudioState (clipState))
    {
        // As far back as its audio starts.
        const auto& tempo = clip.edit.tempoSequence;
        const auto position = getPlayedPosition (clip.edit, clipState);
        const auto earliest = std::max (te::TimePosition(), position.getStart() - position.getOffset());
        const auto latest = tempo.toTime (toTracktion (getClipRange (clip.edit, clipState).getEnd()) - minimumClipLength);

        if (latest >= earliest)
            trimStartTo (clip.edit, clipState, fromTracktion (tempo.toBeats (juce::jlimit (earliest, latest, tempo.toTime (beat)))));
    }
    else if (clipState.isValid())
    {
        const auto range = getClipRange (clipState);
        const auto earliest = std::max (0.0, getContentStart (clipState).inBeats());
        const auto latest = range.getEnd().inBeats() - minimumClipLength.inBeats();

        if (latest >= earliest)
            trimStartTo (clip.edit, clipState, BeatPosition::fromBeats (juce::jlimit (earliest, latest, beat.inBeats())));
    }
}

void trimClipEnd (te::Clip& clip, te::BeatPosition beat)
{
    const auto& tempo = clip.edit.tempoSequence;
    const auto clipState = findClipState (clip);

    if (! clipState.isValid())
        return;

    // An audio clip as far as its audio goes on.
    const auto start = getClipRange (clip.edit, clipState).getStart();
    const auto earliest = tempo.toTime (toTracktion (start) + minimumClipLength);
    const auto latest = isAudioState (clipState) ? getLatestEnd (clip) : te::Edit::getMaximumEditEnd();

    if (latest >= earliest)
        trimEndTo (clip.edit, clipState, fromTracktion (tempo.toBeats (juce::jlimit (earliest, latest, tempo.toTime (beat)))));
}

void setClipFade (te::AudioClipBase& clip, bool fadeIn, te::TimeDuration length)
{
    if (const auto clipState = findClipState (clip); isAudioState (clipState))
        setClipFade (clip.edit, clipState, fadeIn,
                     juce::jlimit (te::TimeDuration(), getPlayedPosition (clip.edit, clipState).getLength(), length).inSeconds());
}

void setCrossfade (te::AudioClipBase& first, te::AudioClipBase& second, te::TimePosition boundary, te::TimeDuration length)
{
    const auto firstState = findClipState (first), secondState = findClipState (second);

    if (! isAudioState (firstState) || ! isAudioState (secondState))
        return;

    // How far each can reach over the boundary: the first as far as its audio goes on, the second as far
    // back as its audio starts.
    auto& edit = first.edit;
    const auto secondPosition = getPlayedPosition (edit, secondState);
    const auto firstReach = getLatestEnd (first) - boundary;
    const auto secondReach = boundary - std::max (te::TimePosition(), secondPosition.getStart() - secondPosition.getOffset());
    const auto half = std::max (te::TimeDuration(), std::min ({ length / 2.0, firstReach, secondReach }));

    // Tracktion plays both where they overlap; the fades make that a crossfade.
    const auto& tempo = edit.tempoSequence;
    trimEndTo (edit, firstState, fromTracktion (tempo.toBeats (boundary + half)));
    trimStartTo (edit, secondState, fromTracktion (tempo.toBeats (boundary - half)));
    setClipFade (edit, firstState, false, (half * 2.0).inSeconds());
    setClipFade (edit, secondState, true, (half * 2.0).inSeconds());
}

void setFadeShape (const juce::Array<te::Clip*>& clips, FadeCurve curve)
{
    for (auto* clip : clips)
    {
        if (auto clipState = findClipState (*clip); isAudioState (clipState))
        {
            setClipFadeCurve (clip->edit, clipState, true, curve);
            setClipFadeCurve (clip->edit, clipState, false, curve);
        }
    }
}

void setFadeCurve (te::Clip& clip, bool fadeIn, FadeCurve curve)
{
    if (auto clipState = findClipState (clip); isAudioState (clipState))
    {
        const UndoStep step (clip.edit, "Change Fade Shape");
        setClipFadeCurve (clip.edit, clipState, fadeIn, curve);
    }
}

te::MidiClip* insertMidiClip (te::ClipTrack& track, te::BeatRange range, Selection* selection, const juce::String& name)
{
    const auto clipState = makeMidiClipState (track.edit, name.isNotEmpty() ? name : getTrackName (track), fromTracktion (range));
    auto* clip = dynamic_cast<te::MidiClip*> (placeClipState (track.edit, clipState, track));

    if (clip != nullptr && selection != nullptr)
        selection->selectOnly (clip);

    return clip;
}

te::MidiClip* insertBarLongMidiClip (te::ClipTrack& track, te::BeatPosition start, Selection* selection)
{
    const auto bar = te::BeatDuration::fromBeats (getTimeSignatureAt (track.edit, fromTracktion (start)).numerator);
    return insertMidiClip (track, { start, start + bar }, selection);
}

te::MidiClip* addMidiClipAt (te::ClipTrack& track, te::BeatPosition start, Selection* selection)
{
    const UndoStep step (track.edit, "Add MIDI Clip");
    return insertBarLongMidiClip (track, start, selection);
}

te::Clip* placeClip (te::ClipTrack& track, te::BeatPosition start, te::Clip* lastUsed)
{
    // The clip used last counts while it is still on one of the song's tracks.
    if (lastUsed != nullptr && te::containsClip (track.edit, lastUsed) && dynamic_cast<te::AudioTrack*> (lastUsed->getTrack()) != nullptr)
    {
        const auto copies = copyClips ({ lastUsed }, dynamic_cast<te::MidiClip*> (lastUsed) != nullptr);

        if (copies.isEmpty())
            return nullptr;

        moveClip (*copies.getFirst(), start, track);
        return copies.getFirst();
    }

    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (&track); audioTrack != nullptr && hasInstrument (*audioTrack))
        return insertBarLongMidiClip (track, start, nullptr);

    return nullptr;
}

void slipClip (te::Clip& clip, te::TimeDuration startOffset, te::TimeDuration distance)
{
    // Tracktion keeps the offset at 0 or more, and so does the song.
    const auto offset = std::max (te::TimeDuration(), startOffset - distance);
    const auto clipState = findClipState (clip);

    if (isAudioState (clipState))
    {
        // The same length of its file, from as far into what it plays.
        const auto settings = getClipStretch (clip.edit, clipState);
        const auto from = offset.inSeconds() / settings.timeRatio;
        auto part = getFilePart (clipState);
        const auto length = part.end - part.start;

        if (settings.reversed)
            part.end = getClipFileLength (clip.edit, clipState) - from;
        else
            part.end = from + length;

        part.start = part.end - length;
        setAudioPart (clip.edit, clipState, getClipRange (clipState).getStart(), part);
    }
    else if (clipState.isValid())
    {
        const auto& tempo = clip.edit.tempoSequence;
        const auto range = getClipRange (clipState);
        const auto contentStart = tempo.toBeats (tempo.toTime (toTracktion (range.getStart())) - offset);
        setClipPlace (clip.edit, clipState, range.getStart(), range.getLength(), range.getStart() - fromTracktion (contentStart));
    }
}

//==============================================================================
juce::Array<te::Clip*> splitClips (const juce::Array<te::Clip*>& clips, te::BeatPosition beat)
{
    juce::Array<te::Clip*> parts;

    for (auto* clip : clips)
        if (const auto clipState = findClipState (*clip); clipState.isValid())
            if (const auto right = splitClipState (clip->edit, clipState, fromTracktion (beat)); right.isValid())
                if (auto* part = te::findClipForID (clip->edit, toTracktion (getClipID (right))))
                    parts.add (part);

    return parts;
}

void splitClipsAtEdges (const juce::Array<te::Track*>& tracks, te::BeatRange range)
{
    juce::Array<te::Clip*> clips;

    for (auto* track : tracks)
        if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
            clips.addArray (clipTrack->getClips());

    // The right-hand parts of the first split can reach past the end too.
    const auto parts = splitClips (clips, range.getStart());
    auto all = clips;
    all.addArray (parts);
    splitClips (all, range.getEnd());
}

juce::Array<te::Clip*> duplicateClips (const juce::Array<te::Clip*>& clips, bool asAliases, Selection* selection)
{
    if (clips.isEmpty())
        return {};

    auto start = clips.getFirst()->getStartBeat(), end = clips.getFirst()->getEndBeat();

    for (auto* clip : clips)
    {
        start = std::min (start, clip->getStartBeat());
        end = std::max (end, clip->getEndBeat());
    }

    if (selection != nullptr)
        selection->deselectAll();

    const auto copies = copyClips (clips, asAliases); // each starts where its original is

    for (auto* copy : copies)
    {
        if (auto* track = copy->getClipTrack())
            moveClip (*copy, copy->getStartBeat() + (end - start), *track);

        if (selection != nullptr)
            selection->addToSelection (copy);
    }

    // The automation under the clips is copied with them.
    moveAutomation (getStretchesOf (clips), fromTracktion (end - start), true);
    return copies;
}

void makeClipsUnique (const juce::Array<te::Clip*>& clips)
{
    for (auto* clip : clips)
    {
        auto clipState = findClipState (*clip);

        if (! clipState.hasProperty (schema::clipLink))
            continue;

        const auto aliases = getAliasStates (clipState);
        clipState.removeProperty (schema::clipLink, getUndo (clip->edit));

        // The last one left in the group leaves it too.
        if (auto last = aliases.size() == 2 ? aliases[1] : juce::ValueTree(); last.isValid())
            last.removeProperty (schema::clipLink, getUndo (clip->edit));
    }
}

bool areAllClipsMuted (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (clips, [] (te::Clip* clip) { return isClipMuted (*clip); });
}

void toggleClipsMuted (const juce::Array<te::Clip*>& clips)
{
    toggleAll (clips, [] (te::Clip* clip) { return isClipMuted (*clip); }, [] (te::Clip* clip, bool on) { setClipMuted (*clip, on); });
}

void nudgeClips (const juce::Array<te::Clip*>& clips, te::BeatDuration amount)
{
    // The earliest clip decides how far back they can go. The automation under them goes along.
    for (auto* clip : clips)
        amount = std::max (amount, te::BeatPosition() - clip->getStartBeat());

    const auto under = getStretchesOf (clips);

    for (auto* clip : clips)
        if (auto* track = clip->getClipTrack())
            moveClip (*clip, clip->getStartBeat() + amount, *track);

    moveAutomation (under, fromTracktion (amount), false);
}

void deleteClips (const juce::Array<te::Clip*>& clips, bool closeGap)
{
    // Latest first, so that closing one gap doesn't move a clip still to be deleted.
    auto sorted = clips;
    std::sort (sorted.begin(), sorted.end(),
               [] (te::Clip* a, te::Clip* b) { return a->getPosition().getStart() > b->getPosition().getStart(); });

    for (auto* clip : sorted)
    {
        const auto removed = clip->getEditBeatRange();
        const auto under = getStretchesOf ({ clip });
        auto* track = clip->getClipTrack();
        removeClip (*clip);

        // Closing the gap takes the clip's time out of its track, its automation with it, and what comes after moves
        // back, automation and all; without, the automation stays where it is.
        if (closeGap && track != nullptr)
        {
            for (auto* other : juce::Array<te::Clip*> (track->getClips())) // a copy: moving clips can reorder the list
                if (other->getStartBeat() >= removed.getStart())
                    moveClipStart (*other, fromTracktion (std::max (removed.getStart(), other->getStartBeat() - removed.getLength())));

            deleteAutomation (under, true);
        }
    }
}

void deleteTimeRange (te::Edit&, te::BeatRange range, const juce::Array<te::Track*>& tracks, bool closeGap)
{
    for (auto* track : tracks)
        if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
            deleteClipsIn (*clipTrack, range, closeGap);

    // The automation there goes too, on folders as well, which hold no clips.
    deleteAutomation (getStretchesOf (tracks, range), closeGap);
}

void clearClipsIn (te::ClipTrack& track, te::BeatRange range)
{
    deleteClipsIn (track, range, false);
}

te::BeatRange duplicateTimeRange (te::Edit&, te::BeatRange range, const juce::Array<te::Track*>& tracks)
{
    const auto length = range.getLength();
    const te::BeatRange copyRange (range.getEnd(), range.getEnd() + length);

    if (length <= tinyBeats)
        return copyRange;

    // The part of each clip that lies in the range, copied where it is, before anything moves.
    std::vector<te::Clip::Ptr> parts;

    for (auto* track : tracks)
        if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
            for (auto* clip : juce::Array<te::Clip*> (clipTrack->getClips())) // a copy: the parts join the list
                if (clip->getStartBeat() < range.getEnd() - tinyBeats && clip->getEndBeat() > range.getStart() + tinyBeats)
                    if (auto* part = copyPartOfClip (*clip, range))
                        parts.emplace_back (part);

    // Each part goes a range's length later, in place of what lies there. What is in the way goes first, so that no
    // part cuts another.
    for (const auto& part : parts)
        if (auto* track = part->getClipTrack())
            deleteClipsIn (*track, te::BeatRange (part->getStartBeat() + length, part->getEndBeat() + length), false);

    for (const auto& part : parts)
        if (auto* track = part->getClipTrack())
            moveClip (*part, part->getStartBeat() + length, *track);

    // And the range's automation, on its tracks and folders, in place of what lies there.
    moveAutomation (getStretchesOf (tracks, range), fromTracktion (length), true);
    return copyRange;
}
} // namespace phazite
