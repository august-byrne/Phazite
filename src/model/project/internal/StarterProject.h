// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// What a new project can start with, so that the computer keyboard plays something at once: a drum track on the drum
// sampler's kit and a synth track on the synth, without clips (the new-project window's choice).
void addInstrumentTracks (te::Edit&);

// What a first project starts with, so Phazite makes sound right away, on its own devices only: the instrument tracks
// (addInstrumentTracks()), the drums playing a 4-bar beat and the synth an arpeggio over C, Am, F and G, looped.
void addStarterContent (te::Edit&);

// The track the computer keyboard should play first in a project made with these: the first on Phazite's synth, whose
// notes are in the keyboard's octave (the drum sampler's pads are two octaves lower). Nothing if there is none.
te::AudioTrack* findSynthTrack (te::Edit&);

// Where new projects go unless the user picks another folder: Documents/Phazite/Projects.
juce::File getDefaultProjectsFolder();
} // namespace phazite
