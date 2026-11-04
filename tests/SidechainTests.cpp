// SPDX-License-Identifier: AGPL-3.0-or-later
// Sidechains (model/mixer/internal/Sidechain.h): which tracks a device can listen to, and why not the others, picking one as an
// undo step (a compressor as it was made starting to duck in it), Duck Under, forgetting a deleted source, and the
// source's sound reaching the device as the song plays.
#include "TestClips.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"

using phazite::DeviceID;
using phazite::TrackID;
using phazite::test::TestGain;
using phazite::test::TestSidechain;
using phazite::test::TestTone;

namespace
{
// A song with three tracks: Kick, Bass and Keys.
struct Song
{
    Song()
    {
        const auto tracks = phazite::ensureTracks (*song, 3);
        kick = tracks[0];
        bass = tracks[1];
        keys = tracks[2];
        phazite::setTrackName (*song, kick, "Kick");
        phazite::setTrackName (*song, bass, "Bass");
        phazite::setTrackName (*song, keys, "Keys");
    }

    // Adds a test device of that type to a track's chain.
    DeviceID add (TrackID track, const char* type) { return phazite::test::addTestDevice (*song, track, type); }

    // Adds Phazite's compressor at the end of a track's chain.
    DeviceID addCompressor (TrackID track)
    {
        const auto id =
            phazite::insertDeviceState (*song, track, phazite::makeOwnDeviceState (*song, phazite::devices::Compressor::typeID));
        REQUIRE (id.isValid());
        return id;
    }

    phazite::Song::Ptr song = phazite::createSong();
    TrackID kick, bass, keys;
};

// Renders the song's first two seconds through Phazite's engine, as they sound with its tracks muted or not, and returns
// their peak level.
float renderPeak (phazite::Song& song)
{
    const auto audio = phazite::renderWithOwnEngine (song, 2 * 44100, 44100.0, 512);
    return audio.getMagnitude (0, audio.getNumSamples());
}
} // namespace

TEST_CASE ("A device's sidechain can listen to any track whose sound doesn't come from the device's own")
{
    Song song;
    auto& model = *song.song;
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    CHECK (phazite::hasSidechain (model, ducker));
    CHECK_FALSE (phazite::hasSidechain (model, song.add (song.keys, TestGain::typeID)));
    CHECK (phazite::getSidechainSources (model, ducker) == std::vector<TrackID> { song.kick, song.keys });

    // A return the bass sends to, and a track with a device that listens to the bass, would make loops.
    const auto reverbReturn = phazite::addNewReturnTrack (model, nullptr);
    REQUIRE (reverbReturn.isValid());
    REQUIRE (phazite::addSend (model, song.bass, reverbReturn, false).isValid());
    phazite::setSidechainSource (model, song.add (song.keys, TestSidechain::typeID), song.bass);
    CHECK (phazite::getSidechainSources (model, ducker) == std::vector<TrackID> { song.kick });

    // A device on a bus can listen to the tracks inside it, whose sound doesn't come from the bus, but the bass can't
    // listen to the bus's sound, which comes from the bass.
    const auto folder = phazite::groupIntoFolder (model, { song.bass }, nullptr);
    REQUIRE (folder.isValid());
    phazite::setMixedAsBus (model, folder, true);
    const auto onBus = song.add (folder, TestSidechain::typeID);
    CHECK (phazite::getSidechainSources (model, onBus) == std::vector<TrackID> { song.kick, song.bass, song.keys, reverbReturn });
}

TEST_CASE ("Picking a sidechain's source is one undo step, and deleting the source forgets it")
{
    namespace schema = phazite::schema;
    Song song;
    auto& model = *song.song;
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    auto& undoManager = model.getUndoManager();
    CHECK_FALSE (phazite::getSidechainSource (model, ducker).isValid());

    phazite::setSidechainSource (model, ducker, song.kick);
    CHECK (phazite::getSidechainSource (model, ducker) == song.kick);
    CHECK (undoManager.getUndoDescription() == juce::String ("Sidechain from Kick"));

    // Its own track would make a loop, so nothing changes.
    phazite::setSidechainSource (model, ducker, song.bass);
    CHECK (phazite::getSidechainSource (model, ducker) == song.kick);

    phazite::setSidechainSource (model, ducker, TrackID());
    CHECK_FALSE (phazite::getSidechainSource (model, ducker).isValid());
    CHECK (undoManager.getUndoDescription() == juce::String ("Turn Sidechain Off"));
    phazite::undo (model);
    CHECK (phazite::getSidechainSource (model, ducker) == song.kick);

    // Deleting the kick turns the sidechain off in the same step; undoing it brings both back.
    {
        const phazite::UndoStep step (model, "Delete Kick");
        phazite::deleteTracks (model, { song.kick }, nullptr);
    }

    CHECK_FALSE (phazite::getSidechainSource (model, ducker).isValid());
    CHECK_FALSE (phazite::findDeviceState (model, ducker).hasProperty (schema::deviceSidechain));

    phazite::undo (model);
    const auto restored = phazite::getSidechainSource (model, ducker);
    REQUIRE (restored.isValid());
    CHECK (phazite::getTrackName (model, restored) == juce::String ("Kick"));
}

TEST_CASE ("A device's sidechain is the song's, by the track's ID, which a song made from its state alone keeps")
{
    namespace schema = phazite::schema;
    Song song;
    auto& model = *song.song;
    const auto compressor = song.addCompressor (song.bass);
    phazite::setSidechainSource (model, compressor, song.kick);

    // The track it listens to, by its ID (Schema.h: deviceSidechain).
    const auto element = phazite::findDeviceState (model, compressor);
    CHECK ((juce::int64) element[schema::deviceSidechain] == (juce::int64) song.kick.getRaw());

    // Made again from the song's state alone, it listens to the kick.
    {
        const auto copy = phazite::makeSong (model.getState().createCopy());
        REQUIRE (copy != nullptr);
        CHECK (phazite::getSidechainSource (*copy, compressor) == song.kick);
    }

    // Turned off, the song's goes.
    phazite::setSidechainSource (model, compressor, TrackID());
    CHECK_FALSE (element.hasProperty (schema::deviceSidechain));
    CHECK_FALSE (phazite::getSidechainSource (model, compressor).isValid());
}

TEST_CASE ("A sidechain's source reaches the device as the song plays, muted or not")
{
    Song song;
    auto& model = *song.song;

    // The kick plays a steady level, and the bass plays what its sidechain hears.
    song.add (song.kick, TestTone::typeID);
    using phazite::test::at, phazite::test::lasting;
    const auto clip = phazite::test::addMidiClip (model, song.kick, { at (0.0), at (4.0) }, "Notes");
    REQUIRE (clip.isValid());
    phazite::test::addMidiNote (model, clip, 60, at (0.0), lasting (4.0), 64);
    const auto ducker = song.add (song.bass, TestSidechain::typeID);

    const auto kickAlone = renderPeak (model); // the bass's sidechain hears nothing yet
    CHECK (kickAlone > 0.1f);

    phazite::setTrackMuted (model, song.kick, true);
    CHECK (renderPeak (model) < 1.0e-4f);

    // A muted kick still feeds the sidechain, the way a "ghost" kick ducks a bass.
    phazite::setSidechainSource (model, ducker, song.kick);
    CHECK (renderPeak (model) > 0.5f * kickAlone);
}

TEST_CASE ("A sidechain's choices name every track but the device's own, in their colours, and why one would make a loop")
{
    Song song;
    auto& model = *song.song;
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    phazite::setSidechainSource (model, song.add (song.keys, TestSidechain::typeID), song.bass); // the keys' sound comes from the bass

    const auto choices = phazite::describeSidechainChoices (model, ducker);
    REQUIRE (choices.size() == 2);
    CHECK (choices[0].name == juce::String ("Kick"));
    CHECK (choices[0].whyNot.isEmpty());
    CHECK (choices[1].name == juce::String ("Keys"));
    CHECK (choices[1].whyNot == juce::String ("its sound comes from Bass"));
    CHECK (choices[0].colour == phazite::getTrackColour (model, song.kick, 0));

    // As the mixer shows the device: what it listens to.
    CHECK_FALSE (phazite::describeSidechainSource (model, ducker).track.isValid());
    phazite::setSidechainSource (model, ducker, song.kick);
    CHECK (phazite::describeSidechainSource (model, ducker).name == juce::String ("Kick"));

    // A device with no sidechain has no choices.
    CHECK (phazite::describeSidechainChoices (model, song.add (song.keys, TestGain::typeID)).empty());
}

TEST_CASE ("Phazite's compressor, as it was made, starts ducking when its sidechain is first chosen, in the same undo step")
{
    Song song;
    auto& model = *song.song;
    const auto valueOf = [&model] (DeviceID device, const char* id)
    { return phazite::getValueSetByHand (model, phazite::ParameterRef { device, id }); };

    // On the bass, from the kick: quick, slow to let go, deep; the key whole, as the kick plays no drum sampler.
    const auto compressor = song.addCompressor (song.bass);
    model.getUndoManager().clearUndoHistory();
    phazite::setSidechainSource (model, compressor, song.kick);
    const auto ducking = phazite::devices::curves::makeCompression (-30.0f, 8.0f); // its curve: 8:1 over -30 dB
    CHECK (valueOf (compressor, "points") == doctest::Approx ((float) ducking.size()));
    CHECK (valueOf (compressor, "point2X") == doctest::Approx (ducking[1][0]));
    CHECK (valueOf (compressor, "point3Y") == doctest::Approx (ducking[2][1]));
    CHECK (valueOf (compressor, "attack") == doctest::Approx (1.0f));
    CHECK (valueOf (compressor, "release") == doctest::Approx (150.0f));
    CHECK (valueOf (compressor, "keyHighCut") == doctest::Approx (20000.0f));
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Sidechain from Kick"));

    // One step: undo puts back both the source and the settings.
    phazite::undo (model);
    CHECK_FALSE (phazite::getSidechainSource (model, compressor).isValid());
    CHECK (valueOf (compressor, "point2X") == doctest::Approx (phazite::devices::curves::axisForDecibels (-18.0f)).epsilon (1.0e-5));
    CHECK (valueOf (compressor, "attack") == doctest::Approx (10.0f));

    // A compressor already set its own way keeps its settings.
    {
        const phazite::UndoStep step (model, "Change Attack");
        phazite::setValueSetByHand (model, phazite::ParameterRef { compressor, "attack" }, 2.0f);
    }

    phazite::setSidechainSource (model, compressor, song.kick);
    CHECK (valueOf (compressor, "attack") == doctest::Approx (2.0f));
    CHECK (valueOf (compressor, "point2X") == doctest::Approx (phazite::devices::curves::axisForDecibels (-18.0f)).epsilon (1.0e-5));

    // A source that plays a drum sampler: only its lows, so that the kick ducks the keys and the hats don't.
    REQUIRE (
        phazite::setInstrument (model, song.kick, phazite::makeOwnDeviceState (model, phazite::devices::DrumSampler::typeID)).isValid());
    const auto onKeys = song.addCompressor (song.keys);
    phazite::setSidechainSource (model, onKeys, song.kick);
    CHECK (valueOf (onKeys, "keyHighCut") == doctest::Approx (150.0f));
}

TEST_CASE ("Duck Under puts a compressor on a track, ducking under another, as one undo step")
{
    Song song;
    auto& model = *song.song;
    model.getUndoManager().clearUndoHistory();
    const auto compressor = phazite::duckUnder (model, song.bass, song.kick);
    REQUIRE (compressor.isValid());
    CHECK (phazite::getDeviceChannel (model, compressor) == song.bass);
    CHECK (phazite::getSidechainSource (model, compressor) == song.kick);
    CHECK (phazite::getValueSetByHand (model, phazite::ParameterRef { compressor, "point2X" })
           == doctest::Approx (phazite::devices::curves::axisForDecibels (-30.0f))); // ducking, 8:1 over -30 dB
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Duck Bass Under Kick"));

    phazite::undo (model);
    CHECK (phazite::getDevices (model, song.bass).empty());

    // Not under its own sound, nor under a track whose sound comes from it.
    CHECK_FALSE (phazite::duckUnder (model, song.bass, song.bass).isValid());
    phazite::setSidechainSource (model, song.add (song.keys, TestSidechain::typeID), song.bass);
    CHECK_FALSE (phazite::duckUnder (model, song.bass, song.keys).isValid());
}
