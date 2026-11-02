// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper process, as JUCE's plug-in instance that Tracktion's te::ExternalPlugin wraps in place of
// one loaded into Phazite's process (docs/tracktion-migration.md, T3): what Tracktion and the model ask of a plug-in
// (its description, buses, parameters by their own IDs, state, latency, a block) goes to the helper's instance of it
// (sandbox/client/RemoteInstance.h). Phazite's engine plays the instance's channel itself (SandboxedProcessor.h); a block
// through processBlock() is one of Tracktion's renders (exports, stems, freezing, bouncing until T5), which waits for
// the plug-in for as long as it takes. One block at a time goes through the channel: the engine never waits for a render
// to be done with it, and skips the block instead. What the plug-in changes itself (its parameters, with the gestures
// around them, its latency, its state) comes in on the message thread, as a plug-in in Phazite's process would tell it.
// If its helper goes (a crash, or ended for a hang), it is crashed: it plays nothing, and keeps the state it last had.
#pragma once

#include "sandbox/client/RemoteInstance.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace phazite
{
class SandboxedInstance final : public juce::AudioPluginInstance,
                                private sandbox::RemoteInstance::Listener,
                                private juce::AsyncUpdater
{
public:
    // The instance opened in a helper, which this owns from now on, as the plug-in described; what keeps its helper (which
    // it keeps until the instance has closed there), and what it calls first as it goes.
    SandboxedInstance (std::unique_ptr<sandbox::RemoteInstance>, const juce::PluginDescription&, std::shared_ptr<void> keepHelper,
                       std::function<void()> onGone);
    ~SandboxedInstance() override;

    sandbox::RemoteInstance& getRemote() noexcept { return *remote; }

    // The channel, for one block: held while it plays, which the audio thread never waits for (tryHold()), and a render
    // does (hold()). let() gives it back.
    bool tryHold() noexcept;
    void hold() noexcept;
    void let() noexcept;

    // Whether its helper has gone (any thread), and its being told so (by the plug-in host, on any thread).
    bool hasCrashed() const noexcept { return crashed.load (std::memory_order_acquire); }
    void helperLost();

    // The values set since the last block, as the block's changes at its start (on the thread that plays it, the channel
    // held).
    void takeChanges (sandbox::BlockInfo&, sandbox::ParameterChange*, int maxChanges) noexcept;

    // The parameters (by their places) whose values Phazite's engine sends with each block, as its automation and
    // modulation move them (SandboxedProcessor.h): a value set by hand of one of them goes with the engine's blocks rather
    // than to the helper at once, so that it never comes between them. Message thread.
    void setMovedByEngine (std::span<const int> parameters);
    void setMovedByEngine (int parameter, bool);

    // What the plug-in's own window grabs and lets go of, by the parameter's place, after Tracktion is told (message
    // thread): for the automation Phazite's engine writes by hand.
    std::function<void (int parameter, bool starting)> onGesture;

    // How long a block may take in real time before it is late: as long as it lasts.
    static std::int64_t getDeadlineNanoseconds (int numSamples, double sampleRate) noexcept;

    //==============================================================================
    void fillInPluginDescription (juce::PluginDescription&) const override;
    const juce::String getName() const override;

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}

    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    bool isBusesLayoutSupported (const BusesLayout&) const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

private:
    class Parameter;

    static BusesProperties getBuses (const sandbox::PluginInfo&); // as the helper set the plug-in's up

    std::shared_ptr<void> keepHelper; // let go of after the instance has closed in it
    std::function<void()> onGone;
    std::unique_ptr<sandbox::RemoteInstance> remote;
    const juce::PluginDescription description;
    std::atomic<bool> crashed { false }, held { false };
    std::atomic<bool> anyPending { false }; // a value set while a block played, for the next
    juce::MemoryBlock lastState;            // as it was last taken or given: what a plug-in whose helper has gone keeps
    std::vector<Parameter*> parameters;
    std::vector<int> movedByEngine;
    double preparedRate = 0.0;
    int preparedBlockSize = 0;

    // What came from the helper, for the message thread.
    juce::CriticalSection lock;
    std::vector<std::pair<int, float>> changedByPlugin;
    std::vector<std::pair<int, bool>> gesturesByPlugin;
    bool latencyCame = false, stateCame = false, lossCame = false;
    int latency = 0;

    // The gestures the plug-in ended, told a message after the values that came with them, which Tracktion takes in a
    // message later (so that a pass of touch has them before it ends).
    struct EndedGestures final : juce::AsyncUpdater
    {
        explicit EndedGestures (SandboxedInstance& o)
            : owner (o)
        {
        }

        void handleAsyncUpdate() override { owner.endGestures(); }
        SandboxedInstance& owner;
    };

    EndedGestures endedGestures { *this };
    std::vector<int> gesturesToEnd;
    void endGestures();

    void parameterChanged (int index, float value) override;
    void gestureChanged (int index, bool starting) override;
    void latencyChanged (int samples) override;
    void stateChanged() override;
    void parametersChanged (const std::vector<sandbox::ParameterInfo>&) override {}
    void handleAsyncUpdate() override;

    void playBlock (juce::AudioBuffer<float>&, const juce::MidiBuffer&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedInstance)
};
} // namespace phazite
