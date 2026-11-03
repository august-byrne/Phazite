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

    // At the same beat: notes end first, then controllers change, then notes start, then what sounds slides.
    int getOrder (Type type)
    {
        switch (type)
        {
            case Type::noteOff:
            case Type::allNotesOff:
                return 0;
            case Type::controller:
            case Type::pitchBend:
                return 1;
            case Type::noteOn:
            case Type::portamentoOn:
                return 2;
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
}

std::size_t NoteSequence::findEvent (double beat) const noexcept
{
    return (std::size_t) std::distance (events.begin(), std::lower_bound (events.begin(), events.end(), beat,
                                                                          [] (const Event& event, double b) { return event.beat < b; }));
}
} // namespace phazite::engine
