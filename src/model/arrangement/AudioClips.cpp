// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioClips.h"
#include "AudioClipModel.h"
#include "internal/Clips.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/engine/AudioFiles.h"
#include "model/mirror/internal/Mirror.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Bumped whenever renders would come out differently, so that a project's old ones aren't played.
    constexpr const char* renderVersion = "rubberband-4.0-r3-1";

    juce::UndoManager* getUndo (te::Edit& edit)
    {
        return &getSong (edit).getUndoManager();
    }

    // What a clip's sound is made from, and how (Schema.h).
    struct Sound
    {
        juce::File file;
        double tempo = 0.0; // the file's, while the clip follows the song's; 0 while it doesn't
        float pitch = 0.0f;
        bool reversed = false;
    };

    Sound getSound (const te::Edit& edit, const juce::ValueTree& clipState)
    {
        return { getClipFile (edit, clipState), (double) clipState.getProperty (schema::clipTempo, 0.0),
                 (float) clipState.getProperty (schema::clipPitch, 0.0f), (bool) clipState.getProperty (schema::clipReversed, false) };
    }

    double getSongTempoAt (const te::Edit& edit, BeatPosition beat)
    {
        const auto& tempo = edit.tempoSequence;
        return tempo.getBeatsPerSecondAt (tempo.toTime (toTracktion (beat))) * 60.0;
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

    void writeSound (te::Edit& edit, juce::ValueTree& clipState, const Sound& sound)
    {
        auto* um = getUndo (edit);
        setOrRemove (clipState, schema::clipTempo, sound.tempo > 0.0 ? juce::var (sound.tempo) : juce::var(), um);
        setOrRemove (clipState, schema::clipPitch, juce::exactlyEqual (sound.pitch, 0.0f) ? juce::var() : juce::var (sound.pitch), um);
        setOrRemove (clipState, schema::clipReversed, sound.reversed ? juce::var (true) : juce::var(), um);
    }

    // Changes how a clip sounds, keeping its start and the part of its file it plays: in each clip of its alias group too.
    template <typename Change>
    void changeSound (te::AudioClipBase& clip, Change&& change)
    {
        const auto clipState = findClipState (clip);

        if (! clipState.isValid())
            return;

        for (auto alias : getAliasStates (clipState))
        {
            auto sound = getSound (clip.edit, alias);
            change (sound);
            writeSound (clip.edit, alias, sound);
            mirror::reconcileClip (clip.edit, getClipID (alias));
        }
    }

    // The tempo a file was played at: from its loop data, or else the song's where the clip starts, so that it sounds as it
    // does until either tempo changes.
    double getFileTempo (const te::Edit& edit, BeatPosition start, const juce::File& file)
    {
        const auto info = getAudioFileInfo (file);
        const auto seconds = info.getSeconds();
        const auto tempo = seconds > 0.0 && info.loopBeats > 0.0 ? info.loopBeats / seconds * 60.0 : getSongTempoAt (edit, start);
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

juce::ValueTree makeAudioClipState (te::Edit& edit, const juce::String& name, const juce::File& file, BeatPosition start, FilePart part)
{
    juce::ValueTree clipState (schema::CLIP);
    clipState.setProperty (schema::clipID, (juce::int64) getSong (edit).makeID(), nullptr);
    clipState.setProperty (schema::clipKind, schema::clipIsAudio, nullptr);
    clipState.setProperty (schema::clipName, name, nullptr);
    clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), nullptr);
    // Relative to the project's folder, as Tracktion finds it again (from the project file's folder) and as devices keep their
    // files (DeviceFiles.h): also while a new project's content is made, before its file exists, when Tracktion's own
    // findPathFromFile() would start from the file as if it were a folder ("../Audio/...").
    clipState.setProperty (schema::clipFile, getProjectPath (edit, file), nullptr);
    clipState.setProperty (schema::clipFileStart, part.start, nullptr);
    clipState.setProperty (schema::clipFileEnd, part.end, nullptr);
    return clipState;
}

juce::File getClipFile (const te::Edit& edit, const juce::ValueTree& clipState)
{
    return te::SourceFileReference::findFileFromString (const_cast<te::Edit&> (edit), clipState[schema::clipFile].toString());
}

double getClipFileLength (const te::Edit& edit, const juce::ValueTree& clipState)
{
    return getFileSeconds (getClipFile (edit, clipState));
}

stretch::Settings getClipStretch (const te::Edit& edit, const juce::ValueTree& clipState)
{
    return getSettings (getSound (edit, clipState), getSongTempoAt (edit, getClipRange (clipState).getStart()));
}

juce::File getPlayedFile (te::Edit& edit, const juce::ValueTree& clipState)
{
    const auto file = getClipFile (edit, clipState);

    if (const auto settings = getClipStretch (edit, clipState); ! stretch::isUnchanged (settings))
        return getRenderFile (edit, file, settings);

    return file;
}

te::ClipPosition getPlayedPosition (const te::Edit& edit, const juce::ValueTree& clipState)
{
    const auto start = edit.tempoSequence.toTime (toTracktion (getClipRange (clipState).getStart()));
    const auto settings = getClipStretch (edit, clipState);
    const auto part = getFilePart (clipState);
    const auto from = settings.reversed ? getFileSeconds (getClipFile (edit, clipState)) - part.end : part.start;
    return { { start, start + te::TimeDuration::fromSeconds ((part.end - part.start) * settings.timeRatio) },
             te::TimeDuration::fromSeconds (from * settings.timeRatio) };
}

BeatRange getAudioClipRange (const te::Edit& edit, const juce::ValueTree& clipState)
{
    const auto start = getClipRange (clipState).getStart();
    return { start, fromTracktion (edit.tempoSequence.toBeats (getPlayedPosition (edit, clipState).getEnd())) };
}

void setAudioPart (te::Edit& edit, juce::ValueTree clipState, BeatPosition start, FilePart part)
{
    auto* um = getUndo (edit);
    clipState.setProperty (schema::clipStart, std::max (0.0, start.inBeats()), um);
    clipState.setProperty (schema::clipFileStart, std::max (0.0, part.start), um);
    clipState.setProperty (schema::clipFileEnd, std::max (part.start, part.end), um);
    mirror::reconcileClip (edit, getClipID (clipState));
}

double getClipFade (const juce::ValueTree& clipState, bool fadeIn)
{
    return (double) clipState.getProperty (fadeIn ? schema::clipFadeIn : schema::clipFadeOut, 0.0);
}

void setClipFade (te::Edit& edit, juce::ValueTree clipState, bool fadeIn, double seconds)
{
    const auto& property = fadeIn ? schema::clipFadeIn : schema::clipFadeOut;

    if (seconds > 0.0)
        clipState.setProperty (property, seconds, getUndo (edit));
    else
        clipState.removeProperty (property, getUndo (edit));

    mirror::reconcileClip (edit, getClipID (clipState));
}

FadeCurve getClipFadeCurve (const juce::ValueTree& clipState, bool fadeIn)
{
    const auto shape = clipState[fadeIn ? schema::clipFadeInShape : schema::clipFadeOutShape].toString();
    const auto bend = (float) clipState.getProperty (fadeIn ? schema::clipFadeInBend : schema::clipFadeOutBend, 0.0f);
    return { shape == schema::fadeIsSCurve ? FadeShape::sCurve : FadeShape::curve, juce::jlimit (-1.0f, 1.0f, bend) };
}

void setClipFadeCurve (te::Edit& edit, juce::ValueTree clipState, bool fadeIn, FadeCurve curve)
{
    // What is as at first isn't written: a straight curve.
    auto* um = getUndo (edit);
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

    mirror::reconcileClip (edit, getClipID (clipState));
}

te::AudioFadeCurve::Type getNearestFadeType (FadeCurve curve)
{
    if (std::abs (curve.bend) < 0.5f)
        return te::AudioFadeCurve::linear;

    if (curve.shape == FadeShape::sCurve)
        return curve.bend > 0.0f ? te::AudioFadeCurve::sCurve : te::AudioFadeCurve::linear;

    return curve.bend > 0.0f ? te::AudioFadeCurve::convex : te::AudioFadeCurve::concave;
}

engine::FadeCurve toEngineFade (FadeCurve curve)
{
    return { curve.shape == FadeShape::sCurve ? engine::FadeShape::sCurve : engine::FadeShape::curve, curve.bend };
}

//==============================================================================
juce::File getClipFile (const te::AudioClipBase& clip)
{
    if (const auto clipState = findClipState (clip); clipState.isValid())
        return getClipFile (clip.edit, clipState);

    return clip.getOriginalFile();
}

stretch::Settings getClipStretch (const te::AudioClipBase& clip)
{
    if (const auto clipState = findClipState (clip); clipState.isValid())
        return getClipStretch (clip.edit, clipState);

    return {};
}

bool isRendered (const te::AudioClipBase& clip)
{
    return ! stretch::isUnchanged (getClipStretch (clip));
}

juce::File getRenderFile (te::Edit& edit, const juce::File& source, const stretch::Settings& settings)
{
    const auto key = source.getFullPathName() + "|" + juce::String (settings.timeRatio, 9) + "|" + juce::String (settings.semitones, 4)
                     + "|" + (settings.reversed ? "backwards" : "forwards") + "|" + renderVersion;
    return edit.getTempDirectory (false).getChildFile (renderFilePrefix + juce::String::toHexString (key.hashCode64()) + ".wav");
}

te::TimeRange getAudioTimeRange (te::AudioClipBase& clip)
{
    const auto position = clip.getPosition();
    const auto start = position.getStart() - position.getOffset();
    return { start, start + te::TimeDuration::fromSeconds (getFileSeconds (getClipFile (clip)) * getClipStretch (clip).timeRatio) };
}

double getFileTimeAt (te::AudioClipBase& clip, te::BeatPosition beat)
{
    const auto position = clip.getPosition();
    const auto settings = getClipStretch (clip);
    const auto intoFile =
        (clip.edit.tempoSequence.toTime (beat) - (position.getStart() - position.getOffset())).inSeconds() / settings.timeRatio;
    return settings.reversed ? getFileSeconds (getClipFile (clip)) - intoFile : intoFile;
}

double getClipTempo (const te::AudioClipBase& clip)
{
    return (double) findClipState (clip).getProperty (schema::clipTempo, 0.0);
}

float getClipPitch (const te::AudioClipBase& clip)
{
    return (float) findClipState (clip).getProperty (schema::clipPitch, 0.0f);
}

bool isClipReversed (const te::AudioClipBase& clip)
{
    return (bool) findClipState (clip).getProperty (schema::clipReversed, false);
}

float getClipGain (const te::AudioClipBase& clip)
{
    return (float) findClipState (clip).getProperty (schema::clipGain, 0.0f);
}

void setClipGain (te::AudioClipBase& clip, float decibels)
{
    if (auto clipState = findClipState (clip); clipState.isValid())
    {
        const auto gain = juce::jlimit (minClipGain, maxClipGain, decibels);
        setOrRemove (clipState, schema::clipGain, juce::exactlyEqual (gain, 0.0f) ? juce::var() : juce::var (gain), getUndo (clip.edit));
        mirror::reconcileClip (clip.edit, getClipID (clipState));
    }
}

void setClipPitch (te::AudioClipBase& clip, float semitones)
{
    const auto pitch = juce::jlimit (-maxClipPitch, maxClipPitch, semitones);

    if (! juce::exactlyEqual (getClipPitch (clip), pitch))
        changeSound (clip, [pitch] (Sound& sound) { sound.pitch = pitch; });
}

void setClipReversed (te::AudioClipBase& clip, bool reversed)
{
    // The same part of the audio plays, backwards.
    if (isClipReversed (clip) != reversed)
        changeSound (clip, [reversed] (Sound& sound) { sound.reversed = reversed; });
}

void setClipFollowsTempo (te::AudioClipBase& clip, bool follows)
{
    const auto clipState = findClipState (clip);

    // Without its file, how long it lasts is unknown, and so is its tempo.
    if (! clipState.isValid() || (getClipTempo (clip) > 0.0) == follows || (follows && getFileSeconds (getClipFile (clip)) <= 0.0))
        return;

    const auto tempo = follows ? getFileTempo (clip.edit, getClipRange (clipState).getStart(), getClipFile (clip)) : 0.0;
    changeSound (clip, [tempo] (Sound& sound) { sound.tempo = tempo; });
}

void setClipTempo (te::AudioClipBase& clip, double bpm)
{
    if (getClipTempo (clip) <= 0.0)
        return;

    const auto tempo = juce::jlimit (minClipTempo, maxClipTempo, bpm);
    changeSound (clip, [tempo] (Sound& sound) { sound.tempo = tempo; });
}

int getClipTakeCount (const te::AudioClipBase& clip)
{
    return (int) findClipState (clip).getProperty (schema::clipTakes, 0);
}

int getClipTake (const te::AudioClipBase& clip)
{
    return (int) findClipState (clip).getProperty (schema::clipTake, 1);
}

void playTake (te::AudioClipBase& clip, int take)
{
    auto clipState = findClipState (clip);
    const auto playing = getClipTake (clip);

    if (getClipTakeCount (clip) < 2 || take < 1 || take > getClipTakeCount (clip) || take == playing)
        return;

    // The takes lie one after another in the clip's file, each as long as the range they were recorded round.
    const auto shift = (double) clipState[schema::clipTakeLength] * (take - playing);
    const auto part = getFilePart (clipState);
    clipState.setProperty (schema::clipTake, take, getUndo (clip.edit));
    setAudioPart (clip.edit, clipState, getClipRange (clipState).getStart(), { part.start + shift, part.end + shift });
}

juce::Array<te::AudioClipBase*> getAudioClips (te::Edit& edit)
{
    juce::Array<te::AudioClipBase*> clips;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
                clips.add (audio);

    return clips;
}

//==============================================================================
namespace
{
    // The audio clips among some clips.
    juce::Array<te::AudioClipBase*> getAudioClipsAmong (const juce::Array<te::Clip*>& clips)
    {
        juce::Array<te::AudioClipBase*> audio;

        for (auto* clip : clips)
            if (auto* audioClip = dynamic_cast<te::AudioClipBase*> (clip))
                audio.add (audioClip);

        return audio;
    }
} // namespace

bool areAllClipsReversed (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (getAudioClipsAmong (clips), [] (te::AudioClipBase* clip) { return isClipReversed (*clip); });
}

void toggleClipsReversed (const juce::Array<te::Clip*>& clips)
{
    toggleAll (
        getAudioClipsAmong (clips), [] (te::AudioClipBase* clip) { return isClipReversed (*clip); },
        [] (te::AudioClipBase* clip, bool on) { setClipReversed (*clip, on); });
}

bool doAllClipsFollowTempo (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (getAudioClipsAmong (clips), [] (te::AudioClipBase* clip) { return getClipTempo (*clip) > 0.0; });
}

void toggleClipsFollowTempo (const juce::Array<te::Clip*>& clips)
{
    toggleAll (
        getAudioClipsAmong (clips), [] (te::AudioClipBase* clip) { return getClipTempo (*clip) > 0.0; },
        [] (te::AudioClipBase* clip, bool on) { setClipFollowsTempo (*clip, on); });
}
} // namespace phazite
