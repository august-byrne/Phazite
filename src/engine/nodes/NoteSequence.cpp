// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NoteSequence.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace phazite::engine
{
namespace
{
    using Type = devices::NoteEvent::Type;

    // At the same beat: notes end first, then controllers change (the channel's pressure too), then notes start, then a
    // note's pressure (after its start) and what sounds slides.
    int getOrder (Type type)
    {
        switch (type)
        {
            case Type::noteOff:
            case Type::allNotesOff:
                return 0;
            case Type::controller:
            case Type::pitchBend:
            case Type::channelPressure:
                return 1;
            case Type::noteOn:
            case Type::portamentoOn:
                return 2;
            case Type::aftertouch:
            case Type::slide:
                return 3;
        }

        return 2;
    }
} // namespace

NoteSequence::NoteSequence (std::vector<Note> allNotes, std::vector<Control> controls)
    : notes (std::move (allNotes))
{
    // The slide notes play no notes of their own.
    std::vector<Note> slides;
    std::copy_if (notes.begin(), notes.end(), std::back_inserter (slides), [] (const Note& note) { return note.slide; });
    std::erase_if (notes, [] (const Note& note) { return note.slide; });

    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });

    // A pitch that starts again while it sounds ends there first.
    std::array<Note*, 128> sounding {};

    for (auto& note : notes)
    {
        note.pitch = std::clamp (note.pitch, 0, 127);

        if (auto* before = sounding[(size_t) note.pitch]; before != nullptr && before->end > note.start)
            before->end = note.start;

        sounding[(size_t) note.pitch] = &note;
    }

    std::erase_if (notes, [] (const Note& note) { return note.end <= note.start; });

    // The marks of the notes sounding: at each, the notes before it that end after its start, those of the mark before
    // that still do and the notes since it.
    std::vector<std::uint32_t> held;

    for (std::size_t mark = 0; mark * markSpacing < notes.size(); ++mark)
    {
        const auto first = mark * markSpacing;

        if (mark > 0)
            for (auto i = first - markSpacing; i < first; ++i)
                held.push_back ((std::uint32_t) i);

        const auto at = notes[first].start;
        std::erase_if (held, [this, at] (std::uint32_t i) { return notes[i].end <= at; });
        markStarts.push_back (heldAtMarks.size());
        heldAtMarks.insert (heldAtMarks.end(), held.begin(), held.end());
    }

    markStarts.push_back (heldAtMarks.size());

    events.reserve (notes.size() * 2 + slides.size() + controls.size());

    for (const auto& note : notes)
    {
        events.push_back ({ note.start, note.portamento ? Type::portamentoOn : Type::noteOn, note.pitch, note.velocity });
        events.push_back ({ note.end, Type::noteOff, note.pitch, 0.0f });
    }

    for (const auto& slide : slides)
        if (slide.end > slide.start)
            events.push_back ({ slide.start, Type::slide, std::clamp (slide.pitch, 0, 127), (float) (slide.end - slide.start) });

    for (const auto& control : controls)
        events.push_back ({ control.beat, control.type, control.number, control.value });

    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
                      { return a.beat < b.beat || (! (b.beat < a.beat) && getOrder (a.type) < getOrder (b.type)); });

    // The index of the controllers' values: each that changes anywhere, a slot of its own (in the order they first change),
    // and its changes one after another, in their order: counted, then put in their places.
    std::array<int, maximumControlKeys> slotOf;
    slotOf.fill (-1);

    const auto keyOf = [] (Type type, int number) -> int
    {
        switch (type)
        {
            case Type::controller:
                return std::clamp (number, 0, 127);
            case Type::pitchBend:
                return 128;
            case Type::channelPressure:
                return 129;
            case Type::aftertouch:
                return 130 + std::clamp (number, 0, 127);
            case Type::noteOn:
            case Type::noteOff:
            case Type::allNotesOff:
            case Type::portamentoOn:
            case Type::slide:
                break;
        }

        return -1;
    };

    std::vector<std::uint32_t> counts;

    for (const auto& event : events)
    {
        const auto key = keyOf (event.type, event.number);

        if (key < 0)
            continue;

        auto& slot = slotOf[(size_t) key];

        if (slot < 0)
        {
            slot = (int) controlKeys.size();
            controlKeys.push_back ({ event.type, key >= 130 ? key - 130 : (key < 128 ? key : 0), 0.0f, 0.0, false });
            counts.push_back (0);
        }

        ++counts[(size_t) slot];
    }

    if (controlKeys.empty())
        return;

    controlStarts.assign (controlKeys.size() + 1, 0);

    for (std::size_t k = 0; k < counts.size(); ++k)
        controlStarts[k + 1] = controlStarts[k] + counts[k];

    controlChanges.resize (controlStarts.back());
    auto placed = std::vector<std::uint32_t> (controlStarts.begin(), controlStarts.end() - 1);

    for (std::size_t i = 0; i < events.size(); ++i)
        if (const auto key = keyOf (events[i].type, events[i].number); key >= 0)
            controlChanges[placed[(size_t) slotOf[(size_t) key]]++] = (std::uint32_t) i;
}

std::size_t NoteSequence::getBytes() const noexcept
{
    return sizeof (*this) + notes.capacity() * sizeof (Note) + events.capacity() * sizeof (Event)
           + controlKeys.capacity() * sizeof (ControlAt)
           + (controlChanges.capacity() + controlStarts.capacity() + heldAtMarks.capacity()) * sizeof (std::uint32_t)
           + markStarts.capacity() * sizeof (std::size_t);
}

std::size_t NoteSequence::findFirstStartingAt (double beat) const noexcept
{
    return (std::size_t) std::distance (
        notes.begin(), std::lower_bound (notes.begin(), notes.end(), beat, [] (const Note& note, double b) { return note.start < b; }));
}

std::size_t NoteSequence::findEvent (double beat) const noexcept
{
    return (std::size_t) std::distance (events.begin(), std::lower_bound (events.begin(), events.end(), beat,
                                                                          [] (const Event& event, double b) { return event.beat < b; }));
}
} // namespace phazite::engine
