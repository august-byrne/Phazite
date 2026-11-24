// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Clips.h"
#include "internal/AudioClips.h"
#include "internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/Toggles.h"
#include "model/base/internal/ClipNotesValue.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/NotesArea.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/Visits.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/AreaClashes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

// The clips' own code, in two parts. The storage, the only functions that know where a clip is kept (the song's clips' area,
// beside its tree: model/base/internal/SongValues.h): a clip's value read (getClipValue(), getClipValues(),
// getClipPlacements()...), and a clip added, set, copied and taken out (addClip(), setClip(), eraseClip()...). And every edit,
// written on the clips' values: each reads the clips it changes as values (each once, as the notes' edits do), works the
// next values out by its rules, and sets them.
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

    // The song's clips as its clips' area holds them now; none for a song that holds no values (one made to be read or checked
    // keeps everything in its tree, which nothing reads clips of by their IDs: Song::getValues()). Each answer counts as one
    // look, and each clip of a list walked as one more, for the speed gates (countVisited(), as a walk of a
    // track's elements counted).
    const SongClips& getSongClips (const Song& song)
    {
        static const SongClips none;
        countVisited (1);

        if (const auto* values = song.getValues())
            return values->clips.get();

        return none;
    }

    // Whether a track holds clips: a track's, not a folder's or a return's (Schema.h: trackKind).
    bool holdsClips (const Song& song, TrackID track)
    {
        return getTrackKind (song, track) == TrackValue::Kind::track;
    }

    ClipPlacement toPlacement (const ClipValue& clip)
    {
        return { clip.id, clip.kind, clip.start, clip.isAudio() ? BeatDuration() : clip.length, clip.offset };
    }

    // The places of a track's clips, in its order.
    std::vector<ClipPlacement> readClipPlacements (const SongClips& clips, TrackID track)
    {
        std::vector<ClipPlacement> places;

        if (const auto* list = clips.tracks.find (track.getRaw()))
        {
            countVisited (list->size());
            places.reserve (list->size());

            for (const auto id : *list)
                if (const auto* clip = clips.find (id))
                    places.push_back (toPlacement (*clip));
        }

        return places;
    }

    // The same of a clip of the song's by its ID (none for one that isn't), and the track it is on.
    std::optional<ClipPlacement> findClipPlacement (const Song& song, ClipID clip, TrackID* track = nullptr)
    {
        const auto* value = getSongClips (song).find (clip);

        if (value == nullptr)
            return std::nullopt;

        if (track != nullptr)
            *track = value->track;

        return toPlacement (*value);
    }

    // Each track's clips' IDs of the song, track by track in the song's order, each track's in its order.
    template <typename Each>
    void forEachClip (const Song& song, Each&& each)
    {
        const auto& clips = getSongClips (song);

        if (clips.clips.empty())
            return;

        for (const auto track : getTrackIDs (song))
            if (const auto* list = clips.tracks.find (track.getRaw()))
            {
                countVisited (list->size());

                for (const auto id : *list)
                    if (const auto* clip = clips.find (id))
                        each (*clip);
            }
    }

    // A copy of a clip of the song (copy: its value, with an ID of its own), added after the clips of the track its value
    // names, with the clip's notes (shared until either is edited). Returns its ID, or none.
    ClipID addCopy (Song& song, const ClipValue& clip, const ClipValue& copy)
    {
        auto* values = song.getValues();

        if (values == nullptr || values->findClip (clip.id) == nullptr || ! holdsClips (song, copy.track))
            return {};

        const auto notes = copy.isMidi() ? values->findClipNotes (clip.id) : nullptr;
        values->setClip (song, copy, -1, notes);
        return copy.id;
    }

    // A clip of the song changed by its ID with one look for it: change (const ClipValue&) gives the value it is to hold,
    // worked out from the one it holds (that one again for no change). Nothing for a clip that isn't the song's.
    template <typename Change>
    void changeClip (Song& song, ClipID clip, Change&& change)
    {
        if (const auto* held = getSongClips (song).find (clip))
        {
            const auto value = *held;
            setClip (song, value, change (value));
        }
    }

    // The places of a clip's alias group's clips, the clip's first, then the others in the song's order (the clip's alone in
    // none; none for a clip that isn't the song's).
    std::vector<ClipPlacement> getAliasPlacements (const Song& song, ClipID clip)
    {
        std::vector<ClipPlacement> places;
        const auto& clips = getSongClips (song);

        for (const auto alias : getAliasClips (song, clip))
            if (const auto* value = clips.find (alias))
                places.push_back (toPlacement (*value));

        return places;
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

std::optional<ClipValue> getClipValue (const Song& song, ClipID clip)
{
    if (const auto* value = getSongClips (song).find (clip))
        return *value;

    return std::nullopt;
}

std::vector<ClipValue> getClipValues (const Song& song, TrackID track)
{
    std::vector<ClipValue> values;
    const auto& clips = getSongClips (song);

    if (const auto* list = clips.tracks.find (track.getRaw()))
    {
        countVisited (list->size());
        values.reserve (list->size());

        for (const auto id : *list)
            if (const auto* clip = clips.find (id))
                values.push_back (*clip);
    }

    return values;
}

std::vector<ClipValue> getClipValues (const Song& song)
{
    std::vector<ClipValue> values;
    forEachClip (song, [&values] (const ClipValue& clip) { values.push_back (clip); });
    return values;
}

std::vector<ClipID> getAllClips (const Song& song)
{
    std::vector<ClipID> clips;
    forEachClip (song, [&clips] (const ClipValue& clip) { clips.push_back (clip.id); });
    return clips;
}

std::vector<ClipID> getClips (const Song& song, TrackID track)
{
    if (const auto* list = getSongClips (song).tracks.find (track.getRaw()))
    {
        countVisited (list->size());
        return { list->begin(), list->end() };
    }

    return {};
}

TrackID getClipTrack (const Song& song, ClipID clip)
{
    if (const auto* value = getSongClips (song).find (clip))
        return value->track;

    return {};
}

bool hasClips (const Song& song, TrackID track)
{
    return getSongClips (song).tracks.find (track.getRaw()) != nullptr;
}

bool hasClips (const Song& song, TrackID track, ClipValue::Kind kind)
{
    const auto& clips = getSongClips (song);

    if (const auto* list = clips.tracks.find (track.getRaw()))
        for (const auto id : *list)
            if (const auto* clip = clips.find (id); clip != nullptr && clip->kind == kind)
                return true;

    return false;
}

bool isMidiClip (const Song& song, ClipID clip)
{
    const auto* value = getSongClips (song).find (clip);
    return value != nullptr && value->isMidi();
}

std::vector<ClipPlacement> getClipPlacements (const Song& song, TrackID track)
{
    return readClipPlacements (getSongClips (song), track);
}

std::vector<ClipPlacement> getClipPlacements (const Song& song)
{
    std::vector<ClipPlacement> places;
    forEachClip (song, [&places] (const ClipValue& clip) { places.push_back (toPlacement (clip)); });
    return places;
}

std::vector<ClipID> getAliasClips (const Song& song, ClipID clip)
{
    const auto& clips = getSongClips (song);
    const auto* value = clips.find (clip);

    if (value == nullptr)
        return {};

    const auto* members = value->link != 0 ? clips.groups.find (value->link) : nullptr;

    if (members == nullptr)
        return { clip }; // in no group

    // The others in the song's order: by their tracks' places among the song's tracks, then by their places on their tracks,
    // each track's list looked through once.
    struct Placed
    {
        ClipID id;
        int track = 0;
        size_t place = 0;
    };

    std::vector<Placed> others;
    std::map<std::uint64_t, std::vector<size_t>> byTrack; // the others on each track, by their places in others

    for (const auto member : *members)
    {
        if (member == clip)
            continue;

        if (const auto* other = clips.find (member))
        {
            byTrack[other->track.getRaw()].push_back (others.size());
            others.push_back ({ member });
        }
    }

    for (const auto& [track, onIt] : byTrack)
    {
        const auto trackPlace = getTrackPlace (song, TrackID::fromRaw (track)).index;
        const auto* list = clips.tracks.find (track);
        size_t place = 0;

        for (const auto index : onIt)
            others[index].track = trackPlace;

        if (list != nullptr)
            for (const auto id : *list)
            {
                for (const auto index : onIt)
                    if (others[index].id == id)
                        others[index].place = place;

                ++place;
            }
    }

    std::sort (others.begin(), others.end(),
               [] (const Placed& a, const Placed& b) { return a.track != b.track ? a.track < b.track : a.place < b.place; });

    std::vector<ClipID> aliases { clip };

    for (const auto& other : others)
        aliases.push_back (other.id);

    return aliases;
}

ClipID addClip (Song& song, ClipValue clip, const ClipNotes* notes, int index)
{
    auto* values = song.getValues();

    if (values == nullptr || ! holdsClips (song, clip.track))
        return {};

    if (! clip.id.isValid() || values->findClip (clip.id) != nullptr)
        clip.id = ClipID::fromRaw (song.makeID());

    // What a clip takes of its value (keepWithinClip()), its notes given to it as it joins the song, without a walk through
    // them; a MIDI clip without, the alias group's it joins or an empty value (SongValues.h: setClip()).
    const auto id = clip.id;
    values->setClip (song, keepWithinClip (std::move (clip)), index, notes);
    return id;
}

void setClip (Song& song, const ClipValue& clip)
{
    // What the song holds of it read, and what a clip takes of the value given set where it differs.
    if (const auto held = getClipValue (song, clip.id))
        setClip (song, *held, keepWithinClip (clip));
}

void setClip (Song& song, const ClipValue& held, const ClipValue& next)
{
    jassert (held.id == next.id);

    // The same value: nothing. One whose sound is alike but not the same value (ClipsValue.h: ClipSound) is set, so that an
    // alias group's clips can be given one, without a step of its own (SongValues.h: setClip()).
    if (held == next && held.sound.isSameAs (next.sound))
        return;

    auto* values = song.getValues();

    if (values == nullptr || values->findClip (held.id) == nullptr)
        return;

    // On another track only if it holds clips, at the end of its clips (one that holds none isn't one it goes on).
    auto value = next;

    if (value.track != held.track && ! holdsClips (song, value.track))
        value.track = held.track;

    values->setClip (song, std::move (value));
}

void eraseClip (Song& song, ClipID clip)
{
    auto* values = song.getValues();
    const auto* value = values != nullptr ? values->findClip (clip) : nullptr;

    if (value == nullptr)
        return;

    // The last one left of a group of linked clips leaves it, as Make Unique leaves it (makeClipsUnique()): it would still
    // show as linked to nothing.
    if (const auto* members = value->link != 0 ? values->clips.get().groups.find (value->link) : nullptr; members != nullptr)
        if (const auto group = *members; group.size() == 2)
            setClipLink (song, group[0] == clip ? group[1] : group[0], 0);

    values->eraseClip (song, clip);
}

void eraseTrackClips (Song& song, const std::vector<TrackID>& tracks)
{
    auto* values = song.getValues();

    if (values == nullptr)
        return;

    std::vector<ClipID> gone;

    for (const auto track : tracks)
        if (const auto* list = values->clips.get().tracks.find (track.getRaw()))
            gone.insert (gone.end(), list->begin(), list->end());

    if (! gone.empty())
        values->eraseClips (song, gone);
}

void takeClipsOf (Song& song, const juce::ValueTree& track)
{
    auto* values = song.getValues();
    const auto id = readTrackElementID (track);

    if (values == nullptr || ! track.hasType (schema::TRACK) || ! holdsClips (song, id))
        return;

    for (const auto& element : track)
    {
        if (! element.hasType (schema::CLIP))
            continue;

        auto value = keepWithinClip (readClipValue (element));
        value.track = id;

        if (! value.id.isValid())
            continue;

        // The sound of the audio alias group it joins, where another clip of it holds one alike: the group holds one.
        if (value.isAudio() && value.link != 0)
            if (const auto* members = values->clips.get().groups.find (value.link))
                for (const auto member : *members)
                    if (const auto* other = values->findClip (member); other != nullptr && member != value.id && other->isAudio())
                    {
                        if (other->sound == value.sound)
                            value.sound = other->sound;

                        break;
                    }

        // A MIDI clip's own notes, but where its group holds some (SongValues.h: setClip() gives it those).
        std::optional<ClipNotes> own;

        if (value.isMidi() && findGroupNotes (values->clips.get(), values->notes.get(), value.link, value.id) == nullptr)
            own = readClipNotes (element);

        values->setClip (song, std::move (value), -1, own.has_value() ? &*own : nullptr);
    }
}

std::optional<ClipValue> getClipValueAsSet (const Song& song, const ClipValue& clip)
{
    const auto* held = getSongClips (song).find (clip.id);

    if (held == nullptr)
        return std::nullopt;

    // What setClip() sets: what a clip takes of it, on its track where that holds clips, else on the one it is on.
    auto value = keepWithinClip (clip);
    value.track = holdsClips (song, clip.track) ? clip.track : held->track;
    return value;
}

void setClipLink (Song& song, ClipID clip, std::uint64_t link)
{
    changeClip (song, clip,
                [link] (ClipValue value)
                {
                    value.link = link;
                    return value;
                });
}

void copyTrackClips (Song& song, TrackID from, TrackID to, const std::map<std::uint64_t, std::uint64_t>& ids, bool asAliases)
{
    auto* values = song.getValues();

    if (values == nullptr || from == to || ! holdsClips (song, to))
        return;

    // Each clip of the track, as the area holds them now, in its order: its value on the copy, with its notes, shared.
    const auto clips = values->clips.get();
    const auto notes = values->notes.get();

    if (const auto* list = clips.tracks.find (from.getRaw()))
    {
        for (const auto old : *list)
        {
            const auto* clip = clips.find (old);

            if (clip == nullptr)
                continue;

            auto copy = *clip;
            const auto found = ids.find (old.getRaw());
            copy.id = ClipID::fromRaw (found != ids.end() ? found->second : song.makeID());
            copy.track = to;

            if (! asAliases)
                copy.link = 0;

            values->setClip (song, std::move (copy), -1, notes.find (old));
        }
    }
}

void setTrackClipOrder (Song& song, TrackID track, const std::vector<ClipID>& order)
{
    auto held = getClips (song, track), given = order;
    std::sort (held.begin(), held.end());
    std::sort (given.begin(), given.end());

    if (held != given || std::adjacent_find (given.begin(), given.end()) != given.end())
    {
        jassertfalse; // the track's clips, each once, in another order
        return;
    }

    if (auto* values = song.getValues(); values != nullptr && order != getClips (song, track))
        values->setTrackClipOrder (song, track, immer::flex_vector<ClipID> (order.begin(), order.end()));
}

BeatPosition getClipsEnd (const Song& song)
{
    // Every clip, whatever its track: a MIDI clip's place, an audio clip's range worked out.
    BeatPosition end;

    for (const auto& entry : getSongClips (song).clips)
    {
        const auto& clip = entry.second;
        end = std::max (end, clip.isAudio() ? getLiveClipRange (song, clip).getEnd() : clip.start + clip.length);
    }

    return end;
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
    // With one look for it: a MIDI clip's place, or an audio clip's range worked out from its value.
    if (const auto* value = getSongClips (song).find (clip))
        return getLiveClipRange (song, *value);

    return {};
}

juce::String getClipName (const Song& song, ClipID clip)
{
    const auto* value = getSongClips (song).find (clip);
    return value != nullptr ? getClipName (song, *value) : getTrackName (song, TrackID());
}

juce::String getClipName (const Song& song, const ClipValue& clip)
{
    return clip.name.isNotEmpty() ? clip.name : getTrackName (song, clip.track);
}

ClipValue makeMidiClip (Song& song, const juce::String& name, BeatRange range)
{
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
    // Each audio clip that plays the file (each of an alias group plays it: each is given the new path), track by track.
    std::vector<ClipValue> playing;
    forEachClip (song,
                 [&playing, &path] (const ClipValue& clip)
                 {
                     if (clip.isAudio() && clip.sound.file == path)
                         playing.push_back (clip);
                 });

    for (const auto& clip : playing)
    {
        auto next = clip;
        next.sound.file = newPath;
        setClip (song, clip, next);
    }

    return ! playing.empty();
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
    // The track's kind from its element, and its clips' kinds.
    if (! holdsClips (song, track))
        return false;

    const auto& clips = getSongClips (song);

    if (const auto* list = clips.tracks.find (track.getRaw()))
        for (const auto id : *list)
            if (const auto* clip = clips.find (id);
                clip != nullptr && clip->isAudio() == midi && std::find (leaving.begin(), leaving.end(), id) == leaving.end())
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

    if (const auto kind = getTrackKind (song, track); kind.has_value() && *kind != TrackValue::Kind::master && hasInstrument (song, track))
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
    // The groups as they are now, each left alone taken out of its group in turn.
    std::vector<ClipID> lone;

    for (const auto& group : getSongClips (song).groups)
        if (group.second.size() == 1)
            lone.push_back (group.second.front());

    for (const auto clip : lone)
        setClipLink (song, clip, 0);
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

//==============================================================================
namespace
{
    // Whether a path in the song (SongMerge.h: findByPath()) ends at a clip or goes through one.
    bool isPathThroughClip (const juce::StringArray& path)
    {
        const auto clipStep = schema::CLIP.toString() + ":";
        return std::any_of (path.begin(), path.end(), [&clipStep] (const juce::String& step) { return step.startsWith (clipStep); });
    }

    // The clip a path ends at, by the ID its last step names ("CLIP:<id>"), on the track the path names before it, as the song
    // holds it now: none for a path that ends elsewhere, or a clip the song hasn't there.
    std::optional<ClipID> findClipOnPath (const Song& song, const juce::StringArray& path)
    {
        const auto clipStep = schema::CLIP.toString() + ":";

        if (path.size() < 2 || ! path[path.size() - 1].startsWith (clipStep))
            return std::nullopt;

        const auto clip = ClipID::fromRaw ((std::uint64_t) path[path.size() - 1].substring (clipStep.length()).getLargeIntValue());
        auto trackPath = path;
        trackPath.remove (trackPath.size() - 1);
        // The track the path names, where the song holds it there (Tracks.h: findChannelOnPath()), none for no track there.
        const auto channel = findChannelOnPath (song, trackPath);
        const auto track = channel.has_value() && ! channel->onMaster ? channel->track : TrackID();

        if (! clip.isValid() || ! track.isValid() || getClipTrack (song, clip) != track)
            return std::nullopt;

        return clip;
    }

    // A clip's own property (its path's last step its type and ID, on the track the path names before it), set through the
    // clips' code; what the audio clips of an alias group share (AudioClips.h: shareTheSound()), which the merge reported
    // once, for the group's keeper, in every clip of the group, as an edit through one of them sets it. Never its ID or its
    // kind, which name and make it; nor a property that isn't a clip's. Whether it was set.
    bool chooseClipProperty (Song& song, ClipID clip, const MergeClash& clash, int value)
    {
        const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };
        const auto held = getClipValue (song, clip);

        // Refused: a clash on a clip's ID or its kind, on a property that isn't a clip's, or (chooseClipClash()) on an element
        // inside a clip other than a note. A merge of two songs Phazite holds makes none of them (a clip's ID names it and its
        // kind makes it; what a clip holds besides its notes merges as lists), but this is no assertion: a commit's song may
        // come from anyone who shares the Versions folder, and the fuzz test chooses every clash of a damaged merge
        // (tests/FuzzTests.cpp). The caller sees false, and the choice stays as it was.
        if (! held.has_value() || clash.property == schema::clipID || clash.property == schema::clipKind)
            return false;

        if (auto chosen = *held; ! setClipProperty (chosen, clash.property, *values[value]))
            return false;

        const auto shared = held->isAudio() && isSharedInAliasGroup (clash.property);
        std::vector<ClipValue> chosen;

        for (const auto each : shared ? getAliasClips (song, clip) : std::vector<ClipID> { clip })
        {
            auto given = getClipValue (song, each);

            if (! given.has_value())
                continue;

            setClipProperty (*given, clash.property, *values[value]);

            // Nothing written where a clip wouldn't hold the value chosen as it is (a start before the song's, a MIDI
            // clip's length under the shortest, a property of the other kind of clip's: Clips.h, getClipValueAsSet()):
            // what it would hold compared with it, set again, as the value compares (a number exactly, text exactly),
            // its length too, which an audio clip's value doesn't compare. The choice stays as it was.
            const auto asSet = getClipValueAsSet (song, *given);

            if (! asSet.has_value())
                return false;

            auto again = *asSet;
            setClipProperty (again, clash.property, *values[value]);

            if (! (again == *asSet) || again.length != asSet->length)
                return false;

            chosen.push_back (*given);
        }

        for (const auto& each : chosen)
            setClip (song, each);

        return true;
    }
} // namespace

AreaChoice chooseClipClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    if (clash.property.isValid())
        if (const auto clip = findClipOnPath (song, clash.path); clip.has_value())
            return chooseClipProperty (song, *clip, clash, value) ? AreaChoice::chosen : AreaChoice::refused;

    // Nothing else inside a clip has a property or an element a merge reports (its markers and controllers' values merge as
    // lists; what is merged whole is a curve, the chord lane, the key set last and a MIDI link, none of them a clip's): a
    // path through one names nothing to set or replace, refused as a clash on a clip's ID is.
    if (isPathThroughClip (clash.path))
        return AreaChoice::refused;

    return AreaChoice::notTheArea;
}
} // namespace phazite
