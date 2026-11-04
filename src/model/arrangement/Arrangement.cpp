// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Arrangement.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/chords/internal/Chords.h"
#include "model/transport/internal/Tempo.h"

namespace phazite
{
namespace
{
    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = BeatDuration::fromBeats (1.0e-6);

    // Every clip of the song, track by track.
    std::vector<ClipID> getAllClips (const Song& song)
    {
        std::vector<ClipID> clips;

        for (const auto& clipState : getClipStates (song))
            clips.push_back (getClipID (clipState));

        return clips;
    }

    void splitContentAt (Song& song, BeatPosition beat)
    {
        splitClips (song, getAllClips (song), beat);
    }

    // Moves a clip of the song later on its track (earlier if negative), playing what it did.
    void moveLater (Song& song, ClipID clip, BeatDuration beats)
    {
        if (auto clipState = findClipState (song, clip); clipState.isValid())
            moveClipState (song, clipState, getClipRange (clipState).getStart() + beats);
    }
} // namespace

//==============================================================================
void insertTime (Song& song, BeatPosition at, BeatDuration length)
{
    if (length <= BeatDuration())
        return;

    // Split what crosses the time, then move every clip from it on: clips move before the tempo changes after them, since
    // they follow the beat.
    splitContentAt (song, at);

    for (const auto clip : getAllClips (song))
        if (getClipRange (findClipState (song, clip)).getStart() >= at - tinyBeats)
            moveLater (song, clip, length);

    insertAutomationTime (song, at, length);
    insertSectionTime (song, at, length);
    insertLaneTime (song, at, length);
    insertTempoMapTime (song, at, length);
}

void removeTime (Song& song, BeatRange range)
{
    if (range.getLength() <= tinyBeats)
        return;

    for (const auto track : getTrackIDs (song))
    {
        clearClipsIn (song, track, range); // trims and splits the clips that reach into it

        // Tiny pieces that rounding leaves behind go too: those shorter than a tenth of a millisecond.
        for (const auto clip : getClips (song, track))
            if (toSeconds (song, getClipRange (song, clip).getEnd()) - toSeconds (song, getClipRange (song, clip).getStart()) < 0.0001)
                deleteClips (song, { clip }, false);

        for (const auto clip : getClips (song, track))
            if (getClipRange (song, clip).getStart() > range.getCentre())
                moveLater (song, clip, BeatDuration::fromBeats (-range.getLength().inBeats()));
    }

    removeAutomationTime (song, range);
    removeSectionTime (song, range);
    removeLaneTime (song, range);

    // Tempo changes last, after what is placed on them.
    removeTempoMapTime (song, range);
}

void moveTime (Song& song, BeatRange range, BeatPosition destination)
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
    const auto otherShift = later ? BeatDuration() - length : length;

    const auto isInChanged = [=] (BeatPosition beat) { return beat >= changedStart - tinyBeats && beat < changedEnd - tinyBeats; };
    const auto isInRange = [=] (BeatPosition beat) { return beat >= start - tinyBeats && beat < end - tinyBeats; };
    const auto newPosition = [=] (BeatPosition beat) { return beat + (isInRange (beat) ? rangeShift : otherShift); };

    // Each clip then lies wholly in the range, between it and the destination, or outside both.
    for (const auto beat : { start, end, destination })
        splitContentAt (song, beat);

    // Where everything goes is worked out before anything moves, so nothing moves twice.
    struct ClipMove
    {
        ClipID clip;
        TrackID track;
        BeatPosition start;
    };

    std::vector<ClipMove> moves;

    for (const auto& clipState : getClipStates (song))
        if (const auto beat = getClipRange (clipState).getStart(); isInChanged (beat))
            moves.push_back ({ getClipID (clipState), getTrackID (clipState.getParent()), newPosition (beat) });

    // Automation keeps its shape where the edges are; the points in the stretch are put back where their beats go.
    addAutomationEdges (song, { start, end, destination });
    const BeatRange changed (changedStart, changedEnd);
    auto points = getAutomationPointsIn (song, changed);

    for (const auto& move : moves)
        moveClip (song, move.clip, move.start, move.track);

    for (auto& [parameter, point] : points)
        point.beat = newPosition (point.beat);

    removeAutomationPointsIn (song, changed);
    addAutomationPoints (song, points);

    moveSectionTime (song, range, destination);
    moveLaneTime (song, range, destination);
}

void copyTime (Song& song, BeatRange range, BeatPosition destination, bool asAliases)
{
    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();

    if (length <= tinyBeats || (destination > start + tinyBeats && destination < end - tinyBeats))
        return;

    splitContentAt (song, start);
    splitContentAt (song, end);

    // What lies in the range, by how far into it each thing starts.
    struct Source
    {
        ClipID clip;
        TrackID track;
        BeatDuration offset;
    };

    std::vector<Source> sources;

    for (const auto& clipState : getClipStates (song))
        if (const auto clipRange = getClipRange (song, clipState);
            clipRange.getStart() >= start - tinyBeats && clipRange.getEnd() <= end + tinyBeats)
            sources.push_back ({ getClipID (clipState), getTrackID (clipState.getParent()), clipRange.getStart() - start });

    addAutomationEdges (song, { start, end, destination });
    auto points = getAutomationPointsIn (song, range);
    const auto sections = copySections (song, range);
    const auto chords = copyLaneChords (song, range);

    insertTime (song, destination, length);
    pasteSections (song, sections, destination);
    pasteLaneChords (song, chords, destination);

    for (const auto& source : sources)
        if (const auto copies = copyClips (song, { source.clip }, asAliases); ! copies.empty())
            moveClip (song, copies.front(), destination + source.offset, source.track);

    for (auto& [parameter, point] : points)
        point.beat = destination + (point.beat - start);

    addAutomationPoints (song, points);
}
} // namespace phazite
