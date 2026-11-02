// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/DevicePlugin.h"
#include "model/engine/internal/EngineSetup.h"

#include <algorithm>

namespace phazite
{
namespace
{
    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // How far an LFO's cycle moves in a sample, as Tracktion's LFO moves it from block to block (its LFOModifierTimer):
    // its rate in hertz, or in parts of a bar at the tempo. One that follows the song's position stands still while the
    // song doesn't play.
    double getCyclesPerSample (const te::LFOModifier& lfo, double tempo, int numerator, double sampleRate, bool isPlaying)
    {
        if (juce::roundToInt (lfo.syncTypeParam->getCurrentValue()) == te::ModifierCommon::transport && ! isPlaying)
            return 0.0;

        const auto rate = (double) lfo.rateParam->getCurrentValue();
        const auto rateType = static_cast<te::ModifierCommon::RateType> (juce::roundToInt (lfo.rateTypeParam->getCurrentValue()));

        if (rateType == te::ModifierCommon::hertz)
            return rate / sampleRate;

        return tempo / 60.0 / numerator * rate / te::ModifierCommon::getBarFraction (rateType) / sampleRate;
    }
} // namespace

DevicePlugin::DevicePlugin (te::PluginCreationInfo creation, const devices::DeviceInfo& info, std::unique_ptr<devices::Processor> dsp)
    : te::Plugin (creation),
      runner (info, std::move (dsp))
{
    auto* undoManager = getUndoManager();

    for (const auto& description : info.parameters)
    {
        const auto id = toString (description.id);
        // Under its ID (Schema.h: no ID is a property Tracktion keeps on every plug-in).
        auto* stored = storedValues.add (new juce::CachedValue<float>());
        stored->referTo (state, juce::Identifier (id), undoManager, description.defaultValue);

        auto* parameter = addParam (id, toString (description.name), devices::getRange (description));
        parameter->attachToCurrentValue (*stored);
        parameters.push_back (parameter);
        values.push_back (description.defaultValue);
    }
}

DevicePlugin::~DevicePlugin()
{
    notifyListenersOfDeletion();

    for (const auto& parameter : parameters)
        parameter->detachFromCurrentValue();
}

te::AutomatableParameter* DevicePlugin::findParameter (std::string_view id) const
{
    const auto index = devices::findParameter (getDeviceInfo(), id);
    return index >= 0 ? parameters[(size_t) index].get() : nullptr;
}

juce::String DevicePlugin::getName() const
{
    return toString (getDeviceInfo().name);
}

juce::String DevicePlugin::getPluginType()
{
    return toString (getDeviceInfo().typeID);
}

juce::String DevicePlugin::getShortName (int)
{
    return getName();
}

juce::String DevicePlugin::getSelectableDescription()
{
    return getName();
}

bool DevicePlugin::takesMidiInput()
{
    return runner.isInstrument();
}

bool DevicePlugin::isSynth()
{
    return runner.isInstrument();
}

bool DevicePlugin::producesAudioWhenNoAudioInput()
{
    return isSynth() || ! noTail();
}

bool DevicePlugin::noTail()
{
    return getTailLength() <= 0.0;
}

double DevicePlugin::getTailLength() const
{
    return runner.getTailSeconds();
}

double DevicePlugin::getLatencySeconds()
{
    return isBypassedForLowLatency() ? 0.0 : latencySeconds;
}

bool DevicePlugin::isBypassedForLowLatency() const
{
    // Never in a render (an export, freeze or bounce), which plays everything as the song has it.
    return latencySeconds > lowLatencyLimitSeconds && isLowLatency (engine) && ! edit.isRendering();
}

void DevicePlugin::getChannelNames (juce::StringArray* ins, juce::StringArray* outs)
{
    te::Plugin::getChannelNames (ins, outs);

    // A sidechain comes in as two more inputs, which Tracktion fills from the source track: picking one wires the
    // source's two channels to them (te::Plugin::guessSidechainRouting()).
    if (ins != nullptr && getDeviceInfo().hasSidechain)
        ins->addArray (juce::StringArray { "Sidechain Left", "Sidechain Right" });
}

te::Plugin::BusLayout DevicePlugin::getBusses() const
{
    // An instrument too, as Tracktion's own synths: being a synth keeps the track's audio from coming in.
    return BusLayout::singleStereoInOut();
}

void DevicePlugin::initialise (const te::PluginInitialisationInfo& info)
{
    runner.prepare (info.sampleRate, info.blockSizeSamples);
    tempoPosition.emplace (te::createPosition (edit.tempoSequence));
    latencySeconds = runner.getLatencySamples() / info.sampleRate;
    updateSidechain();
}

void DevicePlugin::initialiseWithoutStopping (const te::PluginInitialisationInfo&)
{
    updateSidechain();
}

void DevicePlugin::updateSidechain()
{
    // On the message thread, as a new playback graph is built: Tracktion builds one when the source changes.
    sidechainConnected.store (getDeviceInfo().hasSidechain && getSidechainSourceID().isValid() && getNumWires() > 0,
                              std::memory_order_relaxed);
}

void DevicePlugin::reset()
{
    runner.reset();
}

int DevicePlugin::collectEvents (const te::PluginRenderContext& context)
{
    auto* midi = context.bufferForMidiMessages;

    if (midi == nullptr)
        return 0;

    int count = 0;

    if (midi->isAllNotesOff)
        events[(size_t) count++] = { devices::NoteEvent::Type::allNotesOff, 0, 0, 0.0f };

    // The messages are in time order; one that rounds to the block's end stays in it.
    const auto lastSample = context.bufferNumSamples - 1;

    for (const auto& message : *midi)
    {
        if (count == (int) events.size())
            break;

        const auto sample =
            juce::jlimit (0, lastSample, juce::roundToInt ((message.getTimeStamp() + context.midiBufferOffset) * sampleRate));

        if (const auto event = engine::toNoteEvent (message, sample))
            events[(size_t) count++] = *event;
    }

    return count;
}

void DevicePlugin::applyToBuffer (const te::PluginRenderContext& context)
{
    const engine::Handover<DeviceModulation>::Block held { modulations };

    // A device that looks ahead lets the sound through as it comes while low latency is on (model/engine/internal/EngineSetup.h).
    if (context.destBuffer == nullptr || context.bufferNumSamples <= 0 || isBypassedForLowLatency())
        return;

    // Each parameter's value now: one the device modulates itself without Tracktion's modulation.
    auto* plan = held.get();

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        const auto& parameter = *parameters[i];
        values[i] = plan != nullptr && plan->plan.isModulated[i] ? parameter.getCurrentBaseValue() : parameter.getCurrentValue();
    }

    engine::DeviceRunner::Block block;
    block.values = values;
    block.events = std::span (events.data(), (size_t) collectEvents (context));
    block.isPlaying = context.isPlaying;
    block.isRendering = edit.isRendering();
    block.hasSidechain = sidechainConnected.load (std::memory_order_relaxed);

    auto numerator = 4;

    if (tempoPosition.has_value())
    {
        tempoPosition->set (context.editTime.getStart());
        block.tempo = tempoPosition->getTempo();
        block.beat = tempoPosition->getBeats().inBeats();
        numerator = tempoPosition->getTimeSignature().numerator;
    }

    if (plan != nullptr)
    {
        startModulation (*plan, block.tempo, numerator, context.isPlaying);
        block.modulation = &plan->plan;
    }

    runner.process (*context.destBuffer, context.bufferStartSample, context.bufferNumSamples, block);
}

//==============================================================================
void DevicePlugin::setModulation (std::unique_ptr<DeviceModulation> newModulation)
{
    modulations.give (std::move (newModulation));
}

void DevicePlugin::startModulation (DeviceModulation& modulation, double tempo, int numerator, bool isPlaying)
{
    // Each source as the block starts: Tracktion has moved its modulators on to it, and its parameters' macros.
    for (size_t i = 0; i < modulation.sources.size(); ++i)
    {
        const auto& tracktion = modulation.sources[i];
        auto& source = modulation.plan.sources[i];
        source.value = tracktion.macro != nullptr ? tracktion.macro->getCurrentValue() : tracktion.modifier->getCurrentValue();
        source.isOn = tracktion.macro != nullptr || tracktion.modifier->enabledParam->getCurrentValue() >= 0.5f;
        source.followsWave = false;

        if (tracktion.lfo == nullptr)
            continue;

        // An LFO with a wave of its cycle alone is followed through the block from where Tracktion's cycle is now.
        const auto& lfo = *tracktion.lfo;
        const auto wave = static_cast<devices::LfoWave> (juce::roundToInt (lfo.waveParam->getCurrentValue()));

        if (! devices::isPeriodic (wave))
            continue;

        source.followsWave = true;
        source.shape = { wave, lfo.depthParam->getCurrentValue(), lfo.offsetParam->getCurrentValue(),
                         lfo.bipolarParam->getCurrentValue() >= 0.5f };
        source.phase = lfo.getCurrentPhase();
        source.cyclesPerSample = getCyclesPerSample (lfo, tempo, numerator, sampleRate, isPlaying);
    }
}

void DevicePlugin::restorePluginStateFromValueTree (const juce::ValueTree& source)
{
    for (auto* stored : storedValues)
    {
        if (const auto* property = source.getPropertyPointer (stored->getPropertyID()))
            *stored = (float) *property;
        else
            stored->resetToDefault();
    }

    for (const auto& parameter : parameters)
        parameter->updateFromAttachedValue();
}
} // namespace phazite
