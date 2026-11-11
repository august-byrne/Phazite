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
// so that the rest stays where it was played, and the take says so as it is finished. Stopping waits for the writing thread
// to finish what it is writing, however long the drive takes.
#pragma once

#include "AudioRecorder.h"
#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/transport/RecordingValues.h"

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

class Take
{
public:
    // Starts one, giving the session's engine what it records from its next block on; nothing if nothing would record
    // (no track for live MIDI to play, and none armed with an input the audio device has).
    static std::unique_ptr<Take> start (PlaybackSession&);

    // One not finished keeps nothing: its files go.
    ~Take();

    // The range the engine loops while it records, if any.
    std::optional<BeatRange> getLoop() const noexcept { return loop; }

    // Its clips, in the caller's undo step: each armed track's audio where it was played (AudioRecorder.h's placeTake()),
    // and the notes played, each where it was heard, kept to the punched range; round a looped range, once recording has
    // gone round it, those played before the range in a clip of their own and the rest folded into the range; quantized to
    // sixteenths when recordings are; with Overdub, in the first of the track's MIDI clips that they overlap, starting at
    // or before them, or else in a new clip over what was recorded; without, in place of the clips there. Where an input's
    // sound was lost, it tells the session's problem report (PlaybackSession::onProblem) which track, where in the song and
    // how much, in one message. It records nothing more.
    void finish();

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

private:
    explicit Take (PlaybackSession&);

    struct AudioTake;
    class Writer;

    PlaybackSession& session;
    std::optional<engine::TempoMap> tempo;  // the song's, as the take started
    engine::Recording* recording = nullptr; // the engine's, until finished
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

    // Notes played live, in order, each where it goes in the song, and the samples of the song a clip of them would go
    // between: round a looped range, the range's (looped), whose notes are every time round's.
    struct PlayedNotes
    {
        std::vector<Played> notes;
        std::int64_t from = 0, to = 0;
        bool looped = false;
    };

    // The notes played into live MIDI's track while it recorded, up to so many samples from its start, in the parts a
    // clip of them goes over, in order: those played in the punched range, in it; round a looped range, once recording has
    // gone round it, those played before the range (one held over its start ending there), then the rest folded into the
    // range (one held over its end ending there); or else all of them, where they were played.
    std::vector<PlayedNotes> collectNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length);

    void placeNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length);
    void close (bool keep);

    // Where each input's sound was lost, as one message for the user ("Vocals at bar 4, beat 4 (2.0 s)"), each stretch where it
    // starts in the song, kept to what the take's clips keep (placeTake() says); empty if none was lost.
    juce::String describeLost (std::int64_t startSample) const;

    Take (const Take&) = delete;
    Take& operator= (const Take&) = delete;
};
} // namespace phazite
