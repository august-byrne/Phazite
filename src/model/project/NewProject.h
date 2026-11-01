// SPDX-License-Identifier: AGPL-3.0-or-later
// How a new project starts (docs/design.md, section 13; roadmap step 1.9): the tempo, time signature and key that the
// new-project window chooses, and the song outline whose sections it lays out. It starts with no tracks (the owner,
// 2026-11-01): Help > Example Songs opens songs that play at once (ExampleSongs.h).
#pragma once

#include "model/transport/TempoValues.h"

#include <juce_core/juce_core.h>
#include <optional>

namespace phazite
{
struct NewProjectSettings
{
    double tempo = 120.0;
    TimeSignature timeSignature {};
    std::optional<int> keyRoot;   // from 0 (C) to 11 (B); none leaves the tracks added later without a key
    juce::String scale = "major"; // as projects save a scale (getSavedScaleName()): "major", "minor"...
    juce::String outline;         // a built-in outline's id, or empty for none
};

// Whether a new project can start so: a tempo within the limits, a time signature Phazite takes, a root and a scale it
// knows, and an outline it has.
bool isValid (const NewProjectSettings&);
} // namespace phazite
