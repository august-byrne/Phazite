// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestPlugin.h"

namespace phazite::test
{
namespace
{
    // The plug-in itself, as a plug-in format would make it.
    class TestEffect final : public juce::AudioPluginInstance
    {
    public:
        TestEffect()
            : juce::AudioPluginInstance (BusesProperties()
                                             .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
        {
            auto gainParameter = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { TestPlugin::gainID, 1 }, "Gain",
                                                                              juce::NormalisableRange<float> (0.0f, 1.0f), 1.0f);
            auto toneParameter = std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { TestPlugin::toneID, 1 }, "Tone",
                                                                              juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f);
            gain = gainParameter.get();
            tone = toneParameter.get();
            addHostedParameter (std::move (gainParameter));
            addHostedParameter (std::move (toneParameter));
        }

        void fillInPluginDescription (juce::PluginDescription& description) const override { description = TestPlugin::getDescription(); }
        const juce::String getName() const override { return TestPlugin::getDescription().name; }

        void prepareToPlay (double, int) override {}
        void releaseResources() override {}

        using juce::AudioProcessor::processBlock;
        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.applyGain (gain->get()); }

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

        // Its parameters' values, as a plug-in keeps its settings: in a block of its own.
        void getStateInformation (juce::MemoryBlock& data) override
        {
            juce::MemoryOutputStream out (data, false);
            out.writeFloat (gain->get());
            out.writeFloat (tone->get());
        }

        void setStateInformation (const void* data, int size) override
        {
            if (size < 8)
                return;

            juce::MemoryInputStream in (data, (size_t) size, false);
            *gain = in.readFloat();
            *tone = in.readFloat();
        }

    private:
        juce::AudioParameterFloat* gain = nullptr;
        juce::AudioParameterFloat* tone = nullptr;
    };
} // namespace

TestPlugin::TestPlugin()
{
    auto& plugins = getEngine().getPluginManager();
    previous = plugins.createPluginInstance;
    plugins.createPluginInstance = [fallback = previous] (const juce::PluginDescription& description, double sampleRate, int blockSize,
                                                          juce::String& error) -> std::unique_ptr<juce::AudioPluginInstance>
    {
        if (description.pluginFormatName == getDescription().pluginFormatName && description.name == getDescription().name)
            return std::make_unique<TestEffect>();

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
    auto* instance = plugin.getAudioPluginInstance();
    REQUIRE (instance != nullptr);
    return instance->getHostedParameter (0)->getValue();
}

void TestPlugin::setGain (te::ExternalPlugin& plugin, float gain)
{
    auto* instance = plugin.getAudioPluginInstance();
    REQUIRE (instance != nullptr);
    instance->getHostedParameter (0)->setValueNotifyingHost (gain);
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
