// SPDX-License-Identifier: AGPL-3.0-or-later
// Sidechains (model/mixer/internal/Sidechain.h): which tracks a device can listen to, and why not the others, picking one as an
// undo step (a compressor as it was made starting to duck in it, a vocoder playing it as its carrier), Duck Under,
// forgetting a deleted source, the source's sound reaching the device as the song plays, and a multiband's band ducking
// under it.
#include "EnginePlayback.h"
#include "TestClips.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Multiband.h"
#include "devices/vocoder/Vocoder.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

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

TEST_CASE ("A bus can be a sidechain's key, but not for a device on a track in it; a bus made a plain folder lets go of its listeners")
{
    namespace schema = phazite::schema;
    Song song;
    auto& model = *song.song;
    const auto drums = phazite::groupIntoFolder (model, { song.kick }, nullptr);
    REQUIRE (drums.isValid());
    phazite::setTrackName (model, drums, "Drums");
    phazite::setMixedAsBus (model, drums, true);

    // The bass can listen to the drums' bus, which mixes the kick; the kick can't, since the bus's sound comes from it.
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    CHECK (phazite::getSidechainSources (model, ducker) == std::vector<TrackID> { drums, song.kick, song.keys });
    const auto onKick = song.add (song.kick, TestSidechain::typeID);
    CHECK (phazite::getSidechainSources (model, onKick) == std::vector<TrackID> { song.bass, song.keys });

    const auto kickChoices = phazite::describeSidechainChoices (model, onKick);
    REQUIRE (kickChoices.size() == 3);
    CHECK (kickChoices[0].name == juce::String ("Drums"));
    CHECK (kickChoices[0].whyNot == juce::String ("its sound comes from Kick"));

    phazite::setSidechainSource (model, ducker, drums);
    CHECK (phazite::getSidechainSource (model, ducker) == drums);
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Sidechain from Drums"));
    CHECK (phazite::describeSidechainSource (model, ducker).name == juce::String ("Drums"));

    // A plain folder only scales its tracks, with no sound of its own to hear: made one, the bus's listeners hear the sound
    // passing through them again, in the same step, and it is offered greyed, saying why.
    phazite::setMixedAsBus (model, drums, false);
    CHECK_FALSE (phazite::getSidechainSource (model, ducker).isValid());
    CHECK_FALSE (phazite::findDeviceState (model, ducker).hasProperty (schema::deviceSidechain));
    CHECK (phazite::getSidechainSources (model, ducker) == std::vector<TrackID> { song.kick, song.keys });

    const auto choices = phazite::describeSidechainChoices (model, ducker);
    REQUIRE (choices.size() == 3);
    CHECK (choices[0].name == juce::String ("Drums"));
    CHECK (choices[0].whyNot == juce::String ("a plain folder: mix it as a bus to listen to it"));
    CHECK (phazite::duckUnder (model, song.keys, drums) == DeviceID());

    phazite::undo (model);
    CHECK (phazite::isBus (model, drums));
    CHECK (phazite::getSidechainSource (model, ducker) == drums);
}

TEST_CASE ("A change that would make a sidechain a loop turns it off in the same step: a track moved into the bus it listens to")
{
    Song song;
    auto& model = *song.song;
    const auto drums = phazite::groupIntoFolder (model, { song.kick }, nullptr);
    REQUIRE (drums.isValid());
    phazite::setMixedAsBus (model, drums, true);
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    phazite::setSidechainSource (model, ducker, drums);
    REQUIRE (phazite::getSidechainSource (model, ducker) == drums);

    // The bass into the drums' bus: the bus's sound would come from the bass, which listens to it.
    {
        const phazite::UndoStep step (model, "Move Track");
        phazite::moveTrackIntoFolder (model, song.bass, drums);
    }

    CHECK (phazite::getParentFolder (model, song.bass) == drums);
    CHECK_FALSE (phazite::getSidechainSource (model, ducker).isValid());
    phazite::undo (model);
    CHECK_FALSE (phazite::getParentFolder (model, song.bass).isValid());
    CHECK (phazite::getSidechainSource (model, ducker) == drums);

    // The bus ungrouped: what listened to it listens to none, its tracks staying.
    {
        const phazite::UndoStep step (model, "Ungroup");
        phazite::ungroupFolder (model, drums);
    }

    CHECK_FALSE (phazite::getSidechainSource (model, ducker).isValid());
    CHECK_FALSE (phazite::findDeviceState (model, ducker).hasProperty (phazite::schema::deviceSidechain));
    phazite::undo (model);
    CHECK (phazite::getSidechainSource (model, ducker) == drums);

    // A send from the keys to the return their sidechain listens to.
    const auto reverb = phazite::addNewReturnTrack (model, nullptr);
    REQUIRE (reverb.isValid());
    const auto onKeys = song.add (song.keys, TestSidechain::typeID);
    phazite::setSidechainSource (model, onKeys, reverb);
    REQUIRE (phazite::getSidechainSource (model, onKeys) == reverb);

    {
        const phazite::UndoStep step (model, "Add Send");
        REQUIRE (phazite::addSend (model, song.keys, reverb, false).isValid());
    }

    CHECK_FALSE (phazite::getSidechainSource (model, onKeys).isValid());
    CHECK (phazite::getSidechainSource (model, ducker) == drums); // no loop
    phazite::undo (model);
    CHECK (phazite::getSidechainSource (model, onKeys) == reverb);
}

TEST_CASE ("A bus's sound reaches a sidechain that listens to it as the song plays")
{
    Song song;
    auto& model = *song.song;
    const auto drums = phazite::groupIntoFolder (model, { song.kick }, nullptr);
    REQUIRE (drums.isValid());
    phazite::setMixedAsBus (model, drums, true);

    // The kick plays a steady level through its bus, and the bass plays what its sidechain hears: the bus's sound, lined up
    // with it, so that the two add up.
    song.add (song.kick, TestTone::typeID);
    using phazite::test::at, phazite::test::lasting;
    const auto clip = phazite::test::addMidiClip (model, song.kick, { at (0.0), at (4.0) }, "Notes");
    REQUIRE (clip.isValid());
    phazite::test::addMidiNote (model, clip, 60, at (0.0), lasting (4.0), 64);
    const auto ducker = song.add (song.bass, TestSidechain::typeID);

    const auto drumsAlone = renderPeak (model);
    CHECK (drumsAlone > 0.1f);

    phazite::setSidechainSource (model, ducker, drums);
    CHECK (renderPeak (model) == doctest::Approx (2.0f * drumsAlone).epsilon (0.01));
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

TEST_CASE ("Phazite's compressor ducks under the key chosen for it, set up to duck if still as it was made, in the same undo step")
{
    using phazite::devices::Compressor;
    Song song;
    auto& model = *song.song;
    const auto valueOf = [&model] (DeviceID device, const char* id)
    { return phazite::getValueSetByHand (model, phazite::ParameterRef { device, id }); };
    const auto duckMode = (float) Compressor::Mode::duck, compressMode = (float) Compressor::Mode::compress;

    // On the bass, from the kick: it pumps, from -14 dB, 14 dB down, in 1 ms, held 120 ms from the hit and back in a straight
    // line over 80 ms; the key whole, as the kick plays no drum sampler; its curve as it was, for when it compresses.
    const auto compressor = song.addCompressor (song.bass);
    model.getUndoManager().clearUndoHistory();
    phazite::setSidechainSource (model, compressor, song.kick);
    CHECK (valueOf (compressor, "mode") == doctest::Approx (duckMode));

    for (const auto& [id, expected] : { std::pair ("threshold", -14.0f), std::pair ("depth", 14.0f), std::pair ("attack", 1.0f),
                                        std::pair ("hold", 120.0f), std::pair ("release", 80.0f), std::pair ("releaseCurve", 0.0f),
                                        std::pair ("keyLowCut", 20.0f), std::pair ("keyHighCut", 20000.0f) })
    {
        CAPTURE (id);
        CHECK (valueOf (compressor, id) == doctest::Approx (expected));
    }

    CHECK (valueOf (compressor, "point2X") == doctest::Approx (phazite::devices::curves::axisForDecibels (-18.0f)).epsilon (1.0e-5));
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Sidechain from Kick"));

    // One step: undo puts back the source, the mode and the settings.
    phazite::undo (model);
    CHECK_FALSE (phazite::getSidechainSource (model, compressor).isValid());
    CHECK (valueOf (compressor, "mode") == doctest::Approx (compressMode));
    CHECK (valueOf (compressor, "attack") == doctest::Approx (10.0f));
    CHECK (valueOf (compressor, "release") == doctest::Approx (120.0f));

    // Its Duck tab clicked first, as its editor's key picker is on Duck's page: still set up to duck as the key is chosen.
    {
        const phazite::UndoStep step (model, "Change Mode");
        phazite::setValueSetByHand (model, phazite::ParameterRef { compressor, "mode" }, duckMode);
    }

    phazite::setSidechainSource (model, compressor, song.kick);
    CHECK (valueOf (compressor, "attack") == doctest::Approx (1.0f));
    phazite::undo (model);
    phazite::undo (model);

    // A compressor already set its own way keeps its settings and its mode: compressing, it compresses as the key says (a
    // keyed compressor). One undo step either way, exact.
    {
        const phazite::UndoStep step (model, "Change Attack");
        phazite::setValueSetByHand (model, phazite::ParameterRef { compressor, "attack" }, 2.0f);
    }

    {
        phazite::test::UndoCheck undo (model);
        undo.check ("Sidechain from Kick", [&] { phazite::setSidechainSource (model, compressor, song.kick); });
    }

    CHECK (phazite::getSidechainSource (model, compressor) == song.kick);
    CHECK (valueOf (compressor, "mode") == doctest::Approx (compressMode));
    CHECK (valueOf (compressor, "attack") == doctest::Approx (2.0f));
    CHECK (valueOf (compressor, "release") == doctest::Approx (120.0f));
    CHECK (phazite::hearsSidechain (model, compressor)); // keyed

    // Listening to none again, it hears its own sound, as it was set.
    phazite::setSidechainSource (model, compressor, {});
    CHECK (valueOf (compressor, "mode") == doctest::Approx (compressMode));

    // As made, the step that sets it to duck is exact too.
    {
        const auto asMade = song.addCompressor (song.keys);
        phazite::test::UndoCheck undo (model);
        undo.check ("Sidechain from Kick", [&] { phazite::setSidechainSource (model, asMade, song.kick); });
        CHECK (valueOf (asMade, "mode") == doctest::Approx (duckMode));
    }

    // A source that plays a drum sampler: only its kick's body, from 60 to 150 Hz, so that the kick ducks the keys, its long boom
    // doesn't hold them down, and the snare, clap and hats don't.
    REQUIRE (
        phazite::setInstrument (model, song.kick, phazite::makeOwnDeviceState (model, phazite::devices::DrumSampler::typeID)).isValid());
    const auto onKeys = song.addCompressor (song.keys);
    phazite::setSidechainSource (model, onKeys, song.kick);
    CHECK (valueOf (onKeys, "keyLowCut") == doctest::Approx (60.0f));
    CHECK (valueOf (onKeys, "keyHighCut") == doctest::Approx (150.0f));
}

TEST_CASE ("Phazite's compressor hears its key while it compresses or ducks: shaping transients, the mixer shows no key, which it keeps")
{
    using phazite::devices::Compressor;
    phazite::test::TestSong song;
    const auto bass = song.firstTrack;
    song.rename (bass, "Bass");
    const auto kick = song.addTrack ("Kick");
    const auto compressor = phazite::duckUnder (song.context, bass, kick);
    REQUIRE (compressor.isValid());

    // A context's listener, which the mixer's strips are, told what changed.
    struct Changes : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { devices = devices || changes.devices; }
        bool devices = false;
    } changes;

    song.context.addListener (&changes);
    const juce::ScopeGuard stopListening { [&song, &changes] { song.context.removeListener (&changes); } };
    phazite::test::dispatchPendingMessages();
    changes.devices = false;

    const auto slot = [&song, bass]
    {
        const auto effects = phazite::getEffects (song.context, bass);
        REQUIRE (effects.size() == 1);
        return effects.front();
    };
    const auto linked = [&song]
    {
        const auto connections = phazite::getConnections (song.context);
        return std::any_of (connections.begin(), connections.end(),
                            [] (const phazite::Connection& c) { return c.kind == phazite::Connection::Kind::sidechain; });
    };

    // Ducking: its slot names the kick and has its colour, and the mixer links the two.
    CHECK (slot().sidechain.track == kick);
    CHECK (linked());

    // Compressing, keyed by the kick: both still.
    song.setParameter (compressor, "mode", (float) Compressor::Mode::compress);
    phazite::test::dispatchPendingMessages();
    CHECK (slot().sidechain.track == kick);
    CHECK (linked());

    // Shaping transients, which hear its own sound: neither, its key kept; a change the mixer hears of, as one of its
    // devices'.
    changes.devices = false;
    song.setParameter (compressor, "mode", (float) Compressor::Mode::transient);
    phazite::test::dispatchPendingMessages();
    CHECK (changes.devices);
    CHECK_FALSE (slot().sidechain.track.isValid());
    CHECK (slot().hasSidechain);
    CHECK_FALSE (linked());
    CHECK (phazite::getSidechainSource (song.context, compressor) == kick);

    // Ducking again: both, under the same kick.
    changes.devices = false;
    song.setParameter (compressor, "mode", (float) Compressor::Mode::duck);
    phazite::test::dispatchPendingMessages();
    CHECK (changes.devices);
    CHECK (slot().sidechain.track == kick);
    CHECK (linked());
}

TEST_CASE ("Phazite's vocoder plays the track its sidechain listens to as its carrier, chosen in the same undo step")
{
    // The track picked by the vocoder's carrier (its editor's picker) is heard at once, whatever carrier it had: no second step.
    Song song;
    auto& model = *song.song;
    const auto vocoder =
        phazite::insertDeviceState (model, song.bass, phazite::makeOwnDeviceState (model, phazite::devices::Vocoder::typeID));
    REQUIRE (vocoder.isValid());
    const auto carrier = [&model, vocoder]
    { return juce::roundToInt (phazite::getValueSetByHand (model, phazite::ParameterRef { vocoder, "carrier" })); };
    REQUIRE (carrier() == (int) phazite::devices::Vocoder::Carrier::buzz);
    model.getUndoManager().clearUndoHistory();

    phazite::setSidechainSource (model, vocoder, song.keys);
    CHECK (phazite::getSidechainSource (model, vocoder) == song.keys);
    CHECK (carrier() == (int) phazite::devices::Vocoder::Carrier::track);
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Sidechain from Keys"));

    // One step: undo puts back the buzz with the source. Listening to none again leaves its carrier as it is.
    phazite::undo (model);
    CHECK_FALSE (phazite::getSidechainSource (model, vocoder).isValid());
    CHECK (carrier() == (int) phazite::devices::Vocoder::Carrier::buzz);
    phazite::setSidechainSource (model, vocoder, song.keys);
    phazite::setSidechainSource (model, vocoder, {});
    CHECK (carrier() == (int) phazite::devices::Vocoder::Carrier::track);
}

TEST_CASE ("Phazite's multiband, as it was made, starts ducking its lows when its sidechain is first chosen, in the same undo step")
{
    using phazite::devices::Multiband;
    Song song;
    auto& model = *song.song;
    const auto valueOf = [&model] (DeviceID device, const char* id)
    { return phazite::getValueSetByHand (model, phazite::ParameterRef { device, id }); };
    const auto multiband = phazite::insertDeviceState (model, song.bass, phazite::makeOwnDeviceState (model, Multiband::typeID));
    REQUIRE (multiband.isValid());
    model.getUndoManager().clearUndoHistory();
    phazite::setSidechainSource (model, multiband, song.kick);

    // Its low band hears the key, through the compressor's ducking curve (8:1 over -30 dB), quick to grip; the other bands as
    // they were, every band at first a limiter at 0 dB, which a kick under full scale never reaches.
    const auto ducking = phazite::devices::curves::makeCompression (-30.0f, 8.0f);
    CHECK (valueOf (multiband, "lowKey") == doctest::Approx (1.0f));
    CHECK (valueOf (multiband, "lowPoints") == doctest::Approx ((float) ducking.size()));
    CHECK (valueOf (multiband, "lowPoint2X") == doctest::Approx (ducking[1][0]));
    CHECK (valueOf (multiband, "lowPoint3Y") == doctest::Approx (ducking[2][1]));
    CHECK (valueOf (multiband, "lowAttack") == doctest::Approx (1.0f));
    CHECK (valueOf (multiband, "lowRelease2") == doctest::Approx (150.0f));
    CHECK (valueOf (multiband, "midKey") == doctest::Approx (0.0f));
    CHECK (valueOf (multiband, "masterKey") == doctest::Approx (0.0f));
    CHECK (valueOf (multiband, "midPoint2X") == doctest::Approx (0.5f));
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Sidechain from Kick"));

    // One step: undo puts back both the source and the settings.
    phazite::undo (model);
    CHECK_FALSE (phazite::getSidechainSource (model, multiband).isValid());
    CHECK (valueOf (multiband, "lowKey") == doctest::Approx (0.0f));
    CHECK (valueOf (multiband, "lowAttack") == doctest::Approx (10.0f));

    // A multiband already set its own way keeps its settings.
    {
        const phazite::UndoStep step (model, "Change Mid Key");
        phazite::setValueSetByHand (model, phazite::ParameterRef { multiband, "midKey" }, 1.0f);
    }

    phazite::setSidechainSource (model, multiband, song.kick);
    CHECK (valueOf (multiband, "lowKey") == doctest::Approx (0.0f));
    CHECK (valueOf (multiband, "midKey") == doctest::Approx (1.0f));
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
    CHECK (phazite::getValueSetByHand (model, phazite::ParameterRef { compressor, "mode" })
           == doctest::Approx ((float) phazite::devices::Compressor::Mode::duck)); // its Duck mode, as it is set up to duck
    CHECK (phazite::getValueSetByHand (model, phazite::ParameterRef { compressor, "attack" }) == doctest::Approx (1.0f));
    CHECK (phazite::hearsSidechain (model, compressor));
    CHECK (model.getUndoManager().getUndoDescription() == juce::String ("Duck Bass Under Kick"));

    phazite::undo (model);
    CHECK (phazite::getDevices (model, song.bass).empty());

    // Not under its own sound, nor under a track whose sound comes from it.
    CHECK_FALSE (phazite::duckUnder (model, song.bass, song.bass).isValid());
    phazite::setSidechainSource (model, song.add (song.keys, TestSidechain::typeID), song.bass);
    CHECK_FALSE (phazite::duckUnder (model, song.bass, song.keys).isValid());
}

TEST_CASE ("A multiband's band whose Key is on ducks under another track through its sidechain; its other bands don't")
{
    using phazite::devices::Multiband;
    phazite::test::TestSong song;
    song.setTempo (120.0);
    const auto bass = song.firstTrack;
    song.rename (bass, "Bass");
    const auto kick = song.addTrack ("Kick");

    // The kick, a loud low tone, muted (its sidechain still hears it); the bass, a quiet tone in its multiband's low band.
    const auto sounds = song.getSoundsFolder();
    song.addAudioClip (kick, phazite::test::writeSound (sounds.getChildFile ("Kick.wav"), 4.0, 80.0, 0.9f), 0.0);
    song.addAudioClip (bass, phazite::test::writeSound (sounds.getChildFile ("Bass.wav"), 4.0, 60.0, 0.03f), 0.0);
    phazite::setTrackMuted (*phazite::findSong (song.context), kick, true);
    const auto multiband = song.addEffect (bass, Multiband::typeID);

    // Its low and mid bands limit at -12 dB: the bass, at -30 dB, passes them, and a band that hears the kick ducks.
    for (const auto* band : { "low", "mid" })
    {
        const auto prefix = juce::String (band);
        const auto place = phazite::devices::curves::axisForDecibels (-12.0f);
        song.setParameter (multiband, prefix + "Point2X", place);
        song.setParameter (multiband, prefix + "Point2Y", place);
        song.setParameter (multiband, prefix + "Point3Y", place);
    }

    phazite::setSidechainSource (song.context, multiband, kick);
    REQUIRE (phazite::getSidechainSource (song.context, multiband) == kick);

    // What plays over a second, from the song's start, past the multiband's latency and attack: the bass's level.
    const auto play = [&song]
    {
        phazite::test::EnginePlayback playback;
        phazite::test::EnginePlayback::waitForFiles();
        song.run ("transport.playStop");
        playback.play (0.5);
        const auto played = playback.play (1.0);
        song.run ("transport.playStop");
        return juce::Decibels::gainToDecibels (played.getRMSLevel (0, 0, played.getNumSamples()));
    };

    // The bass as it came: a sine's RMS 3 dB under its peak at -30 dB, from a mono file 3 dB down on each side.
    const auto alone = play();
    CHECK (alone == doctest::Approx (-36.5f).epsilon (0.03));

    // The low band's Key on: the bass ducks under the kick, as far as the kick is over -12 dB, about 11 dB.
    song.setParameter (multiband, "lowKey", 1.0f);
    CHECK (play() < alone - 8.0f);

    // The mid band's instead: the bass, in the low band, plays as it did.
    song.setParameter (multiband, "lowKey", 0.0f);
    song.setParameter (multiband, "midKey", 1.0f);
    CHECK (play() == doctest::Approx (alone).epsilon (0.02));
}

TEST_CASE ("Phazite's compressor's sound comes as late as its look-ahead while it ducks, and only as late as its drive while it compresses")
{
    // Its latency follows its mode, which makes it anew in a new plan (model/playback/internal/SongPlayer.h), so that the
    // engine lines the other tracks up with it (devices/dynamics/Compressor.h: duckLookaheadSeconds; compressing, the few
    // samples its drive plays the sound late by, far under what Low Latency leaves out).
    phazite::test::TestSong song;
    auto& context = song.context;
    auto& model = *phazite::findSong (context);
    phazite::addEffect (context, song.firstTrack, song.findDeviceType (phazite::devices::Compressor::typeID));
    REQUIRE (phazite::getEffects (context, song.firstTrack).size() == 1);
    const auto compressor = phazite::getEffects (context, song.firstTrack)[0].id;
    const auto latency = [&context, &song] { return phazite::getEffects (context, song.firstTrack)[0].latencySeconds; };
    const auto setMode = [&model, compressor] (phazite::devices::Compressor::Mode mode)
    {
        const phazite::UndoStep step (model, "Change Mode");
        phazite::setValueSetByHand (model, phazite::ParameterRef { compressor, "mode" }, (float) mode);
    };

    const auto isDrivesAlone = [&latency] { return latency() > 0.0 && latency() < 0.0005; };
    phazite::test::dispatchPendingMessages();
    CHECK (isDrivesAlone());

    setMode (phazite::devices::Compressor::Mode::duck);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&latency] { return latency() > 0.005; }));
    CHECK (latency() == doctest::Approx (phazite::devices::Compressor::duckLookaheadSeconds).epsilon (0.01));

    setMode (phazite::devices::Compressor::Mode::compress);
    REQUIRE (phazite::test::dispatchMessagesUntil (isDrivesAlone));
}

TEST_CASE ("Phazite's compressor's latency follows its Look-Ahead set by hand, and Low Latency leaves it out past a millisecond")
{
    // Compressing, its sound comes as late as its drive and its look-ahead, which makes it anew as the value set by hand
    // changes; while Low Latency is on, one that comes more than a millisecond late is left out, as a ducking one is
    // (model/playback/internal/PlaybackHost.h), and one under it plays on.
    phazite::test::TestSong song;
    auto& context = song.context;
    auto& model = *phazite::findSong (context);
    phazite::addEffect (context, song.firstTrack, song.findDeviceType (phazite::devices::Compressor::typeID));
    const auto compressor = phazite::getEffects (context, song.firstTrack)[0].id;
    const auto latency = [&context, &song] { return phazite::getEffects (context, song.firstTrack)[0].latencySeconds; };
    const auto setLookahead = [&model, compressor] (float milliseconds)
    {
        const phazite::UndoStep step (model, "Change Look-Ahead");
        phazite::setValueSetByHand (model, phazite::ParameterRef { compressor, "lookahead" }, milliseconds);
    };

    phazite::test::dispatchPendingMessages();
    const auto driven = latency();
    REQUIRE (driven > 0.0);
    REQUIRE (driven < 0.0005);

    setLookahead (5.0f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&latency] { return latency() > 0.004; }));
    CHECK (latency() == doctest::Approx (driven + 0.005).epsilon (0.01));

    auto& host = phazite::getPlaybackHost (phazite::test::getEngineContext());
    const juce::ScopeGuard lowLatencyOff { [&host] { host.setLowLatency (false); } };
    host.setLowLatency (true);
    CHECK (juce::exactlyEqual (latency(), 0.0));

    setLookahead (0.5f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&latency] { return latency() > 0.0 && latency() < 0.001; }));
    CHECK (latency() == doctest::Approx (driven + 0.0005).epsilon (0.05));
}

TEST_CASE ("A drag of a compressor's Look-Ahead makes it anew once, as the drag ends, not at each of its steps")
{
    // Its latency follows the Look-Ahead set by hand, and a new latency is a device made anew in a new plan, which starts it
    // again from silence, its reduction lost: a sweep of the knob was hundreds of them. While a gesture drags it, the plan
    // waits for the gesture's end; meanwhile it plays the value within the latency it has.
    phazite::test::TestSong song;
    auto& context = song.context;
    phazite::addEffect (context, song.firstTrack, song.findDeviceType (phazite::devices::Compressor::typeID));
    const auto compressor = phazite::getEffects (context, song.firstTrack)[0].id;
    const auto latency = [&context, &song] { return phazite::getEffects (context, song.firstTrack)[0].latencySeconds; };
    phazite::test::dispatchPendingMessages();
    const auto driven = latency();
    REQUIRE (driven > 0.0);
    REQUIRE (driven < 0.0005);

    // Set by hand to 1 ms first, a step of its own: made anew for it.
    auto handle = phazite::getParameter (context, { compressor, "lookahead" });
    handle.beginGesture();
    handle.setValue (1.0f);
    handle.endGesture();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return latency() > driven + 0.0009; }));
    const auto atOne = latency();

    // Then dragged on to 6 ms, twenty steps: the same device, made for 1 ms, plays on until the drag ends.
    handle.beginGesture();

    for (int step = 1; step <= 20; ++step)
    {
        handle.setValue (1.0f + 0.25f * (float) step);
        phazite::test::dispatchPendingMessages();
        CHECK (latency() == doctest::Approx (atOne));
    }

    handle.endGesture();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&latency] { return latency() > 0.005; }));
    CHECK (latency() == doctest::Approx (driven + 0.006).epsilon (0.01));
}

TEST_CASE ("A compressor whose Mix was turned isn't as it was made: choosing a key keeps its mode")
{
    using phazite::devices::Compressor;
    Song song;
    auto& model = *song.song;
    const auto compressor = song.addCompressor (song.bass);

    {
        const phazite::UndoStep step (model, "Change Mix");
        phazite::setMix (model, compressor, 0.5f);
    }

    phazite::setSidechainSource (model, compressor, song.kick);
    CHECK (phazite::getValueSetByHand (model, phazite::ParameterRef { compressor, "mode" })
           == doctest::Approx ((float) Compressor::Mode::compress));
    CHECK (phazite::getMix (model, compressor) == doctest::Approx (0.5f));
}

TEST_CASE ("A sidechain's key reaches its device while the device's track is soloed, a bus's too, whose tracks send nothing meanwhile")
{
    // The owner, 2026-11-13 (known bug 18): a sub ducking under the Drums bus stopped ducking while it was soloed. A track
    // soloed out plays on for a sidechain that listens to it, and a track in a bus that is a key plays on into the bus, which
    // its own solo silences after the key is taken; neither sends to a return meanwhile.
    Song song;
    auto& model = *song.song;
    using phazite::test::at, phazite::test::lasting;
    song.add (song.kick, TestTone::typeID);
    const auto clip = phazite::test::addMidiClip (model, song.kick, { at (0.0), at (4.0) }, "Notes");
    REQUIRE (clip.isValid());
    phazite::test::addMidiNote (model, clip, 60, at (0.0), lasting (4.0), 64);
    const auto ducker = song.add (song.bass, TestSidechain::typeID); // plays what its sidechain hears
    const auto kickAlone = renderPeak (model);
    REQUIRE (kickAlone > 0.1f);

    SUBCASE ("A track as the key")
    {
        phazite::setSidechainSource (model, ducker, song.kick);
        phazite::setTrackSoloed (model, song.bass, true);
        CHECK (renderPeak (model) == doctest::Approx (kickAlone).epsilon (0.01)); // the bass alone, playing its key
    }

    SUBCASE ("A bus as the key, with a track in it that sends to a return")
    {
        const auto drums = phazite::groupIntoFolder (model, { song.kick }, nullptr);
        REQUIRE (drums.isValid());
        phazite::setMixedAsBus (model, drums, true);
        const auto reverb = phazite::addReturnTrack (model, "Reverb", nullptr);
        REQUIRE (reverb.isValid());

        phazite::DeviceID beforeFader;

        for (const auto preFader : { false, true })
        {
            const auto send = phazite::addSend (model, song.kick, reverb, preFader);
            REQUIRE (send.isValid());
            phazite::setSendLevel (model, send, 0.0f);

            if (preFader)
                beforeFader = send;
        }

        phazite::setSidechainSource (model, ducker, drums);
        CHECK (renderPeak (model) > 2.0f * kickAlone); // the bus, the bass playing its key and the return

        // Soloed, the bass alone: its key still comes, and neither the bus nor the return is heard.
        phazite::setTrackSoloed (model, song.bass, true);
        CHECK (renderPeak (model) == doctest::Approx (kickAlone).epsilon (0.01));

        // Muted, the kick is heard nowhere through the bus, its key and its send after the fader silent too, as the bus's
        // sound is; but its send before the fader goes on (mute silences a track's output only: the owner's decision,
        // roadmap S4.8), so the return hears it until that send comes after the fader.
        phazite::setTrackSoloed (model, song.bass, false);
        phazite::setTrackMuted (model, song.kick, true);
        CHECK (renderPeak (model) > 0.1f);
        phazite::setPreFader (model, beforeFader, false);
        CHECK (renderPeak (model) < 1.0e-4f);
    }
}

TEST_CASE ("Soloing a track in a bus that is a key plays it alone; a muted bus still feeds its key, and a track muted in it doesn't")
{
    // The owner, 2026-11-13: after known bug 18's fix, soloing a drum in the Drums bus still played the bus's other drums,
    // which played on for the bus's key while the bus, holding the soloed drum, was heard. They play on for the key only while
    // the bus isn't heard.
    Song song;
    auto& model = *song.song;
    using phazite::test::at, phazite::test::lasting;

    // Two drums in a bus, each a steady level, the second half the first's; and the bass playing what its sidechain hears.
    for (const auto& [track, velocity] : { std::pair { song.kick, 64 }, std::pair { song.keys, 32 } })
    {
        song.add (track, TestTone::typeID);
        const auto clip = phazite::test::addMidiClip (model, track, { at (0.0), at (4.0) }, "Notes");
        REQUIRE (clip.isValid());
        phazite::test::addMidiNote (model, clip, 60, at (0.0), lasting (4.0), velocity);
    }

    const auto drums = phazite::groupIntoFolder (model, { song.kick, song.keys }, nullptr);
    REQUIRE (drums.isValid());
    phazite::setMixedAsBus (model, drums, true);
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    phazite::setSidechainSource (model, ducker, drums);
    const auto all = renderPeak (model); // the bus, and the bass playing it again as loud
    REQUIRE (all > 0.1f);

    // A drum soloed plays alone, through its bus: the other drum and the bass silent.
    phazite::setTrackSoloed (model, song.kick, true);
    const auto kickAlone = renderPeak (model);
    phazite::setTrackSoloed (model, song.kick, false);
    phazite::setTrackSoloed (model, song.keys, true);
    const auto snareAlone = renderPeak (model);
    phazite::setTrackSoloed (model, song.keys, false);
    CHECK (kickAlone == doctest::Approx (2.0f * snareAlone).epsilon (0.01));
    CHECK (kickAlone + snareAlone == doctest::Approx (all / 2.0f).epsilon (0.01));

    // The bass soloed: its key is the whole bus, which isn't heard (known bug 18).
    phazite::setTrackSoloed (model, song.bass, true);
    CHECK (renderPeak (model) == doctest::Approx (all / 2.0f).epsilon (0.01));
    phazite::setTrackSoloed (model, song.bass, false);

    // The bus muted still feeds its key, as a muted track does: a "ghost" bus, the bass alone heard.
    phazite::setTrackMuted (model, drums, true);
    CHECK (renderPeak (model) == doctest::Approx (all / 2.0f).epsilon (0.01));
    phazite::setTrackMuted (model, drums, false);

    // A drum muted in it leaves the bus's sound, and so its key, soloed out or not.
    phazite::setTrackMuted (model, song.kick, true);
    CHECK (renderPeak (model) == doctest::Approx (all / 3.0f).epsilon (0.01));
    phazite::setTrackSoloed (model, song.bass, true);
    CHECK (renderPeak (model) == doctest::Approx (all / 6.0f).epsilon (0.01));
}

TEST_CASE ("A drum pad on a track of its own in a bus that is a key feeds the key unless its sampler's track or a folder of it is muted")
{
    // A pad's track goes with its sampler's track, whose mute covers it as a folder's covers its tracks
    // (model/devices/internal/DrumKit.h): muted so, it is muted before the bus, and feeds the bus's key no more than a track
    // muted in the bus does, while the bass is soloed or the bus muted.
    Song song;
    auto& model = *song.song;
    using phazite::test::at, phazite::test::lasting;
    const auto sampler =
        phazite::setInstrument (model, song.keys, phazite::makeOwnDeviceState (model, phazite::devices::DrumSampler::typeID));
    REQUIRE (sampler.isValid());
    const auto clip = phazite::test::addMidiClip (model, song.keys, { at (0.0), at (4.0) }, "Beat");
    REQUIRE (clip.isValid());
    phazite::test::addMidiNote (model, clip, 36, at (0.0), lasting (0.25), 127);

    // The kick on a track of its own in the Drums bus, which the bass's sidechain listens to; the kit in a folder of its own.
    const auto kickPad = phazite::givePadItsOwnTrack (model, sampler, 36);
    REQUIRE (kickPad.isValid());
    const auto drums = phazite::groupIntoFolder (model, { kickPad }, nullptr);
    REQUIRE (drums.isValid());
    phazite::setMixedAsBus (model, drums, true);
    const auto kit = phazite::groupIntoFolder (model, { song.keys }, nullptr);
    REQUIRE (kit.isValid());
    const auto ducker = song.add (song.bass, TestSidechain::typeID);
    phazite::setSidechainSource (model, ducker, drums);
    const auto all = renderPeak (model); // the bus, and the bass playing it again as loud
    REQUIRE (all > 0.1f);

    // The bass soloed, or the bus muted: the kick plays on for the key, the bass alone heard.
    phazite::setTrackSoloed (model, song.bass, true);
    CHECK (renderPeak (model) == doctest::Approx (all / 2.0f).epsilon (0.01));
    phazite::setTrackSoloed (model, song.bass, false);
    phazite::setTrackMuted (model, drums, true);
    CHECK (renderPeak (model) == doctest::Approx (all / 2.0f).epsilon (0.01));
    phazite::setTrackMuted (model, drums, false);

    // The kit's folder muted, the kick with it: heard nowhere, nor in the key, also with the bass soloed or the bus muted.
    phazite::setTrackMuted (model, kit, true);
    CHECK (renderPeak (model) < 1.0e-4f);
    phazite::setTrackSoloed (model, song.bass, true);
    CHECK (renderPeak (model) < 1.0e-4f);
    phazite::setTrackSoloed (model, song.bass, false);
    phazite::setTrackMuted (model, drums, true);
    CHECK (renderPeak (model) < 1.0e-4f);
}

TEST_CASE ("A soloed return hears what is sent to it, from tracks its solo silences, but not from muted ones")
{
    // Soloing a return, to hear an effect alone, played silence: every track the solo silenced had its sends cut. A soloed
    // return hears what arrives through sends, as in Ableton Live, Logic Pro and Cubase; the tracks' own sound isn't heard, nor
    // what they send to returns that aren't soloed.
    Song song;
    auto& model = *song.song;
    using phazite::test::at, phazite::test::lasting;
    song.add (song.kick, TestTone::typeID);
    const auto clip = phazite::test::addMidiClip (model, song.kick, { at (0.0), at (4.0) }, "Notes");
    REQUIRE (clip.isValid());
    phazite::test::addMidiNote (model, clip, 60, at (0.0), lasting (4.0), 64);
    const auto kickAlone = renderPeak (model);
    REQUIRE (kickAlone > 0.1f);

    const auto reverb = phazite::addReturnTrack (model, "Reverb", nullptr);
    const auto delay = phazite::addReturnTrack (model, "Delay", nullptr);
    REQUIRE (reverb.isValid());
    REQUIRE (delay.isValid());
    const auto toReverb = phazite::addSend (model, song.kick, reverb, false);
    const auto toDelay = phazite::addSend (model, song.kick, delay, false);
    REQUIRE (toReverb.isValid());
    REQUIRE (toDelay.isValid());
    phazite::setSendLevel (model, toReverb, 0.0f);
    phazite::setSendLevel (model, toDelay, 0.0f);
    CHECK (renderPeak (model) == doctest::Approx (3.0f * kickAlone).epsilon (0.01)); // the kick and both returns

    for (const auto preFader : { false, true })
    {
        CAPTURE (preFader);
        phazite::setPreFader (model, toReverb, preFader);

        // The reverb soloed: what the kick sends to it, alone.
        phazite::setTrackSoloed (model, reverb, true);
        CHECK (renderPeak (model) == doctest::Approx (kickAlone).epsilon (0.01));

        // The kick soloed too: itself, and what it sends to both returns, which no solo silences.
        phazite::setTrackSoloed (model, song.kick, true);
        CHECK (renderPeak (model) == doctest::Approx (3.0f * kickAlone).epsilon (0.01));
        phazite::setTrackSoloed (model, song.kick, false);

        // Muted, the kick sends nothing, to a soloed return either.
        phazite::setTrackMuted (model, song.kick, true);
        CHECK (renderPeak (model) < 1.0e-4f);
        phazite::setTrackMuted (model, song.kick, false);
        phazite::setTrackSoloed (model, reverb, false);
    }

    // Another track soloed: the kick is heard nowhere, as before.
    phazite::setTrackSoloed (model, song.bass, true);
    CHECK (renderPeak (model) < 1.0e-4f);
}
