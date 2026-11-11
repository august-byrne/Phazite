// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Instance.h"
#include "devices/core/AudioThreads.h"
#include "sandbox/core/Vst3State.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace phazite::host
{
namespace
{
    constexpr std::int64_t checkForExitNanoseconds = 100'000'000; // how long the thread sleeps before it checks it should end
    constexpr int maximumChannels = 32;                           // a plug-in's channels the thread plays at most
    constexpr int maximumPrograms = 1024;                         // the programs' names Phazite is told at most

    // Its main buses in stereo, a stereo sidechain as its second input where it takes one, and no others; failing that,
    // without the sidechain; failing that, its main buses as they are.
    void chooseLayout (juce::AudioPluginInstance& plugin)
    {
        auto layout = plugin.getBusesLayout();
        const auto stereo = juce::AudioChannelSet::stereo();

        for (int i = 0; i < layout.inputBuses.size(); ++i)
            layout.inputBuses.getReference (i) = i < 2 ? stereo : juce::AudioChannelSet::disabled();

        for (int i = 0; i < layout.outputBuses.size(); ++i)
            layout.outputBuses.getReference (i) = i == 0 ? stereo : juce::AudioChannelSet::disabled();

        if (plugin.setBusesLayout (layout))
            return;

        if (layout.inputBuses.size() > 1)
        {
            layout.inputBuses.getReference (1) = juce::AudioChannelSet::disabled();

            if (plugin.setBusesLayout (layout))
                return;
        }

        plugin.disableNonMainBuses();
    }

    juce::String getOwnID (juce::AudioProcessorParameter& parameter, int index)
    {
        if (const auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (&parameter))
            if (hosted->getParameterID().isNotEmpty())
                return hosted->getParameterID();

        return juce::String (index);
    }
} // namespace

// The song's place for the plug-in, set for each piece it plays (on the thread that plays it): so far on from the block's
// start while the song plays, where the playhead is in every piece while it is stopped; and the bar it is in, the block's
// or one of the bars after it, each as long.
class Instance::PlayHead final : public juce::AudioPlayHead
{
public:
    void set (const sandbox::BlockInfo& info, double rate, int offset) noexcept
    {
        const auto moved = info.isPlaying != 0 ? offset : 0;
        const auto ppq = info.beat + moved * info.tempo / (60.0 * rate);
        const auto barLength = info.barLength > 0.0 ? info.barLength : 4.0;
        const auto barsOn = moved > 0 ? std::max (0.0, std::floor ((ppq - info.barStart) / barLength + 1.0e-9)) : 0.0;

        position = {};
        position.setBpm (info.tempo);
        position.setTimeSignature (juce::AudioPlayHead::TimeSignature { info.timeSignatureNumerator, info.timeSignatureDenominator });
        position.setTimeInSamples (info.sample + moved);
        position.setTimeInSeconds ((double) (info.sample + moved) / rate);
        position.setPpqPosition (ppq);
        position.setPpqPositionOfLastBarStart (info.barStart + barsOn * barLength);
        position.setBarCount (info.barCount + (std::int64_t) barsOn);
        position.setIsPlaying (info.isPlaying != 0);
    }

    juce::Optional<PositionInfo> getPosition() const override { return position; }

private:
    PositionInfo position;
};

Instance::Instance (std::int64_t instanceID, std::unique_ptr<juce::AudioPluginInstance> pluginToHost,
                    std::unique_ptr<sandbox::BlockChannel> blockChannel, double sampleRate)
    : juce::Thread ("Phazite plug-in"),
      id (instanceID),
      plugin (std::move (pluginToHost)),
      playHead (std::make_unique<PlayHead>())
{
    chooseLayout (*plugin);

    if (auto* sidechain = plugin->getBus (true, 1); sidechain != nullptr && sidechain->isEnabled() && sidechain->getNumberOfChannels() == 2)
        sidechainChannel = plugin->getChannelIndexInProcessBlockBuffer (true, 1, 0);

    parameters = plugin->getParameters();
    changedValues = std::make_unique<std::atomic<float>[]> ((size_t) parameters.size());
    changed = std::make_unique<std::atomic<bool>[]> ((size_t) parameters.size());

    for (auto* parameter : parameters)
        parameter->addListener (this);

    // A VST3's class ID, from the preset file JUCE's VST3 client writes with it.
    if (const auto* vst3 = plugin->getVST3Client())
        vst3ClassID = sandbox::readVst3ClassID (vst3->getPreset());

    plugin->addListener (this);
    plugin->setPlayHead (playHead.get());
    start (std::move (blockChannel), sampleRate);
}

Instance::~Instance()
{
    stop();
    plugin->removeListener (this);

    for (auto* parameter : parameters)
        parameter->removeListener (this);
}

sandbox::PluginInfo Instance::describe() const
{
    sandbox::PluginInfo info;
    info.name = plugin->getName();
    info.isInstrument = plugin->getPluginDescription().isInstrument || (plugin->acceptsMidi() && plugin->getTotalNumInputChannels() == 0);
    info.latencySamples = plugin->getLatencySamples();
    info.tailSeconds = plugin->getTailLengthSeconds();
    info.numInputChannels = plugin->getMainBusNumInputChannels();
    info.numOutputChannels = plugin->getMainBusNumOutputChannels();
    info.hasSidechain = sidechainChannel >= 0;

    for (int i = 0; i < parameters.size(); ++i)
    {
        auto& parameter = *parameters[i];
        sandbox::ParameterInfo described;
        described.id = getOwnID (parameter, i);
        described.name = parameter.getName (128);
        described.label = parameter.getLabel();
        described.defaultValue = parameter.getDefaultValue();
        described.value = parameter.getValue();
        described.numSteps = parameter.isDiscrete() ? parameter.getNumSteps() : 0;
        described.isBoolean = parameter.isBoolean();
        described.isAutomatable = parameter.isAutomatable();
        info.parameters.push_back (described);
    }

    // Its programs, but not the one nameless program that a plug-in without programs of its own may report (JUCE's
    // AudioProcessor counts at least one; a VST3 without a program list reports none).
    if (const auto numPrograms = plugin->getNumPrograms(); numPrograms > 1 || plugin->getProgramName (0).isNotEmpty())
        for (int i = 0; i < std::min (numPrograms, maximumPrograms); ++i)
            info.programs.push_back (plugin->getProgramName (i));

    info.program = plugin->getCurrentProgram();
    info.hasEditor = plugin->hasEditor();
    return info;
}

void Instance::start (std::unique_ptr<sandbox::BlockChannel> blockChannel, double sampleRate)
{
    channel = std::move (blockChannel);
    rate = sampleRate;
    const auto& limits = channel->getLimits();
    plugin->setRateAndBufferSizeDetails (rate, limits.maxSamples);
    plugin->prepareToPlay (rate, limits.maxSamples);
    numPluginChannels = std::min (maximumChannels, std::max (plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()));
    extra.setSize (std::max (1, numPluginChannels), limits.maxSamples);
    midi.ensureSize ((size_t) limits.maxEvents * 16);
    devices::startAudioThread (*this);
}

void Instance::stop()
{
    // Waited for however long the plug-in takes over its block, never ended by force, which could leave the plug-in's
    // locks held: a plug-in that hangs keeps the helper's message thread from answering, and Phazite ends the helper
    // (sandbox/client/Helper.h).
    signalThreadShouldExit();
    stopThread (-1);
    plugin->releaseResources();
}

void Instance::prepare (std::unique_ptr<sandbox::BlockChannel> blockChannel, double sampleRate)
{
    stop();
    start (std::move (blockChannel), sampleRate);
}

juce::MemoryBlock Instance::getState()
{
    // As JUCE takes it (which first sends the plug-in the parameter changes on their way), then, for a VST3, the same two
    // streams as its format's own preset file.
    juce::MemoryBlock state;
    plugin->getStateInformation (state);

    if (vst3ClassID.isNotEmpty())
        if (const auto streams = sandbox::readJuceVst3State (state, vst3ClassID))
            return sandbox::writeVst3Preset (*streams);

    return state;
}

void Instance::setState (const juce::MemoryBlock& state)
{
    // Not while a block plays: the plug-in's lock, which the audio thread holds as it plays.
    const juce::ScopedLock held (plugin->getCallbackLock());
    setPluginState (*plugin, state);
}

void setPluginState (juce::AudioPluginInstance& plugin, const juce::MemoryBlock& state)
{
    if (plugin.getVST3Client() != nullptr)
    {
        if (const auto streams = sandbox::readVst3Preset (state))
        {
            const auto wrapped = sandbox::writeJuceVst3State (*streams);
            plugin.setStateInformation (wrapped.getData(), (int) wrapped.getSize());
            return;
        }
    }

    plugin.setStateInformation (state.getData(), (int) state.getSize());
}

void Instance::setProgram (int index)
{
    // As a state is given, not while a block plays.
    if (! juce::isPositiveAndBelow (index, plugin->getNumPrograms()))
        return;

    const juce::ScopedLock held (plugin->getCallbackLock());
    plugin->setCurrentProgram (index);
}

void Instance::setParameter (int index, float value)
{
    if (juce::isPositiveAndBelow (index, parameters.size()))
        parameters[index]->setValue (juce::jlimit (0.0f, 1.0f, value));
}

juce::String Instance::getParameterText (int index, float value) const
{
    return juce::isPositiveAndBelow (index, parameters.size()) ? parameters[index]->getText (juce::jlimit (0.0f, 1.0f, value), 64)
                                                               : juce::String();
}

float Instance::getParameterValue (int index, const juce::String& text) const
{
    // A VST3 reads it with getParamValueByString(), as JUCE asks it; one that can't, as a number.
    return juce::isPositiveAndBelow (index, parameters.size()) ? juce::jlimit (0.0f, 1.0f, parameters[index]->getValueForText (text))
                                                               : 0.0f;
}

void Instance::takeNotices (const std::function<void (sandbox::MessageWriter&)>& send)
{
    using sandbox::MessageType;
    using sandbox::MessageWriter;

    std::vector<std::pair<int, bool>> gesturesNow;

    {
        const juce::ScopedLock held (gestureLock);
        gesturesNow.swap (gestures);
    }

    // The gestures that begin, then the values, then those that end: a value moved by a control grabbed comes in its
    // gesture.
    const auto sendGestures = [&] (bool starting)
    {
        for (const auto& [index, isStart] : gesturesNow)
        {
            if (isStart != starting)
                continue;

            MessageWriter message (MessageType::gesture, 0);
            message.add (id).add ((std::int32_t) index).add (isStart);
            send (message);
        }
    };

    sendGestures (true);

    for (int i = 0; i < parameters.size(); ++i)
    {
        if (changed[(size_t) i].exchange (false))
        {
            MessageWriter message (MessageType::parameterChanged, 0);
            message.add (id).add ((std::int32_t) i).add ((double) changedValues[(size_t) i].load());
            send (message);
        }
    }

    sendGestures (false);

    if (latencyChanged.exchange (false))
    {
        MessageWriter message (MessageType::latencyChanged, 0);
        message.add (id).add ((std::int32_t) plugin->getLatencySamples());
        send (message);
    }

    if (stateChanged.exchange (false))
    {
        MessageWriter message (MessageType::stateChanged, 0);
        message.add (id).add ((std::int32_t) plugin->getCurrentProgram());
        send (message);
    }

    if (parametersChanged.exchange (false))
    {
        MessageWriter message (MessageType::parametersChanged, 0);
        message.add (id);
        sandbox::write (message, describe().parameters);
        send (message);
    }
}

void Instance::run()
{
    const devices::ProAudioTask proAudio;

    while (! threadShouldExit())
    {
        if (! channel->waitForBlock (checkForExitNanoseconds))
            continue;

        {
            const juce::ScopedLock held (plugin->getCallbackLock());
            play();
        }

        channel->finish();
    }
}

void Instance::play() noexcept
{
    const auto& info = channel->getInfo();
    const auto& limits = channel->getLimits();
    const auto numSamples = juce::jlimit (0, limits.maxSamples, info.numSamples);
    const auto* changes = channel->getChanges();
    const auto numChanges = juce::jlimit (0, limits.maxChanges, info.numChanges);

    // Playback starting from a stop: what the plug-in still rings with stops before it plays (a VST3's setActive() off and
    // on, which can take longer than a block: this block is late then, and silent, as the song starts).
    if (info.reset != 0)
        plugin->reset();

    // In pieces between the parameters' changes, each taking those at its start.
    auto start = 0, next = 0;

    while (start < numSamples)
    {
        for (; next < numChanges && changes[next].sample <= start; ++next)
            if (juce::isPositiveAndBelow (changes[next].index, parameters.size()))
                parameters[changes[next].index]->setValue (juce::jlimit (0.0f, 1.0f, changes[next].value));

        const auto end = next < numChanges ? juce::jlimit (start + 1, numSamples, changes[next].sample) : numSamples;
        playPiece (start, end - start);
        start = end;
    }

    // Any past the block's end take effect for the next.
    for (; next < numChanges; ++next)
        if (juce::isPositiveAndBelow (changes[next].index, parameters.size()))
            parameters[changes[next].index]->setValue (juce::jlimit (0.0f, 1.0f, changes[next].value));
}

void Instance::playPiece (int start, int numSamples) noexcept
{
    const auto& info = channel->getInfo();
    const auto& limits = channel->getLimits();
    const auto numChannels = juce::jlimit (0, limits.maxChannels, info.numChannels);
    const auto numSidechain = juce::jlimit (0, limits.maxSidechainChannels, info.numSidechainChannels);

    // The channel's audio in place, its sidechain where the plug-in has its bus, and any other channels of the plug-in's
    // cleared, as anything that didn't come.
    std::array<float*, maximumChannels> channels {};

    for (int i = 0; i < numPluginChannels; ++i)
    {
        const auto sidechain = sidechainChannel >= 0 ? i - sidechainChannel : -1;
        auto heard = false;

        if (sidechain >= 0 && sidechain < limits.maxSidechainChannels)
        {
            channels[(size_t) i] = channel->getSidechain (sidechain) + start;
            heard = sidechain < numSidechain;
        }
        else if (i < limits.maxChannels)
        {
            channels[(size_t) i] = channel->getChannel (i) + start;
            heard = i < numChannels;
        }
        else
        {
            channels[(size_t) i] = extra.getWritePointer (i) + start;
        }

        if (! heard)
            juce::FloatVectorOperations::clear (channels[(size_t) i], numSamples);
    }

    // Its MIDI in the piece (in the last, any at or past the block's end too).
    midi.clear();
    const auto* events = channel->getEvents();
    const auto end = start + numSamples;
    const auto isLast = end >= juce::jlimit (0, limits.maxSamples, info.numSamples);

    for (int i = 0; i < juce::jlimit (0, limits.maxEvents, info.numEvents); ++i)
    {
        const auto& event = events[i];

        if (event.size > 0 && event.size <= 3 && event.sample >= start && (event.sample < end || isLast))
            midi.addEvent (event.bytes, event.size, juce::jlimit (0, numSamples - 1, event.sample - start));
    }

    playHead->set (info, rate, start);
    juce::AudioBuffer<float> buffer (channels.data(), numPluginChannels, numSamples);
    plugin->processBlock (buffer, midi);
}

void Instance::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details)
{
    if (details.latencyChanged)
        latencyChanged = true;

    if (details.parameterInfoChanged)
        parametersChanged = true;

    // Its settings changed: as it says itself (a VST3's setDirty()), or another program. JUCE's VST3 host says the program
    // changed, with new parameter info, at every restartComponent() a VST3 makes, for its latency or its parameters' names
    // too, which many make as they sit there: that alone changes nothing (the values it changes come as theirs), and counting
    // it made an idle project's plug-ins an unsaved change. JUCE workaround W3 (docs/juce-workarounds.md).
    if (details.nonParameterStateChanged || (details.programChanged && ! details.parameterInfoChanged))
        stateChanged = true;
}

void Instance::parameterValueChanged (int index, float value)
{
    if (! juce::isPositiveAndBelow (index, parameters.size()))
        return;

    changedValues[(size_t) index] = value;
    changed[(size_t) index] = true;
}

void Instance::parameterGestureChanged (int index, bool starting)
{
    const juce::ScopedLock held (gestureLock);
    gestures.emplace_back (index, starting);
}
} // namespace phazite::host
