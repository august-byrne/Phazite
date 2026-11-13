// SPDX-License-Identifier: AGPL-3.0-or-later
// Places and lengths in the song as Phazite writes them for people, by the open project's time signatures: a place in bars,
// beats and sixteenths, "17.3.2", each from 1, as Ableton Live counts (the transport bar's position, and what a drag shows in
// the hint bar); a place typed so; and a length in bars and beats, "2 bars 1.5 beats", by the time signature where it starts.
#pragma once

#include "model/base/Beats.h"
#include "model/project/ProjectContext.h"

#include <optional>

namespace phazite::ui
{
// A sixteenth note: a quarter of a quarter note, whatever the time signature's beat.
inline constexpr double sixteenthBeats = 0.25;

// "17.3.2": the place's bar, its time signature's beat in that, and the sixteenth in that, each from 1, rounded down.
juce::String formatBarPosition (const ProjectContext&, BeatPosition);

// A place typed as a bar, then its beat and its sixteenth if wanted: "17", "17.3", "17.3.2" or "17 3 2". Nothing without a
// bar.
std::optional<BeatPosition> parseBarPosition (const ProjectContext&, const juce::String&);

// "2 bars 1.5 beats", "1 bar", "3 beats", "0.25 beats": a length in whole bars and the beats left, to a hundredth, by the time
// signature where it starts.
juce::String formatSongLength (const ProjectContext&, BeatPosition start, BeatDuration);
} // namespace phazite::ui
