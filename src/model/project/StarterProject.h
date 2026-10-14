// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace phazite
{
namespace te = tracktion;

// What a first project starts with, so Phazite makes sound right away: a synth track playing a
// looped 4-bar arpeggio. Roadmap step 1.9 replaces it with a proper starter project (a drum track
// and Phazite's own synth).
void addStarterContent (te::Edit&);

// Where new projects go unless the user picks another folder: Documents/Phazite/Projects.
juce::File getDefaultProjectsFolder();
} // namespace phazite
