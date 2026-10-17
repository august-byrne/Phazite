// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers by IDs (src/model/mixer/ContainerModel.h; roadmap step 2.4): effects grouped into a container and back as
// named undo steps, the container's row of devices and its eight macros, which modulate its devices only, its devices
// reached by the mixer's functions, its own rack type for each container (a duplicated track's too), and the commands
// that group and ungroup the selected devices.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::DeviceID;
using phazite::ModulatorType;
using phazite::ParameterRef;

phazite::TrackID idOf (const te::Track& track)
{
    return phazite::getTrackID (track);
}

// An open project with two tracks: A, with a synth, then an EQ, a compressor and a reverb; and B, with an EQ.
struct Song
{
    Song()
    {
        edit->ensureNumberOfAudioTracks (2);
        a = te::getAudioTracks (*edit)[0];
        b = te::getAudioTracks (*edit)[1];
        setEdit (context, edit.get());
        phazite::chooseInstrument (context, idOf (*a), find ("phaziteSynth"));

        for (const auto* effect : { "phaziteEq", "phaziteCompressor", "phaziteReverb" })
            phazite::addEffect (context, idOf (*a), find (effect));

        phazite::addEffect (context, idOf (*b), find ("phaziteEq"));
        edit->getUndoManager().clearUndoHistory();
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

    std::vector<DeviceID> effectsOf (const te::Track& track) const
    {
        std::vector<DeviceID> ids;

        for (const auto& effect : phazite::getEffects (context, idOf (track)))
            ids.push_back (effect.id);

        return ids;
    }

    std::vector<juce::String> namesOf (const std::vector<phazite::DeviceSlot>& slots) const
    {
        std::vector<juce::String> names;

        for (const auto& slot : slots)
            names.push_back (slot.name);

        return names;
    }

    std::vector<juce::String> effectNamesOf (const te::Track& track) const { return namesOf (phazite::getEffects (context, idOf (track))); }

    te::AutomatableParameter::Ptr find (const ParameterRef& ref) const { return phazite::findParameter (*edit, ref); }
    juce::String describeUndo() const { return phazite::describeUndo (edit.get()); }

    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::AudioTrack* a = nullptr;
    te::AudioTrack* b = nullptr;
};

// A's EQ and reverb grouped into a container.
DeviceID groupEqAndReverb (Song& song)
{
    const auto effects = song.effectsOf (*song.a);
    return phazite::groupIntoContainer (song.context, { effects[0], effects[2] });
}
} // namespace

TEST_CASE ("Effects of one channel group into a container in the first one's place, as one undo step, and ungroup again")
{
    Song song;
    auto& context = song.context;
    const auto effects = song.effectsOf (*song.a);
    const auto instrument = phazite::getInstrument (context, idOf (*song.a))->id;
    const auto eq = effects[0], compressor = effects[1], reverb = effects[2];

    // Not an instrument, nor effects of two channels.
    CHECK (phazite::canGroupIntoContainer (context, { eq, reverb }));
    CHECK_FALSE (phazite::canGroupIntoContainer (context, { instrument, eq }));
    CHECK_FALSE (phazite::canGroupIntoContainer (context, { eq, song.effectsOf (*song.b)[0] }));
    CHECK_FALSE (phazite::canGroupIntoContainer (context, {}));

    // In the EQ's place, holding the EQ and the reverb in their order, which keep their IDs.
    const auto container = phazite::groupIntoContainer (context, { reverb, eq });
    REQUIRE (container.isValid());
    CHECK (song.describeUndo() == juce::String ("Undo Group into Container"));
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Container", "Compressor" });
    CHECK (song.effectsOf (*song.a)[1] == compressor);
    CHECK (phazite::getEffects (context, idOf (*song.a))[0].isContainer);
    CHECK (phazite::getInstrument (context, idOf (*song.a))->id == instrument); // a container isn't an instrument

    const auto inside = phazite::getContainerDevices (context, container);
    REQUIRE (inside.size() == 2);
    CHECK (inside[0].id == eq);
    CHECK (inside[1].id == reverb);
    CHECK (phazite::getContainerOf (context, eq) == container);
    CHECK_FALSE (phazite::getContainerOf (context, compressor).isValid());

    // Eight macros, each at 0.
    const auto macros = phazite::getContainerMacros (context, container);
    REQUIRE (macros.size() == 8);
    CHECK (macros[0].name == juce::String ("Macro 1"));
    CHECK (macros[7].name == juce::String ("Macro 8"));
    CHECK (macros[0].container == container);
    CHECK (macros[0].track == idOf (*song.a));
    CHECK (phazite::getModulatorValue (context, macros[0].id) == doctest::Approx (0.0f));

    // A second one is named apart; a container can't go in a container.
    const auto other = phazite::groupIntoContainer (context, { compressor });
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Container", "Container 2" });
    CHECK_FALSE (phazite::canGroupIntoContainer (context, { container, other }));

    // Undo puts the effects back where they were; redo groups them again.
    phazite::undo (*song.edit);
    phazite::undo (*song.edit);
    CHECK (song.effectsOf (*song.a) == std::vector<DeviceID> { eq, compressor, reverb });
    CHECK_FALSE (phazite::getContainerOf (context, eq).isValid());
    phazite::redo (*song.edit);
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Container", "Compressor" });
    CHECK (phazite::getContainerDevices (context, container).size() == 2);

    // Ungrouping puts them in its place, in their order, and the container goes.
    phazite::ungroupContainer (context, container);
    CHECK (song.describeUndo() == juce::String ("Undo Ungroup Container"));
    CHECK (song.effectsOf (*song.a) == std::vector<DeviceID> { eq, reverb, compressor });
    CHECK (phazite::getContainerDevices (context, container).empty());
    phazite::undo (*song.edit);
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Container", "Compressor" });
}

TEST_CASE ("A container's devices are wired one after the other, and move, come and go as the mixer's devices do")
{
    Song song;
    auto& context = song.context;
    const auto container = groupEqAndReverb (song);
    auto* rack = phazite::getContainerRack (*te::findPluginForID (*song.edit, phazite::toTracktion (container)));
    REQUIRE (rack != nullptr);

    // From the input through each device to the output: MIDI and two channels at each step.
    const auto wiredInOrder = [rack]
    {
        const auto devices = phazite::getContainerDevices (*phazite::findContainerOf (*rack));
        auto from = te::EditItemID();
        auto ok = true;

        for (auto* device : devices)
        {
            for (int pin = 0; pin <= 2; ++pin)
            {
                const auto connections = rack->getConnections();
                ok = ok
                     && std::any_of (
                         connections.begin(), connections.end(), [&] (const te::RackConnection* c)
                         { return c->sourceID.get() == from && c->destID.get() == device->itemID && c->sourcePin.get() == pin; });
            }

            from = device->itemID;
        }

        const auto connections = rack->getConnections();
        return ok && connections.size() == 3 * (devices.size() + 1)
               && std::any_of (connections.begin(), connections.end(),
                               [&] (const te::RackConnection* c) { return c->sourceID.get() == from && ! c->destID.get().isValid(); });
    };

    CHECK (wiredInOrder());

    // An effect added at its end.
    phazite::addEffectToContainer (context, container, song.find ("phaziteCompressor"));
    CHECK (song.describeUndo() == juce::String ("Undo Add Compressor"));
    CHECK (song.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "EQ", "Reverb", "Compressor" });
    CHECK (wiredInOrder());

    // Moved earlier, among its container's devices only.
    const auto added = phazite::getContainerDevices (context, container)[2].id;
    phazite::moveEffect (context, added, -2);
    CHECK (song.describeUndo() == juce::String ("Undo Move Device"));
    CHECK (song.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "Compressor", "EQ", "Reverb" });
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Container", "Compressor" });
    CHECK (wiredInOrder());

    // Turned off, and removed from it.
    phazite::setDeviceOn (context, added, false);
    CHECK_FALSE (phazite::getContainerDevices (context, container)[0].isOn);
    phazite::removeDevice (context, added);
    CHECK (song.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "EQ", "Reverb" });
    CHECK (wiredInOrder());

    // A container's key controls are its first macros, and it has a mix of its own.
    const auto controls = phazite::getKeyControls (context, container, 3);
    REQUIRE (controls.size() == 3);
    CHECK (controls[0].getName() == juce::String ("Macro 1"));
    CHECK (phazite::getEffects (context, idOf (*song.a))[0].hasMix);
    CHECK (phazite::getMix (context, container).getValue() == doctest::Approx (1.0f));

    // Renamed, and removed, which undo brings back.
    phazite::renameContainer (context, container, "  Space  ");
    CHECK (song.describeUndo() == juce::String ("Undo Rename Container"));
    CHECK (phazite::getEffects (context, idOf (*song.a))[0].name == juce::String ("Space"));
    phazite::removeDevice (context, container);
    CHECK (song.describeUndo() == juce::String ("Undo Remove Space"));
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Compressor" });
    phazite::undo (*song.edit);
    CHECK (song.effectNamesOf (*song.a) == std::vector<juce::String> { "Space", "Compressor" });
    CHECK (phazite::getContainerDevices (context, container).size() == 2);
}

TEST_CASE ("A container's macros modulate its devices; its track's modulators reach them too")
{
    Song song;
    auto& context = song.context;
    const auto container = groupEqAndReverb (song);
    const auto inside = phazite::getContainerDevices (context, container);
    const ParameterRef gain { inside[0].id, "band4Gain" };                     // the EQ's, inside
    const ParameterRef threshold { song.effectsOf (*song.a)[1], "threshold" }; // the compressor's, outside
    const auto macro = phazite::getContainerMacros (context, container)[0].id;

    CHECK (phazite::canModulate (context, macro, gain));
    CHECK_FALSE (phazite::canModulate (context, macro, threshold));

    // Turning the macro moves the EQ's gain by the amount, from where it is: at 0, not at all.
    phazite::modulate (context, macro, gain, 0.5f);
    CHECK (song.describeUndo() == juce::String ("Undo Modulate Band 4 Gain"));
    const auto range = song.find (gain)->valueRange;
    CHECK (song.find (gain)->getCurrentValue() == doctest::Approx (0.0f).epsilon (0.001));
    auto knob = phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro)[0].id);
    REQUIRE (knob.isValid());
    knob.setValue (1.0f);
    CHECK (song.describeUndo() == juce::String ("Undo Change Macro 1"));
    CHECK (song.find (gain)->getCurrentValue()
           == doctest::Approx (range.convertFrom0to1 (range.convertTo0to1 (0.0f) + 0.5f)).epsilon (0.01));

    // The track's LFO reaches the devices in its containers, and a control in the container offers the container's
    // macros first.
    const auto lfo = phazite::addModulator (context, idOf (*song.a), ModulatorType::lfo);
    CHECK (phazite::canModulate (context, lfo, gain));
    const auto handle = phazite::getParameter (context, gain);
    const auto offered = phazite::getModulatorsFor (handle);
    REQUIRE (offered.size() == 9);
    CHECK (offered[0].id == macro);
    CHECK (offered[8].id == lfo);
    CHECK (phazite::getModulatorsFor (phazite::getParameter (context, threshold)).size() == 1);

    // A macro is renamed, but stays: the container keeps all eight.
    phazite::renameMacro (context, macro, "Air");
    CHECK (song.describeUndo() == juce::String ("Undo Rename Macro"));
    CHECK (phazite::getContainerMacros (context, container)[0].name == juce::String ("Air"));
    phazite::removeModulator (context, macro);
    CHECK (phazite::getContainerMacros (context, container).size() == 8);

    // Ungrouping takes the macros' modulation with them, and keeps the LFO's.
    phazite::modulate (context, lfo, gain, 0.25f);
    phazite::ungroupContainer (context, container);
    const auto left = phazite::getModulations (context, gain);
    REQUIRE (left.size() == 1);
    CHECK (left[0].modulator == lfo);
}

TEST_CASE ("A container's devices' parameters can be automated on its track")
{
    Song song;
    auto& context = song.context;
    const auto container = groupEqAndReverb (song);
    const auto inside = phazite::getContainerDevices (context, container);

    const auto targets = phazite::getAutomationTargets (context, inside[0].id);
    REQUIRE_FALSE (targets.empty());
    CHECK (phazite::getParameter (context, targets[0].parameter).isValid());

    // Its lane shows under the track, and its points play.
    phazite::showAutomationLane (context, targets[0].parameter);
    const auto lanes = phazite::getAutomationLanes (context, idOf (*song.a));
    CHECK (std::any_of (lanes.begin(), lanes.end(), [&] (const auto& lane) { return lane.parameter == targets[0].parameter; }));
}

TEST_CASE ("A duplicated track's containers play rack types of their own, which its own modulators modulate")
{
    Song song;
    auto& context = song.context;
    const auto container = groupEqAndReverb (song);
    const auto inside = phazite::getContainerDevices (context, container);
    const ParameterRef gain { inside[0].id, "band4Gain" };
    const auto macro = phazite::getContainerMacros (context, container)[0].id;
    const auto lfo = phazite::addModulator (context, idOf (*song.a), ModulatorType::lfo);
    phazite::modulate (context, macro, gain, 0.5f);
    phazite::modulate (context, lfo, gain, 0.25f);
    phazite::showAutomationLane (context, gain);

    const auto copies = phazite::duplicateTracks ({ song.a }, nullptr);
    REQUIRE (copies.size() == 1);
    auto* copy = copies.getFirst();
    const auto copiedContainer = phazite::getEffects (context, idOf (*copy))[0];
    REQUIRE (copiedContainer.isContainer);
    CHECK (copiedContainer.id != container);
    CHECK (song.edit->getRackList().size() == 2);

    // Its own devices, which its own macro and LFO modulate.
    const auto copiedInside = phazite::getContainerDevices (context, copiedContainer.id);
    REQUIRE (copiedInside.size() == 2);
    CHECK (copiedInside[0].id != inside[0].id);
    const ParameterRef copiedGain { copiedInside[0].id, "band4Gain" };
    const auto copiedMacro = phazite::getContainerMacros (context, copiedContainer.id)[0].id;
    const auto copiedLfo = phazite::getModulators (context, idOf (*copy))[0].id;
    CHECK (copiedMacro != macro);
    CHECK (copiedLfo != lfo);

    std::vector<phazite::ModulatorID> modulators;

    for (const auto& modulation : phazite::getModulations (context, copiedGain))
        modulators.push_back (modulation.modulator);

    CHECK (modulators == std::vector<phazite::ModulatorID> { copiedMacro, copiedLfo });
    CHECK (phazite::getModulations (context, gain).size() == 2); // the original's are as they were

    // Its lane shows its own device's parameter.
    const auto lanes = phazite::getAutomationLanes (context, idOf (*copy));
    CHECK (std::any_of (lanes.begin(), lanes.end(), [&] (const auto& lane) { return lane.parameter == copiedGain; }));

    // A change to one container's device leaves the other's alone.
    phazite::getParameter (context, gain).setValue (6.0f);
    CHECK (song.find (copiedGain)->getCurrentBaseValue() == doctest::Approx (0.0f));

    // Deleted, the copy and its container go; undone, they come back, playing the copy's rack type still.
    phazite::deleteTracks ({ copy }, nullptr);
    CHECK (te::getAudioTracks (*song.edit).size() == 2);
    phazite::undo (*song.edit);
    CHECK (phazite::getContainerDevices (context, copiedContainer.id).size() == 2);
    CHECK (phazite::getModulations (context, copiedGain).size() == 2);
}

TEST_CASE ("Group into Container and Ungroup Container act on the selected devices, which are selected on their own")
{
    Song song;
    auto& context = song.context;
    auto& selection = context.getSongSelection();
    phazite::CommandRegistry registry;
    phazite::addContainerCommands (registry, getSelection (context));
    const auto* group = registry.find ("device.group");
    const auto* ungroup = registry.find ("device.ungroup");
    REQUIRE (group != nullptr);
    REQUIRE (ungroup != nullptr);

    const auto effects = song.effectsOf (*song.a);
    CHECK_FALSE (group->isEnabled());

    // Selecting devices deselects tracks, and selecting a track deselects devices.
    selection.selectOnly (idOf (*song.a));
    selection.add (effects[0]);
    CHECK (selection.getTracks().empty());
    selection.add (effects[1]);
    CHECK (selection.getDevices() == std::vector<DeviceID> { effects[0], effects[1] });
    CHECK (group->isEnabled());
    CHECK_FALSE (ungroup->isEnabled());

    // The container is selected after grouping, and its devices after ungrouping.
    registry.invoke ("device.group");
    CHECK (song.describeUndo() == juce::String ("Undo Group into Container"));
    const auto container = song.effectsOf (*song.a)[0];
    CHECK (selection.getDevices() == std::vector<DeviceID> { container });
    CHECK (ungroup->isEnabled());
    CHECK_FALSE (group->isEnabled());
    registry.invoke ("device.ungroup");
    CHECK (song.describeUndo() == juce::String ("Undo Ungroup Container"));
    CHECK (selection.getDevices() == std::vector<DeviceID> { effects[0], effects[1] });

    selection.add (idOf (*song.b));
    CHECK (selection.getDevices().empty());
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { idOf (*song.b) });
}

TEST_CASE ("A saved project opens with its containers, their devices and their macros")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Containers", error);
    REQUIRE (project != nullptr);

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &project->getEdit());
    const auto track = idOf (*te::getAudioTracks (project->getEdit())[0]);
    phazite::DeviceType eq;

    for (const auto& each : phazite::getDeviceTypes (context))
        if (each.description.fileOrIdentifier == juce::String ("phaziteEq"))
            eq = each;

    phazite::addEffect (context, track, eq);
    phazite::addEffect (context, track, eq);
    const auto container =
        phazite::groupIntoContainer (context, { phazite::getEffects (context, track)[0].id, phazite::getEffects (context, track)[1].id });
    const auto macro = phazite::getContainerMacros (context, container)[0].id;
    const ParameterRef gain { phazite::getContainerDevices (context, container)[1].id, "band4Gain" };
    phazite::modulate (context, macro, gain, 0.5f);
    phazite::renameMacro (context, macro, "Air");

    // A container made and undone leaves its rack type for the session, which the project drops when it opens.
    phazite::addEffect (context, track, eq);
    phazite::groupIntoContainer (context, { phazite::getEffects (context, track)[1].id });
    phazite::undo (project->getEdit());
    phazite::undo (project->getEdit());
    CHECK (project->getEdit().getRackList().size() == 2);
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    setEdit (context, nullptr);
    project = nullptr;

    project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (project != nullptr);
    setEdit (context, &project->getEdit());
    CHECK (project->getEdit().getRackList().size() == 1);
    const auto effects = phazite::getEffects (context, track);
    REQUIRE (effects.size() == 1);
    CHECK (effects[0].isContainer);
    CHECK (phazite::getContainerDevices (context, effects[0].id).size() == 2);
    CHECK (phazite::getContainerMacros (context, effects[0].id)[0].name == juce::String ("Air"));
    const auto modulations = phazite::getModulations (context, gain);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].modulator == macro);
    CHECK (modulations[0].amount == doctest::Approx (0.5f));
    setEdit (context, nullptr);
}
