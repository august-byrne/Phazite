// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer as the UI reaches it, by IDs and plain values (src/model/mixer/MixerModel.h).
#include "EnginePlayback.h"
#include "HostedDevice.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/synth/Synth.h"
#include "engine/nodes/Channel.h"
#include "engine/nodes/DeviceNode.h"
#include "engine/nodes/Strip.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <iterator>

namespace
{
// An open project with two tracks, A and B, as the app has it.
struct TwoTracks
{
    TwoTracks()
    {
        const auto tracks = phazite::ensureTracks (*song, 2);
        REQUIRE (tracks.size() == 2);
        a = tracks[0];
        b = tracks[1];
        phazite::setTrackName (*song, a, "A");
        phazite::setTrackName (*song, b, "B");
        setSong (context, song.get());
    }

    ~TwoTracks() { setSong (context, nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    // What Phazite's engine plays of a device, and of a channel's fader (a track's by its ID, the master's by none), once
    // the song's session has given it the song as it is now; nullptr for what it doesn't play.
    phazite::engine::DeviceNode* findPlayedDevice (phazite::DeviceID device)
    {
        return getUpdatedSession().getPlayer().findDevice ((std::int64_t) device.getRaw());
    }

    phazite::engine::Channel* findPlayedChannel (phazite::TrackID channel)
    {
        auto* strip = getUpdatedSession().getPlayer().findStrip ((std::int64_t) channel.getRaw());
        return strip != nullptr ? &strip->getChannel() : nullptr;
    }

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

std::vector<juce::String> namesOf (const std::vector<phazite::DeviceSlot>& slots)
{
    std::vector<juce::String> names;

    for (const auto& slot : slots)
        names.push_back (slot.name);

    return names;
}
} // namespace

TEST_CASE ("The mixer's channels are the playlist's tracks, then the returns, each with what it is and where it goes")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto folder = phazite::groupIntoFolder (song, { tracks.b }, nullptr); // a bus, as every new folder is
    REQUIRE (folder.isValid());
    phazite::setTrackName (song, folder, "Drums");
    const auto reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
    REQUIRE (reverb.isValid());
    phazite::moveTracksAfter (song, { reverb }, {}); // first in the playlist, but last in the mixer

    const auto channels = phazite::getMixerChannels (context);
    CHECK (channels.tracks == std::vector { tracks.a, folder, tracks.b });
    CHECK (channels.returns == std::vector { reverb });

    // A track goes into the bus of the folder it is in, and the bus to the master.
    const auto track = phazite::getChannel (context, tracks.b);
    REQUIRE (track.has_value());
    CHECK (track->name == juce::String ("B"));
    CHECK (track->kind == phazite::ChannelKind::track);
    CHECK (track->outputBus == juce::String ("Drums"));
    const auto rows = phazite::getTrackIDs (song);
    const auto row = (int) std::distance (rows.begin(), std::find (rows.begin(), rows.end(), tracks.b));
    CHECK (track->colour == phazite::getTrackColour (song, tracks.b, row));
    CHECK (phazite::getChannel (context, folder)->kind == phazite::ChannelKind::bus);
    CHECK (phazite::getChannel (context, folder)->outputBus.isEmpty());
    CHECK (phazite::feeds (context, tracks.b, folder));
    CHECK_FALSE (phazite::feeds (context, tracks.a, folder));

    // A plain folder only scales its tracks' volumes: it has no sound of its own for them to go into.
    phazite::setMixedAsBus (context, folder, false);
    CHECK (phazite::getChannel (context, folder)->kind == phazite::ChannelKind::plainFolder);
    CHECK (phazite::getChannel (context, tracks.b)->outputBus.isEmpty());
    CHECK_FALSE (phazite::feeds (context, tracks.b, folder));
    CHECK_FALSE (phazite::getPan (context, folder).isValid());
    CHECK (phazite::getVolume (context, folder).isValid()); // its VCA

    // A return says which tracks send to it.
    CHECK (phazite::getChannel (context, reverb)->kind == phazite::ChannelKind::returnTrack);
    CHECK (phazite::getChannel (context, reverb)->senders.isEmpty());
    CHECK (phazite::getReturns (context).size() == 1);
    phazite::addSendTo (context, tracks.a, reverb);
    CHECK (phazite::getChannel (context, reverb)->senders == juce::StringArray { "A" });
    CHECK (phazite::feeds (context, tracks.a, reverb));
    CHECK_FALSE (phazite::canHoldInstrument (context, reverb));
    CHECK (phazite::canHoldInstrument (context, tracks.a));
}

TEST_CASE ("Mute and solo by ID, and the channel the inspector shows")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;

    phazite::toggleMute (context, tracks.a);
    CHECK (phazite::isTrackMuted (song, tracks.a, false));
    CHECK (phazite::getMuteSolo (context, tracks.a)->muted);
    phazite::toggleMute (context, tracks.a);
    CHECK_FALSE (phazite::getMuteSolo (context, tracks.a)->muted);

    // A track that others' solos keep quiet.
    phazite::toggleSolo (context, tracks.b);
    CHECK (phazite::getMuteSolo (context, tracks.a)->silencedBySolo);
    CHECK (phazite::getMuteSolo (context, tracks.b)->soloed);

    phazite::toggleSoloAlone (context, tracks.a);
    CHECK (phazite::isTrackSoloed (song, tracks.a));
    CHECK_FALSE (phazite::isTrackSoloed (song, tracks.b));

    // The selected track; with none, the first.
    CHECK (phazite::getInspectedChannel (context) == tracks.a);
    context.getSongSelection().selectOnly (tracks.b);
    CHECK (phazite::getInspectedChannel (context) == tracks.b);
    context.getSongSelection().clear();
}

TEST_CASE ("A channel's devices by ID: its instrument, its effects in their order, switched, moved and removed")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto a = tracks.a;

    CHECK_FALSE (phazite::getInstrument (context, a).has_value());
    phazite::chooseInstrument (context, a, tracks.find ("phaziteSynth"));
    phazite::addEffect (context, a, tracks.find ("phaziteEq"));
    phazite::addEffect (context, a, tracks.find ("phaziteCompressor"));
    phazite::addEffect (context, {}, tracks.find ("phaziteLimiter")); // the master's

    const auto instrument = phazite::getInstrument (context, a);
    REQUIRE (instrument.has_value());
    CHECK (instrument->name == juce::String ("Synth"));
    CHECK_FALSE (instrument->hasMix);
    CHECK (namesOf (phazite::getEffects (context, a)) == std::vector<juce::String> { "EQ", "Compressor" });
    CHECK (namesOf (phazite::getEffects (context, {})).back() == juce::String ("Limiter"));

    // Each effect has a mix; Phazite's own devices have no key controls, which only a plug-in's card shows.
    const auto eq = phazite::getEffects (context, a).front();
    CHECK (eq.hasMix);
    CHECK (eq.isOn);
    CHECK (phazite::getMix (context, eq.id).isValid());
    CHECK (phazite::getKeyControls (context, eq.id, 2).empty());

    phazite::setDeviceOn (context, eq.id, false);
    CHECK_FALSE (phazite::getEffects (context, a).front().isOn);

    phazite::moveEffect (context, eq.id, 1);
    CHECK (namesOf (phazite::getEffects (context, a)) == std::vector<juce::String> { "Compressor", "EQ" });
    phazite::moveEffect (context, eq.id, 1); // already last
    CHECK (namesOf (phazite::getEffects (context, a)) == std::vector<juce::String> { "Compressor", "EQ" });
    phazite::moveEffect (context, eq.id, -1);
    CHECK (namesOf (phazite::getEffects (context, a)) == std::vector<juce::String> { "EQ", "Compressor" });
    CHECK (phazite::getInstrument (context, a)->id == instrument->id); // effects move after the instrument only

    phazite::removeDevice (context, eq.id);
    CHECK (namesOf (phazite::getEffects (context, a)) == std::vector<juce::String> { "Compressor" });
    CHECK_FALSE (phazite::getMix (context, eq.id).isValid()); // a device that is gone

    // An instrument dropped on a track that has one replaces it; dropped on the master, an effect goes there.
    CHECK (phazite::addDeviceTo (context, a, tracks.find ("phaziteDrums")));
    CHECK (phazite::getInstrument (context, a)->name == juce::String ("Drum Sampler"));
    CHECK (phazite::addDeviceTo (context, {}, tracks.find ("phaziteReverb")));
    CHECK (namesOf (phazite::getEffects (context, {})).back() == juce::String ("Reverb"));
}

TEST_CASE ("A channel's devices are the song's, in order and on or off, which what plays follows, undo and redo too")
{
    namespace schema = phazite::schema;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto a = tracks.a;
    phazite::chooseInstrument (context, a, tracks.find ("phaziteSynth"));
    phazite::addEffect (context, a, tracks.find ("phaziteEq"));
    phazite::addEffect (context, a, tracks.find ("phaziteCompressor"));
    phazite::addEffect (context, {}, tracks.find ("phaziteLimiter"));

    // The song's: a track's element holds its devices in order, the instrument first, by their IDs and types; the master's
    // element its own.
    const auto typesIn = [] (const juce::ValueTree& chain)
    {
        juce::StringArray types;

        for (const auto& state : phazite::getDeviceStates (chain))
            types.add (state[schema::deviceType].toString());

        return types.joinIntoString (" ");
    };

    const auto chain = phazite::findTrackState (song, a);
    CHECK (typesIn (chain) == "phaziteSynth phaziteEq phaziteCompressor");
    CHECK (typesIn (phazite::findChainState (song, {})) == "phaziteLimiter");
    const auto effects = phazite::getEffects (context, a);
    REQUIRE (effects.size() == 2);
    CHECK (phazite::getDeviceID (phazite::getDeviceStates (chain)[1]) == effects[0].id);

    // What plays each device: on or off as the song has it, and none for a device the song hasn't any more.
    const auto isPlayedOn = [&tracks] (phazite::DeviceID device)
    {
        auto* node = tracks.findPlayedDevice (device);
        REQUIRE (node != nullptr);
        return node->isOn();
    };

    // Switched off, moved and removed in the song, which what plays follows; undo and redo put both back.
    const auto eq = effects[0].id, compressor = effects[1].id;
    CHECK (isPlayedOn (eq));
    phazite::setDeviceOn (context, eq, false);
    CHECK ((bool) phazite::findDeviceState (song, eq)[schema::deviceOff]);
    CHECK_FALSE (isPlayedOn (eq));
    phazite::moveEffect (context, eq, 1);
    CHECK (typesIn (chain) == "phaziteSynth phaziteCompressor phaziteEq");
    phazite::removeDevice (context, compressor);
    CHECK (typesIn (chain) == "phaziteSynth phaziteEq");
    CHECK (tracks.findPlayedDevice (compressor) == nullptr);

    phazite::undo (song);
    CHECK (typesIn (chain) == "phaziteSynth phaziteCompressor phaziteEq");
    CHECK (isPlayedOn (compressor));
    phazite::undo (song);
    phazite::undo (song);
    CHECK (typesIn (chain) == "phaziteSynth phaziteEq phaziteCompressor");
    CHECK_FALSE (phazite::findDeviceState (song, eq).hasProperty (schema::deviceOff));
    CHECK (isPlayedOn (eq));
    phazite::redo (song);
    CHECK ((bool) phazite::findDeviceState (song, eq)[schema::deviceOff]);
    CHECK_FALSE (isPlayedOn (eq));
}

TEST_CASE ("A channel's CPU meter shows the share of each block's time its devices take as Phazite's engine plays them")
{
    using phazite::test::TestBusy;
    TwoTracks tracks;
    auto& context = tracks.context;

    // Two devices that each take a fifth of the time, one of them in a container: two fifths of A's, once it has played a
    // while, nothing of B's, which has none, and nothing before anything plays.
    const auto first = phazite::test::addTestDevice (*tracks.song, tracks.a, TestBusy::typeID);
    const auto second = phazite::test::addTestDevice (*tracks.song, tracks.a, TestBusy::typeID);
    REQUIRE (phazite::groupIntoContainer (context, { second }).isValid());
    CHECK (phazite::getCpuLoad (context, tracks.a) == 0.0f);

    phazite::test::EnginePlayback playback;
    playback.play (1.5);
    const auto both = phazite::getCpuLoad (context, tracks.a);
    CHECK (both > 1.5 * TestBusy::share);
    CHECK (both < 3.0 * TestBusy::share); // the time the engine itself took for them, or the test thread waited
    CHECK (phazite::getCpuLoad (context, tracks.b) == 0.0f);

    // Switched off, a device takes none, and its share falls away as the engine plays on.
    phazite::setDeviceOn (context, first, false);
    playback.play (2.0);
    const auto one = phazite::getCpuLoad (context, tracks.a);
    CHECK (one > 0.75 * TestBusy::share);
    CHECK (one < 1.5 * TestBusy::share);
}

TEST_CASE ("The values set by hand of Phazite's own devices are the song's, which what plays takes in, undo and redo too")
{
    namespace schema = phazite::schema;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    phazite::chooseInstrument (context, tracks.a, tracks.find ("phaziteSynth"));
    const auto synth = phazite::getInstrument (context, tracks.a)->id;
    auto cutoff = phazite::getParameter (context, phazite::ParameterRef { synth, "cutoff" });
    REQUIRE (cutoff.exists());
    const auto index = phazite::devices::findParameter (phazite::devices::Synth::info, "cutoff");
    REQUIRE (index >= 0);

    // A value set by hand goes into the song, and what plays the device plays it; an undo takes both back.
    const auto values = [&song, synth] { return phazite::findDeviceState (song, synth).getChildWithName (schema::VALUES); };
    const auto played = [&tracks, synth, index]
    {
        auto* device = tracks.findPlayedDevice (synth);
        REQUIRE (device != nullptr);
        return device->getValue (index);
    };

    const auto before = played();
    cutoff.setValue (2000.0f);
    CHECK ((float) values()[juce::Identifier ("cutoff")] == doctest::Approx (2000.0f));
    CHECK (played() == doctest::Approx (2000.0f));
    phazite::undo (song);
    CHECK_FALSE (values().hasProperty ("cutoff"));
    CHECK (played() == doctest::Approx (before));
    phazite::redo (song);
    CHECK (played() == doctest::Approx (2000.0f));
}

TEST_CASE ("A plug-in is the song's: which it is, its mix, and its settings as the project stores them, which an undo makes it from")
{
    namespace schema = phazite::schema;
    using phazite::test::TestPlugin;
    TestPlugin testPlugin;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    phazite::addEffect (context, tracks.a, tracks.find ("PhaziteTestEffect"));
    const auto effects = phazite::getEffects (context, tracks.a);
    REQUIRE (effects.size() == 1);
    const auto id = effects[0].id;
    const auto element = phazite::findDeviceState (song, id);

    // Which plug-in it is, as scanning found it.
    CHECK (element[schema::deviceType].toString() == "plugin");
    CHECK (element[schema::pluginFormat].toString() == "Test");
    CHECK (element[schema::pluginName].toString() == "Test Effect");
    CHECK (element[schema::pluginMaker].toString() == "Phazite Tests");
    CHECK (element[schema::pluginIdentifier].toString() == "PhaziteTestEffect");
    CHECK ((int) element[schema::pluginUid] == TestPlugin::getDescription().uniqueId);

    // Its mix is the song's, which what plays it plays; an undo takes it back.
    const auto playedMix = [&tracks, id]
    {
        auto* device = tracks.findPlayedDevice (id);
        REQUIRE (device != nullptr);
        const auto mix = phazite::devices::findParameter (device->getDeviceInfo(), phazite::devices::mixParameterID);
        REQUIRE (mix >= 0);
        return device->getValue (mix);
    };

    auto mix = phazite::getMix (context, id);
    mix.setValue (0.25f);
    CHECK ((float) element[schema::deviceMix] == doctest::Approx (0.25f));
    CHECK (playedMix() == doctest::Approx (0.25f));
    phazite::undo (song);
    CHECK_FALSE (element.hasProperty (schema::deviceMix));
    CHECK (playedMix() == doctest::Approx (1.0f));
    phazite::redo (song);

    // Its settings, as its window sets them, go into the song as the project stores them.
    TestPlugin::setGain (song, id, 0.3f);
    song.storeDeviceStates();
    const auto stored = element[schema::pluginState].toString();
    CHECK (stored.isNotEmpty());

    // Taken away, it keeps in the song what it held then and plays no more; an undo makes it again from the song, with
    // those settings and its mix.
    TestPlugin::setGain (song, id, 0.6f);
    phazite::removeDevice (context, id);
    CHECK (element[schema::pluginState].toString() != stored);
    CHECK (tracks.findPlayedDevice (id) == nullptr);
    phazite::undo (song);
    CHECK (TestPlugin::getGain (song, id) == doctest::Approx (0.6f));
    CHECK (playedMix() == doctest::Approx (0.25f));
}

TEST_CASE ("A channel's volume and pan are the song's, which its fader plays; the master's starts at -3 dB")
{
    namespace schema = phazite::schema;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto a = tracks.a;
    const auto track = phazite::findTrackState (song, a);
    const auto master = phazite::findChainState (song, {});
    const auto played = [&tracks] (phazite::TrackID channel)
    {
        auto* fader = tracks.findPlayedChannel (channel);
        REQUIRE (fader != nullptr);
        return std::pair { fader->getVolume(), fader->getPan() };
    };

    // Nothing in the song for a new track's 0 dB at the centre, nor for a new song's master at -3 dB, which it plays.
    CHECK_FALSE (track.hasProperty (schema::channelVolume));
    CHECK_FALSE (track.hasProperty (schema::channelPan));
    CHECK_FALSE (master.hasProperty (schema::channelVolume));
    CHECK (phazite::getChannelVolume (song, {}) == doctest::Approx (-3.0f));
    CHECK (phazite::faderPositionToDecibels (phazite::getMasterVolume (context).getValue()) == doctest::Approx (-3.0f).epsilon (0.01));
    CHECK (played ({}).first == doctest::Approx (-3.0f));
    CHECK (std::abs (played (a).first) < 0.01f);

    // Set by the faders' handles, as the strips set them: the song's, which its faders play; each an undo step.
    phazite::getVolume (context, a).setValue (phazite::decibelsToFaderPosition (-6.0f));
    phazite::getPan (context, a).setValue (-0.5f);
    phazite::getMasterVolume (context).setValue (phazite::decibelsToFaderPosition (0.0f));
    CHECK ((double) track[schema::channelVolume] == doctest::Approx (-6.0));
    CHECK ((double) track[schema::channelPan] == doctest::Approx (-0.5));
    CHECK (std::abs ((double) master[schema::channelVolume]) < 1.0e-4);
    CHECK (played (a).first == doctest::Approx (-6.0f).epsilon (0.01));
    CHECK (played (a).second == doctest::Approx (-0.5f));
    CHECK (std::abs (played ({}).first) < 0.01f);

    phazite::undo (song);
    phazite::undo (song);
    phazite::undo (song);
    CHECK_FALSE (track.hasProperty (schema::channelVolume));
    CHECK_FALSE (master.hasProperty (schema::channelVolume));
    CHECK (std::abs (played (a).first) < 0.01f);
    CHECK (played ({}).first == doctest::Approx (-3.0f));
    phazite::redo (song);
    CHECK (played (a).first == doctest::Approx (-6.0f).epsilon (0.01));
}

TEST_CASE ("A send is the song's, to a return by its track, with its level and its place before or after the fader")
{
    namespace schema = phazite::schema;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto a = tracks.a;
    const auto reverb = phazite::addReturnWith (context, a, tracks.find ("phaziteReverb"));
    const auto track = phazite::findTrackState (song, a);
    const auto element = track.getChildWithName (schema::SEND);
    const auto sends = phazite::getSends (context, a);

    // The song's: to the return, by its track, at -6 dB, after the fader.
    REQUIRE (element.isValid());
    REQUIRE (sends.size() == 1);
    CHECK ((juce::int64) element[schema::sendID] == (juce::int64) sends[0].id.getRaw());
    CHECK ((juce::int64) element[schema::sendTo] == (juce::int64) reverb.getRaw());
    CHECK ((double) element[schema::sendLevel] == doctest::Approx (-6.0));
    CHECK_FALSE (element.hasProperty (schema::sendPreFader));
    CHECK (phazite::getSendTarget (song, sends[0].id) == reverb);
    REQUIRE (phazite::getSendTargets (context, a).size() == 1);
    CHECK (phazite::getSendTargets (context, a)[0].send.has_value());

    // Its level and its place before the fader are the song's; an undo takes the place back.
    phazite::getSendLevel (context, sends[0].id).setValue (phazite::decibelsToFaderPosition (-12.0f));
    CHECK ((double) element[schema::sendLevel] == doctest::Approx (-12.0));
    CHECK (phazite::getSendLevel (song, sends[0].id) == doctest::Approx (-12.0f).epsilon (0.01));
    CHECK_FALSE (phazite::isPreFader (song, sends[0].id));
    phazite::setPreFader (context, sends[0].id, true);
    CHECK ((bool) element[schema::sendPreFader]);
    CHECK (phazite::isPreFader (song, sends[0].id));
    phazite::undo (song);
    CHECK_FALSE (element.hasProperty (schema::sendPreFader));
    CHECK_FALSE (phazite::isPreFader (song, sends[0].id));

    // Removed, the song's goes.
    phazite::removeSend (context, phazite::getSends (context, a)[0].id);
    CHECK_FALSE (track.getChildWithName (schema::SEND).isValid());
    CHECK (phazite::getSends (song, a).empty());
}

TEST_CASE ("A device's channel, its handle for an editor, and whether its controls open in a window of their own")
{
    phazite::test::TestPlugin testPlugin;
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto a = tracks.a;
    phazite::addEffect (context, a, tracks.find ("phaziteEq"));
    phazite::addEffect (context, a, tracks.find ("phaziteCompressor"));
    phazite::addEffect (context, {}, tracks.find ("phaziteLimiter")); // the master's
    const auto eq = phazite::getEffects (context, a)[0];
    const auto compressor = phazite::getEffects (context, a)[1];
    const auto limiter = phazite::getEffects (context, {}).back();

    // Phazite's own devices show whole in the Devices view, through their handles, and in a window of their own only when
    // the user pops one out.
    CHECK_FALSE (eq.opensWindow);
    CHECK (eq.popsOut);
    CHECK_FALSE (eq.windowOpen);
    CHECK (phazite::getDeviceHandle (context, eq.id).isValid());
    CHECK (phazite::getDeviceChannel (context, eq.id) == std::optional (a));
    CHECK (phazite::getDeviceChannel (context, limiter.id) == std::optional (phazite::TrackID()));

    // A container's devices are on its channel; a container shows whole too, and has no handle of its own.
    const auto container = phazite::groupIntoContainer (context, { eq.id, compressor.id });
    REQUIRE (container.isValid());
    CHECK (phazite::getDeviceChannel (context, eq.id) == std::optional (a));
    CHECK (phazite::getDeviceChannel (context, container) == std::optional (a));
    CHECK_FALSE (phazite::getEffects (context, a).front().opensWindow);
    CHECK_FALSE (phazite::getDeviceHandle (context, container).isValid());

    // A plug-in has its controls in a window of its own.
    phazite::addEffect (context, a, tracks.find ("PhaziteTestEffect"));
    const auto effects = phazite::getEffects (context, a);
    REQUIRE (effects.size() == 2);
    CHECK (effects.back().opensWindow);
    CHECK_FALSE (phazite::getDeviceHandle (context, effects.back().id).isValid());

    // Nothing for a device that is gone.
    phazite::removeDevice (context, limiter.id);
    CHECK_FALSE (phazite::getDeviceChannel (context, limiter.id).has_value());
    CHECK_FALSE (phazite::getDeviceHandle (context, limiter.id).isValid());
}

TEST_CASE ("Sends by ID: to a return or a new one, before or after the fader, and removed")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto a = tracks.a;

    const auto reverb = phazite::addReturnWith (context, a, tracks.find ("phaziteReverb"));
    REQUIRE (reverb.isValid());
    CHECK (phazite::getChannel (context, reverb)->name == juce::String ("Reverb"));

    auto sends = phazite::getSends (context, a);
    REQUIRE (sends.size() == 1);
    CHECK (sends[0].returnName == juce::String ("Reverb"));
    CHECK_FALSE (sends[0].preFader);
    CHECK (phazite::getSendLevel (context, sends[0].id).isValid());

    phazite::setPreFader (context, sends[0].id, true);
    CHECK (phazite::getSends (context, a)[0].preFader);

    phazite::addSendToNewReturn (context, a);
    sends = phazite::getSends (context, a);
    REQUIRE (sends.size() == 2);
    CHECK (phazite::getReturns (context).size() == 2);

    phazite::removeSend (context, sends[0].id);
    CHECK (phazite::getSends (context, a).size() == 1);
}

TEST_CASE ("Mixer functions give nothing, and do nothing, for what is gone or with no project open")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto gone = tracks.b;
    phazite::deleteTracks (song, { tracks.b }, nullptr);

    CHECK_FALSE (phazite::getChannel (context, gone).has_value());
    CHECK_FALSE (phazite::getMuteSolo (context, gone).has_value());
    CHECK_FALSE (phazite::getVolume (context, gone).isValid());
    CHECK_FALSE (phazite::getLevel (context, gone).isMeasuring());
    CHECK (phazite::getEffects (context, gone).empty());
    CHECK (phazite::getSends (context, gone).empty());
    CHECK_FALSE (phazite::addDeviceTo (context, gone, tracks.find ("phaziteEq")));
    CHECK_FALSE (phazite::addReturnWith (context, gone, tracks.find ("phaziteReverb")).isValid());
    phazite::toggleMute (context, gone);
    phazite::addEffect (context, gone, tracks.find ("phaziteEq"));
    CHECK (phazite::getEffects (context, {}).size() == phazite::getEffects (song, {}).size()); // not the master's
    CHECK (phazite::getEffects (song, {}).empty());

    CHECK (phazite::getMasterVolume (context).isValid());
    // Phazite's engine measures the master's loudness, with the project open.
    const auto loudness = phazite::getLoudness (context);
    REQUIRE (loudness.has_value());
    CHECK_FALSE (loudness->integrated.has_value()); // until enough has been measured

    setSong (context, nullptr);
    CHECK (phazite::getMixerChannels (context).tracks.empty());
    CHECK_FALSE (phazite::getMasterVolume (context).isValid());
    CHECK_FALSE (phazite::getLoudness (context).has_value());
    CHECK (phazite::getEffects (context, {}).empty());
    CHECK_FALSE (phazite::getInspectedChannel (context).isValid());
}

TEST_CASE ("A volume fader's positions stand for levels in decibels")
{
    CHECK (phazite::faderPositionToDecibels (phazite::decibelsToFaderPosition (0.0f)) == doctest::Approx (0.0f).epsilon (0.001));
    CHECK (phazite::faderPositionToDecibels (phazite::decibelsToFaderPosition (-12.0f)) == doctest::Approx (-12.0f).epsilon (0.001));
    CHECK (phazite::decibelsToFaderPosition (-6.0f) < phazite::decibelsToFaderPosition (0.0f));
    CHECK (phazite::decibelsToFaderPosition (6.0f) == doctest::Approx (1.0f));
}

TEST_CASE ("Delay compensation: how long the song's sound waits, and for which devices")
{
    // Played through a device, as the app has it: devices know their latency once playback has prepared them.
    phazite::test::HostedDevice device;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& host = getPlaybackHost (phazite::test::getEngineContext());
    CHECK (phazite::getDelayCompensation (context).devices.empty());
    CHECK (phazite::getDelayCompensation (context).seconds == 0.0);

    // A limiter on B and one on the master look ahead; an EQ doesn't, nor a limiter that is off.
    phazite::addEffect (context, tracks.b, tracks.find ("phaziteLimiter"));
    phazite::addEffect (context, tracks.a, tracks.find ("phaziteEq"));
    phazite::addEffect (context, {}, tracks.find ("phaziteLimiter"));
    phazite::addEffect (context, tracks.a, tracks.find ("phaziteLimiter"));
    phazite::setDeviceOn (context, phazite::getEffects (context, tracks.a).back().id, false);

    phazite::test::settle (300);
    device.process (20);
    const auto compensation = phazite::getDelayCompensation (context);
    REQUIRE (compensation.devices.size() == 2);
    CHECK (juce::StringArray { compensation.devices[0].first, compensation.devices[1].first }
           == juce::StringArray { "Limiter on Master", "Limiter on B" });
    CHECK (compensation.devices[0].second == compensation.devices[1].second);
    CHECK (compensation.devices[0].second > 0.001);
    CHECK (compensation.devices[0].second < 0.005);
    CHECK (compensation.seconds == doctest::Approx (2.0 * compensation.devices[0].second).epsilon (0.02)); // B's, then the master's

    // Low latency turns it off; Phazite's devices that look ahead are bypassed then, so report none.
    host.setLowLatency (true);
    CHECK (phazite::getDelayCompensation (context).seconds == 0.0);
    CHECK (phazite::getDelayCompensation (context).devices.empty());
    host.setLowLatency (false);
    phazite::test::settle (300);
    device.process (20);
    CHECK (phazite::getDelayCompensation (context).seconds == doctest::Approx (compensation.seconds));
}

TEST_CASE ("A bus ducks under a track outside it as a track does; a plain folder, which has no effects, doesn't")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto bus = phazite::groupIntoFolder (song, { tracks.b }, nullptr); // a bus, as every new folder is
    REQUIRE (bus.isValid());

    // The tracks it can duck under: the one outside it, and its own, whose sound comes to it and not from it.
    const auto choices = phazite::getDuckUnderChoices (context, bus);
    REQUIRE (choices.size() == 2);
    CHECK (std::all_of (choices.begin(), choices.end(), [] (const phazite::SidechainChoice& choice) { return choice.whyNot.isEmpty(); }));

    const auto compressor = phazite::duckUnder (context, bus, tracks.a);
    REQUIRE (compressor.isValid());
    CHECK (phazite::getDeviceChannel (context, compressor) == bus);
    CHECK (phazite::getSidechainSource (context, compressor) == tracks.a);

    // What plays follows: the bus's compressor hears the track.
    auto* played = tracks.findPlayedDevice (compressor);
    REQUIRE (played != nullptr);

    // A plain folder has no effects to duck with, and offers none.
    phazite::undo (song);
    phazite::setMixedAsBus (context, bus, false);
    CHECK (phazite::getDuckUnderChoices (context, bus).empty());
    CHECK_FALSE (phazite::duckUnder (context, bus, tracks.a).isValid());
}
