// SPDX-License-Identifier: AGPL-3.0-or-later
// Sidechains (model/mixer/internal/Sidechain.h): which tracks a device can listen to, picking one as an undo step, forgetting a
// deleted one, and the source's sound reaching the device as the song plays.
#include "TestDevices.h"
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
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
