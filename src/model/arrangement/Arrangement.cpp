// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Arrangement.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "model/automation/internal/Automation.h"
#include "model/chords/internal/Chords.h"
#include "model/transport/internal/Tempo.h"

namespace phazite
{
namespace
{
    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);
    const auto tinyTime = te::TimeDuration::fromSeconds (1.0e-6);

    void splitContentAt (te::Edit& edit, te::BeatPosition beat)
    {
        for (auto* track : te::getClipTracks (edit))
            splitClips (juce::Array<te::Clip*> (track->getClips()), beat); // a copy: splitting adds clips to the list
    }

    // Moves a clip of the song later on its track (earlier if negative), playing what it did.
    void moveLater (te::Clip& clip, BeatDuration beats)
    {
        if (auto clipState = findClipState (clip); clipState.isValid())
            moveClipState (clip.edit, clipState, getClipRange (clipState).getStart() + beats);
    }
} // namespace

//==============================================================================
void insertTime (te::Edit& edit, te::BeatPosition at, te::BeatDuration length)
{
    if (length <= te::BeatDuration())
        return;

    const auto time = edit.tempoSequence.toTime (at);

    // As Tracktion's insertSpaceIntoEdit() does: in an Edit counted in bars and beats, clips move before the
    // tempo changes after them, since they follow the beat.
    const auto moveTempo = [&edit, at, length] { insertTempoMapTime (edit, fromTracktion (at), fromTracktion (length)); };

    const auto tempoFirst = ! edit.getTimecodeFormat().isBarsBeats();

    if (tempoFirst)
        moveTempo();

    // Split what crosses the time, then move every clip from it on. (Tracktion's ClipTrack::insertSpace()
    // stops at the first clip before the time, which misses clips when a split has left the list unsorted.)
    splitContentAt (edit, at);

    for (auto* track : te::getClipTracks (edit))
        for (auto* clip : juce::Array<te::Clip*> (track->getClips()))
            if (clip->getPosition().getStart() >= time - tinyTime)
                moveLater (*clip, fromTracktion (length));

    insertAutomationTime (edit, fromTracktion (at), fromTracktion (length));
    insertSectionTime (edit, fromTracktion (at), fromTracktion (length));
    insertLaneTime (edit, fromTracktion (at), fromTracktion (length));

    if (! tempoFirst)
        moveTempo();
}

void removeTime (te::Edit& edit, te::BeatRange range)
{
    const auto time = edit.tempoSequence.toTime (range);
    const auto length = time.getLength();

    if (length <= tinyTime)
        return;

    for (auto* track : te::getClipTracks (edit))
    {
        clearClipsIn (*track, range); // trims and splits the clips that reach into it

        // Tiny pieces that rounding leaves behind go too, as Tracktion's deleteRegionOfTracks() does.
        for (auto* clip : juce::Array<te::Clip*> (track->getClips()))
            if (clip->getPosition().getLength() < te::TimeDuration::fromSeconds (0.0001))
                deleteClips ({ clip }, false);

        for (auto* clip : juce::Array<te::Clip*> (track->getClips()))
            if (clip->getPosition().getStart() > time.getCentre())
                moveLater (*clip, BeatDuration::fromBeats (-range.getLength().inBeats()));
    }

    removeAutomationTime (edit, fromTracktion (range));
    removeSectionTime (edit, fromTracktion (range));
    removeLaneTime (edit, fromTracktion (range));

    // Tempo changes last, as Tracktion's deleteRegionOfTracks() does.
    removeTempoMapTime (edit, fromTracktion (range));
}

void moveTime (te::Edit& edit, te::BeatRange range, te::BeatPosition destination)
{
    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();
    const auto later = destination >= end - tinyBeats;

    if (length <= tinyBeats || (destination > start + tinyBeats && destination < end - tinyBeats))
        return; // nothing to move, or a destination inside the range

    if (std::abs ((destination - (later ? end : start)).inBeats()) < tinyBeats.inBeats())
        return; // already there

    // What changes is the stretch from the range to the destination: the range moves to the far end of
    // it, and what lies between moves the other way, by the range's length.
    const auto changedStart = later ? start : destination;
    const auto changedEnd = later ? destination : end;
    const auto rangeShift = later ? destination - end : destination - start;
    const auto otherShift = later ? -length : length;

    const auto isInChanged = [=] (te::BeatPosition beat) { return beat >= changedStart - tinyBeats && beat < changedEnd - tinyBeats; };
    const auto isInRange = [=] (te::BeatPosition beat) { return beat >= start - tinyBeats && beat < end - tinyBeats; };
    const auto newPosition = [=] (te::BeatPosition beat) { return beat + (isInRange (beat) ? rangeShift : otherShift); };

    // Each clip then lies wholly in the range, between it and the destination, or outside both.
    for (const auto beat : { start, end, destination })
        splitContentAt (edit, beat);

    // Where everything goes is worked out before anything moves, so nothing moves twice.
    struct ClipMove
    {
        te::Clip::Ptr clip;
        te::BeatPosition start;
    };

    std::vector<ClipMove> moves;

    for (auto* track : te::getClipTracks (edit))
        for (auto* clip : track->getClips())
            if (const auto beat = clip->getStartBeat(); isInChanged (beat))
                moves.push_back ({ clip, newPosition (beat) });

    // Automation keeps its shape where the edges are; the points in the stretch are put back where their beats go.
    addAutomationEdges (edit, { fromTracktion (start), fromTracktion (end), fromTracktion (destination) });
    const BeatRange changed (fromTracktion (changedStart), fromTracktion (changedEnd));
    auto points = getAutomationPointsIn (edit, changed);

    for (auto& move : moves)
        if (auto* track = move.clip->getClipTrack())
            moveClip (*move.clip, move.start, *track);

    for (auto& [parameter, point] : points)
        point.beat = fromTracktion (newPosition (toTracktion (point.beat)));

    removeAutomationPointsIn (edit, changed);
    addAutomationPoints (edit, points);

    moveSectionTime (edit, fromTracktion (range), fromTracktion (destination));
    moveLaneTime (edit, fromTracktion (range), fromTracktion (destination));
}

void copyTime (te::Edit& edit, te::BeatRange range, te::BeatPosition destination, bool asAliases)
{
    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();

    if (length <= tinyBeats || (destination > start + tinyBeats && destination < end - tinyBeats))
        return;

    splitContentAt (edit, start);
    splitContentAt (edit, end);

    // What lies in the range, by how far into it each thing starts.
    struct Source
    {
        te::Clip::Ptr clip;
        te::BeatDuration offset;
    };

    std::vector<Source> sources;

    for (auto* track : te::getClipTracks (edit))
        for (auto* clip : track->getClips())
            if (clip->getStartBeat() >= start - tinyBeats && clip->getEndBeat() <= end + tinyBeats)
                sources.push_back ({ clip, clip->getStartBeat() - start });

    addAutomationEdges (edit, { fromTracktion (start), fromTracktion (end), fromTracktion (destination) });
    auto points = getAutomationPointsIn (edit, fromTracktion (range));
    const auto sections = copySections (edit, fromTracktion (range));
    const auto chords = copyLaneChords (edit, fromTracktion (range));

    insertTime (edit, destination, length);
    pasteSections (edit, sections, fromTracktion (destination));
    pasteLaneChords (edit, chords, fromTracktion (destination));

    for (const auto& source : sources)
    {
        auto* track = source.clip->getClipTrack();

        if (track == nullptr)
            continue;

        if (const auto copies = copyClips ({ source.clip.get() }, asAliases); ! copies.isEmpty())
            moveClip (*copies.getFirst(), destination + source.offset, *track);
    }

    for (auto& [parameter, point] : points)
        point.beat = fromTracktion (destination) + (point.beat - fromTracktion (start));

    addAutomationPoints (edit, points);
}
} // namespace phazite
