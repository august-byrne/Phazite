// SPDX-License-Identifier: AGPL-3.0-or-later
// Recording's plain values (RecordingModel.h says what each is for): what a track records, and what is being recorded now,
// as the playlist draws it growing while it records (docs/design.md, section 17).
#pragma once

#include "model/arrangement/PlaylistModel.h"
#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <vector>

namespace phazite
{
// What a track records: an armed track's audio input, or what is played on MIDI keyboards and the computer keyboard into
// the track live MIDI plays (model/transport/TransportModel.h: getLiveMidiTrack()).
enum class RecordSource
{
    input,
    midi
};

// What is being recorded now, where it will go: an armed track's input from where recording started to where it has got to
// (round a looped range, this time round, each a take; punched in, what falls in the range), and once anything is played on
// MIDI, the notes played, over the range they will be kept in (round a looped range, all of it once recording has gone
// round). Round a looped range that recording started before, once it has gone round, what was played before the range is
// a recording of its own beside the range's, whole, as it will be a clip of its own.
struct RecordingInProgress
{
    TrackID track {};
    RecordSource source = RecordSource::input;
    BeatRange range {};
    int take = 1;                    // round a looped range: the time round it is on, from 1
    std::vector<NoteShape> notes {}; // MIDI: the notes, in beats from the range's start; one still held reaches its end
    bool beforeLoop = false;         // what was played before a looped range, once recording has gone round it: it grows no more
};
} // namespace phazite
