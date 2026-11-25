// SPDX-License-Identifier: AGPL-3.0-or-later
// What reads the song's devices outside the devices' own code: the
// selection, the project's files and plug-ins, the DAWproject export, presets, automation lanes and MIDI links, what a
// delete reads of plug-ins, automation's owners and parameters, and drum tracks. Each of these readers would go silently
// empty if it read devices where the song no longer keeps them (it compiles, then finds nothing), so each is held here
// through the model alone, built by IDs (TestDeviceValues.h), which holds wherever the song
// keeps its devices; and each for the odd devices a file can hold: a plug-in that isn't installed, a container without
// chains, devices on the master, a return and a folder made a bus.
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/midi/MidiLinkModel.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ProjectContents.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace keys = phazite::schema::preset;
using phazite::DeviceID;
using phazite::TrackID;

phazite::Song& getSong (phazite::test::TestSong& s)
{
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    return *song;
}

juce::String toString (std::string_view text)
{
    return juce::String (text.data(), text.size());
}

// A plug-in that isn't installed here: the song keeps it as a file named it, and nothing plays it.
juce::PluginDescription describeMissingPlugin (const juce::String& name, bool instrument = false)
{
    juce::PluginDescription description;
    description.name = name;
    description.manufacturerName = "Elsewhere";
    description.pluginFormatName = "VST3";
    description.fileOrIdentifier = "C:/Plugins/" + name + ".vst3";
    description.uniqueId = 1234;
    description.isInstrument = instrument;
    return description;
}

// A short sound file outside the project, which a device's sound copies in.
juce::File writeSound (const juce::File& file)
{
    return phazite::test::writeSound (file, 0.1);
}

void addPointTo (phazite::Song& song, const phazite::ParameterRef& parameter, double beat, float position)
{
    const phazite::UndoStep step (song, "Add Point");
    REQUIRE (phazite::addPoint (song, parameter, { phazite::BeatPosition::fromBeats (beat), position }) >= 0);
}

// The devices' presets' chains, by their names.
juce::StringArray getChainNames (const juce::var& container)
{
    juce::StringArray names;

    if (const auto* chains = container[keys::chains].getArray())
        for (const auto& chain : *chains)
            names.add (chain[keys::name].toString());

    return names;
}
} // namespace

//==============================================================================
TEST_CASE ("A selected device stays selected while the song holds it, wherever it is, and leaves the selection as it goes")
{
    // The selection's check that what it names is still there (model/base/Selection.cpp): a device on a track, on the
    // master, on a return, on a folder made a bus and in a container's chain, and the container.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto onTrack = phazite::test::addOwnDevice (*song, tracks[0], dev::Delay::typeID);
    const auto onMaster = phazite::test::addOwnDevice (*song, phazite::getMasterID (*song), dev::Equalizer::typeID);
    const auto ret = phazite::test::addReturnWithOwnEffect (*song, tracks[1], dev::Delay::typeID);
    const auto onReturn = phazite::getEffects (*song, ret).front();
    const auto folder = phazite::groupIntoFolder (*song, { tracks[1] }, nullptr);
    REQUIRE (folder.isValid());
    const auto onBus = phazite::test::addOwnDevice (*song, folder, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (*song, tracks[0]);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (*song, container).front();
    const auto inChain = phazite::test::addOwnDeviceToChain (*song, chain, dev::Delay::typeID);
    const auto missing = phazite::test::addPluginDevice (*song, tracks[1], describeMissingPlugin ("Gone"));

    phazite::Selection selection;
    selection.setSong (song.get());

    const std::vector<DeviceID> all { onTrack, onMaster, onReturn, onBus, container, inChain, missing };

    for (const auto device : all)
        selection.addToSelection (device);

    CHECK (selection.getDevices() == all);
    CHECK (selection.getNumObjectsSelected() == 7);

    // A chain is no device of the selection's, nor is a track's ID one.
    selection.addToSelection (chain);
    selection.addToSelection (DeviceID::fromRaw (tracks[0].getRaw()));
    CHECK (selection.getDevices() == all);
    CHECK (selection.getNumObjectsSelected() == 7);

    // Removed, a device leaves it; the container with its chain's device; the undo brings them back.
    phazite::removeDevice (*song, onBus);
    phazite::removeDevice (*song, container);
    CHECK (selection.getDevices() == std::vector<DeviceID> { onTrack, onMaster, onReturn, missing });
    CHECK (selection.getNumObjectsSelected() == 4);

    phazite::undo (*song);
    CHECK (selection.getDevices() == std::vector<DeviceID> { onTrack, onMaster, onReturn, container, inChain, missing });
}

TEST_CASE ("A selected automation point of a device's parameter counts as selected while the song holds the device")
{
    // The selection counts the points still in their curve whose owner the song holds (Selection::getNumObjectsSelected()):
    // a device's parameter, a container's chain's volume; none once the device is gone, its curve with it, and again once
    // its Undo brings both back.
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto equalizer = phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (*song, track);
    const auto chain = phazite::getChains (*song, container).front();

    const phazite::ParameterRef output { equalizer, toString (dev::Equalizer::info.parameters[0].id) };
    const phazite::ParameterRef volume { chain, phazite::schema::chainVolumeParameter };
    addPointTo (*song, output, 1.0, 0.25f);
    addPointTo (*song, output, 2.0, 0.75f);
    addPointTo (*song, volume, 1.0, 0.5f);

    phazite::Selection selection;
    selection.setSong (song.get());
    selection.selectPoints (output, phazite::getPointKeys (*song, output));
    CHECK (selection.getNumObjectsSelected() == 2);

    selection.selectPoints (volume, phazite::getPointKeys (*song, volume));
    CHECK (selection.getNumObjectsSelected() == 1);

    selection.selectPoints (output, phazite::getPointKeys (*song, output));
    phazite::removeDevice (*song, equalizer);
    CHECK_FALSE (phazite::hasPoints (*song, output)); // gone with it, for the undo to bring back
    CHECK (selection.getNumObjectsSelected() == 0);
    phazite::undo (*song);
    CHECK (selection.getNumObjectsSelected() == 2);
}

TEST_CASE ("A drum pad's file and a sampler's are among the project's files, wherever their devices are, so that Clean Up keeps them")
{
    // What the project's files list (ProjectContents.h: getProjectAudioFiles()) and what Clean Up and an import keep
    // (AudioClips.h: findAudioFilePaths()): the devices' sound files, a drum sampler's on a track and a sampler's in an
    // instrument container's chain on a track in a folder made a bus.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto drumsTrack = s.addTrack ("Drums"), keysTrack = s.addTrack ("Keys");
    s.addBus ("Bus", { keysTrack });
    const auto drums = s.setInstrument (drumsTrack, dev::DrumSampler::typeID);
    const auto kick = writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"));
    const auto pluck = writeSound (s.getSoundsFolder().getChildFile ("Pluck.wav"));
    DeviceID sampler;

    {
        const phazite::UndoStep step (song, "Sounds");
        REQUIRE (phazite::setPadSound (song, drums, 36, kick));
        const auto container = phazite::addInstrumentContainer (song, keysTrack);
        REQUIRE (container.isValid());
        sampler = phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), dev::Sampler::typeID);
    }

    {
        const phazite::UndoStep step (song, "Sample");
        REQUIRE (phazite::setSampleFile (song, sampler, pluck));
    }

    const auto paths = phazite::findAudioFilePaths (song);
    CHECK (paths.contains ("Audio/Kick.wav"));
    CHECK (paths.contains ("Audio/Pluck.wav"));

    const auto files = phazite::getProjectAudioFiles (s.context);
    const auto find = [&files] (const juce::String& path)
    {
        const auto found = std::find_if (files.begin(), files.end(), [&path] (const auto& file) { return file.path == path; });
        return found != files.end() ? std::optional (*found) : std::nullopt;
    };

    REQUIRE (find ("Audio/Kick.wav").has_value());
    CHECK (find ("Audio/Kick.wav")->channels.contains ("Drums"));
    REQUIRE (find ("Audio/Pluck.wav").has_value());
    CHECK (find ("Audio/Pluck.wav")->uses == 1);
}

TEST_CASE ("A plug-in is among the project's plug-ins wherever it is, one that isn't installed too")
{
    // The Project view's list (ProjectContents.h: getProjectPlugins()): in the order of their channels, the master last, and
    // on each in its order, those in containers too.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto lead = s.addTrack ("Lead");
    const auto ret = s.addReturn (lead, dev::Delay::typeID);
    DeviceID instrument, inChain, onReturn, onMaster;

    {
        const phazite::UndoStep step (song, "Plug-ins");
        instrument = phazite::test::addPluginDevice (song, lead, describeMissingPlugin ("Keys", true));
        inChain = phazite::test::addPluginDevice (song, lead, describeMissingPlugin ("Inside"));
        REQUIRE (phazite::groupIntoContainer (song, { inChain }).isValid());
        onReturn = phazite::test::addPluginDevice (song, ret, describeMissingPlugin ("Echo"));
        onMaster = phazite::test::addPluginDevice (song, phazite::getMasterID (song), describeMissingPlugin ("Limit"));
    }

    const auto plugins = phazite::getProjectPlugins (s.context);
    REQUIRE (plugins.size() == 4);
    CHECK (plugins[0].device == instrument);
    CHECK (plugins[0].name == "Keys");
    CHECK (plugins[0].maker == "Elsewhere");
    CHECK (plugins[0].format == "VST3");
    CHECK (plugins[0].isInstrument);
    CHECK (plugins[0].channel == lead);
    CHECK (plugins[1].device == inChain);
    CHECK (plugins[1].channel == lead);
    CHECK_FALSE (plugins[1].isInstrument);
    CHECK (plugins[2].device == onReturn);
    CHECK (plugins[2].channel == ret);
    CHECK (plugins[3].device == onMaster);
    CHECK (plugins[3].channelName == "Master");
}

TEST_CASE ("A track with a container, or a device another DAW can't play, exports bounced; one with a VST3 alone doesn't")
{
    // What the DAWproject export reads of a track's devices (DawProject.cpp: findWhyBounced(), writeDevices()): one of
    // Phazite's own devices, a container (one without chains too) and a sidechain's device (a compressor keyed from another
    // track) each bounce their track ("devices"); each VST3 goes into the file with what the song keeps of it and its curves'
    // points (writeAutomation(), the channel's plug-ins by their IDs), and the master's own devices are named as left out.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto own = s.addTrack ("Own"), grouped = s.addTrack ("Grouped"), plugin = s.addTrack ("Plugin"), plain = s.addTrack ("Plain");
    const auto bare = s.addTrack ("Bare"), keyed = s.addTrack ("Keyed");
    DeviceID vst3, second;

    {
        const phazite::UndoStep step (song, "Devices");
        phazite::test::addOwnDevice (song, own, dev::Delay::typeID);
        REQUIRE (phazite::addContainer (song, grouped).isValid());
        vst3 = phazite::test::addPluginDevice (song, plugin, describeMissingPlugin ("Shine"));
        second = phazite::test::addPluginDevice (song, plugin, describeMissingPlugin ("Glow"));
        phazite::test::addOwnDevice (song, phazite::getMasterID (song), dev::Equalizer::typeID);
        REQUIRE (phazite::addContainer (song, bare).isValid());
        const auto compressor = phazite::test::addOwnDevice (song, keyed, dev::Compressor::typeID);
        phazite::setSidechainSource (song, compressor, own);
        REQUIRE (phazite::getSidechainSource (song, compressor) == own);
    }

    // Both plug-ins' automation, which goes into the file by their IDs.
    for (const auto device : { vst3, second })
    {
        addPointTo (song, { device, "7" }, 0.0, 0.25f);
        addPointTo (song, { device, "7" }, 4.0, 0.75f);
    }

    // The bare track's container without its chains, as a file may hold it.
    const auto container = phazite::getDevices (song, bare).front();
    s.openSong (phazite::test::copyWholeSongWithDeviceEdited (song, container,
                                                              [] (juce::ValueTree& edited)
                                                              {
                                                                  for (int i = edited.getNumChildren(); --i >= 0;)
                                                                      if (edited.getChild (i).hasType (phazite::schema::CHAIN))
                                                                          edited.removeChild (i, nullptr);
                                                              }));
    auto& opened = getSong (s);
    REQUIRE (phazite::getChains (opened, container).empty());
    opened.storeDeviceStates();
    const auto content = phazite::describeDawProject (opened, "Test", {});

    std::vector<TrackID> bounced;

    for (const auto& [track, clip] : content.bounces)
        bounced.push_back (track);

    CHECK (bounced == std::vector<TrackID> { own, grouped, bare, keyed });
    CHECK (std::find (bounced.begin(), bounced.end(), plugin) == bounced.end());
    CHECK (std::find (bounced.begin(), bounced.end(), plain) == bounced.end());

    // Each VST3's device, named and on, what it didn't load left out, with its automation's points; the master's EQ named as
    // left out.
    const auto text = content.project->toString();
    CHECK (text.contains ("deviceName=\"Shine\""));
    CHECK (text.contains ("deviceName=\"Glow\""));
    CHECK (text.contains ("deviceVendor=\"Elsewhere\""));

    for (const auto device : { vst3, second })
    {
        const auto written = "d" + juce::String ((juce::int64) device.getRaw());
        CHECK (text.contains ("\"" + written + "\""));
        CHECK (text.contains ("\"q" + written + "p7\"")); // its curve's points (DawProject.cpp: writeAutomation())
    }

    const auto comment = content.metadata->getChildByName ("Comment");
    REQUIRE (comment != nullptr);
    INFO (comment->getAllSubText());
    CHECK (comment->getAllSubText().contains ("the master's EQ"));
    CHECK (comment->getAllSubText().contains ("Shine's settings"));
}

TEST_CASE ("A device's, a container's and a track's presets keep each device's type, values, sounds, chains, split and macros")
{
    // What a preset reads of the song's devices (Presets.cpp: describeDevice(), describeContainer(), makeTrackPreset()).
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto track = s.addTrack ("Drums");
    const auto drums = s.setInstrument (track, dev::DrumSampler::typeID);
    const auto equalizer = s.addEffect (track, dev::Equalizer::typeID);
    const auto output = toString (dev::Equalizer::info.parameters[(size_t) dev::Equalizer::outputParameter].id);
    s.setParameter (equalizer, output, -3.0f);
    const auto kick = writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"));
    DeviceID container, second, delay;

    {
        const phazite::UndoStep step (song, "Build");
        REQUIRE (phazite::setPadSound (song, drums, 36, kick));
        container = phazite::addContainer (song, track);
        REQUIRE (container.isValid());
        second = phazite::addChain (song, container);
        REQUIRE (second.isValid());
        phazite::setChainName (song, second, "Highs");
        phazite::setContainerSplit (song, container, phazite::ContainerSplit::bands);
        delay = phazite::test::addOwnDeviceToChain (song, second, dev::Delay::typeID);
    }

    const auto macros = phazite::getContainerMacros (song, container);
    REQUIRE (macros.size() >= 2);

    {
        const phazite::UndoStep step (song, "Macro");
        phazite::renameMacro (song, macros[1], "Brightness");
        phazite::setMacroValue (song, macros[1], 0.625f);
    }

    // The equalizer's: its type, every parameter, its value set by hand among them.
    const auto devicePreset = phazite::makeDevicePreset (song, equalizer);
    CHECK (devicePreset[keys::kind].toString() == keys::deviceKind);
    const auto device = devicePreset[keys::device];
    CHECK (device[keys::type].toString() == toString (dev::Equalizer::typeID));
    CHECK ((float) device[keys::parameters][juce::Identifier (output)] == doctest::Approx (-3.0f));
    CHECK (device[keys::parameters].getDynamicObject()->getProperties().size() == (int) dev::Equalizer::info.parameters.size());

    // The drum sampler's sound by its full path.
    const auto drumsPreset = phazite::makeDevicePreset (song, drums)[keys::device];
    CHECK (drumsPreset[keys::sounds][phazite::schema::getPadSoundFile (36, 0)].toString()
           == s.getProject().getFolder().getChildFile ("Audio/Kick.wav").getFullPathName());

    // The container's chains, split and devices.
    const auto containerPreset = phazite::makeDevicePreset (song, container);
    CHECK (containerPreset[keys::kind].toString() == keys::containerKind);
    const auto saved = containerPreset[keys::container];
    CHECK (saved[keys::split].toString() == phazite::schema::splitBands);
    CHECK (getChainNames (saved).size() == 2);
    CHECK (getChainNames (saved)[1] == "Highs");
    const auto* chains = saved[keys::chains].getArray();
    REQUIRE (chains != nullptr);
    const auto* inside = chains->getReference (1)[keys::devices].getArray();
    REQUIRE (inside != nullptr);
    REQUIRE (inside->size() == 1);
    CHECK (inside->getReference (0)[keys::type].toString() == toString (dev::Delay::typeID));
    CHECK (chains->getReference (1).hasProperty (keys::crossover));

    // And its macros, each by its name and value, in their order.
    const auto* savedMacros = saved[keys::macros].getArray();
    REQUIRE (savedMacros != nullptr);
    REQUIRE (savedMacros->size() == (int) macros.size());

    for (size_t i = 0; i < macros.size(); ++i)
        CHECK (savedMacros->getReference ((int) i)[keys::name].toString() == phazite::getModulatorName (song, macros[i]));

    CHECK (savedMacros->getReference (1)[keys::name].toString() == "Brightness");
    CHECK ((float) savedMacros->getReference (1)[keys::value] == doctest::Approx (0.625f));

    // The track's: its instrument, then its effects and its container, in their order.
    const auto trackPreset = phazite::makeTrackPreset (song, track)[keys::track];
    CHECK (trackPreset[keys::instrument][keys::type].toString() == toString (dev::DrumSampler::typeID));
    const auto* effects = trackPreset[keys::effects].getArray();
    REQUIRE (effects != nullptr);
    REQUIRE (effects->size() == 2);
    CHECK (effects->getReference (0)[keys::type].toString() == toString (dev::Equalizer::typeID));
    CHECK (getChainNames (effects->getReference (1)[keys::container]).size() == 2);
}

TEST_CASE ("The lanes of a track's devices' automation come in their devices' order, and each device's in its parameters' order")
{
    // Automation never hides (Automation.h: getLanes()): a parameter with points shows a lane, by its device's place on the
    // track (the instrument, an effect, a container's chain and the devices in it) and its own place among its device's
    // parameters, whatever order its points were made in.
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto synth = phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
    const auto equalizer = phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (*song, track);
    const auto chain = phazite::getChains (*song, container).front();
    const auto delay = phazite::test::addOwnDeviceToChain (*song, chain, dev::Delay::typeID);

    const auto parameterOf = [] (DeviceID device, const dev::DeviceInfo& info, size_t index)
    { return phazite::ParameterRef { device, toString (info.parameters[index].id) }; };

    const auto later = parameterOf (equalizer, dev::Equalizer::info, 3), earlier = parameterOf (equalizer, dev::Equalizer::info, 1);
    const auto inside = parameterOf (delay, dev::Delay::info, 0), synthFirst = parameterOf (synth, dev::Synth::info, 2);
    const phazite::ParameterRef chainVolume { chain, phazite::schema::chainVolumeParameter };

    for (const auto& parameter : { inside, later, chainVolume, earlier, synthFirst })
        addPointTo (*song, parameter, 1.0, 0.5f);

    std::vector<phazite::ParameterRef> shown;

    for (const auto& lane : phazite::getLanes (*song, track))
        shown.push_back (lane.parameter);

    CHECK (shown == std::vector<phazite::ParameterRef> { synthFirst, earlier, later, chainVolume, inside });
}

TEST_CASE ("A MIDI link of a device's parameter names its device and its track; a chain's its container")
{
    // The Project view's links (MidiLinkModel.h: getMidiLinks()): what a click on one selects.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto track = s.addTrack ("Lead");
    const auto synth = s.setInstrument (track, dev::Synth::typeID);
    DeviceID container, chain;

    {
        const phazite::UndoStep step (song, "Link");
        container = phazite::addContainer (song, track);
        REQUIRE (container.isValid());
        chain = phazite::getChains (song, container).front();
        phazite::linkMidiController (song, { 1, 21 }, { synth, "cutoff" });
        phazite::linkMidiController (song, { 1, 22 }, { chain, phazite::schema::chainVolumeParameter });
    }

    const auto links = phazite::getMidiLinks (s.context);
    REQUIRE (links.size() == 2);
    CHECK (links[0].device == synth);
    CHECK (links[0].track == track);
    CHECK (links[0].turns.startsWith ("Lead: "));
    CHECK (links[1].device == container);
    CHECK (links[1].track == track);
}

TEST_CASE ("Deleting a track reads its plug-ins' states first: a track's plug-in, one in a container's chain, and a folder's track's")
{
    // What Delete and Duplicate ask before they act on tracks (arrangement/Selection.cpp: doesDeleteReadPlugins()): whether
    // a track they take, or one in a folder they take, holds a plug-in, in a container's chain too.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 4);
    const auto onTrack = tracks[0], inChain = tracks[1], inFolder = tracks[2], plain = tracks[3];
    phazite::test::addPluginDevice (*song, onTrack, describeMissingPlugin ("Shine"));
    const auto grouped = phazite::test::addPluginDevice (*song, inChain, describeMissingPlugin ("Inside"));
    REQUIRE (phazite::groupIntoContainer (*song, { grouped }).isValid());
    phazite::test::addPluginDevice (*song, inFolder, describeMissingPlugin ("Deep"));
    const auto folder = phazite::groupIntoFolder (*song, { inFolder }, nullptr);
    REQUIRE (folder.isValid());
    phazite::test::addOwnDevice (*song, plain, dev::Delay::typeID);

    phazite::Selection selection;
    selection.setSong (song.get());
    const phazite::RangeSelection range { selection };

    const auto readsWith = [&] (TrackID track)
    {
        selection.selectOnly (track);
        return phazite::doesDeleteReadPlugins (selection, range);
    };

    CHECK (readsWith (onTrack));
    CHECK (readsWith (inChain));
    CHECK (readsWith (folder));
    CHECK_FALSE (readsWith (plain));
}

TEST_CASE ("A curve's owner is there while the song holds its device or its container's chain, gone with it, back by its Undo")
{
    // Whether automation finds what a curve names (Automation.cpp: isCurveOwnerThere()), as the selected points of a device's
    // parameter and of a chain's volume show it (Automation.h: getSelectedPoints()).
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto delay = phazite::test::addOwnDevice (*song, track, dev::Delay::typeID);
    const auto container = phazite::addContainer (*song, track);
    const auto chain = phazite::getChains (*song, container).front();
    const phazite::ParameterRef time { delay, toString (dev::Delay::info.parameters[0].id) };
    const phazite::ParameterRef volume { chain, phazite::schema::chainVolumeParameter };
    addPointTo (*song, time, 1.0, 0.25f);
    addPointTo (*song, volume, 1.0, 0.5f);

    phazite::Selection selection;
    selection.setSong (song.get());

    selection.selectPoints (volume, phazite::getPointKeys (*song, volume));
    CHECK (phazite::getSelectedPoints (selection).has_value());

    selection.selectPoints (time, phazite::getPointKeys (*song, time));
    CHECK (phazite::getSelectedPoints (selection).has_value());

    phazite::removeDevice (*song, delay);
    CHECK_FALSE (phazite::getSelectedPoints (selection).has_value());
    phazite::undo (*song);
    CHECK (phazite::getSelectedPoints (selection).has_value());
}

TEST_CASE ("A track's automatable parameters: its fader's, then each device's in its order, a container's chain's and its devices'")
{
    // What the automation menus offer (Automation.h: getAutomatableParameters()), in the order the code before the devices'
    // second piece gave: the fader's volume and pan; the effect's parameters but its mix, in its description's order; the
    // container none of its own; its chain's volume and pan (no crossover: it isn't split into bands); then the delay's in
    // the chain, its mix left out.
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto equalizer = phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (*song, track);
    const auto chain = phazite::getChains (*song, container).front();
    const auto delay = phazite::test::addOwnDeviceToChain (*song, chain, dev::Delay::typeID);

    const auto fader = phazite::getFader (*song, track);
    std::vector<phazite::ParameterRef> expected { { fader, phazite::getVolumeParameterID (*song, track) },
                                                  { fader, phazite::getPanParameterID (*song, track) } };

    const auto addOwn = [&expected] (DeviceID device, const dev::DeviceInfo& info)
    {
        for (const auto& parameter : info.parameters)
            if (parameter.id != dev::mixParameterID)
                expected.push_back ({ device, toString (parameter.id) });
    };

    addOwn (equalizer, dev::Equalizer::info);
    expected.push_back ({ chain, phazite::schema::chainVolumeParameter });
    expected.push_back ({ chain, phazite::schema::chainPanParameter });
    addOwn (delay, dev::Delay::info);

    CHECK (phazite::getAutomatableParameters (*song, track) == expected);
}

TEST_CASE ("A track is a drum track by its drum sampler as its instrument, and shows its pads' rows; not one inside a container")
{
    // The piano roll's drum mode and rows (Notes.h: isDrumTrack(), getSamplerSounds()): a drum sampler as a track's
    // instrument; one inside an instrument container, which the track's own devices don't hold; another instrument.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    const auto drumsTrack = s.addTrack ("Drums"), containedTrack = s.addTrack ("Contained"), keysTrack = s.addTrack ("Keys");
    const auto drums = phazite::test::setOwnInstrument (song, drumsTrack, dev::DrumSampler::typeID);
    phazite::test::setOwnInstrument (song, keysTrack, dev::Synth::typeID);
    const auto kick = writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"));
    DeviceID contained;

    {
        const phazite::UndoStep step (song, "Sounds");
        REQUIRE (phazite::setPadSound (song, drums, 36, kick));
        const auto container = phazite::addInstrumentContainer (song, containedTrack);
        REQUIRE (container.isValid());
        contained = phazite::test::addOwnDeviceToChain (song, phazite::getChains (song, container).front(), dev::DrumSampler::typeID);
        REQUIRE (phazite::setPadSound (song, contained, 38, kick));
    }

    CHECK (phazite::isDrumTrack (song, drumsTrack));
    const auto rows = phazite::getSamplerSounds (song, drumsTrack);
    REQUIRE (rows.has_value());
    CHECK (std::any_of (rows->begin(), rows->end(),
                        [] (const phazite::DrumRow& row) { return row.note == dev::DrumSampler::firstNote + 36; }));

    CHECK_FALSE (phazite::isDrumTrack (song, containedTrack));
    CHECK_FALSE (phazite::getSamplerSounds (song, containedTrack).has_value());
    CHECK_FALSE (phazite::isDrumTrack (song, keysTrack));
    CHECK_FALSE (phazite::getSamplerSounds (song, keysTrack).has_value());
}

TEST_CASE ("The readers by ID outside the devices' own code read the odd devices a file holds as the song keeps them")
{
    // Mixer.h's readers for them (getDeviceOutline(), getSongDeviceOutline(), holdsPlugin(), getOwnDeviceInfo(),
    // copyWholeDevice()) and DevicesValue.h's holdsDevice(): a plug-in that isn't installed as a track's instrument and a
    // container without chains after it; an effect on the master, on a return and on a folder made a bus, with a container
    // whose chain holds a delay.
    using phazite::DeviceValue;
    auto made = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*made, 2);
    const auto missing = phazite::test::addPluginDevice (*made, tracks[0], describeMissingPlugin ("Gone", true));
    const auto bare = phazite::addContainer (*made, tracks[0]);
    const auto onMaster = phazite::test::addOwnDevice (*made, phazite::getMasterID (*made), dev::Equalizer::typeID);
    const auto ret = phazite::test::addReturnWithOwnEffect (*made, tracks[1], dev::Delay::typeID);
    const auto onReturn = phazite::getEffects (*made, ret).front();
    const auto folder = phazite::groupIntoFolder (*made, { tracks[1] }, nullptr);
    const auto onBus = phazite::test::addOwnDevice (*made, folder, dev::Equalizer::typeID);
    const auto container = phazite::addContainer (*made, folder);
    const auto chain = phazite::getChains (*made, container).front();
    const auto inChain = phazite::test::addOwnDeviceToChain (*made, chain, dev::Delay::typeID);
    REQUIRE (bare.isValid());
    REQUIRE (folder.isValid());

    const auto song =
        phazite::makeSong (phazite::test::copyWholeSongWithDeviceEdited (*made, bare,
                                                                         [] (juce::ValueTree& edited)
                                                                         {
                                                                             for (int i = edited.getNumChildren(); --i >= 0;)
                                                                                 if (edited.getChild (i).hasType (phazite::schema::CHAIN))
                                                                                     edited.removeChild (i, nullptr);
                                                                         }));
    REQUIRE (song != nullptr);
    const auto master = phazite::getMasterID (*song);

    // The track's: the plug-in, an instrument with its identity, then the container, which holds nothing.
    const auto first = phazite::getDeviceOutline (*song, tracks[0]);
    REQUIRE (first.size() == 2);
    CHECK (first[0].id == missing);
    CHECK (first[0].form == DeviceValue::Form::plugin);
    CHECK (first[0].instrument);
    CHECK (first[0].plugin.name == "Gone");
    CHECK (first[0].plugin.format == "VST3");
    CHECK (first[0].channel == tracks[0]);
    CHECK (first[1].id == bare);
    CHECK (first[1].form == DeviceValue::Form::container);
    CHECK (phazite::holdsPlugin (*song, tracks[0]));
    CHECK_FALSE (phazite::holdsPlugin (*song, folder));
    CHECK (phazite::getOwnDeviceInfo (*song, missing) == nullptr);
    CHECK (phazite::getOwnDeviceInfo (*song, bare) == nullptr);
    CHECK_FALSE (phazite::copyWholeDevice (*song, bare).getChildWithName (phazite::schema::CHAIN).isValid()); // its macros alone

    // The bus's: its equalizer, its container, the container's chain and the delay in it, each with its place.
    const auto bus = phazite::getDeviceOutline (*song, folder);
    REQUIRE (bus.size() == 4);
    CHECK (bus[0].id == onBus);
    CHECK (bus[0].type == toString (dev::Equalizer::typeID));
    CHECK (bus[1].id == container);
    CHECK (bus[2].id == chain);
    CHECK (bus[2].form == DeviceValue::Form::chain);
    CHECK (bus[2].container == container);
    CHECK (bus[3].id == inChain);
    CHECK (bus[3].container == container);
    CHECK (bus[3].chain == chain);
    CHECK (bus[3].channel == folder);
    CHECK (phazite::getOwnDeviceInfo (*song, inChain) == &dev::Delay::info);
    CHECK_FALSE (phazite::copyWholeDevice (*song, chain).isValid()); // a chain is no device of these

    // The master's by its own ID, and the return's; the song's in its order, the master's last.
    const auto onTheMaster = phazite::getDeviceOutline (*song, master);
    REQUIRE (onTheMaster.size() == 1);
    CHECK (onTheMaster[0].id == onMaster);
    CHECK (onTheMaster[0].channel == master);
    CHECK (phazite::getDeviceOutline (*song, {}).size() == 1);
    CHECK (phazite::getDeviceOutline (*song, ret).front().id == onReturn);
    const auto all = phazite::getSongDeviceOutline (*song);
    REQUIRE (all.size() == 8);
    CHECK (all.back().id == onMaster);

    // Held: each device and container, a chain where asked for; not a track's ID nor one the song hasn't.
    for (const auto device : { missing, bare, onMaster, onReturn, onBus, container, inChain })
        CHECK (phazite::holdsDevice (*song, device));

    CHECK_FALSE (phazite::holdsDevice (*song, chain));
    CHECK (phazite::holdsDevice (*song, chain, true));
    CHECK_FALSE (phazite::holdsDevice (*song, DeviceID::fromRaw (tracks[0].getRaw()), true));
    CHECK_FALSE (phazite::holdsDevice (*song, DeviceID()));
}
