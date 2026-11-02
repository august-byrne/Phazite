// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestPlugins.h"

namespace phazite::test
{
namespace
{
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
        float getValueForText (const juce::String& text) const override { return text.getFloatValue(); }

    private:
        const juce::String id, name;
        const float initial;
        std::atomic<float> value;
    };

    class TestEffect final : public juce::AudioPluginInstance
    {
    public:
        TestEffect()
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        {
            auto gainParameter = std::make_unique<StandInParameter> ("1", "Gain", 1.0f);
            auto toneParameter = std::make_unique<StandInParameter> ("0", "Tone", 0.5f);
            gain = gainParameter.get();
            tone = toneParameter.get();
            addHostedParameter (std::move (gainParameter));
            addHostedParameter (std::move (toneParameter));
        }

        void fillInPluginDescription (juce::PluginDescription& description) const override
        {
            description = TestPlugins::getEffectDescription();
        }

        const juce::String getName() const override { return TestPlugins::getEffectDescription().name; }

        void prepareToPlay (double, int) override {}
        void releaseResources() override {}

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain (gain->getValue()); }

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
        bool hasEditor() const override { return true; }

        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}

        // Its parameters' values, as a plug-in keeps its settings: in a block of its own, the gain's then the tone's.
        void getStateInformation (juce::MemoryBlock& data) override
        {
            juce::MemoryOutputStream out (data, false);
            out.writeFloat (gain->getValue());
            out.writeFloat (tone->getValue());
        }

        void setStateInformation (const void* data, int size) override
        {
            if (size < 8)
                return;

            juce::MemoryInputStream in (data, (size_t) size, false);
            gain->setValue (in.readFloat());
            tone->setValue (in.readFloat());
        }

    private:
        StandInParameter* gain = nullptr;
        StandInParameter* tone = nullptr;
    };

    class TestFormat final : public juce::AudioPluginFormat
    {
    public:
        juce::String getName() const override { return TestPlugins::formatName; }

        void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& file) override
        {
            if (file == TestPlugins::effectID)
                results.add (new juce::PluginDescription (TestPlugins::getEffectDescription()));
        }

        bool fileMightContainThisPluginType (const juce::String& file) override { return file == TestPlugins::effectID; }
        juce::String getNameOfPluginFromIdentifier (const juce::String& file) override { return file; }
        bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
        bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
        bool canScanForPlugins() const override { return false; }
        bool isTrivialToScan() const override { return true; }

        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return { TestPlugins::effectID }; }

        juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
        bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

    protected:
        void createPluginInstance (const juce::PluginDescription& description, double, int, PluginCreationCallback callback) override
        {
            if (description.fileOrIdentifier == TestPlugins::effectID)
                callback (TestPlugins::createEffect(), {});
            else
                callback (nullptr, "No such test plug-in.");
        }
    };
} // namespace

juce::PluginDescription TestPlugins::getEffectDescription()
{
    juce::PluginDescription description;
    description.name = "Test Effect";
    description.descriptiveName = description.name;
    description.pluginFormatName = formatName;
    description.category = "Effect";
    description.manufacturerName = "Phazite Tests";
    description.version = "1.0";
    description.fileOrIdentifier = effectID;
    description.uniqueId = 0x7e57e5;
    description.isInstrument = false;
    description.numInputChannels = 2;
    description.numOutputChannels = 2;
    return description;
}

std::unique_ptr<juce::AudioPluginInstance> TestPlugins::createEffect()
{
    return std::make_unique<TestEffect>();
}

std::unique_ptr<juce::AudioPluginFormat> TestPlugins::createFormat()
{
    return std::make_unique<TestFormat>();
}
} // namespace phazite::test
