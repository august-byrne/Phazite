// SPDX-License-Identifier: AGPL-3.0-or-later
// Presets (src/model/presets/PresetModel.h; roadmap step 2.4): what a device's, a container's and a track's preset hold
// and leave out, in Phazite's own names with nothing of the song's own elements, reading what a file holds, and putting
// presets into another project as one undo step. Its "done when": a container saved as a preset loads into another
// project with its macros working. And a device's preset loaded into a device of its kind, as a plug-in window's Load
// Preset does (step 2.12) and a device's Presets menu does.
#include "EnginePlayback.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::DeviceID;
using phazite::ModulatorType;
using phazite::ParameterRef;
using phazite::TrackID;
namespace keys = phazite::schema::preset;

// A project in a folder of its own, open in a context, with two tracks and no undo history.
struct Song
{
    explicit Song (const juce::File& parent, const juce::String& name)
    {
        juce::String error;
        project = phazite::Project::create (parent, name, error);
        REQUIRE (project != nullptr);
        phazite::ensureTracks (getSong(), 2);
        setSong (context, &getSong());
        getSong().getUndoManager().clearUndoHistory();
    }

    ~Song() { setSong (context, nullptr); }

    phazite::Song& getSong() const { return project->getSong(); }
    TrackID track (int index) const { return phazite::getTrackIDs (getSong())[(size_t) index]; }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    DeviceID addEffect (TrackID onto, const char* type)
    {
        phazite::addEffect (context, onto, find (type));
        return phazite::getEffects (context, onto).back().id;
    }

    // A parameter's value as set by hand, and as it plays now, its modulation included.
    float valueOf (const ParameterRef& ref) const { return phazite::getValueSetByHand (getSong(), ref); }
    float playedValueOf (const ParameterRef& ref) const { return phazite::getCurrentValue (getSong(), ref, true); }

    juce::String describeUndo() const { return phazite::describeUndo (&getSong()); }

    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
};

// A drum kit's preset in a folder, its sound beside it ("Kit Sounds/Boom.wav", outside any project), as saved from a first
// project there.
juce::File writeKitPreset (const juce::File& folder)
{
    const auto file = folder.getChildFile ("Kit" + juce::String (phazite::presetExtension));
    const auto sound = phazite::test::writeSound (folder.getChildFile ("Sounds/Boom.wav"), 2.0);
    Song first (folder, "First");
    phazite::chooseInstrument (first.context, first.track (0), first.find ("phaziteDrums"));
    const auto drums = phazite::getInstrument (first.context, first.track (0))->id;
    REQUIRE (phazite::makeDeviceHandle (first.getSong(), drums).setPadSound (0, sound));
    phazite::test::waitForAudioWork(); // its sound copied into the project in the background (known bug 123)
    REQUIRE (phazite::saveDevicePreset (first.context, drums, file));
    return file;
}

// What a drum sampler's first pad plays, as the song names it.
juce::String getPadFile (const phazite::Song& song, DeviceID drums)
{
    return phazite::test::copyDevice (song, drums)
        .getChildWithName (phazite::schema::SOUNDS)[phazite::schema::getPadSoundFile (0)]
        .toString();
}

std::vector<juce::String> namesOf (const std::vector<phazite::DeviceSlot>& slots)
{
    std::vector<juce::String> names;

    for (const auto& slot : slots)
        names.push_back (slot.name);

    return names;
}

} // namespace

TEST_CASE ("A container saved as a preset loads into another project with its macros working")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Presets/Containers/Vocal Air" + juce::String (phazite::presetExtension));

    // In one project: a container of an EQ and a compressor, whose first macro, "Air", lifts the EQ's high shelf.
    {
        Song first (temp.folder, "First");
        const auto track = first.track (0);
        const auto eq = first.addEffect (track, "phaziteEq");
        const auto compressor = first.addEffect (track, "phaziteCompressor");
        const auto container = phazite::groupIntoContainer (first.context, { eq, compressor });
        const auto macro = phazite::getContainerMacros (first.context, container)[0].id;
        phazite::renameMacro (first.context, macro, "Air");
        phazite::modulate (first.context, macro, { eq, "band7Gain" }, 0.5f);
        phazite::getParameter (first.context, { compressor, "makeup" }).setValue (6.0f);

        REQUIRE (phazite::saveDevicePreset (first.context, container, file));
        CHECK (phazite::getPresetFolderName (first.context, container) == juce::String ("Containers"));
    }

    const auto info = phazite::readPresetInfo (file);
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::PresetKind::container);

    // In another: the container, named after its preset, with its devices, settings and macros.
    Song second (temp.folder, "Second");
    const auto track = second.track (1);
    second.addEffect (track, "phaziteReverb");
    REQUIRE (phazite::loadPreset (second.context, file, track));
    CHECK (second.describeUndo() == juce::String ("Undo Add Vocal Air"));

    const auto effects = phazite::getEffects (second.context, track);
    REQUIRE (effects.size() == 2);
    CHECK (effects[1].isContainer);
    CHECK (effects[1].name == juce::String ("Vocal Air"));
    const auto container = effects[1].id;
    const auto inside = phazite::getContainerDevices (second.context, container);
    CHECK (namesOf (inside) == std::vector<juce::String> { "EQ", "Compressor" });
    CHECK (second.valueOf ({ inside[1].id, "makeup" }) == doctest::Approx (6.0f));

    const auto macros = phazite::getContainerMacros (second.context, container);
    REQUIRE (macros.size() == 8);
    CHECK (macros[0].name == juce::String ("Air"));

    // The macro modulates this project's EQ, and turning it moves the EQ as the engine plays it.
    const ParameterRef shelf { inside[0].id, "band7Gain" };
    const auto modulations = phazite::getModulations (second.context, shelf);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].modulator == macros[0].id);
    CHECK (modulations[0].amount == doctest::Approx (0.5f));

    {
        phazite::test::EnginePlayback playback;
        playback.play (0.1);
        const auto before = second.playedValueOf (shelf);
        auto knob = phazite::getModulatorParameter (second.context, macros[0].id,
                                                    phazite::getModulatorParameters (second.context, macros[0].id)[0].id);
        knob.setValue (1.0f);
        playback.play (0.1);
        CHECK (second.playedValueOf (shelf) > before + 1.0f);
    }

    // One undo step takes it all away.
    phazite::undo (second.getSong());
    phazite::undo (second.getSong());
    CHECK (phazite::getEffects (second.context, track).size() == 1);
}

TEST_CASE ("A container's preset with a chain's volume and pan past any float's loads them at the ends of their ranges")
{
    // A reviewer, of known bug 148's fix: a JSON double such as 1e300 was cast to a float, which is undefined behaviour
    // (MSVC makes an infinity of it, which the chain's volume then took for no number, 0 dB).
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Loud" + juce::String (phazite::presetExtension));

    {
        Song first (temp.folder, "First");
        const auto eq = first.addEffect (first.track (0), "phaziteEq");
        const auto container = phazite::groupIntoContainer (first.context, { eq });
        REQUIRE (phazite::saveDevicePreset (first.context, container, file));
    }

    auto preset = phazite::readPreset (file);
    auto* chains = preset[keys::container][keys::chains].getArray();
    REQUIRE (chains != nullptr);
    REQUIRE (! chains->isEmpty());
    chains->getReference (0).getDynamicObject()->setProperty (keys::volume, 1.0e300);
    chains->getReference (0).getDynamicObject()->setProperty (keys::pan, -1.0e300);
    REQUIRE (phazite::writePreset (preset, file));

    Song second (temp.folder, "Second");
    REQUIRE (phazite::loadPreset (second.context, file, second.track (0)));
    const auto effects = phazite::getEffects (second.context, second.track (0));
    REQUIRE (! effects.empty());
    const auto chain = phazite::getChains (second.getSong(), effects.back().id).front();
    CHECK (phazite::getChainVolume (second.getSong(), chain) == doctest::Approx (6.0f));
    CHECK (phazite::getChainPan (second.getSong(), chain) == doctest::Approx (-1.0f));
}

TEST_CASE ("A track's preset whose volume and pan are no finite number leaves the track's as they are")
{
    // A reviewer, of known bug 148's fix: such a volume became 0 dB, set on the track as if the preset gave it (and a pan
    // the middle): a guess, rather than the track as it is.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Lead" + juce::String (phazite::presetExtension));

    {
        Song first (temp.folder, "First");
        REQUIRE (phazite::saveTrackPreset (first.context, first.track (0), file));
    }

    auto preset = phazite::readPreset (file);
    auto* track = preset[keys::track].getDynamicObject();
    REQUIRE (track != nullptr);
    track->setProperty (keys::volume, "1e999");
    track->setProperty (keys::pan, "nan");
    REQUIRE (phazite::writePreset (preset, file));

    Song second (temp.folder, "Second");
    const auto before = phazite::getTrackIDs (second.getSong());
    REQUIRE (phazite::loadPreset (second.context, file, second.track (0)));
    juce::ValueTree added;

    for (const auto id : phazite::getTrackIDs (second.getSong()))
        if (std::find (before.begin(), before.end(), id) == before.end())
            added = phazite::findTrackState (second.getSong(), id);

    REQUIRE (added.isValid());
    CHECK_FALSE (added.hasProperty (phazite::schema::channelVolume));
    CHECK_FALSE (added.hasProperty (phazite::schema::channelPan));
}

TEST_CASE ("A device's preset holds its settings and mix, not its automation, outside modulation, sidechain or switch")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Song");
    auto& context = song.context;
    const auto track = song.track (0), other = song.track (1);
    const auto compressor = song.addEffect (track, "phaziteCompressor");
    const ParameterRef makeup { compressor, "makeup" };

    // Set by hand, automated, modulated by the track's LFO, listening to the other track, and off.
    phazite::getParameter (context, makeup).setValue (-6.0f);
    phazite::getMix (context, compressor).setValue (0.5f);
    phazite::showAutomationLane (context, makeup);
    phazite::addAutomationPoint (context, makeup, { phazite::BeatPosition::fromBeats (4.0), 0.4f });
    const auto lfo = phazite::addModulator (context, track, ModulatorType::lfo);
    phazite::modulate (context, lfo, makeup, 0.25f);
    phazite::setSidechainSource (context, compressor, other);
    phazite::setDeviceOn (context, compressor, false);

    const auto preset = phazite::makeDevicePreset (song.getSong(), compressor);
    CHECK (preset[keys::kind].toString() == juce::String ("device"));

    // Phazite's own names: its type, its name, and every parameter by its ID, as set by hand.
    const auto saved = preset[keys::device];
    REQUIRE (saved.isObject());
    CHECK (saved[keys::type].toString() == juce::String ("phaziteCompressor"));
    CHECK (saved[keys::name].toString() == juce::String ("Compressor"));
    CHECK_FALSE ((bool) saved[keys::instrument]);
    const auto* parameters = saved[keys::parameters].getDynamicObject();
    REQUIRE (parameters != nullptr);
    CHECK (parameters->getProperties().size() == (int) phazite::devices::Compressor::info.parameters.size());
    CHECK ((float) parameters->getProperty ("makeup") == doctest::Approx (-6.0f));
    CHECK ((float) parameters->getProperty ("mix") == doctest::Approx (0.5f));
    CHECK ((float) parameters->getProperty ("attack") == doctest::Approx (10.0f)); // its default, written too

    // Nothing that ties it to its project: none of the song's elements of its automation, its modulation or its values,
    // nor its sidechain or its switch.
    CHECK (saved.getDynamicObject()->getProperties().size() == 4); // type, name, instrument and parameters
    const auto text = juce::JSON::toString (preset);
    for (const auto* songName : { "DEVICE", "AUTOMATION", "MODULATES", "VALUES", "\"sidechain\"", "\"off\"" })
        CHECK_FALSE (text.contains (songName));

    // Loaded below the tracks: an effect goes on the master, on.
    const auto file = temp.folder.getChildFile ("Squash" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::writePreset (preset, file));
    REQUIRE (phazite::loadPreset (context, file, {}));
    const auto master = phazite::getEffects (context, {});
    REQUIRE_FALSE (master.empty());
    CHECK (master.back().name == juce::String ("Compressor"));
    CHECK (master.back().isOn);
    CHECK (master.back().id != compressor);
    CHECK (song.valueOf ({ master.back().id, "makeup" }) == doctest::Approx (-6.0f));
}

TEST_CASE ("An instrument's preset takes a track's instrument's place, or makes a track of its own below the tracks")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Song");
    auto& context = song.context;
    const auto track = song.track (0);
    phazite::chooseInstrument (context, track, song.find ("phaziteSynth"));
    const auto synth = phazite::getInstrument (context, track)->id;
    phazite::getParameter (context, { synth, "cutoff" }).setValue (1000.0f);

    const auto file = temp.folder.getChildFile ("Dark Lead" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (context, synth, file));
    const auto info = phazite::readPresetInfo (file);
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::PresetKind::device);
    CHECK (info->device == juce::String ("Synth"));
    CHECK (info->isInstrument);
    CHECK (phazite::getPresetFolderName (context, synth) == juce::String ("Synth"));

    // Onto the other track, which had none; then below the tracks, a new track.
    REQUIRE (phazite::loadPreset (context, file, song.track (1)));
    const auto loaded = phazite::getInstrument (context, song.track (1));
    REQUIRE (loaded.has_value());
    CHECK (song.valueOf ({ loaded->id, "cutoff" }) == doctest::Approx (1000.0f).epsilon (0.01));

    REQUIRE (phazite::loadPreset (context, file, {}));
    CHECK (phazite::getTrackIDs (song.getSong()).size() == 3);
    CHECK (phazite::getInstrument (context, song.track (2)).has_value());
}

TEST_CASE ("A plug-in's presets go in its folder among the plug-ins', apart from one of Phazite's devices of its name; no device of "
           "Phazite's is named as the folders kept for other presets (known bug 209)")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Song");
    auto& context = song.context;
    const auto track = song.track (0);

    // Phazite's reverb, and a plug-in called Reverb (not installed here), on a track.
    const auto reverb = song.addEffect (track, "phaziteReverb");
    juce::PluginDescription description;
    description.name = "Reverb";
    description.pluginFormatName = "VST3";
    description.manufacturerName = "Elsewhere";
    description.fileOrIdentifier = "C:/Plug-ins/Reverb.vst3";
    const auto pluginType = phazite::makeDeviceType (description);
    const auto plugin = phazite::test::addPluginDevice (song.getSong(), track, pluginType.description);

    // A device in the song and its type in the device lists (as the browser's rows list them) have the same folder.
    CHECK (phazite::getPresetFolderName (context, reverb) == juce::String ("Reverb"));
    CHECK (phazite::getPresetFolderName (song.find ("phaziteReverb")) == juce::String ("Reverb"));
    CHECK (phazite::getPresetFolderName (context, plugin) == juce::String ("Plug-ins/Reverb"));
    CHECK (phazite::getPresetFolderName (pluginType) == juce::String ("Plug-ins/Reverb"));

    // Its name made usable as a file's, inside the plug-ins' folder: one named for a folder kept for other presets, too.
    description.name = "Tracks";
    CHECK (phazite::getPresetFolderName (phazite::makeDeviceType (description)) == juce::String ("Plug-ins/Tracks"));
    description.name = "Echo: Wide";
    CHECK (phazite::getPresetFolderName (phazite::makeDeviceType (description)) == juce::String ("Plug-ins/Echo Wide"));

    // Nor one named as a step along a path, which would put its presets outside the plug-ins' folder.
    description.name = "..";
    CHECK (phazite::getPresetFolderName (phazite::makeDeviceType (description)) == juce::String ("Plug-ins/__"));
    description.name = ".";
    CHECK (phazite::getPresetFolderName (phazite::makeDeviceType (description)) == juce::String ("Plug-ins/_"));

    // None of Phazite's devices' folders is one kept for other presets: no device of Phazite's shares one.
    for (const auto& type : phazite::getDeviceTypes (context))
    {
        if (! type.isBuiltIn)
            continue;

        CAPTURE (type.name);
        const auto folder = phazite::getPresetFolderName (type);
        CHECK (folder.isNotEmpty());
        CHECK_FALSE (folder.containsChar ('/'));

        for (const auto* kept : { phazite::pluginPresetsFolder, phazite::trackPresetsFolder, "Containers" })
            CHECK_FALSE (folder.equalsIgnoreCase (kept));
    }
}

TEST_CASE ("A track's preset makes a new track with its devices, mix and modulation")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Wobble Bass" + juce::String (phazite::presetExtension));

    {
        Song first (temp.folder, "First");
        auto& context = first.context;
        const auto track = first.track (0);
        phazite::renameTrack (first.getSong(), track, "Bass");
        phazite::setTrackColour (first.getSong(), track, juce::Colours::orange);
        phazite::chooseInstrument (context, track, first.find ("phaziteSynth"));
        const auto eq = first.addEffect (track, "phaziteEq");
        const auto reverb = first.addEffect (track, "phaziteReverb");
        const auto container = phazite::groupIntoContainer (context, { reverb });
        phazite::getVolume (context, track).setValue (0.6f);
        phazite::getPan (context, track).setValue (-0.5f);

        // An LFO on the synth's cutoff, a macro on the EQ, and the container's macro on its reverb.
        const auto synth = phazite::getInstrument (context, track)->id;
        const auto lfo = phazite::addModulator (context, track, ModulatorType::lfo);
        phazite::modulate (context, lfo, { synth, "cutoff" }, 0.25f);
        phazite::setModulatorOn (context, lfo, false); // as a device switched off, a preset loads it on
        const auto macro = phazite::addModulator (context, track, ModulatorType::macro);
        phazite::modulate (context, macro, { eq, "band4Gain" }, 0.5f);
        phazite::modulate (context, phazite::getContainerMacros (context, container)[0].id,
                           { phazite::getContainerDevices (context, container)[0].id, "decay" }, 0.5f);

        // A clip and a send, which a track preset leaves out.
        phazite::addSendToNewReturn (context, track);
        phazite::test::addMidiClip (first.getSong(), track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) });

        REQUIRE (phazite::saveTrackPreset (context, track, file));
    }

    const auto info = phazite::readPresetInfo (file);
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::PresetKind::track);

    Song second (temp.folder, "Second");
    auto& context = second.context;
    auto& song = second.getSong();
    REQUIRE (phazite::loadPreset (context, file, second.track (0)));
    CHECK (second.describeUndo() == juce::String ("Undo Add Wobble Bass"));

    // After the track it was dropped on.
    auto tracks = phazite::getTrackIDs (song);
    REQUIRE (tracks.size() == 3);
    const auto track = tracks[1];
    CHECK (phazite::getTrackName (song, track) == juce::String ("Bass"));
    CHECK (phazite::getTrackColour (song, track, 1) == juce::Colours::orange);
    CHECK (phazite::getClips (song, track).empty());
    CHECK (phazite::getSends (context, track).empty());
    CHECK (phazite::getVolume (context, track).getValue() == doctest::Approx (0.6f));
    CHECK (phazite::getPan (context, track).getValue() == doctest::Approx (-0.5f));

    const auto instrument = phazite::getInstrument (context, track);
    REQUIRE (instrument.has_value());
    CHECK (instrument->name == juce::String ("Synth"));
    const auto effects = phazite::getEffects (context, track);
    REQUIRE (effects.size() == 2);
    CHECK (effects[0].name == juce::String ("EQ"));
    CHECK (effects[1].isContainer);

    // Its modulators and macros, which modulate its own devices.
    const auto modulators = phazite::getModulators (context, track);
    REQUIRE (modulators.size() == 2);
    CHECK (modulators[0].type == ModulatorType::lfo);
    CHECK (modulators[0].isOn);
    CHECK (modulators[1].type == ModulatorType::macro);

    const auto cutoff = phazite::getModulations (context, ParameterRef { instrument->id, "cutoff" });
    REQUIRE (cutoff.size() == 1);
    CHECK (cutoff[0].modulator == modulators[0].id);
    const auto gain = phazite::getModulations (context, ParameterRef { effects[0].id, "band4Gain" });
    REQUIRE (gain.size() == 1);
    CHECK (gain[0].modulator == modulators[1].id);

    const auto reverb = phazite::getContainerDevices (context, effects[1].id);
    REQUIRE (reverb.size() == 1);
    const auto decay = phazite::getModulations (context, ParameterRef { reverb[0].id, "decay" });
    REQUIRE (decay.size() == 1);
    CHECK (decay[0].modulator == phazite::getContainerMacros (context, effects[1].id)[0].id);

    // Below the tracks: at the end.
    REQUIRE (phazite::loadPreset (context, file, {}));
    tracks = phazite::getTrackIDs (song);
    REQUIRE (tracks.size() == 4);
    CHECK (phazite::getTrackName (song, tracks[3]) == juce::String ("Bass"));
}

TEST_CASE ("A container's and a track's presets keep their macros' names and values, their modulators' values and steps, and "
           "what each modulates")
{
    // What each preset reads of the song's modulation (Presets.cpp: a container's macros, a track's modulators and macros),
    // which reads it by IDs (model/modulation/internal/Modulation.h): built and checked through the model, so that it holds
    // wherever the song keeps its modulation (roadmap step S5.20). Each preset in a subcase of its own, which fails alone.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Moving" + juce::String (phazite::presetExtension));
    const auto macroValueOf = [] (phazite::ProjectContext& context, phazite::ModulatorID macro)
    { return phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro)[0].id).getValue(); };

    SUBCASE ("A container's preset")
    {
        {
            Song first (temp.folder, "First");
            auto& context = first.context;

            // A container's two macros, named, turned and modulating its EQ.
            const auto eq = first.addEffect (first.track (0), "phaziteEq");
            const auto container = phazite::groupIntoContainer (context, { eq });
            const auto macros = phazite::getContainerMacros (context, container);
            phazite::renameMacro (context, macros[0].id, "Air");
            phazite::getModulatorParameter (context, macros[0].id, phazite::getModulatorParameters (context, macros[0].id)[0].id)
                .setValue (0.25f);
            phazite::modulate (context, macros[0].id, { eq, "band7Gain" }, 0.5f);
            phazite::getModulatorParameter (context, macros[1].id, phazite::getModulatorParameters (context, macros[1].id)[0].id)
                .setValue (0.75f);
            REQUIRE (phazite::saveDevicePreset (context, container, file));
        }

        Song second (temp.folder, "Second");
        auto& context = second.context;
        REQUIRE (phazite::loadPreset (context, file, second.track (0)));

        // The container's macros, each as it was, and what the first modulates.
        const auto effects = phazite::getEffects (context, second.track (0));
        REQUIRE (effects.size() == 1);
        const auto macros = phazite::getContainerMacros (context, effects[0].id);
        REQUIRE (macros.size() == 8);
        CHECK (macros[0].name == juce::String ("Air"));
        CHECK (macroValueOf (context, macros[0].id) == doctest::Approx (0.25f));
        CHECK (macroValueOf (context, macros[1].id) == doctest::Approx (0.75f));
        CHECK (macroValueOf (context, macros[2].id) == doctest::Approx (0.0f));
        const auto eq = phazite::getContainerDevices (context, effects[0].id)[0].id;
        const auto shelf = phazite::getModulations (context, ParameterRef { eq, "band7Gain" });
        REQUIRE (shelf.size() == 1);
        CHECK (shelf[0].modulator == macros[0].id);
        CHECK (shelf[0].amount == doctest::Approx (0.5f));
    }

    SUBCASE ("A track's preset")
    {
        {
            Song first (temp.folder, "First");
            auto& context = first.context;

            // A track's LFO, its rate set; a step sequencer, two steps set; and a macro, named and turned, on its volume.
            const auto track = first.track (1);
            phazite::chooseInstrument (context, track, first.find ("phaziteSynth"));
            const auto synth = phazite::getInstrument (context, track)->id;
            const auto lfo = phazite::addModulator (context, track, ModulatorType::lfo);
            phazite::getModulatorParameter (context, lfo, "rate").setValue (3.0f);
            phazite::modulate (context, lfo, { synth, "cutoff" }, -0.25f);
            const auto steps = phazite::addModulator (context, track, ModulatorType::steps);
            phazite::setStep (context, steps, 1, 0.5f);
            phazite::setStep (context, steps, 3, -0.25f);
            phazite::modulate (context, steps, phazite::getPan (context, track).getRef(), 0.125f);
            const auto macro = phazite::addModulator (context, track, ModulatorType::macro);
            phazite::renameMacro (context, macro, "Swell");
            phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro)[0].id).setValue (0.4f);
            phazite::modulate (context, macro, phazite::getVolume (context, track).getRef(), -0.5f);
            REQUIRE (phazite::saveTrackPreset (context, track, file));
        }

        Song second (temp.folder, "Second");
        auto& context = second.context;
        REQUIRE (phazite::loadPreset (context, file, second.track (0)));

        // The track's modulators and macro, each set as it was, modulating what it did.
        const auto track = phazite::getTrackIDs (second.getSong())[1];
        const auto modulators = phazite::getModulators (context, track);
        REQUIRE (modulators.size() == 3);
        CHECK (modulators[0].type == ModulatorType::lfo);
        CHECK (modulators[1].type == ModulatorType::steps);
        CHECK (modulators[2].type == ModulatorType::macro);
        CHECK (phazite::getModulatorParameter (context, modulators[0].id, "rate").getValue() == doctest::Approx (3.0f));
        const auto loadedSteps = phazite::getSteps (context, modulators[1].id);
        REQUIRE (loadedSteps.size() > 3);
        CHECK (loadedSteps[1] == doctest::Approx (0.5f));
        CHECK (loadedSteps[3] == doctest::Approx (-0.25f));
        CHECK (modulators[2].name == juce::String ("Swell"));
        CHECK (macroValueOf (context, modulators[2].id) == doctest::Approx (0.4f));

        const auto synth = phazite::getInstrument (context, track)->id;
        const auto cutoff = phazite::getModulations (context, ParameterRef { synth, "cutoff" });
        REQUIRE (cutoff.size() == 1);
        CHECK (cutoff[0].modulator == modulators[0].id);
        CHECK (cutoff[0].amount == doctest::Approx (-0.25f));
        const auto pan = phazite::getModulations (context, phazite::getPan (context, track).getRef());
        REQUIRE (pan.size() == 1);
        CHECK (pan[0].modulator == modulators[1].id);
        CHECK (pan[0].amount == doctest::Approx (0.125f));
        const auto volume = phazite::getModulations (context, phazite::getVolume (context, track).getRef());
        REQUIRE (volume.size() == 1);
        CHECK (volume[0].modulator == modulators[2].id);
        CHECK (volume[0].amount == doctest::Approx (-0.5f));
    }
}

TEST_CASE ("A drum sampler's preset brings its sounds into the project that takes it")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Kit" + juce::String (phazite::presetExtension));
    const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Sounds/Boom.wav"), 0.2);

    {
        Song first (temp.folder, "First");
        auto& context = first.context;
        phazite::chooseInstrument (context, first.track (0), first.find ("phaziteDrums"));
        const auto drums = phazite::getInstrument (context, first.track (0))->id;
        auto handle = phazite::makeDeviceHandle (first.getSong(), drums);
        REQUIRE (handle.setPadSound (0, sound));
        phazite::test::waitForAudioWork(); // its sound copied into the project in the background (known bug 123)
        REQUIRE (phazite::saveDevicePreset (context, drums, file));
    }

    // Known bug 107: the preset named the sound by its full path, the user's name in it, and the sound didn't go with it.
    // Its sounds go beside it now, in a folder named after it, and it names each by its path from the preset's folder.
    const auto text = file.loadFileAsString();
    CHECK_FALSE (text.contains (temp.folder.getFullPathName().replace ("\\", "\\\\")));
    CHECK_FALSE (text.contains (temp.folder.getFullPathName()));
    const auto saved = juce::JSON::parse (text);
    CHECK (saved[keys::device][keys::sounds][phazite::schema::getPadSoundFile (0)].toString() == "Kit Sounds/Boom.wav");
    CHECK (file.getSiblingFile ("Kit Sounds").getChildFile ("Boom.wav").existsAsFile());

    // On another computer: the preset and its folder copied there, the projects' and the sound's own folders gone.
    const auto elsewhere = temp.folder.getChildFile ("Another computer");
    REQUIRE (elsewhere.createDirectory().wasOk());
    REQUIRE (file.copyFileTo (elsewhere.getChildFile (file.getFileName())));
    REQUIRE (file.getSiblingFile ("Kit Sounds").copyDirectoryTo (elsewhere.getChildFile ("Kit Sounds")));
    REQUIRE (sound.getParentDirectory().deleteRecursively());
    REQUIRE (temp.folder.getChildFile ("First").deleteRecursively());

    // Read back, the preset names the sound beside it by its full path, which loading copies into the project.
    const auto copied = elsewhere.getChildFile (file.getFileName());
    const auto preset = phazite::readPreset (copied);
    const auto path = preset[keys::device][keys::sounds][phazite::schema::getPadSoundFile (0)].toString();
    CHECK (juce::File (path) == elsewhere.getChildFile ("Kit Sounds/Boom.wav"));

    Song second (temp.folder, "Second");
    auto& context = second.context;
    REQUIRE (phazite::loadPreset (context, copied, second.track (0)));
    phazite::test::waitForAudioWork (context); // its sound copied in, in the background, then the kit added (known bug 183)
    const auto drums = phazite::getInstrument (context, second.track (0));
    REQUIRE (drums.has_value());
    const auto device = phazite::test::copyDevice (second.getSong(), drums->id);
    REQUIRE (device.isValid());
    CHECK (device.getChildWithName (phazite::schema::SOUNDS)[phazite::schema::getPadSoundFile (0)].toString()
           == juce::String ("Audio/Boom.wav"));
    CHECK (second.project->getAudioFolder().getChildFile ("Boom.wav").existsAsFile());
}

TEST_CASE ("A preset whose sounds are outside the project goes in once they are copied in, in the background, as one undo step, "
           "the message thread reading none of them")
{
    // Known bug 183: a kit's sounds were copied into the project, and compared in full with a file of the same name there,
    // in the call that loaded it, holding the window.
    phazite::test::TemporaryFolder temp;
    const auto file = writeKitPreset (temp.folder);
    Song second (temp.folder, "Second");
    auto& context = second.context;
    const auto audio = second.project->getAudioFolder();
    phazite::test::UndoCheck undo (context);
    const auto before = phazite::AudioWork::getMessageThreadReads();

    // Dropped on a track: nothing until its sound is in; then the kit, its pad playing the project's copy.
    undo.check ("Add Kit",
                [&]
                {
                    REQUIRE (phazite::loadPreset (context, file, second.track (0)));
                    CHECK (phazite::isReadingAudio (context));
                    CHECK_FALSE (phazite::getInstrument (context, second.track (0)).has_value());
                    phazite::test::waitForAudioWork (context);
                });

    const auto added = phazite::getInstrument (context, second.track (0));
    REQUIRE (added.has_value());
    CHECK (getPadFile (second.getSong(), added->id) == juce::String ("Audio/Boom.wav"));
    CHECK (audio.getChildFile ("Boom.wav").existsAsFile());
    CHECK (second.describeUndo() == juce::String ("Undo Add Kit"));

    // Loaded into a kit already there, as its Presets menu does: the same, as "Load Preset Kit", the copy made once.
    phazite::chooseInstrument (context, second.track (1), second.find ("phaziteDrums"));
    const auto other = phazite::getInstrument (context, second.track (1))->id;
    undo.check ("Load Preset Kit",
                [&]
                {
                    REQUIRE (phazite::loadPresetInto (context, other, file));
                    phazite::test::waitForAudioWork (context);
                });

    CHECK (getPadFile (second.getSong(), other) == juce::String ("Audio/Boom.wav"));
    CHECK (second.describeUndo() == juce::String ("Undo Load Preset Kit"));
    CHECK_FALSE (audio.getChildFile ("Boom2.wav").existsAsFile());
    CHECK (phazite::AudioWork::getMessageThreadReads() == before);
    undo.walkBackAndForth();
}

TEST_CASE ("A preset's load whose sounds are being copied in changes nothing if it is cancelled, or if its track or device goes "
           "meanwhile, and leaves no copy")
{
    phazite::test::TemporaryFolder temp;
    const auto file = writeKitPreset (temp.folder);
    Song second (temp.folder, "Second");
    auto& context = second.context;
    auto& work = phazite::getAudioWork (context);
    phazite::test::RenderHost window;
    work.setHost (&window);
    work.setShowAfter (0);
    juce::StringArray problems;
    phazite::setProblemReporter (context, [&problems] (const juce::String& title, const juce::String& message)
                                 { problems.add (title + ": " + message); });

    // Its copy held until let go, or stopped.
    std::atomic<bool> letGo { false };
    work.setOnReadStep (
        [&letGo] (int, const phazite::BackgroundJobs::ShouldStop& stop)
        {
            while (! letGo && ! stop())
                juce::Thread::sleep (1);
        });

    SUBCASE ("Cancelled")
    {
        const auto songBefore = phazite::test::writeSong (second.getSong(), phazite::test::SongPart::whole);
        REQUIRE (phazite::loadPreset (context, file, second.track (0)));
        REQUIRE (phazite::test::runTimersUntil ([&window] { return window.render != nullptr; }));
        CHECK (window.render->getTitle() == juce::String ("Loading Kit"));
        window.render->cancel(); // as its Cancel does, before its window deletes it
        window.render = nullptr;
        REQUIRE (phazite::test::runTimersUntil ([&work] { return ! work.isBusy() && work.countReadsGivenUp() == 0; }));
        CHECK (phazite::test::writeSong (second.getSong(), phazite::test::SongPart::whole) == songBefore);
        CHECK (problems.isEmpty());
    }

    SUBCASE ("Its track gone")
    {
        REQUIRE (phazite::loadPreset (context, file, second.track (1)));

        {
            const phazite::UndoStep step (second.getSong(), "Delete Track");
            phazite::deleteTracks (second.getSong(), { second.track (1) }, nullptr);
        }

        const auto songBefore = phazite::test::writeSong (second.getSong(), phazite::test::SongPart::whole);
        letGo = true;
        phazite::test::waitForAudioWork (context);
        CHECK (phazite::test::writeSong (second.getSong(), phazite::test::SongPart::whole) == songBefore);
        REQUIRE (problems.size() == 1);
        CHECK (problems[0].contains ("Kit"));
    }

    SUBCASE ("Its device gone")
    {
        phazite::chooseInstrument (context, second.track (0), second.find ("phaziteDrums"));
        const auto drums = phazite::getInstrument (context, second.track (0))->id;
        REQUIRE (phazite::loadPresetInto (context, drums, file));
        phazite::removeDevice (context, drums);
        const auto songBefore = phazite::test::writeSong (second.getSong(), phazite::test::SongPart::whole);
        letGo = true;
        phazite::test::waitForAudioWork (context);
        CHECK (phazite::test::writeSong (second.getSong(), phazite::test::SongPart::whole) == songBefore);
        REQUIRE (problems.size() == 1);
        CHECK (problems[0].contains ("Kit"));
    }

    // No copy left in the project, which nothing names.
    CHECK_FALSE (second.project->getAudioFolder().getChildFile ("Boom.wav").existsAsFile());
    work.setOnReadStep (nullptr);
    work.setHost (nullptr);
    work.setShowAfter (phazite::AudioWork::showAfterMilliseconds);
    phazite::setProblemReporter (context, nullptr);
}

TEST_CASE ("The presets that come with Phazite name no sound files, so loading one copies nothing (as an example song's making "
           "does)")
{
    // Their sounds are those Phazite makes, named ("808 Kick"): what copyDeviceFilesInto() would copy is none.
    int presets = 0;

    for (const auto& entry :
         juce::RangedDirectoryIterator (phazite::test::getFactoryPresetsFolder(), true, juce::String ("*") + phazite::presetExtension))
    {
        ++presets;
        juce::StringArray files;

        std::function<void (const juce::var&)> look = [&look, &files] (const juce::var& value)
        {
            if (auto* object = value.getDynamicObject())
            {
                if (auto* sounds = object->getProperty (keys::sounds).getDynamicObject())
                    for (const auto& [property, sound] : sounds->getProperties())
                        if (phazite::schema::isSoundFileProperty (property))
                            files.add (property.toString() + " = " + sound.toString());

                for (const auto& property : object->getProperties())
                    look (property.value);
            }
            else if (const auto* array = value.getArray())
            {
                for (const auto& each : *array)
                    look (each);
            }
        };

        look (juce::JSON::parse (entry.getFile()));
        INFO (entry.getFile().getFullPathName().toStdString());
        CHECK (files.isEmpty());
    }

    CHECK (presets > 50);
}

TEST_CASE ("A preset's sounds that can't be taken are told by name, those on a network share among them")
{
    // Known bug 151: a sound on a share (\\NAS\Samples\...) is refused without being touched, and was left out without a word.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Kit" + juce::String (phazite::presetExtension));
    const auto boom = phazite::test::writeSound (temp.folder.getChildFile ("Sounds/Boom.wav"), 0.2);

    {
        Song first (temp.folder, "First");
        auto& context = first.context;
        phazite::chooseInstrument (context, first.track (0), first.find ("phaziteDrums"));
        const auto drums = phazite::getInstrument (context, first.track (0))->id;
        auto handle = phazite::makeDeviceHandle (first.getSong(), drums);
        REQUIRE (handle.setPadSound (0, boom));
        phazite::test::waitForAudioWork(); // its sound copied into the project in the background (known bug 123)
        REQUIRE (phazite::saveDevicePreset (context, drums, file));
    }

    // Two more pads, as a preset from someone else may name them: one on a share, one that isn't there.
    auto preset = juce::JSON::parse (file);
    auto* sounds = preset[keys::device][keys::sounds].getDynamicObject();
    REQUIRE (sounds != nullptr);
    sounds->setProperty (phazite::schema::getPadSoundFile (1), "\\\\NAS\\Samples\\Snare.wav");
    sounds->setProperty (phazite::schema::getPadSoundFile (2), temp.folder.getChildFile ("Gone/Hat.wav").getFullPathName());
    REQUIRE (phazite::writePreset (preset, file));

    Song second (temp.folder, "Second");
    juce::StringArray problems;
    phazite::setProblemReporter (second.context, [&problems] (const juce::String& title, const juce::String& message)
                                 { problems.add (title + ": " + message); });
    REQUIRE (phazite::loadPreset (second.context, file, second.track (0)));
    phazite::test::waitForAudioWork (second.context); // its sound copied in, in the background, then the kit added (known bug 183)
    REQUIRE (problems.size() == 1);
    INFO (problems[0].toStdString());
    CHECK (problems[0].contains ("Snare.wav"));
    CHECK (problems[0].contains ("Hat.wav"));
    CHECK_FALSE (problems[0].contains ("Boom.wav"));
    CHECK (second.project->getAudioFolder().getChildFile ("Boom.wav").existsAsFile());
}

TEST_CASE ("A preset that names a sound by a full path or out of its folder takes nothing from there, and tells it")
{
    // The reviewer's finding on the first wave: a preset from someone else could name any file on this computer by its full
    // path (or "~/..."), which loading it copied into the project. Phazite writes only paths from the preset's folder.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Presets/Kit" + juce::String (phazite::presetExtension));
    const auto memo = phazite::test::writeSound (temp.folder.getChildFile ("Private/Memo.wav"), 0.2);
    const auto secret = phazite::test::writeSound (temp.folder.getChildFile ("Secret.wav"), 0.2);

    {
        Song first (temp.folder, "First");
        phazite::chooseInstrument (first.context, first.track (0), first.find ("phaziteDrums"));
        REQUIRE (phazite::saveDevicePreset (first.context, phazite::getInstrument (first.context, first.track (0))->id, file));
    }

    auto preset = juce::JSON::parse (file);
    auto* sounds = preset[keys::device][keys::sounds].getDynamicObject();

    if (sounds == nullptr)
    {
        preset[keys::device].getDynamicObject()->setProperty (keys::sounds, new juce::DynamicObject());
        sounds = preset[keys::device][keys::sounds].getDynamicObject();
    }

    REQUIRE (sounds != nullptr);
    sounds->setProperty (phazite::schema::getPadSoundFile (0), memo.getFullPathName());
    sounds->setProperty (phazite::schema::getPadSoundFile (1), "../Secret.wav");
    REQUIRE (phazite::writePreset (preset, file));

    // Read, neither names a file to take.
    const auto read = phazite::readPreset (file);
    CHECK (read[keys::device][keys::sounds][phazite::schema::getPadSoundFile (0)].toString() == juce::String ("Memo.wav"));
    CHECK (read[keys::device][keys::sounds][phazite::schema::getPadSoundFile (1)].toString() == juce::String ("../Secret.wav"));

    Song second (temp.folder, "Second");
    juce::StringArray problems;
    phazite::setProblemReporter (second.context, [&problems] (const juce::String& title, const juce::String& message)
                                 { problems.add (title + ": " + message); });
    REQUIRE (phazite::loadPreset (second.context, file, second.track (0)));
    REQUIRE (problems.size() == 1);
    INFO (problems[0].toStdString());
    CHECK (problems[0].contains ("Memo.wav (not beside the preset)"));
    CHECK (problems[0].contains ("Secret.wav (not beside the preset)"));
    CHECK_FALSE (second.project->getAudioFolder().getChildFile ("Memo.wav").exists());
    CHECK_FALSE (second.project->getAudioFolder().getChildFile ("Secret.wav").exists());
    CHECK (memo.existsAsFile());
    CHECK (secret.existsAsFile());
}

TEST_CASE ("A preset whose sound can't be copied beside it isn't saved, leaves nothing behind, and says which sound")
{
    // The reviewer's finding on the first wave: the sound was left out of the preset, which was saved as if whole.
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Presets/Kit" + juce::String (phazite::presetExtension));
    const auto boom = phazite::test::writeSound (temp.folder.getChildFile ("Sounds/Boom.wav"), 0.2);

    // A file where its folder of sounds goes.
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.getSiblingFile ("Kit Sounds").replaceWithText ("in the way"));

    Song song (temp.folder, "Song");
    juce::StringArray problems;
    phazite::setProblemReporter (song.context, [&problems] (const juce::String& title, const juce::String& message)
                                 { problems.add (title + ": " + message); });
    phazite::chooseInstrument (song.context, song.track (0), song.find ("phaziteDrums"));
    const auto drums = phazite::getInstrument (song.context, song.track (0))->id;
    auto handle = phazite::makeDeviceHandle (song.getSong(), drums);
    REQUIRE (handle.setPadSound (0, boom));
    phazite::test::waitForAudioWork(); // its sound copied into the project in the background (known bug 123)

    CHECK_FALSE (phazite::saveDevicePreset (song.context, drums, file));
    CHECK_FALSE (file.exists());
    CHECK (file.getSiblingFile ("Kit Sounds").existsAsFile()); // the user's, as it was
    REQUIRE (problems.size() == 1);
    INFO (problems[0].toStdString());
    CHECK (problems[0].startsWith ("Phazite couldn't save the preset: "));
    CHECK (problems[0].contains ("Boom.wav"));

    // Saved where its sounds can go, it holds them.
    problems.clear();
    const auto other = file.getSiblingFile ("Other" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, drums, other));
    CHECK (problems.isEmpty());
    CHECK (other.getSiblingFile ("Other Sounds").getChildFile ("Boom.wav").existsAsFile());
}

TEST_CASE ("What a preset file holds is read from its first element; other files aren't presets")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("A" + juce::String (phazite::presetExtension));
    CHECK (phazite::isPresetFile (file));
    CHECK_FALSE (phazite::isPresetFile (temp.folder.getChildFile ("A.wav")));
    CHECK_FALSE (phazite::readPresetInfo (file).has_value()); // not there

    const juce::var preset (new juce::DynamicObject());
    preset.getDynamicObject()->setProperty (keys::kind, "track");
    REQUIRE (phazite::writePreset (preset, file));
    REQUIRE (phazite::readPresetInfo (file).has_value());
    CHECK (phazite::readPresetInfo (file)->kind == phazite::PresetKind::track);

    // One of a kind there is none of.
    preset.getDynamicObject()->setProperty (keys::kind, "song");
    REQUIRE (phazite::writePreset (preset, file));
    CHECK_FALSE (phazite::readPresetInfo (file).has_value());
    CHECK_FALSE (phazite::readPreset (file).isObject());

    // A file that isn't one at all, and a preset that loads nothing.
    REQUIRE (file.replaceWithText ("not a preset"));
    CHECK_FALSE (phazite::readPresetInfo (file).has_value());

    Song song (temp.folder, "Song");
    CHECK_FALSE (phazite::loadPreset (song.context, file, song.track (0)));
}

TEST_CASE ("A device's preset loads into a device of its kind, in place of its settings and mix, as one undo step")
{
    using phazite::test::TestPlugin;
    TestPlugin testPlugin;
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Levels");
    auto& s = song.getSong();
    const auto track = song.track (0);

    // A quiet, wet one, saved; and a loud, drier one.
    const auto quiet = TestPlugin::add (s, track);
    const auto loud = TestPlugin::add (s, track);
    TestPlugin::setGain (s, quiet, 0.2f);
    phazite::setMix (s, quiet, 0.6f);
    TestPlugin::setGain (s, loud, 0.9f);
    phazite::setMix (s, loud, 0.1f);
    s.getUndoManager().clearUndoHistory();

    // Saving the preset stores the plug-in's settings in the project, but is no undo step.
    const auto file = temp.folder.getChildFile ("Quiet" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, quiet, file));
    CHECK_FALSE (s.getUndoManager().canUndo());

    // Loaded into the loud one: its settings and mix, as one undo step named after the preset.
    REQUIRE (phazite::loadPresetInto (song.context, loud, file));
    phazite::test::dispatchPendingMessages();
    CHECK (TestPlugin::getGain (s, loud) == doctest::Approx (0.2f));
    CHECK (phazite::getMix (s, loud) == doctest::Approx (0.6f));
    CHECK (song.describeUndo() == juce::String ("Undo Load Preset Quiet"));

    // Undo puts its own settings back, and plays them.
    phazite::undo (s);
    phazite::test::dispatchPendingMessages();
    CHECK (TestPlugin::getGain (s, loud) == doctest::Approx (0.9f));
    CHECK (phazite::getMix (s, loud) == doctest::Approx (0.1f));
    CHECK_FALSE (s.getUndoManager().canUndo());

    // Not into a device of another kind, nor a container's or a track's preset: none is an undo step.
    const auto eq = song.addEffect (track, "phaziteEq");
    const auto eqFile = temp.folder.getChildFile ("Bright" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, eq, eqFile));
    const auto trackFile = temp.folder.getChildFile ("Lead" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveTrackPreset (song.context, track, trackFile));
    const auto container = phazite::groupIntoContainer (song.context, { eq });
    const auto containerFile = temp.folder.getChildFile ("Chain" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, container, containerFile));
    s.getUndoManager().clearUndoHistory();

    CHECK_FALSE (phazite::loadPresetInto (song.context, loud, eqFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, loud, trackFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, loud, containerFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, phazite::getEffects (song.context, track).back().id, containerFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, {}, file));
    CHECK_FALSE (s.getUndoManager().canUndo());
    CHECK (TestPlugin::getGain (s, loud) == doctest::Approx (0.9f));

    // One of Phazite's own takes a preset of its type: the EQ in the container, as its Presets menu loads one.
    const auto inside = phazite::getContainerDevices (song.context, container)[0].id;
    phazite::getParameter (song.context, { inside, "band4Gain" }).setValue (-9.0f);
    REQUIRE (phazite::loadPresetInto (song.context, inside, eqFile));
    CHECK (song.valueOf ({ inside, "band4Gain" }) == doctest::Approx (0.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Load Preset Bright"));
    phazite::undo (s);
    CHECK (song.valueOf ({ inside, "band4Gain" }) == doctest::Approx (-9.0f));
}

TEST_CASE ("A preset loaded into one of Phazite's devices sets every parameter: those it names, and the rest to their defaults")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Starting points");
    auto& s = song.getSong();
    const auto eq = song.addEffect (song.track (0), "phaziteEq");

    // The user's own settings, which a preset that names only one band's gain mustn't leave behind.
    phazite::getParameter (song.context, { eq, "band2Gain" }).setValue (6.0f);
    phazite::getParameter (song.context, { eq, "band4Gain" }).setValue (-9.0f);
    phazite::getParameter (song.context, { eq, "output" }).setValue (-3.0f);
    const auto file = temp.folder.getChildFile ("Lift" + juce::String (phazite::presetExtension));
    REQUIRE (file.replaceWithText (R"({ "kind": "device", "device": { "type": "phaziteEq", "name": "EQ", "instrument": false,
                                        "parameters": { "band1Gain": 3.0 } } })"));
    s.getUndoManager().clearUndoHistory();

    // Loaded: as a new EQ would be with the preset, every parameter.
    REQUIRE (phazite::loadPresetInto (song.context, eq, file));
    const auto* info = phazite::findOwnDevice ("phaziteEq");
    REQUIRE (info != nullptr);

    for (const auto& parameter : info->parameters)
    {
        const auto id = juce::String (parameter.id.data(), parameter.id.size());
        CAPTURE (id.toStdString());
        CHECK (song.valueOf ({ eq, id }) == doctest::Approx (id == "band1Gain" ? 3.0f : parameter.defaultValue));
    }

    // One undo step puts the user's settings back.
    CHECK (song.describeUndo() == juce::String ("Undo Load Preset Lift"));
    phazite::undo (s);
    CHECK (song.valueOf ({ eq, "band1Gain" }) == doctest::Approx (0.0f));
    CHECK (song.valueOf ({ eq, "band2Gain" }) == doctest::Approx (6.0f));
    CHECK (song.valueOf ({ eq, "band4Gain" }) == doctest::Approx (-9.0f));
    CHECK (song.valueOf ({ eq, "output" }) == doctest::Approx (-3.0f));

    // Saved again, a preset names every parameter: choices by their names, switches as 0 or 1, and the rest as written.
    const auto saved = temp.folder.getChildFile ("Saved" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, eq, saved));
    const auto values = phazite::readPreset (saved)[phazite::schema::preset::device][phazite::schema::preset::parameters];
    REQUIRE (values.getDynamicObject() != nullptr);
    CHECK (values.getDynamicObject()->getProperties().size() == (int) info->parameters.size());
    CHECK (saved.loadFileAsString().contains ("\"output\": -3.0"));
    CHECK_FALSE (saved.loadFileAsString().contains ("00000"));
}

TEST_CASE ("A preset's choices are read by their names, in a device added from it and in one it loads into")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Choices");
    const auto file = temp.folder.getChildFile ("Slow" + juce::String (phazite::presetExtension));
    REQUIRE (file.replaceWithText (R"({ "kind": "device", "device": { "type": "phaziteMotion", "name": "Motion", "instrument": false,
                                        "parameters": { "division": "1 bar", "shape": "Sine" } } })"));

    // "1 bar" is a choice far from the one its text's number would pick, and "Sine" isn't the shape's default.
    const auto* info = phazite::findOwnDevice ("phaziteMotion");
    REQUIRE (info != nullptr);
    const auto& division = info->parameters[(size_t) phazite::devices::findParameter (*info, "division")];
    const auto& shape = info->parameters[(size_t) phazite::devices::findParameter (*info, "shape")];
    const auto bar = phazite::devices::findChoice (division, "1 bar");
    const auto sine = phazite::devices::findChoice (shape, "Sine");
    REQUIRE (bar.has_value());
    REQUIRE (sine.has_value());
    REQUIRE (*bar > 1);
    REQUIRE_FALSE (juce::exactlyEqual (shape.minimum + (float) *sine, shape.defaultValue));

    const auto added = phazite::addDeviceFromPreset (song.getSong(), phazite::getPluginHost (phazite::test::getEngineContext()),
                                                     song.track (0), phazite::readPreset (file), nullptr);
    REQUIRE (added.isValid());
    CHECK (song.valueOf ({ added, "division" }) == doctest::Approx (division.minimum + (float) *bar));
    CHECK (song.valueOf ({ added, "shape" }) == doctest::Approx (shape.minimum + (float) *sine));

    const auto chorus = song.addEffect (song.track (1), "phaziteMotion");
    REQUIRE (phazite::loadPresetInto (song.context, chorus, file));
    CHECK (song.valueOf ({ chorus, "division" }) == doctest::Approx (division.minimum + (float) *bar));
    CHECK (song.valueOf ({ chorus, "shape" }) == doctest::Approx (shape.minimum + (float) *sine));
}
