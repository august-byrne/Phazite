// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's thin Tracktion plug-in (src/model/devices/internal/DevicePlugin.h), with the devices made for tests
// (TestDevices.h): an effect (a gain) and an instrument that plays the velocity of the note it holds; and Phazite's own
// devices keeping their parameters' values in a project.
#include "GoldenFiles.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "devices/core/Processor.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"

#include <map>

using namespace tracktion::literals;

namespace
{
namespace devices = phazite::devices;
using phazite::test::TestGain;
using phazite::test::TestTone;

constexpr double testRate = 48000.0;
constexpr int testBlock = 512;

phazite::DevicePlugin& createDevice (te::Edit& edit, const char* type, te::Plugin::Ptr& keep)
{
    return phazite::test::createTestDevice (edit, type, keep);
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

TEST_CASE ("An effect's mix blends its sound with what came in, lined up with its latency")
{
    using phazite::test::TestDelay;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::Plugin::Ptr keep;
    auto& device = createDevice (*edit, TestDelay::typeID, keep);
    device.baseClassInitialise ({ 0_tp, testRate, testBlock });

    const auto play = [&device] (float value, float mix, int length)
    {
        device.findParameter ("mix")->setParameter (mix, juce::sendNotification);
        juce::AudioBuffer<float> buffer (2, length);

        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), value, length);

        device.applyToBuffer (makeContext (buffer));
        return buffer;
    };

    // At 100 %, only the effect: what came in, upside down.
    CHECK (play (1.0f, 1.0f, 512).getSample (0, 511) == doctest::Approx (-1.0f));

    // At 0 %, only what came in; half way, the two cancel. (Each first block ramps from the last mix.)
    play (1.0f, 0.0f, 512);
    CHECK (play (1.0f, 0.0f, 512).getSample (1, 300) == doctest::Approx (1.0f));
    play (1.0f, 0.5f, 512);
    CHECK (std::abs (play (1.0f, 0.5f, 512).getSample (0, 300)) < 1.0e-6f);

    // The sound that came in is delayed as much as the effect's, so a click and its echo come out together.
    device.reset();
    play (0.0f, 0.5f, 512);
    juce::AudioBuffer<float> click (2, 256);
    click.clear();
    click.setSample (0, 0, 1.0f);
    click.setSample (1, 0, 1.0f);
    device.applyToBuffer (makeContext (click));
    CHECK (click.getSample (0, 0) == doctest::Approx (0.0f));
    CHECK (click.getSample (0, TestDelay::latency) == doctest::Approx (0.0f)); // half the click and half of it upside down

    device.baseClassDeinitialise();
}

TEST_CASE ("Each of Phazite's devices keeps its parameters' values through a save and a reopen, and its type with them")
{
    phazite::test::TemporaryFolder temp;
    juce::File file;
    std::map<juce::String, std::vector<float>> setTo; // each device's type, and the values its parameters were set to

    {
        juce::String error;
        const auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Devices", error);
        REQUIRE (project != nullptr);
        auto& edit = project->getEdit();
        const auto own = phazite::getOwnDevices();
        edit.ensureNumberOfAudioTracks ((int) own.size());

        for (size_t d = 0; d < own.size(); ++d)
        {
            const auto& info = *own[d];
            const juce::String type (info.typeID.data(), info.typeID.size());
            INFO (type);
            auto& track = *te::getAudioTracks (edit)[(int) d];
            const auto plugin = edit.getPluginCache().createNewPlugin (type, {});
            const auto* placed = info.kind == devices::DeviceKind::instrument ? phazite::setInstrument (track, plugin)
                                                                              : phazite::insertDevice (track, plugin);
            REQUIRE (placed != nullptr);
            auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
            REQUIRE (device != nullptr);
            auto& values = setTo[type];

            // Each one away from its default, to the other end of its range: the filters' types to Sample & Hold.
            for (const auto& parameter : info.parameters)
            {
                const auto value = parameter.defaultValue < parameter.maximum ? parameter.maximum : parameter.minimum;
                device->findParameter (parameter.id)->setParameter (value, juce::sendNotification);
                values.push_back (value);
            }

            CHECK (plugin->state[te::IDs::type].toString() == type);
        }

        phazite::test::settle();
        REQUIRE (project->save().wasOk());
        file = project->getFile();
    }

    juce::String error;
    const auto project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (project != nullptr);

    for (const auto& [type, values] : setTo)
    {
        INFO (type);
        phazite::DevicePlugin* device = nullptr;

        for (auto* plugin : te::getAllPlugins (project->getEdit(), false))
            if (plugin->getPluginType() == type)
                device = dynamic_cast<phazite::DevicePlugin*> (plugin);

        REQUIRE (device != nullptr);
        const auto parameters = device->getAutomatableParameters();
        REQUIRE (parameters.size() == (int) values.size());

        for (size_t i = 0; i < values.size(); ++i)
        {
            INFO (parameters[(int) i]->paramID);
            CHECK (parameters[(int) i]->getCurrentValue() == doctest::Approx (values[i]));
        }
    }
}

TEST_CASE ("No parameter of Phazite's devices is named as a property Tracktion keeps on every plug-in")
{
    // Its value would take that property's place: a changed value would change the plug-in's type, say, and the project
    // would lose the device when it next opened.
    const juce::Identifier tracktions[] = { te::IDs::type,          te::IDs::id,         te::IDs::enabled,           te::IDs::process,
                                            te::IDs::frozen,        te::IDs::customName, te::IDs::quickParamName,    te::IDs::windowX,
                                            te::IDs::windowY,       te::IDs::srcChan,    te::IDs::sidechainSourceID, te::IDs::dstChan,
                                            te::IDs::masterPluginID };

    for (const auto* info : phazite::getOwnDevices())
    {
        for (const auto& parameter : info->parameters)
        {
            const juce::String id (parameter.id.data(), parameter.id.size());
            INFO (juce::String (info->typeID.data(), info->typeID.size()) << ": " << id);

            for (const auto& name : tracktions)
                CHECK (id != name.toString());
        }
    }
}
