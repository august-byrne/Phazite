// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in hosted in a helper process (docs/design.md, section 19), as a plug-in instance of JUCE's, which a song's
// PluginInstances owns (PluginInstances.h): what the model asks of a plug-in (its description, parameters by their own IDs,
// state, programs, latency) goes to the helper's instance of it (sandbox/client/RemoteInstance.h). Phazite's engine plays
// the instance's channel itself (SandboxedProcessor.h), live and in its renders (exports, stems, freezing, bouncing), which
// wait for the plug-in for as long as it takes; a block through processBlock() waits too. One block at a time goes through
// the channel: the engine never waits for a render to be done with it, and skips the block instead. What the plug-in
// changes itself (its parameters, with the gestures around them, its latency, its state) comes in on the message thread,
// as a plug-in in Phazite's process would tell it. Its editor is a window of the helper's, which a window of the app's
// shows (app/SandboxedEditor.h). If its helper goes (a crash, or ended for a hang), it is crashed: it plays nothing, shows no
// editor, and keeps the state it last had.
#pragma once

#include "sandbox/client/RemoteInstance.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
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

    // Whether its helper has gone (any thread), and its being told so (by the plug-in host, on any thread); then what it
    // calls on the message thread (the plug-in host's, which tells the views).
    bool hasCrashed() const noexcept { return crashed.load (std::memory_order_acquire); }
    void helperLost();
    std::function<void()> onLost;

    // Its plug-in opened again in another helper, in place of the one whose helper went, with what keeps that helper and
    // what it calls as it goes: the song's instances, what the engine plays and its window keep this instance (the plug-in
    // host's reload()). The engine skips its blocks meanwhile. What it reopens from: its plug-in, the state it last had, its
    // program and its sample rate.
    void replaceRemote (std::unique_ptr<sandbox::RemoteInstance>, std::shared_ptr<void> keepHelper, std::function<void()> onGone);
    const juce::PluginDescription& getDescription() const noexcept { return description; }
    const juce::MemoryBlock& getLastState() const noexcept { return lastState; }
    double getPreparedRate() const noexcept { return preparedRate; }

    // The values set since the last block, as the block's changes at its start (on the thread that plays it, the channel
    // held).
    void takeChanges (sandbox::BlockInfo&, sandbox::ParameterChange*, int maxChanges) noexcept;

    // The parameters (by their places) whose values Phazite's engine sends with each block, as its automation and
    // modulation move them (SandboxedProcessor.h): a value set by hand of one of them goes with the engine's blocks rather
    // than to the helper at once, so that it never comes between them. Message thread.
    void setMovedByEngine (std::span<const int> parameters);
    void setMovedByEngine (int parameter, bool);

    // Its parameters, by their places in the plug-in's list: as the helper told them as it opened (their own IDs, names,
    // steps and defaults); one's place by its own ID (-1 for none); its value now, from 0 to 1; a value set by hand, which
    // goes to the helper at once, unless Phazite's engine moves the parameter (then it goes with the engine's blocks); how
    // a value reads, as the plug-in says, and the value text reads as ("-6 dB"), as the plug-in reads it (each within a
    // quarter of a second, else as a number). Message thread.
    const std::vector<sandbox::ParameterInfo>& getParameterInfos() const noexcept { return remote->getInfo().parameters; }
    int findParameter (const juce::String& ownID) const;
    float getParameterValue (int parameter) const;
    void setParameterValue (int parameter, float value);
    juce::String describeValue (int parameter, float value) const;   // not getParameterText(), which would hide JUCE's
    float readValue (int parameter, const juce::String& text) const; // from 0 to 1

    // What the plug-in changes itself, told on the message thread: a value of its own, as its window or a program of its own
    // set it (and as it opens again after a crash), by the parameter's place; what its own window grabs and lets go of, a
    // message after the values that came with the gesture, for the automation Phazite's engine writes by hand; its state
    // (anything it keeps that its values don't say); and its latency.
    std::function<void (int parameter, float value)> onParameterChanged;
    std::function<void (int parameter, bool starting)> onGesture;
    std::function<void()> onStateChanged, onLatencyChanged;

    // What its editor in the helper tells (message thread), for the editor's window in Phazite: its new size, as the
    // plug-in resized it; and a key Phazite takes over it, down or let go, by its code and modifiers (JUCE's).
    std::function<void (int width, int height)> onEditorResized;
    std::function<void (bool isDown, int keyCode, int modifiers)> onEditorKey;

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

    // Its editor is a window of the helper's, which the app shows (app/SandboxedEditor.h): none of JUCE's.
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }

    // The plug-in's programs as the helper told them (one without a name for a plug-in that has none), the one it has,
    // and another chosen, within the time a state takes.
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    // Its state as the helper gives it: a VST3's as its format's own preset file (.vstpreset), which other hosts and
    // DAWproject read (sandbox/core/Vst3State.h); another format's as JUCE gives it.
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
    std::unordered_map<juce::String, int> places; // each parameter's, by its own ID
    std::vector<int> movedByEngine;
    double preparedRate = 0.0;
    int preparedBlockSize = 0;

    // What came from the helper, for the message thread.
    juce::CriticalSection lock;
    std::vector<std::pair<int, float>> changedByPlugin;
    std::vector<std::pair<int, bool>> gesturesByPlugin;
    bool latencyCame = false, stateCame = false, lossCame = false;
    int latency = 0;

    struct EditorKey
    {
        bool isDown = false;
        int keyCode = 0, modifiers = 0;
    };

    std::vector<EditorKey> editorKeys;
    std::optional<std::pair<int, int>> editorSize;

    // The gestures the plug-in ended, told a message after the values that came with them (so that a pass of touch has
    // them before it ends).
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
    void editorResized (int width, int height) override;
    void editorKey (bool isDown, int keyCode, int modifiers) override;
    void handleAsyncUpdate() override;

    void playBlock (juce::AudioBuffer<float>&, const juce::MidiBuffer&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedInstance)
};
} // namespace phazite
