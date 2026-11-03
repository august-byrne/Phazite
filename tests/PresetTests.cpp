// SPDX-License-Identifier: AGPL-3.0-or-later
// Presets (src/model/presets/PresetModel.h; roadmap step 2.4): what a device's, a container's and a track's preset hold
// and leave out, in Phazite's own names with nothing of Tracktion's (roadmap A5, 2026-10-30), reading what a file holds,
// and putting presets into another project as one undo step. Its "done when": a container saved as a preset loads into
// another project with its macros working. And a device's preset loaded into a device of its kind, as a plug-in window's
// Load Preset does (step 2.12) and a device's Presets menu does.
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/dynamics/Compressor.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
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

TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

// A project in a folder of its own, open in a context, with two tracks and no undo history.
struct Song
{
    explicit Song (const juce::File& parent, const juce::String& name)
    {
        juce::String error;
        project = phazite::Project::create (phazite::test::getEngine(), parent, name, error);
        REQUIRE (project != nullptr);
        phazite::ensureTracks (edit(), 2);
        setEdit (context, &edit());
        phazite::getSong (edit()).getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    te::Edit& edit() const { return project->getEdit(); }
    TrackID track (int index) const { return idOf (*te::getAudioTracks (edit())[index]); }

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

    te::AutomatableParameter::Ptr find (const ParameterRef& ref) const { return phazite::findParameter (edit(), ref); }
    juce::String describeUndo() const { return phazite::describeUndo (&edit()); }

    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
};

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
    CHECK (second.find ({ inside[1].id, "makeup" })->getCurrentValue() == doctest::Approx (6.0f));

    const auto macros = phazite::getContainerMacros (second.context, container);
    REQUIRE (macros.size() == 8);
    CHECK (macros[0].name == juce::String ("Air"));

    // The macro modulates this project's EQ, and turning it moves the EQ.
    const ParameterRef shelf { inside[0].id, "band7Gain" };
    const auto modulations = phazite::getModulations (second.context, shelf);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].modulator == macros[0].id);
    CHECK (modulations[0].amount == doctest::Approx (0.5f));

    const auto before = second.find (shelf)->getCurrentValue();
    auto knob =
        phazite::getModulatorParameter (second.context, macros[0].id, phazite::getModulatorParameters (second.context, macros[0].id)[0].id);
    knob.setValue (1.0f);
    CHECK (second.find (shelf)->getCurrentValue() > before + 1.0f);

    // One undo step takes it all away.
    phazite::undo (second.edit());
    phazite::undo (second.edit());
    CHECK (phazite::getEffects (second.context, track).size() == 1);
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
    phazite::addAutomationPoint (context, makeup, { phazite::BeatPosition::fromBeats (4.0), 3.0f });
    const auto lfo = phazite::addModulator (context, track, ModulatorType::lfo);
    phazite::modulate (context, lfo, makeup, 0.25f);
    phazite::setSidechainSource (context, compressor, other);
    phazite::setDeviceOn (context, compressor, false);

    const auto preset = phazite::makeDevicePreset (*te::findPluginForID (song.edit(), phazite::toTracktion (compressor)));
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

    // Nothing that ties it to its project, and nothing of Tracktion's.
    CHECK (saved.getDynamicObject()->getProperties().size() == 4); // type, name, instrument and parameters
    const auto text = juce::JSON::toString (preset);
    for (const auto* tracktionName : { "PLUGIN", "AUTOMATIONCURVE", "MODIFIERASSIGNMENTS", "sidechainSourceID", "enabled" })
        CHECK_FALSE (text.contains (tracktionName));

    // Loaded below the tracks: an effect goes on the master, on.
    const auto file = temp.folder.getChildFile ("Squash" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::writePreset (preset, file));
    REQUIRE (phazite::loadPreset (context, file, {}));
    const auto master = phazite::getEffects (context, {});
    REQUIRE_FALSE (master.empty());
    CHECK (master.back().name == juce::String ("Compressor"));
    CHECK (master.back().isOn);
    CHECK (master.back().id != compressor);
    CHECK (song.find ({ master.back().id, "makeup" })->getCurrentValue() == doctest::Approx (-6.0f));
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
    CHECK (song.find ({ loaded->id, "cutoff" })->getCurrentValue() == doctest::Approx (1000.0f).epsilon (0.01));

    REQUIRE (phazite::loadPreset (context, file, {}));
    CHECK (te::getAudioTracks (song.edit()).size() == 3);
    CHECK (phazite::getInstrument (context, song.track (2)).has_value());
}

TEST_CASE ("A track's preset makes a new track with its devices, mix and modulation")
{
    phazite::test::TemporaryFolder temp;
    const auto file = temp.folder.getChildFile ("Wobble Bass" + juce::String (phazite::presetExtension));

    {
        Song first (temp.folder, "First");
        auto& context = first.context;
        const auto track = first.track (0);
        auto* bass = te::getAudioTracks (first.edit())[0];
        phazite::renameTrack (*bass, "Bass");
        phazite::setTrackColour (*bass, juce::Colours::orange);
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
        phazite::test::addMidiClip (*bass, { te::BeatPosition(), te::BeatPosition::fromBeats (4.0) });

        REQUIRE (phazite::saveTrackPreset (context, track, file));
    }

    const auto info = phazite::readPresetInfo (file);
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::PresetKind::track);

    Song second (temp.folder, "Second");
    auto& context = second.context;
    REQUIRE (phazite::loadPreset (context, file, second.track (0)));
    CHECK (second.describeUndo() == juce::String ("Undo Add Wobble Bass"));

    // After the track it was dropped on.
    auto tracks = te::getAudioTracks (second.edit());
    REQUIRE (tracks.size() == 3);
    auto* bass = tracks[1];
    const auto track = idOf (*bass);
    CHECK (bass->getName() == juce::String ("Bass"));
    CHECK (bass->getColour() == juce::Colours::orange);
    CHECK (bass->getClips().isEmpty());
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
    tracks = te::getAudioTracks (second.edit());
    REQUIRE (tracks.size() == 4);
    CHECK (tracks[3]->getName() == juce::String ("Bass"));
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
        auto handle = phazite::makeDeviceHandle (*te::findPluginForID (first.edit(), phazite::toTracktion (drums)));
        REQUIRE (handle.setPadSound (0, sound));
        REQUIRE (phazite::saveDevicePreset (context, drums, file));
    }

    // The preset names the sound by its full path, the project's copy of it.
    const auto preset = phazite::readPreset (file);
    const auto path = preset[keys::device][keys::sounds][phazite::schema::getPadSoundFile (0)].toString();
    CHECK (juce::File::isAbsolutePath (path));
    CHECK (juce::File (path).existsAsFile());

    Song second (temp.folder, "Second");
    auto& context = second.context;
    REQUIRE (phazite::loadPreset (context, file, second.track (0)));
    const auto drums = phazite::getInstrument (context, second.track (0));
    REQUIRE (drums.has_value());
    auto* plugin = te::findPluginForID (second.edit(), phazite::toTracktion (drums->id)).get();
    REQUIRE (plugin != nullptr);
    CHECK (plugin->state[phazite::schema::getPadSoundFile (0)].toString() == juce::String ("Audio/Boom.wav"));
    CHECK (second.project->getAudioFolder().getChildFile ("Boom.wav").existsAsFile());
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
    phazite::test::TestPlugin testPlugin;
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Levels");
    auto& edit = song.edit();
    auto* track = te::getAudioTracks (edit)[0];
    const auto addPlugin = [&edit, track]
    {
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (phazite::insertDevice (*track, phazite::test::TestPlugin::create (edit)));
        REQUIRE (plugin != nullptr);
        return plugin;
    };

    // A quiet, wet one, saved; and a loud, drier one.
    using phazite::test::TestPlugin;
    auto* quiet = addPlugin();
    auto* loud = addPlugin();
    TestPlugin::setGain (*quiet, 0.2f);
    phazite::setMix (*quiet, 0.6f);
    TestPlugin::setGain (*loud, 0.9f);
    phazite::setMix (*loud, 0.1f);
    phazite::getSong (edit).getUndoManager().clearUndoHistory();

    // Saving the preset stores the plug-in's settings in the project, but is no undo step.
    const auto file = temp.folder.getChildFile ("Quiet" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, phazite::getDeviceID (*quiet), file));
    CHECK_FALSE (phazite::getSong (edit).getUndoManager().canUndo());

    // Loaded into the loud one: its settings and mix, as one undo step named after the preset.
    REQUIRE (phazite::loadPresetInto (song.context, phazite::getDeviceID (*loud), file));
    CHECK (TestPlugin::getGain (*loud) == doctest::Approx (0.2f));
    CHECK (phazite::getMix (*loud) == doctest::Approx (0.6f));
    CHECK (song.describeUndo() == juce::String ("Undo Load Preset Quiet"));

    // Undo puts its own settings back, and plays them.
    phazite::undo (edit);
    CHECK (TestPlugin::getGain (*loud) == doctest::Approx (0.9f));
    CHECK (phazite::getMix (*loud) == doctest::Approx (0.1f));
    CHECK_FALSE (phazite::getSong (edit).getUndoManager().canUndo());

    // Not into a device of another kind, nor a container's or a track's preset: none is an undo step.
    const auto eq = song.addEffect (song.track (0), "phaziteEq");
    const auto eqFile = temp.folder.getChildFile ("Bright" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, eq, eqFile));
    const auto trackFile = temp.folder.getChildFile ("Lead" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveTrackPreset (song.context, song.track (0), trackFile));
    const auto container = phazite::groupIntoContainer (song.context, { eq });
    const auto containerFile = temp.folder.getChildFile ("Chain" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, container, containerFile));
    phazite::getSong (edit).getUndoManager().clearUndoHistory();

    CHECK_FALSE (phazite::loadPresetInto (song.context, phazite::getDeviceID (*loud), eqFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, phazite::getDeviceID (*loud), trackFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, phazite::getDeviceID (*loud), containerFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, phazite::getEffects (song.context, song.track (0)).back().id, containerFile));
    CHECK_FALSE (phazite::loadPresetInto (song.context, {}, file));
    CHECK_FALSE (phazite::getSong (edit).getUndoManager().canUndo());
    CHECK (TestPlugin::getGain (*loud) == doctest::Approx (0.9f));

    // One of Phazite's own takes a preset of its type: the EQ in the container, as its Presets menu loads one.
    const auto inside = phazite::getContainerDevices (song.context, container)[0].id;
    phazite::getParameter (song.context, { inside, "band4Gain" }).setValue (-9.0f);
    REQUIRE (phazite::loadPresetInto (song.context, inside, eqFile));
    CHECK (song.find ({ inside, "band4Gain" })->getCurrentValue() == doctest::Approx (0.0f));
    CHECK (song.describeUndo() == juce::String ("Undo Load Preset Bright"));
    phazite::undo (edit);
    CHECK (song.find ({ inside, "band4Gain" })->getCurrentValue() == doctest::Approx (-9.0f));
}

TEST_CASE ("A preset loaded into one of Phazite's devices sets every parameter: those it names, and the rest to their defaults")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Starting points");
    auto& edit = song.edit();
    const auto eq = song.addEffect (song.track (0), "phaziteEq");

    // The user's own settings, which a preset that names only one band's gain mustn't leave behind.
    phazite::getParameter (song.context, { eq, "band2Gain" }).setValue (6.0f);
    phazite::getParameter (song.context, { eq, "band4Gain" }).setValue (-9.0f);
    phazite::getParameter (song.context, { eq, "output" }).setValue (-3.0f);
    const auto file = temp.folder.getChildFile ("Lift" + juce::String (phazite::presetExtension));
    REQUIRE (file.replaceWithText (R"({ "kind": "device", "device": { "type": "phaziteEq", "name": "EQ", "instrument": false,
                                        "parameters": { "band1Gain": 3.0 } } })"));
    phazite::getSong (edit).getUndoManager().clearUndoHistory();

    // Loaded: as a new EQ would be with the preset, every parameter.
    REQUIRE (phazite::loadPresetInto (song.context, eq, file));
    const phazite::devices::DeviceInfo* info = nullptr;

    for (const auto* own : phazite::getOwnDevices())
        if (own->typeID == std::string_view ("phaziteEq"))
            info = own;

    REQUIRE (info != nullptr);

    for (const auto& parameter : info->parameters)
    {
        const auto id = juce::String (parameter.id.data(), parameter.id.size());
        CAPTURE (id.toStdString());
        CHECK (song.find ({ eq, id })->getCurrentValue() == doctest::Approx (id == "band1Gain" ? 3.0f : parameter.defaultValue));
    }

    // One undo step puts the user's settings back.
    CHECK (song.describeUndo() == juce::String ("Undo Load Preset Lift"));
    phazite::undo (edit);
    CHECK (song.find ({ eq, "band1Gain" })->getCurrentValue() == doctest::Approx (0.0f));
    CHECK (song.find ({ eq, "band2Gain" })->getCurrentValue() == doctest::Approx (6.0f));
    CHECK (song.find ({ eq, "band4Gain" })->getCurrentValue() == doctest::Approx (-9.0f));
    CHECK (song.find ({ eq, "output" })->getCurrentValue() == doctest::Approx (-3.0f));

    // Saved again, a preset names every parameter, choices and switches as whole numbers and the rest as written.
    const auto saved = temp.folder.getChildFile ("Saved" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, eq, saved));
    const auto values = phazite::readPreset (saved)[phazite::schema::preset::device][phazite::schema::preset::parameters];
    REQUIRE (values.getDynamicObject() != nullptr);
    CHECK (values.getDynamicObject()->getProperties().size() == (int) info->parameters.size());
    CHECK (saved.loadFileAsString().contains ("\"output\": -3.0"));
    CHECK_FALSE (saved.loadFileAsString().contains ("00000"));
}
