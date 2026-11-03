// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioRecorder.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/AudioIO.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"

#include <map>

namespace phazite
{
namespace
{
    // How long a punch's edges cross-fade with what was there, in seconds.
    constexpr double crossfadeSeconds = 0.01;

    // A recording shorter than this keeps no clip, in seconds.
    constexpr double shortestRecording = 0.01;

    std::map<const te::Edit*, AudioRecorder*>& getRecorders()
    {
        static std::map<const te::Edit*, AudioRecorder*> recorders; // message thread only
        return recorders;
    }

    juce::String getInputOf (const juce::ValueTree& trackState)
    {
        return trackState.getProperty (schema::trackInput).toString();
    }

    // Whether the track hears its input always (schema::monitorAlways), or while it is armed (none), or never.
    bool alwaysHears (const juce::ValueTree& trackState)
    {
        return trackState.getProperty (schema::trackMonitoring).toString() == schema::monitorAlways;
    }
} // namespace

int AudioRecorder::getNumInputs() const
{
    if (auto* session = findPlayingSession (edit))
        return session->getNumInputs();

    auto* device = audio->getDevices().getCurrentAudioDevice();
    return device != nullptr ? device->getInputChannelNames().size() : 0;
}

std::vector<int> getInputChannels (const juce::String& inputID)
{
    std::vector<int> channels;

    for (const auto& part : juce::StringArray::fromTokens (inputID, "+", {}))
    {
        if (part.isEmpty() || ! part.containsOnly ("0123456789") || part.getIntValue() < 1)
            return {};

        channels.push_back (part.getIntValue() - 1);
    }

    // A channel alone, or a pair from an odd-numbered one (1+2, 3+4...).
    if (channels.size() > 2 || (channels.size() == 2 && (channels[0] % 2 != 0 || channels[1] != channels[0] + 1)))
        return {};

    return channels;
}

double getRecordingCorrection (te::Engine& engine)
{
    const auto* io = AudioIO::findFor (engine);
    return io != nullptr ? io->getRecordingCorrection() : 0.0;
}

void setRecordingCorrection (te::Engine& engine, double milliseconds)
{
    if (auto* io = AudioIO::findFor (engine))
        io->setRecordingCorrection (milliseconds);
}

//==============================================================================
AudioRecorder::AudioRecorder (te::Edit& e)
    : edit (e),
      engine (e.engine),
      audio (AudioIO::getFor (e.engine)),
      alive (e),
      key (&e),
      editState (e.state)
{
    getRecorders()[key] = this;
    editState.addListener (this);
    audio->addChangeListener (this);
    update();
}

AudioRecorder::~AudioRecorder()
{
    audio->removeChangeListener (this);
    editState.removeListener (this);
    armed.clear();
    update(); // what was opened for its tracks closes

    if (const auto found = getRecorders().find (key); found != getRecorders().end() && found->second == this)
        getRecorders().erase (found);
}

AudioRecorder* AudioRecorder::findFor (const te::Edit& edit)
{
    const auto found = getRecorders().find (&edit);
    return found != getRecorders().end() && found->second->alive.get() == &edit ? found->second : nullptr;
}

bool AudioRecorder::isArmed (const te::AudioTrack& track) const
{
    return isArmed (getTrackID (findTrackState (track)));
}

bool AudioRecorder::isArmed (TrackID track) const
{
    return armed.count (track) > 0;
}

void AudioRecorder::setArmed (te::AudioTrack& track, bool shouldArm)
{
    if (shouldArm && getInputChannels (getInputOf (findTrackState (track))).empty())
        return;

    if (const auto id = getTrackID (findTrackState (track)); shouldArm ? armed.insert (id).second : armed.erase (id) > 0)
    {
        update();

        if (auto* session = findSession (edit))
            session->update(); // the track hears its input while armed
    }
}

juce::Array<te::AudioTrack*> AudioRecorder::getArmedTracks() const
{
    juce::Array<te::AudioTrack*> tracks;

    for (auto* track : te::getAudioTracks (edit))
        if (isArmed (*track))
            tracks.add (track);

    return tracks;
}

bool AudioRecorder::canRecord() const
{
    const auto numInputs = getNumInputs();

    for (auto* track : getArmedTracks())
        if (const auto channels = getInputChannels (getInputOf (findTrackState (*track)));
            ! channels.empty() && channels.back() < numInputs)
            return true;

    return false;
}

//==============================================================================
void AudioRecorder::update()
{
    // While a take records, the inputs stay as they are: changing them opens the device again (Take.h updates them after).
    if (const auto* session = alive.get() != nullptr ? findSession (edit) : nullptr;
        updating || (session != nullptr && session->isRecording()))
        return;

    const juce::ScopedValueSetter<bool> svs (updating, true);
    auto& devices = audio->getDevices();
    auto* device = devices.getCurrentAudioDevice();
    const auto numInputs = device != nullptr ? device->getInputChannelNames().size() : 0;
    auto* liveEdit = alive.get(); // once the Edit is gone, its tracks need nothing

    // The channels the tracks need now: those of a track that is armed or always hears its input.
    juce::BigInteger wanted;

    for (auto* track : liveEdit != nullptr ? te::getAudioTracks (*liveEdit) : juce::Array<te::AudioTrack*>())
    {
        const auto trackState = findTrackState (*track);
        const auto channels = getInputChannels (getInputOf (trackState));

        if (channels.empty() || channels.back() >= numInputs)
            continue;

        if (isArmed (*track) || alwaysHears (trackState))
            for (auto channel : channels)
                wanted.setBit (channel);
    }

    // The audio device's inputs: those the tracks need opened, and those opened for them before and no longer needed
    // closed again; the ones the user opened in the audio settings stay. Only a change opens the device again: then its
    // channels are named, or JUCE would put back its default ones (none: Phazite opens no microphone by itself) whenever
    // it opened again.
    if (device != nullptr && numInputs > 0)
    {
        auto setup = devices.getAudioDeviceSetup();
        auto channels = setup.inputChannels;

        for (int channel = 0; channel < numInputs; ++channel)
        {
            if (wanted[channel])
                channels.setBit (channel);
            else if (openedForTracks[channel])
                channels.clearBit (channel);
        }

        if (channels != setup.inputChannels)
        {
            for (int channel = 0; channel < numInputs; ++channel)
                openedForTracks.setBit (channel, wanted[channel] && (openedForTracks[channel] || ! setup.inputChannels[channel]));

            setup.inputChannels = channels;
            setup.useDefaultInputChannels = false;
            devices.setAudioDeviceSetup (setup, true);
        }
    }
}

juce::File AudioRecorder::getTakeFile (const te::AudioTrack& track) const
{
    const auto folder = edit.engine.getEngineBehaviour().getDefaultFolderForAudioRecordings (edit);
    const auto name =
        juce::File::createLegalFileName (getTrackName (track).isNotEmpty() ? getTrackName (track) : juce::String ("Recording"));

    for (int take = 1;; ++take)
        if (const auto file = folder.getChildFile (name + " Take " + juce::String (take) + ".wav"); ! file.exists())
            return file;
}

void placeTake (te::Edit& edit, TrackID trackID, const juce::File& file, const TakePlace& take)
{
    const auto rate = take.sampleRate;
    auto* found = dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, toTracktion (trackID)));

    if (found == nullptr)
    {
        file.deleteFile();
        return;
    }

    auto& track = *found;

    // Where in the song, as if nothing looped, each frame of the file was played; and the frame played at a time.
    const auto timeOf = [&] (double frame) { return take.firstFrame + frame / rate; };
    const auto frameOf = [&] (double seconds) { return (seconds - take.firstFrame) * rate; };

    const auto from = std::max (take.start, timeOf (0.0));
    const auto to = std::min (take.stop, timeOf ((double) take.frames));

    if (take.frames <= 0 || to - from < shortestRecording)
    {
        file.deleteFile();
        return;
    }

    const auto name = getTrackName (track);
    const auto& tempo = edit.tempoSequence;
    const auto toBeats = [&tempo] (double seconds) { return fromTracktion (tempo.toBeats (te::TimePosition::fromSeconds (seconds))); };

    // A clip of the song's playing the file from a time in it, as it was recorded.
    const auto place = [&] (double clipStart, double clipEnd, double fileStart) -> te::AudioClipBase*
    {
        const auto partStart = std::max (0.0, fileStart);
        const auto clipState = makeAudioClipState (edit, name, file, toBeats (clipStart), { partStart, partStart + (clipEnd - clipStart) });
        return dynamic_cast<te::AudioClipBase*> (placeClipState (edit, clipState, track));
    };

    // Punched in: only what falls in the range, in place of what was there, cross-fading at the edges: the new clip
    // reaches half a fade past each, fading in and out over what was there, which fades out and in where it is cut.
    if (take.punch.has_value())
    {
        const auto in = std::max (from, take.punch->getStart());
        const auto out = std::min (to, take.punch->getEnd());

        if (out - in < shortestRecording)
        {
            file.deleteFile();
            return;
        }

        const auto half = crossfadeSeconds / 2.0;
        const auto clipStart = std::max (from, in - half), clipEnd = std::min (to, out + half);
        clearClipsIn (track, toTracktion (BeatRange (toBeats (clipStart + crossfadeSeconds), toBeats (clipEnd - crossfadeSeconds))));
        const auto crossfade = te::TimeDuration::fromSeconds (crossfadeSeconds);

        for (auto* clip : track.getClips())
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                const auto range = audio->getPosition().time;

                if (std::abs (range.getEnd().inSeconds() - (clipStart + crossfadeSeconds)) < 1.0e-6)
                    setClipFade (*audio, false, crossfade);

                if (std::abs (range.getStart().inSeconds() - (clipEnd - crossfadeSeconds)) < 1.0e-6)
                    setClipFade (*audio, true, crossfade);
            }

        if (auto* clip = place (clipStart, clipEnd, frameOf (clipStart) / rate))
        {
            setClipFade (*clip, true, crossfade);
            setClipFade (*clip, false, crossfade);
        }

        return;
    }

    // Round a looped range: a take for each time round that began within the recording, in one clip over the range, the
    // last whole one playing (or the last one, if none was whole). Once round or less, a clip of what was played.
    if (take.loop.has_value() && take.loop->getLength() > 0.0)
    {
        const auto loopStart = take.loop->getStart(), length = take.loop->getLength();
        const auto firstLoopStart = take.firstLoopStart; // where the first time round began
        const auto passOf = [&] (double seconds) { return (int) std::floor ((seconds - firstLoopStart) / length + 1.0e-9); };

        if (passOf (to - 1.0e-6) > passOf (from))
        {
            std::vector<int> takes; // the times round that began within the recording

            for (auto pass = passOf (from); pass <= passOf (to - 1.0e-6); ++pass)
                if (frameOf (firstLoopStart + pass * length) >= 0.0 && firstLoopStart + pass * length < to)
                    takes.push_back (pass);

            if (! takes.empty())
            {
                auto playing = takes.back();

                for (auto pass : takes)
                    if (firstLoopStart + (pass + 1) * length <= to + 1.0e-6)
                        playing = pass;

                const auto passStart = firstLoopStart + playing * length;

                if (auto* clip = place (loopStart, loopStart + length, frameOf (passStart) / rate))
                {
                    auto clipState = findClipState (*clip);
                    clipState.setProperty (schema::clipTakes, (int) takes.size(), &getSong (edit).getUndoManager());
                    clipState.setProperty (schema::clipTake, (int) (std::find (takes.begin(), takes.end(), playing) - takes.begin()) + 1,
                                           &getSong (edit).getUndoManager());
                    clipState.setProperty (schema::clipTakeLength, length, &getSong (edit).getUndoManager());
                }

                return;
            }
        }

        // Not round once: where it was played in the range.
        const auto offsetInLoop = loopStart - firstLoopStart - passOf (from) * length;
        place (from + offsetInLoop, to + offsetInLoop, frameOf (from) / rate);
        return;
    }

    place (from, to, frameOf (from) / rate);
}

//==============================================================================
void AudioRecorder::changeListenerCallback (juce::ChangeBroadcaster*)
{
    update(); // the audio device changed
}

void AudioRecorder::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if ((property == schema::trackInput || property == schema::trackMonitoring) && tree.hasType (schema::TRACK) && alive.get() != nullptr)
    {
        // A track left without an input can't stay armed.
        if (getInputChannels (tree[schema::trackInput].toString()).empty())
            armed.erase (getTrackID (tree));

        update();
    }
}

void AudioRecorder::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (schema::TRACK))
    {
        armed.erase (getTrackID (child));
        update();
    }
}
} // namespace phazite
