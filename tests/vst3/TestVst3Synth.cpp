// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' VST3 instrument (vst3/TestVst3s.h): a sine for each note held, at its pitch and velocity, with an input bus
// it doesn't listen to.
#include "TestVst3s.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>

namespace
{
namespace synth = phazite::test::vst3::synth;

class TestVst3Synth final : public juce::AudioProcessor
{
public:
    TestVst3Synth()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return synth::name; }

    // Stereo out; stereo in, or none.
    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto in = layout.getMainInputChannelSet();
        return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
               && (in == juce::AudioChannelSet::stereo() || in.isDisabled());
    }

    void prepareToPlay (double sampleRate, int) override
    {
        rate = sampleRate;
        reset();
    }

    void releaseResources() override {}

    void reset() override
    {
        for (auto& voice : voices)
            voice = {};
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        // What came in is left out: the output is the notes alone.
        buffer.clear();
        auto done = 0;

        for (const auto metadata : midi)
        {
            const auto at = juce::jlimit (done, buffer.getNumSamples(), metadata.samplePosition);
            play (buffer, done, at);
            done = at;

            // Every message but a note's, controllers and all notes off among them, does nothing.
            const auto message = metadata.getMessage();

            if (message.isNoteOn())
                voices[(size_t) message.getNoteNumber()] = { true, 0.0, synth::fullLevel * message.getFloatVelocity() };
            else if (message.isNoteOff())
                voices[(size_t) message.getNoteNumber()].on = false;
        }

        play (buffer, done, buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    struct Voice
    {
        bool on = false;
        double phase = 0.0; // in cycles
        float level = 0.0f;
    };

    std::array<Voice, 128> voices {};
    double rate = 48000.0;

    // The notes held, from one sample of the block to another, on both sides.
    void play (juce::AudioBuffer<float>& buffer, int from, int to)
    {
        if (buffer.getNumChannels() < 2)
            return;

        for (size_t note = 0; note < voices.size(); ++note)
        {
            auto& voice = voices[note];

            if (! voice.on)
                continue;

            const auto cycles = juce::MidiMessage::getMidiNoteInHertz ((int) note) / rate;

            for (int i = from; i < to; ++i)
            {
                const auto sample = voice.level * (float) std::sin (juce::MathConstants<double>::twoPi * voice.phase);
                buffer.addSample (0, i, sample);
                buffer.addSample (1, i, sample);
                voice.phase += cycles;
                voice.phase -= std::floor (voice.phase);
            }
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestVst3Synth)
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestVst3Synth();
}
