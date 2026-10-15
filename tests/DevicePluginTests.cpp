// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's thin Tracktion plug-in (src/model/devices/DevicePlugin.h), with two devices made for these tests: an
// effect (a gain) and an instrument that plays the velocity of the note it holds.
#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "model/devices/DevicePlugin.h"

#include <vector>

using namespace tracktion::literals;

namespace
{
namespace devices = phazite::devices;
constexpr double testRate = 48000.0;
constexpr int testBlock = 512;

constexpr std::string_view modes[] = { "Normal", "Inverted" };

constexpr devices::ParameterInfo gainParameters[] = {
    { .id = "gain", .name = "Gain", .minimum = -60.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = devices::Unit::decibels },
    { .id = "mode",
      .name = "Mode",
      .minimum = 0.0f,
      .maximum = 1.0f,
      .defaultValue = 0.0f,
      .unit = devices::Unit::choice,
      .choices = modes },
};

// Scales the sound by its gain, upside down when inverted; 64 samples of latency and a 1.5 s tail, as if it had them.
struct TestGain final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestGain";
    static constexpr devices::DeviceInfo info { .typeID = typeID,
                                                .name = "Test Gain",
                                                .kind = devices::DeviceKind::effect,
                                                .parameters = gainParameters };

    void prepare (double, int) override { prepared = true; }
    void reset() override {}

    void process (const devices::ProcessContext& block) override
    {
        const auto gain = juce::Decibels::decibelsToGain (block.parameters[0]) * (block.parameters[1] > 0.5f ? -1.0f : 1.0f);

        for (int channel = 0; channel < block.numChannels; ++channel)
            for (int i = 0; i < block.numSamples; ++i)
                block.channels[channel][i] *= gain;

        lastValues.assign (block.parameters.begin(), block.parameters.end());
        lastNumChannels = block.numChannels;
    }

    int getLatencySamples() const override { return 64; }
    double getTailSeconds() const override { return 1.5; }

    bool prepared = false;
    std::vector<float> lastValues;
    int lastNumChannels = 0;
};

constexpr devices::ParameterInfo toneParameters[] = {
    { .id = "level", .name = "Level", .minimum = 0.0f, .maximum = 1.0f, .defaultValue = 1.0f, .unit = devices::Unit::percent },
};

// Plays the velocity of the note it holds, and keeps every event it was given.
struct TestTone final : devices::Processor
{
    static constexpr const char* typeID = "phaziteTestTone";
    static constexpr devices::DeviceInfo info { .typeID = typeID,
                                                .name = "Test Tone",
                                                .kind = devices::DeviceKind::instrument,
                                                .parameters = toneParameters };

    void prepare (double, int) override {}
    void reset() override { held = 0.0f; }

    void process (const devices::ProcessContext& block) override
    {
        received.insert (received.end(), block.events.begin(), block.events.end());
        size_t next = 0;

        for (int i = 0; i < block.numSamples; ++i)
        {
            for (; next < block.events.size() && block.events[next].sample <= i; ++next)
            {
                const auto& event = block.events[next];

                if (event.type == devices::NoteEvent::Type::noteOn)
                    held = event.value;
                else if (event.type == devices::NoteEvent::Type::noteOff || event.type == devices::NoteEvent::Type::allNotesOff)
                    held = 0.0f;
            }

            block.channels[0][i] += held;
            block.channels[1][i] += held;
        }
    }

    float held = 0.0f;
    std::vector<devices::NoteEvent> received;
};

void registerTestDevices()
{
    [[maybe_unused]] static const bool registered = []
    {
        auto& plugins = phazite::test::getEngine().getPluginManager();
        plugins.createBuiltInType<phazite::DevicePluginOf<TestGain>>();
        plugins.createBuiltInType<phazite::DevicePluginOf<TestTone>>();
        return true;
    }();
}

phazite::DevicePlugin& createDevice (te::Edit& edit, const char* type, te::Plugin::Ptr& keep)
{
    registerTestDevices();
    keep = edit.getPluginCache().createNewPlugin (type, {});
    auto* device = dynamic_cast<phazite::DevicePlugin*> (keep.get());
    REQUIRE (device != nullptr);
    return *device;
}

te::PluginRenderContext makeContext (juce::AudioBuffer<float>& buffer, te::MidiMessageArray* midi = nullptr)
{
    const auto length = te::TimeDuration::fromSamples (buffer.getNumSamples(), testRate);
    return { &buffer, 0, buffer.getNumSamples(), midi, 0.0, te::TimeRange (0_tp, length), true, false, false, false };
}
} // namespace

TEST_CASE ("A device's parameters are automatable, and stored in its state under their IDs")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    auto& device = createDevice (*edit, TestGain::typeID, keep);

    CHECK (device.getPluginType() == juce::String ("phaziteTestGain"));
    CHECK (device.getName() == juce::String ("Test Gain"));

    const auto parameters = device.getAutomatableParameters();
    REQUIRE (parameters.size() == 2);
    CHECK (parameters[0]->paramID == juce::String ("gain"));
    CHECK (parameters[0]->getParameterName() == juce::String ("Gain"));
    CHECK (parameters[0]->valueRange.start == doctest::Approx (-60.0f));
    CHECK (parameters[0]->valueRange.end == doctest::Approx (12.0f));
    CHECK (parameters[0]->getCurrentValue() == doctest::Approx (0.0f));
    CHECK (parameters[0]->getDefaultValue().value_or (99.0f) == doctest::Approx (0.0f));
    CHECK (parameters[1]->valueRange.interval == doctest::Approx (1.0f));
    CHECK (device.findParameter ("mode") == parameters[1]);
    CHECK (device.findParameter ("volume") == nullptr);

    parameters[0]->setParameter (-6.0f, juce::sendNotification);
    CHECK ((float) device.state["gain"] == doctest::Approx (-6.0f));

    // Restoring a state takes the values it has, and the defaults of those it hasn't.
    parameters[1]->setParameter (1.0f, juce::sendNotification);
    juce::ValueTree preset (te::IDs::PLUGIN);
    preset.setProperty ("gain", -12.0f, nullptr);
    device.restorePluginStateFromValueTree (preset);
    CHECK (parameters[0]->getCurrentValue() == doctest::Approx (-12.0f));
    CHECK (parameters[1]->getCurrentValue() == doctest::Approx (0.0f));

    // A device made from a saved state has the values saved.
    juce::ValueTree saved (te::IDs::PLUGIN);
    saved.setProperty (te::IDs::type, TestGain::typeID, nullptr);
    saved.setProperty ("gain", -3.0f, nullptr);
    saved.setProperty ("mode", 1.0f, nullptr);
    const auto loaded = edit->getPluginCache().createNewPlugin (saved);
    auto* loadedDevice = dynamic_cast<phazite::DevicePlugin*> (loaded.get());
    REQUIRE (loadedDevice != nullptr);
    CHECK (loadedDevice->findParameter ("gain")->getCurrentValue() == doctest::Approx (-3.0f));
    CHECK (loadedDevice->findParameter ("mode")->getCurrentValue() == doctest::Approx (1.0f));
}

TEST_CASE ("An effect gets its parameters' values and stereo audio, and reports its latency and tail")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    auto& device = createDevice (*edit, TestGain::typeID, keep);
    auto& gain = dynamic_cast<TestGain&> (device.getProcessor());

    device.baseClassInitialise ({ 0_tp, testRate, testBlock });
    CHECK (gain.prepared);
    CHECK (device.getLatencySeconds() == doctest::Approx (64.0 / testRate));
    CHECK (device.getTailLength() == doctest::Approx (1.5));
    CHECK_FALSE (device.noTail());
    CHECK (device.producesAudioWhenNoAudioInput()); // its tail goes on after the input stops
    CHECK_FALSE (device.isSynth());
    CHECK_FALSE (device.takesMidiInput());

    device.findParameter ("gain")->setParameter (-6.0206f, juce::sendNotification);
    juce::AudioBuffer<float> buffer (2, 256);

    for (int channel = 0; channel < 2; ++channel)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), 1.0f, 256);

    device.applyToBuffer (makeContext (buffer));
    CHECK (buffer.getSample (0, 10) == doctest::Approx (0.5f).epsilon (0.001));
    CHECK (buffer.getSample (1, 255) == doctest::Approx (0.5f).epsilon (0.001));
    REQUIRE (gain.lastValues.size() == 2);
    CHECK (gain.lastValues[0] == doctest::Approx (-6.0206f));

    // A choice reaches the DSP as a whole number, however it was set.
    device.findParameter ("mode")->setParameter (0.7f, juce::sendNotification);

    // A mono channel is played in stereo, a stretch at a time, and gets the average back.
    juce::AudioBuffer<float> mono (1, 1300);
    juce::FloatVectorOperations::fill (mono.getWritePointer (0), 1.0f, 1300);
    device.applyToBuffer (makeContext (mono));
    CHECK (gain.lastValues[1] == doctest::Approx (1.0f));
    CHECK (gain.lastNumChannels == 2);
    CHECK (mono.getSample (0, 0) == doctest::Approx (-0.5f).epsilon (0.001));
    CHECK (mono.getSample (0, 1299) == doctest::Approx (-0.5f).epsilon (0.001));

    device.baseClassDeinitialise();
}

TEST_CASE ("An instrument gets its notes at their samples, and plays from silence")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    auto& device = createDevice (*edit, TestTone::typeID, keep);
    auto& tone = dynamic_cast<TestTone&> (device.getProcessor());

    device.baseClassInitialise ({ 0_tp, testRate, testBlock });
    CHECK (device.isSynth());
    CHECK (device.takesMidiInput());
    CHECK (device.producesAudioWhenNoAudioInput());

    te::MidiMessageArray midi;
    midi.isAllNotesOff = true;
    midi.addMidiMessage (juce::MidiMessage::pitchWheel (1, 16383), 0.002, te::MPESourceID {});            // sample 96
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 64), 0.005, te::MPESourceID {}); // 240
    midi.addMidiMessage (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 0), 0.0099, te::MPESourceID {}); // 475: an off
    midi.addMidiMessage (juce::MidiMessage::controllerEvent (1, 74, 127), 0.0105, te::MPESourceID {});    // 504
    midi.addMidiMessage (juce::MidiMessage::programChange (1, 3), 0.0106, te::MPESourceID {});            // not used
    midi.addMidiMessage (juce::MidiMessage::noteOn (2, 62, (juce::uint8) 127), 0.02, te::MPESourceID {}); // past the end

    juce::AudioBuffer<float> buffer (2, testBlock);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), 0.3f, testBlock); // the track's audio: not heard
    juce::FloatVectorOperations::fill (buffer.getWritePointer (1), 0.3f, testBlock);
    device.applyToBuffer (makeContext (buffer, &midi));

    using Type = devices::NoteEvent::Type;
    REQUIRE (tone.received.size() == 6);
    CHECK (tone.received[0].type == Type::allNotesOff);
    CHECK (tone.received[0].sample == 0);
    CHECK (tone.received[1].type == Type::pitchBend);
    CHECK (tone.received[1].sample == 96);
    CHECK (tone.received[1].value == doctest::Approx (8191.0f / 8192.0f));
    CHECK (tone.received[2].type == Type::noteOn);
    CHECK (tone.received[2].sample == 240);
    CHECK (tone.received[2].number == 60);
    CHECK (tone.received[2].value == doctest::Approx (64.0f / 127.0f));
    CHECK (tone.received[3].type == Type::noteOff);
    CHECK (tone.received[3].sample == 475);
    CHECK (tone.received[4].type == Type::controller);
    CHECK (tone.received[4].number == 74);
    CHECK (tone.received[4].value == doctest::Approx (1.0f));
    CHECK (tone.received[5].type == Type::noteOn); // late: at the block's last sample
    CHECK (tone.received[5].sample == testBlock - 1);

    CHECK (buffer.getSample (0, 239) == doctest::Approx (0.0f));
    CHECK (buffer.getSample (0, 300) == doctest::Approx (64.0f / 127.0f));
    CHECK (buffer.getSample (1, 480) == doctest::Approx (0.0f));

    device.baseClassDeinitialise();
}
