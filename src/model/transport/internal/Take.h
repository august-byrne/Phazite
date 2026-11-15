// SPDX-License-Identifier: AGPL-3.0-or-later
// A take recorded through Phazite's own engine (Recording.h says what recording does): each armed track's audio input into
// a WAV file in the song's recordings folder (AudioRecorder.h), which a thread of its own writes, and the notes played live
// into the track live MIDI plays, from where the engine starts playing it (after the count-in) to where it stops, known to
// the sample from the engine's own playhead and clock (engine/core/Recording.h). What came in goes where it was heard: an
// input's sound earlier by the delays the audio device reports for its input and output, the song's delay compensation
// and the audio settings' correction (AudioIO::getRecordingCorrection(), model/engine/internal/AudioIO.h: how much earlier,
// in milliseconds, for a driver that reports its delays wrongly), a note by the output's delay and the delay compensation.
// While it records, the session that keeps it (PlaybackSession::startTake()) loops the range looped on purpose, takes
// round it of any length, the first time round from wherever recording started (AudioRecorder.h: TakeLoop), each one a
// take, the first too: as Logic Pro and Bitwig keep a partial first pass, an input's file starts with silence from the
// range's start when recording started within it. The song's own loop doesn't loop a take, which plays on past the song's
// end, nor does a punched one. Meanwhile it says what it has recorded so far, where it will go, for the playlist to draw it
// growing on each track it records onto, as Ableton Live does (model/transport/RecordingModel.h: getRecordingsInProgress()).
// Where the writing thread fell so far behind that sound was lost (a drive that stalls), its file has silence in its place,
// so that the rest stays where it was played, and the take says so as it is finished. A write that fails (a full drive, one
// that went away) ends the file there, whole: the session stops recording, keeping what was written, and the take says
// so; and where the drive has room for little recording, starting says so. Stopping waits for the writing thread to finish
// what it is writing, however long the drive takes. The notes keep those played just before the take's start, moved to it
// (a downbeat played a little early after the count-in), and the sustain pedal, the mod wheel and every other controller
// and the pitch bend played on the track go into its clips beside them, as a MIDI file's import makes them.
#pragma once

#include "AudioRecorder.h"
#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/transport/RecordingValues.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class PlaybackSession;

namespace engine
{
    class Recording;
}

// How long a recording of so many bytes a second the space left on a drive holds, as the user is told when it is little
// (under littleSpaceSeconds) before recording starts: "about 4 minutes"; nothing when there is room enough.
inline constexpr double littleSpaceSeconds = 10.0 * 60.0;
std::optional<juce::String> describeLittleSpace (std::int64_t bytesFree, double bytesPerSecond);

class Take
{
public:
    // Starts one, giving the session's engine what it records from its next block on; nothing if nothing would record
    // (no track for live MIDI to play, and none armed with an input the audio device has). Where the drive has little room
    // for its inputs, the session's problem report says so (describeLittleSpace()), and where an input's file can't be made,
    // which track isn't recorded (after a jump too). Nothing while the engine's plan is for another rate than the engine's
    // (the audio device started again, and the song isn't given for its rate yet). One that goes on from another after a
    // jump (PlaybackSession::locateWhileRecording()) is started whole, its files made, before the jump is asked for, which
    // it waits for (the next locate the message thread asks): it starts where the playhead lands, to the sample, the one
    // before handing it what the engine records once the jump is taken (engine/core/Recording.h); and it
    // records the inputs of the armed tracks that one recorded, no others: a track armed since waits for Stop, as it did
    // before the jump, when the audio device's inputs are opened for it (AudioRecorder::update()), and until then its input
    // isn't open to record. It keeps the notes played (keepPlayed()) once the one before is finished.
    static std::unique_ptr<Take> start (PlaybackSession&, Take* goesOnFrom = nullptr);

    // The armed tracks whose inputs it records.
    std::vector<TrackID> getInputTracks() const;

    // The sample rate it records at: the engine's as it started; and how many times the audio device had started then
    // (PlaybackHost::getDeviceStarts()). An audio device that starts again, at another rate or the same, ends it
    // (PlaybackSession::deviceStarted()).
    double getSampleRate() const noexcept { return rate; }
    int getDeviceStarts() const noexcept { return deviceStarts; }

    // The notes played live are kept for it from just before it starts (PlaybackSession::keepPlayedFrom()).
    void keepPlayed();

    // Whether a write of one of its files failed (a full drive), after which nothing more is written into any of them: the
    // session stops recording (any thread).
    bool hasFailedWriting() const noexcept { return writeFailed.load(); }

    // One not finished keeps nothing: its files go.
    ~Take();

    // The range the engine loops while it records, if any.
    std::optional<BeatRange> getLoop() const noexcept { return loop; }

    // Its clips, in the caller's undo step: each armed track's audio where it was played (AudioRecorder.h's placeTake()),
    // and the notes played, each where it was heard, kept to the punched range; round a looped range, once recording has
    // gone round it, those played before the range in a clip of their own and the rest folded into the range; quantized to
    // sixteenths when recordings are; with Overdub, in the first of the track's MIDI clips that they overlap, starting at
    // or before them, or else in a new clip over what was recorded; without, in place of the clips there. The controllers
    // and the bend played go into the clips the notes do, where they were heard, beside them (the pedal held down and the
    // bend away from its middle as the take started, at its start). Where an input's sound was lost, it tells the
    // session's problem report (PlaybackSession::onProblem) which track, where in the song and how much, in one message;
    // where a write failed, where its files end. It records nothing more. end() and then place(), at once.
    void finish();

    // Its end: the engine records nothing more, its files are written whole, and what it recorded is kept for place(), which
    // makes its clips; until then the song is as it was (a take stopped during a drag waits for the drag to end, so that its
    // Escape doesn't take the take back: model/transport/internal/Playback.h, stopPlaying()). Nothing to place if it hadn't
    // started.
    void end();
    void place();

    // What it has recorded so far, where it will go (RecordingModel.h): each armed track's input from where it started to
    // where it has got to (round a looped range, once recording has gone round it, what was played before the range, and
    // this time round from the range's start; punched in, what falls in the range), and the notes played into live MIDI's
    // track once there are any, over what they will be kept in. Nothing once it is finished. Message thread.
    std::vector<RecordingInProgress> getInProgress();

    // An armed track's input so far: the loudest sample in each of so many columns across a stretch of the song, as this
    // time round a looped range plays it (and before the range, as the first time round played it), from 0 to 1; 0 where
    // nothing has come in yet. Message thread.
    std::vector<float> getLevels (TrackID, BeatRange span, int columns) const;

    // Whether it records onto a track: one armed with an input the audio device has, or live MIDI's.
    bool recordsOnto (TrackID) const;

    // For tests: holds the writing thread back until let go, as a drive that stalls holds it, from its next turn (holding it
    // waits for that); what comes in meanwhile waits in the inputs' buffers, and is lost once they are full.
    void holdWriting (bool held);

    // For tests: the drive its files are on is full from now on: a write that would make a file longer fails, as the
    // operating system fails it, and one over what a file holds already (its header, as the file is ended) doesn't.
    void fillDrive() noexcept { driveFull.store (true); }

private:
    explicit Take (PlaybackSession&);

    struct AudioTake;
    class Writer;
    class FileStream;

    PlaybackSession& session;
    double rate = 44100.0;                  // the engine's, as the take started
    std::optional<engine::TempoMap> tempo;  // the song's, as the take started
    engine::Recording* recording = nullptr; // the engine's, until finished
    std::int64_t keepFrom = 0;              // the engine's clock, from which the notes played are kept for it
    std::atomic<bool> writeFailed { false }, driveFull { false };
    std::vector<std::unique_ptr<AudioTake>> audio;
    std::unique_ptr<Writer> writer;
    TrackID midiTrack; // live MIDI's, as recording starts, or none
    std::optional<BeatRange> punch, loop;
    std::optional<TakeLoop> looped;             // the loop in the song's samples, as the engine plays it
    std::int64_t audioShift = 0, midiShift = 0; // how late an input's sound and a note come in, in samples
    bool finished = false;

    // A sample of the song, as the engine places a beat.
    std::int64_t toSample (BeatPosition) const;

    // The time round a looped range that the take has got to, once so many samples have been heard of it: that of the last
    // one (TakeLoop::getRound()); 0 without a loop.
    int getRoundHeard (std::int64_t heard) const;

    // How much silence an input's file starts with, for a take that started at a sample of the song: round a looped range
    // that recording started within, from the range's start to where what came in first was played, so that each time round
    // lies in the file where the range does, the first one too (AudioRecorder.h: TakePlace::lead); otherwise none.
    std::int64_t getLead (std::int64_t startSample) const;

    // The writing thread, then the message thread as the take ends: what has come into each input, written into its file
    // as far as so many frames of the take, once the take has started (where it started says how much silence each file
    // starts with).
    void write (std::int64_t upTo);

    // A note played live: its pitch and velocity, and where it was heard in the song, as if nothing looped, to where it
    // ended (or the take did, while it is held), in samples.
    struct Played
    {
        int pitch = 60, velocity = 100;
        std::int64_t start = 0, end = 0;
    };

    // A controller moved or the pitch bend played live: in MIDI's own terms (a controller's number and its value from 0 to
    // 127, or the bend from 0 to 16383, 8192 its middle), and where it was heard in the song, as if nothing looped.
    struct PlayedControl
    {
        bool bend = false;
        int number = 0, value = 0;
        std::int64_t at = 0;
    };

    // Notes and controls played live, in order, each where it goes in the song, and the samples of the song a clip of them
    // would go between: round a looped range, the range's (looped), whose notes are every time round's. Only what was played
    // in it (notes, and controls moved within it) makes a clip; the state it starts with (carried: the pedal held down, the
    // bend away from its middle, a controller moved a moment before) and the pedal and bend let go of just before its end
    // (released: a control at or past a clip's end isn't played, and the song plays its clips' controls as one stream, so a
    // pedal left down would hold the next clip's notes) go only into a clip that what was played made.
    struct PlayedNotes
    {
        std::vector<Played> notes;
        std::int64_t from = 0, to = 0;
        bool looped = false;
        std::vector<PlayedControl> controls, carried, released;
        int endPedal = 0, endBend = 8192; // the pedal's and the bend's values at its end
    };

    // The notes played into live MIDI's track while it recorded, up to so many samples from its start, in the parts a
    // clip of them goes over, in order: those played in the punched range, in it; round a looped range, once recording has
    // gone round it, those played before the range (one held over its start ending there), then the rest folded into the
    // range (one held over its end ending there); or else all of them, where they were played. A note played up to
    // earlyNoteSeconds before the start (or the punched range's) starts there, but not for a take that goes on after a jump,
    // whose take before kept it. The controls go the same way, a controller's value that repeats the one before it left
    // out.
    std::vector<PlayedNotes> collectNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length);
    static constexpr double earlyNoteSeconds = 0.1;
    bool afterJump = false; // it goes on from another after a jump

    // How many times the audio device had started as it started (PlaybackHost::getDeviceStarts()): one that starts again
    // meanwhile ends it.
    int deviceStarts = 0;

    // Each controller's last value on live MIDI's track as it started, and the bend's (128); -1 where none was played: the
    // state a take longer than the played events are kept for starts with (the take before's, after a jump).
    std::array<int, 129> heldAtStart {};

    // What it recorded, as it ended, for place(): where it started, how long it was, and its notes.
    struct Ended
    {
        std::int64_t startSample = 0, length = 0;
        std::vector<PlayedNotes> parts;
    };

    std::optional<Ended> ended;
    bool clipsPlaced = false;

    void placeNotes (const std::vector<PlayedNotes>&);
    void close (bool keep);

    // Where each input's sound was lost, as one message for the user ("Vocals at bar 4, beat 4 (2.0 s)"), each stretch where it
    // starts in the song, kept to what the take's clips keep (placeTake() says); empty if none was lost.
    juce::String describeLost (std::int64_t startSample) const;

    // Where the files end, a write having failed, as one message for the user; empty if none failed.
    juce::String describeFailedWrite (std::int64_t startSample) const;

    // Where a sample of the song is, as the user reads it ("bar 4, beat 4").
    juce::String describeSample (std::int64_t) const;

    Take (const Take&) = delete;
    Take& operator= (const Take&) = delete;
};
} // namespace phazite
