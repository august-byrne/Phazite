// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineContext.h"
#include "internal/EngineContext.h"

namespace phazite
{
struct EngineContext::Impl
{
    te::Engine& engine;
};

std::unique_ptr<EngineContext> createEngineContext (te::Engine& engine)
{
    return std::make_unique<EngineContext> (std::make_unique<EngineContext::Impl> (engine));
}

EngineContext::EngineContext (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

EngineContext::~EngineContext() = default;

juce::AudioDeviceManager& EngineContext::getAudioDeviceManager()
{
    return impl->engine.getDeviceManager().deviceManager;
}

double EngineContext::getCpuUsage() const
{
    return impl->engine.getDeviceManager().getCpuUsage();
}

std::vector<EngineContext::MidiInput> EngineContext::getMidiInputs() const
{
    std::vector<MidiInput> inputs;

    // Not Phazite's own inputs (the computer keyboard's, for one), which Tracktion lists too.
    for (const auto& device : impl->engine.getDeviceManager().getMidiInDevices())
        if (device->getDeviceType() == te::InputDevice::physicalMidiDevice)
            inputs.push_back ({ device->getDeviceID(), device->getName(), device->isEnabled() });

    return inputs;
}

void EngineContext::setMidiInputOn (const juce::String& id, bool on)
{
    // Tracktion may replace its device objects when devices change: found by ID each time.
    for (const auto& device : impl->engine.getDeviceManager().getMidiInDevices())
        if (device->getDeviceID() == id)
            device->setEnabled (on);
}

void EngineContext::addChangeListener (juce::ChangeListener* listener)
{
    impl->engine.getDeviceManager().addChangeListener (listener);
}

void EngineContext::removeChangeListener (juce::ChangeListener* listener)
{
    impl->engine.getDeviceManager().removeChangeListener (listener);
}
} // namespace phazite
