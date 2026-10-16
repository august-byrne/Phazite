// SPDX-License-Identifier: AGPL-3.0-or-later
// How a new project starts (docs/design.md, section 13; roadmap step 1.9): the tempo, time signature and key that the
// new-project window chooses, the song outline whose sections it lays out, and whether it starts with a drum track and a
// synth track that the computer keyboard can play at once.
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
    std::optional<int> keyRoot;   // from 0 (C) to 11 (B); none leaves the tracks without a key
    juce::String scale = "major"; // as projects save a scale (getSavedScaleName()): "major", "minor"...
    juce::String outline;         // a built-in outline's id, or empty for none
    bool withInstruments = true;  // a drum track and a synth track (addInstrumentTracks())
};

// Whether a new project can start so: a tempo within the limits, a time signature Phazite takes, a root and a scale it
// knows, and an outline it has.
bool isValid (const NewProjectSettings&);
} // namespace phazite
