// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LoudnessMeterPlugin.h"
#include "model/mixer/Mixer.h"

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

TEST_CASE ("A track's instrument plays its notes, and its effects come after it")
{
    Mix mix;
    CHECK (phazite::getInstrument (*mix.a) == nullptr);
    CHECK_FALSE (phazite::needsInstrument (*mix.a)); // no notes yet

    // Notes without an instrument make no sound.
    mix.a->insertMIDIClip ({ 0_tp, 2_tp }, nullptr);
    CHECK (phazite::needsInstrument (*mix.a));

    // An effect doesn't play notes.
    auto* reverb = phazite::insertDevice (*mix.a, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverb != nullptr);
    CHECK (phazite::getInstrument (*mix.a) == nullptr);
    CHECK (phazite::needsInstrument (*mix.a));
    CHECK (phazite::getEffects (*mix.a) == juce::Array<te::Plugin*> { reverb });

    auto* synth = phazite::setInstrument (*mix.a, mix.create (te::FourOscPlugin::xmlTypeName));
    REQUIRE (synth != nullptr);
    CHECK (phazite::getInstrument (*mix.a) == synth);
    CHECK_FALSE (phazite::needsInstrument (*mix.a));
    CHECK (phazite::getEffects (*mix.a) == juce::Array<te::Plugin*> { reverb });

    // Another instrument takes the first one's place; an effect isn't one.
    auto* other = phazite::setInstrument (*mix.a, mix.create (te::FourOscPlugin::xmlTypeName));
    REQUIRE (other != nullptr);
    CHECK (phazite::getDevices (*mix.a) == juce::Array<te::Plugin*> { other, reverb });
    CHECK (phazite::setInstrument (*mix.a, mix.create (te::DelayPlugin::xmlTypeName)) == nullptr);
    CHECK (phazite::getInstrument (*mix.a) == other);
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

    // Returns are solo-safe: soloing a track leaves them audible, so its sends to them are heard.
    mix.a->setSolo (true);
    CHECK_FALSE (phazite::isSilencedBySolo (*reverbReturn));
    CHECK (phazite::isSilencedBySolo (*mix.b));
    mix.a->setSolo (false);
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

TEST_CASE ("The device list offers Phazite's own devices, and two of Tracktion's until Phazite has its own")
{
    // Phazite's instruments, Tracktion's 4OSC synth, which the starter project uses, Phazite's effects, and Tracktion's
    // reverb, which follows the real-time rules (docs/realtime-audit.md, finding 16). Roadmap step 1.6 replaces both.
    Mix mix;
    juce::StringArray builtIn;

    for (const auto& type : phazite::getDeviceTypes (phazite::test::getEngine()))
    {
        if (te::PluginManager::isBuiltInPlugin (type.description))
        {
            builtIn.add (type.description.fileOrIdentifier);
            CHECK (phazite::createDevice (*mix.edit, type) != nullptr);

            if (type.description.fileOrIdentifier.startsWith ("phazite"))
                CHECK (type.description.manufacturerName == juce::String ("Phazite"));
        }
    }

    CHECK (builtIn
           == juce::StringArray { te::FourOscPlugin::xmlTypeName, "phaziteEq", "phaziteCompressor", te::ReverbPlugin::xmlTypeName });
}

TEST_CASE ("Phazite's effects have a mix of their own, and name their key controls")
{
    Mix mix;
    auto* eq = phazite::insertDevice (*mix.a, mix.create ("phaziteEq"));
    REQUIRE (eq != nullptr);

    REQUIRE (phazite::hasMix (*eq));
    CHECK (phazite::getMix (*eq) == doctest::Approx (1.0f)); // all effect to start with
    phazite::setMix (*eq, 0.3f);
    CHECK (phazite::getMix (*eq) == doctest::Approx (0.3f));
    CHECK (eq->getAutomatableParameterByID ("mix")->getCurrentValue() == doctest::Approx (0.3f));
    REQUIRE (phazite::getMixParameters (*eq).size() == 1);
    CHECK (phazite::getMixParameters (*eq)[0]->paramID == juce::String ("mix"));

    // The EQ's slot shows its low and high shelves' gains.
    CHECK (phazite::getKeyControls (*eq, 2) == juce::StringArray { "band2Gain", "band7Gain" });
    CHECK (phazite::getKeyControls (*eq, 1) == juce::StringArray { "band2Gain" });
}

TEST_CASE ("A folder's bus switch makes it a bus that stays one, and plain again while it has no effects")
{
    Mix mix;
    auto* folder = phazite::groupIntoFolder ({ mix.a, mix.b }, nullptr);
    REQUIRE (folder != nullptr);

    phazite::setMixedAsBus (*folder, true);
    CHECK (phazite::isBus (*folder));

    // Made a bus with the switch, it stays one when its last effect goes.
    auto* compressor = phazite::insertDevice (*folder, mix.create (te::CompressorPlugin::xmlTypeName));
    REQUIRE (compressor != nullptr);
    phazite::removeDevice (*compressor);
    CHECK (phazite::isBus (*folder));

    // With an effect, it stays a bus: making it plain would take the effect away.
    auto* reverb = phazite::insertDevice (*folder, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverb != nullptr);
    phazite::setMixedAsBus (*folder, false);
    CHECK (phazite::isBus (*folder));

    phazite::removeDevice (*reverb);
    phazite::setMixedAsBus (*folder, false);
    CHECK_FALSE (phazite::isBus (*folder));
    REQUIRE (folder->getVCAPlugin() != nullptr);

    // A bus that became one by getting an effect turns plain again without it, as before.
    auto* delay = phazite::insertDevice (*folder, mix.create (te::DelayPlugin::xmlTypeName));
    REQUIRE (delay != nullptr);
    CHECK (phazite::isBus (*folder));
    phazite::removeDevice (*delay);
    CHECK_FALSE (phazite::isBus (*folder));
}

TEST_CASE ("A return with an effect comes in one step, named after the effect, with a send to it")
{
    Mix mix;
    auto* reverbReturn = phazite::addReturnWithEffect (*mix.a, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverbReturn != nullptr);
    CHECK (reverbReturn->getName() == juce::String ("Reverb"));
    CHECK (reverbReturn->isSoloIsolate (false));

    // The reverb gives only its effect: the dry sound reaches the master through the track itself.
    auto* reverb = dynamic_cast<te::ReverbPlugin*> (phazite::getEffects (*reverbReturn).getFirst());
    REQUIRE (reverb != nullptr);
    CHECK (reverb->dryParam->getCurrentValue() == doctest::Approx (0.0f));

    const auto sends = phazite::getSends (*mix.a);
    REQUIRE (sends.size() == 1);
    CHECK (sends[0]->getBusNumber() == phazite::getReturnBus (*reverbReturn));
    CHECK_FALSE (phazite::isPreFader (*sends[0]));

    // Another one gets a name of its own.
    auto* second = phazite::addReturnWithEffect (*mix.b, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (second != nullptr);
    CHECK (second->getName() == juce::String ("Reverb 2"));

    // Instruments don't go on returns.
    CHECK (phazite::addReturnWithEffect (*mix.a, mix.create (te::FourOscPlugin::xmlTypeName)) == nullptr);
}

TEST_CASE ("An effect's mix crossfades between the sound that comes in and the effect")
{
    Mix mix;
    auto* reverb = phazite::insertDevice (*mix.a, mix.create (te::ReverbPlugin::xmlTypeName));
    REQUIRE (reverb != nullptr);
    REQUIRE (phazite::hasMix (*reverb));
    auto& reverbPlugin = dynamic_cast<te::ReverbPlugin&> (*reverb);

    // Tracktion's reverb passes both at full level to start with: an even mix.
    CHECK (phazite::getMix (*reverb) == doctest::Approx (0.5f));

    // It scales its wet level by 3 and its dry level by 2.
    phazite::setMix (*reverb, 0.25f);
    CHECK (reverbPlugin.wetParam->getCurrentValue() == doctest::Approx (0.25f / 3.0f));
    CHECK (reverbPlugin.dryParam->getCurrentValue() == doctest::Approx (0.75f * 0.5f));
    CHECK (phazite::getMix (*reverb) == doctest::Approx (0.25f));

    phazite::setMix (*reverb, 1.0f);
    CHECK (reverbPlugin.dryParam->getCurrentValue() == doctest::Approx (0.0f));
    CHECK (phazite::getMixParameters (*reverb).size() == 2);

    // The mix sets the dry and wet levels, so the slot's key controls are others.
    for (const auto& id : phazite::getKeyControls (*reverb, 2))
        CHECK ((id != "dry level" && id != "wet level"));

    // An instrument has no mix.
    auto* synth = phazite::setInstrument (*mix.a, mix.create (te::FourOscPlugin::xmlTypeName));
    REQUIRE (synth != nullptr);
    CHECK_FALSE (phazite::hasMix (*synth));
    CHECK (phazite::getMix (*synth) == doctest::Approx (1.0f));
}

TEST_CASE ("New return tracks are numbered in turn")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::SelectionManager selection (phazite::test::getEngine());
    selection.edit = edit.get();

    auto* first = phazite::addNewReturnTrack (*edit, &selection);
    REQUIRE (first != nullptr);
    CHECK (first->getName() == juce::String ("Return 1"));
    CHECK (selection.isSelected (first));

    // Without a selection manager, the selection stays as it was.
    auto* second = phazite::addNewReturnTrack (*edit, nullptr);
    REQUIRE (second != nullptr);
    CHECK (second->getName() == juce::String ("Return 2"));
    CHECK (selection.isSelected (first));
    CHECK (phazite::getReturnBus (*second) != phazite::getReturnBus (*first));
    selection.deselectAll();
}
