// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
// What a first project starts with, so Phazite makes sound right away, on its own devices only: a drum track playing a
// 4-bar beat on the drum sampler's kit, and a synth track playing an arpeggio over C, Am, F and G, looped. Roadmap
// step 1.9 adds guidance to it.
void addStarterContent (te::Edit&);

// Where new projects go unless the user picks another folder: Documents/Phazite/Projects.
juce::File getDefaultProjectsFolder();
} // namespace phazite
