// SPDX-License-Identifier: AGPL-3.0-or-later
// How many elements and clips the model looked at to find what it was asked for, since the program started: one for each
// answer the clips' storage gave and each clip of a list it walked (model/arrangement/Clips.cpp), one for each look a
// drag's notes made (model/notes/Notes.cpp), and one for each TRACK element a walk of a song without values looked at
// (model/base/TracksValue.cpp: listTrackElements(), findTrackElementByID()). What the speed gates read
// (tests/PerformanceGateTests.cpp, tests/TracksValueTests.cpp), which count what an edit looks at. Atomic: songs made on
// other threads count into it too.
#pragma once

#include <cstdint>

namespace phazite
{
std::uint64_t countElementsVisited() noexcept;
void countVisited (std::uint64_t) noexcept;
} // namespace phazite
