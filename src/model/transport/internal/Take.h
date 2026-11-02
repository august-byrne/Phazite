// SPDX-License-Identifier: AGPL-3.0-or-later
// A take recorded through Phazite's own engine (docs/tracktion-migration.md, T2 step 9; Recording.h says what recording
// does): each armed track's audio input into a WAV file in the project's Audio folder, which a thread of its own writes,
// and the notes played live into the track live MIDI plays, from where the engine starts playing it (after the count-in)
// to where it stops, known to the sample from the engine's own playhead and clock (engine/core/Recording.h). What came
// in goes where it was heard: an input's sound earlier by the delays the audio device reports for its input and output,
// the song's delay compensation and the audio settings' correction (AudioRecorder.h), a note by the output's delay and the
// delay compensation. While it records, the session that keeps it (PlaybackSession::startTake()) loops the range looped
// on purpose, takes round it of any length; the song's own loop doesn't loop a take, which plays on past the song's end,
// nor does a punched one.
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"

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
    // and the notes played, each where it was heard, folded into the looped range, kept to the punched one, quantized to
    // sixteenths when recordings are; with Overdub, in the first of the track's MIDI clips that they overlap, starting at
    // or before them, or else in a new clip over what was recorded; without, in place of the clips there. It records
    // nothing more.
    void finish();

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

    void placeNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length);
    void close (bool keep);

    Take (const Take&) = delete;
    Take& operator= (const Take&) = delete;
};
} // namespace phazite
