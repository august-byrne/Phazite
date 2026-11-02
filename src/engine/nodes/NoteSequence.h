// SPDX-License-Identifier: AGPL-3.0-or-later
// What a track's MIDI clips play, as Phazite's engine plays it (docs/tracktion-migration.md, T2): the notes and controller
// events of every clip on the track, in beats from the song's start, each clip's cut to where the clip is. Built by the model
// from the song (model/playback/), never changed once built, and shared by the plans that play it.
#pragma once

#include "devices/core/Processor.h"

#include <cstdint>
#include <vector>

namespace phazite::engine
{
class NoteSequence
{
public:
    // A note as it plays: from its start to its end, in beats, at most to its clip's end.
    struct Note
    {
        double start = 0.0, end = 0.0;
        int pitch = 60;
        float velocity = 0.8f; // 0 to 1
    };

    // A controller's change (a pitch bend or a MIDI controller), at a beat.
    struct Control
    {
        double beat = 0.0;
        devices::NoteEvent::Type type = devices::NoteEvent::Type::controller;
        int number = 0;
        float value = 0.0f; // as a device takes it (devices/core/Processor.h)
    };

    // What happens at a beat, in the order played: a note's start or end, or a controller's change.
    struct Event
    {
        double beat = 0.0;
        devices::NoteEvent::Type type = devices::NoteEvent::Type::noteOn;
        int number = 0;
        float value = 0.0f;
    };

    NoteSequence() = default;

    // From the notes and controller events of a track's clips. A note that starts again on a pitch still sounding ends the
    // one before it there, as a MIDI note can't play twice at once.
    NoteSequence (std::vector<Note>, std::vector<Control>);

    const std::vector<Event>& getEvents() const noexcept { return events; } // in time order, ends before starts at a beat
    const std::vector<Note>& getNotes() const noexcept { return notes; }    // by their starts

    // The first event at or after a beat.
    std::size_t findEvent (double beat) const noexcept;

    bool isEmpty() const noexcept { return events.empty(); }

private:
    std::vector<Note> notes;
    std::vector<Event> events;
};
} // namespace phazite::engine
