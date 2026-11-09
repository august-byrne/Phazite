// SPDX-License-Identifier: AGPL-3.0-or-later
// A take recorded through Phazite's own engine (Recording.h says what recording does): each armed track's audio input into
// a WAV file in the song's recordings folder (AudioRecorder.h), which a thread of its own writes, and the notes played live
// into the track live MIDI plays, from where the engine starts playing it (after the count-in) to where it stops, known to
// the sample from the engine's own playhead and clock (engine/core/Recording.h). What came in goes where it was heard: an
// input's sound earlier by the delays the audio device reports for its input and output, the song's delay compensation
// and the audio settings' correction (AudioIO::getRecordingCorrection(), model/engine/internal/AudioIO.h: how much earlier,
// in milliseconds, for a driver that reports its delays wrongly), a note by the output's delay and the delay compensation.
// While it records, the session that keeps it (PlaybackSession::startTake()) loops the range looped on purpose, takes
// round it of any length; the song's own loop doesn't loop a take, which plays on past the song's end, nor does a punched
// one. Meanwhile it says what it has recorded so far, where it will go, for the playlist to draw it growing on each track
// it records onto, as Ableton Live does (model/transport/RecordingModel.h: getRecordingsInProgress()).
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/transport/RecordingValues.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
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
    // and the notes played, each where it was heard, folded into the looped range, kept to the punched one, quantized to
    // sixteenths when recordings are; with Overdub, in the first of the track's MIDI clips that they overlap, starting at
    // or before them, or else in a new clip over what was recorded; without, in place of the clips there. It records
    // nothing more.
    void finish();

    // What it has recorded so far, where it will go (RecordingModel.h): each armed track's input from where it started to
    // where it has got to (round a looped range, this time round; punched in, what falls in the range), and the notes played
    // into live MIDI's track once there are any, over what they will be kept in. Nothing once it is finished. Message thread.
    std::vector<RecordingInProgress> getInProgress();

    // An armed track's input so far: the loudest sample in each of so many columns across a stretch of the song, as this
    // time round a looped range plays it, from 0 to 1; 0 where nothing has come in yet. Message thread.
    std::vector<float> getLevels (TrackID, BeatRange span, int columns) const;

    // Whether it records onto a track: one armed with an input the audio device has, or live MIDI's.
    bool recordsOnto (TrackID) const;

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
    std::int64_t audioShift = 0, midiShift = 0; // how late an input's sound and a note come in, in samples
    bool finished = false;

    // A note played live: its pitch and velocity, and where it was heard in the song, as if nothing looped, to where it
    // ended (or the take did, while it is held), in samples.
    struct Played
    {
        int pitch = 60, velocity = 100;
        std::int64_t start = 0, end = 0;
    };

    // The notes played into live MIDI's track while it recorded, up to so many samples from its start, each where it was
    // heard, folded into the looped range (one held over its end ending there) and kept to the punched one, in order; and
    // the samples of the song a clip of them would go between.
    struct PlayedNotes
    {
        std::vector<Played> notes;
        std::int64_t from = 0, to = 0;
    };

    PlayedNotes collectNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length);

    // The looped range in the song's samples, if the take loops one; and where a sample of the song, as if nothing looped,
    // plays: folded into the range, with the time round it is on (0 for the first, which ends at the range's end).
    std::optional<std::pair<std::int64_t, std::int64_t>> getLoopSamples() const;
    std::pair<std::int64_t, int> fold (std::int64_t sample) const;

    void placeNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length);
    void close (bool keep);

    Take (const Take&) = delete;
    Take& operator= (const Take&) = delete;
};
} // namespace phazite
