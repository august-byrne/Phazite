// SPDX-License-Identifier: AGPL-3.0-or-later
// How the piano roll lays out a drum track: which rows it shows, and in what order. The track keeps the order and the
// hidden rows (model/notes/Notes.h); these functions take them, and the facts the rows come from, as plain values.
#pragma once

#include "model/notes/Notes.h"

#include <optional>
#include <vector>

namespace phazite::ui
{
// The rows of a drum track: its sampler's sounds if it has a sampler (none for one without sounds), otherwise General
// MIDI's usual drums; then any other notes its clips play. In the track's order (rows it doesn't mention keep theirs,
// after them), without the hidden rows.
std::vector<DrumRow> getDrumRows (const std::optional<std::vector<DrumRow>>& samplerSounds, const std::vector<int>& notesPlayed,
                                  const std::vector<int>& order, const std::vector<int>& hidden);

// The order a track keeps after one of its shown rows is dragged to a new place among them.
std::vector<int> getDrumOrderAfterMove (const std::vector<DrumRow>& shown, int note, int newIndex);
} // namespace phazite::ui
