// SPDX-License-Identifier: AGPL-3.0-or-later
// Whether two plans of one song play alike (engine/core/Plan.h): one a player compiled from what the song's changes touched
// (model/playback/internal/SongPlayer.h), the other a fresh player's whole compile, as a render makes it. What sounds on
// from plan to plan is kept by identity in the first and new in the second, so what is compared is what each holds: the
// strips in their order, each's notes, audio regions (each file by what it was opened as: its path, and when it was
// written and how long it was then), devices (their kinds, switches, values set by hand, latencies, automation, sidechains
// and how many routes modulate them), inputs (their strips, points, delays, send levels and cuts), fader curves, and how
// many containers and modulators it has; and the loop's bounds, the metronome and the tempo map (the first 256 beats).
//
// What it doesn't compare, as each plan makes it again whole from the song or it is no part's that a plan keeps: the
// strips' taps and rings, the delays' rings and the departures and alignment numbers (the paths' identity across plans,
// which a fresh player can't have); a container's chains, mix and split, and a chain's fader, ranges and crossover; the
// modulators' values and their routes' amounts; the parts of a drum sampler's pads; and the sounds a device plays (its
// pads', its oscillators', its sampler's: a processor keeps them, with no common way to ask).
#pragma once

#include "engine/core/Plan.h"

#include <juce_core/juce_core.h>

namespace phazite::test
{
// A line for each difference, by the strip's ID and what differs; a few dozen at most. Empty for plans that play alike.
juce::StringArray describePlanDifferences (const engine::Plan& kept, const engine::Plan& fresh);
} // namespace phazite::test
