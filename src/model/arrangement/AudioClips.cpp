// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioClips.h"
#include "AudioClipModel.h"
#include "model/base/Toggles.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // How long the part of the clip's file that its loop data counts beats over lasts: between the data's in and out
    // markers, which hold all of the file unless they are set. From the file itself: a reversed copy, which is as long,
    // may still be on its way.
    double getMarkedSeconds (te::AudioClipBase& clip)
    {
        const auto file = te::AudioFile (clip.edit.engine, clip.getOriginalFile()).getInfo();
        const auto& loop = clip.getLoopInfo();
        auto out = loop.getOutMarker();

        if (out < 0 || out > file.lengthInSamples)
            out = file.lengthInSamples;

        return file.sampleRate > 0.0 ? (double) (out - loop.getInMarker()) / file.sampleRate : 0.0;
    }

    // The file's beats in a second, as its loop data counts them; 0 without a tempo.
    double getFileBeatsPerSecond (te::AudioClipBase& clip)
    {
        const auto seconds = getMarkedSeconds (clip);
        const auto beats = clip.getLoopInfo().getNumBeats();
        return seconds > 0.0 && beats > 0.0 ? beats / seconds : 0.0;
    }

    // Whether the clip stretches to the song's tempo now: it follows it, from a tempo of its own.
    bool isStretchedToTempo (te::AudioClipBase& clip)
    {
        return clip.getAutoTempo() && getFileBeatsPerSecond (clip) > 0.0;
    }

    // The song's beats in a second where the clip starts, as Tracktion counts a clip's offset in beats.
    double getSongBeatsPerSecond (const te::AudioClipBase& clip)
    {
        return clip.edit.tempoSequence.getBeatsPerSecondAt (clip.getPosition().getStart());
    }

    // The part of its file that the clip plays: from how many seconds into the file, for how many seconds of it.
    struct FilePart
    {
        double start = 0.0, length = 0.0;
    };

    FilePart getFilePart (te::AudioClipBase& clip)
    {
        // Stretched, the clip's offset and length count beats of its file.
        if (isStretchedToTempo (clip))
        {
            const auto fileBps = getFileBeatsPerSecond (clip);
            return { clip.getOffsetInBeats().inBeats() / fileBps, clip.getLengthInBeats().inBeats() / fileBps };
        }

        const auto position = clip.getPosition();
        const auto speed = clip.getSpeedRatio();
        return { position.getOffset().inSeconds() * speed, position.getLength().inSeconds() * speed };
    }

    // Places the clip, from where it starts, so that it plays that part of its file as it plays its audio now.
    void playFilePart (te::AudioClipBase& clip, FilePart part)
    {
        const auto start = clip.getPosition().getStart();

        if (isStretchedToTempo (clip))
        {
            const auto fileBps = getFileBeatsPerSecond (clip);
            const auto end = clip.edit.tempoSequence.toTime (clip.getStartBeat() + te::BeatDuration::fromBeats (part.length * fileBps));
            clip.setPosition ({ { start, end }, te::TimeDuration::fromSeconds (part.start * fileBps / getSongBeatsPerSecond (clip)) });
            return;
        }

        const auto speed = clip.getSpeedRatio();
        clip.setPosition (
            { { start, start + te::TimeDuration::fromSeconds (part.length / speed) }, te::TimeDuration::fromSeconds (part.start / speed) });
    }

    // Gives the file's loop data a tempo: as many beats between its markers as that tempo plays in their time.
    void setFileTempo (te::AudioClipBase& clip, double bpm)
    {
        if (const auto seconds = getMarkedSeconds (clip); seconds > 0.0)
            clip.getLoopInfo().setNumBeats (bpm / 60.0 * seconds);
    }

    // The audio clips among some clips.
    juce::Array<te::AudioClipBase*> getAudioClips (const juce::Array<te::Clip*>& clips)
    {
        juce::Array<te::AudioClipBase*> audio;

        for (auto* clip : clips)
            if (auto* audioClip = dynamic_cast<te::AudioClipBase*> (clip))
                audio.add (audioClip);

        return audio;
    }
} // namespace

te::TimeRange getAudioTimeRange (te::AudioClipBase& clip)
{
    if (isStretchedToTempo (clip))
    {
        const auto& tempo = clip.edit.tempoSequence;
        const auto start = clip.getStartBeat() - clip.getOffsetInBeats();
        const auto beats = te::BeatDuration::fromBeats (getMarkedSeconds (clip) * getFileBeatsPerSecond (clip));
        return { tempo.toTime (start), tempo.toTime (start + beats) };
    }

    const auto start = clip.getPosition().getStartOfSource();
    return { start, start + clip.getSourceLength() / clip.getSpeedRatio() };
}

double getFileTimeAt (te::AudioClipBase& clip, te::BeatPosition beat)
{
    // Stretched, from the loop data's in marker, the beats of the file at its own tempo (as Tracktion's AudioSegmentList
    // lays them out); otherwise from its start, at the clip's speed.
    if (isStretchedToTempo (clip))
    {
        const auto file = te::AudioFile (clip.edit.engine, clip.getOriginalFile()).getInfo();
        const auto inMarker = file.sampleRate > 0.0 ? (double) clip.getLoopInfo().getInMarker() / file.sampleRate : 0.0;
        return inMarker + (beat - (clip.getStartBeat() - clip.getOffsetInBeats())).inBeats() / getFileBeatsPerSecond (clip);
    }

    return (clip.edit.tempoSequence.toTime (beat) - clip.getPosition().getStartOfSource()).inSeconds() * clip.getSpeedRatio();
}

double getClipTempo (te::AudioClipBase& clip)
{
    return isStretchedToTempo (clip) ? getFileBeatsPerSecond (clip) * 60.0 : 0.0;
}

void setClipGain (te::AudioClipBase& clip, float decibels)
{
    clip.setGainDB (juce::jlimit (minClipGain, maxClipGain, decibels));
}

void setClipPitch (te::AudioClipBase& clip, float semitones)
{
    clip.setPitchChange (juce::jlimit (-maxClipPitch, maxClipPitch, semitones));
}

void setClipReversed (te::AudioClipBase& clip, bool reversed)
{
    // Tracktion plays a reversed copy of the file, which it renders in the background, and moves the clip's offset so that
    // the same part of the audio plays.
    if (clip.getIsReversed() != reversed)
        clip.setIsReversed (reversed);
}

void setClipFollowsTempo (te::AudioClipBase& clip, bool follows)
{
    // Without its file, how long it lasts is unknown, and so is its tempo.
    if (clip.getAutoTempo() == follows || (follows && getMarkedSeconds (clip) <= 0.0))
        return;

    const auto part = getFilePart (clip);

    if (follows)
    {
        // Without a tempo from its file, the clip plays at the song's tempo where it starts, as it sounds now.
        if (getFileBeatsPerSecond (clip) <= 0.0)
            setFileTempo (clip, getSongBeatsPerSecond (clip) * 60.0);

        auto& loop = clip.getLoopInfo();

        if (loop.getNumerator() <= 0 || loop.getDenominator() <= 0)
        {
            const auto& signature = clip.edit.tempoSequence.getTimeSigAt (clip.getPosition().getStart());
            loop.setNumerator (signature.numerator);
            loop.setDenominator (signature.denominator);
        }
    }

    clip.setAutoTempo (follows);
    playFilePart (clip, part);
}

void setClipTempo (te::AudioClipBase& clip, double bpm)
{
    if (! isStretchedToTempo (clip))
        return;

    const auto part = getFilePart (clip);
    setFileTempo (clip, juce::jlimit (minClipTempo, maxClipTempo, bpm));
    playFilePart (clip, part);
}

//==============================================================================
bool areAllClipsReversed (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (getAudioClips (clips), [] (te::AudioClipBase* clip) { return clip->getIsReversed(); });
}

void toggleClipsReversed (const juce::Array<te::Clip*>& clips)
{
    toggleAll (
        getAudioClips (clips), [] (te::AudioClipBase* clip) { return clip->getIsReversed(); },
        [] (te::AudioClipBase* clip, bool on) { setClipReversed (*clip, on); });
}

bool doAllClipsFollowTempo (const juce::Array<te::Clip*>& clips)
{
    return areAllOn (getAudioClips (clips), [] (te::AudioClipBase* clip) { return clip->getAutoTempo(); });
}

void toggleClipsFollowTempo (const juce::Array<te::Clip*>& clips)
{
    toggleAll (
        getAudioClips (clips), [] (te::AudioClipBase* clip) { return clip->getAutoTempo(); },
        [] (te::AudioClipBase* clip, bool on) { setClipFollowsTempo (*clip, on); });
}
} // namespace phazite
