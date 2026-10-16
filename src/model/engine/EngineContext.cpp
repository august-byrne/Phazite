// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EngineContext.h"
#include "internal/EngineContext.h"
#include "model/mixer/Mixer.h"

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

te::Engine& getEngine (EngineContext& context)
{
    return context.getImpl().engine;
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

juce::StringArray EngineContext::getAudioFileExtensions() const
{
    juce::StringArray extensions;
    auto& formats = impl->engine.getAudioFileFormatManager().readFormatManager;

    for (int i = 0; i < formats.getNumKnownFormats(); ++i)
        for (const auto& extension : formats.getKnownFormat (i)->getFileExtensions())
            extensions.addIfNotAlreadyThere (extension.toLowerCase());

    return extensions;
}

std::vector<DeviceType> EngineContext::getDeviceTypes() const
{
    return phazite::getDeviceTypes (impl->engine);
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
