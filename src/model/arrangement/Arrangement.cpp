// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Arrangement.h"
#include "internal/Clips.h"
#include "internal/Sections.h"
#include "model/chords/internal/Chords.h"
#include "model/transport/internal/Tempo.h"

namespace phazite
{
namespace
{
    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = te::BeatDuration::fromBeats (1.0e-6);
    const auto tinyTime = te::TimeDuration::fromSeconds (1.0e-6);

    juce::Array<te::AutomatableParameter*> getAutomatedParameters (const te::Edit& edit)
    {
        juce::Array<te::AutomatableParameter*> parameters;

        for (auto* item : te::getAllAutomatableEditItems (edit))
            for (auto* parameter : item->getAutomatableParameters())
                if (parameter->getCurve().getNumPoints() > 0)
                    parameters.add (parameter);

        return parameters;
    }

    void splitContentAt (te::Edit& edit, te::BeatPosition beat)
    {
        for (auto* track : te::getClipTracks (edit))
            splitClips (juce::Array<te::Clip*> (track->getClips()), beat); // a copy: splitting adds clips to the list
    }

    // An automation point, by its position in beats, so that it can be put back elsewhere.
    struct StoredPoint
    {
        te::AutomatableParameter* parameter;
        te::BeatPosition beat;
        float value, curve;
    };

    // Points where the edges of what moves are, at the values the curves have there: one just before each beat
    // and one at it, so that each part keeps its shape at its edges wherever it goes.
    void addEdgePoints (te::Edit& edit, std::initializer_list<te::BeatPosition> beats)
    {
        const auto& tempo = edit.tempoSequence;
        auto& undoManager = edit.getUndoManager();
        const auto justBefore = te::BeatDuration::fromBeats (1.0e-4);

        for (auto* parameter : getAutomatedParameters (edit))
        {
            auto& curve = parameter->getCurve();

            for (const auto beat : beats)
            {
                const auto value = curve.getValueAt (tempo.toTime (beat), parameter->getCurrentValue());

                if (beat > te::BeatPosition() + justBefore)
                    curve.addPoint (tempo.toTime (beat - justBefore), value, 0.0f, &undoManager);

                curve.addPoint (tempo.toTime (beat), value, 0.0f, &undoManager);
            }
        }
    }

    std::vector<StoredPoint> getPointsIn (te::Edit& edit, te::BeatRange range)
    {
        const auto& tempo = edit.tempoSequence;
        std::vector<StoredPoint> points;

        for (auto* parameter : getAutomatedParameters (edit))
            for (const auto& point : parameter->getCurve().getPointsInRegion (tempo.toTime (range)))
                points.push_back ({ parameter, te::toBeats (point.time, tempo), point.value, point.curve });

        return points;
    }

    // Like Tracktion's insertSpaceIntoEditFromBeatRange(): the time some beats take at the tempo where they go.
    te::TimeDuration getLengthInTime (const te::TempoSequence& tempo, te::TimePosition at, te::BeatDuration beats)
    {
        const auto& setting = tempo.getTempoAt (at - te::TimeDuration::fromSeconds (0.0001));
        return te::TimeDuration::fromSeconds (beats.inBeats() * setting.getApproxBeatLength().inSeconds());
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
    const auto timeLength = getLengthInTime (edit.tempoSequence, time, length);

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

    for (auto* parameter : getAutomatedParameters (edit))
        parameter->getCurve().insertSpace (*parameter, time, timeLength);

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

    for (auto* parameter : getAutomatedParameters (edit))
        parameter->getCurve().removeRegionAndCloseGap (*parameter, time, &edit.getUndoManager());

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

    addEdgePoints (edit, { start, end, destination });
    const auto points = getPointsIn (edit, { changedStart, changedEnd });

    for (auto& move : moves)
        if (auto* track = move.clip->getClipTrack())
            moveClip (*move.clip, move.start, *track);

    // Automation the same way: the points in the stretch are put back where their beats go.
    auto& undoManager = edit.getUndoManager();
    const auto& tempo = edit.tempoSequence;

    for (auto* parameter : getAutomatedParameters (edit))
        parameter->getCurve().removePointsInRegion (tempo.toTime (te::BeatRange (changedStart, changedEnd)), &undoManager);

    for (const auto& point : points)
        point.parameter->getCurve().addPoint (tempo.toTime (newPosition (point.beat)), point.value, point.curve, &undoManager);

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

    addEdgePoints (edit, { start, end, destination });
    const auto points = getPointsIn (edit, range);
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

    auto& undoManager = edit.getUndoManager();

    for (const auto& point : points)
        point.parameter->getCurve().addPoint (edit.tempoSequence.toTime (destination + (point.beat - start)), point.value, point.curve,
                                              &undoManager);
}
} // namespace phazite
