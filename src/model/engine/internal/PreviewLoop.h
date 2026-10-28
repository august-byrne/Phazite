// SPDX-License-Identifier: AGPL-3.0-or-later
// A loop the browser previews in time with the song, stretched to the song's tempo as clips are: by Rubber Band's R3
// engine (devices/stretch/Stretch.h). Tracktion's preview stretched it with SoundTouch as it played, which plays drum hits
// twice and allocates as each preview begins (docs/realtime-audit.md, finding 17). Here the loop is rendered on a thread of
// its own, into memory, at the output's rate, and plays from there as soon as its start is ready, the render keeping
// ahead: the audio thread only copies. For the previewer (model/engine/Preview.cpp) and tests.
#pragma once

#include "devices/stretch/Stretch.h"
#include "model/base/internal/Tracktion.h"

#include <atomic>

namespace phazite
{
// A loop stretched to a tempo, rendered in the background from the moment it is made.
class StretchedLoop : private juce::Thread
{
public:
    // Renders the file so many times as long, for playing at the sample rate. Opens it on the calling thread: isValid()
    // says whether it could.
    StretchedLoop (te::Engine&, const juce::File&, double timeRatio, double sampleRate);
    ~StretchedLoop() override; // stops the render, if it is still going

    bool isValid() const noexcept { return length > 0; }
    double getSampleRate() const noexcept { return sampleRate; }
    juce::int64 getLength() const noexcept { return length; } // in frames, at the rate it plays at

    // Whether it can play from its start without the playback catching up with the render: all of it is ready, or a
    // quarter of a second of it and the render runs at least twice as fast as it plays.
    bool isReadyToPlay() const;
    bool isComplete() const noexcept;

    // Its frames from a position on into the channels (a mono loop into each), and silence before its start, after its
    // end, and where the render hasn't reached yet. Real-time safe.
    void read (juce::int64 position, float* const* channels, int numChannels, int numFrames) const noexcept;

private:
    std::unique_ptr<juce::AudioFormatReader> reader;
    stretch::Settings settings;
    double sampleRate = 0.0;
    juce::int64 length = 0;
    juce::AudioBuffer<float> audio;       // all of it, at sampleRate, allocated before the render starts
    std::atomic<juce::int64> ready { 0 }; // the frames rendered, from its start
    const double startedAt = juce::Time::getMillisecondCounterHiRes();

    void run() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StretchedLoop)
};

// What plays a StretchedLoop in the preview Edit: an instrument on a track of its own, which plays the loop where the
// Edit's playhead is (the Edit loops over the loop's length), and silence without one. Never in a project.
class PreviewLoopPlugin : public te::Plugin
{
public:
    explicit PreviewLoopPlugin (te::PluginCreationInfo);
    ~PreviewLoopPlugin() override;

    static const char* xmlTypeName;

    // The loop to play, or nullptr. The loop must outlive the Edit's playback (freePlaybackContext()).
    void setLoop (const StretchedLoop*) noexcept;

    juce::String getName() const override { return "Preview"; }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    bool isSynth() override { return true; }
    bool producesAudioWhenNoAudioInput() override { return true; }
    int getNumOutputChannelsGivenInputs (int) override { return 2; }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override; // real-time safe: no locks, no allocation

private:
    std::atomic<const StretchedLoop*> loop { nullptr };
    double sampleRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreviewLoopPlugin)
};

// For a preview Edit that Tracktion made of a loop in time (te::Edit::createEditForPreviewingFile()), whose clip it would
// stretch as it plays: switches that clip off, which keeps the Edit as long as the loop, and plays the loop stretched by
// Rubber Band instead, from a track of its own, rendering from now on. Returns the loop, which must outlive the Edit's
// playback (clearPreviewLoop()), or nullptr if the Edit has no such clip or the file can't be read.
std::unique_ptr<StretchedLoop> stretchPreviewLoop (te::Edit& preview, const juce::File&);

// Lets the preview Edit play no loop, once its playback is freed (te::TransportControl::freePlaybackContext()).
void clearPreviewLoop (te::Edit& preview);
} // namespace phazite
