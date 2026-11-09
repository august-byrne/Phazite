// SPDX-License-Identifier: AGPL-3.0-or-later
// An audio clip's waveform as the playlist and the audio clip editor draw it. The app makes them
// (src/app/ClipWaveforms.h): the levels of the clip's file load in the background and call back as they go, for what shows
// them to be painted again, which needs the open project's clips, so they belong to the app. Without a maker set, clips
// show no waveform (in tests, say).
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
// It keeps what it drew last, for one area and stretch of the file, as a mask that it fills with the current colour, and
// draws of it only what the Graphics context's clip region asks for: give it the same area while the view stays the same
// (the part of the clip in view, not the part being painted), and a strip that repaints, such as under the playhead,
// reads nothing again. It looks the same wherever a part of a pixel moves it, as the playlist moves the song it glides.
class ClipWaveform
{
public:
    virtual ~ClipWaveform() = default;

    // Draws the clip's waveform over an area of a component as wide as the clip (width), in the current colour.
    virtual void paint (juce::Graphics&, juce::Rectangle<int> area, int width) = 0;

    // Draws what the clip's audio file plays over a stretch of the song, which can reach past the clip's edges to the audio
    // trimmed off it: the stretch's start at the area's left, its end at its right. Nothing where the file has no audio.
    virtual void paintSpan (juce::Graphics&, juce::Rectangle<int> area, BeatRange span) = 0;

    // Lets go of what it keeps of what it drew last, while nothing shows it (the playlist's clip left the view): it draws
    // it again when it next paints.
    virtual void forgetDrawing() {}
};

// Makes the waveform of an audio clip of the open project, which calls loaded() on the message thread as more of its levels
// load, for what shows it to paint it again (and to drop a picture of it, such as the playlist's clips keep).
using ClipWaveformMaker = std::function<std::unique_ptr<ClipWaveform> (ProjectContext&, ClipID, std::function<void()> loaded)>;

void setClipWaveformMaker (ClipWaveformMaker);                                                          // the app's, or nothing
std::unique_ptr<ClipWaveform> makeClipWaveform (ProjectContext&, ClipID, std::function<void()> loaded); // nullptr without a maker
} // namespace phazite::ui
