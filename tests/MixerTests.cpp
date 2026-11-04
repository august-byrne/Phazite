// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/delay/Delay.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/DeviceNode.h"
#include "engine/nodes/Strip.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/MixHandle.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/MixHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::devices::Compressor, phazite::devices::Delay, phazite::devices::Equalizer, phazite::devices::Reverb, phazite::devices::Synth;
namespace schema = phazite::schema;

// A song with two tracks, A and B, open in a context of its own, so that Phazite's engine plays it.
struct Mix
{
    Mix()
    {
        const auto tracks = phazite::ensureTracks (*song, 2);
        REQUIRE (tracks.size() == 2);
        a = tracks[0];
        b = tracks[1];
        phazite::setTrackName (*song, a, "A");
        phazite::setTrackName (*song, b, "B");
        setSong (context, song.get());
    }

    ~Mix() { setSong (context, nullptr); }

    // One of Phazite's own devices, new, put on a channel at a place among its devices (-1: after them), in no undo step of
    // its own; none if the channel can't hold it.
    phazite::DeviceID add (phazite::TrackID channel, const char* type, int index = -1)
    {
        return phazite::insertDeviceState (*song, channel, phazite::makeOwnDeviceState (*song, type), index);
    }

    // What Phazite's engine plays of a channel (a track's by its ID, the master's by none) and of a device, once the song's
    // session has given it the song as it is now: nullptr for what it doesn't play. A plain folder has no strip, and so no
    // meter: no sound of its own.
    phazite::engine::Channel* findPlayedChannel (phazite::TrackID channel)
    {
        auto* strip = getUpdatedSession().getPlayer().findStrip ((std::int64_t) channel.getRaw());
        return strip != nullptr ? &strip->getChannel() : nullptr;
    }

    phazite::engine::DeviceNode* findPlayedDevice (phazite::DeviceID device)
    {
        return getUpdatedSession().getPlayer().findDevice ((std::int64_t) device.getRaw());
    }

    bool hasMeter (phazite::TrackID channel) { return findPlayedChannel (channel) != nullptr; }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::TrackID a, b;

private:
    phazite::PlaybackSession& getUpdatedSession()
    {
        auto* session = phazite::getSession (context);
        REQUIRE (session != nullptr);
        session->update();
        return *session;
    }
};

phazite::DeviceID idOf (const juce::ValueTree& channel, const juce::Identifier& property)
{
    return phazite::DeviceID::fromRaw ((std::uint64_t) (juce::int64) channel[property]);
}
} // namespace

TEST_CASE ("Devices go before the fader, instruments first")
{
    Mix mix;
    auto& song = *mix.song;
    const auto reverb = mix.add (mix.a, Reverb::typeID);
    REQUIRE (reverb.isValid());

    const auto synth = mix.add (mix.a, Synth::typeID, -1);
    REQUIRE (synth.isValid());

    // The instrument went first though it was added at the end; the fader and meter aren't devices.
    CHECK (phazite::getDevices (song, mix.a) == std::vector { synth, reverb });

    const auto delay = mix.add (mix.a, Delay::typeID, 1);
    CHECK (phazite::getDevices (song, mix.a)[1] == delay);

    phazite::moveDevice (song, delay, 2);
    CHECK (phazite::getDevices (song, mix.a)[2] == delay);
    phazite::moveDevice (song, delay, 0);
    CHECK (phazite::getDevices (song, mix.a)[0] == delay);
    CHECK (phazite::getDevices (song, mix.a)[1] == synth);

    phazite::removeDevice (song, reverb);
    CHECK (phazite::getDevices (song, mix.a).size() == 2);
}

TEST_CASE ("A track's instrument plays its notes, and its effects come after it")
{
    Mix mix;
    auto& song = *mix.song;
    CHECK_FALSE (phazite::getInstrument (song, mix.a).isValid());
    CHECK_FALSE (phazite::needsInstrument (song, mix.a)); // no notes yet

    // Notes without an instrument make no sound.
    phazite::test::addMidiClip (song, mix.a, { phazite::test::at (0.0), phazite::test::at (4.0) });
    CHECK (phazite::needsInstrument (song, mix.a));

    // An effect doesn't play notes.
    const auto reverb = mix.add (mix.a, Reverb::typeID);
    REQUIRE (reverb.isValid());
    CHECK_FALSE (phazite::getInstrument (song, mix.a).isValid());
    CHECK (phazite::needsInstrument (song, mix.a));
    CHECK (phazite::getEffects (song, mix.a) == std::vector { reverb });

    const auto synth = phazite::setInstrument (song, mix.a, phazite::makeOwnDeviceState (song, Synth::typeID));
    REQUIRE (synth.isValid());
    CHECK (phazite::getInstrument (song, mix.a) == synth);
    CHECK (phazite::hasInstrument (song, mix.a));
    CHECK_FALSE (phazite::needsInstrument (song, mix.a));
    CHECK (phazite::getEffects (song, mix.a) == std::vector { reverb });

    // Another instrument takes the first one's place; an effect isn't one.
    const auto other = phazite::setInstrument (song, mix.a, phazite::makeOwnDeviceState (song, Synth::typeID));
    REQUIRE (other.isValid());
    CHECK (phazite::getDevices (song, mix.a) == std::vector { other, reverb });
    CHECK_FALSE (phazite::setInstrument (song, mix.a, phazite::makeOwnDeviceState (song, Delay::typeID)).isValid());
    CHECK (phazite::getInstrument (song, mix.a) == other);
}

TEST_CASE ("A new folder is a bus; a plain one becomes a bus with its first device, and plain again without one")
{
    Mix mix;
    auto& song = *mix.song;
    const auto folder = phazite::groupIntoFolder (song, { mix.a, mix.b }, nullptr);
    REQUIRE (folder.isValid());
    const auto folderState = phazite::findTrackState (song, folder);

    // A bus from the start (design question 11): its tracks' sound goes through its own fader and meter.
    CHECK (phazite::isBus (song, folder));
    CHECK (folderState.hasProperty (schema::channelFader));
    CHECK_FALSE (folderState.hasProperty (schema::channelVCA));
    CHECK (mix.hasMeter (folder));
    CHECK (phazite::isBus (song, phazite::insertFolder (song, {}, nullptr)));

    // Made plain with its switch, its volume scales its tracks' volumes instead, as a VCA does.
    phazite::setMixedAsBus (song, folder, false);
    CHECK_FALSE (phazite::isBus (song, folder));
    REQUIRE (folderState.hasProperty (schema::channelVCA));
    CHECK_FALSE (folderState.hasProperty (schema::channelFader));
    phazite::setChannelVolume (song, folder, -3.0f);
    CHECK_FALSE (mix.hasMeter (folder)); // a plain folder has no sound of its own
    REQUIRE (mix.findPlayedChannel (mix.a) != nullptr);
    CHECK (mix.findPlayedChannel (mix.a)->getVolume() == doctest::Approx (-3.0f));

    const auto compressor = mix.add (folder, Compressor::typeID);
    REQUIRE (compressor.isValid());
    CHECK (phazite::isBus (song, folder));
    CHECK_FALSE (folderState.hasProperty (schema::channelVCA));
    REQUIRE (folderState.hasProperty (schema::channelFader));
    CHECK (phazite::getChannelVolume (song, folder) == doctest::Approx (-3.0f));
    CHECK (mix.hasMeter (folder));
    CHECK (phazite::getDevices (song, folder).size() == 1);
    CHECK (phazite::getFader (song, folder) == idOf (folderState, schema::channelFader));
    REQUIRE (mix.findPlayedChannel (folder) != nullptr);
    CHECK (mix.findPlayedChannel (folder)->getVolume() == doctest::Approx (-3.0f));
    CHECK (std::abs (mix.findPlayedChannel (mix.a)->getVolume()) < 1.0e-4f); // its own fader alone again

    phazite::removeDevice (song, compressor);
    CHECK_FALSE (phazite::isBus (song, folder));
    REQUIRE (folderState.hasProperty (schema::channelVCA));
    CHECK (phazite::getChannelVolume (song, folder) == doctest::Approx (-3.0f));
}

TEST_CASE ("A channel's fader is the song's, by its ID, which what plays the channel plays")
{
    Mix mix;
    auto& song = *mix.song;
    const auto a = phazite::findTrackState (song, mix.a);
    const auto master = phazite::getMasterState (song);
    CHECK (idOf (a, schema::channelFader).isValid());
    CHECK (phazite::getFader (song, mix.a) == idOf (a, schema::channelFader));
    CHECK (phazite::getFader (song, {}) == idOf (master, schema::channelFader));
    CHECK (phazite::getFader (song, mix.a) != phazite::getFader (song, {}));
    CHECK (phazite::findChannelOfFader (song, phazite::getFader (song, mix.a)) == a);

    // A folder starts as a bus, with a fader; made plain, a VCA of its own takes the fader's place and its meter goes, and
    // undo brings them back.
    const auto folder = phazite::groupIntoFolder (song, { mix.b }, nullptr);
    REQUIRE (folder.isValid());
    const auto folderState = phazite::findTrackState (song, folder);
    const auto fader = idOf (folderState, schema::channelFader);
    CHECK (fader.isValid());
    CHECK (mix.hasMeter (folder));

    phazite::setMixedAsBus (song, folder, false);
    CHECK_FALSE (folderState.hasProperty (schema::channelFader));
    const auto vca = idOf (folderState, schema::channelVCA);
    CHECK (vca.isValid());
    CHECK (vca != fader);
    CHECK (phazite::getFader (song, folder) == vca);
    CHECK (phazite::getVolumeParameterID (song, folder) == juce::String (schema::vcaVolume));
    CHECK_FALSE (mix.hasMeter (folder));

    phazite::undo (song);
    CHECK (idOf (folderState, schema::channelFader) == fader);
    CHECK_FALSE (folderState.hasProperty (schema::channelVCA));
    CHECK (phazite::getFader (song, folder) == fader);
    CHECK (mix.hasMeter (folder));

    // What plays each channel plays the song's volumes, the master's at -3 dB.
    phazite::setChannelVolume (song, mix.a, -6.0f);
    REQUIRE (mix.findPlayedChannel (mix.a) != nullptr);
    REQUIRE (mix.findPlayedChannel ({}) != nullptr);
    CHECK (mix.findPlayedChannel (mix.a)->getVolume() == doctest::Approx (-6.0f).epsilon (1.0e-3));
    CHECK (mix.findPlayedChannel ({})->getVolume() == doctest::Approx (-3.0f).epsilon (1.0e-3));
}

TEST_CASE ("Return tracks take in what sends feed them from before or after the fader, and no solo silences them")
{
    Mix mix;
    auto& song = *mix.song;
    const auto reverbReturn = phazite::addReturnTrack (song, "Reverb", nullptr);
    REQUIRE (reverbReturn.isValid());
    CHECK (phazite::getTrackName (song, reverbReturn) == juce::String ("Reverb"));
    CHECK (phazite::isReturnTrack (song, reverbReturn));
    CHECK_FALSE (phazite::isReturnTrack (song, mix.a));
    CHECK (phazite::getTrackIDs (song).back() == reverbReturn); // a channel, like any track

    // A return holds effects, as a track does.
    const auto reverb = mix.add (reverbReturn, Reverb::typeID);
    REQUIRE (reverb.isValid());
    CHECK (phazite::getEffects (song, reverbReturn) == std::vector { reverb });

    const auto send = phazite::addSend (song, mix.a, reverbReturn, false);
    REQUIRE (send.isValid());
    CHECK (phazite::getSendTarget (song, send) == reverbReturn);
    CHECK (phazite::getSendLevel (song, send) == doctest::Approx (-6.0f));
    CHECK_FALSE (phazite::isPreFader (song, send));
    CHECK (phazite::getSends (song, mix.a) == std::vector { send });

    phazite::setPreFader (song, send, true);
    CHECK (phazite::isPreFader (song, send));
    phazite::setPreFader (song, send, false);
    CHECK_FALSE (phazite::isPreFader (song, send));

    const auto second = phazite::addReturnTrack (song, "Delay", nullptr);
    REQUIRE (second.isValid());
    CHECK (phazite::getReturnTracks (song) == std::vector { reverbReturn, second });

    // Returns are solo-safe: soloing a track leaves them audible, so its sends to them are heard.
    phazite::setTrackSoloed (song, mix.a, true);
    CHECK_FALSE (phazite::isSilencedBySolo (song, reverbReturn));
    CHECK (phazite::isSilencedBySolo (song, mix.b));
    phazite::setTrackSoloed (song, mix.a, false);
}

TEST_CASE ("The device list offers Phazite's own devices, instruments first, then the plug-ins found")
{
    phazite::test::TestPlugin testPlugin;
    Mix mix;
    juce::StringArray own;
    bool pluginAfterThem = false;

    for (const auto& type : phazite::getDeviceTypes (phazite::getPluginHost (phazite::test::getEngineContext())))
    {
        if (type.description.pluginFormatName == juce::String (schema::ownDeviceFormat))
        {
            CHECK_FALSE (pluginAfterThem);
            own.add (type.description.fileOrIdentifier);
            CHECK (type.description.manufacturerName == juce::String ("Phazite"));

            // Each makes a device of the song's, of its type.
            const auto device = phazite::makeDeviceState (*mix.song, type);
            CHECK (device[schema::deviceType].toString() == type.description.fileOrIdentifier);
        }
        else if (type.description.fileOrIdentifier == phazite::test::TestPlugin::getDescription().fileOrIdentifier)
        {
            pluginAfterThem = true;
        }
    }

    CHECK (pluginAfterThem);
    CHECK (own
           == juce::StringArray { "phaziteSynth", "phaziteWavetable", "phaziteDrums", "phaziteEq", "phaziteFilter", "phaziteCompressor",
                                  "phaziteMultiband", "phaziteLimiter", "phaziteSaturator", "phaziteDelay", "phaziteReverb",
                                  "phazitePhaser", "phaziteFlanger", "phaziteVocoder", "phaziteStereoExpander" });
}

TEST_CASE ("Phazite's effects have a mix of their own, and no key controls")
{
    Mix mix;
    auto& song = *mix.song;
    const auto eq = mix.add (mix.a, Equalizer::typeID);
    REQUIRE (eq.isValid());

    REQUIRE (phazite::hasMix (song, eq));
    CHECK (phazite::getMix (song, eq) == doctest::Approx (1.0f)); // all effect to start with
    phazite::setMix (song, eq, 0.3f);
    CHECK (phazite::getMix (song, eq) == doctest::Approx (0.3f));
    CHECK (phazite::getValueSetByHand (song, phazite::ParameterRef { eq, phazite::devices::mixParameterID.data() })
           == doctest::Approx (0.3f));
    CHECK (phazite::getKeyControls (song, eq, 2).isEmpty());
}

TEST_CASE ("A folder's bus switch makes it a bus that stays one, and plain again while it has no effects")
{
    Mix mix;
    auto& song = *mix.song;
    const auto folder = phazite::groupIntoFolder (song, { mix.a, mix.b }, nullptr);
    REQUIRE (folder.isValid());

    phazite::setMixedAsBus (song, folder, true);
    CHECK (phazite::isBus (song, folder));

    // Made a bus with the switch, it stays one when its last effect goes.
    const auto compressor = mix.add (folder, Compressor::typeID);
    REQUIRE (compressor.isValid());
    phazite::removeDevice (song, compressor);
    CHECK (phazite::isBus (song, folder));

    // With an effect, it stays a bus: making it plain would take the effect away.
    const auto reverb = mix.add (folder, Reverb::typeID);
    REQUIRE (reverb.isValid());
    phazite::setMixedAsBus (song, folder, false);
    CHECK (phazite::isBus (song, folder));

    phazite::removeDevice (song, reverb);
    phazite::setMixedAsBus (song, folder, false);
    CHECK_FALSE (phazite::isBus (song, folder));
    REQUIRE (phazite::findTrackState (song, folder).hasProperty (schema::channelVCA));

    // A bus that became one by getting an effect turns plain again without it, as before.
    const auto delay = mix.add (folder, Delay::typeID);
    REQUIRE (delay.isValid());
    CHECK (phazite::isBus (song, folder));
    phazite::removeDevice (song, delay);
    CHECK_FALSE (phazite::isBus (song, folder));
}

TEST_CASE ("A return with an effect comes in one step, named after the effect, with a send to it")
{
    phazite::test::TestPlugin testPlugin;
    Mix mix;
    auto& song = *mix.song;

    // Phazite's reverb, at 25 % on a track, plays only its effect on a return: the dry sound reaches the master through the
    // track itself.
    const auto mixIndex = phazite::devices::findParameter (Reverb::info, phazite::devices::mixParameterID);
    REQUIRE (mixIndex >= 0);
    REQUIRE (Reverb::info.parameters[(size_t) mixIndex].defaultValue == doctest::Approx (0.25f));
    const auto reverb = phazite::makeOwnDeviceState (song, Reverb::typeID);
    const auto reverbReturn = phazite::addReturnWithEffect (song, mix.a, reverb);
    REQUIRE (reverbReturn.isValid());
    CHECK (phazite::getTrackName (song, reverbReturn) == juce::String ("Reverb"));
    CHECK (phazite::isReturnTrack (song, reverbReturn));
    CHECK (phazite::getEffects (song, reverbReturn) == std::vector { phazite::getDeviceID (reverb) });
    CHECK (phazite::getMix (song, phazite::getDeviceID (reverb)) == doctest::Approx (1.0f));

    const auto sends = phazite::getSends (song, mix.a);
    REQUIRE (sends.size() == 1);
    CHECK (phazite::getSendTarget (song, sends[0]) == reverbReturn);
    CHECK_FALSE (phazite::isPreFader (song, sends[0]));

    // Another one gets a name of its own. A plug-in plays only its effect there too, whatever mix it came with.
    const auto second = phazite::addReturnWithEffect (song, mix.b, phazite::makeOwnDeviceState (song, Reverb::typeID));
    REQUIRE (second.isValid());
    CHECK (phazite::getTrackName (song, second) == juce::String ("Reverb 2"));

    auto plugin = phazite::makeDeviceState (song, phazite::makeDeviceType (phazite::test::TestPlugin::getDescription()));
    plugin.setProperty (schema::deviceMix, 0.5f, nullptr);
    const auto pluginReturn = phazite::addReturnWithEffect (song, mix.b, plugin);
    REQUIRE (pluginReturn.isValid());
    CHECK (phazite::getTrackName (song, pluginReturn) == juce::String ("Test Effect"));
    CHECK (phazite::getMix (song, phazite::getDeviceID (plugin)) == doctest::Approx (1.0f));

    // Instruments don't go on returns.
    CHECK_FALSE (phazite::addReturnWithEffect (song, mix.a, phazite::makeOwnDeviceState (song, Synth::typeID)).isValid());
}

TEST_CASE ("A plug-in's mix is the song's, which what plays it plays, and its key controls are its own parameters")
{
    phazite::test::TestPlugin testPlugin;
    Mix mix;
    auto& song = *mix.song;
    const auto plugin = phazite::test::TestPlugin::add (song, mix.a);
    REQUIRE (phazite::hasMix (song, plugin));

    // All effect to start with.
    CHECK (phazite::getMix (song, plugin) == doctest::Approx (1.0f));

    const auto playedMix = [&mix, plugin]
    {
        auto* device = mix.findPlayedDevice (plugin);
        REQUIRE (device != nullptr);
        const auto index = phazite::devices::findParameter (device->getDeviceInfo(), phazite::devices::mixParameterID);
        REQUIRE (index >= 0);
        return device->getValue (index);
    };

    phazite::setMix (song, plugin, 0.25f);
    CHECK (phazite::getMix (song, plugin) == doctest::Approx (0.25f));
    CHECK ((float) phazite::findDeviceState (song, plugin)[schema::deviceMix] == doctest::Approx (0.25f));
    CHECK (playedMix() == doctest::Approx (0.25f));

    // Its key controls are the plug-in's own parameters, by its own IDs.
    CHECK (phazite::getKeyControls (song, plugin, 2)
           == juce::StringArray { phazite::test::TestPlugin::gainID, phazite::test::TestPlugin::toneID });

    // An instrument has no mix.
    const auto synth = phazite::setInstrument (song, mix.a, phazite::makeOwnDeviceState (song, Synth::typeID));
    REQUIRE (synth.isValid());
    CHECK_FALSE (phazite::hasMix (song, synth));
    CHECK (phazite::getMix (song, synth) == doctest::Approx (1.0f));
}

TEST_CASE ("New return tracks are numbered in turn")
{
    const auto song = phazite::createSong();
    phazite::Selection selection;
    selection.setSong (song.get());

    const auto first = phazite::addNewReturnTrack (*song, &selection);
    REQUIRE (first.isValid());
    CHECK (phazite::getTrackName (*song, first) == juce::String ("Return 1"));
    CHECK (selection.isSelected (first));

    // Without a selection, the selection stays as it was.
    const auto second = phazite::addNewReturnTrack (*song, nullptr);
    REQUIRE (second.isValid());
    CHECK (phazite::getTrackName (*song, second) == juce::String ("Return 2"));
    CHECK (selection.isSelected (first));
    CHECK_FALSE (selection.isSelected (second));
    CHECK (phazite::getReturnTracks (*song) == std::vector { first, second });
    selection.deselectAll();
}

TEST_CASE ("The device menus' choices are undo steps named after the device")
{
    Mix mix;
    auto& song = *mix.song;
    auto& undo = song.getUndoManager();
    const auto types = phazite::getDeviceTypes (phazite::getPluginHost (phazite::test::getEngineContext()));
    const auto find = [&types] (const char* type)
    {
        for (const auto& each : types)
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return phazite::DeviceType {};
    };

    REQUIRE (phazite::chooseInstrument (song, mix.a, find ("phaziteSynth")).isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Add Synth"));
    REQUIRE (phazite::chooseInstrument (song, mix.a, find ("phaziteDrums")).isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Replace Instrument with Drum Sampler"));

    REQUIRE (phazite::addEffect (song, mix.a, find ("phaziteEq")).isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Add EQ"));
    REQUIRE (phazite::addEffect (song, {}, find ("phaziteLimiter")).isValid());
    CHECK (phazite::getDeviceName (song, phazite::getEffects (song, {}).back()) == juce::String ("Limiter"));

    const auto reverbReturn = phazite::addReturnWith (song, mix.b, find ("phaziteReverb"));
    REQUIRE (reverbReturn.isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Add Reverb Return"));
    REQUIRE (phazite::getEffects (song, reverbReturn).size() == 1);
    CHECK (phazite::getMix (song, phazite::getEffects (song, reverbReturn).front()) == doctest::Approx (1.0f));
}

TEST_CASE ("The mixer strip's edits are undo steps of their own, and a mix drag is one")
{
    Mix mix;
    auto& song = *mix.song;
    auto& undo = song.getUndoManager();
    const auto eq = mix.add (mix.a, Equalizer::typeID);
    const auto compressor = mix.add (mix.a, Compressor::typeID);
    REQUIRE (eq.isValid());
    REQUIRE (compressor.isValid());
    undo.clearUndoHistory();

    phazite::setDeviceOn (song, eq, false);
    CHECK_FALSE (phazite::isDeviceOn (song, eq));
    CHECK (undo.getUndoDescription() == juce::String ("Turn Device Off"));

    phazite::moveDevice (song, compressor, 0);
    CHECK (phazite::getEffects (song, mix.a).front() == compressor);
    CHECK (undo.getUndoDescription() == juce::String ("Move Device"));

    // A drag of the mix knob is one step from its first move to its last; a key press is a step of its own.
    {
        auto handle = phazite::makeMixHandle (song, eq);
        handle.beginGesture();
        handle.setValue (0.8f);
        handle.setValue (0.5f);
        handle.endGesture();
        CHECK (handle.getValue() == doctest::Approx (0.5f));
        CHECK (undo.getUndoDescription() == juce::String ("Change Mix"));
        phazite::undo (song);
        CHECK (phazite::getMix (song, eq) == doctest::Approx (1.0f));

        handle.setValue (0.25f);
        CHECK (undo.getUndoDescription() == juce::String ("Change Mix"));
    }

    phazite::removeDevice (song, compressor);
    CHECK (undo.getUndoDescription() == juce::String ("Remove Compressor"));

    // Sends.
    const auto reverbReturn = phazite::addReturnTrack (song, "Reverb", nullptr);
    REQUIRE (reverbReturn.isValid());
    const auto send = phazite::addSendTo (song, mix.a, reverbReturn);
    REQUIRE (send.isValid());
    CHECK (undo.getUndoDescription() == juce::String ("Add Send"));
    CHECK_FALSE (phazite::isPreFader (song, send));

    phazite::setPreFader (song, send, true);
    CHECK (phazite::isPreFader (song, send));
    CHECK (undo.getUndoDescription() == juce::String ("Change Send"));

    phazite::removeSend (song, send);
    CHECK (phazite::getSends (song, mix.a).empty());
    CHECK (undo.getUndoDescription() == juce::String ("Remove Send"));

    const auto returnsBefore = phazite::getReturnTracks (song).size();
    REQUIRE (phazite::addSendToNewReturn (song, mix.b).isValid());
    CHECK (phazite::getReturnTracks (song).size() == returnsBefore + 1);
    CHECK (undo.getUndoDescription() == juce::String ("Add Send"));

    // The bus switch, on a folder that starts as a bus.
    const auto folder = phazite::groupIntoFolder (song, { mix.b }, nullptr);
    REQUIRE (folder.isValid());
    phazite::setMixedAsBus (song, folder, false);
    CHECK (undo.getUndoDescription() == juce::String ("Make Folder Plain"));
    phazite::setMixedAsBus (song, folder, true);
    CHECK (undo.getUndoDescription() == juce::String ("Mix Folder as Bus"));
}
