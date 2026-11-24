// SPDX-License-Identifier: AGPL-3.0-or-later
// What reads the song's tracks outside the tracks' own code (roadmap step S5.20, the tracks' second piece): the selection,
// the project's contents, the DAWproject export, the stems an export writes, the frozen tracks, the drum pads' own tracks,
// the armed tracks, a track's preset, and the readers by ID they read through (model/arrangement/internal/Tracks.h). Each
// of these would go silently empty if it read tracks where the song no longer keeps them (it compiles, then finds nothing),
// so each is held here through the model alone, built by IDs (TestSong.h, TestDeviceValues.h), which holds wherever the
// song keeps its tracks; and the readers by ID for the odd tracks: the master, a return, a folder, a bus, a plain folder, a
// pad's own track, a track in a nested folder, and an ID the song hasn't.
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Selection.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace keys = phazite::schema::preset;
using phazite::DeviceID;
using phazite::TrackID;
using Kind = phazite::TrackValue::Kind;

phazite::Song& getSong (phazite::test::TestSong& s)
{
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    return *song;
}

// A plug-in that isn't installed here: the song keeps it as a file named it, and nothing plays it.
juce::PluginDescription describeMissingPlugin (const juce::String& name)
{
    juce::PluginDescription description;
    description.name = name;
    description.manufacturerName = "Elsewhere";
    description.pluginFormatName = "VST3";
    description.fileOrIdentifier = "C:/Plugins/" + name + ".vst3";
    description.uniqueId = 1234;
    return description;
}

void deleteTrack (phazite::Song& song, TrackID track)
{
    const phazite::UndoStep step (song, "Delete Track");
    phazite::deleteTracks (song, { track }, nullptr);
}

// The DAWproject's Track element of a name, anywhere in its structure; nullptr for none.
const juce::XmlElement* findTrackNamed (const juce::XmlElement& parent, const juce::String& name)
{
    for (const auto* child : parent.getChildIterator())
    {
        if (child->hasTagName ("Track") && child->getStringAttribute ("name") == name)
            return child;

        if (const auto* found = findTrackNamed (*child, name))
            return found;
    }

    return nullptr;
}

juce::String getChannelParameter (const juce::XmlElement& track, const char* type)
{
    const auto* channel = track.getChildByName ("Channel");
    REQUIRE (channel != nullptr);
    const auto* parameter = channel->getChildByName (type);
    REQUIRE (parameter != nullptr);
    return parameter->getStringAttribute ("value");
}
} // namespace

//==============================================================================
TEST_CASE ("A selected track stays selected while the song holds it, the master too, and leaves the selection as it goes")
{
    // The selection's check that what it names is still there (model/base/Selection.cpp, Selection::isThere()): a track, a
    // folder, a return, the master and a track in a nested folder; one deleted leaves it, and its Undo brings it back.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto inner = phazite::groupIntoFolder (*song, { tracks[2] }, nullptr);
    const auto outer = phazite::groupIntoFolder (*song, { inner }, nullptr);
    const auto ret = phazite::test::addReturnWithOwnEffect (*song, tracks[0], dev::Equalizer::typeID);
    const auto master = phazite::getMasterID (*song);
    REQUIRE (outer.isValid());

    phazite::Selection selection;
    selection.setSong (song.get());

    const std::vector<TrackID> all { tracks[0], tracks[1], tracks[2], inner, outer, ret, master };

    for (const auto track : all)
        selection.addToSelection (track);

    CHECK (selection.getTracks() == all);
    CHECK (selection.getNumObjectsSelected() == (int) all.size());

    // An ID the song hasn't is no track of the selection's.
    selection.addToSelection (TrackID::fromRaw (999999));
    CHECK (selection.getTracks() == all);

    deleteTrack (*song, tracks[1]);
    CHECK (selection.getTracks() == std::vector<TrackID> { tracks[0], tracks[2], inner, outer, ret, master });

    // The outer folder goes with the folder and the track in it.
    deleteTrack (*song, outer);
    CHECK (selection.getTracks() == std::vector<TrackID> { tracks[0], ret, master });

    phazite::undo (*song);
    phazite::undo (*song);
    CHECK (selection.getTracks() == all);
}

TEST_CASE ("A selected automation point of a fader counts as selected while the song holds its channel")
{
    // The selection counts the points still in their curve whose owner the song holds (Selection::getNumObjectsSelected()): a
    // track's fader, a plain folder's VCA and the master's fader (model/base/Selection.cpp: holdsCurveOwner()); none once the
    // track is gone, its curve with it, and again once its Undo brings both back.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto folder = phazite::groupIntoFolder (*song, { tracks[1] }, nullptr);
    REQUIRE (folder.isValid());

    {
        const phazite::UndoStep step (*song, "Plain Folder");
        phazite::makePlainFolder (*song, folder);
    }

    const auto master = phazite::getMasterID (*song);
    const phazite::ParameterRef volume { phazite::getFader (*song, tracks[0]), phazite::getVolumeParameterID (*song, tracks[0]) };
    const phazite::ParameterRef vca { phazite::getFader (*song, folder), phazite::getVolumeParameterID (*song, folder) };
    const phazite::ParameterRef masterPan { phazite::getFader (*song, master), phazite::getPanParameterID (*song, master) };
    REQUIRE (vca.parameter == juce::String (phazite::schema::vcaVolume));

    for (const auto& parameter : { volume, vca, masterPan })
    {
        const phazite::UndoStep step (*song, "Add Points");
        REQUIRE (phazite::addPoint (*song, parameter, { phazite::BeatPosition::fromBeats (1.0), 0.25f }) >= 0);
        REQUIRE (phazite::addPoint (*song, parameter, { phazite::BeatPosition::fromBeats (2.0), 0.75f }) >= 0);
    }

    phazite::Selection selection;
    selection.setSong (song.get());

    for (const auto& parameter : { volume, vca, masterPan })
    {
        selection.selectPoints (parameter, phazite::getPointKeys (*song, parameter));
        CHECK (selection.getNumObjectsSelected() == 2);
    }

    selection.selectPoints (volume, phazite::getPointKeys (*song, volume));
    deleteTrack (*song, tracks[0]);
    CHECK_FALSE (phazite::hasPoints (*song, volume)); // gone with its fader, for the undo to bring back
    CHECK (selection.getNumObjectsSelected() == 0);
    phazite::undo (*song);
    CHECK (selection.getNumObjectsSelected() == 2);
}

TEST_CASE ("The project's contents list each channel with its files and its plug-ins, a folder's, a return's and the master's")
{
    // ProjectContents.cpp: getChannels(), each channel by its ID and its name, top to bottom, the master last.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto lead = s.addTrack ("Lead"), keys = s.addTrack ("Keys");
    const auto bus = s.addBus ("Bus", { keys });
    const auto ret = s.addReturn (lead, dev::Equalizer::typeID);
    s.addAudioClip (lead, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Lead.wav"), 0.1), 0.0);
    s.addAudioClip (keys, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Keys.wav"), 0.1), 0.0);
    DeviceID onLead, onBus, onReturn, onMaster;

    {
        const phazite::UndoStep step (song, "Plug-ins");
        onLead = phazite::test::addPluginDevice (song, lead, describeMissingPlugin ("Shine"));
        onBus = phazite::test::addPluginDevice (song, bus, describeMissingPlugin ("Glue"));
        onReturn = phazite::test::addPluginDevice (song, ret, describeMissingPlugin ("Echo"));
        onMaster = phazite::test::addPluginDevice (song, phazite::getMasterID (song), describeMissingPlugin ("Limit"));
    }

    const auto files = phazite::getProjectAudioFiles (s.context);
    REQUIRE (files.size() == 2);
    CHECK (files[0].channels == juce::StringArray { "Lead" });
    CHECK (files[1].channels == juce::StringArray { "Keys" });

    // Top to bottom: the lead, the bus (before its track), the return, then the master.
    const auto plugins = phazite::getProjectPlugins (s.context);
    REQUIRE (plugins.size() == 4);
    CHECK (plugins[0].device == onLead);
    CHECK (plugins[0].channel == lead);
    CHECK (plugins[0].channelName == "Lead");
    CHECK (plugins[1].device == onBus);
    CHECK (plugins[1].channel == bus);
    CHECK (plugins[1].channelName == "Bus");
    CHECK (plugins[2].device == onReturn);
    CHECK (plugins[2].channel == ret);
    CHECK (plugins[3].device == onMaster);
    CHECK (plugins[3].channelName == "Master");
}

TEST_CASE ("The DAWproject export writes every track with its name, colour, kind, volume, pan, mute and solo, and the master")
{
    // DawProject.cpp: writeTracks(), writeTrack(), writeChannelOf(), writeMaster(), from the tracks' value and their
    // switches by their IDs.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto bass = s.addTrack ("Bass"), keys = s.addTrack ("Keys");
    const auto bus = s.addBus ("Drums", { keys });
    const auto ret = s.addReturn (bass, dev::Equalizer::typeID);
    s.addMidiClip (bass, 0.0, 4.0);
    s.setVolume (bass, -6.0f);
    s.setPan (bass, 0.3f);
    {
        const phazite::UndoStep step (song, "Pan");
        phazite::setChannelPan (song, {}, -0.5f); // the master's
    }

    phazite::setTrackColours (song, { bass }, juce::Colour (0xff336699));
    phazite::toggleTracksMuted (song, { keys });
    phazite::toggleTracksSoloed (song, { bass });

    const auto content = phazite::describeDawProject (song, "Test", {});
    REQUIRE (content.project != nullptr);
    const auto* structure = content.project->getChildByName ("Structure");
    REQUIRE (structure != nullptr);

    const auto* bassTrack = findTrackNamed (*structure, "Bass");
    REQUIRE (bassTrack != nullptr);
    CHECK (bassTrack->getStringAttribute ("contentType") == "notes");
    CHECK (bassTrack->getStringAttribute ("color").containsIgnoreCase ("336699"));
    CHECK (bassTrack->getChildByName ("Channel")->getStringAttribute ("solo") == "true");
    CHECK (bassTrack->getChildByName ("Channel")->getStringAttribute ("role") == "regular");
    CHECK (getChannelParameter (*bassTrack, "Mute") == "false");
    CHECK (getChannelParameter (*bassTrack, "Pan") == "0.65"); // 0.3, as the song writes it, from -1..1 to 0..1
    CHECK (getChannelParameter (*bassTrack, "Volume").getDoubleValue() == doctest::Approx (std::pow (10.0, -6.0 / 20.0)).epsilon (1.0e-6));

    // The bus, which holds its track, a submix; its track muted; the return an effect channel.
    const auto* busTrack = findTrackNamed (*structure, "Drums");
    REQUIRE (busTrack != nullptr);
    CHECK (busTrack->getStringAttribute ("contentType") == "tracks");
    CHECK (busTrack->getChildByName ("Channel")->getStringAttribute ("role") == "submix");
    const auto* keysTrack = findTrackNamed (*busTrack, "Keys");
    REQUIRE (keysTrack != nullptr);
    CHECK (getChannelParameter (*keysTrack, "Mute") == "true");
    CHECK (keysTrack->getChildByName ("Channel")->getStringAttribute ("destination") == "c" + juce::String ((juce::int64) bus.getRaw()));
    const auto* retTrack = findTrackNamed (*structure, phazite::getTrackName (song, ret));
    REQUIRE (retTrack != nullptr);
    CHECK (retTrack->getChildByName ("Channel")->getStringAttribute ("role") == "effect");

    // The master, its volume at its default (-3 dB) and its pan set.
    const auto* masterTrack = findTrackNamed (*structure, "Master");
    REQUIRE (masterTrack != nullptr);
    CHECK (getChannelParameter (*masterTrack, "Pan") == "0.25");
    CHECK (getChannelParameter (*masterTrack, "Volume").getDoubleValue()
           == doctest::Approx (std::pow (10.0, -3.0 / 20.0)).epsilon (1.0e-6));
}

TEST_CASE ("A stem export lists the channels that play something: tracks, buses with their tracks, returns sent to")
{
    // Export.cpp: getStemChannels(), hasSomethingToPlay(), each channel's kind by its ID.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto bass = s.addTrack ("Bass"), keys = s.addTrack ("Keys"), empty = s.addTrack ("Empty");
    s.addMidiClip (bass, 0.0, 4.0);
    s.addMidiClip (keys, 0.0, 4.0);
    const auto bus = s.addBus ("Bus", { keys });
    const auto plain = phazite::groupIntoFolder (song, { empty }, nullptr);
    REQUIRE (plain.isValid());

    {
        const phazite::UndoStep step (song, "Plain Folder");
        phazite::makePlainFolder (song, plain);
    }

    const auto fed = s.addReturn (bass, dev::Equalizer::typeID);
    const auto unfed = phazite::addReturnTrack (song, "Unfed", nullptr);

    const auto stems = phazite::getStemChannels (song);
    CHECK (std::find (stems.begin(), stems.end(), bass) != stems.end());
    CHECK (std::find (stems.begin(), stems.end(), bus) != stems.end());
    CHECK (std::find (stems.begin(), stems.end(), fed) != stems.end());
    CHECK (std::find (stems.begin(), stems.end(), keys) == stems.end()); // in its bus's stem
    CHECK (std::find (stems.begin(), stems.end(), plain) == stems.end());
    CHECK (std::find (stems.begin(), stems.end(), empty) == stems.end());
    CHECK (std::find (stems.begin(), stems.end(), unfed) == stems.end());
}

TEST_CASE ("A frozen track is among the song's frozen tracks, and its devices frozen, until it plays live again")
{
    // Freeze.cpp: getFrozenTracks(), isFrozenDevice(), canRender(), by the tracks' switch.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto bass = s.addTrack ("Bass"), keys = s.addTrack ("Keys");
    s.addMidiClip (bass, 0.0, 4.0);
    const auto synth = s.setInstrument (bass, dev::Synth::typeID);
    const auto bus = s.addBus ("Bus", { keys });
    CHECK (phazite::getFrozenTracks (song).empty());

    phazite::freezeFromFile (song, bass, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Frozen.wav"), 0.5));
    CHECK (phazite::getFrozenTracks (song) == std::vector { bass });
    CHECK (phazite::isTrackFrozen (song, bass));
    CHECK (phazite::isFrozenDevice (song, synth));

    // Neither a bus nor the master can be frozen.
    phazite::freezeFromFile (song, bus, phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Bus.wav"), 0.5));
    phazite::freezeFromFile (song, phazite::getMasterID (song),
                             phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Master.wav"), 0.5));
    CHECK (phazite::getFrozenTracks (song) == std::vector { bass });

    phazite::unfreeze (song, bass);
    CHECK (phazite::getFrozenTracks (song).empty());
    CHECK_FALSE (phazite::isFrozenDevice (song, synth));
}

TEST_CASE ("A drum pad's own track is found by its sampler and its pad, and its sampler's track has pads on tracks of their own")
{
    // DrumKit.cpp: findPadTrack(), getPlayedPad(), hasPadsOnOwnTracks(), from the tracks' pads (Tracks.h: getPadTracks()).
    auto song = phazite::createSong();
    const auto drums = phazite::ensureTracks (*song, 1).front();
    const auto sampler = phazite::test::setOwnInstrument (*song, drums, dev::DrumSampler::typeID);
    const auto kick = phazite::givePadItsOwnTrack (*song, sampler, 36);
    const auto snare = phazite::givePadItsOwnTrack (*song, sampler, 38);
    REQUIRE (kick.isValid());
    REQUIRE (snare.isValid());

    CHECK (phazite::findPadTrack (*song, sampler, 36) == kick);
    CHECK (phazite::findPadTrack (*song, sampler, 38) == snare);
    CHECK_FALSE (phazite::findPadTrack (*song, sampler, 40).isValid());
    CHECK (phazite::getPlayedPad (*song, kick) == std::optional (std::pair (sampler, 36)));
    CHECK_FALSE (phazite::getPlayedPad (*song, drums).has_value());
    CHECK (phazite::hasPadsOnOwnTracks (*song, drums));
    CHECK_FALSE (phazite::hasPadsOnOwnTracks (*song, kick));

    // A pad's track asked again gives the same one; brought back, it goes.
    CHECK (phazite::givePadItsOwnTrack (*song, sampler, 36) == kick);
    phazite::bringPadBack (*song, sampler, 36);
    CHECK_FALSE (phazite::findPadTrack (*song, sampler, 36).isValid());
    CHECK (phazite::getPadTracks (*song).size() == 1);
}

TEST_CASE ("An armed track records its input: armed only with an input, a track or a return alone, and listed in the song's order")
{
    // AudioRecorder.cpp and RecordingModel.cpp: hasInputs(), canRecordAudio(), getArmedTracks(), isArmed(), by the tracks'
    // kind and input by their IDs.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto first = s.firstTrack;
    const auto second = s.addTrack ("Second");
    const auto folder = phazite::groupIntoFolder (song, { second }, nullptr);
    REQUIRE (folder.isValid());

    for (const auto track : { first, second })
    {
        phazite::setTrackInput (s.context, track, "1");
        REQUIRE (phazite::getTrackInput (s.context, track) == "1");
    }

    phazite::setTrackInput (s.context, folder, "1");
    CHECK (phazite::getTrackInput (s.context, folder).isEmpty());
    CHECK_FALSE (phazite::canRecordAudio (s.context, folder));
    CHECK_FALSE (phazite::canRecordAudio (s.context, phazite::getMasterID (song)));

    phazite::setArmed (s.context, second, true);
    phazite::setArmed (s.context, first, true);
    phazite::setArmed (s.context, folder, true);
    CHECK (phazite::isArmed (s.context, first));
    CHECK (phazite::isArmed (s.context, second));
    CHECK_FALSE (phazite::isArmed (s.context, folder));
    CHECK (phazite::getArmedTracks (s.context) == std::vector { first, second });

    phazite::setInputMonitoring (s.context, first, phazite::InputMonitoring::always);
    CHECK (phazite::getInputMonitoring (s.context, first) == phazite::InputMonitoring::always);
    CHECK (phazite::getInputMonitoring (s.context, folder) == phazite::InputMonitoring::whileArmed);
}

TEST_CASE ("A track's preset keeps its name, colour, drum view, volume, pan and devices")
{
    // Presets.cpp: makeTrackPreset(), its track's fields by its ID (Tracks.h: findTrack(), getTrackView()); none of a folder,
    // a return or the master saved as a track's preset.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto drums = s.addTrack ("Drums");
    s.setInstrument (drums, dev::DrumSampler::typeID);
    s.addEffect (drums, dev::Equalizer::typeID);
    s.setVolume (drums, -4.5f);
    s.setPan (drums, -0.25f);
    phazite::setTrackColours (song, { drums }, juce::Colour (0xff996633));
    phazite::setDrumTrack (song, drums, true);
    phazite::setDrumRowOrder (song, drums, { 38, 36 });
    phazite::hideDrumRow (song, drums, 42);

    const auto preset = phazite::makeTrackPreset (song, drums)[keys::track];
    CHECK (preset[keys::name].toString() == "Drums");
    CHECK (juce::Colour::fromString (preset[keys::colour].toString()) == juce::Colour (0xff996633));
    CHECK ((bool) preset[keys::drumMode]);
    CHECK (preset[keys::drumOrder].toString().isNotEmpty());
    CHECK (preset[keys::drumHidden].toString().isNotEmpty());
    CHECK ((float) preset[keys::volume] == doctest::Approx (-4.5f));
    CHECK ((float) preset[keys::pan] == doctest::Approx (-0.25f));
    CHECK (preset[keys::instrument][keys::type].toString() == juce::String (dev::DrumSampler::typeID));
    REQUIRE (preset[keys::effects].getArray() != nullptr);
    CHECK (preset[keys::effects].getArray()->size() == 1);

    // Loaded as a new track (Presets.cpp: addTrackFromPreset()): its drum view as saved; an empty order and hidden list as
    // none, as the drum rows' writers write none for an empty list.
    const auto& host = phazite::getPluginHost (phazite::test::getEngineContext());
    TrackID loaded;

    {
        const phazite::UndoStep step (song, "Add Track");
        loaded = phazite::addTrackFromPreset (song, host, drums, phazite::makeTrackPreset (song, drums), nullptr);
    }

    REQUIRE (loaded.isValid());
    CHECK (phazite::isDrumTrack (song, loaded));
    CHECK (phazite::getDrumRowOrder (song, loaded) == std::vector<int> { 38, 36 });
    CHECK (phazite::getHiddenDrumRows (song, loaded) == std::vector<int> { 42 });

    auto emptyRows = phazite::makeTrackPreset (song, drums);
    REQUIRE (emptyRows[keys::track].getDynamicObject() != nullptr);
    emptyRows[keys::track].getDynamicObject()->setProperty (keys::drumOrder, "");
    emptyRows[keys::track].getDynamicObject()->setProperty (keys::drumHidden, "");

    {
        const phazite::UndoStep step (song, "Add Track");
        loaded = phazite::addTrackFromPreset (song, host, drums, emptyRows, nullptr);
    }

    REQUIRE (loaded.isValid());
    CHECK (phazite::isDrumTrack (song, loaded));
    CHECK_FALSE (phazite::getTrackView (song, loaded).drumOrder.has_value());
    CHECK_FALSE (phazite::getTrackView (song, loaded).drumHidden.has_value());

    CHECK (phazite::makeTrackPreset (song, phazite::getMasterID (song)).isVoid());
    CHECK (phazite::makeTrackPreset (song, TrackID::fromRaw (999999)).isVoid());
}

TEST_CASE ("A track's preset writes its drum mode the same, whether its song was opened from a file or edited live")
{
    // Presets.cpp: makeTrackPreset() writes the drum mode as the track's view holds it, a switch (true). Before the tracks'
    // readers moved, it copied the element's property as it was: true for a song edited live, the text "1" for one opened
    // from a file. Loading either gives the same track; the two are held the same here so that they cannot drift apart again.
    phazite::test::TestSong s;
    const auto drums = s.addTrack ("Drums");
    phazite::setDrumTrack (getSong (s), drums, true);
    const auto live = juce::JSON::toString (phazite::makeTrackPreset (getSong (s), drums)[keys::track][keys::drumMode], true);

    s.openSong (phazite::test::copyWholeSong (getSong (s)));
    auto& opened = getSong (s);
    TrackID openedDrums;

    for (const auto track : phazite::getTrackIDs (opened))
        if (phazite::getTrackName (opened, track) == "Drums")
            openedDrums = track;

    REQUIRE (openedDrums.isValid());
    const auto fromFile = juce::JSON::toString (phazite::makeTrackPreset (opened, openedDrums)[keys::track][keys::drumMode], true);
    CHECK (live == "true");
    CHECK (fromFile == live);
}

//==============================================================================
TEST_CASE ("The tracks' readers by ID read the master, a return, a folder, a bus, a plain folder, a pad's track and a nested track")
{
    // Tracks.h: getTrackKind(), getTrackIDsOfKind(), getTrackInput(), getTrackMonitoring(), isTrackFrozen(), getTrackPad(),
    // getPadTracks(), getTrackPlace(), getTrackView(), getChannelFader(), getChannelVCA(), isFaderOf(), getAutomatableIDs();
    // Mixer.h: findChannelOfFader(). Each for the odd tracks, and for an ID the song hasn't.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto sampler = phazite::test::setOwnInstrument (*song, tracks[0], dev::DrumSampler::typeID);
    const auto equalizer = phazite::test::addOwnDevice (*song, tracks[0], dev::Equalizer::typeID);
    const auto pad = phazite::givePadItsOwnTrack (*song, sampler, 36);
    const auto inner = phazite::groupIntoFolder (*song, { tracks[2] }, nullptr);
    const auto outer = phazite::groupIntoFolder (*song, { inner }, nullptr);
    const auto bus = phazite::groupIntoFolder (*song, { tracks[1] }, nullptr);
    const auto ret = phazite::test::addReturnWithOwnEffect (*song, tracks[0], dev::Equalizer::typeID);
    const auto master = phazite::getMasterID (*song);
    const auto none = TrackID::fromRaw (999999);
    REQUIRE (pad.isValid());
    REQUIRE (outer.isValid());

    {
        const phazite::UndoStep step (*song, "Plain Folder");
        phazite::makePlainFolder (*song, inner);
    }

    // Their kinds.
    CHECK (phazite::getTrackKind (*song, tracks[0]) == Kind::track);
    CHECK (phazite::getTrackKind (*song, pad) == Kind::track);
    CHECK (phazite::getTrackKind (*song, tracks[2]) == Kind::track);
    CHECK (phazite::getTrackKind (*song, inner) == Kind::folder);
    CHECK (phazite::getTrackKind (*song, bus) == Kind::folder);
    CHECK (phazite::getTrackKind (*song, ret) == Kind::returnTrack);
    CHECK (phazite::getTrackKind (*song, master) == Kind::master);
    CHECK_FALSE (phazite::getTrackKind (*song, none).has_value());
    CHECK_FALSE (phazite::getTrackKind (*song, TrackID()).has_value());
    std::vector<TrackID> folders;

    for (const auto track : phazite::getTrackIDs (*song))
        if (track == outer || track == inner || track == bus)
            folders.push_back (track);

    CHECK (folders.size() == 3);
    CHECK (phazite::getTrackIDsOfKind (*song, Kind::folder) == folders);
    CHECK (phazite::getTrackIDsOfKind (*song, Kind::returnTrack) == std::vector { ret });
    CHECK (phazite::getTrackIDsOfKind (*song, Kind::master).empty());

    // Places in the order: a nested track after its folders; the master and none have none.
    const auto order = phazite::getTrackIDs (*song);
    const auto placeOf = [&order] (TrackID track) { return (int) (std::find (order.begin(), order.end(), track) - order.begin()); };

    for (const auto track : order)
        CHECK (phazite::getTrackPlace (*song, track).index == placeOf (track));

    CHECK (phazite::getTrackPlace (*song, tracks[2]).index > phazite::getTrackPlace (*song, inner).index);
    CHECK (phazite::getTrackPlace (*song, inner).number == 0); // a folder has no number
    CHECK (phazite::getTrackPlace (*song, master) == phazite::TrackOrder::Place());
    CHECK (phazite::getTrackPlace (*song, none) == phazite::TrackOrder::Place());

    // Inputs and monitoring, frozen: none of the master, a folder or an ID the song hasn't.
    for (const auto track : { master, inner, none })
    {
        CHECK (phazite::getTrackInput (*song, track).isEmpty());
        CHECK (phazite::getTrackMonitoring (*song, track) == phazite::TrackValue::Monitoring::whileArmed);
        CHECK_FALSE (phazite::isTrackFrozen (*song, track));
        CHECK_FALSE (phazite::getTrackPad (*song, track).has_value());
        CHECK (phazite::getTrackView (*song, track) == phazite::TrackView());
    }

    // A pad's track.
    const auto played = phazite::getTrackPad (*song, pad);
    REQUIRE (played.has_value());
    CHECK (played->track == pad);
    CHECK (played->sampler == sampler);
    CHECK (played->pad == 36);
    REQUIRE (phazite::getPadTracks (*song).size() == 1);
    CHECK (phazite::getPadTracks (*song).front().track == pad);

    // Faders: a track's, a bus's, the master's by its flag, a plain folder's VCA; a device is none's.
    using phazite::ChannelRef;
    CHECK (phazite::findChannelOfFader (*song, phazite::getFader (*song, tracks[0])) == ChannelRef { .track = tracks[0] });
    CHECK (phazite::findChannelOfFader (*song, phazite::getFader (*song, bus)) == ChannelRef { .track = bus });
    CHECK (phazite::findChannelOfFader (*song, phazite::getFader (*song, ret)) == ChannelRef { .track = ret });
    CHECK (phazite::findChannelOfFader (*song, phazite::getFader (*song, inner)) == ChannelRef { .track = inner });
    CHECK (phazite::findChannelOfFader (*song, phazite::getFader (*song, {})) == ChannelRef { .onMaster = true });
    CHECK_FALSE (phazite::findChannelOfFader (*song, equalizer).isValid());
    CHECK_FALSE (phazite::findChannelOfFader (*song, DeviceID()).isValid());
    CHECK (phazite::getChannelVCA (*song, { .track = inner }) == phazite::getFader (*song, inner));
    CHECK_FALSE (phazite::getChannelVCA (*song, { .track = bus }).isValid());
    CHECK (phazite::getChannelFader (*song, { .track = bus }) == phazite::getFader (*song, bus));
    CHECK (phazite::getChannelFader (*song, { .track = master }) == phazite::getFader (*song, {}));
    CHECK (phazite::getChannelFader (*song, { .onMaster = true }) == phazite::getFader (*song, {}));
    CHECK_FALSE (phazite::getChannelFader (*song, { .track = inner }).isValid());
    CHECK_FALSE (phazite::getChannelFader (*song, { .track = none }).isValid());
    CHECK (phazite::isFaderOf (*song, { .onMaster = true }, phazite::getFader (*song, {})));
    CHECK (phazite::isFaderOf (*song, { .track = inner }, phazite::getFader (*song, inner)));
    CHECK_FALSE (phazite::isFaderOf (*song, { .track = bus }, phazite::getFader (*song, tracks[0])));
    CHECK_FALSE (phazite::isFaderOf (*song, { .track = none }, phazite::getFader (*song, tracks[0])));
    CHECK_FALSE (phazite::isFaderOf (*song, {}, phazite::getFader (*song, {})));

    // What a track's parameters are of: its fader, its devices, its sends, its modulators; the master's its own; a folder's
    // not its tracks'.
    const auto lfo = phazite::addModulator (*song, tracks[0], phazite::ModulatorType::lfo);
    REQUIRE (lfo.isValid());
    const auto owned = phazite::getAutomatableIDs (*song, tracks[0]);
    CHECK (owned.contains (phazite::getFader (*song, tracks[0]).getRaw()));
    CHECK (owned.contains (sampler.getRaw()));
    CHECK (owned.contains (equalizer.getRaw()));
    CHECK (owned.contains (phazite::getSends (*song, tracks[0]).front().getRaw()));
    CHECK (owned.contains (lfo.getRaw()));
    CHECK (phazite::getAutomatableIDs (*song, master) == std::set<std::uint64_t> { phazite::getFader (*song, {}).getRaw() });
    CHECK (phazite::getAutomatableIDs (*song, outer) == std::set<std::uint64_t> { phazite::getFader (*song, outer).getRaw() });
    CHECK (phazite::getAutomatableIDs (*song, none).empty());
}
