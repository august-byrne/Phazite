// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip's waveform as the playlist and the audio clip editor draw it. The app makes them (src/app/ClipWaveforms.h):
// Tracktion's thumbnails load in the background and repaint a component as they go, which needs both a window component
// and Tracktion, so they belong to the app. Without a maker set, clips show no waveform (in tests, say).
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class ClipWaveform
{
public:
    virtual ~ClipWaveform() = default;

    // Draws the clip's waveform over an area of a component as wide as the clip (width), in the current colour.
    virtual void paint (juce::Graphics&, juce::Rectangle<int> area, int width) = 0;

    // Draws what the clip's audio file plays over a stretch of the song, which can reach past the clip's edges to the audio
    // trimmed off it: the stretch's start at the area's left, its end at its right. Nothing where the file has no audio.
    virtual void paintSpan (juce::Graphics&, juce::Rectangle<int> area, BeatRange span) = 0;
};

// Makes the waveform of an audio clip of the open project, which repaints a component as it loads.
using ClipWaveformMaker = std::function<std::unique_ptr<ClipWaveform> (ProjectContext&, ClipID, juce::Component& toRepaint)>;

void setClipWaveformMaker (ClipWaveformMaker);                                                        // the app's, or nothing
std::unique_ptr<ClipWaveform> makeClipWaveform (ProjectContext&, ClipID, juce::Component& toRepaint); // nullptr without a maker
} // namespace phazite::ui
