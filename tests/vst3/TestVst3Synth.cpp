// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' VST3 instrument (vst3/TestVst3s.h): a sine for each note held, by its channel and pitch, at its pitch (bent by
// its channel's bend, an octave up on the second program) and its velocity over its channel's number, times its Level,
// with an input bus it doesn't listen to.
#include "TestVst3s.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
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
        addParameter (
            level = new juce::AudioParameterFloat (juce::ParameterID { synth::levelID, 1 }, "Level", 0.0f, 1.0f, synth::programLevels[0]));
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
        for (auto& channel : voices)
            for (auto& voice : channel)
                voice = {};

        bends.fill (0.0);
    }

    using juce::AudioProcessor::processBlock;
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

            // Every message but a note's and the bend's, controllers, all notes off and program changes among them, does
            // nothing: its program is set only as its host sets it (setCurrentProgram()).
            const auto message = metadata.getMessage();
            const auto channel = (size_t) juce::jlimit (1, 16, message.getChannel());

            if (message.isNoteOn())
                voices[channel - 1][(size_t) message.getNoteNumber()] = { true, 0.0,
                                                                          synth::fullLevel * message.getFloatVelocity() / (float) channel };
            else if (message.isNoteOff())
                voices[channel - 1][(size_t) message.getNoteNumber()].on = false;
            else if (message.isPitchWheel())
                bends[channel - 1] = synth::bendSemitones * (message.getPitchWheelValue() - 8192) / 8192.0;
        }

        play (buffer, done, buffer.getNumSamples());
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }

    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    int getNumPrograms() override { return (int) std::size (synth::programs); }
    int getCurrentProgram() override { return program.load(); }
    const juce::String getProgramName (int index) override
    {
        return juce::isPositiveAndBelow (index, getNumPrograms()) ? synth::programs[index] : "";
    }
    void changeProgramName (int, const juce::String&) override {}

    void setCurrentProgram (int index) override
    {
        if (juce::isPositiveAndBelow (index, getNumPrograms()))
        {
            program = index;
            *level = synth::programLevels[index]; // which tells its host, as a synth's program setting its parameters does
        }
    }

    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    struct Voice
    {
        bool on = false;
        double phase = 0.0; // in cycles
        float level = 0.0f;
    };

    std::array<std::array<Voice, 128>, 16> voices {}; // by channel and pitch
    std::array<double, 16> bends {};                  // each channel's, in semitones
    std::atomic<int> program { 0 };                   // set on the message thread, read as it plays
    juce::AudioParameterFloat* level = nullptr;       // owned by the processor
    double rate = 48000.0;

    // The notes held, from one sample of the block to another, on both sides.
    void play (juce::AudioBuffer<float>& buffer, int from, int to)
    {
        if (buffer.getNumChannels() < 2)
            return;

        const auto octaves = (double) program.load();
        const auto gain = level->get();

        for (size_t channel = 0; channel < voices.size(); ++channel)
        {
            for (size_t note = 0; note < voices[channel].size(); ++note)
            {
                auto& voice = voices[channel][note];

                if (! voice.on)
                    continue;

                const auto hertz = 440.0 * std::pow (2.0, ((double) note - 69.0 + bends[channel]) / 12.0 + octaves);
                const auto cycles = hertz / rate;

                for (int i = from; i < to; ++i)
                {
                    const auto sample = gain * voice.level * (float) std::sin (juce::MathConstants<double>::twoPi * voice.phase);
                    buffer.addSample (0, i, sample);
                    buffer.addSample (1, i, sample);
                    voice.phase += cycles;
                    voice.phase -= std::floor (voice.phase);
                }
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
