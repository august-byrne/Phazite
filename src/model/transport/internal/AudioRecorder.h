// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio inputs of one song (docs/design.md, section 17; roadmap step 3.4), the model's side of RecordingModel.h, which
// the song's playback session owns (model/playback/internal/PlaybackSession.h): which tracks are armed, and the audio
// device's inputs opened while a track is armed or always hears its input (and closed otherwise, so that no microphone is
// open for nothing). Phazite's engine plays what comes in and records it (Take.h), and placeTake() puts what a take
// recorded where it was played. Message thread only.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <optional>
#include <set>
#include <vector>

namespace phazite
{
class AudioIO;
class PlaybackSession;

class AudioRecorder : private juce::ChangeListener,     // Phazite's devices (AudioIO.h), as the audio device changes
                      private juce::ValueTree::Listener // the tracks' inputs and monitoring
{
public:
    // The recorder of a session's song, on the session's audio device: the session makes it as it starts.
    explicit AudioRecorder (PlaybackSession&);
    ~AudioRecorder() override; // what it opened for the tracks closes

    // Arming (RecordingModel.h): not saved, not an undo step. A track without an input can't be armed. A change brings the
    // inputs in line, and gives the session's engine the song again, where an armed track hears its input.
    bool isArmed (TrackID) const;
    void setArmed (TrackID, bool);
    std::vector<TrackID> getArmedTracks() const; // in the song's order

    // Whether Record would record audio: a track is armed, with an input the audio device has.
    bool canRecord() const;

    // The audio device's inputs: those Phazite's engine is given while it plays the song, or else the device's.
    int getNumInputs() const;

    // Brings the inputs in line with the tracks now, as it does by itself when they change, but while a take records.
    void update();

    // The file a take of a track's input goes into: in the song's recordings folder (getRecordingsFolder()), named after
    // the track, a take not there yet.
    juce::File getTakeFile (TrackID) const;

private:
    PlaybackSession& session;
    Song& song;
    AudioIO& audio;
    juce::ValueTree songState;
    std::set<TrackID> armed;
    juce::BigInteger openedForTracks; // the audio device's inputs opened for the tracks, which close when they no longer need them
    bool updating = false;
    bool closing = false; // as it goes: its tracks need nothing more

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;

    JUCE_DECLARE_NON_COPYABLE (AudioRecorder)
};

// Where a song's recordings go: its project's Audio folder, beside the project file, or for a song without a file,
// Documents/Phazite/Recordings.
juce::File getRecordingsFolder (const Song&);

// Whether a track can record audio: a track, not a folder or a return, without an instrument.
bool canRecordAudio (const Song&, TrackID);

// Where a take's file goes in the song: where its file's first frame was played, and when recording started and stopped,
// as if nothing looped, in seconds; the range a punched take keeps; and the range it went round, with where, as if nothing
// looped, the first time round began.
struct TakePlace
{
    double sampleRate = 44100.0;
    juce::int64 frames = 0; // in the file
    double firstFrame = 0.0, start = 0.0, stop = 0.0;
    std::optional<juce::Range<double>> punch, loop;
    double firstLoopStart = 0.0;
};

// Makes the clips of a track's take, in the caller's undo step: what was played where it was played; punched in, only
// what falls in the range, in place of what was there, cross-fading at its edges; round a looped range, a take for each
// time round, in one clip over the range, the last whole one playing. The file goes if it keeps nothing.
void placeTake (Song&, TrackID, const juce::File&, const TakePlace&);

// The channels of the audio device's inputs that a track's input names ("1" or "1+2"), from 0; none for an id that names
// none.
std::vector<int> getInputChannels (const juce::String& inputID);
} // namespace phazite
