// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioClips.h"
#include "AudioClipModel.h"
#include "internal/Clips.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/engine/AudioFiles.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace phazite
{
namespace
{
    // Bumped whenever renders would come out differently, so that a project's old ones aren't played.
    constexpr const char* renderVersion = "rubberband-4.0-r3-1";

    // What a clip's sound is made from, and how (Schema.h).
    struct Sound
    {
        juce::File file;
        double tempo = 0.0; // the file's, while the clip follows the song's; 0 while it doesn't
        float pitch = 0.0f;
        bool reversed = false;
    };

    Sound getSound (const Song& song, const juce::ValueTree& clipState)
    {
        return { getClipFile (song, clipState), (double) clipState.getProperty (schema::clipTempo, 0.0),
                 (float) clipState.getProperty (schema::clipPitch, 0.0f), (bool) clipState.getProperty (schema::clipReversed, false) };
    }

    stretch::Settings getSettings (const Sound& sound, double songTempo)
    {
        return { sound.tempo > 0.0 && songTempo > 0.0 ? sound.tempo / songTempo : 1.0, (double) sound.pitch, sound.reversed };
    }

    double getFileSeconds (const juce::File& file)
    {
        return getAudioFileInfo (file).getSeconds();
    }

    void setOrRemove (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value, juce::UndoManager* um)
    {
        if (value.isVoid())
            state.removeProperty (property, um);
        else
            state.setProperty (property, value, um);
    }

    void writeSound (Song& song, juce::ValueTree& clipState, const Sound& sound)
    {
        auto* um = &song.getUndoManager();
        setOrRemove (clipState, schema::clipTempo, sound.tempo > 0.0 ? juce::var (sound.tempo) : juce::var(), um);
        setOrRemove (clipState, schema::clipPitch, juce::exactlyEqual (sound.pitch, 0.0f) ? juce::var() : juce::var (sound.pitch), um);
        setOrRemove (clipState, schema::clipReversed, sound.reversed ? juce::var (true) : juce::var(), um);
    }

    // An audio clip's element, or invalid.
    juce::ValueTree findAudioState (const Song& song, ClipID clip)
    {
        const auto clipState = findClipState (song, clip);
        return isAudioState (clipState) ? clipState : juce::ValueTree();
    }

    // Changes how a clip sounds, keeping its start and the part of its file it plays: in each clip of its alias group too.
    template <typename Change>
    void changeSound (Song& song, ClipID clip, Change&& change)
    {
        const auto clipState = findAudioState (song, clip);

        if (! clipState.isValid())
            return;

        for (auto alias : getAliasStates (clipState))
        {
            auto sound = getSound (song, alias);
            change (sound);
            writeSound (song, alias, sound);
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
    std::vector<std::pair<double, double>> getWarpPoints (const juce::ValueTree& clipState)
    {
        std::vector<std::pair<double, double>> points;

        if ((double) clipState.getProperty (schema::clipTempo, 0.0) <= 0.0)
            return points;

        for (const auto& child : clipState)
            if (child.hasType (schema::WARP))
                points.emplace_back ((double) child[schema::warpTime], (double) child[schema::warpPlaysAt]);

        std::sort (points.begin(), points.end());
        std::vector<std::pair<double, double>> rising;

        for (const auto& point : points)
            if (rising.empty() || (point.first > rising.back().first && point.second > rising.back().second))
                rising.push_back (point);

        return rising;
    }

    // A clip's warp markers, in each clip of its alias group (which play the same audio), in place of those there.
    void writeWarpPoints (Song& song, const juce::ValueTree& clipState, const std::vector<std::pair<double, double>>& points)
    {
        auto* um = &song.getUndoManager();

        for (auto alias : getAliasStates (clipState))
        {
            for (auto i = alias.getNumChildren(); --i >= 0;)
                if (alias.getChild (i).hasType (schema::WARP))
                    alias.removeChild (i, um);

            for (const auto& [at, playsAt] : points)
            {
                juce::ValueTree warp (schema::WARP);
                warp.setProperty (schema::warpTime, at, nullptr);
                warp.setProperty (schema::warpPlaysAt, playsAt, nullptr);
                alias.appendChild (warp, um);
            }
        }
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

    ClipTime getClipTime (const Song& song, const juce::ValueTree& clipState)
    {
        const auto part = getFilePart (clipState);
        const auto length = getClipFileLength (song, clipState);
        const auto reversed = (bool) clipState.getProperty (schema::clipReversed, false);
        ClipTime time { reversed ? length - part.end : part.start, part.end - part.start, length,
                        getClipRange (clipState).getStart().inBeats(), (double) clipState.getProperty (schema::clipTempo, 0.0) };
        time.warps = getWarpPoints (clipState);

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
    std::optional<ClipTime> findMappedTime (const Song& song, const juce::ValueTree& clipState)
    {
        if ((double) clipState.getProperty (schema::clipTempo, 0.0) <= 0.0)
            return std::nullopt;

        const auto warped = clipState.getChildWithName (schema::WARP).isValid();

        if (! warped && ! hasTempoChanges (song))
            return std::nullopt;

        const auto time = getClipTime (song, clipState);

        if (time.fileLength <= 0.0
            || (time.warps.empty() && getTempoMap (song).isSteadyBetween (time.beatAt (0.0), time.beatAt (time.fileLength))))
            return std::nullopt;

        return time;
    }

    // Its stretch along its markers and the tempo's changes: at every half beat of the sound, at each marker and where the
    // song's tempo changes, the time in the sound with where it plays in the render, which starts where the sound's start
    // plays.
    std::vector<std::pair<double, double>> makeTimeMap (const Song& song, const ClipTime& time)
    {
        const auto map = getTempoMap (song); // in seconds
        const auto origin = map.toSamples (time.beatAt (0.0));
        const auto step = 30.0 / time.tempo; // half a beat of the sound, played straight
        const auto first = time.unwarp (0.0), last = time.unwarp (time.fileLength);
        std::vector<double> times;

        for (int each = 1; first + each * step < last; ++each)
            times.push_back (time.warp (first + each * step));

        for (const auto& change : getTempoChanges (song))
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

FilePart getFilePart (const juce::ValueTree& clipState)
{
    return { (double) clipState[schema::clipFileStart], (double) clipState[schema::clipFileEnd] };
}

juce::ValueTree makeAudioClipState (Song& song, const juce::String& name, const juce::File& file, BeatPosition start, FilePart part)
{
    juce::ValueTree clipState (schema::CLIP);
    clipState.setProperty (schema::clipID, (juce::int64) song.makeID(), nullptr);
    clipState.setProperty (schema::clipKind, schema::clipIsAudio, nullptr);
    clipState.setProperty (schema::clipName, name, nullptr);
    clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), nullptr);
    // Relative to the project's folder, as devices keep their files (DeviceFiles.h), written the same on every system.
    clipState.setProperty (schema::clipFile, getProjectPath (song, file), nullptr);
    clipState.setProperty (schema::clipFileStart, part.start, nullptr);
    clipState.setProperty (schema::clipFileEnd, part.end, nullptr);
    return clipState;
}

juce::File getClipFile (const Song& song, const juce::ValueTree& clipState)
{
    return findProjectFile (song, clipState[schema::clipFile].toString());
}

double getClipFileLength (const Song& song, const juce::ValueTree& clipState)
{
    return getFileSeconds (getClipFile (song, clipState));
}

stretch::Settings getClipStretch (const Song& song, const juce::ValueTree& clipState)
{
    auto settings = getSettings (getSound (song, clipState), getTempoAt (song, getClipRange (clipState).getStart()));

    // Along its warp markers and the song's tempo changes, the stretch follows each of them, its ratio being the whole file's.
    if (const auto time = findMappedTime (song, clipState))
    {
        settings.timeMap = makeTimeMap (song, *time);
        settings.timeRatio = settings.timeMap.back().second / time->fileLength;
    }

    return settings;
}

bool followsTempoChanges (const Song& song, const juce::ValueTree& clipState)
{
    return isAudioState (clipState) && findMappedTime (song, clipState).has_value();
}

double getFileSecondsBetween (const Song& song, const juce::ValueTree& clipState, double from, BeatPosition to)
{
    // Along markers and tempo changes, the clip's beats are its sound's, at its file's tempo between its markers.
    if (const auto time = findMappedTime (song, clipState))
        return time->timeAt (to.inBeats()) - time->timeAt (toBeats (song, from).inBeats());

    return (toSeconds (song, to) - from) / getClipStretch (song, clipState).timeRatio;
}

double getPartStartFor (const Song& song, const juce::ValueTree& clipState, double offset)
{
    // The sound's start would play at the song's time so far before the clip's start: its part starts where that puts it.
    if (const auto time = findMappedTime (song, clipState))
    {
        const auto start = toSeconds (song, BeatPosition::fromBeats (time->startBeat));
        const auto soundStart = toBeats (song, start - offset).inBeats();
        return time->warp (time->unwarp (0.0) + (time->startBeat - soundStart) * 60.0 / time->tempo);
    }

    return offset / getClipStretch (song, clipState).timeRatio;
}

juce::File getPlayedFile (const Song& song, const juce::ValueTree& clipState)
{
    const auto file = getClipFile (song, clipState);

    if (const auto settings = getClipStretch (song, clipState); ! stretch::isUnchanged (settings))
        return getRenderFile (song, file, settings);

    return file;
}

PlayedPosition getPlayedPosition (const Song& song, const juce::ValueTree& clipState)
{
    const auto start = toSeconds (song, getClipRange (clipState).getStart());

    // Along its markers and the song's tempo changes: where its part's end plays, and how far into the render its part's
    // start is.
    if (const auto time = findMappedTime (song, clipState))
    {
        const auto map = getTempoMap (song);
        return { start, map.toSamples (time->beatAt (time->partStart + time->partLength)), start - map.toSamples (time->beatAt (0.0)) };
    }

    const auto settings = getClipStretch (song, clipState);
    const auto part = getFilePart (clipState);
    const auto from = settings.reversed ? getFileSeconds (getClipFile (song, clipState)) - part.end : part.start;
    return { start, start + (part.end - part.start) * settings.timeRatio, from * settings.timeRatio };
}

BeatRange getAudioClipRange (const Song& song, const juce::ValueTree& clipState)
{
    const auto start = getClipRange (clipState).getStart();
    return { start, toBeats (song, getPlayedPosition (song, clipState).end) };
}

void setAudioPart (Song& song, juce::ValueTree clipState, BeatPosition start, FilePart part)
{
    auto* um = &song.getUndoManager();
    clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), um);
    clipState.setProperty (schema::clipFileStart, std::max (0.0, part.start), um);
    clipState.setProperty (schema::clipFileEnd, std::max (part.start, part.end), um);
}

double getClipFade (const juce::ValueTree& clipState, bool fadeIn)
{
    return (double) clipState.getProperty (fadeIn ? schema::clipFadeIn : schema::clipFadeOut, 0.0);
}

std::pair<double, double> getFittedFades (const Song& song, const juce::ValueTree& clipState)
{
    const auto position = getPlayedPosition (song, clipState);
    const auto length = position.end - position.start;
    auto in = juce::jlimit (0.0, length, getClipFade (clipState, true));
    auto out = juce::jlimit (0.0, length, getClipFade (clipState, false));

    if (in + out > length && in + out > 0.0)
    {
        const auto scale = length / (in + out);
        in *= scale;
        out *= scale;
    }

    return { in, out };
}

void setClipFade (Song& song, juce::ValueTree clipState, bool fadeIn, double seconds)
{
    const auto& property = fadeIn ? schema::clipFadeIn : schema::clipFadeOut;

    if (seconds > 0.0)
        clipState.setProperty (property, seconds, &song.getUndoManager());
    else
        clipState.removeProperty (property, &song.getUndoManager());
}

FadeCurve getClipFadeCurve (const juce::ValueTree& clipState, bool fadeIn)
{
    const auto shape = clipState[fadeIn ? schema::clipFadeInShape : schema::clipFadeOutShape].toString();
    const auto bend = (float) clipState.getProperty (fadeIn ? schema::clipFadeInBend : schema::clipFadeOutBend, 0.0f);
    return { shape == schema::fadeIsSCurve ? FadeShape::sCurve : FadeShape::curve, juce::jlimit (-1.0f, 1.0f, bend) };
}

void setClipFadeCurve (Song& song, juce::ValueTree clipState, bool fadeIn, FadeCurve curve)
{
    // What is as at first isn't written: a straight curve.
    auto* um = &song.getUndoManager();
    const auto& shapeProperty = fadeIn ? schema::clipFadeInShape : schema::clipFadeOutShape;
    const auto& bendProperty = fadeIn ? schema::clipFadeInBend : schema::clipFadeOutBend;
    const auto bend = juce::jlimit (-1.0f, 1.0f, curve.bend);

    if (curve.shape == FadeShape::sCurve)
        clipState.setProperty (shapeProperty, schema::fadeIsSCurve, um);
    else
        clipState.removeProperty (shapeProperty, um);

    if (juce::exactlyEqual (bend, 0.0f))
        clipState.removeProperty (bendProperty, um);
    else
        clipState.setProperty (bendProperty, bend, um);
}

engine::FadeCurve toEngineFade (FadeCurve curve)
{
    return { curve.shape == FadeShape::sCurve ? engine::FadeShape::sCurve : engine::FadeShape::curve, curve.bend };
}

//==============================================================================
stretch::Settings getClipStretch (const Song& song, ClipID clip)
{
    if (const auto clipState = findAudioState (song, clip); clipState.isValid())
        return getClipStretch (song, clipState);

    return {};
}

bool isRendered (const Song& song, ClipID clip)
{
    return ! stretch::isUnchanged (getClipStretch (song, clip));
}

juce::File getRenderFile (const Song& song, const juce::File& source, const stretch::Settings& settings)
{
    auto key = source.getFullPathName() + "|" + juce::String (settings.timeRatio, 9) + "|" + juce::String (settings.semitones, 4) + "|"
               + (settings.reversed ? "backwards" : "forwards") + "|" + renderVersion;

    // Along tempo changes, the whole stretch: two clips under the same changes share it.
    for (const auto& [in, out] : settings.timeMap)
        key << "|" << juce::String (in, 6) << ":" << juce::String (out, 6);

    return song.getCacheFolder().getChildFile (renderFilePrefix + juce::String::toHexString (key.hashCode64()) + ".wav");
}

SecondsRange getAudioFileRange (const Song& song, const juce::ValueTree& clipState)
{
    if (const auto time = findMappedTime (song, clipState))
    {
        const auto map = getTempoMap (song);
        return { map.toSamples (time->beatAt (0.0)), map.toSamples (time->beatAt (time->fileLength)) };
    }

    const auto position = getPlayedPosition (song, clipState);
    const auto start = position.start - position.offset;
    return { start, start + getClipFileLength (song, clipState) * getClipStretch (song, clipState).timeRatio };
}

double getFileTimeAt (const Song& song, ClipID clip, BeatPosition beat)
{
    const auto clipState = findAudioState (song, clip);

    // Along markers and tempo changes, the sound's time is the song's beat, at the file's tempo between its markers.
    if (const auto time = findMappedTime (song, clipState))
    {
        const auto into = time->timeAt (beat.inBeats());
        return (bool) clipState.getProperty (schema::clipReversed, false) ? time->fileLength - into : into;
    }

    const auto position = getPlayedPosition (song, clipState);
    const auto settings = getClipStretch (song, clipState);
    const auto intoFile = (toSeconds (song, beat) - (position.start - position.offset)) / settings.timeRatio;
    return settings.reversed ? getClipFileLength (song, clipState) - intoFile : intoFile;
}

double getClipTempo (const Song& song, ClipID clip)
{
    return (double) findAudioState (song, clip).getProperty (schema::clipTempo, 0.0);
}

float getClipPitch (const Song& song, ClipID clip)
{
    return (float) findAudioState (song, clip).getProperty (schema::clipPitch, 0.0f);
}

bool isClipReversed (const Song& song, ClipID clip)
{
    return (bool) findAudioState (song, clip).getProperty (schema::clipReversed, false);
}

float getClipGain (const Song& song, ClipID clip)
{
    return (float) findAudioState (song, clip).getProperty (schema::clipGain, 0.0f);
}

void setClipGain (Song& song, ClipID clip, float decibels)
{
    if (auto clipState = findAudioState (song, clip); clipState.isValid())
    {
        const auto gain = juce::jlimit (minClipGain, maxClipGain, decibels);
        setOrRemove (clipState, schema::clipGain, juce::exactlyEqual (gain, 0.0f) ? juce::var() : juce::var (gain), &song.getUndoManager());
    }
}

void setClipPitch (Song& song, ClipID clip, float semitones)
{
    const auto pitch = juce::jlimit (-maxClipPitch, maxClipPitch, semitones);

    if (! juce::exactlyEqual (getClipPitch (song, clip), pitch))
        changeSound (song, clip, [pitch] (Sound& sound) { sound.pitch = pitch; });
}

void setClipReversed (Song& song, ClipID clip, bool reversed)
{
    // The same part of the audio plays, backwards.
    if (isClipReversed (song, clip) != reversed)
        changeSound (song, clip, [reversed] (Sound& sound) { sound.reversed = reversed; });
}

void setClipFollowsTempo (Song& song, ClipID clip, bool follows)
{
    const auto clipState = findAudioState (song, clip);

    // Without its file, how long it lasts is unknown, and so is its tempo.
    if (! clipState.isValid() || (getClipTempo (song, clip) > 0.0) == follows || (follows && getClipFileLength (song, clipState) <= 0.0))
        return;

    const auto tempo = follows ? getFileTempo (song, getClipRange (clipState).getStart(), getClipFile (song, clipState)) : 0.0;
    changeSound (song, clip, [tempo] (Sound& sound) { sound.tempo = tempo; });

    // At its own speed, its warp markers go with the tempo they were set at.
    if (! follows)
        writeWarpPoints (song, clipState, {});
}

void setClipTempo (Song& song, ClipID clip, double bpm)
{
    if (getClipTempo (song, clip) <= 0.0)
        return;

    const auto tempo = juce::jlimit (minClipTempo, maxClipTempo, bpm);
    changeSound (song, clip, [tempo] (Sound& sound) { sound.tempo = tempo; });
}

//==============================================================================
namespace
{
    // How far a stretch between two warp markers can go, as a share of its length played straight: as fast as eight times, as
    // slow as an eighth.
    constexpr double fastestWarp = 8.0;

    // The time of a clip's file where the clip starts, as recorded: its part's start, or its end while it plays backwards.
    double getStartTimeInFile (const juce::ValueTree& clipState)
    {
        const auto part = getFilePart (clipState);
        return (bool) clipState.getProperty (schema::clipReversed, false) ? part.end : part.start;
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
} // namespace

std::vector<WarpMarker> getWarpMarkers (const Song& song, ClipID clip)
{
    std::vector<WarpMarker> markers;
    const auto points = getWarpPoints (findAudioState (song, clip));
    std::vector<double> times;

    for (const auto& point : points)
        times.push_back (point.first);

    const auto beats = getBeatsOfFileTimes (song, clip, times);

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
    std::vector<BeatPosition> beats;
    const auto clipState = findAudioState (song, clip);

    if (! clipState.isValid())
        return std::vector<BeatPosition> (fileTimes.size());

    const auto reversed = (bool) clipState.getProperty (schema::clipReversed, false);

    // Along markers and tempo changes, by the clip's time; else at one ratio, from where the file's start plays.
    if (const auto time = findMappedTime (song, clipState))
    {
        for (const auto fileTime : fileTimes)
            beats.push_back (BeatPosition::fromBeats (time->beatAt (reversed ? time->fileLength - fileTime : fileTime)));

        return beats;
    }

    const auto length = getClipFileLength (song, clipState);
    const auto start = getAudioFileRange (song, clipState).start;
    const auto ratio = getClipStretch (song, clipState).timeRatio;

    for (const auto fileTime : fileTimes)
        beats.push_back (toBeats (song, start + (reversed ? length - fileTime : fileTime) * ratio));

    return beats;
}

bool addWarpMarker (Song& song, ClipID clip, double at)
{
    if (! findAudioState (song, clip).isValid() || at < 0.0 || at > getClipFileLength (song, findAudioState (song, clip)))
        return false;

    setClipFollowsTempo (song, clip, true);
    const auto clipState = findAudioState (song, clip);
    auto points = getWarpPoints (clipState);

    if (getClipTempo (song, clip) <= 0.0
        || std::any_of (points.begin(), points.end(), [at] (const auto& point) { return std::abs (point.first - at) < minWarpGap; }))
        return false;

    // Where it plays now, straight: nothing moves.
    points.emplace_back (at, alongPoints (points, at, false));
    std::sort (points.begin(), points.end());
    writeWarpPoints (song, clipState, points);
    return true;
}

void moveWarpMarker (Song& song, ClipID clip, double at, BeatPosition beat)
{
    const auto clipState = findAudioState (song, clip);
    auto points = getWarpPoints (clipState);

    if (findPoint (points, at) == points.end())
        return;

    // The audio where the clip starts stays there (a marker on it stays with it): held by a marker of its own while the moved
    // one's stretches would reach it.
    const auto startTime = getStartTimeInFile (clipState);
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
    const auto time = getClipTime (song, clipState);
    const auto reversed = (bool) clipState.getProperty (schema::clipReversed, false);
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
    writeWarpPoints (song, clipState, points);
}

void removeWarpMarker (Song& song, ClipID clip, double at)
{
    const auto clipState = findAudioState (song, clip);
    auto points = getWarpPoints (clipState);

    if (const auto found = findPoint (points, at); found != points.end())
    {
        points.erase (found);
        writeWarpPoints (song, clipState, points);
    }
}

void removeWarpMarkers (Song& song, ClipID clip)
{
    if (const auto clipState = findAudioState (song, clip); clipState.isValid() && clipState.getChildWithName (schema::WARP).isValid())
        writeWarpPoints (song, clipState, {});
}

void quantizeClipAudio (Song& song, ClipID clip, const std::vector<double>& hits, BeatDuration grid, double strength)
{
    if (! findAudioState (song, clip).isValid() || grid.inBeats() <= 0.0)
        return;

    setClipFollowsTempo (song, clip, true);
    const auto clipState = findAudioState (song, clip);

    if (getClipTempo (song, clip) <= 0.0)
        return;

    const auto time = getClipTime (song, clipState);
    const auto reversed = (bool) clipState.getProperty (schema::clipReversed, false);
    const auto points = getWarpPoints (clipState);
    const auto startTime = getStartTimeInFile (clipState);
    const auto startStraight = alongPoints (points, startTime, false);
    const auto played = getAudioClipRange (song, clipState);
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

    writeWarpPoints (song, clipState, rising);
}

int getClipTakeCount (const Song& song, ClipID clip)
{
    return (int) findAudioState (song, clip).getProperty (schema::clipTakes, 0);
}

int getClipTake (const Song& song, ClipID clip)
{
    return (int) findAudioState (song, clip).getProperty (schema::clipTake, 1);
}

void playTake (Song& song, ClipID clip, int take)
{
    auto clipState = findAudioState (song, clip);
    const auto playing = getClipTake (song, clip);

    if (getClipTakeCount (song, clip) < 2 || take < 1 || take > getClipTakeCount (song, clip) || take == playing)
        return;

    // The takes lie one after another in the clip's file, each as long as the range they were recorded round.
    const auto shift = (double) clipState[schema::clipTakeLength] * (take - playing);
    const auto part = getFilePart (clipState);
    clipState.setProperty (schema::clipTake, take, &song.getUndoManager());
    setAudioPart (song, clipState, getClipRange (clipState).getStart(), { part.start + shift, part.end + shift });
}

std::vector<ClipID> getAudioClips (const Song& song)
{
    std::vector<ClipID> clips;

    for (const auto& clipState : getClipStates (song))
        if (isAudioState (clipState))
            clips.push_back (getClipID (clipState));

    return clips;
}

//==============================================================================
namespace
{
    // The audio clips among some clips.
    std::vector<ClipID> getAudioClipsAmong (const Song& song, const std::vector<ClipID>& clips)
    {
        std::vector<ClipID> audio;

        for (const auto clip : clips)
            if (findAudioState (song, clip).isValid())
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
    const auto clipState = findAudioState (song, clip);

    if (! clipState.isValid())
        return {};

    const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (getClipFile (song, clipState)));

    if (reader == nullptr || reader->numChannels == 0 || reader->sampleRate <= 0.0)
        return {};

    // The samples the part covers, whole: from the one it starts in to the one it ends in.
    const auto part = getFilePart (clipState);
    const auto first = juce::jlimit ((juce::int64) 0, reader->lengthInSamples, (juce::int64) std::floor (part.start * reader->sampleRate));
    const auto end = juce::jlimit (first, reader->lengthInSamples, (juce::int64) std::ceil (part.end * reader->sampleRate));

    if (end == first)
        return 0.0f;

    std::vector<juce::Range<float>> levels ((size_t) reader->numChannels);
    reader->readMaxLevels (first, end - first, levels.data(), (int) reader->numChannels);
    auto peak = 0.0f;

    for (const auto& level : levels)
        peak = std::max ({ peak, std::abs (level.getStart()), std::abs (level.getEnd()) });

    return peak;
}

void normalizeClips (Song& song, const std::vector<ClipID>& clips)
{
    for (const auto clip : getAudioClipsAmong (song, clips))
        if (const auto peak = getClipPeak (song, clip); peak.has_value() && *peak > 0.0f)
            setClipGain (song, clip, normalizedPeak - juce::Decibels::gainToDecibels (*peak, -200.0f));
}
} // namespace phazite
