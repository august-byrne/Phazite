// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' VST3 effect (vst3/TestVst3s.h): mono, late by its latency, times its gain, its program's trim and its polarity.
#include "TestVst3s.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <iterator>

namespace
{
namespace effect = phazite::test::vst3::effect;

class TestVst3Effect final : public juce::AudioProcessor
{
public:
    TestVst3Effect()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::mono(), true)
                              .withOutput ("Output", juce::AudioChannelSet::mono(), true))
    {
        addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { effect::gainID, 1 }, "Gain", 0.0f, 1.0f, 1.0f));
        addParameter (invert = new juce::AudioParameterBool (juce::ParameterID { effect::invertID, 1 }, "Invert", false));
        setLatencySamples (effect::latencySamples);
    }

    const juce::String getName() const override { return effect::name; }

    // One channel in and one out, and nothing else.
    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet() == juce::AudioChannelSet::mono()
               && layout.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

    void prepareToPlay (double, int) override { reset(); }
    void releaseResources() override {}

    void reset() override
    {
        delay.fill (0.0f);
        at = 0;
    }

    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (buffer.getNumChannels() < 1)
            return;

        const auto times = gain->get() * trim.load() * (invert->get() ? -1.0f : 1.0f);
        auto* samples = buffer.getWritePointer (0);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto in = samples[i];
            samples[i] = delay[(size_t) at] * times;
            delay[(size_t) at] = in;
            at = (at + 1) % effect::latencySamples;
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    int getNumPrograms() override { return (int) std::size (effect::programs); }
    int getCurrentProgram() override { return program.load(); }
    const juce::String getProgramName (int index) override
    {
        return juce::isPositiveAndBelow (index, getNumPrograms()) ? effect::programs[index] : "";
    }
    void changeProgramName (int, const juce::String&) override {}

    void setCurrentProgram (int index) override
    {
        if (! juce::isPositiveAndBelow (index, getNumPrograms()))
            return;

        program = index;
        trim = effect::trims[index];
    }

    // Its gain, its invert and its trim, but not its program.
    void getStateInformation (juce::MemoryBlock& state) override
    {
        juce::MemoryOutputStream out (state, false);
        out.writeFloat (gain->get());
        out.writeFloat (invert->get() ? 1.0f : 0.0f);
        out.writeFloat (trim.load());
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size < 12)
            return;

        juce::MemoryInputStream in (data, (size_t) size, false);
        *gain = in.readFloat();
        *invert = in.readFloat() > 0.5f;
        trim = in.readFloat();
    }

private:
    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioParameterBool* invert = nullptr;
    std::atomic<float> trim { effect::trims[0] };
    std::atomic<int> program { 0 };
    std::array<float, (size_t) effect::latencySamples> delay {};
    int at = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestVst3Effect)
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestVst3Effect();
}
