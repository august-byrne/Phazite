// SPDX-License-Identifier: AGPL-3.0-or-later
// Sidechains (model/mixer/internal/Sidechain.h): which tracks a device can listen to, and why not the others, picking one as an
// undo step (a compressor as it was made starting to duck in it), Duck Under, forgetting a deleted source, and the
// source's sound reaching the device as the song plays.
#include "TestDevices.h"
#include "TestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"

using namespace tracktion::literals;
using phazite::test::TestGain;
using phazite::test::TestSidechain;
using phazite::test::TestTone;

namespace
{
// An Edit with three tracks: Kick, Bass and Keys.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (3);
        const auto tracks = te::getAudioTracks (*edit);
        kick = tracks[0];
        bass = tracks[1];
        keys = tracks[2];
        kick->setName ("Kick");
        bass->setName ("Bass");
        keys->setName ("Keys");
    }

    // Adds a test device of that type to a track's chain.
    te::Plugin& add (te::Track& track, const char* type)
    {
        te::Plugin::Ptr device;
        phazite::test::createTestDevice (*edit, type, device);
        REQUIRE (phazite::insertDevice (track, device) != nullptr);
        return *device;
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::AudioTrack* kick = nullptr;
    te::AudioTrack* bass = nullptr;
    te::AudioTrack* keys = nullptr;
};

// Renders the song's first two seconds, as they sound with its tracks muted or not, and returns their peak level.
// (Tracktion's simplest render unmutes every track while it renders.)
float renderPeak (te::Edit& edit)
{
    juce::TemporaryFile wav (".wav");
    te::Renderer::Parameters params (edit);
    params.destFile = wav.getFile();
    params.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
    params.bitDepth = 32;
    params.sampleRateForAudio = 44100.0;
    params.blockSizeForAudio = 512;
    params.time = te::TimeRange (0_tp, 2_tp);
    params.tracksToDo = te::toBitSet (te::getAllTracks (edit));
    params.useMasterPlugins = true;
    params.canRenderInMono = false;
    params.ditheringEnabled = false;
    REQUIRE (te::Renderer::renderToFile ("Sidechain test", params) == params.destFile);

    const std::unique_ptr<juce::AudioFormatReader> reader (
        edit.engine.getAudioFileFormatManager().readFormatManager.createReaderFor (wav.getFile()));
    REQUIRE (reader != nullptr);

    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    return audio.getMagnitude (0, audio.getNumSamples());
}
} // namespace

TEST_CASE ("A device's sidechain can listen to any track whose sound doesn't come from the device's own")
{
    Song song;
    auto& ducker = song.add (*song.bass, TestSidechain::typeID);
    CHECK (phazite::hasSidechain (ducker));
    CHECK_FALSE (phazite::hasSidechain (song.add (*song.keys, TestGain::typeID)));
    CHECK (phazite::getSidechainSources (ducker) == juce::Array<te::AudioTrack*> { song.kick, song.keys });

    // A return the bass sends to, and a track with a device that listens to the bass, would make loops.
    auto* reverbReturn = phazite::addNewReturnTrack (*song.edit, nullptr);
    REQUIRE (reverbReturn != nullptr);
    REQUIRE (phazite::addSend (*song.bass, phazite::getReturnBus (*reverbReturn), false) != nullptr);
    phazite::setSidechainSource (song.add (*song.keys, TestSidechain::typeID), song.bass);
    CHECK (phazite::getSidechainSources (ducker) == juce::Array<te::AudioTrack*> { song.kick });

    // A device on a bus can listen to the tracks inside it, whose sound doesn't come from the bus, but the bass can't
    // listen to the bus's sound, which comes from the bass.
    auto* folder = phazite::groupIntoFolder ({ song.bass }, nullptr);
    REQUIRE (folder != nullptr);
    phazite::setMixedAsBus (*folder, true);
    auto& onBus = song.add (*folder, TestSidechain::typeID);
    CHECK (phazite::getSidechainSources (onBus) == juce::Array<te::AudioTrack*> { song.kick, song.bass, song.keys, reverbReturn });
}

TEST_CASE ("Picking a sidechain's source is one undo step, and deleting the source forgets it")
{
    Song song;
    auto& ducker = song.add (*song.bass, TestSidechain::typeID);
    auto& undoManager = song.edit->getUndoManager();
    CHECK (phazite::getSidechainSource (ducker) == nullptr);

    phazite::setSidechainSource (ducker, song.kick);
    CHECK (phazite::getSidechainSource (ducker) == song.kick);
    CHECK (undoManager.getUndoDescription() == juce::String ("Sidechain from Kick"));
    CHECK (ducker.getNumWires() == 4); // the kick's left and right to the sidechain's inputs

    // Its own track would make a loop, so nothing changes.
    phazite::setSidechainSource (ducker, song.bass);
    CHECK (phazite::getSidechainSource (ducker) == song.kick);

    phazite::setSidechainSource (ducker, nullptr);
    CHECK (phazite::getSidechainSource (ducker) == nullptr);
    CHECK (undoManager.getUndoDescription() == juce::String ("Turn Sidechain Off"));
    phazite::undo (*song.edit);
    CHECK (phazite::getSidechainSource (ducker) == song.kick);

    // Deleting the kick turns the sidechain off in the same step; undoing it brings both back.
    {
        const phazite::UndoStep step (*song.edit, "Delete Kick");
        phazite::deleteTracks ({ song.kick }, nullptr);
    }

    CHECK (phazite::getSidechainSource (ducker) == nullptr);
    CHECK_FALSE (ducker.getSidechainSourceID().isValid());

    phazite::undo (*song.edit);
    auto* restored = phazite::getSidechainSource (ducker);
    REQUIRE (restored != nullptr);
    CHECK (restored->getName() == juce::String ("Kick"));
}

TEST_CASE ("A sidechain's source reaches the device as the song plays, muted or not")
{
    Song song;

    // The kick plays a steady level, and the bass plays what its sidechain hears.
    te::Plugin::Ptr tone;
    phazite::test::createTestDevice (*song.edit, TestTone::typeID, tone);
    REQUIRE (phazite::setInstrument (*song.kick, tone) != nullptr);
    auto clip = song.kick->insertMIDIClip ("Notes", song.edit->tempoSequence.toTime ({ 0_bp, 4_bp }), nullptr);
    REQUIRE (clip != nullptr);
    clip->getSequence().addNote (60, 0_bp, 4_bd, 64, 0, nullptr);
    auto& ducker = song.add (*song.bass, TestSidechain::typeID);

    const auto kickAlone = renderPeak (*song.edit); // the bass's sidechain hears nothing yet
    CHECK (kickAlone > 0.1f);

    song.kick->setMute (true);
    CHECK (renderPeak (*song.edit) < 1.0e-4f);

    // A muted kick still feeds the sidechain, the way a "ghost" kick ducks a bass.
    phazite::setSidechainSource (ducker, song.kick);
    CHECK (renderPeak (*song.edit) > 0.5f * kickAlone);
}

TEST_CASE ("A sidechain's choices name every track but the device's own, in their colours, and why one would make a loop")
{
    Song song;
    auto& ducker = song.add (*song.bass, TestSidechain::typeID);
    phazite::setSidechainSource (song.add (*song.keys, TestSidechain::typeID), song.bass); // the keys' sound comes from the bass

    const auto choices = phazite::describeSidechainChoices (ducker);
    REQUIRE (choices.size() == 2);
    CHECK (choices[0].name == juce::String ("Kick"));
    CHECK (choices[0].whyNot.isEmpty());
    CHECK (choices[1].name == juce::String ("Keys"));
    CHECK (choices[1].whyNot == juce::String ("its sound comes from Bass"));
    CHECK (choices[0].colour == phazite::getTrackColour (*song.kick, 0));

    // As the mixer shows the device: what it listens to.
    CHECK_FALSE (phazite::describeSidechainSource (ducker).track.isValid());
    phazite::setSidechainSource (ducker, song.kick);
    CHECK (phazite::describeSidechainSource (ducker).name == juce::String ("Kick"));

    // A device with no sidechain has no choices.
    CHECK (phazite::describeSidechainChoices (song.add (*song.keys, TestGain::typeID)).empty());
}

TEST_CASE ("Phazite's compressor, as it was made, starts ducking when its sidechain is first chosen, in the same undo step")
{
    using phazite::devices::Compressor;
    Song song;
    const auto valueOf = [] (te::Plugin& device, const char* id) { return device.getAutomatableParameterByID (id)->getCurrentValue(); };
    const auto addCompressor = [&song] (te::Track& track) -> te::Plugin&
    { return *phazite::insertDevice (track, song.edit->getPluginCache().createNewPlugin (Compressor::typeID, {})); };

    // On the bass, from the kick: quick, slow to let go, deep; the key whole, as the kick plays no drum sampler.
    auto& compressor = addCompressor (*song.bass);
    song.edit->getUndoManager().clearUndoHistory();
    phazite::setSidechainSource (compressor, song.kick);
    CHECK (valueOf (compressor, "threshold") == doctest::Approx (-30.0f));
    CHECK (valueOf (compressor, "ratio") == doctest::Approx (8.0f));
    CHECK (valueOf (compressor, "attack") == doctest::Approx (1.0f));
    CHECK (valueOf (compressor, "release") == doctest::Approx (150.0f));
    CHECK (valueOf (compressor, "keyHighCut") == doctest::Approx (20000.0f));
    CHECK (song.edit->getUndoManager().getUndoDescription() == juce::String ("Sidechain from Kick"));

    // One step: undo puts back both the source and the settings.
    phazite::undo (*song.edit);
    CHECK (phazite::getSidechainSource (compressor) == nullptr);
    CHECK (valueOf (compressor, "threshold") == doctest::Approx (-18.0f));
    CHECK (valueOf (compressor, "attack") == doctest::Approx (10.0f));

    // A compressor already set its own way keeps its settings.
    compressor.getAutomatableParameterByID ("ratio")->setParameter (2.0f, juce::sendNotification);
    phazite::setSidechainSource (compressor, song.kick);
    CHECK (valueOf (compressor, "ratio") == doctest::Approx (2.0f));
    CHECK (valueOf (compressor, "threshold") == doctest::Approx (-18.0f));

    // A source that plays a drum sampler: only its lows, so that the kick ducks the keys and the hats don't.
    REQUIRE (phazite::setInstrument (*song.kick, song.edit->getPluginCache().createNewPlugin (phazite::devices::DrumSampler::typeID, {}))
             != nullptr);
    auto& onKeys = addCompressor (*song.keys);
    phazite::setSidechainSource (onKeys, song.kick);
    CHECK (valueOf (onKeys, "keyHighCut") == doctest::Approx (150.0f));
}

TEST_CASE ("Duck Under puts a compressor on a track, ducking under another, as one undo step")
{
    Song song;
    song.edit->getUndoManager().clearUndoHistory();
    auto* compressor = phazite::duckUnder (*song.bass, *song.kick);
    REQUIRE (compressor != nullptr);
    CHECK (compressor->getOwnerTrack() == song.bass);
    CHECK (phazite::getSidechainSource (*compressor) == song.kick);
    CHECK (compressor->getAutomatableParameterByID ("ratio")->getCurrentValue() == doctest::Approx (8.0f));
    CHECK (song.edit->getUndoManager().getUndoDescription() == juce::String ("Duck Bass Under Kick"));

    phazite::undo (*song.edit);
    CHECK (song.bass->pluginList.getPluginsOfType<phazite::DevicePlugin>().isEmpty());

    // Not under its own sound, nor under a track whose sound comes from it.
    CHECK (phazite::duckUnder (*song.bass, *song.bass) == nullptr);
    phazite::setSidechainSource (song.add (*song.keys, TestSidechain::typeID), song.bass);
    CHECK (phazite::duckUnder (*song.bass, *song.keys) == nullptr);
}
