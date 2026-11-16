// SPDX-License-Identifier: AGPL-3.0-or-later
// What a track's MIDI clips play, as Phazite's engine plays it: the notes and controller events of every clip on the
// track, in beats from the song's start, each clip's cut to where the clip is. Built by the model from the song
// (model/playback/), never changed once built, and shared by the plans that play it.
#pragma once

#include "devices/core/Processor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace phazite::engine
{
class NoteSequence
{
public:
    // A note as it plays: from its start to its end, in beats, at most to its clip's end. A slide note plays nothing itself
    // but a slide event at its start, which glides what sounds then to its pitch by its end; a portamento note starts with a
    // portamento note on (devices/core/Processor.h).
    struct Note
    {
        double start = 0.0, end = 0.0;
        int pitch = 60;
        float velocity = 0.8f; // 0 to 1
        bool slide = false, portamento = false;
    };

    // A controller's change (a pitch bend or a MIDI controller), at a beat.
    struct Control
    {
        double beat = 0.0;
        devices::NoteEvent::Type type = devices::NoteEvent::Type::controller;
        int number = 0;
        float value = 0.0f; // as a device takes it (devices/core/Processor.h)
    };

    // What happens at a beat, in the order played: a note's start or end, a controller's change, or a slide (whose value is
    // its length in beats, which the engine plays in seconds).
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
    const std::vector<Note>& getNotes() const noexcept { return notes; }    // by their starts, without the slide notes

    // The first event at or after a beat.
    std::size_t findEvent (double beat) const noexcept;

    // Real-time safe: calls an action with each note that started before a beat and ends after another (no earlier than
    // it: where a stretch played from a beat starts, the notes sounding there), by their starts, as a look through every
    // note before it would find them; without looking through them (known bug 85: a loop's wrap and a jump did, on every
    // track, in one block). It looks through the notes since the last of its marks before the beat and those its mark holds:
    // at most 63 and 128, as no pitch sounds twice at once (the sequence ends a note where the next on its pitch starts, so
    // a mark holds one note of a pitch at most however a file's notes overlap).
    template <typename Action>
    void forEachSounding (double before, double endsAfter, Action&& action) const noexcept
    {
        const auto upTo = findFirstStartingAt (before);

        if (upTo == 0)
            return;

        const auto mark = (upTo - 1) / markSpacing;

        for (auto i = markStarts[mark]; i < markStarts[mark + 1]; ++i)
            if (const auto& note = notes[heldAtMarks[i]]; note.end > endsAfter)
                action (note);

        for (auto i = mark * markSpacing; i < upTo; ++i)
            if (notes[i].end > endsAfter)
                action (notes[i]);
    }

    // A controller (by its number), the bend, the channel's pressure or a note's pressure (by the note) that the sequence
    // changes somewhere, and its value as it is just before a beat: its last change before there, and where that was, or
    // none (held false).
    struct ControlAt
    {
        devices::NoteEvent::Type type = devices::NoteEvent::Type::controller;
        int number = 0;
        float value = 0.0f; // as a device takes it
        double beat = 0.0;
        bool held = false;
    };

    // Real-time safe: calls an action with each of them, as it is just before a beat (what playback that starts or jumps
    // there chases: Engine.h), but for those that change between that beat and another (whose change there plays where
    // playback starts: the first sample's): a search through its own changes for each one the sequence changes (258 at
    // most), nothing allocated.
    template <typename Action>
    void forEachControlAt (double before, double changesUntil, Action&& action) const noexcept
    {
        for (std::size_t k = 0; k + 1 < controlStarts.size(); ++k)
        {
            const auto* first = controlChanges.data() + controlStarts[k];
            const auto* end = controlChanges.data() + controlStarts[k + 1];
            const auto* next = std::lower_bound (first, end, before, [this] (std::uint32_t i, double b) { return events[i].beat < b; });

            if (next != end && events[*next].beat < changesUntil)
                continue;

            auto control = controlKeys[k];

            if (next != first)
            {
                const auto& change = events[*(next - 1)];
                control.value = change.value;
                control.beat = change.beat;
                control.held = true;
            }

            action (std::as_const (control));
        }
    }

    bool isEmpty() const noexcept { return events.empty(); }

    // How many bytes it holds, its events and its indexes (what a long take with dense pressure costs).
    std::size_t getBytes() const noexcept;

private:
    std::vector<Note> notes;
    std::vector<Event> events;

    // The index of the controllers' values (forEachControlAt()): each that changes anywhere once (by its type and number),
    // and its changes, by their places among the events, in their order, one after another (each one's from its start in
    // controlStarts to the next's): as many as the changes, whatever their kinds.
    static constexpr std::size_t maximumControlKeys = 128 + 2 + 128;
    std::vector<ControlAt> controlKeys;
    std::vector<std::uint32_t> controlChanges, controlStarts;

    // The index of the notes sounding (forEachSounding()): a mark at every 64th note by start, holding the notes before it
    // that end after its start, in their order; each mark's first in heldAtMarks, and the end of the last's. Made with the
    // sequence, from the track's notes alone.
    static constexpr std::size_t markSpacing = 64;
    std::vector<std::uint32_t> heldAtMarks;
    std::vector<std::size_t> markStarts;
    std::size_t findFirstStartingAt (double beat) const noexcept; // the first note that starts at or after a beat
};
} // namespace phazite::engine
