// SPDX-License-Identifier: AGPL-3.0-or-later
// The mixer as the UI reaches it, by IDs and plain values (src/model/mixer/MixerModel.h).
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
// An open project with two tracks, A and B, as the app has it.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        a->setName ("A");
        b->setName ("B");
        setEdit (context, edit.get());
    }

    ~Song() { setEdit (context, nullptr); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

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
    Song song;
    auto& context = song.context;
    auto* folder = phazite::groupIntoFolder ({ song.b }, nullptr); // a bus, as every new folder is
    REQUIRE (folder != nullptr);
    folder->setName ("Drums");
    auto* reverb = phazite::addReturnTrack (*song.edit, "Reverb", nullptr);
    REQUIRE (reverb != nullptr);
    song.edit->moveTrack (reverb, { nullptr, nullptr }); // first in the playlist, but last in the mixer

    const auto channels = phazite::getMixerChannels (context);
    CHECK (channels.tracks == std::vector { idOf (*song.a), idOf (*folder), idOf (*song.b) });
    CHECK (channels.returns == std::vector { idOf (*reverb) });

    // A track goes into the bus of the folder it is in, and the bus to the master.
    const auto track = phazite::getChannel (context, idOf (*song.b));
    REQUIRE (track.has_value());
    CHECK (track->name == juce::String ("B"));
    CHECK (track->kind == phazite::ChannelKind::track);
    CHECK (track->outputBus == juce::String ("Drums"));
    CHECK (track->colour == phazite::getTrackColour (*song.b, phazite::getPlaylistTracks (*song.edit).indexOf (song.b)));
    CHECK (phazite::getChannel (context, idOf (*folder))->kind == phazite::ChannelKind::bus);
    CHECK (phazite::getChannel (context, idOf (*folder))->outputBus.isEmpty());
    CHECK (phazite::feeds (context, idOf (*song.b), idOf (*folder)));
    CHECK_FALSE (phazite::feeds (context, idOf (*song.a), idOf (*folder)));

    // A plain folder only scales its tracks' volumes: it has no sound of its own for them to go into.
    phazite::setMixedAsBus (context, idOf (*folder), false);
    CHECK (phazite::getChannel (context, idOf (*folder))->kind == phazite::ChannelKind::plainFolder);
    CHECK (phazite::getChannel (context, idOf (*song.b))->outputBus.isEmpty());
    CHECK_FALSE (phazite::feeds (context, idOf (*song.b), idOf (*folder)));
    CHECK_FALSE (phazite::getPan (context, idOf (*folder)).isValid());
    CHECK (phazite::getVolume (context, idOf (*folder)).isValid()); // its VCA

    // A return says which tracks send to it.
    CHECK (phazite::getChannel (context, idOf (*reverb))->kind == phazite::ChannelKind::returnTrack);
    CHECK (phazite::getChannel (context, idOf (*reverb))->senders.isEmpty());
    CHECK (phazite::getReturns (context).size() == 1);
    phazite::addSendTo (context, idOf (*song.a), idOf (*reverb));
    CHECK (phazite::getChannel (context, idOf (*reverb))->senders == juce::StringArray { "A" });
    CHECK (phazite::feeds (context, idOf (*song.a), idOf (*reverb)));
    CHECK_FALSE (phazite::canHoldInstrument (context, idOf (*reverb)));
    CHECK (phazite::canHoldInstrument (context, idOf (*song.a)));
}

TEST_CASE ("Mute and solo by ID, and the channel the inspector shows")
{
    Song song;
    auto& context = song.context;

    phazite::toggleMute (context, idOf (*song.a));
    CHECK (song.a->isMuted (false));
    CHECK (phazite::getMuteSolo (context, idOf (*song.a))->muted);
    phazite::toggleMute (context, idOf (*song.a));
    CHECK_FALSE (phazite::getMuteSolo (context, idOf (*song.a))->muted);

    // A track that others' solos keep quiet.
    phazite::toggleSolo (context, idOf (*song.b));
    CHECK (phazite::getMuteSolo (context, idOf (*song.a))->silencedBySolo);
    CHECK (phazite::getMuteSolo (context, idOf (*song.b))->soloed);

    phazite::toggleSoloAlone (context, idOf (*song.a));
    CHECK (song.a->isSolo (false));
    CHECK_FALSE (song.b->isSolo (false));

    // The selected track; with none, the first.
    CHECK (phazite::getInspectedChannel (context) == idOf (*song.a));
    context.getSongSelection().selectOnly (idOf (*song.b));
    CHECK (phazite::getInspectedChannel (context) == idOf (*song.b));
    context.getSongSelection().clear();
}

TEST_CASE ("A channel's devices by ID: its instrument, its effects in their order, switched, moved and removed")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);

    CHECK_FALSE (phazite::getInstrument (context, a).has_value());
    phazite::chooseInstrument (context, a, song.find ("phaziteSynth"));
    phazite::addEffect (context, a, song.find ("phaziteEq"));
    phazite::addEffect (context, a, song.find ("phaziteCompressor"));
    phazite::addEffect (context, {}, song.find ("phaziteLimiter")); // the master's

    const auto instrument = phazite::getInstrument (context, a);
    REQUIRE (instrument.has_value());
    CHECK (instrument->name == juce::String ("Synth"));
    CHECK_FALSE (instrument->hasMix);
    CHECK (namesOf (phazite::getEffects (context, a)) == std::vector<juce::String> { "EQ", "Compressor" });
    CHECK (namesOf (phazite::getEffects (context, {})).back() == juce::String ("Limiter"));

    // Each effect has a mix, and a few key controls for its slot.
    const auto eq = phazite::getEffects (context, a).front();
    CHECK (eq.hasMix);
    CHECK (eq.isOn);
    CHECK (phazite::getMix (context, eq.id).isValid());
    CHECK (phazite::getKeyControls (context, eq.id, 2).size() == 2);

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
    CHECK (phazite::addDeviceTo (context, a, song.find ("phaziteDrums")));
    CHECK (phazite::getInstrument (context, a)->name == juce::String ("Drum Sampler"));
    CHECK (phazite::addDeviceTo (context, {}, song.find ("phaziteReverb")));
    CHECK (namesOf (phazite::getEffects (context, {})).back() == juce::String ("Reverb"));
}

TEST_CASE ("Sends by ID: to a return or a new one, before or after the fader, and removed")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);

    const auto reverb = phazite::addReturnWith (context, a, song.find ("phaziteReverb"));
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
    Song song;
    auto& context = song.context;
    const auto gone = idOf (*song.b);
    phazite::deleteTracks ({ song.b }, nullptr);

    CHECK_FALSE (phazite::getChannel (context, gone).has_value());
    CHECK_FALSE (phazite::getMuteSolo (context, gone).has_value());
    CHECK_FALSE (phazite::getVolume (context, gone).isValid());
    CHECK_FALSE (phazite::getLevel (context, gone).isMeasuring());
    CHECK (phazite::getEffects (context, gone).empty());
    CHECK (phazite::getSends (context, gone).empty());
    CHECK_FALSE (phazite::addDeviceTo (context, gone, song.find ("phaziteEq")));
    CHECK_FALSE (phazite::addReturnWith (context, gone, song.find ("phaziteReverb")).isValid());
    phazite::toggleMute (context, gone);
    phazite::addEffect (context, gone, song.find ("phaziteEq"));
    CHECK (phazite::getEffects (context, {}).size() == phazite::getMasterDevices (*song.edit).size()); // not the master's

    CHECK (phazite::getMasterVolume (context).isValid());
    CHECK_FALSE (phazite::getLoudness (context).has_value()); // no loudness meter on the master yet
    REQUIRE (phazite::ensureLoudnessMeter (*song.edit) != nullptr);
    const auto loudness = phazite::getLoudness (context);
    REQUIRE (loudness.has_value());
    CHECK_FALSE (loudness->integrated.has_value()); // until enough has been measured

    setEdit (context, nullptr);
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
