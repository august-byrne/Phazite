// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SandboxedInstance.h"
#include "devices/core/AudioThreads.h"
#include "devices/core/Clock.h"
#include "model/base/Json.h"
#include "model/transport/TempoValues.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <thread>
#include <utility>

namespace phazite
{
namespace
{
    constexpr int defaultStateMilliseconds = 10000; // how long a plug-in's state may take to come or go
    std::atomic<int> stateMilliseconds { defaultStateMilliseconds };
    // How long a channel for another rate may take to come, which the helper makes after whatever it does before (opening
    // another plug-in, which may take a minute): after that the helper is taken for hung, as its answer holds off the
    // heartbeat (sandbox/client/Helper.h).
    constexpr int defaultPrepareMilliseconds = 60000;
    std::atomic<int> prepareMilliseconds { defaultPrepareMilliseconds };
    // How long a state or a program given may take to be set while the helper's message thread answers nothing, as long as a
    // plug-in may take to load (PluginHost::defaultLoadMilliseconds).
    constexpr int defaultSettingMilliseconds = 60000;
    std::atomic<int> settingMilliseconds { defaultSettingMilliseconds };
    constexpr double quietMilliseconds = 250.0;                // how long a plug-in that said it changed is left alone before it is asked
    constexpr double latestAskMilliseconds = 1000.0;           // how long after its first change it is asked at the latest
    constexpr double answerSpacing = 10.0;                     // and no sooner after its last ask than so many times its last answer took
    constexpr int textLookMilliseconds = 10;                   // how often the answers to the texts asked are looked at
    constexpr size_t textsKept = 4096;                         // the values' texts kept at most, before they are forgotten
    constexpr size_t maxCallsWaiting = 64;                     // the calls one text asked keeps at most, for those that show it
    constexpr size_t maxNoticesWaiting = 1024;                 // a plug-in's editor's keys kept at most between turns
    constexpr int smallestChannel = 4096;                      // the longest block a channel takes at least
    constexpr std::int64_t smallestDeadline = 1'000'000;       // a millisecond, which a round trip to the helper takes well within
    constexpr std::int64_t waitNanoseconds = 50'000'000;       // how long a wait for a block goes before it looks whether to go on
    constexpr int waitMilliseconds = 50;                       // and a wait for a channel
    constexpr int preparingLookMilliseconds = 10;              // how often the message thread looks at a channel that waits
    constexpr std::int64_t minimumHoldNanoseconds = 1'000'000; // how long preparing tries for the channel at least
} // namespace

juce::AudioProcessor::BusesProperties SandboxedInstance::getBuses (const sandbox::PluginInfo& info)
{
    BusesProperties buses;

    if (info.numInputChannels > 0)
        buses = buses.withInput ("Input", juce::AudioChannelSet::stereo(), true);

    if (info.hasSidechain)
        buses = buses.withInput ("Sidechain", juce::AudioChannelSet::stereo(), true);

    return buses.withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

// A parameter of the plug-in's, by its own ID: a value set from the message thread goes to the helper at once (unless
// Phazite's engine moves it, and sends it with its blocks), one set while a block plays (on its
// thread) joins the next block's changes; one the plug-in set itself comes back without going to it again.
class SandboxedInstance::Parameter final : public juce::HostedAudioProcessorParameter
{
public:
    Parameter (SandboxedInstance& o, int i, const sandbox::ParameterInfo& info)
        : owner (o),
          index (i),
          id (info.id),
          name (info.name),
          label (info.label),
          initial (info.defaultValue),
          steps (info.numSteps),
          boolean (info.isBoolean),
          automatable (info.isAutomatable),
          value (info.value)
    {
    }

    juce::String getParameterID() const override { return id; }
    float getValue() const override { return value.load (std::memory_order_relaxed); }

    void setValue (float newValue) override
    {
        if (! std::isfinite (newValue)) // known bug 181
            return;

        value.store (newValue, std::memory_order_relaxed);

        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            owner.noteStateChange();

            if (! movedByEngine)
                owner.remote->setParameter (index, newValue);
        }
        else
        {
            pending.store (newValue, std::memory_order_relaxed);
            isPending.store (true, std::memory_order_release);
            owner.anyPending.store (true, std::memory_order_release);
        }
    }

    bool movedByEngine = false; // message thread

    // What the plug-in set itself: told to its listeners, not to the helper again (message thread).
    void setFromPlugin (float newValue)
    {
        value.store (newValue, std::memory_order_relaxed);
        sendValueChangedMessageToListeners (newValue);
    }

    float getDefaultValue() const override { return initial; }
    juce::String getName (int maximumLength) const override { return name.substring (0, maximumLength); }
    juce::String getLabel() const override { return label; }
    int getNumSteps() const override { return steps > 0 ? steps : juce::AudioProcessor::getDefaultNumParameterSteps(); }
    bool isDiscrete() const override { return steps > 0; }
    bool isBoolean() const override { return boolean; }
    bool isAutomatable() const override { return automatable; }

    // As the plug-in wrote it last, asked without waiting (describeValue()).
    juce::String getText (float normalised, int maximumLength) const override
    {
        return owner.describeValue (index, normalised).substring (0, maximumLength);
    }

    // JUCE's, which can't wait for the plug-in (known bug 166: it waited for its helper; Phazite reads a value typed through
    // readValue(), which asks the plug-in): as a number from 0 to 1, else the value it has ("1e300" too; no finite number,
    // "inf" or "1e999": as it is). Phazite never sets a value read so.
    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, readFloat (text, getValue())); }

    // A value set while a block played, once (on the thread that plays the next).
    bool takePending (float& newValue) noexcept
    {
        if (! isPending.exchange (false, std::memory_order_acquire))
            return false;

        newValue = pending.load (std::memory_order_relaxed);
        return true;
    }

    // A value for the next block, as one set while a block played is kept, leaving the value set by hand as it is (on the
    // thread that plays the blocks).
    void keepPending (float newValue) noexcept
    {
        pending.store (newValue, std::memory_order_relaxed);
        isPending.store (true, std::memory_order_release);
    }

private:
    SandboxedInstance& owner;
    const int index;
    const juce::String id, name, label;
    const float initial;
    const int steps;
    const bool boolean, automatable;
    std::atomic<float> value, pending { 0.0f };
    std::atomic<bool> isPending { false };
};

SandboxedInstance::SandboxedInstance (std::unique_ptr<sandbox::RemoteInstance> instance, const juce::PluginDescription& described,
                                      const juce::MemoryBlock& openedFrom, std::shared_ptr<void> keep, std::function<void()> gone)
    : juce::AudioPluginInstance (getBuses (instance->getInfo())),
      keepHelper (std::move (keep)),
      onGone (std::move (gone)),
      remote (std::move (instance)),
      description (described),
      lastState (openedFrom),
      latency (remote->getLatencySamples())
{
    const auto& info = remote->getInfo();

    for (size_t i = 0; i < info.parameters.size(); ++i)
    {
        // Each ID once, as the helper's info was read (sandbox::readParameters(): known bug 181).
        auto parameter = std::make_unique<Parameter> (*this, (int) i, info.parameters[i]);
        parameters.push_back (parameter.get());
        places.emplace (info.parameters[i].id, (int) i);
        addHostedParameter (std::move (parameter));
    }

    changedSlots.assign (parameters.size(), -1);
    gesturesTold.assign (parameters.size(), {});
    gesturesByPlugin.reserve (parameters.size());

    setLatencySamples (latency);
    remote->setListener (this);

    // Its channel is for the rate it opened at, which the engine's first preparing needn't ask of the helper again (a
    // request the message thread would wait for, behind whatever else the helper does, such as opening another plug-in).
    channelRate = remote->getSampleRate();

    // Opened from a state the song holds: that is its state until it changes.
    if (! openedFrom.isEmpty())
        stateTakenAt = stateChanges;
}

SandboxedInstance::~SandboxedInstance()
{
    // Told of no more, then closed in its helper (remote), then its helper let go of (keepHelper).
    stopTimer();
    textAnswers.stopTimer();
    answerTextsAsGone();

    if (onGone != nullptr)
        onGone();

    remote->setListener (nullptr);
    cancelPendingUpdate();
}

bool SandboxedInstance::tryHold() noexcept
{
    auto expected = false;
    return held.compare_exchange_strong (expected, true, std::memory_order_acquire);
}

void SandboxedInstance::hold() noexcept
{
    while (! tryHold())
        std::this_thread::yield();
}

void SandboxedInstance::let() noexcept
{
    held.store (false, std::memory_order_release);
}

void SandboxedInstance::helperLost()
{
    crashed.store (true, std::memory_order_release);

    {
        const juce::ScopedLock locked (lock);
        lossCame = true;
    }

    triggerAsyncUpdate();
}

std::function<bool()> SandboxedInstance::watchHelper() const
{
    // The helper lives as long as what keeps it (the plug-in host's), which this lets go of as it goes.
    return [kept = std::weak_ptr<void> (keepHelper), helper = &remote->getHelper()]
    {
        const auto running = kept.lock();
        return running != nullptr && helper->isRunning();
    };
}

void SandboxedInstance::replaceRemote (std::unique_ptr<sandbox::RemoteInstance> newRemote, double openedAtRate, std::shared_ptr<void> keep,
                                       std::function<void()> gone)
{
    // No block plays while it changes (the engine skips them; a render waits).
    hold();

    if (onGone != nullptr)
        onGone();

    remote->setListener (nullptr);
    auto old = std::exchange (remote, std::move (newRemote));
    auto oldHelper = std::exchange (keepHelper, std::move (keep));
    onGone = std::move (gone);
    channelRate = openedAtRate;
    preparing.store (false, std::memory_order_release);
    prepareAsked = nullptr; // the helper that went owes its answer for ever
    stopTimer();

    {
        const juce::ScopedLock locked (lock);
        latency = remote->getLatencySamples();
        lossCame = false;
    }

    remote->setListener (this);
    crashed.store (false, std::memory_order_release);
    let();

    // Opened again from the state it last had, which it has now; an answer the helper that went owed never comes.
    stateAsked = nullptr;
    given = nullptr;
    stateTakenAt = stateChanges;

    // Its values as the plug-in has them now, as if it had set them itself; and its latency.
    const auto& reopened = remote->getInfo().parameters;

    for (size_t i = 0; i < std::min (parameters.size(), reopened.size()); ++i)
    {
        if (! juce::exactlyEqual (parameters[i]->getValue(), reopened[i].value))
        {
            parameters[i]->setFromPlugin (reopened[i].value);

            if (onParameterChanged != nullptr)
                onParameterChanged ((int) i, reopened[i].value);
        }
    }

    setLatencySamples (remote->getLatencySamples());
    old = nullptr;       // closed in its helper, which has gone
    oldHelper = nullptr; // and that let go of
}

void SandboxedInstance::takeChanges (sandbox::BlockInfo& info, sandbox::ParameterChange* changes, int maxChanges) noexcept
{
    // Rarely any: a plug-in can have thousands of parameters, looked through only when one was set.
    if (! anyPending.exchange (false, std::memory_order_acquire))
        return;

    size_t i = 0;

    for (; i < parameters.size() && info.numChanges < maxChanges; ++i)
        if (float value = 0.0f; parameters[i]->takePending (value))
            changes[info.numChanges++] = { (int) i, 0, value };

    // More set than a block takes: those past here go with the next block, which looks again (they waited until another
    // value was set, however long that was).
    if (i < parameters.size())
        anyPending.store (true, std::memory_order_release);
}

void SandboxedInstance::keepChange (int parameter, float value) noexcept
{
    if (! juce::isPositiveAndBelow (parameter, (int) parameters.size()) || ! std::isfinite (value))
        return;

    parameters[(size_t) parameter]->keepPending (juce::jlimit (0.0f, 1.0f, value));
    anyPending.store (true, std::memory_order_release);
}

void SandboxedInstance::setMovedByEngine (std::span<const int> moved)
{
    for (const auto parameter : movedByEngine)
        parameters[(size_t) parameter]->movedByEngine = false;

    movedByEngine.clear();

    for (const auto parameter : moved)
        setMovedByEngine (parameter, true);
}

int SandboxedInstance::findParameter (const juce::String& ownID) const
{
    if (const auto found = places.find (ownID); found != places.end())
        return found->second;

    return -1;
}

float SandboxedInstance::getParameterValue (int parameter) const
{
    return juce::isPositiveAndBelow (parameter, (int) parameters.size()) ? parameters[(size_t) parameter]->getValue() : 0.0f;
}

void SandboxedInstance::setParameterValue (int parameter, float value)
{
    // Not a value that is no number, which jlimit() passes on (known bug 181).
    if (juce::isPositiveAndBelow (parameter, (int) parameters.size()) && std::isfinite (value))
        parameters[(size_t) parameter]->setValue (juce::jlimit (0.0f, 1.0f, value));
}

juce::String SandboxedInstance::describeValue (int parameter, float value, std::function<void()> whenKnown)
{
    if (! juce::isPositiveAndBelow (parameter, (int) parameters.size()))
        return {};

    if (! std::isfinite (value))
        value = parameters[(size_t) parameter]->getValue();

    value = juce::jlimit (0.0f, 1.0f, value);

    if (const auto known = texts.find ({ parameter, value }); known != texts.end())
        return known->second;

    // Asked of the helper without waiting (known bug 166), one value of a parameter at a time, the newest asked for meanwhile
    // next (a knob turned while the helper is busy asks for none it has left); meanwhile the value as a number, never another
    // value's text as if it were this one's, which the hint bar and a screen reader would read too.
    if (! hasCrashed())
    {
        auto asked =
            std::find_if (textAsks.begin(), textAsks.end(), [parameter] (const TextAsk& ask) { return ask.parameter == parameter; });

        if (asked == textAsks.end())
        {
            textAsks.push_back (
                { parameter, value, remote->askParameterText (parameter, value), devices::Clock::getMilliseconds(), {}, {} });
            asked = std::prev (textAsks.end());
            textAnswers.startTimer (textLookMilliseconds);
        }
        else if (! juce::exactlyEqual (asked->value, value))
        {
            asked->next = value;
        }

        if (whenKnown != nullptr && asked->whenKnown.size() < maxCallsWaiting)
            asked->whenKnown.push_back (std::move (whenKnown));
    }

    return juce::String (value, 2);
}

void SandboxedInstance::readValue (int parameter, const juce::String& text, std::function<void (std::optional<float>)> then)
{
    if (then == nullptr)
        return;

    if (! juce::isPositiveAndBelow (parameter, (int) parameters.size()) || hasCrashed())
    {
        then (std::nullopt); // never a number the plug-in didn't give (the review of known bug 166)
        return;
    }

    valueAsks.push_back ({ text, remote->askParameterValue (parameter, text), devices::Clock::getMilliseconds(), std::move (then) });
    textAnswers.startTimer (textLookMilliseconds);
}

void SandboxedInstance::takeTextAnswers()
{
    // Each answer that came, or that won't (its helper gone, or past its time: a value's text a state's time, a value read
    // a typed value's), taken out before what waits for it is called, which may ask again.
    const auto now = devices::Clock::getMilliseconds();
    std::vector<std::function<void()>> calls;

    for (auto it = textAsks.begin(); it != textAsks.end();)
    {
        auto& ask = *it;

        if (! ask.reply->isDone() && ! hasCrashed() && now - ask.since < (double) getStateMilliseconds())
        {
            ++it;
            continue;
        }

        if (const auto text = sandbox::RemoteInstance::readParameterText (*ask.reply))
        {
            if (texts.size() >= textsKept)
                texts.clear();

            texts[{ ask.parameter, ask.value }] = *text;
        }

        // What waits is told whether its text came or not: one that didn't asks again as it shows the value once more.
        calls.insert (calls.end(), ask.whenKnown.begin(), ask.whenKnown.end());

        if (const auto next = ask.next; next.has_value() && ! hasCrashed() && ! texts.contains ({ ask.parameter, *next }))
        {
            ask = { ask.parameter, *next, remote->askParameterText (ask.parameter, *next), now, {}, {} };
            ++it;
        }
        else
        {
            it = textAsks.erase (it);
        }
    }

    for (auto it = valueAsks.begin(); it != valueAsks.end();)
    {
        if (! it->reply->isDone() && ! hasCrashed())
        {
            ++it;
            continue;
        }

        // The plug-in's answer, checked as it came (RemoteInstance.h), waited for as long as its helper lives and answers;
        // none if its helper went (never a number the plug-in didn't give: the review of known bug 166).
        const auto came = it->reply->isDone() && ! it->reply->getReply().isEmpty();
        calls.push_back ([then = std::move (it->then),
                          value = came ? sandbox::RemoteInstance::readParameterValue (*it->reply) : std::nullopt] { then (value); });
        it = valueAsks.erase (it);
    }

    if (textAsks.empty() && valueAsks.empty())
        textAnswers.stopTimer();

    // Last, nothing of this touched after them: what they do may let go of it.
    for (const auto& call : calls)
        call();
}

void SandboxedInstance::answerTextsAsGone()
{
    // Each ask still to come answered with nothing, a message later (this is going): a text read reads as none, and a
    // value's text is shown as a number again, asked of the instance in its place (known bug 166's review).
    for (auto& ask : std::exchange (valueAsks, {}))
        juce::MessageManager::callAsync ([then = std::move (ask.then)] { then (std::nullopt); });

    for (auto& ask : std::exchange (textAsks, {}))
        for (auto& tell : ask.whenKnown)
            juce::MessageManager::callAsync (std::move (tell));
}

void SandboxedInstance::setMovedByEngine (int parameter, bool isMoved)
{
    if (! juce::isPositiveAndBelow (parameter, (int) parameters.size()) || parameters[(size_t) parameter]->movedByEngine == isMoved)
        return;

    parameters[(size_t) parameter]->movedByEngine = isMoved;

    if (isMoved)
        movedByEngine.push_back (parameter);
    else
        std::erase (movedByEngine, parameter);
}

std::int64_t SandboxedInstance::getDeadlineNanoseconds (int numSamples, int maximumBlockSize, double sampleRate) noexcept
{
    const auto lasting = [sampleRate] (int samples) { return (std::int64_t) (1.0e9 * samples / std::max (1.0, sampleRate)); };
    return std::max (lasting (numSamples), std::min (smallestDeadline, lasting (maximumBlockSize)));
}

SandboxedInstance::Answer SandboxedInstance::waitForAnswer (std::int64_t limitNanoseconds, const std::atomic<bool>* cancelled) noexcept
{
    auto& channel = remote->getChannel();
    const auto since = sandbox::getNanoseconds();

    for (;;)
    {
        // Nothing rings the bell for a helper that has gone: it is looked for between the slices.
        if (channel.waitForAnswer (0, waitNanoseconds) == sandbox::BlockChannel::Played::done)
            return Answer::played;

        if (cancelled != nullptr && cancelled->load (std::memory_order_acquire))
            return Answer::cancelled;

        if (hasCrashed())
            return Answer::gone;

        if (sandbox::getNanoseconds() - since > limitNanoseconds)
        {
            remote->getHelper().reportHung();
            return Answer::hung;
        }
    }
}

void SandboxedInstance::fillInPluginDescription (juce::PluginDescription& out) const
{
    out = description;
}

const juce::String SandboxedInstance::getName() const
{
    return description.name;
}

bool SandboxedInstance::isBusesLayoutSupported (const BusesLayout& layout) const
{
    // Its buses as the helper set the plug-in up: stereo, and its sidechain on or off.
    return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
           && (layout.inputBuses.isEmpty() || layout.inputBuses[0] == juce::AudioChannelSet::stereo())
           && (layout.inputBuses.size() < 2 || layout.inputBuses[1] == juce::AudioChannelSet::stereo()
               || layout.inputBuses[1].isDisabled());
}

double SandboxedInstance::getTailLengthSeconds() const
{
    return remote->getInfo().tailSeconds;
}

bool SandboxedInstance::acceptsMidi() const
{
    return remote->getInfo().isInstrument || description.isInstrument;
}

void SandboxedInstance::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    // What it is to play at, kept by a crashed one too, which its reload opens at.
    setRateAndBufferSizeDetails (sampleRate, maximumBlockSize);
    preparedRate.store (sampleRate, std::memory_order_relaxed);
    preparedBlockSize.store (maximumBlockSize, std::memory_order_relaxed);

    if (hasCrashed())
        return;

    // A new channel only for another rate or a longer block than it takes, asked of the helper without waiting (known bug
    // 166: the message thread waited for it, behind whatever else the helper did): the engine skips its blocks until it has
    // come. Not asked while the helper keeps a block, which it can't prepare the plug-in before it gives back: the channel
    // waits for that block first. Not waited for while a render holds the channel, which it may for as long as a plug-in
    // takes over a block (the live plan made again on the message thread as the render switched a plug-in to play offline
    // and it said another latency): the engine skips its blocks from now on, and the timer prepares it once the channel is
    // let go of (timerCallback()), unless the render has prepared it first (finishPreparing()). The audio thread holds it for
    // most of every block and lets go within one: tried for a block's length first, spinning, so that a plug-in that
    // changed its latency as it played (its processor made anew) isn't skipped until the timer's next look.
    auto isHeld = tryHold();

    if (! isHeld)
    {
        const auto blockNanoseconds = (std::int64_t) (1.0e9 * std::max (1, maximumBlockSize) / std::max (1.0, sampleRate));
        const auto until = sandbox::getNanoseconds() + std::max (minimumHoldNanoseconds, blockNanoseconds);

        while (! isHeld && sandbox::getNanoseconds() < until)
        {
            devices::pauseInSpin();
            isHeld = tryHold();
        }
    }

    if (! isHeld)
    {
        if (! isPreparing())
            preparingSince = sandbox::getNanoseconds();

        preparing.store (true, std::memory_order_release);
        startTimer (preparingLookMilliseconds);
        return;
    }

    prepareHeld();
    let();
}

void SandboxedInstance::prepareHeld()
{
    if (isPrepared() && prepareAsked == nullptr)
    {
        preparing.store (false, std::memory_order_release);
    }
    else
    {
        // The clock of one that prepares already goes on (known bug 166: each new plan put the limit off).
        if (! isPreparing())
            preparingSince = sandbox::getNanoseconds();

        preparing.store (true, std::memory_order_release);
        prepareNow();

        if (isPreparing())
            startTimer (preparingLookMilliseconds);
    }
}

bool SandboxedInstance::isPrepared() noexcept
{
    return juce::exactlyEqual (channelRate, getPreparedRate())
           && std::max (smallestChannel, getPreparedBlockSize()) <= remote->getChannel().getLimits().maxSamples;
}

void SandboxedInstance::prepareNow()
{
    // The channel asked for before, taken in once the helper has answered (or has gone); then, for a rate or a block asked
    // since, another asked for, once the helper has given back the block it keeps. One the helper couldn't make leaves the
    // channel there is.
    if (prepareAsked != nullptr)
    {
        if (! prepareAsked->reply->isDone() && ! hasCrashed())
            return;

        const auto askedRate = prepareAsked->sampleRate;
        const auto askedSamples = prepareAsked->channel->getLimits().maxSamples;

        if (remote->finishPreparing (*prepareAsked))
            channelRate = remote->getSampleRate();

        prepareAsked = nullptr;

        if (juce::exactlyEqual (askedRate, getPreparedRate()) && std::max (smallestChannel, getPreparedBlockSize()) <= askedSamples)
        {
            preparing.store (false, std::memory_order_release);
            return;
        }
    }

    if (isPrepared() || hasCrashed())
    {
        preparing.store (false, std::memory_order_release);
        return;
    }

    if (remote->getChannel().isBusy())
        return;

    auto limits = remote->getChannel().getLimits();
    limits.maxSamples = std::max ({ smallestChannel, getPreparedBlockSize(), limits.maxSamples });
    prepareAsked = remote->askToPrepare (getPreparedRate(), limits);
    preparingSince = sandbox::getNanoseconds();

    if (prepareAsked == nullptr)
        preparing.store (false, std::memory_order_release);
}

void SandboxedInstance::playLive()
{
    if (! hasCrashed())
        remote->playLive();
}

bool SandboxedInstance::finishPreparing (const std::atomic<bool>* cancelled)
{
    // On a thread that may wait (a render's, or JUCE's processBlock()), the channel held, the helper's block back: the channel
    // waited for, a little at a time, until it has come for the rate and block asked last (one asked before, for others, is
    // taken in and another asked), its helper has gone, or the render is cancelled (known bug 166: a render cancelled waited
    // up to a minute, and one that began while an earlier channel was still to come played the plug-in's blocks as silence);
    // the helper taken for hung if it doesn't come in time.
    const auto since = sandbox::getNanoseconds();

    while (isPreparing() && ! hasCrashed() && ! remote->getChannel().isBusy())
    {
        if (cancelled != nullptr && cancelled->load (std::memory_order_acquire))
            return false;

        prepareNow();

        if (prepareAsked != nullptr)
            prepareAsked->reply->wait (waitMilliseconds);

        if (sandbox::getNanoseconds() - since > (std::int64_t) prepareMilliseconds.load() * 1'000'000)
        {
            remote->getHelper().reportHung();
            return false;
        }
    }

    // Prepared for what was asked: not merely done preparing, as a channel the helper refused, or one that couldn't be made,
    // leaves the old one, for another rate or a shorter block (the review of known bug 166: a render played it so).
    return ! isPreparing() && ! hasCrashed() && isPrepared();
}

void SandboxedInstance::timerCallback()
{
    // One whose helper has gone prepares as it is reloaded (replaceRemote()).
    if (! isPreparing() || hasCrashed())
    {
        stopTimer();
        return;
    }

    // Not while a block plays, nor while a render has the channel (it prepares it itself).
    if (! tryHold())
        return;

    prepareNow();

    if (! isPreparing())
    {
        stopTimer();
    }
    else if (const auto waited = sandbox::getNanoseconds() - preparingSince;
             (prepareAsked == nullptr && waited > (std::int64_t) remote->getHelper().getAudioHangMilliseconds() * 1'000'000)
             || (prepareAsked != nullptr && waited > (std::int64_t) prepareMilliseconds.load() * 1'000'000))
    {
        // A block kept as long as a hang takes, or a channel the helper doesn't make in time.
        remote->getHelper().reportHung();
    }

    let();
}

void SandboxedInstance::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // A block through JUCE's processor: the channel waited for, and the plug-in for as long as it takes.
    hold();

    if (remote->getChannel().isBusy())
        waitForAnswer ((std::int64_t) remote->getHelper().getAudioHangMilliseconds() * 1'000'000);

    finishPreparing();
    playBlock (buffer, midi);
    let();
}

void SandboxedInstance::playBlock (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi)
{
    auto& channel = remote->getChannel();
    const auto& limits = channel.getLimits();
    auto& info = channel.getInfo();
    const auto numSamples = std::min (buffer.getNumSamples(), limits.maxSamples);
    const auto numMain = std::min (2, buffer.getNumChannels());

    if (hasCrashed() || channel.isBusy() || isPreparing())
    {
        buffer.clear();
        return;
    }

    info.numSamples = numSamples;
    info.numChannels = numMain;
    info.numSidechainChannels = 0;

    for (int i = 0; i < numMain; ++i)
        juce::FloatVectorOperations::copy (channel.getChannel (i), buffer.getReadPointer (i), numSamples);

    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
    {
        const auto first = getChannelIndexInProcessBlockBuffer (true, 1, 0);

        for (int i = 0; i < 2 && first + i < buffer.getNumChannels(); ++i)
            juce::FloatVectorOperations::copy (channel.getSidechain (i), buffer.getReadPointer (first + i), numSamples);

        info.numSidechainChannels = std::min (2, buffer.getNumChannels() - first);
    }

    info.numEvents = 0;

    for (const auto metadata : midi)
    {
        if (info.numEvents >= limits.maxEvents || metadata.numBytes > 3)
            continue;

        auto& event = channel.getEvents()[info.numEvents++];
        event.sample = metadata.samplePosition;
        event.size = (std::uint8_t) metadata.numBytes;
        std::copy (metadata.data, metadata.data + metadata.numBytes, event.bytes);
    }

    info.numChanges = 0;
    takeChanges (info, channel.getChanges(), limits.maxChanges);

    // The song's place, as the processor's playhead has it.
    if (const auto position = getPlayHead() != nullptr ? getPlayHead()->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>())
    {
        info.tempo = position->getBpm().orFallback (120.0);
        info.beat = position->getPpqPosition().orFallback (0.0);
        info.sample = position->getTimeInSamples().orFallback (0);
        info.isPlaying = position->getIsPlaying() ? 1 : 0;

        if (const auto signature = position->getTimeSignature())
        {
            info.timeSignatureNumerator = signature->numerator;
            info.timeSignatureDenominator = signature->denominator;
        }

        // The bar it is in, counted from the song's start in its time signature where the playhead doesn't say.
        info.barLength = TimeSignature { info.timeSignatureNumerator, info.timeSignatureDenominator }.getBarLength().inBeats();
        info.barCount = position->getBarCount().orFallback ((std::int64_t) std::floor (info.beat / info.barLength));
        info.barStart = position->getPpqPositionOfLastBarStart().orFallback ((double) info.barCount * info.barLength);
    }

    info.isRendering = 1;
    info.reset = 0;

    // As long as the plug-in takes, but for a block kept so long that it is a hang, which its helper is ended for, or a
    // helper that has gone, which nothing tells the channel of.
    const auto hangNanoseconds = (std::int64_t) remote->getHelper().getAudioHangMilliseconds() * 1'000'000;

    const auto handedOver = channel.handOver();

    if (handedOver)
        noteHandOver();

    if (! handedOver || waitForAnswer (hangNanoseconds) != Answer::played)
    {
        buffer.clear();
        return;
    }

    for (int i = 0; i < std::min (2, buffer.getNumChannels()); ++i)
        buffer.copyFrom (i, 0, channel.getChannel (i), numSamples);
}

int SandboxedInstance::getNumPrograms()
{
    return std::max (1, (int) remote->getInfo().programs.size());
}

int SandboxedInstance::getCurrentProgram()
{
    return juce::jlimit (0, getNumPrograms() - 1, remote->getProgram());
}

void SandboxedInstance::setCurrentProgram (int index)
{
    // Without waiting for the helper (known bug 166), which takes what it is asked in turn.
    if (! hasCrashed() && juce::isPositiveAndBelow (index, (int) remote->getInfo().programs.size()))
    {
        noteStateChange();
        keepGiven (remote->askToSetProgram (index));
    }
}

void SandboxedInstance::keepGiven (std::shared_ptr<sandbox::Helper::Reply> reply)
{
    // Its answer kept until the next is given (the helper takes them in turn): a plug-in may take long to take a state, its
    // helper's message thread answering nothing meanwhile, which isn't a hang until the setting's time is up.
    reply->stopHoldingOffHangAfter (settingMilliseconds.load());
    given = std::move (reply);
}

const juce::String SandboxedInstance::getProgramName (int index)
{
    const auto& programs = remote->getInfo().programs;
    return juce::isPositiveAndBelow (index, (int) programs.size()) ? programs[(size_t) index] : juce::String();
}

void SandboxedInstance::getStateInformation (juce::MemoryBlock& state)
{
    takeState (state);
}

bool SandboxedInstance::takeState (juce::MemoryBlock& state)
{
    auto isNow = false;

    if (! hasCrashed())
    {
        if (auto taken = remote->getState (stateMilliseconds))
        {
            lastState = std::move (*taken);
            stateTakenAt = stateChanges;
            editorSinceTaken = editorsOpen > 0;
            isNow = true;
        }
        else
        {
            stateMissedAt = stateChanges;
        }
    }

    state = lastState;
    return isNow;
}

void SandboxedInstance::setStateInformation (const void* data, int size)
{
    // The state it has now: an answer still to come from before is older, and is given up; taken in, it would replace this
    // one, and the next store would write it over what the song gave the plug-in (a preset, an undo).
    lastState.replaceAll (data, (size_t) std::max (0, size));
    givenSinceOpened = true;
    stateAsked = nullptr;
    stateTakenAt = stateChanges;
    editorSinceTaken = editorsOpen > 0;

    // Without waiting for the helper (known bug 166): what is asked of it after this, it answers after it.
    if (! hasCrashed())
        keepGiven (remote->askToSetState (lastState));
}

void SandboxedInstance::keepStateAsOpened() noexcept
{
    if (stateTakenAt >= 0)
        stateTakenAt = stateChanges;

    openedAt = stateChanges;
    givenSinceOpened = false;
}

bool SandboxedInstance::mayHaveChangedState() const noexcept
{
    return stateTakenAt != stateChanges || editorsOpen > 0 || editorSinceTaken;
}

bool SandboxedInstance::isStateOlder() const noexcept
{
    return stateTakenAt != stateChanges;
}

void SandboxedInstance::noteStateChange() noexcept
{
    ++stateChanges;
    changedAt = devices::Clock::getMilliseconds();

    if (firstUnaskedChangeAt <= 0.0)
        firstUnaskedChangeAt = changedAt;

    if (onStateMayChange != nullptr)
        onStateMayChange();
}

bool SandboxedInstance::isStateUnasked() const noexcept
{
    return ! hasCrashed() && stateTakenAt != stateChanges && stateMissedAt != stateChanges;
}

bool SandboxedInstance::shouldBeAsked() const noexcept
{
    // Once it has been quiet for a moment, or at the latest a second after the first change it hasn't been asked about, as
    // changes go on (a plug-in that reports a value of its own all the time: an LFO's, a meter's); and no more often than ten
    // times as long as its last answer took (a state of tens of megabytes costs both sides each time).
    const auto now = devices::Clock::getMilliseconds();
    const auto due =
        now - changedAt >= quietMilliseconds || (firstUnaskedChangeAt > 0.0 && now - firstUnaskedChangeAt >= latestAskMilliseconds);
    return stateAsked == nullptr && isStateUnasked() && due && now - stateAskedSince >= answerSpacing * lastAnswerMilliseconds;
}

void SandboxedInstance::editorOpened() noexcept
{
    ++editorsOpen;
    ++editorMoves;
    editorSinceTaken = true;
}

void SandboxedInstance::editorClosed() noexcept
{
    editorsOpen = std::max (0, editorsOpen - 1);
    ++editorMoves;
}

void SandboxedInstance::askState (Ask which, bool unasked)
{
    // A store, and what is asked unasked, ask what said it changed and answers, so that one that didn't answer isn't asked
    // again until it changes again, nor is what a save just took; Auto Save asks what may have changed, as its window was
    // open too, and asks again one that didn't answer before.
    const auto wanted = which == Ask::all || (which == Ask::changed && mayHaveChangedState())
                        || (which == Ask::changedAndAnswering && stateTakenAt != stateChanges && stateMissedAt != stateChanges);

    if (stateAsked != nullptr || hasCrashed() || ! wanted)
        return;

    stateAsked = remote->askState();
    stateAskedUnasked = unasked;
    stateAskedAt = stateChanges;
    firstUnaskedChangeAt = 0.0;
    editorMovesAtAsk = editorMoves;
    stateAskedSince = devices::Clock::getMilliseconds();
}

bool SandboxedInstance::takeAskedState (double waitUntil, bool giveUp)
{
    if (stateAsked == nullptr)
        return true;

    if (const auto now = devices::Clock::getMilliseconds(); ! stateAsked->isDone() && waitUntil > now)
        stateAsked->wait ((int) std::ceil (waitUntil - now));

    if (stateAsked->isDone())
    {
        // As it was when it was asked: changed since, it is asked again at the next save; and so is one whose window is open,
        // or was shown or closed meanwhile.
        lastAnswerMilliseconds = devices::Clock::getMilliseconds() - stateAskedSince;
        ++answers;

        if (auto taken = sandbox::RemoteInstance::readState (*stateAsked))
        {
            lastState = std::move (*taken);
            stateTakenAt = stateAskedAt;
            editorSinceTaken = editorsOpen > 0 || editorMoves != editorMovesAtAsk;
        }
        else
        {
            stateMissedAt = stateAskedAt;
        }

        stateAsked = nullptr;
        return true;
    }

    if (giveUp || devices::Clock::getMilliseconds() - stateAskedSince >= (double) stateMilliseconds.load())
    {
        stateMissedAt = stateAskedAt; // given up on: the helper is taken for hung if it still doesn't answer
        stateAsked = nullptr;
        return true;
    }

    return false;
}

void SandboxedInstance::takeAskedStateIfIn()
{
    // Or given up on once its time has passed, if it was asked unasked (while it waits, the helper isn't taken for hung:
    // sandbox/client/Helper.h); what else asked for it (Auto Save, a save, a store) gives it up itself.
    if (stateAsked != nullptr
        && (stateAsked->isDone()
            || (stateAskedUnasked && devices::Clock::getMilliseconds() - stateAskedSince >= (double) stateMilliseconds.load())))
        takeAskedState (0.0, true);
}

void SandboxedInstance::setStateMilliseconds (int milliseconds) noexcept
{
    stateMilliseconds = milliseconds > 0 ? milliseconds : defaultStateMilliseconds;
}

void SandboxedInstance::setPrepareMilliseconds (int milliseconds) noexcept
{
    prepareMilliseconds = milliseconds > 0 ? milliseconds : defaultPrepareMilliseconds;
}

int SandboxedInstance::getPrepareMilliseconds() noexcept
{
    return prepareMilliseconds.load();
}

int SandboxedInstance::getStateMilliseconds() noexcept
{
    return stateMilliseconds.load();
}

void SandboxedInstance::setSettingMilliseconds (int milliseconds) noexcept
{
    settingMilliseconds = milliseconds > 0 ? milliseconds : defaultSettingMilliseconds;
}

int SandboxedInstance::getSettingMilliseconds() noexcept
{
    return settingMilliseconds.load();
}

//==============================================================================
void SandboxedInstance::parameterChanged (int index, float value)
{
    // Checked as it came (RemoteInstance::handleNotice()); here too, as nothing past this may see what isn't a number.
    if (! std::isfinite (value))
        return;

    // The last of each parameter's alone until the message thread takes them (known bug 181's review: a helper that told
    // values without end grew this, and each wrote the song): one for each parameter at most.
    {
        const juce::ScopedLock locked (lock);

        if (! juce::isPositiveAndBelow (index, (int) changedSlots.size()))
            return;

        if (auto& slot = changedSlots[(size_t) index]; slot >= 0)
        {
            changedByPlugin[(size_t) slot].second = value;
        }
        else
        {
            slot = (int) changedByPlugin.size();
            changedByPlugin.emplace_back (index, value);
        }
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::gestureChanged (int index, bool starting)
{
    {
        const juce::ScopedLock locked (lock);

        if (! juce::isPositiveAndBelow (index, (int) gesturesTold.size()))
            return;

        auto& told = gesturesTold[(size_t) index];

        if (! told.pending)
        {
            told = { true, starting, false };
            gesturesByPlugin.push_back (index);
        }

        told.begun = starting;
        told.sawBegin = told.sawBegin || starting;
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::latencyChanged (int samples)
{
    {
        const juce::ScopedLock locked (lock);
        latency = samples;
        latencyCame = true;
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::stateChanged()
{
    {
        const juce::ScopedLock locked (lock);
        stateCame = true;
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::editorResized (int width, int height)
{
    {
        const juce::ScopedLock locked (lock);
        editorSize = std::pair (width, height);
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::editorKey (bool isDown, int keyCode, int modifiers, int scanCode)
{
    {
        const juce::ScopedLock locked (lock);

        if (editorKeys.size() >= maxNoticesWaiting)
            return;

        editorKeys.push_back ({ isDown, keyCode, modifiers, scanCode });
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::handleAsyncUpdate()
{
    std::vector<std::pair<int, float>> changes;
    struct Told
    {
        int index;
        bool begun, sawBegin;
    };

    std::vector<Told> gestures;
    std::vector<EditorKey> keys;
    std::optional<std::pair<int, int>> newEditorSize;
    bool newLatency = false, newState = false, lost = false;
    int samples = 0;

    {
        const juce::ScopedLock locked (lock);
        changes.swap (changedByPlugin);

        for (const auto& [index, value] : changes)
            changedSlots[(size_t) index] = -1;

        for (const auto index : std::exchange (gesturesByPlugin, {}))
        {
            auto& told = gesturesTold[(size_t) index];
            gestures.push_back ({ index, told.begun, told.sawBegin });
            told = {};
        }
        keys.swap (editorKeys);
        std::swap (newEditorSize, editorSize);
        std::swap (newLatency, latencyCame);
        std::swap (newState, stateCame);
        std::swap (lost, lossCame);
        samples = latency;
    }

    // Its editor's keys first, as they came: they play.
    for (const auto& key : keys)
        if (onEditorKey != nullptr)
            onEditorKey (key.isDown, key.keyCode, key.modifiers, key.scanCode);

    if (newEditorSize.has_value() && onEditorResized != nullptr)
        onEditorResized (newEditorSize->first, newEditorSize->second);

    // As the helper told them: the gestures that begin, the values, then (a message later) the gestures that end. Each
    // parameter's in turn, whatever the helper tells (known bug 181's review: two begun, or one ended that wasn't begun,
    // reached JUCE as they came), from the last it told of each since the last turn: one begun again before its end has gone
    // on is the same gesture; one begun and ended meanwhile begins and ends.
    inGesture.resize (parameters.size(), false);

    for (const auto& told : gestures)
    {
        const auto index = told.index;

        if (! juce::isPositiveAndBelow (index, (int) parameters.size()))
            continue;

        const auto ending = std::find (gesturesToEnd.begin(), gesturesToEnd.end(), index);

        if (told.begun && inGesture[(size_t) index])
        {
            if (ending != gesturesToEnd.end())
                gesturesToEnd.erase (ending);

            continue;
        }

        if (! inGesture[(size_t) index] && (told.begun || told.sawBegin))
        {
            inGesture[(size_t) index] = true;
            parameters[(size_t) index]->beginChangeGesture();

            if (onGesture != nullptr)
                onGesture (index, true);
        }

        if (! told.begun && inGesture[(size_t) index]
            && std::find (gesturesToEnd.begin(), gesturesToEnd.end(), index) == gesturesToEnd.end())
            gesturesToEnd.push_back (index);
    }

    // A value it tells again as it was (many plug-ins do, as their windows open) changes nothing, nor is an unsaved change.
    for (const auto& [index, value] : changes)
    {
        if (juce::isPositiveAndBelow (index, (int) parameters.size())
            && ! juce::exactlyEqual (parameters[(size_t) index]->getValue(), value))
        {
            noteStateChange();
            parameters[(size_t) index]->setFromPlugin (value);

            if (onParameterChanged != nullptr)
                onParameterChanged (index, value);
        }
    }

    if (! gesturesToEnd.empty())
        endedGestures.triggerAsyncUpdate();

    if (newLatency)
    {
        setLatencySamples (samples);

        if (onLatencyChanged != nullptr)
            onLatencyChanged();
    }

    if (newState)
        noteStateChange();

    if (newState && onStateChanged != nullptr)
        onStateChanged();

    // Its helper gone: no change of its own, which would be an unsaved one; the views are told (the plug-in host's).
    if (lost && hasCrashed() && onLost != nullptr)
        onLost();
}

void SandboxedInstance::endGestures()
{
    for (const auto index : std::exchange (gesturesToEnd, {}))
    {
        if (! inGesture[(size_t) index])
            continue;

        inGesture[(size_t) index] = false;
        parameters[(size_t) index]->endChangeGesture();

        if (onGesture != nullptr)
            onGesture (index, false);
    }
}
} // namespace phazite
