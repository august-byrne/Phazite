// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestAudioDevice.h"

#include <set>

namespace phazite::test
{
TestAudioDevice::TestAudioDevice()
    : juce::AudioIODevice ("Test", typeName)
{
}

TestAudioDevice::~TestAudioDevice()
{
    close();
}

TestAudioDevice* TestAudioDevice::findIn (juce::AudioDeviceManager& devices)
{
    return dynamic_cast<TestAudioDevice*> (devices.getCurrentAudioDevice());
}

juce::String TestAudioDevice::open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels, double rate,
                                    int bufferSizeSamples)
{
    inputs = inputChannels & juce::BigInteger ((1 << numChannels) - 1);
    outputs = outputChannels & juce::BigInteger ((1 << numChannels) - 1);
    sampleRate = rate > 0.0 ? rate : 48000.0;
    bufferSize = bufferSizeSamples > 0 ? bufferSizeSamples : 480;
    incoming.setSize (numChannels, bufferSize);
    opened = true;
    return {};
}

void TestAudioDevice::close()
{
    stop();
    opened = false;
}

void TestAudioDevice::start (juce::AudioIODeviceCallback* newCallback)
{
    if (newCallback == nullptr || ! opened)
        return;

    newCallback->audioDeviceAboutToStart (this);
    const juce::ScopedLock held (lock);
    callback = newCallback;
}

void TestAudioDevice::stop()
{
    juce::AudioIODeviceCallback* stopped = nullptr;

    {
        const juce::ScopedLock held (lock);
        stopped = callback.exchange (nullptr);
    }

    if (stopped != nullptr)
        stopped->audioDeviceStopped();
}

void TestAudioDevice::process (juce::AudioBuffer<float>& buffer)
{
    const juce::ScopedLock held (lock);
    auto* playing = callback.load();
    const auto numSamples = std::min (buffer.getNumSamples(), bufferSize);

    if (playing == nullptr || numSamples <= 0)
        return;

    // What comes in, copied before what goes out takes its place: the open inputs in their order, as a driver hands them.
    const float* in[numChannels] {};
    float* out[numChannels] {};
    int numIn = 0, numOut = 0;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        if (inputs[channel])
        {
            incoming.copyFrom (numIn, 0, buffer, std::min (channel, buffer.getNumChannels() - 1), 0, numSamples);
            in[numIn] = incoming.getReadPointer (numIn);
            ++numIn;
        }

        if (outputs[channel] && channel < buffer.getNumChannels())
            out[numOut++] = buffer.getWritePointer (channel);
    }

    playing->audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, {});
}

//==============================================================================
TestAudioDeviceType::TestAudioDeviceType()
    : juce::AudioIODeviceType (TestAudioDevice::typeName)
{
}

juce::AudioIODevice* TestAudioDeviceType::createDevice (const juce::String&, const juce::String&)
{
    auto device = std::make_unique<TestAudioDevice>();
    device->setInputLatency (inputLatency);
    return device.release();
}

TestAudioDevice* TestAudioDeviceType::open (juce::AudioDeviceManager& devices, double sampleRate, int blockSize, int numInputs,
                                            int inputDelay)
{
    // The type once for each device manager, added before JUCE makes its own (which would look for the computer's devices).
    static std::set<const juce::AudioDeviceManager*> withType;

    if (withType.insert (&devices).second)
        devices.addAudioDeviceType (std::make_unique<TestAudioDeviceType>());

    // A device made anew, with what it reports.
    devices.closeAudioDevice();
    devices.setCurrentAudioDeviceType (TestAudioDevice::typeName, true);

    for (auto* type : devices.getAvailableDeviceTypes())
        if (auto* ours = dynamic_cast<TestAudioDeviceType*> (type))
            ours->inputLatency = inputDelay;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = "Test";
    setup.inputDeviceName = numInputs > 0 ? "Test" : "";
    setup.sampleRate = sampleRate;
    setup.bufferSize = blockSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.setRange (0, std::clamp (numInputs, 0, TestAudioDevice::numChannels), true);
    setup.outputChannels.setRange (0, TestAudioDevice::numChannels, true);

    if (devices.setAudioDeviceSetup (setup, true).isNotEmpty())
        return nullptr;

    return TestAudioDevice::findIn (devices);
}

void TestAudioDeviceType::close (juce::AudioDeviceManager& devices)
{
    devices.closeAudioDevice();
}
} // namespace phazite::test
