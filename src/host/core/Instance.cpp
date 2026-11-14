// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Instance.h"
#include "devices/core/AudioThreads.h"
#include "sandbox/core/Vst3State.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

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

        // A switch as the plug-in says, or a parameter of two steps (each with its own text, which Phazite asks the plug-in
        // for): JUCE's VST3 host says that of a VST3's switch, which VST3 describes as a parameter of one step and no more. JUCE
        // workaround W33 (docs/juce-workarounds.md).
        described.isBoolean = parameter.isBoolean() || described.numSteps == 2;
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
    monoIn = plugin->getMainBusNumInputChannels() == 1;
    monoOut = plugin->getMainBusNumOutputChannels() == 1;
    extra.setSize (std::max (1, numPluginChannels), limits.maxSamples);
    midi.ensureSize ((size_t) (limits.maxEvents + 16 * 128) * 16); // a block's events, and a note-off for every note

    for (auto& notes : notesOn)
        notes.reset();

    endsNotes = false;
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

void Instance::playLive()
{
    offlineWanted = false;
    tellOffline();
}

void Instance::tellOffline()
{
    // The plug-in told whether it plays offline, as the blocks Phazite hands it say (a render's or live playback's), or as
    // Phazite says once a render has given it back: not while a block plays, and not by waiting for one to end (a plug-in
    // that hangs in its block would keep the message thread too, and with it the values that would let it go): tried again
    // at the next tick (takeNotices()).
    if (const auto wanted = offlineWanted.load(); wanted != offline.load())
    {
        const juce::ScopedTryLock held (plugin->getCallbackLock());

        if (held.isLocked())
        {
            setOffline (wanted);
            offline = wanted;
        }
    }
}

void Instance::setOffline (bool isOffline)
{
    // As JUCE's hosts tell a plug-in: set while it isn't prepared, then prepared again, which a VST3 takes its process mode
    // from (kOffline or kRealtime, as JUCE's VST3 host sets it up).
    const auto maximumSamples = channel->getLimits().maxSamples;
    plugin->releaseResources();
    plugin->setNonRealtime (isOffline);
    plugin->setRateAndBufferSizeDetails (rate, maximumSamples);
    plugin->prepareToPlay (rate, maximumSamples);
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

    tellOffline();

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
    const juce::ScopedNoDenormals noDenormals; // as a host's audio thread has them: a plug-in's tails cost no more as they fade

    while (! threadShouldExit())
    {
        if (! channel->waitForBlock (checkForExitNanoseconds))
            continue;

        // A render's block waits for the plug-in to be told that it plays offline, which the message thread tells it (as
        // JUCE's VST3 host prepares a plug-in only there) within a tick of its timer: so that one that streams from disk
        // or loads in the background has what it plays in time, and one with a mode of its own for rendering takes it.
        // A live block after a render's doesn't wait for the plug-in to be told that it plays live again.
        const auto rendering = channel->getInfo().isRendering != 0;
        offlineWanted = rendering;

        while (rendering && ! offline.load() && ! threadShouldExit())
            juce::Thread::sleep (1);

        if (threadShouldExit())
            break;

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
    // on, which can take longer than a block: this block is late then, and silent, as the song starts), and the notes it
    // was given end, for a plug-in whose reset leaves them sounding.
    if (info.reset != 0)
    {
        plugin->reset();
        endsNotes = true;
    }

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

    // Its MIDI in the piece (in the last, any at or past the block's end too), with the notes it gives the plug-in kept
    // count of: where every note is to end, each ends with a note-off of its own, before the controller that says so.
    midi.clear();

    if (std::exchange (endsNotes, false))
        for (int midiChannel = 0; midiChannel < 16; ++midiChannel)
            endNotes (midiChannel, 0);

    const auto* events = channel->getEvents();
    const auto end = start + numSamples;
    const auto isLast = end >= juce::jlimit (0, limits.maxSamples, info.numSamples);

    for (int i = 0; i < juce::jlimit (0, limits.maxEvents, info.numEvents); ++i)
    {
        const auto& event = events[i];

        if (event.size == 0 || event.size > 3 || event.sample < start || (event.sample >= end && ! isLast))
            continue;

        const auto at = juce::jlimit (0, numSamples - 1, event.sample - start);

        if (event.size == 3)
        {
            const auto status = event.bytes[0] & 0xf0;
            const auto midiChannel = event.bytes[0] & 0x0f;
            const auto number = (size_t) (event.bytes[1] & 0x7f);

            if (status == 0x90 && event.bytes[2] > 0)
                notesOn[(size_t) midiChannel].set (number);
            else if (status == 0x80 || status == 0x90)
                notesOn[(size_t) midiChannel].reset (number);
            else if (status == 0xb0 && (number == 123 || number == 120)) // all notes off, all sound off
                endNotes (midiChannel, at);
        }

        midi.addEvent (event.bytes, event.size, at);
    }

    // A plug-in in mono (one that would take neither of its main buses in stereo) between Phazite's two sides: it hears
    // both, half each, on its one input, and its one output goes to both, so that no side is left as it came (or silent).
    const auto stereo = numChannels >= 2 && limits.maxChannels >= 2 && numPluginChannels >= 1;
    auto* right = stereo ? channel->getChannel (1) + start : nullptr;

    if (stereo && monoIn)
    {
        juce::FloatVectorOperations::multiply (channels[0], 0.5f, numSamples);
        juce::FloatVectorOperations::addWithMultiply (channels[0], right, 0.5f, numSamples);
    }

    playHead->set (info, rate, start);
    juce::AudioBuffer<float> buffer (channels.data(), numPluginChannels, numSamples);
    plugin->processBlock (buffer, midi);

    if (stereo && monoOut)
        juce::FloatVectorOperations::copy (right, channels[0], numSamples);
}

void Instance::endNotes (int midiChannel, int sample) noexcept
{
    auto& notes = notesOn[(size_t) midiChannel];

    if (notes.none())
        return;

    for (size_t note = 0; note < notes.size(); ++note)
    {
        if (notes[note])
        {
            const std::uint8_t noteOff[] = { (std::uint8_t) (0x80 | midiChannel), (std::uint8_t) note, 0 };
            midi.addEvent (noteOff, 3, sample);
        }
    }

    notes.reset();
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
