// SPDX-License-Identifier: AGPL-3.0-or-later
// The kinds of a controller's value a MIDI clip holds beside its notes, as MIDI has them, below the notes' code: the song's
// values (model/base/internal/SongValues.h) hold them, and the notes' code (model/notes/NoteValues.h, which includes this)
// edits and names them.
#pragma once

namespace phazite
{
// A MIDI controller (by its number: the sustain pedal 64, the mod wheel 1; its value 0 to 127), the pitch bend (0 to 16383,
// its middle 8192), the channel's pressure (0 to 127), a note's pressure, polyphonic aftertouch (by the note's number; 0 to
// 127), and a program change (its value the program, 0 to 127). Each on a MIDI channel, from 1 to 16.
enum class ControlKind
{
    controller,
    pitchWheel,
    channelPressure,
    aftertouch,
    programChange
};

// A controller's and a note's pressure have a number; the other kinds have none.
constexpr bool hasNumber (ControlKind kind) noexcept
{
    return kind == ControlKind::controller || kind == ControlKind::aftertouch;
}

// 16383 for the pitch bend, 127 for the others.
constexpr int getHighestValue (ControlKind kind) noexcept
{
    return kind == ControlKind::pitchWheel ? 16383 : 127;
}
} // namespace phazite
