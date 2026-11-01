// SPDX-License-Identifier: AGPL-3.0-or-later
// Modulation by IDs and plain values (src/model/modulation/ModulationModel.h; roadmap step 2.3): a track's modulators and
// macros, added and removed as named undo steps; what they modulate, by how much, and what that does to a parameter as
// the song plays; a modulator's parameters and a step sequencer's steps; and that they are saved with the project.
#include "GoldenFiles.h"
#include "RealTimePlayback.h"
#include "TestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

namespace
{
using phazite::ModulatorID;
using phazite::ModulatorType;
using phazite::ParameterRef;

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

// An open project with two tracks, A (with a synth, then a compressor) and B, and no undo history.
struct Song
{
    Song()
    {
        phazite::ensureTracks (*edit, 2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        setEdit (context, edit.get());
        phazite::chooseInstrument (context, idOf (*a), find ("phaziteSynth"));
        phazite::addEffect (context, idOf (*a), find ("phaziteCompressor"));
        phazite::getSongUndoManager (*edit).clearUndoHistory();
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

    // A's compressor's makeup gain, from -12 to 24 dB, and its synth's cutoff.
    ParameterRef makeup() const { return { phazite::getEffects (context, idOf (*a))[0].id, "makeup" }; }
    ParameterRef cutoff() const { return { phazite::getInstrument (context, idOf (*a))->id, "cutoff" }; }

    te::AutomatableParameter::Ptr find (const ParameterRef& ref) const { return phazite::findParameter (*edit, ref); }

    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::vector<juce::String> namesOf (phazite::TrackID track) const
    {
        std::vector<juce::String> names;

        for (const auto& modulator : phazite::getModulators (context, track))
            names.push_back (modulator.name);

        return names;
    }

    std::unique_ptr<te::Edit> edit = phazite::createSongEdit (phazite::test::getEngine()); // 120 BPM, 4/4
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};
} // namespace

TEST_CASE ("A track's modulators and macros are added and removed, each one named undo step")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);

    const auto lfo = phazite::addModulator (context, a, ModulatorType::lfo);
    REQUIRE (lfo.isValid());
    CHECK (song.describeUndo() == juce::String ("Undo Add LFO"));
    const auto steps = phazite::addModulator (context, a, ModulatorType::steps);
    const auto macro = phazite::addModulator (context, a, ModulatorType::macro);
    const auto secondLfo = phazite::addModulator (context, a, ModulatorType::lfo);
    REQUIRE (steps.isValid());
    REQUIRE (macro.isValid());
    REQUIRE (secondLfo.isValid());

    // Modulators in their order, then macros, each named after its kind.
    CHECK (song.namesOf (a) == std::vector<juce::String> { "LFO 1", "Step Sequencer 1", "LFO 2", "Macro 1" });
    const auto info = phazite::getModulator (context, macro);
    REQUIRE (info.has_value());
    CHECK (info->type == ModulatorType::macro);
    CHECK (info->track == a);
    CHECK (song.namesOf (idOf (*song.b)).empty());

    phazite::removeModulator (context, lfo);
    CHECK (song.describeUndo() == juce::String ("Undo Remove LFO 1"));
    CHECK (song.namesOf (a) == std::vector<juce::String> { "Step Sequencer 1", "LFO 1", "Macro 1" });
    phazite::undo (*song.edit);
    CHECK (song.namesOf (a).size() == 4);

    // Every kind of modulator; folders and the master hold modulators, but not macros.
    for (const auto type : phazite::allModulatorTypes)
        CHECK (phazite::addModulator (context, idOf (*song.b), type).isValid());

    auto* folder = phazite::groupIntoFolder ({ song.b }, nullptr);
    REQUIRE (folder != nullptr);
    CHECK (phazite::addModulator (context, idOf (*folder), ModulatorType::random).isValid());
    CHECK_FALSE (phazite::addModulator (context, idOf (*folder), ModulatorType::macro).isValid());
    CHECK (phazite::addModulator (context, phazite::getMasterTrack (context), ModulatorType::lfo).isValid());
}

TEST_CASE ("Modulators and macros are the song's, with what they modulate, which Tracktion's mirror one way")
{
    namespace schema = phazite::schema;
    Song song;
    auto& context = song.context;
    auto& edit = *song.edit;
    const auto a = idOf (*song.a);

    // The song's LFO: its kind, as Phazite starts one (both ways); its rate, its switch and what it modulates.
    const auto lfo = phazite::addModulator (context, a, ModulatorType::lfo);
    const auto element = phazite::findModulatorState (edit, lfo);
    REQUIRE (element.isValid());
    CHECK (element[schema::modulatorType].toString() == juce::String ("lfo"));
    CHECK ((float) element.getChildWithName (schema::VALUES)["biopolar"] == doctest::Approx (1.0f));
    CHECK (phazite::isBipolar (context, lfo));

    phazite::getModulatorParameter (context, lfo, "rate").setValue (4.0f);
    CHECK ((float) element.getChildWithName (schema::VALUES)["rate"] == doctest::Approx (4.0f));
    phazite::setModulatorOn (context, lfo, false);
    CHECK ((bool) element[schema::deviceOff]);
    CHECK_FALSE (phazite::isModulatorOn (context, lfo));
    phazite::setModulatorOn (context, lfo, true);
    phazite::modulate (context, lfo, song.cutoff(), 0.5f);
    const auto modulates = element.getChildWithName (schema::MODULATES);
    CHECK (modulates[schema::modulatesParameter].toString() == juce::String ("cutoff"));
    CHECK ((double) modulates[schema::modulatesAmount] == doctest::Approx (0.5));
    CHECK (phazite::getModulations (context, song.cutoff()).size() == 1);
    phazite::undo (edit);
    CHECK_FALSE (element.getChildWithName (schema::MODULATES).isValid());
    CHECK (phazite::getModulations (context, song.cutoff()).empty());
    phazite::redo (edit);
    CHECK (phazite::getModulations (context, song.cutoff()).size() == 1);

    // A macro's name and value, and a step sequencer's steps, are the song's.
    const auto macro = phazite::addModulator (context, a, ModulatorType::macro);
    const auto macroElement = phazite::findModulatorState (edit, macro);
    REQUIRE (macroElement.hasType (schema::MACRO));
    phazite::renameMacro (context, macro, "Air");
    CHECK (macroElement[schema::macroName].toString() == juce::String ("Air"));
    phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro)[0].id).setValue (0.25f);
    CHECK ((float) macroElement[schema::macroValue] == doctest::Approx (0.25f));
    const auto steps = phazite::addModulator (context, a, ModulatorType::steps);
    phazite::setStep (context, steps, 1, 0.5f);
    CHECK (phazite::getModulatorSteps (phazite::findModulatorState (edit, steps))[1] == doctest::Approx (0.5f));
    CHECK (phazite::getSteps (context, steps)[1] == doctest::Approx (0.5f));

    // One way: a modulator Tracktion lost is made again from the song, with its rate and what it modulates.
    phazite::findModulator (edit, lfo)->modifier->remove();
    CHECK (phazite::getModulations (context, song.cutoff()).empty());
    phazite::mirror::reconcileModulation (edit);
    REQUIRE (phazite::getModulations (context, song.cutoff()).size() == 1);
    CHECK (phazite::getModulations (context, song.cutoff())[0].modulator == lfo);
    CHECK (phazite::getModulatorParameter (context, lfo, "rate").getValue() == doctest::Approx (4.0f));

    phazite::getSongUndoManager (edit).clearUndoHistory();
    phazite::mirror::reconcileModulation (edit);
    CHECK_FALSE (phazite::getSongUndoManager (edit).canUndo());

    // Removed, it goes from both, with what it modulates.
    phazite::removeModulator (context, lfo);
    CHECK_FALSE (phazite::findModulatorState (edit, lfo).isValid());
    CHECK (phazite::getModulations (context, song.cutoff()).empty());
}

TEST_CASE ("A modulator modulates its track's parameters by an amount, each change one named undo step")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);
    const auto makeup = song.makeup();
    const auto lfo = phazite::addModulator (context, a, ModulatorType::lfo);

    CHECK (phazite::canModulate (context, lfo, makeup));
    CHECK (phazite::canModulate (context, lfo, phazite::getVolume (context, a).getRef()));
    CHECK_FALSE (phazite::canModulate (context, lfo, phazite::getVolume (context, idOf (*song.b)).getRef())); // another track's
    CHECK_FALSE (phazite::canModulate (context, lfo, { phazite::DeviceID::fromRaw (lfo.getRaw()), "rate" })); // its own

    phazite::modulate (context, lfo, makeup, 0.5f);
    CHECK (song.describeUndo() == juce::String ("Undo Modulate Makeup"));
    auto modulations = phazite::getModulations (context, makeup);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].modulator == lfo);
    CHECK (modulations[0].parameter == makeup);
    CHECK (modulations[0].amount == doctest::Approx (0.5f));
    CHECK (phazite::getModulations (context, lfo).size() == 1);

    // Again: its amount changes, and amounts stay from -1 to 1.
    phazite::modulate (context, lfo, makeup, -3.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Modulation"));
    modulations = phazite::getModulations (context, makeup);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].amount == doctest::Approx (-1.0f));

    // A modulator can't modulate another track's parameters.
    phazite::modulate (context, lfo, phazite::getVolume (context, idOf (*song.b)).getRef(), 0.5f);
    CHECK (phazite::getModulations (context, phazite::getVolume (context, idOf (*song.b)).getRef()).empty());

    phazite::removeModulation (context, lfo, makeup);
    CHECK (song.describeUndo() == juce::String ("Undo Remove Modulation"));
    CHECK (phazite::getModulations (context, makeup).empty());

    // Removing a modulator takes what it modulates with it, and undo puts both back.
    phazite::modulate (context, lfo, makeup, 0.25f);
    phazite::removeModulator (context, lfo);
    CHECK (phazite::getModulations (context, makeup).empty());
    phazite::undo (*song.edit);
    CHECK (phazite::getModulations (context, makeup).size() == 1);
}

TEST_CASE ("A macro moves what it modulates as it turns")
{
    Song song;
    auto& context = song.context;
    const auto makeup = song.makeup();
    phazite::getParameter (context, makeup).setValue (-6.0f);
    const auto macro = phazite::addModulator (context, idOf (*song.a), ModulatorType::macro);
    const auto parameters = phazite::getModulatorParameters (context, macro);
    REQUIRE (parameters.size() == 1);
    CHECK (parameters[0].name == juce::String ("Macro 1"));

    phazite::modulate (context, macro, makeup, 0.5f);
    auto knob = phazite::getModulatorParameter (context, macro, parameters[0].id);
    REQUIRE (knob.isValid());

    // A macro at 0 leaves it where it is; turned all the way, it adds half the range (18 dB).
    knob.setValue (0.0f);
    CHECK (song.find (makeup)->getCurrentValue() == doctest::Approx (-6.0f).epsilon (0.01));
    knob.setValue (1.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Macro 1"));
    CHECK (song.find (makeup)->getCurrentValue() == doctest::Approx (12.0f).epsilon (0.01));

    // Its value, not its modulation, is what the parameter is set to.
    CHECK (song.find (makeup)->getCurrentExplicitValue() == doctest::Approx (-6.0f));
}

TEST_CASE ("A control's modulation: a new modulator for it, its modulators, and dragging the amount")
{
    Song song;
    auto& context = song.context;
    const auto a = idOf (*song.a);
    auto handle = phazite::getParameter (context, song.makeup());
    CHECK_FALSE (phazite::getModulationRange (handle).has_value());

    const auto lfo = phazite::modulateWithNew (handle, ModulatorType::lfo, phazite::defaultModulationAmount);
    REQUIRE (lfo.isValid());
    CHECK (song.describeUndo() == juce::String ("Undo Modulate Makeup"));
    CHECK (song.namesOf (a) == std::vector<juce::String> { "LFO 1" });

    // One undo step took both back.
    phazite::undo (*song.edit);
    CHECK (song.namesOf (a).empty());
    phazite::redo (*song.edit);
    REQUIRE (phazite::getModulations (handle).size() == 1);

    const auto modulators = phazite::getModulatorsFor (handle);
    REQUIRE (modulators.size() == 1);
    CHECK (modulators[0].id == lfo);

    // The ring: a new LFO moves both ways, a quarter of the range each.
    handle.setValue (6.0f); // the middle
    const auto range = phazite::getModulationRange (handle);
    REQUIRE (range.has_value());
    CHECK (range->low == doctest::Approx (0.25f));
    CHECK (range->high == doctest::Approx (0.75f));

    {
        auto drag = phazite::beginModulationDrag (handle, lfo);
        REQUIRE (drag != nullptr);
        CHECK (drag->getStartAmount() == doctest::Approx (0.25f));
        drag->setAmount (0.4f);
        drag->setAmount (0.5f);
    }

    CHECK (phazite::getModulations (handle)[0].amount == doctest::Approx (0.5f));
    CHECK (song.describeUndo() == juce::String ("Undo Change Modulation"));
    phazite::undo (*song.edit);
    CHECK (phazite::getModulations (handle)[0].amount == doctest::Approx (0.25f));

    phazite::removeModulation (handle, lfo);
    CHECK (phazite::getModulations (handle).empty());
    CHECK_FALSE (phazite::getModulationRange (handle).has_value());
}

TEST_CASE ("A modulator's parameters have names of their own, and a step sequencer's steps are edited one at a time")
{
    Song song;
    auto& context = song.context;
    const auto lfo = phazite::addModulator (context, idOf (*song.a), ModulatorType::lfo);
    const auto parameters = phazite::getModulatorParameters (context, lfo);
    const auto has = [&parameters] (const char* name)
    { return std::any_of (parameters.begin(), parameters.end(), [name] (const auto& p) { return p.name == juce::String (name); }); };
    CHECK (has ("Wave"));
    CHECK (has ("Rate"));
    CHECK (has ("Depth"));
    CHECK (has ("Polarity"));
    CHECK (phazite::isBipolar (context, lfo));

    auto depth = phazite::getModulatorParameter (context, lfo, "depth");
    REQUIRE (depth.isValid());
    depth.setValue (0.5f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Depth"));

    const auto steps = phazite::addModulator (context, idOf (*song.a), ModulatorType::steps);
    auto values = phazite::getSteps (context, steps);
    REQUIRE (values.size() >= 2);
    CHECK (values.front() == doctest::Approx (-1.0f)); // rising, from the bottom to the top
    CHECK (values.back() == doctest::Approx (1.0f));

    phazite::setStep (context, steps, 1, 0.5f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Step"));
    CHECK (phazite::getSteps (context, steps)[1] == doctest::Approx (0.5f));
    phazite::undo (*song.edit);
    CHECK (phazite::getSteps (context, steps)[1] == doctest::Approx (values[1]));
}

TEST_CASE ("An LFO moves the synth's cutoff as the song plays")
{
    Song song;
    auto& context = song.context;
    const auto cutoff = song.cutoff();
    auto handle = phazite::getParameter (context, cutoff);
    const auto lfo = phazite::modulateWithNew (handle, ModulatorType::lfo, 0.3f);
    REQUIRE (lfo.isValid());
    phazite::getModulatorParameter (context, lfo, "rate").setValue (8.0f); // fast enough to see within a second

    const auto parameter = song.find (cutoff);
    const auto base = parameter->getCurrentBaseValue();
    const phazite::test::RealTimePlayback playback (phazite::test::getEngine());
    auto& transport = song.edit->getTransport();
    transport.setPosition (te::TimePosition());
    transport.play (false);

    // Sampled as it plays: it moves, around where it is set.
    std::vector<float> seen;

    for (int i = 0; i < 20; ++i)
    {
        phazite::test::RealTimePlayback::wait (25);
        seen.push_back (parameter->getCurrentValue());
    }

    transport.stop (false, false);
    phazite::test::RealTimePlayback::wait (200);

    const auto [lowest, highest] = std::minmax_element (seen.begin(), seen.end());
    CHECK (*highest > *lowest);
    CHECK (parameter->getCurrentBaseValue() == doctest::Approx (base));
}

TEST_CASE ("Modulators and what they modulate are saved with the project, and playing them is no unsaved change")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Modulated", error);
    REQUIRE (project != nullptr);
    phazite::test::settle();

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &project->getEdit());
    const auto track = idOf (*te::getAudioTracks (project->getEdit())[0]);
    auto volume = phazite::getVolume (context, track);
    const auto lfo = phazite::modulateWithNew (volume, ModulatorType::lfo, 0.5f);
    const auto macro = phazite::addModulator (context, track, ModulatorType::macro);
    phazite::modulate (context, macro, phazite::getVolume (context, track).getRef(), 0.25f);
    REQUIRE (project->save().wasOk());

    // What Tracktion writes into the device's state as the modulation plays is no change.
    auto* volumePlugin = te::getAudioTracks (project->getEdit())[0]->getVolumePlugin();
    volumePlugin->state.setProperty (te::IDs::volume, 0.123f, nullptr);
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());

    const auto file = project->getFile();
    setEdit (context, nullptr);
    project = nullptr;
    project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (project != nullptr);
    setEdit (context, &project->getEdit());

    CHECK (phazite::getModulators (context, track).size() == 2);
    const auto modulations = phazite::getModulations (context, phazite::getVolume (context, track).getRef());
    REQUIRE (modulations.size() == 2);
    CHECK (modulations[0].modulator == lfo);
    CHECK (modulations[0].amount == doctest::Approx (0.5f));
    CHECK (modulations[1].modulator == macro);
    setEdit (context, nullptr);
}

TEST_CASE ("A modulator switched off moves nothing and keeps what it modulates, one undo step either way")
{
    Song song;
    auto& context = song.context;
    const auto cutoff = song.cutoff();
    auto handle = phazite::getParameter (context, cutoff);
    const auto lfo = phazite::modulateWithNew (handle, ModulatorType::lfo, 0.3f);
    REQUIRE (lfo.isValid());
    phazite::getModulatorParameter (context, lfo, "rate").setValue (8.0f); // fast enough to see within a second
    CHECK (phazite::isModulatorOn (context, lfo));
    CHECK (phazite::getModulator (context, lfo)->isOn);
    REQUIRE (phazite::getModulationRange (handle).has_value());
    CHECK (phazite::getModulationRange (handle)->isOn);

    phazite::setModulatorOn (context, lfo, false);
    CHECK (song.describeUndo() == juce::String ("Undo Turn Modulator Off"));
    CHECK_FALSE (phazite::isModulatorOn (context, lfo));
    CHECK_FALSE (phazite::getModulator (context, lfo)->isOn);
    CHECK (phazite::getModulations (context, lfo).size() == 1);

    // Its knob's ring shows where it would move the cutoff, as switched off.
    const auto range = phazite::getModulationRange (handle);
    REQUIRE (range.has_value());
    CHECK_FALSE (range->isOn);
    CHECK (range->high > range->low);

    // Played, the cutoff stays where it is set.
    const auto parameter = song.find (cutoff);
    const auto base = parameter->getCurrentBaseValue();

    const auto play = [&song, &parameter]
    {
        const phazite::test::RealTimePlayback playback (phazite::test::getEngine());
        auto& transport = song.edit->getTransport();
        transport.setPosition (te::TimePosition());
        transport.play (false);
        std::vector<float> seen;

        for (int i = 0; i < 16; ++i)
        {
            phazite::test::RealTimePlayback::wait (25);
            seen.push_back (parameter->getCurrentValue());
        }

        transport.stop (false, false);
        phazite::test::RealTimePlayback::wait (200);
        return seen;
    };

    for (const auto value : play())
        CHECK (value == doctest::Approx (base));

    // Undone, it is on, and moves the cutoff again; redone, off.
    phazite::undo (*song.edit);
    CHECK (phazite::isModulatorOn (context, lfo));
    const auto seen = play();
    const auto [lowest, highest] = std::minmax_element (seen.begin(), seen.end());
    CHECK (*highest > *lowest);

    phazite::redo (*song.edit);
    CHECK_FALSE (phazite::isModulatorOn (context, lfo));
    phazite::setModulatorOn (context, lfo, true);
    CHECK (song.describeUndo() == juce::String ("Undo Turn Modulator On"));

    // A macro has no switch.
    const auto macro = phazite::addModulator (context, idOf (*song.a), ModulatorType::macro);
    phazite::setModulatorOn (context, macro, false);
    CHECK (phazite::isModulatorOn (context, macro));
    CHECK (song.describeUndo() == juce::String ("Undo Add Macro"));
}

TEST_CASE ("Undoing the switch of a modulator on the master plays it as it was")
{
    // The master's modulators are left out of Tracktion's list of an Edit's parameters, which undo puts back.
    Song song;
    auto& context = song.context;
    const auto lfo = phazite::addModulator (context, phazite::getMasterTrack (context), ModulatorType::lfo);
    REQUIRE (lfo.isValid());

    phazite::setModulatorOn (context, lfo, false);
    CHECK_FALSE (phazite::isModulatorOn (context, lfo));
    phazite::undo (*song.edit);
    CHECK (phazite::isModulatorOn (context, lfo)); // what Tracktion's modulation reads, not only what is stored
    phazite::redo (*song.edit);
    CHECK_FALSE (phazite::isModulatorOn (context, lfo));
}
