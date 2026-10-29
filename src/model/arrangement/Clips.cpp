// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clips.h"
#include "internal/AudioClips.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"

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

    // The clips in a range of a track, as deleteTimeRange() deletes them, without the automation there.
    void deleteClipsIn (te::ClipTrack& track, te::TimeRange time, bool closeGap)
    {
        for (auto* clip : juce::Array<te::Clip*> (track.getClips())) // a copy: deleting changes the list
            if (clip->getEditTimeRange().overlaps (time))
                te::deleteRegionOfClip (*clip, time);

        if (closeGap)
            for (auto* clip : juce::Array<te::Clip*> (track.getClips()))
                if (clip->getPosition().getStart() >= time.getEnd())
                    clip->setStart (clip->getPosition().getStart() - time.getLength(), false, true);
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
            part->removeFromParent(); // what lies before the range
            part = rest.getFirst();
        }

        for (auto* after : splitClips ({ part }, range.getEnd()))
            after->removeFromParent();

        return part;
    }
} // namespace

juce::Array<te::Clip*> getSelectedClips (const te::SelectionManager& selection)
{
    auto clips = selection.getItemsOfType<te::Clip>();
    auto* edit = selection.getEdit();

    if (edit == nullptr)
        return {};

    // Sections are arranger clips, with edits of their own (Sections.h).
    clips.removeIf ([] (te::Clip* clip) { return dynamic_cast<te::ArrangerClip*> (clip) != nullptr; });

    const auto tracks = getPlaylistTracks (*edit);
    std::stable_sort (clips.begin(), clips.end(),
                      [&tracks] (te::Clip* a, te::Clip* b)
                      {
                          const auto rowA = tracks.indexOf (a->getTrack()), rowB = tracks.indexOf (b->getTrack());
                          return rowA != rowB ? rowA < rowB : a->getPosition().getStart() < b->getPosition().getStart();
                      });
    return clips;
}

bool hasSelectedClips (const te::SelectionManager& selection)
{
    return ! getSelectedClips (selection).isEmpty();
}

bool hasAliases (const juce::Array<te::Clip*>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return clip->isLinked(); });
}

bool hasAudioClips (const juce::Array<te::Clip*>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [] (te::Clip* clip) { return dynamic_cast<te::AudioClipBase*> (clip) != nullptr; });
}

bool hasFadeShape (const juce::Array<te::Clip*>& clips, te::AudioFadeCurve::Type type)
{
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
}

namespace
{
    // The song beat in a clip whose notes play while a clip linked to it plays at a song beat, if one does.
    std::optional<double> getLinkedPlayBeat (const te::Clip& shown, te::BeatPosition beat)
    {
        if (! shown.isLinked())
            return {};

        auto& edit = shown.edit;
        const auto time = edit.tempoSequence.toTime (beat);

        for (auto* other : edit.findClipsInLinkGroup (shown.getLinkGroupID()))
        {
            if (other == &shown || ! other->getEditBeatRange().contains (beat))
                continue;

            // The place in their shared notes: from the start of the notes, and within its loop if it loops.
            auto content = other->getContentBeatAtTime (time).inBeats();

            if (other->isLooping() && other->getLoopLengthBeats() > te::BeatDuration())
            {
                const auto loopStart = other->getLoopStartBeats().inBeats(), loopLength = other->getLoopLengthBeats().inBeats();
                const auto intoLoop = content - loopStart;
                content = loopStart + intoLoop - loopLength * std::floor (intoLoop / loopLength);
            }

            return edit.tempoSequence.toBeats (shown.getTimeOfContentBeat (te::BeatPosition::fromBeats (content))).inBeats();
        }

        return {};
    }
} // namespace

std::optional<double> getLinkedPlayBeat (const juce::Array<te::MidiClip*>& shown, double songBeat)
{
    const auto beat = te::BeatPosition::fromBeats (songBeat);

    if (std::any_of (shown.begin(), shown.end(), [beat] (te::MidiClip* clip) { return clip->getEditBeatRange().contains (beat); }))
        return {};

    for (auto* clip : shown)
        if (const auto linkedBeat = getLinkedPlayBeat (*clip, beat))
            return linkedBeat;

    return {};
}

juce::Array<te::Clip*> copyClips (const juce::Array<te::Clip*>& clips, bool asAliases)
{
    juce::Array<te::Clip*> copies;

    for (auto* clip : clips)
    {
        // Linked clips share a link group; a copy that isn't an alias leaves its original's.
        if (asAliases && ! clip->isLinked())
            clip->setLinkGroupID (juce::Uuid().toString());

        if (auto copy = te::duplicateClip (*clip))
        {
            if (! asAliases)
                copy->setLinkGroupID ({});

            copies.add (copy.get());
        }
    }

    return copies;
}

void moveClip (te::Clip& clip, te::BeatPosition start, te::ClipTrack& track)
{
    const auto time = clip.edit.tempoSequence.toTime (te::BeatPosition::fromBeats (std::max (0.0, start.inBeats())));

    if (clip.getClipTrack() != &track)
        clip.moveTo (track);

    clip.setStart (time, false, true);
}

void trimClipStart (te::Clip& clip, te::BeatPosition beat)
{
    const auto& tempo = clip.edit.tempoSequence;
    const auto earliest = std::max (te::TimePosition(), clip.getPosition().getStartOfSource());
    const auto latest = tempo.toTime (clip.getEndBeat() - minimumClipLength);

    if (latest < earliest)
        return;

    clip.setStart (juce::jlimit (earliest, latest, tempo.toTime (beat)), true, false);
}

void trimClipEnd (te::Clip& clip, te::BeatPosition beat)
{
    const auto& tempo = clip.edit.tempoSequence;
    const auto earliest = tempo.toTime (clip.getStartBeat() + minimumClipLength);
    const auto latest = getLatestEnd (clip);

    if (latest < earliest)
        return;

    clip.setEnd (juce::jlimit (earliest, latest, tempo.toTime (beat)), true);
}

void setClipFade (te::AudioClipBase& clip, bool fadeIn, te::TimeDuration length)
{
    const auto limited = juce::jlimit (te::TimeDuration(), clip.getPosition().getLength(), length);

    if (fadeIn)
        clip.setFadeIn (limited);
    else
        clip.setFadeOut (limited);
}

void setCrossfade (te::AudioClipBase& first, te::AudioClipBase& second, te::TimePosition boundary, te::TimeDuration length)
{
    // How far each can reach over the boundary: the first as far as its audio goes on, the second as far
    // back as its audio starts.
    const auto firstReach = getLatestEnd (first) - boundary;
    const auto secondReach = boundary - std::max (te::TimePosition(), second.getPosition().getStartOfSource());
    const auto half = std::max (te::TimeDuration(), std::min ({ length / 2.0, firstReach, secondReach }));

    // Tracktion plays both where they overlap; the fades make that a crossfade.
    first.setEnd (boundary + half, true);
    second.setStart (boundary - half, true, false);
    first.setFadeOut (half * 2.0);
    second.setFadeIn (half * 2.0);
}

void setFadeShape (const juce::Array<te::Clip*>& clips, te::AudioFadeCurve::Type shape)
{
    for (auto* clip : clips)
    {
        if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
        {
            audio->setFadeInType (shape);
            audio->setFadeOutType (shape);
        }
    }
}

te::MidiClip* insertMidiClip (te::ClipTrack& track, te::BeatRange range, te::SelectionManager* selection)
{
    const auto clip = track.insertMIDIClip (track.getName(), track.edit.tempoSequence.toTime (range), selection);
    return clip.get();
}

te::MidiClip* insertBarLongMidiClip (te::ClipTrack& track, te::BeatPosition start, te::SelectionManager* selection)
{
    const auto bar = te::BeatDuration::fromBeats (track.edit.tempoSequence.getTimeSigAt (start).numerator.get());
    return insertMidiClip (track, { start, start + bar }, selection);
}

te::MidiClip* addMidiClipAt (te::ClipTrack& track, te::BeatPosition start, te::SelectionManager* selection)
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
    clip.setOffset (startOffset - distance); // Tracktion keeps the offset at 0 or more
}

//==============================================================================
juce::Array<te::Clip*> splitClips (const juce::Array<te::Clip*>& clips, te::BeatPosition beat)
{
    juce::Array<te::Clip*> parts;

    for (auto* clip : clips)
    {
        const auto time = clip->edit.tempoSequence.toTime (beat);
        const auto range = clip->getEditTimeRange();

        if (time > range.getStart() && time < range.getEnd())
            if (auto* track = clip->getClipTrack())
                if (auto* part = track->splitClip (*clip, time))
                    parts.add (part);
    }

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

juce::Array<te::Clip*> duplicateClips (const juce::Array<te::Clip*>& clips, bool asAliases, te::SelectionManager* selection)
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
        const auto group = clip->getLinkGroupID();

        if (group.isEmpty())
            continue;

        clip->setLinkGroupID ({});

        if (const auto rest = clip->edit.findClipsInLinkGroup (group); rest.size() == 1)
            rest.getFirst()->setLinkGroupID ({});
    }
}

bool areAllClipsMuted (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (clips, [] (te::Clip* clip) { return clip->isMuted(); });
}

void toggleClipsMuted (const juce::Array<te::Clip*>& clips)
{
    toggleAll (clips, [] (te::Clip* clip) { return clip->isMuted(); }, [] (te::Clip* clip, bool on) { clip->setMuted (on); });
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
        const auto removed = clip->getPosition();
        const auto under = getStretchesOf ({ clip });
        auto* track = clip->getClipTrack();
        clip->removeFromParent();

        // Closing the gap takes the clip's time out of its track, its automation with it, and what comes after moves
        // back, automation and all; without, the automation stays where it is.
        if (closeGap && track != nullptr)
        {
            for (auto* other : juce::Array<te::Clip*> (track->getClips())) // a copy: moving clips can reorder the list
                if (other->getPosition().getStart() >= removed.getStart())
                    other->setStart (std::max (removed.getStart(), other->getPosition().getStart() - removed.getLength()), false, true);

            deleteAutomation (under, true);
        }
    }
}

void deleteTimeRange (te::Edit& edit, te::BeatRange range, const juce::Array<te::Track*>& tracks, bool closeGap)
{
    const auto time = edit.tempoSequence.toTime (range);

    for (auto* track : tracks)
        if (auto* clipTrack = dynamic_cast<te::ClipTrack*> (track))
            deleteClipsIn (*clipTrack, time, closeGap);

    // The automation there goes too, on folders as well, which hold no clips.
    deleteAutomation (getStretchesOf (tracks, range), closeGap);
}

void clearClipsIn (te::ClipTrack& track, te::BeatRange range)
{
    deleteClipsIn (track, track.edit.tempoSequence.toTime (range), false);
}

te::BeatRange duplicateTimeRange (te::Edit& edit, te::BeatRange range, const juce::Array<te::Track*>& tracks)
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
    const auto& tempo = edit.tempoSequence;

    for (const auto& part : parts)
        if (auto* track = part->getClipTrack())
            deleteClipsIn (*track, tempo.toTime (te::BeatRange (part->getStartBeat() + length, part->getEndBeat() + length)), false);

    for (const auto& part : parts)
        if (auto* track = part->getClipTrack())
            moveClip (*part, part->getStartBeat() + length, *track);

    // And the range's automation, on its tracks and folders, in place of what lies there.
    moveAutomation (getStretchesOf (tracks, range), fromTracktion (length), true);
    return copyRange;
}
} // namespace phazite
