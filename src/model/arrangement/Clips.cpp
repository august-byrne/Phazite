// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clips.h"
#include "internal/AudioClips.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace phazite
{
namespace
{
    const auto minimumClipLength = BeatDuration::fromBeats (1.0 / 64.0);

    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = BeatDuration::fromBeats (1.0e-6);

    // How far inside a clip a cut must be, in seconds.
    constexpr double splitMargin = 0.001;

    // The latest a clip can end, in the song's seconds: where its audio does, for an audio clip; a MIDI clip has no end.
    double getLatestEnd (const Song& song, const juce::ValueTree& clipState)
    {
        return isAudioState (clipState) ? getAudioFileRange (song, clipState).end : std::numeric_limits<double>::max();
    }

    // Where clips are on their tracks, for the automation under them (Automation.h: moveAutomation()).
    std::vector<AutomationStretch> getStretchesOf (const Song& song, const std::vector<ClipID>& clips)
    {
        std::vector<AutomationStretch> stretches;

        for (const auto clip : clips)
            if (const auto clipState = findClipState (song, clip); clipState.isValid())
                stretches.push_back ({ getTrackID (clipState.getParent()), getClipRange (song, clipState) });

        return stretches;
    }

    std::vector<AutomationStretch> getStretchesOf (const std::vector<TrackID>& tracks, BeatRange range)
    {
        std::vector<AutomationStretch> stretches;

        for (const auto track : tracks)
            stretches.push_back ({ track, range });

        return stretches;
    }

    //==============================================================================
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

    void removeClipState (Song& song, juce::ValueTree clipState)
    {
        clipState.getParent().removeChild (clipState, &song.getUndoManager());
    }

    // How far a move of an audio clip's start or end, from where it plays now (in the song's seconds) to a beat, takes it
    // into its file, in the file's seconds as it plays them.
    double getFileSecondsTo (const Song& song, const juce::ValueTree& clipState, double from, BeatPosition to)
    {
        return (toSeconds (song, to) - from) / getClipStretch (song, clipState).timeRatio;
    }

    // Moves a clip's start to a beat, its end staying where it is, and what it plays too.
    void trimStartTo (Song& song, juce::ValueTree clipState, BeatPosition start)
    {
        if (isAudioState (clipState))
        {
            const auto moved = getFileSecondsTo (song, clipState, getPlayedPosition (song, clipState).start, start);
            auto part = getFilePart (clipState);

            if (getClipStretch (song, clipState).reversed)
                part.end -= moved;
            else
                part.start += moved;

            setAudioPart (song, clipState, start, part);
            return;
        }

        const auto range = getClipRange (clipState);
        const auto moved = start - range.getStart();
        setClipPlace (song, clipState, start, range.getLength() - moved, getClipOffset (clipState) + moved);
    }

    void trimEndTo (Song& song, juce::ValueTree clipState, BeatPosition end)
    {
        const auto range = getClipRange (clipState);

        if (isAudioState (clipState))
        {
            const auto moved = getFileSecondsTo (song, clipState, getPlayedPosition (song, clipState).end, end);
            auto part = getFilePart (clipState);

            if (getClipStretch (song, clipState).reversed)
                part.start -= moved;
            else
                part.end += moved;

            setAudioPart (song, clipState, range.getStart(), part);
            return;
        }

        setClipPlace (song, clipState, range.getStart(), end - range.getStart(), getClipOffset (clipState));
    }

    // Splits a clip of the song into two at a beat, if it is far enough inside it: both halves play what they did, and stay
    // in its alias group if it is in one; an audio clip has no fade where it is cut. Returns the right-hand half's element.
    juce::ValueTree splitClipState (Song& song, juce::ValueTree clipState, BeatPosition beat)
    {
        const auto range = getClipRange (song, clipState);
        const auto time = toSeconds (song, beat);

        if (time <= toSeconds (song, range.getStart()) + splitMargin || time >= toSeconds (song, range.getEnd()) - splitMargin)
            return {};

        auto right = copyClipState (song, clipState);
        right.setProperty (schema::clipStart, beat.inBeats(), nullptr);
        auto* undo = &song.getUndoManager();

        if (isAudioState (clipState))
        {
            const auto into = getFileSecondsTo (song, clipState, getPlayedPosition (song, clipState).start, beat);
            auto left = getFilePart (clipState), rest = left;

            if (getClipStretch (song, clipState).reversed) // the left half plays the end of the part, backwards
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
            clipState.getParent().appendChild (right, undo);
            clipState.removeProperty (schema::clipFadeOut, undo);
            setAudioPart (song, clipState, range.getStart(), left);
        }
        else
        {
            right.setProperty (schema::clipLength, (range.getEnd() - beat).inBeats(), nullptr);
            right.setProperty (schema::clipOffset, (getClipOffset (clipState) + (beat - range.getStart())).inBeats(), nullptr);
            clipState.getParent().appendChild (right, undo);
            trimEndTo (song, clipState, beat);
        }

        return right;
    }

    // What lies in a range of a clip goes: the whole clip, or its start or end, or its middle, splitting it in two.
    void deleteRegionOf (Song& song, juce::ValueTree clipState, BeatRange region)
    {
        const auto range = getClipRange (song, clipState);

        if (region.getStart() <= range.getStart() && range.getEnd() <= region.getEnd())
        {
            removeClipState (song, clipState);
        }
        else if (range.getStart() < region.getStart() && range.getEnd() > region.getEnd())
        {
            if (const auto right = splitClipState (song, clipState, region.getStart()); right.isValid())
                trimStartTo (song, right, region.getEnd());
        }
        else if (range.overlaps (region))
        {
            if (region.getEnd() < range.getEnd())
                trimStartTo (song, clipState, region.getEnd());
            else if (range.getStart() < region.getStart())
                trimEndTo (song, clipState, region.getStart());
        }
    }

    // The clips in a range of a track, as deleteTimeRange() deletes them, without the automation there.
    void deleteClipsIn (Song& song, TrackID track, BeatRange range, bool closeGap)
    {
        for (const auto& clipState : getClipStates (findTrackState (song, track))) // a copy: deleting changes the element
            if (getClipRange (song, clipState).overlaps (range))
                deleteRegionOf (song, clipState, range);

        if (closeGap)
            for (const auto& clipState : getClipStates (findTrackState (song, track)))
                if (const auto start = getClipRange (clipState).getStart(); start >= range.getEnd())
                    moveClipState (song, clipState, start - range.getLength());
    }

    // A copy of the part of a clip that lies in a range, where that part is. The copy is cut at the range's edges as
    // Split cuts clips (an audio clip loses its fades where it is cut); the clip itself stays whole.
    ClipID copyPartOfClip (Song& song, ClipID clip, BeatRange range)
    {
        const auto copies = copyClips (song, { clip }, false);

        if (copies.empty())
            return {};

        auto part = copies.front();

        if (const auto rest = splitClips (song, { part }, range.getStart()); ! rest.empty())
        {
            removeClipState (song, findClipState (song, part)); // what lies before the range
            part = rest.front();
        }

        for (const auto after : splitClips (song, { part }, range.getEnd()))
            removeClipState (song, findClipState (song, after));

        return part;
    }
} // namespace

//==============================================================================
juce::ValueTree findClipState (const Song& song, ClipID id)
{
    if (id.isValid())
        for (const auto& trackState : getTrackStates (song))
            if (auto found = findIn (trackState, id); found.isValid())
                return found;

    return {};
}

std::vector<juce::ValueTree> getClipStates (const juce::ValueTree& trackState)
{
    std::vector<juce::ValueTree> clips;

    for (const auto& child : trackState)
        if (child.hasType (schema::CLIP))
            clips.push_back (child);

    return clips;
}

std::vector<juce::ValueTree> getClipStates (const Song& song)
{
    std::vector<juce::ValueTree> clips;

    for (const auto& trackState : getTrackStates (song))
        for (const auto& child : trackState)
            if (child.hasType (schema::CLIP))
                clips.push_back (child);

    return clips;
}

std::vector<ClipID> getClips (const Song& song, TrackID track)
{
    std::vector<ClipID> clips;

    for (const auto& clipState : getClipStates (findTrackState (song, track)))
        clips.push_back (getClipID (clipState));

    return clips;
}

TrackID getClipTrack (const Song& song, ClipID clip)
{
    if (const auto clipState = findClipState (song, clip); clipState.isValid())
        return getTrackID (clipState.getParent());

    return {};
}

ClipID getClipID (const juce::ValueTree& clipState)
{
    return ClipID::fromRaw ((std::uint64_t) (juce::int64) clipState[schema::clipID]);
}

bool isMidiState (const juce::ValueTree& clipState)
{
    return clipState.hasType (schema::CLIP) && ! isAudioState (clipState);
}

BeatRange getClipRange (const juce::ValueTree& clipState)
{
    const auto start = BeatPosition::fromBeats (clipState[schema::clipStart]);
    return { start, start + BeatDuration::fromBeats (clipState[schema::clipLength]) };
}

BeatRange getClipRange (const Song& song, const juce::ValueTree& clipState)
{
    return isAudioState (clipState) ? getAudioClipRange (song, clipState) : getClipRange (clipState);
}

BeatRange getClipRange (const Song& song, ClipID clip)
{
    return getClipRange (song, findClipState (song, clip));
}

BeatPosition getClipsEnd (const Song& song)
{
    BeatPosition end;

    for (const auto& clipState : getClipStates (song))
        end = std::max (end, getClipRange (song, clipState).getEnd());

    return end;
}

void moveClipState (Song& song, juce::ValueTree clipState, BeatPosition start)
{
    if (isAudioState (clipState))
        setAudioPart (song, clipState, start, getFilePart (clipState));
    else
        setClipPlace (song, clipState, start, getClipRange (clipState).getLength(), getClipOffset (clipState));
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

juce::ValueTree copyClipState (Song& song, const juce::ValueTree& clipState)
{
    auto copy = clipState.createCopy();
    copy.setProperty (schema::clipID, (juce::int64) song.makeID(), nullptr);
    return copy;
}

juce::ValueTree makeMidiClipState (Song& song, const juce::String& name, BeatRange range)
{
    const auto start = std::max (0.0, range.getStart().inBeats());
    juce::ValueTree clipState (schema::CLIP);
    clipState.setProperty (schema::clipID, (juce::int64) song.makeID(), nullptr);
    clipState.setProperty (schema::clipKind, schema::clipIsMidi, nullptr);
    clipState.setProperty (schema::clipName, name, nullptr);
    clipState.setProperty (schema::clipStart, start, nullptr);
    clipState.setProperty (schema::clipLength, std::max (range.getLength().inBeats(), minimumClipLength.inBeats()), nullptr);
    return clipState;
}

ClipID placeClipState (Song& song, juce::ValueTree clipState, TrackID track)
{
    auto trackState = findTrackState (song, track);

    if (! trackState.hasType (schema::TRACK))
        return {};

    if (clipState.getParent() != trackState)
    {
        if (auto parent = clipState.getParent(); parent.isValid())
            parent.removeChild (clipState, &song.getUndoManager());

        trackState.appendChild (clipState, &song.getUndoManager());
    }

    return getClipID (clipState);
}

void setClipPlace (Song& song, juce::ValueTree clipState, BeatPosition start, BeatDuration length, BeatDuration offset)
{
    auto* undo = &song.getUndoManager();
    clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), undo);
    clipState.setProperty (schema::clipLength, std::max (length.inBeats(), minimumClipLength.inBeats()), undo);

    if (offset.inBeats() > 0.0)
        clipState.setProperty (schema::clipOffset, offset.inBeats(), undo);
    else
        clipState.removeProperty (schema::clipOffset, undo);
}

//==============================================================================
std::vector<ClipID> getSelectedClips (const Selection& selection)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return {};

    // Top to bottom by their tracks, and on a track from the earliest.
    struct Placed
    {
        ClipID id;
        size_t row;
        BeatPosition start;
    };

    const auto tracks = getTrackIDs (*song);
    std::vector<Placed> placed;

    for (const auto id : selection.getClips())
    {
        const auto clipState = findClipState (*song, id);
        const auto row = (size_t) (std::find (tracks.begin(), tracks.end(), getTrackID (clipState.getParent())) - tracks.begin());
        placed.push_back ({ id, row, getClipRange (clipState).getStart() });
    }

    std::stable_sort (placed.begin(), placed.end(),
                      [] (const Placed& a, const Placed& b) { return a.row != b.row ? a.row < b.row : a.start < b.start; });

    std::vector<ClipID> ids;

    for (const auto& clip : placed)
        ids.push_back (clip.id);

    return ids;
}

bool hasSelectedClips (const Selection& selection)
{
    return ! getSelectedClips (selection).empty();
}

bool hasAliases (const Song& song, const std::vector<ClipID>& clips)
{
    return std::any_of (clips.begin(), clips.end(),
                        [&song] (ClipID id) { return findClipState (song, id).hasProperty (schema::clipLink); });
}

bool hasAudioClips (const Song& song, const std::vector<ClipID>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [&song] (ClipID id) { return isAudioState (findClipState (song, id)); });
}

bool hasFadeShape (const Song& song, const std::vector<ClipID>& clips, FadeCurve curve)
{
    auto any = false;

    for (const auto id : clips)
    {
        if (const auto clipState = findClipState (song, id); isAudioState (clipState))
        {
            if (! (getClipFadeCurve (clipState, true) == curve && getClipFadeCurve (clipState, false) == curve))
                return false;

            any = true;
        }
    }

    return any;
}

std::optional<double> getLinkedPlayBeat (const Song& song, const std::vector<ClipID>& shown, double songBeat)
{
    const auto beat = BeatPosition::fromBeats (songBeat);
    std::vector<juce::ValueTree> states;

    for (const auto clip : shown)
        if (const auto clipState = findClipState (song, clip); clipState.isValid())
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

std::vector<ClipID> copyClips (Song& song, const std::vector<ClipID>& clips, bool asAliases)
{
    std::vector<ClipID> copies;

    for (const auto clip : clips)
    {
        auto clipState = findClipState (song, clip);

        if (! clipState.isValid())
            continue;

        // Aliases share an alias group, which the original gets if it is in none; a copy that isn't an alias leaves the
        // original's.
        if (asAliases && ! clipState.hasProperty (schema::clipLink))
            clipState.setProperty (schema::clipLink, (juce::int64) song.makeID(), &song.getUndoManager());

        auto copy = copyClipState (song, clipState);

        if (! asAliases)
            copy.removeProperty (schema::clipLink, nullptr);

        if (const auto made = placeClipState (song, copy, getTrackID (clipState.getParent())); made.isValid())
            copies.push_back (made);
    }

    return copies;
}

void moveClip (Song& song, ClipID clip, BeatPosition start, TrackID track)
{
    // To its new start (playing what it did) and track.
    if (auto clipState = findClipState (song, clip); clipState.isValid())
    {
        clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), &song.getUndoManager());
        placeClipState (song, clipState, track);
    }
}

void trimClipStart (Song& song, ClipID clip, BeatPosition beat)
{
    const auto clipState = findClipState (song, clip);

    if (isAudioState (clipState))
    {
        // As far back as its audio starts.
        const auto earliest = std::max (0.0, getAudioFileRange (song, clipState).start);
        const auto latest = toSeconds (song, getClipRange (song, clipState).getEnd() - minimumClipLength);

        if (latest >= earliest)
            trimStartTo (song, clipState, toBeats (song, juce::jlimit (earliest, latest, toSeconds (song, beat))));
    }
    else if (clipState.isValid())
    {
        const auto range = getClipRange (clipState);
        const auto earliest = std::max (0.0, getContentStart (clipState).inBeats());
        const auto latest = range.getEnd().inBeats() - minimumClipLength.inBeats();

        if (latest >= earliest)
            trimStartTo (song, clipState, BeatPosition::fromBeats (juce::jlimit (earliest, latest, beat.inBeats())));
    }
}

void trimClipEnd (Song& song, ClipID clip, BeatPosition beat)
{
    const auto clipState = findClipState (song, clip);

    if (! clipState.isValid())
        return;

    // An audio clip as far as its audio goes on.
    const auto start = getClipRange (song, clipState).getStart();
    const auto earliest = toSeconds (song, start + minimumClipLength);
    const auto latest = getLatestEnd (song, clipState);

    if (latest >= earliest)
        trimEndTo (song, clipState, toBeats (song, juce::jlimit (earliest, latest, toSeconds (song, beat))));
}

void setClipFade (Song& song, ClipID clip, bool fadeIn, double seconds)
{
    if (const auto clipState = findClipState (song, clip); isAudioState (clipState))
    {
        const auto position = getPlayedPosition (song, clipState);
        setClipFade (song, clipState, fadeIn, juce::jlimit (0.0, position.end - position.start, seconds));
    }
}

void setCrossfade (Song& song, ClipID first, ClipID second, double boundary, double length)
{
    const auto firstState = findClipState (song, first), secondState = findClipState (song, second);

    if (! isAudioState (firstState) || ! isAudioState (secondState))
        return;

    // How far each can reach over the boundary: the first as far as its audio goes on, the second as far
    // back as its audio starts.
    const auto firstReach = getLatestEnd (song, firstState) - boundary;
    const auto secondReach = boundary - std::max (0.0, getAudioFileRange (song, secondState).start);
    const auto half = std::max (0.0, std::min ({ length / 2.0, firstReach, secondReach }));

    // Both play where they overlap; the fades make that a crossfade.
    trimEndTo (song, firstState, toBeats (song, boundary + half));
    trimStartTo (song, secondState, toBeats (song, boundary - half));
    setClipFade (song, firstState, false, half * 2.0);
    setClipFade (song, secondState, true, half * 2.0);
}

void setFadeShape (Song& song, const std::vector<ClipID>& clips, FadeCurve curve)
{
    for (const auto clip : clips)
    {
        if (auto clipState = findClipState (song, clip); isAudioState (clipState))
        {
            setClipFadeCurve (song, clipState, true, curve);
            setClipFadeCurve (song, clipState, false, curve);
        }
    }
}

void setFadeCurve (Song& song, ClipID clip, bool fadeIn, FadeCurve curve)
{
    if (auto clipState = findClipState (song, clip); isAudioState (clipState))
    {
        const UndoStep step (song, "Change Fade Shape");
        setClipFadeCurve (song, clipState, fadeIn, curve);
    }
}

ClipID insertMidiClip (Song& song, TrackID track, BeatRange range, Selection* selection, const juce::String& name)
{
    const auto clipState = makeMidiClipState (song, name.isNotEmpty() ? name : getTrackName (song, track), range);
    const auto clip = placeClipState (song, clipState, track);

    if (clip.isValid() && selection != nullptr)
        selection->selectOnly (clip);

    return clip;
}

ClipID insertBarLongMidiClip (Song& song, TrackID track, BeatPosition start, Selection* selection)
{
    const auto bar = BeatDuration::fromBeats (getBars (song).getBeatsPerBar (start));
    return insertMidiClip (song, track, { start, start + bar }, selection);
}

ClipID addMidiClipAt (Song& song, TrackID track, BeatPosition start, Selection* selection)
{
    const UndoStep step (song, "Add MIDI Clip");
    return insertBarLongMidiClip (song, track, start, selection);
}

ClipID placeClip (Song& song, TrackID track, BeatPosition start, ClipID lastUsed)
{
    // The clip used last counts while it is still on one of the song's tracks.
    if (const auto used = findClipState (song, lastUsed); used.isValid())
    {
        const auto copies = copyClips (song, { lastUsed }, isMidiState (used));

        if (copies.empty())
            return {};

        moveClip (song, copies.front(), start, track);
        return copies.front();
    }

    if (findTrackState (song, track).hasType (schema::TRACK) && hasInstrument (song, track))
        return insertBarLongMidiClip (song, track, start, nullptr);

    return {};
}

double getClipStartOffset (const Song& song, ClipID clip)
{
    const auto clipState = findClipState (song, clip);

    if (isAudioState (clipState))
        return getPlayedPosition (song, clipState).offset;

    return toSeconds (song, getClipRange (clipState).getStart()) - toSeconds (song, getContentStart (clipState));
}

void slipClip (Song& song, ClipID clip, double startOffset, double distance)
{
    // The offset stays at 0 or more.
    const auto offset = std::max (0.0, startOffset - distance);
    const auto clipState = findClipState (song, clip);

    if (isAudioState (clipState))
    {
        // The same length of its file, from as far into what it plays.
        const auto settings = getClipStretch (song, clipState);
        const auto from = offset / settings.timeRatio;
        auto part = getFilePart (clipState);
        const auto length = part.end - part.start;

        if (settings.reversed)
            part.end = getClipFileLength (song, clipState) - from;
        else
            part.end = from + length;

        part.start = part.end - length;
        setAudioPart (song, clipState, getClipRange (clipState).getStart(), part);
    }
    else if (clipState.isValid())
    {
        const auto range = getClipRange (clipState);
        const auto contentStart = toBeats (song, toSeconds (song, range.getStart()) - offset);
        setClipPlace (song, clipState, range.getStart(), range.getLength(), range.getStart() - contentStart);
    }
}

//==============================================================================
std::vector<ClipID> splitClips (Song& song, const std::vector<ClipID>& clips, BeatPosition beat)
{
    std::vector<ClipID> parts;

    for (const auto clip : clips)
        if (const auto clipState = findClipState (song, clip); clipState.isValid())
            if (const auto right = splitClipState (song, clipState, beat); right.isValid())
                parts.push_back (getClipID (right));

    return parts;
}

void splitClipsAtEdges (Song& song, const std::vector<TrackID>& tracks, BeatRange range)
{
    std::vector<ClipID> clips;

    for (const auto track : tracks)
        for (const auto clip : getClips (song, track))
            clips.push_back (clip);

    // The right-hand parts of the first split can reach past the end too.
    const auto parts = splitClips (song, clips, range.getStart());
    auto all = clips;
    all.insert (all.end(), parts.begin(), parts.end());
    splitClips (song, all, range.getEnd());
}

std::vector<ClipID> duplicateClips (Song& song, const std::vector<ClipID>& clips, bool asAliases, Selection* selection)
{
    std::optional<BeatRange> span;

    for (const auto clip : clips)
        if (const auto clipState = findClipState (song, clip); clipState.isValid())
            span = span.has_value() ? span->getUnionWith (getClipRange (song, clipState)) : getClipRange (song, clipState);

    if (! span.has_value())
        return {};

    if (selection != nullptr)
        selection->deselectAll();

    const auto copies = copyClips (song, clips, asAliases); // each starts where its original is

    for (const auto copy : copies)
    {
        const auto clipState = findClipState (song, copy);
        moveClip (song, copy, getClipRange (clipState).getStart() + span->getLength(), getTrackID (clipState.getParent()));

        if (selection != nullptr)
            selection->addToSelection (copy);
    }

    // The automation under the clips is copied with them.
    moveAutomation (song, getStretchesOf (song, clips), span->getLength(), true);
    return copies;
}

void makeClipsUnique (Song& song, const std::vector<ClipID>& clips)
{
    for (const auto id : clips)
    {
        auto clipState = findClipState (song, id);

        if (! clipState.hasProperty (schema::clipLink))
            continue;

        const auto aliases = getAliasStates (clipState);
        clipState.removeProperty (schema::clipLink, &song.getUndoManager());

        // The last one left in the group leaves it too.
        if (auto last = aliases.size() == 2 ? aliases[1] : juce::ValueTree(); last.isValid())
            last.removeProperty (schema::clipLink, &song.getUndoManager());
    }
}

bool areAllClipsMuted (const Song& song, const std::vector<ClipID>& clips)
{
    return areAllOn (clips, [&song] (ClipID id) { return (bool) findClipState (song, id)[schema::clipMute]; });
}

void toggleClipsMuted (Song& song, const std::vector<ClipID>& clips)
{
    toggleAll (
        clips, [&song] (ClipID id) { return (bool) findClipState (song, id)[schema::clipMute]; },
        [&song] (ClipID id, bool on)
        {
            if (auto clipState = findClipState (song, id); clipState.isValid())
            {
                if (on) // off is no property at all, as it is at first
                    clipState.setProperty (schema::clipMute, true, &song.getUndoManager());
                else
                    clipState.removeProperty (schema::clipMute, &song.getUndoManager());
            }
        });
}

void nudgeClips (Song& song, const std::vector<ClipID>& clips, BeatDuration amount)
{
    // The earliest clip decides how far back they can go. The automation under them goes along.
    for (const auto clip : clips)
        if (const auto clipState = findClipState (song, clip); clipState.isValid())
            amount = std::max (amount, BeatPosition() - getClipRange (clipState).getStart());

    const auto under = getStretchesOf (song, clips);

    for (const auto clip : clips)
        if (const auto clipState = findClipState (song, clip); clipState.isValid())
            moveClip (song, clip, getClipRange (clipState).getStart() + amount, getTrackID (clipState.getParent()));

    moveAutomation (song, under, amount, false);
}

void deleteClips (Song& song, const std::vector<ClipID>& clips, bool closeGap)
{
    // Latest first, so that closing one gap doesn't move a clip still to be deleted.
    std::vector<juce::ValueTree> sorted;

    for (const auto clip : clips)
        if (const auto clipState = findClipState (song, clip); clipState.isValid())
            sorted.push_back (clipState);

    std::sort (sorted.begin(), sorted.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
               { return getClipRange (a).getStart() > getClipRange (b).getStart(); });

    for (const auto& clipState : sorted)
    {
        const auto removed = getClipRange (song, clipState);
        const auto track = getTrackID (clipState.getParent());
        const auto under = getStretchesOf (song, { getClipID (clipState) });
        removeClipState (song, clipState);

        // Closing the gap takes the clip's time out of its track, its automation with it, and what comes after moves
        // back, automation and all; without, the automation stays where it is.
        if (closeGap)
        {
            for (const auto& other : getClipStates (findTrackState (song, track)))
                if (const auto start = getClipRange (other).getStart(); start >= removed.getStart())
                    moveClipState (song, other, std::max (removed.getStart(), start - removed.getLength()));

            deleteAutomation (song, under, true);
        }
    }
}

void deleteTimeRange (Song& song, BeatRange range, const std::vector<TrackID>& tracks, bool closeGap)
{
    for (const auto track : tracks)
        deleteClipsIn (song, track, range, closeGap);

    // The automation there goes too, on folders as well, which hold no clips.
    deleteAutomation (song, getStretchesOf (tracks, range), closeGap);
}

void clearClipsIn (Song& song, TrackID track, BeatRange range)
{
    deleteClipsIn (song, track, range, false);
}

BeatRange duplicateTimeRange (Song& song, BeatRange range, const std::vector<TrackID>& tracks)
{
    const auto length = range.getLength();
    const BeatRange copyRange (range.getEnd(), range.getEnd() + length);

    if (length <= tinyBeats)
        return copyRange;

    // The part of each clip that lies in the range, copied where it is, before anything moves.
    std::vector<ClipID> parts;

    for (const auto track : tracks)
        for (const auto& clipState : getClipStates (findTrackState (song, track))) // a copy: the parts join the track
            if (const auto clipRange = getClipRange (song, clipState);
                clipRange.getStart() < range.getEnd() - tinyBeats && clipRange.getEnd() > range.getStart() + tinyBeats)
                if (const auto part = copyPartOfClip (song, getClipID (clipState), range); part.isValid())
                    parts.push_back (part);

    // Each part goes a range's length later, in place of what lies there. What is in the way goes first, so that no
    // part cuts another.
    for (const auto part : parts)
        if (const auto partRange = getClipRange (song, part); findClipState (song, part).isValid())
            deleteClipsIn (song, getClipTrack (song, part), { partRange.getStart() + length, partRange.getEnd() + length }, false);

    for (const auto part : parts)
        if (const auto clipState = findClipState (song, part); clipState.isValid())
            moveClip (song, part, getClipRange (clipState).getStart() + length, getTrackID (clipState.getParent()));

    // And the range's automation, on its tracks and folders, in place of what lies there.
    moveAutomation (song, getStretchesOf (tracks, range), length, true);
    return copyRange;
}
} // namespace phazite
