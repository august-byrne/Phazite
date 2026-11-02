// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestPlugins.h"

#include <array>
#include <cmath>

namespace phazite::test
{
namespace
{
    using Kind = TestPlugins::Kind;

    // A parameter as a plug-in format hosts one (a VST3's): with the plug-in's own ID for it.
    class StandInParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        StandInParameter (juce::String ownID, juce::String parameterName, float defaultValue)
            : id (std::move (ownID)),
              name (std::move (parameterName)),
              initial (defaultValue),
              value (defaultValue)
        {
        }

        juce::String getParameterID() const override { return id; }
        float getValue() const override { return value.load(); }
        void setValue (float newValue) override { value = newValue; }
        float getDefaultValue() const override { return initial; }
        juce::String getName (int maximumLength) const override { return name.substring (0, maximumLength); }
        juce::String getLabel() const override { return {}; }
        juce::String getText (float v, int) const override { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
        float getValueForText (const juce::String& text) const override { return text.getFloatValue() / 100.0f; }

    private:
        const juce::String id, name;
        const float initial;
        std::atomic<float> value;
    };

    // What every stand-in has: its description, a generic editor, one program, and its parameters' values as its state,
    // in their order.
    class StandIn : public juce::AudioPluginInstance
    {
    public:
        StandIn (Kind k, const BusesProperties& buses)
            : juce::AudioPluginInstance (buses),
              kind (k)
        {
        }

        void fillInPluginDescription (juce::PluginDescription& description) const override
        {
            description = TestPlugins::getDescription (kind);
        }

        const juce::String getName() const override { return TestPlugins::getDescription (kind).name; }

        void prepareToPlay (double, int) override {}
        void releaseResources() override {}

        using juce::AudioProcessor::processBlock;

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return kind == Kind::synth; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
        bool hasEditor() const override { return true; }

        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}

        void getStateInformation (juce::MemoryBlock& data) override
        {
            juce::MemoryOutputStream out (data, false);

            for (auto* parameter : getParameters())
                out.writeFloat (parameter->getValue());
        }

        void setStateInformation (const void* data, int size) override
        {
            juce::MemoryInputStream in (data, (size_t) size, false);

            for (auto* parameter : getParameters())
                if (in.getNumBytesRemaining() >= 4)
                    parameter->setValue (in.readFloat());
        }

    protected:
        StandInParameter* add (const char* ownID, const char* name, float defaultValue)
        {
            auto parameter = std::make_unique<StandInParameter> (ownID, name, defaultValue);
            auto* added = parameter.get();
            addHostedParameter (std::move (parameter));
            return added;
        }

        static BusesProperties stereoEffect()
        {
            return BusesProperties()
                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
        }

    private:
        const Kind kind;
    };

    class TestEffect final : public StandIn
    {
    public:
        TestEffect()
            : StandIn (Kind::effect, stereoEffect())
        {
            gain = add ("1", "Gain", 1.0f);
            add ("0", "Tone", 0.5f);
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain (gain->getValue()); }

        // Its programs, Full and Half, set its Gain, and tell the host so, as a plug-in's programs do.
        int getNumPrograms() override { return 2; }
        int getCurrentProgram() override { return program; }
        const juce::String getProgramName (int index) override { return index == 0 ? "Full" : index == 1 ? "Half" : ""; }

        void setCurrentProgram (int index) override
        {
            program = juce::jlimit (0, 1, index);
            gain->setValueNotifyingHost (program == 0 ? 1.0f : 0.5f);
            updateHostDisplay (ChangeDetails().withProgramChanged (true));
        }

    private:
        StandInParameter* gain = nullptr;
        int program = 0;
    };

    class TestSynth final : public StandIn
    {
    public:
        TestSynth()
            : StandIn (Kind::synth, BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        {
            level = add ("10", "Level", 0.25f);
        }

        void prepareToPlay (double sampleRate, int) override { rate = sampleRate; }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
        {
            buffer.clear();
            auto done = 0;

            for (const auto metadata : midi)
            {
                playNotes (buffer, done, metadata.samplePosition);
                done = metadata.samplePosition;
                const auto message = metadata.getMessage();

                // Note 127: its own Level to 0.75, in a gesture held while the note is, as a control of its window turns it.
                if (message.isNoteOn() && message.getNoteNumber() == 127)
                {
                    level->beginChangeGesture();
                    level->setValueNotifyingHost (0.75f);
                }
                else if (message.isNoteOff() && message.getNoteNumber() == 127)
                    level->endChangeGesture();
                else if (message.isNoteOn())
                    start (message.getNoteNumber());
                else if (message.isNoteOff() || message.isAllNotesOff())
                    stop (message.isNoteOff() ? message.getNoteNumber() : -1);
            }

            playNotes (buffer, done, buffer.getNumSamples());
        }

    private:
        struct Voice
        {
            int note = -1;
            double phase = 0.0;
        };

        StandInParameter* level = nullptr;
        std::array<Voice, 16> voices {};
        double rate = 44100.0;

        void start (int note)
        {
            for (auto& voice : voices)
                if (voice.note < 0)
                {
                    voice = { note, 0.0 };
                    return;
                }
        }

        void stop (int note)
        {
            for (auto& voice : voices)
                if (note < 0 || voice.note == note)
                    voice.note = -1;
        }

        void playNotes (juce::AudioBuffer<float>& buffer, int from, int to)
        {
            for (auto& voice : voices)
            {
                if (voice.note < 0)
                    continue;

                const auto step = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (voice.note) / rate;

                for (int i = from; i < to; ++i)
                {
                    const auto sample = level->getValue() * (float) std::sin (voice.phase);
                    voice.phase += step;

                    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                        buffer.addSample (channel, i, sample);
                }
            }
        }
    };

    class TestCrash final : public StandIn
    {
    public:
        TestCrash()
            : StandIn (Kind::crash, stereoEffect())
        {
            crash = add ("20", "Crash", 0.0f);
        }

        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
        {
            // As a plug-in that crashes does: an access to memory that isn't there.
            if (crash->getValue() > 0.5f)
            {
                volatile int* nowhere = nullptr;
                *nowhere = 1;
            }
        }

    private:
        StandInParameter* crash = nullptr;
    };

    class TestHang final : public StandIn
    {
    public:
        TestHang()
            : StandIn (Kind::hang, stereoEffect())
        {
            hang = add ("30", "Hang", 0.0f);
        }

        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override
        {
            while (hang->getValue() > 0.5f)
                juce::Thread::sleep (1);
        }

    private:
        StandInParameter* hang = nullptr;
    };

    class TestLatency final : public StandIn
    {
    public:
        TestLatency()
            : StandIn (Kind::latency, stereoEffect())
        {
            longer = add ("40", "Latency", 0.0f);
            setLatencySamples (64);
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            const auto latency = longer->getValue() > 0.5f ? 128 : 64;

            if (latency != getLatencySamples())
                setLatencySamples (latency);

            // A delay of latency samples, through a ring of 128 for each side.
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                for (int channel = 0; channel < std::min (2, buffer.getNumChannels()); ++channel)
                {
                    auto& ring = rings[(size_t) channel];
                    ring[(size_t) at] = buffer.getSample (channel, i);
                    buffer.setSample (channel, i, ring[(size_t) ((at - latency + 128) % 128)]);
                }

                at = (at + 1) % 128;
            }
        }

    private:
        StandInParameter* longer = nullptr;
        std::array<std::array<float, 128>, 2> rings {};
        int at = 0;
    };

    class TestSidechain final : public StandIn
    {
    public:
        TestSidechain()
            : StandIn (Kind::sidechain, stereoEffect().withInput ("Sidechain", juce::AudioChannelSet::stereo(), false))
        {
        }

        bool isBusesLayoutSupported (const BusesLayout& layout) const override
        {
            return layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
                   && layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
                   && (layout.inputBuses.size() < 2 || layout.inputBuses[1] == juce::AudioChannelSet::stereo()
                       || layout.inputBuses[1].isDisabled());
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            // What came into its sidechain, in place of its sound (silence without one).
            auto sidechain = getBusBuffer (buffer, true, 1);

            for (int channel = 0; channel < 2; ++channel)
            {
                if (channel < sidechain.getNumChannels())
                    buffer.copyFrom (channel, 0, sidechain, channel, 0, buffer.getNumSamples());
                else
                    buffer.clear (channel, 0, buffer.getNumSamples());
            }
        }
    };

    // Each kind's name, its "file" in the format, and its number.
    struct Described
    {
        const char* name;
        const char* file;
        int uniqueId;
        bool isInstrument;
    };

    Described describe (Kind kind)
    {
        switch (kind)
        {
            case Kind::effect:
                return { "Test Effect", "PhaziteTestEffect", 0x7e57e5, false };
            case Kind::synth:
                return { "Test Synth", "PhaziteTestSynth", 0x7e5751, true };
            case Kind::crash:
                return { "Test Crash", "PhaziteTestCrash", 0x7e57c1, false };
            case Kind::hang:
                return { "Test Hang", "PhaziteTestHang", 0x7e57a1, false };
            case Kind::latency:
                return { "Test Latency", "PhaziteTestLatency", 0x7e5711, false };
            case Kind::sidechain:
                return { "Test Sidechain", "PhaziteTestSidechain", 0x7e5751 + 1, false };
        }

        return { "Test Effect", "PhaziteTestEffect", 0x7e57e5, false };
    }

    class TestFormat final : public juce::AudioPluginFormat
    {
    public:
        juce::String getName() const override { return TestPlugins::formatName; }

        void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& file) override
        {
            for (const auto kind : TestPlugins::kinds)
                if (file == describe (kind).file)
                    results.add (new juce::PluginDescription (TestPlugins::getDescription (kind)));
        }

        bool fileMightContainThisPluginType (const juce::String& file) override { return file.startsWith ("PhaziteTest"); }
        juce::String getNameOfPluginFromIdentifier (const juce::String& file) override { return file; }
        bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
        bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
        bool canScanForPlugins() const override { return false; }
        bool isTrivialToScan() const override { return true; }

        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override
        {
            juce::StringArray files;

            for (const auto kind : TestPlugins::kinds)
                files.add (describe (kind).file);

            return files;
        }

        juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
        bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

    protected:
        void createPluginInstance (const juce::PluginDescription& description, double, int, PluginCreationCallback callback) override
        {
            for (const auto kind : TestPlugins::kinds)
                if (description.fileOrIdentifier == describe (kind).file)
                    return callback (TestPlugins::create (kind), {});

            callback (nullptr, "No such test plug-in.");
        }
    };
} // namespace

juce::PluginDescription TestPlugins::getDescription (Kind kind)
{
    const auto described = describe (kind);
    juce::PluginDescription description;
    description.name = described.name;
    description.descriptiveName = description.name;
    description.pluginFormatName = formatName;
    description.category = described.isInstrument ? "Synth" : "Effect";
    description.manufacturerName = "Phazite Tests";
    description.version = "1.0";
    description.fileOrIdentifier = described.file;
    description.uniqueId = described.uniqueId;
    description.isInstrument = described.isInstrument;
    description.numInputChannels = described.isInstrument ? 0 : 2;
    description.numOutputChannels = 2;
    return description;
}

std::unique_ptr<juce::AudioPluginInstance> TestPlugins::create (Kind kind)
{
    switch (kind)
    {
        case Kind::effect:
            return std::make_unique<TestEffect>();
        case Kind::synth:
            return std::make_unique<TestSynth>();
        case Kind::crash:
            return std::make_unique<TestCrash>();
        case Kind::hang:
            return std::make_unique<TestHang>();
        case Kind::latency:
            return std::make_unique<TestLatency>();
        case Kind::sidechain:
            return std::make_unique<TestSidechain>();
    }

    return nullptr;
}

std::unique_ptr<juce::AudioPluginFormat> TestPlugins::createFormat()
{
    return std::make_unique<TestFormat>();
}
} // namespace phazite::test
