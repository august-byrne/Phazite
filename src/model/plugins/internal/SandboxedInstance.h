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
#include <map>
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
                                private juce::AsyncUpdater,
                                private juce::Timer
{
public:
    // The instance opened in a helper, which this owns from now on, as the plug-in described, from a state (the one it has
    // until it gives or is given another); what keeps its helper (which it keeps until the instance has closed there), and
    // what it calls first as it goes.
    SandboxedInstance (std::unique_ptr<sandbox::RemoteInstance>, const juce::PluginDescription&, const juce::MemoryBlock& openedFrom,
                       std::shared_ptr<void> keepHelper, std::function<void()> onGone);
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

    // Whether the helper it is in now still runs, asked later from anywhere on the message thread, after this has gone too
    // (a socket of its editor's that waits for the helper's window to leave it: app/SandboxedEditor.h).
    std::function<bool()> watchHelper() const;

    // Its plug-in opened again in another helper (at a sample rate, for its channel's limits), in place of the one whose
    // helper went, with what keeps that helper and what it calls as it goes: the song's instances, what the engine plays and
    // its window keep this instance (the plug-in host's reload()). The engine skips its blocks meanwhile. What it reopens
    // from: its plug-in, the state it last had, its program, and the sample rate and longest block it was last prepared for
    // (none before it was), which a crashed one keeps too, so that one that went in a render at another rate comes back at
    // the rate it plays at now.
    void replaceRemote (std::unique_ptr<sandbox::RemoteInstance>, double openedAtRate, std::shared_ptr<void> keepHelper,
                        std::function<void()> onGone);
    const juce::PluginDescription& getDescription() const noexcept { return description; }
    const juce::MemoryBlock& getLastState() const noexcept { return lastState; }
    double getPreparedRate() const noexcept { return preparedRate.load (std::memory_order_relaxed); }
    int getPreparedBlockSize() const noexcept { return preparedBlockSize.load (std::memory_order_relaxed); }

    // Whether its channel for the rate and the longest block it was last prepared for is still to come (any thread): asked of
    // the helper without waiting, once the helper has given back the block it kept then, if it kept one. The engine skips its
    // blocks meanwhile, and a render (the channel held, the block back) waits for the channel itself (finishPreparing()).
    // The message thread looks a hundred times a second, and ends its helper once the block has been kept as long as a hang
    // takes, or the channel hasn't come in a minute. Nothing waits for the plug-in so (known bug 166: the message thread
    // waited for the helper, behind whatever else it did): cancelling a render that it keeps a block of doesn't wait for
    // the helper, which can't prepare it until it gives the block back.
    bool isPreparing() const noexcept { return preparing.load (std::memory_order_acquire); }
    bool finishPreparing (const std::atomic<bool>* cancelled = nullptr); // whether it is prepared then, for what was asked

    // A render has given it back (message thread): the plug-in told now that it plays live again, not as the first live block
    // comes, which the switch would make late (a post: nothing waits for the helper).
    void playLive();

    // The block handed over last waited for, a little at a time, from a thread that may wait (a render's, or JUCE's
    // processBlock()), the channel held: until the helper answers it (played), its helper has gone (gone), it has kept
    // it so long that it hangs (hung: its helper is then ended for it, told without waiting), or cancelled is set.
    enum class Answer
    {
        played,
        gone,
        hung,
        cancelled
    };

    Answer waitForAnswer (std::int64_t limitNanoseconds, const std::atomic<bool>* cancelled = nullptr) noexcept;

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
    // goes to the helper at once, unless Phazite's engine moves the parameter (then it goes with the engine's blocks), and
    // none that is no number (known bug 181). Message thread.
    const std::vector<sandbox::ParameterInfo>& getParameterInfos() const noexcept { return remote->getInfo().parameters; }
    int findParameter (const juce::String& ownID) const;
    float getParameterValue (int parameter) const;
    void setParameterValue (int parameter, float value);

    // How a value reads, as the plug-in writes it, and the value a text reads as ("-6 dB"), from 0 to 1, as the plug-in reads
    // it: each asked of its helper without waiting (known bug 166: each kept the message thread a quarter of a second at
    // most). A value's text the plug-in wrote before comes at once; else the value as a number, with the helper asked (one
    // value of a parameter at a time, the newest asked for meanwhile next), and whenKnown called once, on the message thread,
    // as its answer comes or doesn't come in a state's time (getStateMilliseconds()), so that what shows it asks again. A text
    // read calls back once the plug-in has read it, however long that takes while its helper lives, with none for what it
    // reads as no number (known bug 181) or if its helper goes first: never a number the plug-in didn't give (the review of
    // known bug 166: a text read as a value from 0 to 1 set "-6 dB" to the bottom). What is still to come as this goes, or leaves its song (answerTextsAsGone(), which what
    // plays it may still hold), is answered with nothing, a message later. Message thread.
    juce::String describeValue (int parameter, float value, std::function<void()> whenKnown = {}); // not getParameterText()
    void readValue (int parameter, const juce::String& text, std::function<void (std::optional<float>)> then);
    void answerTextsAsGone();
    bool isAskingTexts() const noexcept { return ! textAsks.empty() || ! valueAsks.empty(); } // for the tests

    // What the plug-in changes itself, told on the message thread: a value of its own, as its window or a program of its own
    // set it (and as it opens again after a crash), by the parameter's place; what its own window grabs and lets go of, a
    // message after the values that came with the gesture, for the automation Phazite's engine writes by hand; its state
    // (anything it keeps that its values don't say); and its latency.
    std::function<void (int parameter, float value)> onParameterChanged;
    std::function<void (int parameter, bool starting)> onGesture;
    std::function<void()> onStateChanged, onLatencyChanged;
    std::function<void()> onStateMayChange; // any change counted that may have changed its state, whoever made it (message thread)

    // What its editor in the helper tells (message thread), for the editor's window in Phazite: its new size, as the
    // plug-in resized it; and a key Phazite takes over it, down or let go, by its code and modifiers (JUCE's).
    std::function<void (int width, int height)> onEditorResized;
    std::function<void (bool isDown, int keyCode, int modifiers, int scanCode)> onEditorKey;

    // How long a block of so many samples may take in real time before it is late: as long as it lasts, but a millisecond
    // at least (or as long as the longest block of the device's, where that is shorter), so that a block the engine cut
    // short (at a loop's end, after a count-in) isn't late for the round trip to the helper alone.
    static std::int64_t getDeadlineNanoseconds (int numSamples, int maximumBlockSize, double sampleRate) noexcept;

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
    // DAWproject read (sandbox/core/Vst3State.h); another format's as JUCE gives it. From one whose helper has gone, or
    // doesn't give it in time, the state it last had: the one it opened from, was given or gave last. takeState() says
    // which: true for the plug-in's state now, false for the one it last had.
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;
    bool takeState (juce::MemoryBlock&);

    // Its state asked for without waiting (message thread), so that a save asks every plug-in at once and the message thread
    // waits for none (known bugs 69 and 89): of any (a save by hand, closing); of one whose state may have changed since it
    // last gave it or was given one (a value or a program set, by Phazite or by the plug-in, its settings said to have
    // changed, or its window open since: a plug-in needn't say what its window changes), whether or not it answered when
    // last asked (Auto Save); or of one that said it changed (a value or a program set, its settings said to have changed)
    // and answered when last asked (a store, or asked unasked: shouldBeAsked()). Not again while an answer is still to come,
    // nor of one whose helper has gone; and an answer still to come is
    // given up as it is given a state, which that answer would be older than. takeAskedState() takes the answer in once it
    // has come (the state it last had from then on), waiting for it until a time
    // (juce::Time::getMillisecondCounterHiRes(); none for 0), and gives it up once the state's time limit has passed since
    // it was asked (the state it had before stays), or with giveUp: true once no answer is still to come.
    enum class Ask
    {
        all,
        changed,
        changedAndAnswering
    };

    void askState (Ask, bool unasked = false); // unasked: by nothing that waits for it (shouldBeAsked())
    bool takeAskedState (double waitUntil = 0.0, bool giveUp = false);
    void takeAskedStateIfIn(); // the answer taken in if it has come; one asked unasked given up on after its time
    bool isAskingState() const noexcept { return stateAsked != nullptr; }
    bool mayHaveChangedState() const noexcept;
    bool isStateOlder() const noexcept; // than the plug-in's: it may have changed since it last gave its state, unasked or unanswered

    // Whether it is time to ask it for its state, unasked, so that the state it last gave is its own by the time a store
    // writes it without waiting (PluginInstances::store()): it said it changed (a value or a program set, by Phazite or by
    // it, its settings said to have changed), and has been quiet since for a moment (a knob turned in its window isn't asked
    // at each step), or at the latest a second after its first change since it was asked, as changes go on; and no sooner
    // after its last ask than ten times as long as that answer took; it isn't asked already, and answered when last asked.
    // The answers that came, counted, and how long the last took (for the tests' measures).
    bool shouldBeAsked() const noexcept;
    int countAnswers() const noexcept { return answers; }
    double getLastAnswerMilliseconds() const noexcept { return lastAnswerMilliseconds; }
    bool isStateUnasked() const noexcept; // as shouldBeAsked(), but for the moment and an answer to come: it may be, later

    // The changes counted so far (above), and whether it has answered an ask made since so many, or didn't in time.
    int countStateChanges() const noexcept { return stateChanges; }
    int getStateTakenAt() const noexcept { return stateTakenAt; } // the count the state it last had is as of (-1: none)
    bool hasGivenStateSince (int changes) const noexcept { return stateTakenAt >= changes || stateMissedAt >= changes; }

    // Opened in the background as its project opened, while the engine played silence in its place (message thread): the
    // first processor made of it then fades its sound in, as after a block it didn't play (SandboxedProcessor.h).
    void joinLate() noexcept { joinsLate = true; }
    bool takeJoinLate() noexcept { return std::exchange (joinsLate, false); }

    // Its editor shown (in Phazite's window: app/SandboxedEditor.h) and closed: while one is, and since, until it next
    // gives its state, its state may have changed.
    void editorOpened() noexcept;
    void editorClosed() noexcept;

    // What it was set to as it opened (the values the song sets by hand, which it sets again at each opening) counted as no
    // change of its state, which is the one it opened from, if it opened from one.
    void keepStateAsOpened() noexcept;
    bool hasChangedSinceOpened() const noexcept { return stateChanges != openedAt || givenSinceOpened; } // or given a state

    // How long a state may take to come or go, for a plug-in that never answers (shorter in the tests).
    static void setStateMilliseconds (int) noexcept;
    static int getStateMilliseconds() noexcept;

    // How long a channel for another rate may take to come before the helper is taken for hung (a minute; shorter in the
    // tests).
    static void setPrepareMilliseconds (int) noexcept;
    static int getPrepareMilliseconds() noexcept;

private:
    class Parameter;

    static BusesProperties getBuses (const sandbox::PluginInfo&); // as the helper set the plug-in's up

    std::shared_ptr<void> keepHelper; // let go of after the instance has closed in it
    std::function<void()> onGone;
    std::unique_ptr<sandbox::RemoteInstance> remote;
    const juce::PluginDescription description;
    std::atomic<bool> crashed { false }, held { false };
    std::atomic<bool> anyPending { false }; // a value set while a block played, for the next
    juce::MemoryBlock lastState;            // as it opened from, or was last taken or given: what one whose helper has gone keeps

    // On the message thread: the changes that may have changed its state, counted; the count lastState was taken or given at
    // (-1 for none: it opened from nothing), and the last it was asked at and didn't answer; and the answer still to come,
    // asked at a count, since a time.
    int stateChanges = 0, stateTakenAt = -1, stateMissedAt = -1, stateAskedAt = 0;
    double changedAt = 0.0;                                          // as the last of them was (juce::Time::getMillisecondCounterHiRes())
    int openedAt = 0;                                                // the count as it opened (keepStateAsOpened())
    double firstUnaskedChangeAt = 0.0, lastAnswerMilliseconds = 0.0; // shouldBeAsked()'s
    int answers = 0;
    bool givenSinceOpened = false;
    void noteStateChange() noexcept;
    std::shared_ptr<sandbox::Helper::Reply> stateAsked;
    double stateAskedSince = 0.0;
    bool stateAskedUnasked = false;
    // Its editors shown now, and shown or closed (counted, and as its state was asked); and whether one has shown since its
    // state was last taken.
    int editorsOpen = 0, editorMoves = 0, editorMovesAtAsk = 0;
    bool editorSinceTaken = false;
    bool joinsLate = false;
    std::vector<Parameter*> parameters;
    std::unordered_map<juce::String, int> places; // each parameter's, by its own ID
    std::vector<int> movedByEngine;

    // The sample rate and longest block it was last prepared for (asked of it on the message thread), and the rate its
    // channel is for (changed with the channel held); whether that channel waits for the helper's block, and since when (in
    // getNanoseconds()).
    std::atomic<double> preparedRate { 0.0 };
    std::atomic<int> preparedBlockSize { 0 };
    double channelRate = 0.0;
    std::atomic<bool> preparing { false };
    std::int64_t preparingSince = 0;
    std::unique_ptr<sandbox::RemoteInstance::Preparing> prepareAsked; // the channel asked of the helper, until it comes (held)

    // What came from the helper, for the message thread.
    juce::CriticalSection lock;
    std::vector<std::pair<int, float>> changedByPlugin; // each parameter's last value, once, in the order they first came
    std::vector<int> changedSlots;                      // each parameter's place there, or -1
    // Each parameter's gesture as the helper told it last between two turns, and whether it began one meanwhile, as values
    // are kept: nothing dropped, however many came, so that every begin is paired with its end (the second review of known
    // bug 181: an end dropped past a cap left a gesture begun, and Touch writing).
    struct GestureTold
    {
        bool pending = false, begun = false, sawBegin = false;
    };

    std::vector<GestureTold> gesturesTold; // by parameter
    std::vector<int> gesturesByPlugin;     // the parameters told, each once, in the order they first came
    bool latencyCame = false, stateCame = false, lossCame = false;
    int latency = 0;

    struct EditorKey
    {
        bool isDown = false;
        int keyCode = 0, modifiers = 0, scanCode = 0;
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
    std::vector<bool> inGesture; // each parameter's, as JUCE was told it (message thread)
    void endGestures();

    // The texts asked of the helper (describeValue(), readValue()), until their answers come, looked at a hundred times a
    // second while any is to come; and the values' texts the plug-in wrote, by parameter and value (forgotten all at once
    // past a few thousand), and the last of each parameter's.
    struct TextAsk
    {
        int parameter = 0;
        float value = 0.0f;
        std::shared_ptr<sandbox::Helper::Reply> reply;
        double since = 0.0;
        std::vector<std::function<void()>> whenKnown;
        std::optional<float> next; // the value asked for since, which is asked once this answer is in
    };

    struct ValueAsk
    {
        juce::String text;
        std::shared_ptr<sandbox::Helper::Reply> reply;
        double since = 0.0;
        std::function<void (std::optional<float>)> then;
    };

    struct TextAnswers final : juce::Timer
    {
        explicit TextAnswers (SandboxedInstance& o)
            : owner (o)
        {
        }

        void timerCallback() override { owner.takeTextAnswers(); }
        SandboxedInstance& owner;
    };

    std::vector<TextAsk> textAsks; // one for each parameter at most
    std::vector<ValueAsk> valueAsks;
    std::map<std::pair<int, float>, juce::String> texts;
    TextAnswers textAnswers { *this };
    void takeTextAnswers();

    void parameterChanged (int index, float value) override;
    void gestureChanged (int index, bool starting) override;
    void latencyChanged (int samples) override;
    void stateChanged() override;
    void parametersChanged (const std::vector<sandbox::ParameterInfo>&) override {}
    void editorResized (int width, int height) override;
    void editorKey (bool isDown, int keyCode, int modifiers, int scanCode) override;
    void handleAsyncUpdate() override;
    void timerCallback() override; // a channel that waits for the helper's block

    bool isPrepared() noexcept; // its channel for the rate and block asked (held)
    void prepareNow();          // its channel asked for them, or taken in once it has come (held)
    void playBlock (juce::AudioBuffer<float>&, const juce::MidiBuffer&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SandboxedInstance)
};
} // namespace phazite
