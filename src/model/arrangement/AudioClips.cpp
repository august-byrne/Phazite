// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioClips.h"
#include "AudioClipModel.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Bumped whenever renders would come out differently, so that a project's old ones aren't played.
    constexpr const char* renderVersion = "rubberband-4.0-r3-1";

    // What a clip's sound is made from, and how: its file and its Phazite properties (Schema.h).
    struct Sound
    {
        juce::File file;
        double tempo = 0.0; // the file's, while the clip follows the song's; 0 while it doesn't
        float pitch = 0.0f;
        bool reversed = false;
    };

    Sound getSound (const te::AudioClipBase& clip)
    {
        return { getClipFile (clip), getClipTempo (clip), getClipPitch (clip), isClipReversed (clip) };
    }

    double getSongTempoAt (const te::AudioClipBase& clip)
    {
        return clip.edit.tempoSequence.getBeatsPerSecondAt (clip.getPosition().getStart()) * 60.0;
    }

    stretch::Settings getSettings (const Sound& sound, double songTempo)
    {
        return { sound.tempo > 0.0 && songTempo > 0.0 ? sound.tempo / songTempo : 1.0, (double) sound.pitch, sound.reversed };
    }

    double getFileSeconds (const te::Edit& edit, const juce::File& file)
    {
        return te::AudioFile (edit.engine, file).getLength();
    }

    // The part of its file that the clip plays, in seconds of the file as it was recorded.
    struct FilePart
    {
        double start = 0.0, end = 0.0;
    };

    FilePart getFilePart (const te::AudioClipBase& clip, const Sound& sound, const stretch::Settings& settings)
    {
        const auto position = clip.getPosition();
        const auto from = position.getOffset().inSeconds() / settings.timeRatio;
        const auto to = (position.getOffset() + position.getLength()).inSeconds() / settings.timeRatio;

        if (! sound.reversed)
            return { from, to };

        const auto length = getFileSeconds (clip.edit, sound.file);
        return { length - to, length - from };
    }

    // Lays the clip out, from where it starts, so that it plays that part of its file as it plays its sound now.
    void playFilePart (te::AudioClipBase& clip, const Sound& sound, const stretch::Settings& settings, FilePart part)
    {
        const auto start = clip.getPosition().getStart();
        const auto from = sound.reversed ? getFileSeconds (clip.edit, sound.file) - part.end : part.start;
        clip.setPosition ({ { start, start + te::TimeDuration::fromSeconds ((part.end - part.start) * settings.timeRatio) },
                            te::TimeDuration::fromSeconds (from * settings.timeRatio) });
    }

    void setOrRemove (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value, juce::UndoManager* um)
    {
        if (value.isVoid())
            state.removeProperty (property, um);
        else
            state.setProperty (property, value, um);
    }

    // Writes the sound into the clip's properties, and points Tracktion at what plays it: its file, or a render of it.
    void setSound (te::AudioClipBase& clip, const Sound& sound)
    {
        auto* um = &clip.edit.getUndoManager();
        auto& state = clip.state;
        setOrRemove (state, schema::phaziteTempo, sound.tempo > 0.0 ? juce::var (sound.tempo) : juce::var(), um);
        setOrRemove (state, schema::phazitePitch, juce::exactlyEqual (sound.pitch, 0.0f) ? juce::var() : juce::var (sound.pitch), um);
        setOrRemove (state, schema::phaziteReversed, sound.reversed ? juce::var (true) : juce::var(), um);

        const auto settings = getSettings (sound, getSongTempoAt (clip));
        const auto played = stretch::isUnchanged (settings) ? sound.file : getRenderFile (clip.edit, sound.file, settings);
        setOrRemove (
            state, schema::phaziteSource,
            played == sound.file ? juce::var() : juce::var (te::SourceFileReference::findPathFromFile (clip.edit, sound.file, true)), um);

        if (auto& source = clip.getSourceFileReference(); source.getFile() != played)
            source.source = te::SourceFileReference::findPathFromFile (clip.edit, played, true);
    }

    // Changes how a clip sounds, keeping its start and the part of its file it plays: on each clip it is linked with too,
    // which Tracktion would otherwise only partly mirror.
    template <typename Change>
    void changeSound (te::AudioClipBase& clip, Change&& change)
    {
        auto linked = clip.isLinked() ? clip.edit.findClipsInLinkGroup (clip.getLinkGroupID())
                                      : juce::Array<te::Clip*> { static_cast<te::Clip*> (&clip) };

        for (auto* other : linked)
        {
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (other))
            {
                auto sound = getSound (*audio);
                const auto part = getFilePart (*audio, sound, getSettings (sound, getSongTempoAt (*audio)));
                change (sound);
                setSound (*audio, sound);
                playFilePart (*audio, sound, getSettings (sound, getSongTempoAt (*audio)), part);
            }
        }
    }

    // The tempo a file was played at: from its loop data, or else the song's where the clip starts, so that it sounds as it
    // does until either tempo changes.
    double getFileTempo (const te::AudioClipBase& clip, const juce::File& file)
    {
        const auto info = te::AudioFile (clip.edit.engine, file).getInfo();
        const auto& loop = info.loopInfo;
        auto out = loop.getOutMarker();

        if (out < 0 || out > info.lengthInSamples)
            out = info.lengthInSamples;

        const auto seconds = info.sampleRate > 0.0 ? (double) (out - loop.getInMarker()) / info.sampleRate : 0.0;
        const auto tempo = seconds > 0.0 && loop.getNumBeats() > 0.0 ? loop.getNumBeats() / seconds * 60.0 : getSongTempoAt (clip);
        return juce::jlimit (minClipTempo, maxClipTempo, tempo);
    }
} // namespace

juce::File getClipFile (const te::AudioClipBase& clip)
{
    if (const auto source = clip.state.getProperty (schema::phaziteSource).toString(); source.isNotEmpty())
        return te::SourceFileReference::findFileFromString (clip.edit, source);

    return clip.getOriginalFile();
}

stretch::Settings getClipStretch (const te::AudioClipBase& clip)
{
    return getSettings (getSound (clip), getSongTempoAt (clip));
}

bool isRendered (const te::AudioClipBase& clip)
{
    return clip.state.hasProperty (schema::phaziteSource);
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
    return { start,
             start + te::TimeDuration::fromSeconds (getFileSeconds (clip.edit, getClipFile (clip)) * getClipStretch (clip).timeRatio) };
}

double getFileTimeAt (te::AudioClipBase& clip, te::BeatPosition beat)
{
    const auto position = clip.getPosition();
    const auto settings = getClipStretch (clip);
    const auto intoFile =
        (clip.edit.tempoSequence.toTime (beat) - (position.getStart() - position.getOffset())).inSeconds() / settings.timeRatio;
    return settings.reversed ? getFileSeconds (clip.edit, getClipFile (clip)) - intoFile : intoFile;
}

double getClipTempo (const te::AudioClipBase& clip)
{
    return (double) clip.state.getProperty (schema::phaziteTempo, 0.0);
}

float getClipPitch (const te::AudioClipBase& clip)
{
    return (float) clip.state.getProperty (schema::phazitePitch, 0.0f);
}

bool isClipReversed (const te::AudioClipBase& clip)
{
    return (bool) clip.state.getProperty (schema::phaziteReversed, false);
}

void setClipGain (te::AudioClipBase& clip, float decibels)
{
    clip.setGainDB (juce::jlimit (minClipGain, maxClipGain, decibels));
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
    // Without its file, how long it lasts is unknown, and so is its tempo.
    if ((getClipTempo (clip) > 0.0) == follows || (follows && getFileSeconds (clip.edit, getClipFile (clip)) <= 0.0))
        return;

    const auto tempo = follows ? getFileTempo (clip, getClipFile (clip)) : 0.0;
    changeSound (clip, [tempo] (Sound& sound) { sound.tempo = tempo; });
}

void setClipTempo (te::AudioClipBase& clip, double bpm)
{
    if (getClipTempo (clip) <= 0.0)
        return;

    const auto tempo = juce::jlimit (minClipTempo, maxClipTempo, bpm);
    changeSound (clip, [tempo] (Sound& sound) { sound.tempo = tempo; });
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
FollowingClipsOnBeats::FollowingClipsOnBeats (te::Edit& edit)
{
    for (auto* clip : getAudioClips (edit))
    {
        if (const auto sound = getSound (*clip); sound.tempo > 0.0)
        {
            const auto part = getFilePart (*clip, sound, getSettings (sound, getSongTempoAt (*clip)));
            kept.push_back ({ clip, part.start, part.end });
        }
    }
}

FollowingClipsOnBeats::~FollowingClipsOnBeats()
{
    for (auto& [clip, fileStart, fileEnd] : kept)
    {
        if (auto* audio = clip.get())
        {
            const auto sound = getSound (*audio);
            setSound (*audio, sound);
            playFilePart (*audio, sound, getSettings (sound, getSongTempoAt (*audio)), { fileStart, fileEnd });
        }
    }
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
