// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper: the instance JUCE's format loaded, prepared for its channel's blocks with its main
// buses in stereo and a stereo sidechain where it has one, and the thread that plays them, which waits on the channel's
// bell, plays the block in place and rings Phazite back. A block's parameter changes play at their samples: the block
// is played in pieces between them. The plug-in's playhead is the song's place in the block. What the plug-in changes
// itself (its parameters from its window, and the gestures around them, its latency, its state, its list of parameters)
// is kept for the message thread to tell Phazite (takeNotices()), never told from the thread that changed it, which may
// be its audio thread. The audio thread is in MMCSS's Pro Audio task, as Phazite's own are
// (devices/core/AudioThreads.h).
#pragma once

#include "sandbox/core/BlockChannel.h"
#include "sandbox/core/PluginInfo.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <vector>

namespace phazite::host
{
class Instance final : private juce::Thread,
                       private juce::AudioProcessorListener,
                       private juce::AudioProcessorParameter::Listener
{
public:
    // The plug-in, prepared at the sample rate for the channel's longest block; its thread starts at once.
    Instance (std::int64_t id, std::unique_ptr<juce::AudioPluginInstance>, std::unique_ptr<sandbox::BlockChannel>, double sampleRate);
    ~Instance() override;

    std::int64_t getID() const noexcept { return id; }
    juce::AudioPluginInstance& getPlugin() noexcept { return *plugin; }
    sandbox::PluginInfo describe() const;

    // On the message thread. Its state: a VST3's as its format's own preset file (.vstpreset: sandbox/core/Vst3State.h),
    // which projects keep and other hosts and DAWproject read, rather than JUCE's wrapper of the same streams; another
    // format's as JUCE gives it.
    void prepare (std::unique_ptr<sandbox::BlockChannel>, double sampleRate); // a new channel, for another rate or size
    juce::MemoryBlock getState();
    void setState (const juce::MemoryBlock&);
    void setProgram (int index);
    void setParameter (int index, float value);
    juce::String getParameterText (int index, float value) const;

    // What the plug-in changed itself since the last call, as messages to Phazite (on the message thread).
    void takeNotices (const std::function<void (sandbox::MessageWriter&)>& send);

private:
    class PlayHead;

    const std::int64_t id;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<sandbox::BlockChannel> channel;
    std::unique_ptr<PlayHead> playHead;
    double rate = 44100.0;
    int numPluginChannels = 0, sidechainChannel = -1; // where the sidechain bus's channels are in the plug-in's buffer
    juce::AudioBuffer<float> extra;                   // channels the plug-in has beyond the channel's: made once, as prepared
    juce::MidiBuffer midi;                            // a piece's MIDI, its room made once
    juce::Array<juce::AudioProcessorParameter*> parameters;

    // What the plug-in changed, for takeNotices(): each parameter's last value and whether it changed, its gestures in
    // the order they came, and whether its latency, state or parameters changed.
    std::unique_ptr<std::atomic<float>[]> changedValues;
    std::unique_ptr<std::atomic<bool>[]> changed;
    juce::CriticalSection gestureLock;
    std::vector<std::pair<int, bool>> gestures;
    std::atomic<bool> latencyChanged { false }, stateChanged { false }, parametersChanged { false };

    // A VST3's class ID, as its own preset file writes it, which its state's preset file is written with; none for
    // another format.
    juce::String vst3ClassID;

    void start (std::unique_ptr<sandbox::BlockChannel>, double sampleRate);
    void stop();
    void run() override;
    void play() noexcept;
    void playPiece (int start, int numSamples) noexcept;

    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override;
    void parameterValueChanged (int index, float value) override;
    void parameterGestureChanged (int index, bool starting) override;
};

// Gives a plug-in a state as Phazite keeps it: a VST3's preset file as the same streams in JUCE's wrapper (which sets its
// parameters' values again too, as a preset file set through JUCE's VST3 client doesn't), anything else as it is. On the
// message thread, not while a block plays.
void setPluginState (juce::AudioPluginInstance&, const juce::MemoryBlock&);
} // namespace phazite::host
