// SPDX-License-Identifier: AGPL-3.0-or-later
// A container's settings as plain values (ContainerModel.h, internal/Containers.h): what part of the sound coming in each of
// its chains plays, and the notes and velocities an instrument container's chain plays.
#pragma once

namespace phazite
{
// What part of the sound coming into a container of effects each of its chains plays (Schema.h: containerSplit). The parts
// add up to the sound, but layers, each of which is all of it.
enum class ContainerSplit
{
    layers,   // all of it: each chain a layer, and what they put out adds up
    bands,    // a band of it, low to high in the chains' order, each from its chain's crossover (the first's from the bottom)
    midSide,  // the first chain its middle, what its sides share, and the second its side, what they don't
    leftRight // the first chain its left side, and the second its right
};

// A range of notes (0 to 127, 60 the middle C) or of velocities (1 to 127), both ends in it.
struct NoteRange
{
    int low = 0, high = 127;

    bool contains (int value) const noexcept { return value >= low && value <= high; }
    bool operator== (const NoteRange&) const = default;
};

inline constexpr NoteRange allKeys { 0, 127 };
inline constexpr NoteRange allVelocities { 1, 127 };

// How many chains a container holds at most, as Phazite's engine plays them (engine/nodes/Container.h).
inline constexpr int maximumChains = 16;
} // namespace phazite
