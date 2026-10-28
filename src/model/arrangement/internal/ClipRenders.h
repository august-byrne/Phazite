// SPDX-License-Identifier: AGPL-3.0-or-later
// The renders that audio clips play stretched, pitched or reversed (AudioClips.h; roadmap step 2.18): made with Phazite's
// stretcher (devices/stretch/Stretch.h) into the Edit's temporary folder, the project's Cache folder, as 32-bit WAV files
// at the source's rate. A render is never changed once made: other settings make another file, so undo plays the old
// one again at once.
#pragma once

#include "devices/stretch/Stretch.h"
#include "model/base/internal/Tracktion.h"

#include <map>

namespace phazite
{
// Renders a file with some settings into another, on the calling thread: into a file beside it first, which becomes the
// render once it is whole. Returns false if the source can't be read or the render written, or shouldStop stopped it;
// true if the render is there, even if someone else made it meanwhile.
bool renderClipAudio (te::Engine&, const juce::File& source, const stretch::Settings&, const juce::File& render,
                      const std::function<bool()>& shouldStop = {});

// Renders, on the calling thread, every clip's render that isn't there yet, and tells Tracktion about each new one. What
// the project's ClipRenders does in the background; exports and freezes call it first, as Tracktion would render such
// clips silent. Returns false if shouldStop stopped it or a render failed.
bool renderMissingClipAudio (te::Edit&, const std::function<bool()>& shouldStop = {});

// Deletes the renders in the Edit's temporary folder that no clip plays, and any left half-made. When a project closes
// (model/mixer/internal/Freeze.h: purgeCache()).
void purgeClipRenders (te::Edit&);

//==============================================================================
// Makes the renders a project's clips need, in the background, while the project context holds their Edit: each one that
// isn't there yet, a short while after the clips last changed (a drag of a clip's tempo makes no render for each value
// it passes), two at a time. When one is ready, Tracktion is told, and playback is made again, so that the clip sounds.
// One that no clip needs any more stops.
class ClipRenders : private juce::ValueTree::Listener,
                    private juce::Timer,
                    private juce::AsyncUpdater
{
public:
    explicit ClipRenders (te::Edit&);
    ~ClipRenders() override;

    // Whether any clip still waits for its render.
    bool isRendering() const;

private:
    class Job;

    te::Edit& edit;
    juce::ValueTree state; // the Edit's
    juce::ThreadPool pool;
    std::map<juce::File, Job*> running;

    juce::CriticalSection finishedLock;
    juce::Array<juce::File> finished; // by the jobs, for the message thread to tell Tracktion about

    void update();
    void jobFinished (const juce::File& render); // on its thread

    void timerCallback() override;
    void handleAsyncUpdate() override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;

    JUCE_DECLARE_NON_COPYABLE (ClipRenders)
};
} // namespace phazite
