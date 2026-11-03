// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/SandboxedInstance.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace phazite
{
namespace
{
    constexpr int stateMilliseconds = 10000; // how long a plug-in's state may take to come or go
    constexpr int prepareMilliseconds = 10000;
    constexpr int textMilliseconds = 250; // how a value reads, for a tooltip or a field
    constexpr int smallestChannel = 4096; // the longest block a channel takes at least
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
// Phazite's engine moves it, and sends it with its blocks), one set while a block plays (automation, as Tracktion's
// renders play it) joins the next block's changes; one the plug-in set itself comes back without going to it again.
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
        value.store (newValue, std::memory_order_relaxed);

        if (juce::MessageManager::existsAndIsCurrentThread())
        {
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

    // What the plug-in set itself: told to Tracktion, not to the helper again (message thread).
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

    juce::String getText (float normalised, int maximumLength) const override
    {
        if (auto text = owner.remote->getParameterText (index, normalised, textMilliseconds); text.has_value() && ! owner.hasCrashed())
            return text->substring (0, maximumLength);

        return juce::String (normalised, 2).substring (0, maximumLength);
    }

    float getValueForText (const juce::String& text) const override { return juce::jlimit (0.0f, 1.0f, text.getFloatValue()); }

    // A value set while a block played, once (on the thread that plays the next).
    bool takePending (float& newValue) noexcept
    {
        if (! isPending.exchange (false, std::memory_order_acquire))
            return false;

        newValue = pending.load (std::memory_order_relaxed);
        return true;
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
                                      std::shared_ptr<void> keep, std::function<void()> gone, EditorMaker editorMaker)
    : juce::AudioPluginInstance (getBuses (instance->getInfo())),
      keepHelper (std::move (keep)),
      onGone (std::move (gone)),
      makeEditor (std::move (editorMaker)),
      remote (std::move (instance)),
      description (described),
      latency (remote->getLatencySamples())
{
    const auto& info = remote->getInfo();

    for (size_t i = 0; i < info.parameters.size(); ++i)
    {
        auto parameter = std::make_unique<Parameter> (*this, (int) i, info.parameters[i]);
        parameters.push_back (parameter.get());
        addHostedParameter (std::move (parameter));
    }

    setLatencySamples (latency);
    remote->setListener (this);
}

SandboxedInstance::~SandboxedInstance()
{
    // Told of no more, then closed in its helper (remote), then its helper let go of (keepHelper).
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

void SandboxedInstance::replaceRemote (std::unique_ptr<sandbox::RemoteInstance> newRemote, std::shared_ptr<void> keep,
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

    {
        const juce::ScopedLock locked (lock);
        latency = remote->getLatencySamples();
        lossCame = false;
    }

    remote->setListener (this);
    crashed.store (false, std::memory_order_release);
    let();

    // Its values as the plug-in has them now, as if it had set them itself; and its latency.
    const auto& reopened = remote->getInfo().parameters;

    for (size_t i = 0; i < std::min (parameters.size(), reopened.size()); ++i)
        if (! juce::exactlyEqual (parameters[i]->getValue(), reopened[i].value))
            parameters[i]->setFromPlugin (reopened[i].value);

    setLatencySamples (remote->getLatencySamples());
    old = nullptr;       // closed in its helper, which has gone
    oldHelper = nullptr; // and that let go of
}

void SandboxedInstance::takeChanges (sandbox::BlockInfo& info, sandbox::ParameterChange* changes, int maxChanges) noexcept
{
    // Rarely any: a plug-in can have thousands of parameters, looked through only when one was set.
    if (! anyPending.exchange (false, std::memory_order_acquire))
        return;

    for (size_t i = 0; i < parameters.size() && info.numChanges < maxChanges; ++i)
        if (float value = 0.0f; parameters[i]->takePending (value))
            changes[info.numChanges++] = { (int) i, 0, value };
}

void SandboxedInstance::setMovedByEngine (std::span<const int> moved)
{
    for (const auto parameter : movedByEngine)
        parameters[(size_t) parameter]->movedByEngine = false;

    movedByEngine.clear();

    for (const auto parameter : moved)
        setMovedByEngine (parameter, true);
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

std::int64_t SandboxedInstance::getDeadlineNanoseconds (int numSamples, double sampleRate) noexcept
{
    return (std::int64_t) (1.0e9 * numSamples / std::max (1.0, sampleRate));
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
    setRateAndBufferSizeDetails (sampleRate, maximumBlockSize);
    const auto channelSize = std::max (smallestChannel, maximumBlockSize);

    // A new channel only for another rate or a longer block than it takes.
    if (hasCrashed() || (juce::exactlyEqual (sampleRate, preparedRate) && channelSize <= remote->getChannel().getLimits().maxSamples))
        return;

    hold();
    auto limits = remote->getChannel().getLimits();
    limits.maxSamples = channelSize;

    if (remote->prepare (sampleRate, limits, prepareMilliseconds))
    {
        preparedRate = sampleRate;
        preparedBlockSize = maximumBlockSize;
    }

    let();
}

void SandboxedInstance::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // One of Tracktion's renders: the channel waited for, and the plug-in for as long as it takes.
    hold();
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

    if (hasCrashed() || channel.isBusy())
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

    // The song's place, as Tracktion's playhead has it.
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
    }

    info.isRendering = 1;
    info.reset = 0;

    // As long as the plug-in takes, but for a block kept so long that it is a hang, which its helper is ended for.
    const auto hangNanoseconds = (std::int64_t) remote->getHelper().getAudioHangMilliseconds() * 1'000'000;

    if (channel.play (0, hangNanoseconds) != sandbox::BlockChannel::Played::done)
    {
        if (channel.isBusy())
            remote->getHelper().reportHung();

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
    if (! hasCrashed() && juce::isPositiveAndBelow (index, (int) remote->getInfo().programs.size()))
        remote->setProgram (index, stateMilliseconds);
}

const juce::String SandboxedInstance::getProgramName (int index)
{
    const auto& programs = remote->getInfo().programs;
    return juce::isPositiveAndBelow (index, (int) programs.size()) ? programs[(size_t) index] : juce::String();
}

void SandboxedInstance::getStateInformation (juce::MemoryBlock& state)
{
    if (! hasCrashed())
        if (auto taken = remote->getState (stateMilliseconds))
            lastState = std::move (*taken);

    state = lastState;
}

void SandboxedInstance::setStateInformation (const void* data, int size)
{
    lastState.replaceAll (data, (size_t) std::max (0, size));

    if (! hasCrashed())
        remote->setState (lastState, stateMilliseconds);
}

//==============================================================================
void SandboxedInstance::parameterChanged (int index, float value)
{
    {
        const juce::ScopedLock locked (lock);
        changedByPlugin.emplace_back (index, value);
    }

    triggerAsyncUpdate();
}

void SandboxedInstance::gestureChanged (int index, bool starting)
{
    {
        const juce::ScopedLock locked (lock);
        gesturesByPlugin.emplace_back (index, starting);
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

void SandboxedInstance::editorKey (bool isDown, int keyCode, int modifiers)
{
    {
        const juce::ScopedLock locked (lock);
        editorKeys.push_back ({ isDown, keyCode, modifiers });
    }

    triggerAsyncUpdate();
}

bool SandboxedInstance::hasEditor() const
{
    return makeEditor != nullptr && remote->getInfo().hasEditor;
}

juce::AudioProcessorEditor* SandboxedInstance::createEditor()
{
    return hasEditor() && ! hasCrashed() ? makeEditor (*this) : nullptr;
}

void SandboxedInstance::handleAsyncUpdate()
{
    std::vector<std::pair<int, float>> changes;
    std::vector<std::pair<int, bool>> gestures;
    std::vector<EditorKey> keys;
    std::optional<std::pair<int, int>> newEditorSize;
    bool newLatency = false, newState = false, lost = false;
    int samples = 0;

    {
        const juce::ScopedLock locked (lock);
        changes.swap (changedByPlugin);
        gestures.swap (gesturesByPlugin);
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
            onEditorKey (key.isDown, key.keyCode, key.modifiers);

    if (newEditorSize.has_value() && onEditorResized != nullptr)
        onEditorResized (newEditorSize->first, newEditorSize->second);

    // As the helper told them: the gestures that begin, the values, then (a message later, once Tracktion has the values)
    // the gestures that end.
    for (const auto& [index, starting] : gestures)
    {
        if (! juce::isPositiveAndBelow (index, (int) parameters.size()))
            continue;

        if (starting)
        {
            parameters[(size_t) index]->beginChangeGesture();

            if (onGesture != nullptr)
                onGesture (index, true);
        }
        else
        {
            gesturesToEnd.push_back (index);
        }
    }

    for (const auto& [index, value] : changes)
        if (juce::isPositiveAndBelow (index, (int) parameters.size()))
            parameters[(size_t) index]->setFromPlugin (value);

    if (! gesturesToEnd.empty())
        endedGestures.triggerAsyncUpdate();

    if (newLatency)
        setLatencySamples (samples);

    if (newState)
        updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));

    // Its helper gone: no change of its own, which would be an unsaved one; the views are told (the plug-in host's).
    if (lost && hasCrashed() && onLost != nullptr)
        onLost();
}

void SandboxedInstance::endGestures()
{
    for (const auto index : std::exchange (gesturesToEnd, {}))
    {
        parameters[(size_t) index]->endChangeGesture();

        if (onGesture != nullptr)
            onGesture (index, false);
    }
}
} // namespace phazite
