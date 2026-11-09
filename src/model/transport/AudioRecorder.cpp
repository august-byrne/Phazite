// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/AudioRecorder.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AppInfo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // How long a punch's edges cross-fade with what was there, in seconds.
    constexpr double crossfadeSeconds = 0.01;

    // A recording shorter than this keeps no clip, in seconds.
    constexpr double shortestRecording = 0.01;

    juce::String getInputOf (const juce::ValueTree& trackState)
    {
        return trackState.getProperty (schema::trackInput).toString();
    }

    // Whether the track hears its input always (schema::monitorAlways), or while it is armed (none), or never.
    bool alwaysHears (const juce::ValueTree& trackState)
    {
        return trackState.getProperty (schema::trackMonitoring).toString() == schema::monitorAlways;
    }

    // A track that can have an input: a track, not a folder (nor the master).
    bool hasInputs (const juce::ValueTree& trackState)
    {
        return trackState.hasType (schema::TRACK) && ! isFolderState (trackState);
    }
} // namespace

juce::File getRecordingsFolder (const Song& song)
{
    if (const auto& file = song.getFile(); file != juce::File())
        return file.getSiblingFile ("Audio");

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Recordings");
}

bool canRecordAudio (const Song& song, TrackID track)
{
    const auto state = findTrackState (song, track);
    return hasInputs (state) && ! isReturnState (state) && ! hasInstrument (song, track);
}

int AudioRecorder::getNumInputs() const
{
    if (session.isPlayingProject())
        return session.getNumInputs();

    auto* device = audio.getDevices().getCurrentAudioDevice();
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

//==============================================================================
AudioRecorder::AudioRecorder (PlaybackSession& s)
    : session (s),
      song (s.getSong()),
      audio (s.getAudioIO()),
      songState (song.getState())
{
    songState.addListener (this);
    audio.addChangeListener (this);
    update();
}

AudioRecorder::~AudioRecorder()
{
    audio.removeChangeListener (this);
    songState.removeListener (this);
    armed.clear();
    closing = true;
    update(); // what was opened for its tracks closes
}

bool AudioRecorder::isArmed (TrackID track) const
{
    return armed.count (track) > 0;
}

void AudioRecorder::setArmed (TrackID track, bool shouldArm)
{
    if (const auto state = findTrackState (song, track);
        shouldArm && (! hasInputs (state) || getInputChannels (getInputOf (state)).empty()))
        return;

    if (shouldArm ? armed.insert (track).second : armed.erase (track) > 0)
    {
        update();
        session.update(); // the track hears its input while armed
    }
}

std::vector<TrackID> AudioRecorder::getArmedTracks() const
{
    std::vector<TrackID> tracks;

    for (const auto& trackState : getTrackStates (song))
        if (const auto id = getTrackID (trackState); hasInputs (trackState) && isArmed (id))
            tracks.push_back (id);

    return tracks;
}

bool AudioRecorder::canRecord() const
{
    const auto numInputs = getNumInputs();

    for (const auto track : getArmedTracks())
        if (const auto channels = getInputChannels (getInputOf (findTrackState (song, track)));
            ! channels.empty() && channels.back() < numInputs)
            return true;

    return false;
}

//==============================================================================
void AudioRecorder::update()
{
    // While a take records, the inputs stay as they are: changing them opens the device again (Take.h updates them after).
    if (updating || session.isRecording())
        return;

    const juce::ScopedValueSetter<bool> svs (updating, true);
    auto& devices = audio.getDevices();
    auto* device = devices.getCurrentAudioDevice();
    const auto numInputs = device != nullptr ? device->getInputChannelNames().size() : 0;

    // The channels the tracks need now: those of a track that is armed or always hears its input (none as the recorder
    // goes).
    juce::BigInteger wanted;

    for (const auto& trackState : closing ? std::vector<juce::ValueTree>() : getTrackStates (song))
    {
        const auto channels = hasInputs (trackState) ? getInputChannels (getInputOf (trackState)) : std::vector<int>();

        if (channels.empty() || channels.back() >= numInputs)
            continue;

        if (isArmed (getTrackID (trackState)) || alwaysHears (trackState))
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

juce::File AudioRecorder::getTakeFile (TrackID track) const
{
    const auto folder = getRecordingsFolder (song);
    const auto trackName = getTrackName (song, track);
    const auto name = juce::File::createLegalFileName (trackName.isNotEmpty() ? trackName : juce::String ("Recording"));

    for (int take = 1;; ++take)
        if (const auto file = folder.getChildFile (name + " Take " + juce::String (take) + ".wav"); ! file.exists())
            return file;
}

void placeTake (Song& song, TrackID track, const juce::File& file, const TakePlace& take)
{
    const auto rate = take.sampleRate;

    if (const auto state = findTrackState (song, track); ! state.hasType (schema::TRACK) || isFolderState (state))
    {
        file.deleteFile();
        return;
    }

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

    const auto name = getTrackName (song, track);
    const auto beatsOf = [&song] (double seconds) { return toBeats (song, seconds); };

    // A clip of the song's playing the file from a time in it, as it was recorded: following the song's tempo from the tempo it
    // was played at, as Ableton Live warps a recording, while the tempo holds steady over it, so that it keeps to its beats
    // when the tempo changes later (one played along tempo changes keeps its own speed).
    const auto place = [&] (double clipStart, double clipEnd, double fileStart)
    {
        const auto partStart = std::max (0.0, fileStart);
        const auto clipState = makeAudioClipState (song, name, file, beatsOf (clipStart), { partStart, partStart + (clipEnd - clipStart) });
        const auto clip = placeClipState (song, clipState, track);

        if (clip.isValid() && getTempoMap (song).isSteadyBetween (beatsOf (clipStart).inBeats(), beatsOf (clipEnd).inBeats()))
            setClipFollowsTempo (song, clip, true);

        return clip;
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
        clearClipsIn (song, track, BeatRange (beatsOf (clipStart + crossfadeSeconds), beatsOf (clipEnd - crossfadeSeconds)));

        for (const auto& clipState : getClipStates (findTrackState (song, track)))
        {
            if (! isAudioState (clipState))
                continue;

            const auto position = getPlayedPosition (song, clipState);

            if (std::abs (position.end - (clipStart + crossfadeSeconds)) < 1.0e-6)
                setClipFade (song, getClipID (clipState), false, crossfadeSeconds);

            if (std::abs (position.start - (clipEnd - crossfadeSeconds)) < 1.0e-6)
                setClipFade (song, getClipID (clipState), true, crossfadeSeconds);
        }

        if (const auto clip = place (clipStart, clipEnd, frameOf (clipStart) / rate); clip.isValid())
        {
            setClipFade (song, clip, true, crossfadeSeconds);
            setClipFade (song, clip, false, crossfadeSeconds);
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

                if (const auto clip = place (loopStart, loopStart + length, frameOf (passStart) / rate); clip.isValid())
                {
                    auto clipState = findClipState (song, clip);
                    auto* undo = &song.getUndoManager();
                    clipState.setProperty (schema::clipTakes, (int) takes.size(), undo);
                    clipState.setProperty (schema::clipTake, (int) (std::find (takes.begin(), takes.end(), playing) - takes.begin()) + 1,
                                           undo);
                    clipState.setProperty (schema::clipTakeLength, length, undo);
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
    if ((property == schema::trackInput || property == schema::trackMonitoring) && tree.hasType (schema::TRACK))
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
        // The track, and those in it if it was a folder.
        std::erase_if (armed, [this] (TrackID track) { return ! findTrackState (song, track).isValid(); });
        update();
    }
}
} // namespace phazite
