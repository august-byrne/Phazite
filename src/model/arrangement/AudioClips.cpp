// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioClips.h"
#include "AudioClipModel.h"
#include "internal/AudioWork.h"
#include "internal/Clips.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace phazite
{
// Every edit here works on clips' values (ClipsValue.h: ClipValue): it reads each clip it changes once (Clips.h:
// getClipValue()), works the next value out, and sets it (Clips.h: setClip()); what an alias group's audio clips share, their
// sound (ClipsValue.h: ClipSound), is given to each of them as one value. The functions that take a clip's element read it
// into a value and ask the value's.
namespace
{
    // Bumped whenever renders would come out differently, so that a project's old ones aren't played.
    constexpr const char* renderVersion = "rubberband-4.0-r3-1";

    stretch::Settings getSettings (const ClipSound& sound, double songTempo)
    {
        return { sound.tempo > 0.0 && songTempo > 0.0 ? sound.tempo / songTempo : 1.0, (double) sound.pitch, sound.reversed };
    }

    double getFileSeconds (const juce::File& file)
    {
        return getAudioFileInfo (file).getSeconds();
    }

    // An audio clip's value: none for a clip that isn't an audio clip of the song's.
    std::optional<ClipValue> findAudioClip (const Song& song, ClipID clip)
    {
        auto value = getClipValue (song, clip);
        return value.has_value() && value->isAudio() ? value : std::nullopt;
    }

    // What an audio clip's value is, for one that isn't there: as an element without any of its properties would read (an
    // invalid file, its start and part at 0), which is what the functions by ID below have always worked out for it.
    ClipValue getNoAudioClip()
    {
        ClipValue none;
        none.kind = ClipValue::Kind::audio;
        none.length = {};
        return none;
    }

    // Gives a clip and each other clip of its alias group (which play the same audio) one sound, the same value in each
    // (ClipsValue.h: ClipSound), keeping their starts and the parts of their files they play.
    void giveSound (Song& song, const ClipValue& clip, const ClipSound& sound)
    {
        for (const auto alias : getAliasClips (song, clip.id))
        {
            const auto held = alias == clip.id ? std::optional<ClipValue> (clip) : getClipValue (song, alias);

            if (held.has_value() && held->isAudio())
            {
                auto next = *held;
                next.sound = sound;
                setClip (song, *held, next);
            }
        }
    }

    // The tempo a file was played at: from its loop data, or else the song's where the clip starts, so that it sounds as it
    // does until either tempo changes.
    double getFileTempo (const Song& song, BeatPosition start, const juce::File& file)
    {
        const auto info = getAudioFileInfo (file);
        const auto seconds = info.getSeconds();
        const auto tempo = seconds > 0.0 && info.loopBeats > 0.0 ? info.loopBeats / seconds * 60.0 : getTempoAt (song, start);
        return juce::jlimit (minClipTempo, maxClipTempo, tempo);
    }

    // A line through points in order, straight from each to the next, from each point's first to its second (or, inverse,
    // the other way round), and straight on at a slope of 1 before the first and after the last: the same without points.
    double alongPoints (const std::vector<std::pair<double, double>>& points, double x, bool inverse)
    {
        if (points.empty())
            return x;

        const auto from = [inverse] (const std::pair<double, double>& point) { return inverse ? point.second : point.first; };
        const auto to = [inverse] (const std::pair<double, double>& point) { return inverse ? point.first : point.second; };

        if (x <= from (points.front()))
            return to (points.front()) + (x - from (points.front()));

        if (x >= from (points.back()))
            return to (points.back()) + (x - from (points.back()));

        const auto next = std::upper_bound (points.begin(), points.end(), x, [&from] (double value, const std::pair<double, double>& point)
                                            { return value < from (point); });
        const auto& a = *std::prev (next);
        const auto& b = *next;
        return to (a) + (x - from (a)) / (from (b) - from (a)) * (to (b) - to (a));
    }

    // A clip's warp markers as the song holds them (Schema.h: WARP), each a time of its file as recorded with where that
    // plays straight at the clip's tempo, in time order, both rising (any that doesn't is left out); none while the clip
    // doesn't follow the song's tempo.
    std::vector<std::pair<double, double>> getWarpPoints (const ClipValue& clip)
    {
        std::vector<std::pair<double, double>> points;

        if (clip.sound.tempo <= 0.0)
            return points;

        for (const auto& warp : clip.sound.warps)
            points.emplace_back (warp.time, warp.playsAt);

        std::sort (points.begin(), points.end());
        std::vector<std::pair<double, double>> rising;

        for (const auto& point : points)
            if (rising.empty() || (point.first > rising.back().first && point.second > rising.back().second))
                rising.push_back (point);

        return rising;
    }

    // A clip's sound with these warp markers in place of its own, in their order.
    ClipSound withWarpPoints (ClipSound sound, const std::vector<std::pair<double, double>>& points)
    {
        auto warps = immer::flex_vector<WarpValue>().transient();

        for (const auto& [at, playsAt] : points)
            warps.push_back ({ at, playsAt });

        sound.warps = points.empty() ? immer::flex_vector<WarpValue>() : warps.persistent();
        return sound;
    }

    // A clip's warp markers, in each clip of its alias group (which play the same audio), in place of those there.
    void writeWarpPoints (Song& song, const ClipValue& clip, const std::vector<std::pair<double, double>>& points)
    {
        giveSound (song, clip, withWarpPoints (clip.sound, points));
    }

    // How a clip's played sound (its file, backwards while the clip plays backwards) lines up with the song: where its part
    // starts in it and how long that is, in seconds, the file's whole length, the beat the clip starts at, the file's tempo
    // while the clip follows the song's, and its warp markers in the sound as it plays. Following it, a time in the sound is a
    // beat of the song, at the file's tempo from where the clip starts, whatever the song's tempo does there; between two
    // markers, the sound stretches to fit, and before the first and after the last it plays straight.
    struct ClipTime
    {
        double partStart = 0.0, partLength = 0.0, fileLength = 0.0;
        double startBeat = 0.0;
        double tempo = 0.0;                              // the file's, while the clip follows the song's; 0 while it doesn't
        std::vector<std::pair<double, double>> warps {}; // each a time of the sound with where it plays straight

        double unwarp (double time) const { return alongPoints (warps, time, false); } // where it plays straight
        double warp (double straight) const { return alongPoints (warps, straight, true); }
        double beatAt (double time) const { return startBeat + (unwarp (time) - unwarp (partStart)) * tempo / 60.0; }
        double timeAt (double beat) const { return warp (unwarp (partStart) + (beat - startBeat) * 60.0 / tempo); }
    };

    ClipTime getClipTime (const Song& song, const ClipValue& clip)
    {
        const auto part = getFilePart (clip);
        const auto length = getClipFileLength (song, clip);
        const auto reversed = clip.sound.reversed;
        ClipTime time { reversed ? length - part.end : part.start, part.end - part.start, length, clip.start.inBeats(), clip.sound.tempo };
        time.warps = getWarpPoints (clip);

        // Backwards, the sound's time runs from the file's end: each marker at the same audio, mirrored, each stretch as long.
        if (reversed && ! time.warps.empty())
        {
            const auto end = alongPoints (time.warps, length, false);

            for (auto& [at, playsAt] : time.warps)
            {
                at = length - at;
                playsAt = end - playsAt;
            }

            std::reverse (time.warps.begin(), time.warps.end());
        }

        return time;
    }

    // How the clip lines up with the song while it follows the song's tempo along warp markers, or where the tempo changes
    // under the clip's file, from the file's start to its end as the clip places them: then it plays along the markers and
    // the changes, rather than at one ratio as it does under one tempo. Nothing otherwise, found without reading the file for a
    // clip that keeps its speed, or one without markers in a song of one tempo.
    std::optional<ClipTime> findMappedTime (const Song& song, const ClipValue& clip, const ClipTempo& tempo)
    {
        if (clip.sound.tempo <= 0.0)
            return std::nullopt;

        if (clip.sound.warps.empty() && tempo.changes.size() <= 1)
            return std::nullopt;

        const auto time = getClipTime (song, clip);

        if (time.fileLength <= 0.0 || (time.warps.empty() && tempo.map.isSteadyBetween (time.beatAt (0.0), time.beatAt (time.fileLength))))
            return std::nullopt;

        return time;
    }

    std::optional<ClipTime> findMappedTime (const Song& song, const ClipValue& clip)
    {
        // Without making the tempo map for a clip that keeps its speed, or one without markers in a song of one tempo.
        if (clip.sound.tempo <= 0.0 || (clip.sound.warps.empty() && ! hasTempoChanges (song)))
            return std::nullopt;

        return findMappedTime (song, clip, ClipTempo (song));
    }

    std::vector<std::pair<double, double>> makeTimeMap (const ClipTempo&, const ClipTime&); // below

    // How it plays its file (getClipStretch()), with the song's tempo read already and how it lines up along markers and
    // the tempo's changes found (findMappedTime()).
    stretch::Settings getStretch (const ClipValue& clip, const ClipTempo& tempo, const std::optional<ClipTime>& mapped)
    {
        auto settings = getSettings (clip.sound, tempo.map.getTempoAt (clip.start.inBeats()));

        // Along its warp markers and the song's tempo changes, the stretch follows each of them, its ratio being the whole file's.
        if (mapped.has_value())
        {
            settings.timeMap = makeTimeMap (tempo, *mapped);
            settings.timeRatio = settings.timeMap.back().second / mapped->fileLength;
        }

        return settings;
    }

    // Where it plays (getPlayedPosition()), with the same, and its stretch worked out.
    PlayedPosition getPosition (const Song& song, const ClipValue& clip, const ClipTempo& tempo, const std::optional<ClipTime>& mapped,
                                const stretch::Settings& settings)
    {
        const auto start = tempo.map.toSamples (clip.start.inBeats());

        // Along its markers and the song's tempo changes: where its part's end plays, and how far into the render its part's
        // start is.
        if (const auto& time = mapped)
            return { start, tempo.map.toSamples (time->beatAt (time->partStart + time->partLength)),
                     start - tempo.map.toSamples (time->beatAt (0.0)) };

        const auto part = getFilePart (clip);
        const auto from = settings.reversed ? getFileSeconds (getClipFile (song, clip)) - part.end : part.start;
        return { start, start + (part.end - part.start) * settings.timeRatio, from * settings.timeRatio };
    }

    // Its fades as they play (getFittedFades()), where it plays.
    std::pair<double, double> getFades (const ClipValue& clip, const PlayedPosition& position)
    {
        const auto length = position.end - position.start;
        auto in = juce::jlimit (0.0, length, clip.fadeIn.seconds);
        auto out = juce::jlimit (0.0, length, clip.fadeOut.seconds);

        if (in + out > length && in + out > 0.0)
        {
            const auto scale = length / (in + out);
            in *= scale;
            out *= scale;
        }

        return { in, out };
    }

    // Its stretch along its markers and the tempo's changes: at every half beat of the sound, at each marker and where the
    // song's tempo changes, the time in the sound with where it plays in the render, which starts where the sound's start
    // plays.
    std::vector<std::pair<double, double>> makeTimeMap (const ClipTempo& tempo, const ClipTime& time)
    {
        const auto& map = tempo.map; // in seconds
        const auto origin = map.toSamples (time.beatAt (0.0));
        const auto step = 30.0 / time.tempo; // half a beat of the sound, played straight
        const auto first = time.unwarp (0.0), last = time.unwarp (time.fileLength);
        std::vector<double> times;

        for (int each = 1; first + each * step < last; ++each)
            times.push_back (time.warp (first + each * step));

        for (const auto& change : tempo.changes)
            if (const auto at = time.timeAt (change.beat.inBeats()); at > 0.0 && at < time.fileLength)
                times.push_back (at);

        for (const auto& warp : time.warps)
            if (warp.first > 0.0 && warp.first < time.fileLength)
                times.push_back (warp.first);

        times.push_back (time.fileLength);
        std::sort (times.begin(), times.end());
        times.erase (std::unique (times.begin(), times.end(), [] (double a, double b) { return b - a < 1.0e-6; }), times.end());

        std::vector<std::pair<double, double>> timeMap;

        for (const auto at : times)
            timeMap.emplace_back (at, map.toSamples (time.beatAt (at)) - origin);

        return timeMap;
    }
} // namespace

//==============================================================================
bool isAudioState (const juce::ValueTree& clipState)
{
    return clipState[schema::clipKind].toString() == schema::clipIsAudio;
}

ClipValue makeAudioClip (Song& song, const juce::String& name, const juce::File& file, BeatPosition start, FilePart part)
{
    ClipValue clip;
    clip.id = ClipID::fromRaw (song.makeID());
    clip.kind = ClipValue::Kind::audio;
    clip.length = {};
    clip.name = name;
    clip.start = BeatPosition::fromBeats (std::max (0.0, start.inBeats()));
    // Relative to the project's folder, as devices keep their files (DeviceFiles.h), written the same on every system.
    clip.sound.file = getProjectPath (song, file);
    clip.fileStart = part.start;
    clip.fileEnd = part.end;
    return clip;
}

ClipTempo::ClipTempo (const Song& song)
    : map (getTempoMap (song)),
      changes (getTempoChanges (song))
{
}

//==============================================================================
juce::File getClipFile (const Song& song, const ClipValue& clip)
{
    return findProjectFile (song, clip.sound.file);
}

double getClipFileLength (const Song& song, const ClipValue& clip)
{
    return getFileSeconds (getClipFile (song, clip));
}

stretch::Settings getClipStretch (const Song& song, const ClipValue& clip)
{
    const ClipTempo tempo (song);
    return getStretch (clip, tempo, findMappedTime (song, clip, tempo));
}

bool followsTempoChanges (const Song& song, const ClipValue& clip)
{
    return clip.isAudio() && findMappedTime (song, clip).has_value();
}

double getFileSecondsBetween (const Song& song, const ClipValue& clip, double from, BeatPosition to)
{
    // Along markers and tempo changes, the clip's beats are its sound's, at its file's tempo between its markers.
    if (const auto time = findMappedTime (song, clip))
        return time->timeAt (to.inBeats()) - time->timeAt (toBeats (song, from).inBeats());

    return (toSeconds (song, to) - from) / getClipStretch (song, clip).timeRatio;
}

double getPartStartFor (const Song& song, const ClipValue& clip, double offset)
{
    // The sound's start would play at the song's time so far before the clip's start: its part starts where that puts it.
    if (const auto time = findMappedTime (song, clip))
    {
        const auto start = toSeconds (song, BeatPosition::fromBeats (time->startBeat));
        const auto soundStart = toBeats (song, start - offset).inBeats();
        return time->warp (time->unwarp (0.0) + (time->startBeat - soundStart) * 60.0 / time->tempo);
    }

    return offset / getClipStretch (song, clip).timeRatio;
}

juce::File getPlayedFile (const Song& song, const ClipValue& clip)
{
    const auto file = getClipFile (song, clip);

    if (const auto settings = getClipStretch (song, clip); ! stretch::isUnchanged (settings))
        return getRenderFile (song, file, settings);

    return file;
}

PlayedPosition getPlayedPosition (const Song& song, const ClipValue& clip)
{
    const ClipTempo tempo (song);
    const auto mapped = findMappedTime (song, clip, tempo);
    return getPosition (song, clip, tempo, mapped, getStretch (clip, tempo, mapped));
}

PlayedClip getPlayedClip (const Song& song, const ClipValue& clip, const ClipTempo& tempo)
{
    // Each worked out once, as getPlayedFile(), getPlayedPosition() and getFittedFades() work it out.
    const auto mapped = findMappedTime (song, clip, tempo);
    const auto settings = getStretch (clip, tempo, mapped);
    const auto file = getClipFile (song, clip);
    PlayedClip played { stretch::isUnchanged (settings) ? file : getRenderFile (song, file, settings),
                        getPosition (song, clip, tempo, mapped, settings) };
    std::tie (played.fadeIn, played.fadeOut) = getFades (clip, played.position);
    return played;
}

BeatRange getAudioClipRange (const Song& song, const ClipValue& clip)
{
    return { clip.start, toBeats (song, getPlayedPosition (song, clip).end) };
}

engine::FadeCurve toEngineFade (FadeCurve curve)
{
    return { curve.shape == FadeShape::sCurve ? engine::FadeShape::sCurve : engine::FadeShape::curve, curve.bend };
}

//==============================================================================
bool isAudioClip (const Song& song, ClipID clip)
{
    // The clip's kind alone, where the song keeps it (SongValues.h: the clips' area).
    const auto* values = song.getValues();
    const auto* value = values != nullptr ? values->findClip (clip) : nullptr;
    return value != nullptr && value->isAudio();
}

FilePart getFilePart (const ClipValue& clip)
{
    return { clip.fileStart, clip.fileEnd };
}

FadeCurve getFadeCurve (const ClipFade& fade)
{
    return { fade.sCurve ? FadeShape::sCurve : FadeShape::curve, juce::jlimit (-1.0f, 1.0f, fade.bend) };
}

juce::File getClipFile (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? getClipFile (song, *value) : juce::File();
}

double getClipFileLength (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? getClipFileLength (song, *value) : 0.0;
}

bool followsTempoChanges (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() && followsTempoChanges (song, *value);
}

juce::File getPlayedFile (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? getPlayedFile (song, *value) : juce::File();
}

PlayedPosition getPlayedPosition (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? getPlayedPosition (song, *value) : PlayedPosition();
}

std::pair<double, double> getFittedFades (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? getFades (*value, getPlayedPosition (song, *value)) : std::pair<double, double>();
}

SecondsRange getAudioFileRange (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? getAudioFileRange (song, *value) : SecondsRange();
}

stretch::Settings getClipStretch (const Song& song, ClipID clip)
{
    if (const auto value = findAudioClip (song, clip))
        return getClipStretch (song, *value);

    return {};
}

bool isRendered (const Song& song, ClipID clip)
{
    return ! stretch::isUnchanged (getClipStretch (song, clip));
}

juce::File getRenderFile (const Song& song, const juce::File& source, const stretch::Settings& settings)
{
    // Written into a stream, which grows by doubling: a String appended to point by point copies the whole key each time,
    // which took a tenth of a second of the message thread at each plan for a clip with thousands of warp markers.
    // The source by its identity on the disk, as last asked (at most once a second: model/engine/AudioFiles.h): a source
    // written again in its place asks for a render of its own, and the old one is asked for no more (known bug 198).
    juce::MemoryOutputStream key;
    key << getFileIdentity (source) << "|" << juce::String (settings.timeRatio, 9) << "|" << juce::String (settings.semitones, 4) << "|"
        << (settings.reversed ? "backwards" : "forwards") << "|" << renderVersion;

    // Along tempo changes, the whole stretch: two clips under the same changes share it.
    for (const auto& [in, out] : settings.timeMap)
        key << "|" << juce::String (in, 6) << ":" << juce::String (out, 6);

    return song.getCacheFolder().getChildFile (renderFilePrefix + juce::String::toHexString (key.toString().hashCode64()) + ".wav");
}

SecondsRange getAudioFileRange (const Song& song, const ClipValue& clip)
{
    if (const auto time = findMappedTime (song, clip))
    {
        const auto map = getTempoMap (song);
        return { map.toSamples (time->beatAt (0.0)), map.toSamples (time->beatAt (time->fileLength)) };
    }

    const auto position = getPlayedPosition (song, clip);
    const auto start = position.start - position.offset;
    return { start, start + getClipFileLength (song, clip) * getClipStretch (song, clip).timeRatio };
}

double getFileTimeAt (const Song& song, ClipID clip, BeatPosition beat)
{
    const auto found = findAudioClip (song, clip);
    const auto value = found.has_value() ? *found : getNoAudioClip();

    // Along markers and tempo changes, the sound's time is the song's beat, at the file's tempo between its markers.
    if (const auto time = findMappedTime (song, value))
    {
        const auto into = time->timeAt (beat.inBeats());
        return value.sound.reversed ? time->fileLength - into : into;
    }

    const auto position = getPlayedPosition (song, value);
    const auto settings = getClipStretch (song, value);
    const auto intoFile = (toSeconds (song, beat) - (position.start - position.offset)) / settings.timeRatio;
    return settings.reversed ? getClipFileLength (song, value) - intoFile : intoFile;
}

double getClipTempo (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? value->sound.tempo : 0.0;
}

float getClipPitch (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? value->sound.pitch : 0.0f;
}

bool isClipReversed (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() && value->sound.reversed;
}

float getClipGain (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? value->gain : 0.0f;
}

void setClipGain (Song& song, ClipID clip, float decibels)
{
    if (const auto held = findAudioClip (song, clip))
    {
        auto next = *held;
        next.gain = juce::jlimit (minClipGain, maxClipGain, decibels);
        setClip (song, *held, next);
    }
}

void setClipPitch (Song& song, ClipID clip, float semitones)
{
    const auto pitch = juce::jlimit (-maxClipPitch, maxClipPitch, semitones);

    if (const auto held = findAudioClip (song, clip); held.has_value() && ! juce::exactlyEqual (held->sound.pitch, pitch))
    {
        auto sound = held->sound;
        sound.pitch = pitch;
        giveSound (song, *held, sound);
    }
}

void setClipReversed (Song& song, ClipID clip, bool reversed)
{
    // The same part of the audio plays, backwards.
    if (const auto held = findAudioClip (song, clip); held.has_value() && held->sound.reversed != reversed)
    {
        auto sound = held->sound;
        sound.reversed = reversed;
        giveSound (song, *held, sound);
    }
}

namespace
{
    // An audio clip's value as setClipFollowsTempo() would leave it (and each clip of its alias group): following the song's
    // tempo from its file's, or at its own speed again, its warp markers gone with the tempo they were set at. The same value
    // where nothing changes: it follows (or doesn't) already, or it would follow without its file, whose length, and so its
    // tempo, is unknown.
    ClipValue withTempoFollowed (const Song& song, const ClipValue& clip, bool follows)
    {
        if ((clip.sound.tempo > 0.0) == follows || (follows && getClipFileLength (song, clip) <= 0.0))
            return clip;

        auto next = clip;
        next.sound.tempo = follows ? getFileTempo (song, clip.start, getClipFile (song, clip)) : 0.0;

        if (! follows)
            next.sound = withWarpPoints (next.sound, {});

        return next;
    }
} // namespace

void setClipFollowsTempo (Song& song, ClipID clip, bool follows)
{
    if (const auto held = findAudioClip (song, clip))
        if (const auto next = withTempoFollowed (song, *held, follows); ! (next.sound == held->sound))
            giveSound (song, *held, next.sound);
}

void setClipTempo (Song& song, ClipID clip, double bpm)
{
    const auto held = findAudioClip (song, clip);

    if (! held.has_value() || held->sound.tempo <= 0.0)
        return;

    auto sound = held->sound;
    sound.tempo = juce::jlimit (minClipTempo, maxClipTempo, bpm);
    giveSound (song, *held, sound);
}

//==============================================================================
namespace
{
    // The properties an alias group's audio clips share, each with its value where a clip has none (the clips' code writes
    // none for it; every audio clip names its file, as the loader keeps it). The file's path is compared as the song holds it
    // (Schema.h: clipFile), whether the file is there or not. For the elements of a whole copy of a song (the loader's, a
    // merge's): the clips' own edits give a group's clips one value (ClipsValue.h: ClipSound).
    struct SharedProperty
    {
        juce::Identifier name;
        juce::var none;
    };

    const std::vector<SharedProperty>& getSharedProperties()
    {
        static const std::vector<SharedProperty> properties { { schema::clipFile, juce::String() },
                                                              { schema::clipTempo, 0.0 },
                                                              { schema::clipPitch, 0.0 },
                                                              { schema::clipReversed, false } };
        return properties;
    }

    std::vector<juce::ValueTree> getWarpElements (const juce::ValueTree& clipState)
    {
        std::vector<juce::ValueTree> markers;

        for (const auto& child : clipState)
            if (child.hasType (schema::WARP))
                markers.push_back (child);

        return markers;
    }
} // namespace

bool isSharedInAliasGroup (const juce::Identifier& property)
{
    const auto& shared = getSharedProperties();
    return std::any_of (shared.begin(), shared.end(), [&property] (const SharedProperty& each) { return each.name == property; });
}

bool shareTheSound (const juce::ValueTree& first, const juce::ValueTree& other)
{
    for (const auto& [name, none] : getSharedProperties())
        if (first.getProperty (name, none) != other.getProperty (name, none))
            return false;

    const auto a = getWarpElements (first), b = getWarpElements (other);
    return a.size() == b.size()
           && std::equal (a.begin(), a.end(), b.begin(),
                          [] (const juce::ValueTree& x, const juce::ValueTree& y) { return x.isEquivalentTo (y); });
}

void giveSharedSound (const juce::ValueTree& from, juce::ValueTree to, juce::UndoManager* undo)
{
    if (shareTheSound (from, to))
        return;

    for (const auto& [name, none] : getSharedProperties())
    {
        if (from.hasProperty (name))
            to.setProperty (name, from[name], undo);
        else
            to.removeProperty (name, undo);
    }

    for (const auto& marker : getWarpElements (to))
        to.removeChild (marker, undo);

    for (const auto& marker : getWarpElements (from))
        to.appendChild (marker.createCopy(), undo);
}

//==============================================================================
namespace
{
    // How far a stretch between two warp markers can go, as a share of its length played straight: as fast as eight times, as
    // slow as an eighth.
    constexpr double fastestWarp = 8.0;

    // The time of a clip's file where the clip starts, as recorded: its part's start, or its end while it plays backwards.
    double getStartTimeInFile (const ClipValue& clip)
    {
        return clip.sound.reversed ? clip.fileEnd : clip.fileStart;
    }

    std::vector<std::pair<double, double>>::iterator findPoint (std::vector<std::pair<double, double>>& points, double time)
    {
        return std::find_if (points.begin(), points.end(), [time] (const auto& point) { return std::abs (point.first - time) < 1.0e-7; });
    }

    // Where a time of the file plays straight, for a clip at its tempo whose start's audio plays straight at a time: the beat
    // in the song, as the clip plays it, made a time played straight (backwards, a later beat is an earlier time).
    double getStraightFor (const ClipTime& time, bool reversed, double startStraight, BeatPosition beat)
    {
        const auto along = (beat.inBeats() - time.startBeat) * 60.0 / time.tempo;
        return reversed ? startStraight - along : startStraight + along;
    }

    // The beats times of a clip's file play at (getBeatsOfFileTimes()).
    std::vector<BeatPosition> getBeatsOfFileTimes (const Song& song, const ClipValue& clip, const std::vector<double>& fileTimes)
    {
        std::vector<BeatPosition> beats;
        const auto reversed = clip.sound.reversed;

        // Along markers and tempo changes, by the clip's time; else at one ratio, from where the file's start plays.
        if (const auto time = findMappedTime (song, clip))
        {
            for (const auto fileTime : fileTimes)
                beats.push_back (BeatPosition::fromBeats (time->beatAt (reversed ? time->fileLength - fileTime : fileTime)));

            return beats;
        }

        const auto length = getClipFileLength (song, clip);
        const auto start = getAudioFileRange (song, clip).start;
        const auto ratio = getClipStretch (song, clip).timeRatio;

        for (const auto fileTime : fileTimes)
            beats.push_back (toBeats (song, start + (reversed ? length - fileTime : fileTime) * ratio));

        return beats;
    }
} // namespace

std::vector<WarpMarker> getWarpMarkers (const Song& song, ClipID clip)
{
    std::vector<WarpMarker> markers;
    const auto value = findAudioClip (song, clip);

    if (! value.has_value())
        return markers;

    std::vector<double> times;

    for (const auto& point : getWarpPoints (*value))
        times.push_back (point.first);

    const auto beats = getBeatsOfFileTimes (song, *value, times);

    for (size_t i = 0; i < times.size(); ++i)
        markers.push_back ({ times[i], beats[i] });

    return markers;
}

BeatPosition getBeatOfFileTime (const Song& song, ClipID clip, double fileTime)
{
    return getBeatsOfFileTimes (song, clip, { fileTime }).front();
}

std::vector<BeatPosition> getBeatsOfFileTimes (const Song& song, ClipID clip, const std::vector<double>& fileTimes)
{
    if (const auto value = findAudioClip (song, clip))
        return getBeatsOfFileTimes (song, *value, fileTimes);

    return std::vector<BeatPosition> (fileTimes.size());
}

bool addWarpMarker (Song& song, ClipID clip, double at)
{
    const auto held = findAudioClip (song, clip);

    if (! held.has_value() || at < 0.0 || at > getClipFileLength (song, *held))
        return false;

    // It follows the song's tempo first (setClipFollowsTempo()), which stays done whatever comes of the marker.
    const auto following = withTempoFollowed (song, *held, true);
    auto points = getWarpPoints (following);

    if (following.sound.tempo <= 0.0
        || std::any_of (points.begin(), points.end(), [at] (const auto& point) { return std::abs (point.first - at) < minWarpGap; }))
    {
        if (! (following.sound == held->sound))
            giveSound (song, *held, following.sound);

        return false;
    }

    // Where it plays now, straight: nothing moves.
    points.emplace_back (at, alongPoints (points, at, false));
    std::sort (points.begin(), points.end());
    giveSound (song, *held, withWarpPoints (following.sound, points));
    return true;
}

void moveWarpMarker (Song& song, ClipID clip, double at, BeatPosition beat)
{
    const auto held = findAudioClip (song, clip);

    if (! held.has_value())
        return;

    const auto& value = *held;
    auto points = getWarpPoints (value);

    if (findPoint (points, at) == points.end())
        return;

    // The audio where the clip starts stays there (a marker on it stays with it): held by a marker of its own while the moved
    // one's stretches would reach it.
    const auto startTime = getStartTimeInFile (value);
    const auto startStraight = alongPoints (points, startTime, false);

    if (std::abs (startTime - at) < minWarpGap)
        return;
    const auto moving = findPoint (points, at);
    const auto before = moving == points.begin() ? -std::numeric_limits<double>::max() : std::prev (moving)->first;
    const auto after = std::next (moving) == points.end() ? std::numeric_limits<double>::max() : std::next (moving)->first;

    if (startTime > before && startTime < after && std::abs (startTime - at) >= minWarpGap && findPoint (points, startTime) == points.end())
    {
        points.emplace_back (startTime, startStraight);
        std::sort (points.begin(), points.end());
    }

    // Where the beat is, played straight, kept between its neighbours, each stretch no faster or slower than fastestWarp.
    const auto time = getClipTime (song, value);
    const auto reversed = value.sound.reversed;
    const auto index = (size_t) std::distance (points.begin(), findPoint (points, at));
    auto low = -std::numeric_limits<double>::max(), high = std::numeric_limits<double>::max();

    if (index > 0)
    {
        const auto& previous = points[index - 1];
        low = previous.second + (at - previous.first) / fastestWarp;
        high = previous.second + (at - previous.first) * fastestWarp;
    }

    if (index + 1 < points.size())
    {
        const auto& next = points[index + 1];
        low = std::max (low, next.second - (next.first - at) * fastestWarp);
        high = std::min (high, next.second - (next.first - at) / fastestWarp);
    }

    if (low > high)
        return;

    points[index].second = juce::jlimit (low, high, getStraightFor (time, reversed, startStraight, beat));
    writeWarpPoints (song, value, points);
}

void removeWarpMarker (Song& song, ClipID clip, double at)
{
    const auto held = findAudioClip (song, clip);

    if (! held.has_value())
        return;

    auto points = getWarpPoints (*held);

    if (const auto found = findPoint (points, at); found != points.end())
    {
        points.erase (found);
        writeWarpPoints (song, *held, points);
    }
}

void removeWarpMarkers (Song& song, ClipID clip)
{
    if (const auto held = findAudioClip (song, clip); held.has_value() && ! held->sound.warps.empty())
        writeWarpPoints (song, *held, {});
}

void quantizeClipAudio (Song& song, ClipID clip, const std::vector<double>& hits, BeatDuration grid, double strength)
{
    const auto held = findAudioClip (song, clip);

    if (! held.has_value() || grid.inBeats() <= 0.0)
        return;

    // It follows the song's tempo first (setClipFollowsTempo()), which stays done whatever comes of the hits.
    const auto value = withTempoFollowed (song, *held, true);

    if (value.sound.tempo <= 0.0)
        return;

    const auto time = getClipTime (song, value);
    const auto reversed = value.sound.reversed;
    const auto points = getWarpPoints (value);
    const auto startTime = getStartTimeInFile (value);
    const auto startStraight = alongPoints (points, startTime, false);
    const auto played = getAudioClipRange (song, value);
    const auto step = grid.inBeats();
    const auto share = juce::jlimit (0.0, 1.0, strength);

    // Each hit the clip plays, towards the grid line nearest where it plays now.
    std::vector<std::pair<double, double>> quantized;

    for (const auto hit : hits)
    {
        const auto beat = time.beatAt (reversed ? time.fileLength - hit : hit);

        if (beat < played.getStart().inBeats() || beat >= played.getEnd().inBeats())
            continue;

        const auto target = beat + (std::round (beat / step) * step - beat) * share;
        quantized.emplace_back (hit, getStraightFor (time, reversed, startStraight, BeatPosition::fromBeats (target)));
    }

    // The markers there that no hit replaces stay where they play, and one holds the audio where the clip starts.
    const auto isNear = [&quantized] (double at)
    {
        return std::any_of (quantized.begin(), quantized.end(),
                            [at] (const auto& point) { return std::abs (point.first - at) < minWarpGap; });
    };

    for (const auto& point : points)
        if (! isNear (point.first))
            quantized.push_back (point);

    if (! isNear (startTime))
        quantized.emplace_back (startTime, startStraight);

    // In time order, each later than the one before where it plays: one that would play with or before it stays unquantized.
    std::sort (quantized.begin(), quantized.end());
    std::vector<std::pair<double, double>> rising;

    for (const auto& point : quantized)
        if (rising.empty() || point.second > rising.back().second + minWarpGap / fastestWarp)
            rising.push_back (point);

    giveSound (song, *held, withWarpPoints (value.sound, rising));
}

int getClipTakeCount (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? value->takes : 0;
}

int getClipTake (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);
    return value.has_value() ? value->take : 1;
}

void playTake (Song& song, ClipID clip, int take)
{
    const auto held = findAudioClip (song, clip);

    if (! held.has_value() || held->takes < 2 || take < 1 || take > held->takes || take == held->take)
        return;

    // The takes lie one after another in the clip's file, each as long as the range they were recorded round.
    const auto shift = held->takeLength * (take - held->take);
    auto next = *held;
    next.take = take;
    next.start = BeatPosition::fromBeats (std::max (0.0, held->start.inBeats())); // as the clips' edits keep it
    next.fileStart = std::max (0.0, held->fileStart + shift);
    next.fileEnd = std::max (held->fileStart + shift, held->fileEnd + shift);
    setClip (song, *held, next);
}

std::vector<ClipID> getAudioClips (const Song& song)
{
    std::vector<ClipID> clips;

    // Track by track, each track's in its order.
    for (const auto& clip : getClipPlacements (song))
        if (clip.isAudio())
            clips.push_back (clip.id);

    return clips;
}

juce::StringArray findAudioFilePaths (const Song& song)
{
    juce::StringArray paths;

    for (const auto& clip : getClipValues (song))
        if (clip.sound.file.isNotEmpty())
            paths.addIfNotAlreadyThere (clip.sound.file);

    // The devices' sound files, by their devices' IDs (Mixer.h: getDeviceSoundFiles()).
    for (const auto& sound : getDeviceSoundFiles (song))
        paths.addIfNotAlreadyThere (sound.path);

    return paths;
}

//==============================================================================
namespace
{
    // The audio clips among some clips.
    std::vector<ClipID> getAudioClipsAmong (const Song& song, const std::vector<ClipID>& clips)
    {
        std::vector<ClipID> audio;

        for (const auto clip : clips)
            if (isAudioClip (song, clip))
                audio.push_back (clip);

        return audio;
    }
} // namespace

bool areAllClipsReversed (const Song& song, const std::vector<ClipID>& clips)
{
    return areAllOn (getAudioClipsAmong (song, clips), [&song] (ClipID clip) { return isClipReversed (song, clip); });
}

void toggleClipsReversed (Song& song, const std::vector<ClipID>& clips)
{
    toggleAll (
        getAudioClipsAmong (song, clips), [&song] (ClipID clip) { return isClipReversed (song, clip); },
        [&song] (ClipID clip, bool on) { setClipReversed (song, clip, on); });
}

bool doAllClipsFollowTempo (const Song& song, const std::vector<ClipID>& clips)
{
    return areAllOn (getAudioClipsAmong (song, clips), [&song] (ClipID clip) { return getClipTempo (song, clip) > 0.0; });
}

void toggleClipsFollowTempo (Song& song, const std::vector<ClipID>& clips)
{
    toggleAll (
        getAudioClipsAmong (song, clips), [&song] (ClipID clip) { return getClipTempo (song, clip) > 0.0; },
        [&song] (ClipID clip, bool on) { setClipFollowsTempo (song, clip, on); });
}

//==============================================================================
std::optional<float> getClipPeak (const Song& song, ClipID clip)
{
    const auto value = findAudioClip (song, clip);

    if (! value.has_value())
        return {};

    return readPeak (getClipFile (song, *value), getFilePart (*value), nullptr, nullptr).peak;
}

PeakRead readPeak (const juce::File& file, FilePart part, const std::function<bool()>& shouldStop,
                   const std::function<void (float)>& progress)
{
    PeakRead read;
    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file));

    if (reader == nullptr || reader->numChannels == 0 || reader->numChannels > 64 || ! (reader->sampleRate > 0.0))
        return read;

    // As long as it says it is: the read is a second at a time and can stop, so a header that lies costs only time.
    const auto held = reader->lengthInSamples;

    // The samples the part covers, whole: from the one it starts in to the one it ends in, a second at a time, so that it can
    // stop between them.
    const auto first = juce::jlimit ((juce::int64) 0, held, (juce::int64) std::floor (part.start * reader->sampleRate));
    const auto end = juce::jlimit (first, held, (juce::int64) std::ceil (part.end * reader->sampleRate));
    const auto piece = juce::jlimit ((juce::int64) 4096, (juce::int64) 1 << 20, (juce::int64) reader->sampleRate);
    const auto bytesPerFrame = (juce::int64) reader->numChannels * std::max (1, (int) reader->bitsPerSample / 8);
    std::vector<juce::Range<float>> levels ((size_t) reader->numChannels);
    auto peak = 0.0f;

    for (auto at = first; at < end; at += piece)
    {
        if (shouldStop != nullptr && shouldStop())
        {
            read.stopped = true;
            return read;
        }

        const auto length = std::min (piece, end - at);
        reader->readMaxLevels (at, length, levels.data(), (int) reader->numChannels);
        AudioWork::noteRead (length * bytesPerFrame);

        for (const auto& level : levels)
            peak = std::max ({ peak, std::abs (level.getStart()), std::abs (level.getEnd()) });

        if (progress != nullptr)
            progress ((float) ((double) (at + length - first) / (double) (end - first)));
    }

    read.peak = peak;
    return read;
}
} // namespace phazite
