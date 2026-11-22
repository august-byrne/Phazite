// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clips.h"
#include "internal/AudioClips.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// The clips' own code, in two parts. The storage, the only functions that know where a clip is kept (the song's tree, its
// CLIP elements on their tracks' elements, until the song holds its clips as values: roadmap step S5.20): a clip's value
// read (getClipValue(), getClipValues(), getClipPlacements()...), a clip added, set, copied and taken out (addClip(), setClip(),
// eraseClip()...), and the reads of the clips' elements kept for the tests and what reads a song's elements (getClipID(),
// getClipRange() of an element...). And every edit, written on the clips' values: each reads the clips it changes as values
// (each once, as the notes' edits do), works the next values out by its rules, and sets them.
namespace phazite
{
namespace
{
    const auto minimumClipLength = BeatDuration::fromBeats (shortestClipBeats);

    // Positions this close count as the same: rounding between beats and seconds leaves differences this small.
    const auto tinyBeats = BeatDuration::fromBeats (1.0e-6);

    // How far inside a clip a cut must be, in seconds.
    constexpr double splitMargin = 0.001;

    //==============================================================================
    // The storage's own.

    // Whether a track's element holds clips: a track's, not a folder's or a return's (Schema.h: trackKind).
    bool holdsClips (const juce::ValueTree& trackState)
    {
        return trackState.hasType (schema::TRACK) && ! isFolderState (trackState) && ! isReturnState (trackState);
    }

    // Where among a track's elements a clip goes to be at a place among its clips (from 0): before the clip there now, or
    // after them all (-1) for a place past them, or none (-1).
    int findChildIndexForClip (const juce::ValueTree& trackState, int place)
    {
        if (place < 0)
            return -1;

        for (int i = 0, clips = 0; i < trackState.getNumChildren(); ++i)
            if (trackState.getChild (i).hasType (schema::CLIP) && clips++ == place)
                return i;

        return -1;
    }

    // A clip's place, read from its element without the rest of its value: as readClipValue() reads those properties.
    ClipPlacement readClipPlacement (const juce::ValueTree& clipState)
    {
        const auto audio = isAudioState (clipState);
        const auto range = getClipRange (clipState);
        return { getClipID (clipState), audio ? ClipValue::Kind::audio : ClipValue::Kind::midi, range.getStart(),
                 audio ? BeatDuration() : range.getLength(), getClipOffset (clipState) };
    }

    // The places of a track's element's clips, in its order.
    std::vector<ClipPlacement> readClipPlacements (const juce::ValueTree& trackState)
    {
        const auto clips = getClipStates (trackState);
        std::vector<ClipPlacement> places;
        places.reserve (clips.size());

        for (const auto& clipState : clips)
            places.push_back (readClipPlacement (clipState));

        return places;
    }

    // The same of a clip of the song's by its ID (none for one that isn't), and the track it is on.
    std::optional<ClipPlacement> findClipPlacement (const Song& song, ClipID clip, TrackID* track = nullptr)
    {
        const auto clipState = findClipState (song, clip);

        if (! clipState.isValid())
            return std::nullopt;

        if (track != nullptr)
            *track = getTrackID (clipState.getParent());

        return readClipPlacement (clipState);
    }

    // A copy of a clip of the song (copy: its value, with an ID of its own), added after the clips of the track its value
    // names: its element the clip's copied (so that what the copy's value doesn't hold of it comes along, as a copy of the
    // element always took it), then given what differs; its notes the clip's (SongValues.h: copyClipNotes()), shared until
    // either is edited, where the song holds its values, else its elements in the copy. Returns its ID, or none.
    ClipID addCopy (Song& song, const ClipValue& clip, const ClipValue& copy)
    {
        const auto clipState = findClipState (song, clip.id);
        auto trackState = findTrackState (song, copy.track);

        if (! clipState.isValid() || ! holdsClips (trackState))
            return {};

        auto copyState = clipState.createCopy();
        writeClipElement (copy, clip, copyState, nullptr);

        if (auto* values = song.getValues())
            values->copyClipNotes (song, clip.id, copy.id);

        trackState.appendChild (copyState, getUndoOf (song));
        return copy.id;
    }

    // A clip's element, which holds held, made to hold next (setClip()): only what differs written, and on another track,
    // moved to the end of that one's clips.
    void writeClip (Song& song, juce::ValueTree& clipState, const ClipValue& held, const ClipValue& next)
    {
        jassert (held.id == next.id);

        if (held == next)
            return;

        auto* undo = getUndoOf (song);
        writeClipElement (next, held, clipState, undo);

        // To the end of another track's clips, as placeClipState() moves it.
        if (next.track != held.track)
        {
            if (auto trackState = findTrackState (song, next.track); holdsClips (trackState) && clipState.getParent() != trackState)
            {
                clipState.getParent().removeChild (clipState, undo);
                trackState.appendChild (clipState, undo);
            }
        }
    }

    // A clip of the song changed by its ID with one look for it: change (const ClipValue&) gives the value it is to hold,
    // worked out from the one it holds (that one again for no change). Nothing for a clip that isn't the song's.
    template <typename Change>
    void changeClip (Song& song, ClipID clip, Change&& change)
    {
        if (auto clipState = findClipState (song, clip); clipState.isValid())
        {
            const auto held = readClipValue (clipState);
            writeClip (song, clipState, held, change (held));
        }
    }

    // The places of a clip's alias group's clips, the clip's first, then the others in the song's order (the clip's alone in
    // none; none for a clip that isn't the song's), with one look for it.
    std::vector<ClipPlacement> getAliasPlacements (const Song& song, ClipID clip)
    {
        const auto clipState = findClipState (song, clip);

        if (! clipState.isValid())
            return {};

        if (! clipState.hasProperty (schema::clipLink))
            return { readClipPlacement (clipState) };

        std::vector<ClipPlacement> places;

        for (const auto& alias : getAliasStates (song, clipState))
            places.push_back (readClipPlacement (alias));

        return places;
    }

    // Every clip of the song in an alias group, with its group (Schema.h: clipLink), track by track.
    std::vector<std::pair<ClipID, std::uint64_t>> getClipLinks (const Song& song)
    {
        std::vector<std::pair<ClipID, std::uint64_t>> links;

        for (const auto& clipState : getClipStates (song))
            if (const auto link = (std::uint64_t) (juce::int64) clipState[schema::clipLink]; link != 0)
                links.emplace_back (getClipID (clipState), link);

        return links;
    }

    //==============================================================================
    // The edits' own, on values.

    // An audio clip's value: none for a clip that isn't an audio clip of the song's.
    std::optional<ClipValue> findAudioClip (const Song& song, ClipID clip)
    {
        auto value = getClipValue (song, clip);
        return value.has_value() && value->isAudio() ? value : std::nullopt;
    }

    // The clips an edit of several reads, each read from the song once, and what it set them to since: what it reads of one
    // after setting it is what it set (as the notes' edits read a clip once: model/notes/Notes.cpp).
    class ClipsRead
    {
    public:
        explicit ClipsRead (const Song& s)
            : song (s)
        {
        }

        std::optional<ClipValue> find (ClipID clip)
        {
            auto found = read.find (clip.getRaw());

            if (found == read.end())
                found = read.emplace (clip.getRaw(), getClipValue (song, clip)).first;

            return found->second;
        }

        void hold (const ClipValue& clip) { read[clip.id.getRaw()] = clip; }

    private:
        const Song& song;
        std::map<std::uint64_t, std::optional<ClipValue>> read;
    };

    // A MIDI clip's value placed: its start not before the song's, its length at least the shortest, and how far into its
    // notes it starts, from 0 (setClipPlace()).
    ClipValue withPlace (ClipValue clip, BeatPosition start, BeatDuration length, BeatDuration offset)
    {
        clip.start = BeatPosition::fromBeats (std::max (0.0, start.inBeats()));
        clip.length = BeatDuration::fromBeats (std::max (length.inBeats(), minimumClipLength.inBeats()));
        clip.offset = offset.inBeats() > 0.0 ? offset : BeatDuration();
        return clip;
    }

    // An audio clip's value starting at a beat (not before the song's), playing a part of its file that starts at 0 or later
    // and ends at its start or later.
    ClipValue withAudioPart (ClipValue clip, BeatPosition start, FilePart part)
    {
        clip.start = BeatPosition::fromBeats (std::max (0.0, start.inBeats()));
        clip.fileStart = std::max (0.0, part.start);
        clip.fileEnd = std::max (part.start, part.end);
        return clip;
    }

    // A clip set to its next value, which is returned: what the song holds of it now.
    ClipValue setTo (Song& song, const ClipValue& clip, const ClipValue& next)
    {
        setClip (song, clip, next);
        return next;
    }

    // The latest a clip can end, in the song's seconds: where its audio does, for an audio clip; a MIDI clip has no end.
    double getLatestEnd (const Song& song, const ClipValue& clip)
    {
        return clip.isAudio() ? getAudioFileRange (song, clip).end : std::numeric_limits<double>::max();
    }

    // Where clips are on their tracks, for the automation under them (Automation.h: moveAutomation()).
    std::vector<AutomationStretch> getStretchesOf (const Song& song, ClipsRead& read, const std::vector<ClipID>& clips)
    {
        std::vector<AutomationStretch> stretches;

        for (const auto id : clips)
            if (const auto clip = read.find (id))
                stretches.push_back ({ clip->track, getLiveClipRange (song, *clip) });

        return stretches;
    }

    std::vector<AutomationStretch> getStretchesOf (const std::vector<TrackID>& tracks, BeatRange range)
    {
        std::vector<AutomationStretch> stretches;

        for (const auto track : tracks)
            stretches.push_back ({ track, range });

        return stretches;
    }

    // A clip moved to start at a beat, playing what it did.
    ClipValue withStartMoved (const ClipValue& clip, BeatPosition start)
    {
        return clip.isAudio() ? withAudioPart (clip, start, getFilePart (clip))
                              : withPlace (clip, start, getClipRange (clip).getLength(), clip.offset);
    }

    // A clip moved to start at a beat (not before the song does) on a track, keeping its length: onto another track only if
    // that track can hold it (canTrackHold()), else nothing changes.
    ClipValue withMove (const Song& song, const ClipValue& clip, BeatPosition start, TrackID track)
    {
        if (track != clip.track && ! canTrackHold (song, track, clip.isMidi()))
            return clip;

        auto next = clip;
        next.start = BeatPosition::fromBeats (std::max (0.0, start.inBeats()));
        next.track = track;
        return next;
    }

    // A clip's start moved to a beat, its end staying where it is, and what it plays too.
    ClipValue withStartTrimmed (const Song& song, const ClipValue& clip, BeatPosition start)
    {
        if (clip.isAudio())
        {
            const auto moved = getFileSecondsBetween (song, clip, getPlayedPosition (song, clip).start, start);
            auto part = getFilePart (clip);

            if (clip.sound.reversed)
                part.end -= moved;
            else
                part.start += moved;

            return withAudioPart (clip, start, part);
        }

        const auto range = getClipRange (clip);
        const auto moved = start - range.getStart();
        return withPlace (clip, start, range.getLength() - moved, clip.offset + moved);
    }

    ClipValue withEndTrimmed (const Song& song, const ClipValue& clip, BeatPosition end)
    {
        const auto range = getClipRange (clip);

        if (clip.isAudio())
        {
            const auto moved = getFileSecondsBetween (song, clip, getPlayedPosition (song, clip).end, end);
            auto part = getFilePart (clip);

            if (clip.sound.reversed)
                part.start -= moved;
            else
                part.end += moved;

            return withAudioPart (clip, range.getStart(), part);
        }

        return withPlace (clip, range.getStart(), end - range.getStart(), clip.offset);
    }

    // An audio clip's fade-in or fade-out, in seconds (none at 0 or less).
    ClipValue withFade (ClipValue clip, bool fadeIn, double seconds)
    {
        (fadeIn ? clip.fadeIn : clip.fadeOut).seconds = std::max (0.0, seconds);
        return clip;
    }

    // The same, set.
    ClipValue moveStartTo (Song& song, const ClipValue& clip, BeatPosition start)
    {
        return setTo (song, clip, withStartMoved (clip, start));
    }

    ClipValue moveTo (Song& song, const ClipValue& clip, BeatPosition start, TrackID track)
    {
        return setTo (song, clip, withMove (song, clip, start, track));
    }

    ClipValue trimStartTo (Song& song, const ClipValue& clip, BeatPosition start)
    {
        return setTo (song, clip, withStartTrimmed (song, clip, start));
    }

    ClipValue trimEndTo (Song& song, const ClipValue& clip, BeatPosition end)
    {
        return setTo (song, clip, withEndTrimmed (song, clip, end));
    }

    ClipValue setFadeOf (Song& song, const ClipValue& clip, bool fadeIn, double seconds)
    {
        return setTo (song, clip, withFade (clip, fadeIn, seconds));
    }

    // An audio clip's value with one of its fades in a shape.
    ClipValue withFadeCurve (ClipValue clip, bool fadeIn, FadeCurve curve)
    {
        auto& fade = fadeIn ? clip.fadeIn : clip.fadeOut;
        fade.sCurve = curve.shape == FadeShape::sCurve;
        fade.bend = juce::jlimit (-1.0f, 1.0f, curve.bend);
        return clip;
    }

    // A copy of a clip, on the same track and in the same place: as an alias, in the clip's alias group (which the clip
    // gets if it is in none), else in none. The copy's value, or none.
    std::optional<ClipValue> copyOf (Song& song, ClipsRead& read, ClipID id, bool asAliases)
    {
        auto clip = read.find (id);

        if (! clip.has_value())
            return std::nullopt;

        if (asAliases && clip->link == 0)
        {
            auto linked = *clip;
            linked.link = song.makeID();
            clip = setTo (song, *clip, linked);
            read.hold (*clip);
        }

        auto copy = *clip;
        copy.id = ClipID::fromRaw (song.makeID());

        if (! asAliases)
            copy.link = 0;

        if (! addCopy (song, *clip, copy).isValid())
            return std::nullopt;

        read.hold (copy);
        return copy;
    }

    std::vector<ClipValue> copyClipValues (Song& song, ClipsRead& read, const std::vector<ClipID>& clips, bool asAliases)
    {
        std::vector<ClipValue> copies;

        for (const auto clip : clips)
            if (const auto copy = copyOf (song, read, clip, asAliases))
                copies.push_back (*copy);

        return copies;
    }

    // Splits a clip of the song into two at a beat, if it is far enough inside it: both halves play what they did, and stay
    // in its alias group if it is in one; an audio clip has no fade where it is cut. The clip's value becomes its left half's;
    // returns the right half's.
    std::optional<ClipValue> splitAt (Song& song, ClipValue& clip, BeatPosition beat)
    {
        const auto range = getLiveClipRange (song, clip);
        const auto time = toSeconds (song, beat);

        if (time <= toSeconds (song, range.getStart()) + splitMargin || time >= toSeconds (song, range.getEnd()) - splitMargin)
            return std::nullopt;

        auto right = clip;
        right.id = ClipID::fromRaw (song.makeID());
        right.start = beat;

        if (clip.isAudio())
        {
            const auto into = getFileSecondsBetween (song, clip, getPlayedPosition (song, clip).start, beat);
            auto left = getFilePart (clip), rest = left;

            if (clip.sound.reversed) // the left half plays the end of the part, backwards
            {
                left.start = left.end - into;
                rest.end = left.start;
            }
            else
            {
                left.end = left.start + into;
                rest.start = left.end;
            }

            right.fileStart = rest.start;
            right.fileEnd = rest.end;
            right.fadeIn.seconds = 0.0;

            if (! addCopy (song, clip, right).isValid())
                return std::nullopt;

            auto leftHalf = withAudioPart (clip, range.getStart(), left);
            leftHalf.fadeOut.seconds = 0.0;
            clip = setTo (song, clip, leftHalf);
        }
        else
        {
            right.length = range.getEnd() - beat;
            right.offset = clip.offset + (beat - range.getStart());

            if (! addCopy (song, clip, right).isValid())
                return std::nullopt;

            clip = trimEndTo (song, clip, beat);
        }

        return right;
    }

    // What lies in a range of a clip goes: the whole clip, or its start or end, or its middle, splitting it in two.
    void deleteRegionOf (Song& song, ClipValue clip, BeatRange region)
    {
        const auto range = getLiveClipRange (song, clip);

        if (region.getStart() <= range.getStart() && range.getEnd() <= region.getEnd())
        {
            eraseClip (song, clip.id);
        }
        else if (range.getStart() < region.getStart() && range.getEnd() > region.getEnd())
        {
            if (const auto right = splitAt (song, clip, region.getStart()))
                trimStartTo (song, *right, region.getEnd());
        }
        else if (range.overlaps (region))
        {
            if (region.getEnd() < range.getEnd())
                trimStartTo (song, clip, region.getEnd());
            else if (range.getStart() < region.getStart())
                trimEndTo (song, clip, region.getStart());
        }
    }

    // The clips in a range of a track, as deleteTimeRange() deletes them, without the automation there: those on the track
    // found by their places (an audio clip's range read with its value), each read whole once where it is to change.
    void deleteClipsIn (Song& song, TrackID track, BeatRange range, bool closeGap)
    {
        for (const auto& place : getClipPlacements (song, track)) // as they were: deleting changes the track's clips
        {
            if (place.isAudio())
            {
                if (const auto clip = getClipValue (song, place.id); clip.has_value() && getLiveClipRange (song, *clip).overlaps (range))
                    deleteRegionOf (song, *clip, range);
            }
            else if (getClipRange (song, place).overlaps (range))
            {
                if (const auto clip = getClipValue (song, place.id))
                    deleteRegionOf (song, *clip, range);
            }
        }

        if (closeGap)
            for (const auto& place : getClipPlacements (song, track))
                if (place.start >= range.getEnd())
                    if (const auto clip = getClipValue (song, place.id))
                        moveStartTo (song, *clip, place.start - range.getLength());
    }

    // A copy of the part of a clip that lies in a range, where that part is. The copy is cut at the range's edges as
    // Split cuts clips (an audio clip loses its fades where it is cut); the clip itself stays whole.
    ClipID copyPartOfClip (Song& song, ClipID clip, BeatRange range)
    {
        ClipsRead read (song);
        auto part = copyOf (song, read, clip, false);

        if (! part.has_value())
            return {};

        if (auto rest = splitAt (song, *part, range.getStart()))
        {
            eraseClip (song, part->id); // what lies before the range
            part = rest;
        }

        if (const auto after = splitAt (song, *part, range.getEnd()))
            eraseClip (song, after->id);

        return part->id;
    }
} // namespace

//==============================================================================
// The storage.

juce::UndoManager* getUndoOf (Song& song)
{
    return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
}

juce::ValueTree findClipState (const Song& song, ClipID id)
{
    return id.isValid() ? song.getIndex().findClip (id.getRaw()) : juce::ValueTree();
}

std::vector<juce::ValueTree> getClipStates (const juce::ValueTree& trackState)
{
    std::vector<juce::ValueTree> clips;
    SongIndex::countVisited ((std::uint64_t) trackState.getNumChildren()); // a walk of a track's elements, for the speed gates

    for (const auto& child : trackState)
        if (child.hasType (schema::CLIP))
            clips.push_back (child);

    return clips;
}

std::vector<juce::ValueTree> getClipStates (const Song& song)
{
    std::vector<juce::ValueTree> clips;

    for (const auto& trackState : getTrackStates (song))
    {
        SongIndex::countVisited ((std::uint64_t) trackState.getNumChildren());

        for (const auto& child : trackState)
            if (child.hasType (schema::CLIP))
                clips.push_back (child);
    }

    return clips;
}

std::optional<ClipValue> getClipValue (const Song& song, ClipID clip)
{
    if (const auto clipState = findClipState (song, clip); clipState.isValid())
        return readClipValue (clipState);

    return std::nullopt;
}

std::vector<ClipValue> getClipValues (const Song& song, TrackID track)
{
    std::vector<ClipValue> values;

    for (const auto& clipState : getClipStates (findTrackState (song, track)))
        values.push_back (readClipValue (clipState));

    return values;
}

std::vector<ClipValue> getClipValues (const Song& song)
{
    std::vector<ClipValue> values;

    for (const auto& clipState : getClipStates (song))
        values.push_back (readClipValue (clipState));

    return values;
}

std::vector<ClipID> getAllClips (const Song& song)
{
    std::vector<ClipID> clips;

    for (const auto& clipState : getClipStates (song))
        clips.push_back (getClipID (clipState));

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

bool hasClips (const Song& song, TrackID track)
{
    return ! getClipStates (findTrackState (song, track)).empty();
}

bool hasClips (const Song& song, TrackID track, ClipValue::Kind kind)
{
    const auto clips = getClipStates (findTrackState (song, track));
    return std::any_of (clips.begin(), clips.end(),
                        [kind] (const juce::ValueTree& clipState) { return isAudioState (clipState) == (kind == ClipValue::Kind::audio); });
}

bool isMidiClip (const Song& song, ClipID clip)
{
    return isMidiState (findClipState (song, clip));
}

std::vector<ClipPlacement> getClipPlacements (const Song& song, TrackID track)
{
    return readClipPlacements (findTrackState (song, track));
}

std::vector<ClipPlacement> getClipPlacements (const Song& song)
{
    const auto clips = getClipStates (song);
    std::vector<ClipPlacement> places;
    places.reserve (clips.size());

    for (const auto& clipState : clips)
        places.push_back (readClipPlacement (clipState));

    return places;
}

std::vector<ClipID> getAliasClips (const Song& song, ClipID clip)
{
    std::vector<ClipID> clips;

    if (const auto clipState = findClipState (song, clip); clipState.isValid())
    {
        if (! clipState.hasProperty (schema::clipLink))
            return { clip }; // in no group

        for (const auto& alias : getAliasStates (song, clipState))
            clips.push_back (getClipID (alias));
    }

    return clips;
}

ClipID addClip (Song& song, ClipValue clip, const ClipNotes* notes, int index)
{
    auto trackState = findTrackState (song, clip.track);

    if (! holdsClips (trackState))
        return {};

    if (! clip.id.isValid() || findClipState (song, clip.id).isValid())
        clip.id = ClipID::fromRaw (song.makeID());

    // Its element written as its maker writes a new one; its notes given to it as it joins the song, without a walk through
    // them (SongValues.h: a clip of a song that holds its values holds none in its element; one of a song that keeps
    // everything in its tree, its elements).
    juce::ValueTree clipState (schema::CLIP);
    writeClipElement (clip, clipState, nullptr);

    if (notes != nullptr && clip.isMidi())
    {
        if (auto* values = song.getValues())
            values->setClipNotes (song, clip.id, *notes);
        else
            writeClipNotes (*notes, clipState);
    }

    trackState.addChild (clipState, findChildIndexForClip (trackState, index), getUndoOf (song));
    return clip.id;
}

void setClip (Song& song, const ClipValue& clip)
{
    // What the song holds of it read, and what a clip takes of the value given written where it differs.
    if (const auto held = getClipValue (song, clip.id))
        setClip (song, *held, keepWithinClip (clip));
}

void setClip (Song& song, const ClipValue& held, const ClipValue& next)
{
    if (held == next)
        return;

    if (auto clipState = findClipState (song, held.id); clipState.isValid())
        writeClip (song, clipState, held, next);
}

void eraseClip (Song& song, ClipID clip)
{
    auto clipState = findClipState (song, clip);

    if (! clipState.isValid())
        return;

    // The last one left of a group of linked clips leaves it, as Make Unique leaves it (makeClipsUnique()): it would still
    // show as linked to nothing.
    if (clipState.hasProperty (schema::clipLink))
        if (const auto aliases = getAliasClips (song, clip); aliases.size() == 2)
            setClipLink (song, aliases[1], 0);

    clipState.getParent().removeChild (clipState, getUndoOf (song));
}

std::optional<ClipValue> getClipValueAsSet (const Song& song, const ClipValue& clip)
{
    const auto clipState = findClipState (song, clip.id);

    if (! clipState.isValid())
        return std::nullopt;

    // Written as setClip() writes it, into a copy of what of its element it writes (its properties and its markers, not its
    // notes), and read back.
    juce::ValueTree copy (schema::CLIP);
    copy.copyPropertiesFrom (clipState, nullptr);

    for (const auto& child : clipState)
        if (child.hasType (schema::WARP))
            copy.appendChild (child.createCopy(), nullptr);

    writeClipElement (keepWithinClip (clip), readClipValue (copy), copy, nullptr);
    auto held = readClipValue (copy);
    held.track = holdsClips (findTrackState (song, clip.track)) ? clip.track : getClipTrack (song, clip.id);
    return held;
}

void setClipLink (Song& song, ClipID clip, std::uint64_t link)
{
    // The one property alone, as the clips' code writes it (copyClips(), makeClipsUnique()).
    auto clipState = findClipState (song, clip);

    if (! clipState.isValid())
        return;

    if (link != 0)
        clipState.setProperty (schema::clipLink, (juce::int64) link, getUndoOf (song));
    else
        clipState.removeProperty (schema::clipLink, getUndoOf (song));
}

void copyTrackClips (Song& song, TrackID from, TrackID to, const std::map<std::uint64_t, std::uint64_t>& ids, bool asAliases)
{
    const auto fromState = findTrackState (song, from);
    auto toState = findTrackState (song, to);

    if (! fromState.isValid() || ! holdsClips (toState) || fromState == toState)
        return;

    auto* values = song.getValues();

    for (int i = 0; i < fromState.getNumChildren(); ++i)
    {
        const auto clipState = fromState.getChild (i);

        if (! clipState.hasType (schema::CLIP))
            continue;

        // Its element as it is, its notes in it where the song keeps everything in its tree; else its value shared.
        const auto old = getClipID (clipState);
        const auto found = ids.find (old.getRaw());
        const auto id = ClipID::fromRaw (found != ids.end() ? found->second : song.makeID());
        auto copy = clipState.createCopy();
        copy.setProperty (schema::clipID, (juce::int64) id.getRaw(), nullptr);

        if (! asAliases)
            copy.removeProperty (schema::clipLink, nullptr);

        if (values != nullptr)
            values->copyClipNotes (song, old, id);

        toState.addChild (copy, i, getUndoOf (song));
    }
}

void setTrackClipOrder (Song& song, TrackID track, const std::vector<ClipID>& order)
{
    auto trackState = findTrackState (song, track);
    std::vector<int> places; // where the track's clips' elements are among its elements, in their order

    for (int i = 0; i < trackState.getNumChildren(); ++i)
        if (trackState.getChild (i).hasType (schema::CLIP))
            places.push_back (i);

    auto held = getClips (song, track), given = order;
    std::sort (held.begin(), held.end());
    std::sort (given.begin(), given.end());

    if (held != given || std::adjacent_find (given.begin(), given.end()) != given.end())
    {
        jassertfalse; // the track's clips, each once, in another order
        return;
    }

    // Each place filled in turn with the clip the order puts there: the two elements swapped, by two moves that leave every
    // element between them where it was.
    auto* undo = getUndoOf (song);

    for (size_t i = 0; i < places.size(); ++i)
    {
        const auto place = places[i];

        if (getClipID (trackState.getChild (place)) == order[i])
            continue;

        int from = -1;

        for (size_t j = i + 1; j < places.size() && from < 0; ++j)
            if (getClipID (trackState.getChild (places[j])) == order[i])
                from = places[j];

        jassert (from > place);
        trackState.moveChild (from, place, undo); // the clip that was at place is now at place + 1

        if (from > place + 1)
            trackState.moveChild (place + 1, from, undo);
    }
}

BeatPosition getClipsEnd (const Song& song)
{
    // Every clip read where the song keeps it, without a list of their places: a MIDI clip's place, an audio clip's value.
    BeatPosition end;

    for (const auto& clipState : getClipStates (song))
    {
        const auto clip = readClipPlacement (clipState);
        end = std::max (end, clip.isAudio() ? getLiveClipRange (song, readClipValue (clipState)).getEnd() : clip.start + clip.length);
    }

    return end;
}

//==============================================================================
// The clips' elements read (for the tests, and what reads a song's elements: Clips.h).

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
    return isAudioState (clipState) ? getLiveClipRange (song, readClipValue (clipState)) : getClipRange (clipState);
}

BeatDuration getClipOffset (const juce::ValueTree& clipState)
{
    return BeatDuration::fromBeats (clipState.getProperty (schema::clipOffset, 0.0));
}

BeatPosition getContentStart (const juce::ValueTree& clipState)
{
    return getClipRange (clipState).getStart() - getClipOffset (clipState);
}

std::vector<juce::ValueTree> getAliasStates (const Song& song, const juce::ValueTree& clipState)
{
    return song.getIndex().findLinked (clipState);
}

juce::ValueTree makeMidiClipState (Song& song, const juce::String& name, BeatRange range)
{
    const auto start = std::max (0.0, range.getStart().inBeats());
    juce::ValueTree clipState (schema::CLIP);
    clipState.setProperty (schema::clipID, (juce::int64) song.makeID(), nullptr);
    clipState.setProperty (schema::clipKind, schema::clipIsMidi, nullptr);

    if (name.isNotEmpty())
        clipState.setProperty (schema::clipName, name, nullptr);

    clipState.setProperty (schema::clipStart, start, nullptr);
    clipState.setProperty (schema::clipLength, std::max (range.getLength().inBeats(), minimumClipLength.inBeats()), nullptr);
    return clipState;
}

juce::String getClipName (const Song& song, const juce::ValueTree& clipState)
{
    if (const auto own = clipState[schema::clipName].toString(); own.isNotEmpty())
        return own;

    return getTrackName (song, getTrackID (clipState.getParent()));
}

ClipID placeClipState (Song& song, juce::ValueTree clipState, TrackID track)
{
    auto trackState = findTrackState (song, track);

    // Only a track holds clips, not a folder or a return (Schema.h: trackKind), which would neither show nor play them.
    if (! holdsClips (trackState))
        return {};

    if (clipState.getParent() != trackState)
    {
        if (auto parent = clipState.getParent(); parent.isValid())
            parent.removeChild (clipState, getUndoOf (song));

        trackState.appendChild (clipState, getUndoOf (song));
    }

    return getClipID (clipState);
}

void setClipPlace (Song& song, juce::ValueTree clipState, BeatPosition start, BeatDuration length, BeatDuration offset)
{
    // The element given, which a value is read from and written into, as setClipPlace() by ID sets a clip of the song.
    const auto held = readClipValue (clipState);
    writeClipElement (withPlace (held, start, length, offset), held, clipState, getUndoOf (song));
}

//==============================================================================
// The clips by their values.

BeatPosition getContentStart (const ClipValue& clip)
{
    return clip.start - clip.offset;
}

BeatPosition getContentStart (const Song& song, ClipID clip)
{
    const auto place = findClipPlacement (song, clip);
    return place.has_value() ? place->start - place->offset : BeatPosition();
}

BeatRange getClipRange (const ClipValue& clip)
{
    return { clip.start, clip.start + (clip.isAudio() ? BeatDuration() : clip.length) };
}

BeatRange getLiveClipRange (const Song& song, const ClipValue& clip)
{
    // An audio clip's for as long as its part takes to play, worked out from the value (AudioClips.h).
    return clip.isAudio() ? getAudioClipRange (song, clip) : getClipRange (clip);
}

BeatRange getClipRange (const Song& song, const ClipPlacement& clip)
{
    return clip.isAudio() ? getClipRange (song, clip.id) : BeatRange (clip.start, clip.start + clip.length);
}

BeatRange getClipRange (const Song& song, ClipID clip)
{
    // With one look for it: its place, or an audio clip's value (getClipRange() of an element).
    return getClipRange (song, findClipState (song, clip));
}

juce::String getClipName (const Song& song, ClipID clip)
{
    const auto value = getClipValue (song, clip);
    return value.has_value() ? getClipName (song, *value) : getTrackName (song, TrackID());
}

juce::String getClipName (const Song& song, const ClipValue& clip)
{
    return clip.name.isNotEmpty() ? clip.name : getTrackName (song, clip.track);
}

ClipValue makeMidiClip (Song& song, const juce::String& name, BeatRange range)
{
    // As makeMidiClipState() makes its element.
    ClipValue clip;
    clip.id = ClipID::fromRaw (song.makeID());
    clip.kind = ClipValue::Kind::midi;
    clip.name = name;
    clip.start = BeatPosition::fromBeats (std::max (0.0, range.getStart().inBeats()));
    clip.length = BeatDuration::fromBeats (std::max (range.getLength().inBeats(), minimumClipLength.inBeats()));
    return clip;
}

bool setClipFiles (Song& song, const juce::String& path, const juce::String& newPath)
{
    // Each audio clip that plays the file (each of an alias group plays it: each is given the new path).
    auto any = false;

    for (const auto& place : getClipPlacements (song))
    {
        if (! place.isAudio())
            continue;

        if (const auto clip = getClipValue (song, place.id); clip.has_value() && clip->sound.file == path)
        {
            auto next = *clip;
            next.sound.file = newPath;
            setClip (song, *clip, next);
            any = true;
        }
    }

    return any;
}

void moveClipStart (Song& song, ClipID clip, BeatPosition start)
{
    changeClip (song, clip, [start] (const ClipValue& value) { return withStartMoved (value, start); });
}

void setClipPlace (Song& song, ClipID clip, BeatPosition start, BeatDuration length, BeatDuration offset)
{
    changeClip (song, clip, [&] (const ClipValue& value) { return withPlace (value, start, length, offset); });
}

bool canTrackHold (const Song& song, TrackID track, bool midi, const std::vector<ClipID>& leaving)
{
    // The track's kind from its element, and its clips' places with one look for it.
    const auto trackState = findTrackState (song, track);

    if (! holdsClips (trackState))
        return false;

    for (const auto& clip : readClipPlacements (trackState))
        if (clip.isAudio() == midi && std::find (leaving.begin(), leaving.end(), clip.id) == leaving.end())
            return false;

    return midi || ! hasInstrument (song, track);
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
        TrackID track;
        const auto place = findClipPlacement (*song, id, &track);
        const auto row = (size_t) (std::find (tracks.begin(), tracks.end(), track) - tracks.begin());
        placed.push_back ({ id, row, place.has_value() ? place->start : BeatPosition() });
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
                        [&song] (ClipID id)
                        {
                            const auto clip = getClipValue (song, id);
                            return clip.has_value() && clip->link != 0;
                        });
}

bool hasAudioClips (const Song& song, const std::vector<ClipID>& clips)
{
    return std::any_of (clips.begin(), clips.end(), [&song] (ClipID id) { return isAudioClip (song, id); });
}

bool hasFadeShape (const Song& song, const std::vector<ClipID>& clips, FadeCurve curve)
{
    auto any = false;

    for (const auto id : clips)
    {
        if (const auto clip = findAudioClip (song, id))
        {
            if (! (getFadeCurve (clip->fadeIn) == curve && getFadeCurve (clip->fadeOut) == curve))
                return false;

            any = true;
        }
    }

    return any;
}

std::optional<double> getLinkedPlayBeat (const Song& song, const std::vector<ClipID>& shown, double songBeat)
{
    // Each clip by its place (a MIDI clip's range; an audio clip's start alone): what plays notes.
    const auto beat = BeatPosition::fromBeats (songBeat);
    const auto rangeOf = [] (const ClipPlacement& clip) { return BeatRange (clip.start, clip.start + clip.length); };
    const auto contentStartOf = [] (const ClipPlacement& clip) { return clip.start - clip.offset; };
    std::vector<std::vector<ClipPlacement>> groups; // each shown clip's group, the clip first

    for (const auto clip : shown)
        if (auto group = getAliasPlacements (song, clip); ! group.empty())
            groups.push_back (std::move (group));

    if (std::any_of (groups.begin(), groups.end(),
                     [&rangeOf, beat] (const std::vector<ClipPlacement>& group) { return rangeOf (group.front()).contains (beat); }))
        return {};

    // The place in their shared notes, from where those count, in a clip linked to one of these that plays there.
    for (const auto& group : groups)
        for (size_t i = 1; i < group.size(); ++i)
            if (rangeOf (group[i]).contains (beat))
                return (contentStartOf (group.front()) + (beat - contentStartOf (group[i]))).inBeats();

    return {};
}

std::vector<ClipID> copyClips (Song& song, const std::vector<ClipID>& clips, bool asAliases)
{
    // Aliases share an alias group, which the original gets if it is in none; a copy that isn't an alias leaves the
    // original's.
    ClipsRead read (song);
    std::vector<ClipID> copies;

    for (const auto& copy : copyClipValues (song, read, clips, asAliases))
        copies.push_back (copy.id);

    return copies;
}

void moveClip (Song& song, ClipID clip, BeatPosition start, TrackID track)
{
    // To its new start (playing what it did) and track: another only if it can hold it.
    changeClip (song, clip, [&] (const ClipValue& value) { return withMove (song, value, start, track); });
}

void trimClipStart (Song& song, ClipID clip, BeatPosition beat)
{
    changeClip (song, clip,
                [&song, beat] (const ClipValue& value)
                {
                    if (value.isAudio())
                    {
                        // As far back as its audio starts.
                        const auto earliest = std::max (0.0, getAudioFileRange (song, value).start);
                        const auto latest = toSeconds (song, getLiveClipRange (song, value).getEnd() - minimumClipLength);

                        if (latest >= earliest)
                            return withStartTrimmed (song, value, toBeats (song, juce::jlimit (earliest, latest, toSeconds (song, beat))));

                        return value;
                    }

                    const auto range = getClipRange (value);
                    const auto earliest = std::max (0.0, getContentStart (value).inBeats());
                    const auto latest = range.getEnd().inBeats() - minimumClipLength.inBeats();

                    if (latest >= earliest)
                        return withStartTrimmed (song, value, BeatPosition::fromBeats (juce::jlimit (earliest, latest, beat.inBeats())));

                    return value;
                });
}

void trimClipEnd (Song& song, ClipID clip, BeatPosition beat)
{
    changeClip (song, clip,
                [&song, beat] (const ClipValue& value)
                {
                    // An audio clip as far as its audio goes on.
                    const auto start = getLiveClipRange (song, value).getStart();
                    const auto earliest = toSeconds (song, start + minimumClipLength);
                    const auto latest = getLatestEnd (song, value);

                    if (latest >= earliest)
                        return withEndTrimmed (song, value, toBeats (song, juce::jlimit (earliest, latest, toSeconds (song, beat))));

                    return value;
                });
}

void setClipFade (Song& song, ClipID clip, bool fadeIn, double seconds)
{
    changeClip (song, clip,
                [&song, fadeIn, seconds] (const ClipValue& value)
                {
                    if (! value.isAudio())
                        return value;

                    const auto position = getPlayedPosition (song, value);
                    return withFade (value, fadeIn, juce::jlimit (0.0, position.end - position.start, seconds));
                });
}

void setCrossfade (Song& song, ClipID first, ClipID second, double boundary, double length)
{
    const auto firstValue = findAudioClip (song, first), secondValue = findAudioClip (song, second);

    if (! firstValue.has_value() || ! secondValue.has_value())
        return;

    // How far each can reach over the boundary: the first as far as its audio goes on, the second as far
    // back as its audio starts.
    const auto firstReach = getLatestEnd (song, *firstValue) - boundary;
    const auto secondReach = boundary - std::max (0.0, getAudioFileRange (song, *secondValue).start);
    const auto half = std::max (0.0, std::min ({ length / 2.0, firstReach, secondReach }));
    const auto same = first == second; // each edit then starts from what the one before left

    // Both play where they overlap; the fades make that a crossfade.
    auto a = trimEndTo (song, *firstValue, toBeats (song, boundary + half));
    auto b = trimStartTo (song, same ? a : *secondValue, toBeats (song, boundary - half));
    a = setFadeOf (song, same ? b : a, false, half * 2.0);
    setFadeOf (song, same ? a : b, true, half * 2.0);
}

void setFadeShape (Song& song, const std::vector<ClipID>& clips, FadeCurve curve)
{
    for (const auto clip : clips)
        if (const auto value = findAudioClip (song, clip))
            setClip (song, *value, withFadeCurve (withFadeCurve (*value, true, curve), false, curve));
}

void setFadeCurve (Song& song, ClipID clip, bool fadeIn, FadeCurve curve)
{
    if (const auto value = findAudioClip (song, clip))
    {
        const UndoStep step (song, "Change Fade Shape");
        setClip (song, *value, withFadeCurve (*value, fadeIn, curve));
    }
}

ClipID insertMidiClip (Song& song, TrackID track, BeatRange range, Selection* selection, const juce::String& name)
{
    auto value = makeMidiClip (song, name, range);
    value.track = track;
    const auto clip = addClip (song, value);

    if (clip.isValid() && selection != nullptr)
        selection->selectOnly (clip);

    return clip;
}

ClipID insertBarLongMidiClip (Song& song, TrackID track, BeatPosition start, Selection* selection)
{
    const auto bar = getBars (song).getBarLength (start);
    return insertMidiClip (song, track, { start, start + bar }, selection);
}

ClipID addMidiClipAt (Song& song, TrackID track, BeatPosition start, Selection* selection)
{
    const UndoStep step (song, "Add MIDI Clip");
    return insertBarLongMidiClip (song, track, start, selection);
}

ClipID placeClip (Song& song, TrackID track, BeatPosition start, ClipID lastUsed)
{
    // The clip used last counts while it is still on one of the song's tracks, and on a track it suits (canTrackHold()): an
    // audio clip went on a track with an instrument, silenced by it, and notes on one of audio, unheard.
    if (const auto used = findClipPlacement (song, lastUsed); used.has_value() && canTrackHold (song, track, ! used->isAudio()))
    {
        ClipsRead read (song);
        const auto copy = copyOf (song, read, lastUsed, ! used->isAudio());

        if (! copy.has_value())
            return {};

        moveTo (song, *copy, start, track);
        return copy->id;
    }

    if (findTrackState (song, track).hasType (schema::TRACK) && hasInstrument (song, track))
        return insertBarLongMidiClip (song, track, start, nullptr);

    return {};
}

double getClipStartOffset (const Song& song, ClipID clip)
{
    const auto value = getClipValue (song, clip);

    if (! value.has_value())
        return 0.0;

    if (value->isAudio())
        return getPlayedPosition (song, *value).offset;

    return toSeconds (song, value->start) - toSeconds (song, getContentStart (*value));
}

void slipClip (Song& song, ClipID clip, double startOffset, double distance)
{
    // The offset stays at 0 or more.
    const auto offset = std::max (0.0, startOffset - distance);

    changeClip (song, clip,
                [&song, offset] (const ClipValue& value)
                {
                    if (! value.isAudio())
                    {
                        const auto range = getClipRange (value);
                        const auto contentStart = toBeats (song, toSeconds (song, range.getStart()) - offset);
                        return withPlace (value, range.getStart(), range.getLength(), range.getStart() - contentStart);
                    }

                    // The same length of its file, from as far into what it plays, the whole part kept inside the file either
                    // way.
                    auto part = getFilePart (value);
                    const auto length = part.end - part.start;
                    const auto fileLength = getClipFileLength (song, value);
                    const auto reversed = value.sound.reversed;

                    if (reversed && fileLength <= 0.0)
                        return value; // backwards, from the end of a file that can't be read

                    if (fileLength > 0.0 && length >= fileLength)
                        return value; // the whole file already, or more: no other part of it is as long

                    const auto latest = fileLength > 0.0 ? std::max (0.0, fileLength - length) : std::numeric_limits<double>::max();
                    const auto from = juce::jlimit (0.0, latest, getPartStartFor (song, value, offset));

                    if (reversed)
                        part.end = fileLength - from;
                    else
                        part.end = from + length;

                    part.start = part.end - length;
                    return withAudioPart (value, value.start, part);
                });
}

//==============================================================================
std::vector<ClipID> splitClips (Song& song, const std::vector<ClipID>& clips, BeatPosition beat)
{
    std::vector<ClipID> parts;

    for (const auto clip : clips)
        if (auto value = getClipValue (song, clip))
            if (const auto right = splitAt (song, *value, beat))
                parts.push_back (right->id);

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
    ClipsRead read (song);
    std::optional<BeatRange> span;

    for (const auto id : clips)
        if (const auto clip = read.find (id))
        {
            const auto range = getLiveClipRange (song, *clip);
            span = span.has_value() ? span->getUnionWith (range) : range;
        }

    if (! span.has_value())
        return {};

    if (selection != nullptr)
        selection->deselectAll();

    std::vector<ClipID> copies;

    for (const auto& copy : copyClipValues (song, read, clips, asAliases)) // each starts where its original is
    {
        read.hold (moveTo (song, copy, copy.start + span->getLength(), copy.track));
        copies.push_back (copy.id);

        if (selection != nullptr)
            selection->addToSelection (copy.id);
    }

    // The automation under the clips is copied with them.
    moveAutomation (song, getStretchesOf (song, read, clips), span->getLength(), true);
    return copies;
}

void unlinkLoneClips (Song& song)
{
    std::map<std::uint64_t, std::vector<ClipID>> groups;

    for (const auto& [clip, link] : getClipLinks (song))
        groups[link].push_back (clip);

    for (const auto& [link, members] : groups)
        if (members.size() == 1)
            setClipLink (song, members.front(), 0);
}

void makeClipsUnique (Song& song, const std::vector<ClipID>& clips)
{
    for (const auto id : clips)
    {
        const auto clip = getClipValue (song, id);

        if (! clip.has_value() || clip->link == 0)
            continue;

        const auto aliases = getAliasClips (song, id);
        auto next = *clip;
        next.link = 0;
        setClip (song, *clip, next);

        // The last one left in the group leaves it too.
        if (aliases.size() == 2)
            setClipLink (song, aliases[1], 0);
    }
}

void renameClips (Song& song, const std::vector<ClipID>& clips, const juce::String& name)
{
    // The clips with their aliases, each once, but only those that the name changes.
    const auto trimmed = name.trim();
    const auto ownNameOf = [&song, &trimmed] (const ClipValue& clip)
    { return trimmed == getTrackName (song, clip.track) ? juce::String() : trimmed; };
    std::vector<ClipValue> renamed;

    for (const auto clip : clips)
    {
        for (const auto alias : getAliasClips (song, clip))
        {
            if (std::any_of (renamed.begin(), renamed.end(), [alias] (const ClipValue& each) { return each.id == alias; }))
                continue;

            if (const auto value = getClipValue (song, alias); value.has_value() && value->name != ownNameOf (*value))
                renamed.push_back (*value);
        }
    }

    if (renamed.empty())
        return;

    const UndoStep step (song, clips.size() > 1 ? "Rename Clips" : "Rename Clip");

    for (const auto& clip : renamed)
    {
        auto next = clip;
        next.name = ownNameOf (clip);
        setClip (song, clip, next);
    }
}

bool areAllClipsMuted (const Song& song, const std::vector<ClipID>& clips)
{
    return areAllOn (clips,
                     [&song] (ClipID id)
                     {
                         const auto clip = getClipValue (song, id);
                         return clip.has_value() && clip->muted;
                     });
}

void toggleClipsMuted (Song& song, const std::vector<ClipID>& clips)
{
    toggleAll (
        clips,
        [&song] (ClipID id)
        {
            const auto clip = getClipValue (song, id);
            return clip.has_value() && clip->muted;
        },
        [&song] (ClipID id, bool on)
        {
            changeClip (song, id,
                        [on] (ClipValue clip)
                        {
                            clip.muted = on; // off is no property at all, as it is at first
                            return clip;
                        });
        });
}

void nudgeClips (Song& song, const std::vector<ClipID>& clips, BeatDuration amount)
{
    // The earliest clip decides how far back they can go. The automation under them goes along.
    ClipsRead read (song);

    for (const auto id : clips)
        if (const auto clip = read.find (id))
            amount = std::max (amount, BeatPosition() - clip->start);

    const auto under = getStretchesOf (song, read, clips);

    for (const auto id : clips)
        if (const auto clip = read.find (id))
            read.hold (moveTo (song, *clip, clip->start + amount, clip->track));

    moveAutomation (song, under, amount, false);
}

void deleteClips (Song& song, const std::vector<ClipID>& clips, bool closeGap)
{
    // Latest first, so that closing one gap doesn't move a clip still to be deleted; each read whole at its turn.
    std::vector<ClipPlacement> sorted;

    for (const auto clip : clips)
        if (const auto place = findClipPlacement (song, clip))
            sorted.push_back (*place);

    std::sort (sorted.begin(), sorted.end(), [] (const ClipPlacement& a, const ClipPlacement& b) { return a.start > b.start; });

    for (const auto& place : sorted)
    {
        const auto clip = getClipValue (song, place.id);

        if (! clip.has_value())
            continue;

        const auto removed = getLiveClipRange (song, *clip);
        const auto track = clip->track;
        const std::vector<AutomationStretch> under { { track, removed } };
        eraseClip (song, clip->id);

        // Closing the gap takes the clip's time out of its track, its automation with it, and what comes after moves
        // back, automation and all; without, the automation stays where it is.
        if (closeGap)
        {
            for (const auto& other : getClipPlacements (song, track))
                if (other.start >= removed.getStart())
                    if (const auto value = getClipValue (song, other.id))
                        moveStartTo (song, *value, std::max (removed.getStart(), other.start - removed.getLength()));

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
        for (const auto& clip : getClipPlacements (song, track)) // as they were: the parts join the track
            if (const auto clipRange = getClipRange (song, clip);
                clipRange.getStart() < range.getEnd() - tinyBeats && clipRange.getEnd() > range.getStart() + tinyBeats)
                if (const auto part = copyPartOfClip (song, clip.id, range); part.isValid())
                    parts.push_back (part);

    // Each part goes a range's length later, in place of what lies there. What is in the way goes first, so that no
    // part cuts another.
    for (const auto part : parts)
        if (const auto clip = getClipValue (song, part))
        {
            const auto partRange = getLiveClipRange (song, *clip);
            deleteClipsIn (song, clip->track, { partRange.getStart() + length, partRange.getEnd() + length }, false);
        }

    for (const auto part : parts)
        if (const auto clip = getClipValue (song, part))
            moveTo (song, *clip, clip->start + length, clip->track);

    // And the range's automation, on its tracks and folders, in place of what lies there.
    moveAutomation (song, getStretchesOf (tracks, range), length, true);
    return copyRange;
}
} // namespace phazite
