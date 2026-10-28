// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestPlugin.h"

namespace phazite::test
{
namespace
{
    // A parameter as a plug-in format hosts one (a VST3's): with the plug-in's own ID for it, and no ID in JUCE, so that
    // Tracktion knows it by its place in the list.
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

    // The plug-in itself, as a plug-in format would make it, in one of its versions (TestPlugin::setVersion()).
    class TestEffect final : public juce::AudioPluginInstance
    {
    public:
        explicit TestEffect (int version)
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        {
            const auto add = [this] (const char* ownID, const char* parameterName, float defaultValue)
            {
                auto parameter = std::make_unique<StandInParameter> (ownID, parameterName, defaultValue);
                auto* added = parameter.get();
                addHostedParameter (std::move (parameter));
                return added;
            };

            if (version == 1)
            {
                gain = add (TestPlugin::gainOwnID, "Gain", 1.0f);
                tone = add (TestPlugin::toneOwnID, "Tone", 0.5f);
            }
            else
            {
                tone = add (TestPlugin::toneOwnID, "Tone", 0.5f);

                if (version == 2)
                {
                    gain = add (TestPlugin::gainOwnID, "Gain", 1.0f);
                    add ("2", "Drive", 0.0f);
                }
            }
        }

        void fillInPluginDescription (juce::PluginDescription& description) const override { description = TestPlugin::getDescription(); }
        const juce::String getName() const override { return TestPlugin::getDescription().name; }

        void prepareToPlay (double, int) override {}
        void releaseResources() override {}

        using juce::AudioProcessor::processBlock;

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            if (gain != nullptr)
                buffer.applyGain (gain->getValue());
        }

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
            out.writeFloat (gain != nullptr ? gain->getValue() : 1.0f);
            out.writeFloat (tone->getValue());
        }

        void setStateInformation (const void* data, int size) override
        {
            if (size < 8)
                return;

            juce::MemoryInputStream in (data, (size_t) size, false);
            const auto gainValue = in.readFloat();

            if (gain != nullptr)
                gain->setValue (gainValue);

            tone->setValue (in.readFloat());
        }

    private:
        StandInParameter* gain = nullptr; // none in version 3
        StandInParameter* tone = nullptr;
    };

    // The plug-in's Gain, wherever its version has it.
    juce::AudioProcessorParameter& getGainParameter (const te::ExternalPlugin& plugin)
    {
        auto* instance = plugin.getAudioPluginInstance();
        REQUIRE (instance != nullptr);
        juce::AudioProcessorParameter* found = nullptr;

        for (auto* parameter : instance->getParameters())
            if (const auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (parameter))
                if (hosted->getParameterID() == TestPlugin::gainOwnID)
                    found = parameter;

        REQUIRE (found != nullptr);
        return *found;
    }
} // namespace

TestPlugin::TestPlugin()
{
    auto& plugins = getEngine().getPluginManager();
    previous = plugins.createPluginInstance;
    plugins.createPluginInstance = [this, fallback = previous] (const juce::PluginDescription& description, double sampleRate,
                                                                int blockSize,
                                                                juce::String& error) -> std::unique_ptr<juce::AudioPluginInstance>
    {
        if (description.pluginFormatName == getDescription().pluginFormatName && description.name == getDescription().name)
            return std::make_unique<TestEffect> (version);

        return fallback != nullptr ? fallback (description, sampleRate, blockSize, error) : nullptr;
    };

    plugins.knownPluginList.addType (getDescription());
}

TestPlugin::~TestPlugin()
{
    auto& plugins = getEngine().getPluginManager();
    plugins.knownPluginList.removeType (getDescription());
    plugins.createPluginInstance = previous;
}

void TestPlugin::setVersion (int newVersion)
{
    version = newVersion;
}

juce::PluginDescription TestPlugin::getDescription()
{
    juce::PluginDescription description;
    description.name = "Test Effect";
    description.descriptiveName = description.name;
    description.pluginFormatName = "Test";
    description.category = "Effect";
    description.manufacturerName = "Phazite Tests";
    description.version = "1.0";
    description.fileOrIdentifier = "PhaziteTestEffect";
    description.uniqueId = 0x7e57e5;
    description.isInstrument = false;
    description.numInputChannels = 2;
    description.numOutputChannels = 2;
    return description;
}

float TestPlugin::getGain (const te::ExternalPlugin& plugin)
{
    return getGainParameter (plugin).getValue();
}

void TestPlugin::setGain (te::ExternalPlugin& plugin, float gain)
{
    getGainParameter (plugin).setValueNotifyingHost (gain);
}

te::ExternalPlugin::Ptr TestPlugin::create (te::Edit& edit)
{
    te::ExternalPlugin::Ptr plugin =
        dynamic_cast<te::ExternalPlugin*> (edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, getDescription()).get());
    REQUIRE (plugin != nullptr);
    plugin->initialiseFully();
    REQUIRE (plugin->getAudioPluginInstance() != nullptr);
    return plugin;
}
} // namespace phazite::test
