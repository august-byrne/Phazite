// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/LoudnessMeterPlugin.h"
#include "model/Mixer.h"
#include "model/Tracks.h"

using namespace tracktion::literals;

namespace
{
// An Edit with two tracks, A and B.
struct Mix
{
    Mix()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");
    }

    te::Plugin::Ptr create (const char* xmlType) const { return edit->getPluginCache().createNewPlugin (xmlType, {}); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};
} // namespace

TEST_CASE ("Devices go before the fader, instruments first")
{
    Mix mix;
    auto* reverb = phazite::insertDevice (*mix.a, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverb != nullptr);

    auto* synth = phazite::insertDevice (*mix.a, mix.create (te::FourOscPlugin::xmlTypeName), -1);
    REQUIRE (synth != nullptr);

    // The instrument went first though it was added at the end; the fader and meter aren't devices.
    const auto devices = phazite::getDevices (*mix.a);
    REQUIRE (devices.size() == 2);
    CHECK (devices[0] == synth);
    CHECK (devices[1] == reverb);
    CHECK (mix.a->pluginList.indexOf (reverb) < mix.a->pluginList.indexOf (mix.a->getVolumePlugin()));

    auto* delay = phazite::insertDevice (*mix.a, mix.create (te::DelayPlugin::xmlTypeName), 1);
    CHECK (phazite::getDevices (*mix.a)[1] == delay);

    phazite::moveDevice (*delay, 2);
    CHECK (phazite::getDevices (*mix.a)[2] == delay);
    phazite::moveDevice (*delay, 0);
    CHECK (phazite::getDevices (*mix.a)[0] == delay);
    CHECK (phazite::getDevices (*mix.a)[1] == synth);

    phazite::removeDevice (*reverb);
    CHECK (phazite::getDevices (*mix.a).size() == 2);

    // A device's slot shows its first parameters.
    CHECK (phazite::getKeyControls (*delay, 2).size() == 2);
}

TEST_CASE ("A folder becomes a bus with its first device, and plain again without one")
{
    Mix mix;
    auto* folder = phazite::groupIntoFolder ({ mix.a, mix.b }, nullptr);
    REQUIRE (folder != nullptr);
    CHECK_FALSE (phazite::isBus (*folder));
    REQUIRE (folder->getVCAPlugin() != nullptr);
    folder->getVCAPlugin()->setVolumeDb (-3.0f);
    CHECK (phazite::getLevelMeasurer (*folder) == nullptr); // a plain folder has no sound of its own

    auto* compressor = phazite::insertDevice (*folder, mix.create (te::CompressorPlugin::xmlTypeName));
    REQUIRE (compressor != nullptr);
    CHECK (phazite::isBus (*folder));
    CHECK (folder->getVCAPlugin() == nullptr);
    REQUIRE (folder->getVolumePlugin() != nullptr);
    CHECK (folder->getVolumePlugin()->getVolumeDb() == doctest::Approx (-3.0f));
    CHECK (phazite::getLevelMeasurer (*folder) != nullptr);
    CHECK (phazite::getDevices (*folder).size() == 1);
    CHECK (phazite::getVolumeDevice (*folder) == folder->getVolumePlugin());

    phazite::removeDevice (*compressor);
    CHECK_FALSE (phazite::isBus (*folder));
    REQUIRE (folder->getVCAPlugin() != nullptr);
    CHECK (folder->getVCAPlugin()->getVolumeDb() == doctest::Approx (-3.0f));
}

TEST_CASE ("Return tracks take a bus, and sends feed them from before or after the fader")
{
    Mix mix;
    auto* reverbReturn = phazite::addReturnTrack (*mix.edit, "Reverb", nullptr);
    REQUIRE (reverbReturn != nullptr);
    CHECK (reverbReturn->getName() == juce::String ("Reverb"));
    CHECK (phazite::getReturnBus (*reverbReturn) == 0);
    CHECK (mix.edit->getAuxBusName (0) == juce::String ("Reverb"));
    CHECK (phazite::getReturnBus (*mix.a) == -1);
    CHECK (phazite::getPlaylistTracks (*mix.edit).getLast() == reverbReturn); // a channel, like any track

    // The return's devices come after its input.
    auto* reverb = phazite::insertDevice (*reverbReturn, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverb != nullptr);
    CHECK (reverbReturn->pluginList.indexOf (reverb) == 1);

    auto* send = phazite::addSend (*mix.a, 0, false);
    REQUIRE (send != nullptr);
    CHECK (send->getBusNumber() == 0);
    CHECK (send->getGainDb() == doctest::Approx (-6.0f));
    CHECK_FALSE (phazite::isPreFader (*send));
    CHECK (phazite::getSends (*mix.a).size() == 1);

    phazite::setPreFader (*send, true);
    CHECK (phazite::isPreFader (*send));
    CHECK (mix.a->pluginList.indexOf (send) == mix.a->pluginList.indexOf (mix.a->getVolumePlugin()) - 1);
    phazite::setPreFader (*send, false);
    CHECK_FALSE (phazite::isPreFader (*send));

    auto* second = phazite::addReturnTrack (*mix.edit, "Delay", nullptr);
    REQUIRE (second != nullptr);
    CHECK (phazite::getReturnBus (*second) == 1);
    CHECK (phazite::findReturnTrack (*mix.edit, 1) == second);
    CHECK (phazite::getReturnTracks (*mix.edit).size() == 2);
}

TEST_CASE ("The master's loudness meter reads a stereo sine at its level")
{
    Mix mix;
    auto* meter = phazite::ensureLoudnessMeter (*mix.edit);
    REQUIRE (meter != nullptr);
    CHECK (phazite::ensureLoudnessMeter (*mix.edit) == meter); // only one
    CHECK (phazite::getMasterDevices (*mix.edit).isEmpty());   // it isn't a device

    // A device on the master goes before it.
    auto* eq = phazite::insertMasterDevice (*mix.edit, mix.create (te::EqualiserPlugin::xmlTypeName));
    REQUIRE (eq != nullptr);
    CHECK (mix.edit->getMasterPluginList().indexOf (eq) < mix.edit->getMasterPluginList().indexOf (meter));

    // A 1 kHz sine at -20 dBFS in both channels measures -20 LUFS (ITU-R BS.1770).
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 480;
    meter->initialise ({ 0_tp, sampleRate, blockSize });

    juce::AudioBuffer<float> buffer (2, blockSize);
    const auto amplitude = juce::Decibels::decibelsToGain (-20.0f);
    auto phase = 0.0;

    for (int block = 0; block < 100; ++block) // a second
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = amplitude * (float) std::sin (phase);
            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
            phase += juce::MathConstants<double>::twoPi * 1000.0 / sampleRate;
        }

        meter->applyToBuffer ({ &buffer, 0, blockSize, nullptr, 0.0, {}, true, false, false, false });
    }

    const auto readings = meter->getReadings();
    CHECK (readings.momentaryValid);
    CHECK (readings.momentaryLufs == doctest::Approx (-20.0f).epsilon (0.02));
    CHECK (readings.truePeakDb == doctest::Approx (-20.0f).epsilon (0.02));
}

TEST_CASE ("Until Phazite's own devices arrive, the device list offers two of Tracktion's")
{
    // The starter project's 4OSC synth, and the reverb, which follows the real-time rules (docs/realtime-audit.md,
    // finding 16). Roadmap step 1.6 replaces them.
    Mix mix;
    juce::StringArray builtIn;

    for (const auto& type : phazite::getDeviceTypes (phazite::test::getEngine()))
    {
        if (te::PluginManager::isBuiltInPlugin (type.description))
        {
            builtIn.add (type.description.fileOrIdentifier);
            CHECK (phazite::createDevice (*mix.edit, type) != nullptr);
        }
    }

    CHECK (builtIn == juce::StringArray { te::FourOscPlugin::xmlTypeName, te::ReverbPlugin::xmlTypeName });
}
