// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio inputs of one song (docs/design.md, section 17; roadmap step 3.4), the model's side of RecordingModel.h, which
// the song's playback session owns (model/playback/internal/PlaybackSession.h): which tracks are armed, and the audio
// device's inputs opened while a track is armed or always hears its input (and closed otherwise, so that no microphone is
// open for nothing). Phazite's engine plays what comes in and records it (Take.h), and placeTake() puts what a take
// recorded where it was played. Message thread only.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <cstdint>
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

// A range a take was recorded round, in the song's samples as Phazite's engine plays it, and the one rule for which time
// round a moment of the take is on and where in the song it was played, for its audio, its notes and what shows while it
// records. Moments are counted as if nothing looped, from where recording started. The engine plays on from there to the
// range's end before it first goes round (engine::Engine::process()), so the first time round (0) holds what was played
// before the range too, where it was played; each time round after it is as long as the range, and folds into it.
struct TakeLoop
{
    std::int64_t start = 0, end = 0;

    std::int64_t getLength() const noexcept { return end - start; }

    int getRound (std::int64_t moment) const noexcept { return moment < end ? 0 : (int) ((moment - start) / getLength()); }
    std::int64_t fold (std::int64_t moment) const noexcept { return moment - getRound (moment) * getLength(); }

    // Where a time round reaches the range's start, as if nothing looped (the first time round may begin before it).
    std::int64_t getRoundStart (int pass) const noexcept { return start + pass * getLength(); }
};

// Where a take's file goes in the song, in the song's samples as if nothing looped: where its file's first frame was played,
// and where recording started and stopped; the range a punched take keeps; and the range it went round. Round a looped range
// that recording started within, the file starts with silence from the range's start (lead) up to where what came in first
// was played, so that every time round is a take that lies where the range does, the first too.
struct TakePlace
{
    double sampleRate = 44100.0;
    std::int64_t frames = 0; // in the file, the lead's included
    std::int64_t lead = 0;   // of them, the silence before what came in first
    std::int64_t firstFrame = 0, start = 0, stop = 0;
    std::optional<juce::Range<std::int64_t>> punch;
    std::optional<TakeLoop> loop;
};

// Makes the clips of a track's take, in the caller's undo step: what was played where it was played; punched in, only
// what falls in the range, in place of what was there, cross-fading at its edges; round a looped range, once recording has
// gone round it, what was played before the range in a clip of its own, and a take for each time round, from the first, in
// one clip over the range, the last whole one playing (a time round is whole if the file holds what was played over all of
// it). The file goes if it keeps nothing. Returns what its clips keep of it, in the song's samples as if nothing looped, in
// order: none if it kept nothing.
std::vector<juce::Range<std::int64_t>> placeTake (Song&, TrackID, const juce::File&, const TakePlace&);

// The channels of the audio device's inputs that a track's input names ("1" or "1+2"), from 0; none for an id that names
// none.
std::vector<int> getInputChannels (const juce::String& inputID);
} // namespace phazite
