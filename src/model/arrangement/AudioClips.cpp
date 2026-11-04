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
    return getSettings (getSound (song, clipState), getTempoAt (song, getClipRange (clipState).getStart()));
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
    const auto key = source.getFullPathName() + "|" + juce::String (settings.timeRatio, 9) + "|" + juce::String (settings.semitones, 4)
                     + "|" + (settings.reversed ? "backwards" : "forwards") + "|" + renderVersion;
    return song.getCacheFolder().getChildFile (renderFilePrefix + juce::String::toHexString (key.hashCode64()) + ".wav");
}

SecondsRange getAudioFileRange (const Song& song, const juce::ValueTree& clipState)
{
    const auto position = getPlayedPosition (song, clipState);
    const auto start = position.start - position.offset;
    return { start, start + getClipFileLength (song, clipState) * getClipStretch (song, clipState).timeRatio };
}

double getFileTimeAt (const Song& song, ClipID clip, BeatPosition beat)
{
    const auto clipState = findAudioState (song, clip);
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
}

void setClipTempo (Song& song, ClipID clip, double bpm)
{
    if (getClipTempo (song, clip) <= 0.0)
        return;

    const auto tempo = juce::jlimit (minClipTempo, maxClipTempo, bpm);
    changeSound (song, clip, [tempo] (Sound& sound) { sound.tempo = tempo; });
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
} // namespace phazite
