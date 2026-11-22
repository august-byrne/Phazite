// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/ClipsValue.h"
#include "internal/Schema.h"
#include "internal/SongText.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
bool ClipSound::isSameAs (const ClipSound& other) const noexcept
{
    return warps.identity() == other.warps.identity() && file == other.file && juce::exactlyEqual (tempo, other.tempo)
           && juce::exactlyEqual (pitch, other.pitch) && reversed == other.reversed;
}

bool ClipSound::operator== (const ClipSound& other) const
{
    return file == other.file && juce::exactlyEqual (tempo, other.tempo) && juce::exactlyEqual (pitch, other.pitch)
           && reversed == other.reversed && (warps.identity() == other.warps.identity() || warps == other.warps);
}

bool ClipValue::operator== (const ClipValue& other) const
{
    // An audio clip's length is none of its value: it has none of its own (its range follows from its part).
    return id == other.id && kind == other.kind && track == other.track && name == other.name && start == other.start
           && (isAudio() || length == other.length) && offset == other.offset && muted == other.muted && link == other.link
           && sound == other.sound && juce::exactlyEqual (fileStart, other.fileStart) && juce::exactlyEqual (fileEnd, other.fileEnd)
           && juce::exactlyEqual (gain, other.gain) && fadeIn == other.fadeIn && fadeOut == other.fadeOut && takes == other.takes
           && take == other.take && juce::exactlyEqual (takeLength, other.takeLength);
}

//==============================================================================
namespace
{
    using ClipList = immer::flex_vector<ClipID>;
    using ClipLists = immer::map<std::uint64_t, ClipList>;

    // A list without a clip (the same list for one it hasn't); and the lists without it under a key, the key gone with the
    // last clip.
    ClipList without (const ClipList& list, ClipID clip)
    {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == clip)
                return list.erase (i);

        return list;
    }

    ClipLists withoutIn (const ClipLists& lists, std::uint64_t key, ClipID clip)
    {
        if (key == 0)
            return lists;

        if (const auto* list = lists.find (key))
        {
            const auto rest = without (*list, clip);
            return rest.empty() ? lists.erase (key) : lists.set (key, rest);
        }

        return lists;
    }

    // The lists with a clip under a key, at a place (or after the others).
    ClipLists withIn (const ClipLists& lists, std::uint64_t key, ClipID clip, int index)
    {
        if (key == 0)
            return lists;

        const auto* found = lists.find (key);
        auto list = found != nullptr ? *found : ClipList();
        const auto place = index >= 0 ? std::min ((size_t) index, list.size()) : list.size();
        return lists.set (key, list.insert (place, clip));
    }
} // namespace

SongClips withClip (const SongClips& clips, ClipValue clip, int index)
{
    jassert (clip.id.isValid());
    auto next = clips;
    const auto* old = clips.find (clip.id);

    if (old == nullptr || old->track != clip.track)
    {
        if (old != nullptr)
            next.tracks = withoutIn (next.tracks, old->track.getRaw(), clip.id);

        next.tracks = withIn (next.tracks, clip.track.getRaw(), clip.id, index);
    }

    if (old == nullptr || old->link != clip.link)
    {
        if (old != nullptr)
            next.groups = withoutIn (next.groups, old->link, clip.id);

        next.groups = withIn (next.groups, clip.link, clip.id, -1);
    }

    const auto key = clip.id.getRaw(); // read before the value is moved: a call's arguments come in no set order
    next.clips = next.clips.set (key, std::move (clip));
    return next;
}

SongClips withoutClip (const SongClips& clips, ClipID id)
{
    const auto* old = clips.find (id);

    if (old == nullptr)
        return clips;

    auto next = clips;
    next.tracks = withoutIn (next.tracks, old->track.getRaw(), id);
    next.groups = withoutIn (next.groups, old->link, id);
    next.clips = next.clips.erase (id.getRaw());
    return next;
}

SongClips withTrackOrder (const SongClips& clips, TrackID track, immer::flex_vector<ClipID> order)
{
    const auto* list = clips.tracks.find (track.getRaw());
    const auto now = list != nullptr ? *list : ClipList();
    std::vector<ClipID> held (now.begin(), now.end()), given (order.begin(), order.end());
    std::sort (held.begin(), held.end());
    std::sort (given.begin(), given.end());

    if (held != given)
    {
        jassertfalse; // the same clips, in another order
        return clips;
    }

    if (order.empty() || std::equal (order.begin(), order.end(), now.begin(), now.end()))
        return clips; // the order it holds: the same value

    auto next = clips;
    next.tracks = next.tracks.set (track.getRaw(), std::move (order));
    return next;
}

bool holdsItsLists (const SongClips& clips)
{
    // Each list's clips: of its track or group, each once in it.
    const auto listsHold = [&clips] (const ClipLists& lists, bool byTrack, size_t& count)
    {
        for (const auto& [key, list] : lists)
        {
            if (list.empty())
                return false;

            std::vector<ClipID> seen (list.begin(), list.end());
            std::sort (seen.begin(), seen.end());

            if (std::adjacent_find (seen.begin(), seen.end()) != seen.end())
                return false;

            for (const auto& id : seen)
            {
                const auto* clip = clips.find (id);

                if (clip == nullptr || (byTrack ? clip->track.getRaw() : clip->link) != key)
                    return false;
            }

            count += list.size();
        }

        return true;
    };

    size_t onTracks = 0, inGroups = 0, expectedOnTracks = 0, expectedInGroups = 0;

    if (! listsHold (clips.tracks, true, onTracks) || ! listsHold (clips.groups, false, inGroups))
        return false;

    for (const auto& [id, clip] : clips.clips)
    {
        if (id != clip.id.getRaw())
            return false;

        expectedOnTracks += clip.track.isValid() ? size_t { 1 } : size_t { 0 };
        expectedInGroups += clip.link != 0 ? size_t { 1 } : size_t { 0 };
    }

    return onTracks == expectedOnTracks && inGroups == expectedInGroups;
}

bool hasGroupsOfOne (const SongClips& clips)
{
    return std::any_of (clips.groups.begin(), clips.groups.end(), [] (const auto& group) { return group.second.size() == 1; });
}

//==============================================================================
namespace
{
    // One property of a clip's element read into its value, as its description reads it (readDescribed(): a void value, as
    // if the element hadn't it, reads as the description's fallback, else as none: 0, false, empty; but the take, 1; and a
    // number outside its range is kept within it): whether it is a clip's property.
    bool readInto (ClipValue& value, const juce::Identifier& property, const juce::var& held)
    {
        if (property == schema::clipID)
            value.id = ClipID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) held));
        else if (property == schema::clipKind)
            value.kind = held.toString() == schema::clipIsAudio ? ClipValue::Kind::audio : ClipValue::Kind::midi;
        else if (property == schema::clipName)
            value.name = held.toString();
        else if (property == schema::clipStart)
            value.start = BeatPosition::fromBeats (readDescribed (held, schema::clipStart));
        else if (property == schema::clipLength)
            value.length = BeatDuration::fromBeats (readDescribed (held, schema::clipLength));
        else if (property == schema::clipOffset)
            value.offset = BeatDuration::fromBeats (readDescribed (held, schema::clipOffset));
        else if (property == schema::clipMute)
            value.muted = (bool) held;
        else if (property == schema::clipLink)
            value.link = (std::uint64_t) std::max<juce::int64> (0, (juce::int64) held);
        else if (property == schema::clipFile)
            value.sound.file = held.toString();
        else if (property == schema::clipFileStart)
            value.fileStart = readDescribed (held, schema::clipFileStart);
        else if (property == schema::clipFileEnd)
            value.fileEnd = readDescribed (held, schema::clipFileEnd);
        else if (property == schema::clipTempo)
            value.sound.tempo = held.isVoid() ? 0.0 : readDescribed (held, schema::clipTempo);
        else if (property == schema::clipPitch)
            value.sound.pitch = (float) readDescribed (held, schema::clipPitch);
        else if (property == schema::clipReversed)
            value.sound.reversed = (bool) held;
        else if (property == schema::clipGain)
            value.gain = (float) readDescribed (held, schema::clipGain);
        else if (property == schema::clipFadeIn)
            value.fadeIn.seconds = readDescribed (held, schema::clipFadeIn);
        else if (property == schema::clipFadeOut)
            value.fadeOut.seconds = readDescribed (held, schema::clipFadeOut);
        else if (property == schema::clipFadeInShape)
            value.fadeIn.sCurve = held.toString() == schema::fadeIsSCurve;
        else if (property == schema::clipFadeOutShape)
            value.fadeOut.sCurve = held.toString() == schema::fadeIsSCurve;
        else if (property == schema::clipFadeInBend)
            value.fadeIn.bend = (float) readDescribed (held, schema::clipFadeInBend);
        else if (property == schema::clipFadeOutBend)
            value.fadeOut.bend = (float) readDescribed (held, schema::clipFadeOutBend);
        else if (property == schema::clipTakes)
            value.takes = held.isVoid() ? 0 : (int) readDescribed (held, schema::clipTakes);
        else if (property == schema::clipTake)
            value.take = held.isVoid() ? 1 : (int) readDescribed (held, schema::clipTake);
        else if (property == schema::clipTakeLength)
            value.takeLength = held.isVoid() ? 0.0 : readDescribed (held, schema::clipTakeLength);
        else
            return false;

        return true;
    }

    // A clip's value with nothing of the other kind of clip's properties (each at its default): a MIDI clip has no sound, part of
    // a file, gain, fades or takes; an audio clip no length or offset of its own.
    ClipValue ofItsKind (ClipValue value)
    {
        if (value.isAudio())
        {
            value.length = {};
            value.offset = {};
            return value;
        }

        const ClipValue none;
        value.sound = none.sound;
        value.fileStart = none.fileStart;
        value.fileEnd = none.fileEnd;
        value.gain = none.gain;
        value.fadeIn = none.fadeIn;
        value.fadeOut = none.fadeOut;
        value.takes = none.takes;
        value.take = none.take;
        value.takeLength = none.takeLength;
        return value;
    }
} // namespace

ClipValue readClipValue (const juce::ValueTree& clip)
{
    // Each property the element holds; those it hasn't, as their descriptions read none (a value's defaults are those: a
    // MIDI clip's length of 4 beats, a start of 0, a take of 1); and its track, where it is.
    ClipValue value;

    for (int i = 0; i < clip.getNumProperties(); ++i)
    {
        const auto name = clip.getPropertyName (i);
        readInto (value, name, clip.getProperty (name));
    }

    // Nothing of the other kind of clip's: an audio clip has no length of its own (its part of its file and its tempo give its
    // range), a MIDI clip no file.
    value = ofItsKind (std::move (value));

    if (const auto parent = clip.getParent(); parent.hasType (schema::TRACK))
        value.track = TrackID::fromRaw ((std::uint64_t) (juce::int64) parent[schema::trackID]);

    // An audio clip's markers; a clip without any shares the one empty vector.
    if (value.isAudio() && clip.getChildWithName (schema::WARP).isValid())
    {
        auto warps = immer::flex_vector<WarpValue>().transient();

        for (const auto& child : clip)
            if (child.hasType (schema::WARP))
                warps.push_back ({ readDescribed (child, schema::warpTime), readDescribed (child, schema::warpPlaysAt) });

        value.sound.warps = warps.persistent();
    }

    return value;
}

juce::ValueTree writeClipElement (const ClipValue& value, const ClipNotes* notes)
{
    // Each property of the clip's kind, only while it is set:
    //
    //   property                       kind   written
    //   clipID, clipKind, clipStart    both   always
    //   clipName                       both   while not empty
    //   clipLength                     MIDI   always
    //   clipOffset                     MIDI   while more than 0
    //   clipMute                       both   while on
    //   clipLink                       both   while not 0
    //   clipFile                       audio  always
    //   clipFileStart, clipFileEnd     audio  always
    //   clipTempo                      audio  while more than 0
    //   clipPitch                      audio  while not 0
    //   clipReversed                   audio  while on
    //   clipGain                       audio  while not 0
    //   clipFadeIn, clipFadeOut        audio  while more than 0
    //   clipFadeInShape, ...OutShape   audio  while an S-curve
    //   clipFadeInBend, ...OutBend     audio  while not 0
    //   clipTakes, clipTake            audio  while it has takes
    //   clipTakeLength                 audio  while it has takes, and is more than 0
    //   WARP children                  audio  each marker, in the value's order
    juce::ValueTree clip (schema::CLIP);
    const auto audio = value.isAudio();
    const auto set = [&clip] (bool held, const juce::Identifier& name, const juce::var& written)
    {
        if (held)
            clip.setProperty (name, written, nullptr);
    };

    clip.setProperty (schema::clipID, (juce::int64) value.id.getRaw(), nullptr);
    clip.setProperty (schema::clipKind, juce::String (audio ? schema::clipIsAudio : schema::clipIsMidi), nullptr);
    set (value.name.isNotEmpty(), schema::clipName, value.name);
    clip.setProperty (schema::clipStart, value.start.inBeats(), nullptr);
    set (! audio, schema::clipLength, value.length.inBeats());
    set (! audio && value.offset.inBeats() > 0.0, schema::clipOffset, value.offset.inBeats());
    set (value.muted, schema::clipMute, true);
    set (value.link != 0, schema::clipLink, (juce::int64) value.link);

    if (audio)
    {
        const auto writeFade =
            [&set] (const ClipFade& fade, const juce::Identifier& seconds, const juce::Identifier& shape, const juce::Identifier& bend)
        {
            set (fade.seconds > 0.0, seconds, fade.seconds);
            set (fade.sCurve, shape, juce::String (schema::fadeIsSCurve));
            set (! juce::exactlyEqual (fade.bend, 0.0f), bend, fade.bend);
        };

        clip.setProperty (schema::clipFile, value.sound.file, nullptr);
        clip.setProperty (schema::clipFileStart, value.fileStart, nullptr);
        clip.setProperty (schema::clipFileEnd, value.fileEnd, nullptr);
        set (value.sound.tempo > 0.0, schema::clipTempo, value.sound.tempo);
        set (! juce::exactlyEqual (value.sound.pitch, 0.0f), schema::clipPitch, value.sound.pitch);
        set (value.sound.reversed, schema::clipReversed, true);
        set (! juce::exactlyEqual (value.gain, 0.0f), schema::clipGain, value.gain);
        writeFade (value.fadeIn, schema::clipFadeIn, schema::clipFadeInShape, schema::clipFadeInBend);
        writeFade (value.fadeOut, schema::clipFadeOut, schema::clipFadeOutShape, schema::clipFadeOutBend);
        set (value.takes > 0, schema::clipTakes, value.takes);
        set (value.takes > 0, schema::clipTake, value.take);
        set (value.takes > 0 && value.takeLength > 0.0, schema::clipTakeLength, value.takeLength);

        for (const auto& warp : value.sound.warps)
        {
            juce::ValueTree marker (schema::WARP);
            marker.setProperty (schema::warpTime, warp.time, nullptr);
            marker.setProperty (schema::warpPlaysAt, warp.playsAt, nullptr);
            clip.appendChild (marker, nullptr);
        }
    }

    if (notes != nullptr && ! audio)
        writeClipNotes (*notes, clip);

    return clip;
}

ClipValue keepWithinClip (ClipValue value)
{
    value = ofItsKind (std::move (value));
    value.start = BeatPosition::fromBeats (std::max (0.0, value.start.inBeats()));

    if (value.isMidi())
        value.length = BeatDuration::fromBeats (std::max (value.length.inBeats(), shortestClipBeats));

    value.fileStart = std::max (0.0, value.fileStart);
    value.fileEnd = std::max (value.fileStart, value.fileEnd);
    value.fadeIn.bend = juce::jlimit (-1.0f, 1.0f, value.fadeIn.bend);
    value.fadeOut.bend = juce::jlimit (-1.0f, 1.0f, value.fadeOut.bend);

    // Its takes: none, or the one it plays among them (what a file holds of them is written only with a count: ClipsValue.h,
    // writeClipElement()).
    if (value.takes <= 0)
    {
        const ClipValue none;
        value.takes = none.takes;
        value.take = none.take;
        value.takeLength = none.takeLength;
    }
    else
    {
        value.take = juce::jlimit (1, value.takes, value.take);
    }

    return value;
}

bool setClipProperty (ClipValue& value, const juce::Identifier& property, const juce::var& given)
{
    return readInto (value, property, given);
}
} // namespace phazite
