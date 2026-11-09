// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers by IDs (src/model/mixer/ContainerModel.h; roadmap step 2.4): effects grouped into a container and back as
// named undo steps, an empty one added, the container's row of devices and its eight macros, which modulate its devices
// only, its devices reached by the mixer's functions, a duplicated track's containers with devices and macros of their
// own, and the commands that group and ungroup the selected devices.
#include "EnginePlayback.h"
#include "TestHelpers.h"
#include "model/arrangement/SongSelection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
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
using phazite::TrackID;

// An open project with two tracks: A, with a synth, then an EQ, a compressor and a reverb; and B, with an EQ.
struct TwoTracks
{
    TwoTracks()
    {
        const auto tracks = phazite::ensureTracks (*song, 2);
        a = tracks[0];
        b = tracks[1];
        setSong (context, song.get());
        phazite::chooseInstrument (context, a, find ("phaziteSynth"));

        for (const auto* effect : { "phaziteEq", "phaziteCompressor", "phaziteReverb" })
            phazite::addEffect (context, a, find (effect));

        phazite::addEffect (context, b, find ("phaziteEq"));
        song->getUndoManager().clearUndoHistory();
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

    std::vector<DeviceID> effectsOf (TrackID track) const
    {
        std::vector<DeviceID> ids;

        for (const auto& effect : phazite::getEffects (context, track))
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

    std::vector<juce::String> effectNamesOf (TrackID track) const { return namesOf (phazite::getEffects (context, track)); }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    TrackID a, b;
};

// A's EQ and reverb grouped into a container.
DeviceID groupEqAndReverb (TwoTracks& tracks)
{
    const auto effects = tracks.effectsOf (tracks.a);
    return phazite::groupIntoContainer (tracks.context, { effects[0], effects[2] });
}
} // namespace

TEST_CASE ("An effect is replaced by another in its place, on its channel or in its container, as one undo step")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto effects = tracks.effectsOf (tracks.a);
    const auto names = tracks.effectNamesOf (tracks.a);
    REQUIRE (names.size() == 3);

    // The compressor's place, between the EQ and the reverb, the delay's.
    phazite::replaceEffect (context, effects[1], tracks.find ("phaziteDelay"));
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { names[0], "Delay", names[2] });
    CHECK (tracks.effectsOf (tracks.a)[0] == effects[0]);
    CHECK (tracks.describeUndo() == juce::String ("Undo Replace Compressor with Delay"));
    phazite::undo (*tracks.song);
    CHECK (tracks.effectNamesOf (tracks.a) == names);
    CHECK (tracks.effectsOf (tracks.a) == effects);

    // In a container, its place among the container's devices.
    const auto container = groupEqAndReverb (tracks);
    const auto inside = phazite::getContainerDevices (context, container);
    REQUIRE (inside.size() == 2);
    phazite::replaceEffect (context, inside[0].id, tracks.find ("phazitePhaser"));
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "Phaser", inside[1].name });

    // Never an instrument, nor by one.
    const auto instrument = phazite::getInstrument (context, tracks.a)->id;
    phazite::replaceEffect (context, instrument, tracks.find ("phaziteDelay"));
    phazite::replaceEffect (context, tracks.effectsOf (tracks.b)[0], tracks.find ("phaziteSynth"));
    CHECK (phazite::getInstrument (context, tracks.a)->id == instrument);
    CHECK (tracks.effectNamesOf (tracks.b) == std::vector<juce::String> { names[0] });
}

TEST_CASE ("Effects of one channel group into a container in the first one's place, as one undo step, and ungroup again")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto effects = tracks.effectsOf (tracks.a);
    const auto instrument = phazite::getInstrument (context, tracks.a)->id;
    const auto eq = effects[0], compressor = effects[1], reverb = effects[2];

    // Effects of one channel (or its instrument with them, into an instrument container), not of two.
    CHECK (phazite::canGroupIntoContainer (context, { eq, reverb }));
    CHECK (phazite::canGroupIntoContainer (context, { instrument, eq }));
    CHECK_FALSE (phazite::canGroupIntoContainer (context, { eq, tracks.effectsOf (tracks.b)[0] }));
    CHECK_FALSE (phazite::canGroupIntoContainer (context, {}));

    // In the EQ's place, holding the EQ and the reverb in their order, which keep their IDs.
    const auto container = phazite::groupIntoContainer (context, { reverb, eq });
    REQUIRE (container.isValid());
    CHECK (tracks.describeUndo() == juce::String ("Undo Group into Container"));
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Container", "Compressor" });
    CHECK (tracks.effectsOf (tracks.a)[1] == compressor);
    CHECK (phazite::getEffects (context, tracks.a)[0].isContainer);
    CHECK (phazite::getInstrument (context, tracks.a)->id == instrument); // a container isn't an instrument

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
    CHECK (macros[0].track == tracks.a);
    CHECK (phazite::getModulatorValue (context, macros[0].id) == doctest::Approx (0.0f));

    // A second one is named apart; a container can't go in a container.
    const auto other = phazite::groupIntoContainer (context, { compressor });
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Container", "Container 2" });
    CHECK_FALSE (phazite::canGroupIntoContainer (context, { container, other }));

    // Undo puts the effects back where they were; redo groups them again.
    phazite::undo (*tracks.song);
    phazite::undo (*tracks.song);
    CHECK (tracks.effectsOf (tracks.a) == std::vector<DeviceID> { eq, compressor, reverb });
    CHECK_FALSE (phazite::getContainerOf (context, eq).isValid());
    phazite::redo (*tracks.song);
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Container", "Compressor" });
    CHECK (phazite::getContainerDevices (context, container).size() == 2);

    // Ungrouping puts them in its place, in their order, and the container goes.
    phazite::ungroupContainer (context, container);
    CHECK (tracks.describeUndo() == juce::String ("Undo Ungroup Container"));
    CHECK (tracks.effectsOf (tracks.a) == std::vector<DeviceID> { eq, reverb, compressor });
    CHECK (phazite::getContainerDevices (context, container).empty());
    phazite::undo (*tracks.song);
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Container", "Compressor" });
}

TEST_CASE ("An empty container goes after a track's effects, or the master's, as one undo step, for effects to be added to it")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto names = tracks.effectNamesOf (tracks.a);

    // After the track's effects, holding nothing, with eight macros at 0, as a grouped one has.
    const auto container = phazite::addContainer (context, tracks.a);
    REQUIRE (container.isValid());
    CHECK (tracks.describeUndo() == juce::String ("Undo Add Container"));
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "EQ", "Compressor", "Reverb", "Container" });
    CHECK (phazite::getEffects (context, tracks.a).back().id == container);
    CHECK (phazite::getEffects (context, tracks.a).back().isContainer);
    CHECK (phazite::getContainerDevices (context, container).empty());
    const auto macros = phazite::getContainerMacros (context, container);
    REQUIRE (macros.size() == 8);
    CHECK (macros[0].name == juce::String ("Macro 1"));
    CHECK (macros[7].name == juce::String ("Macro 8"));
    CHECK (macros[0].container == container);
    CHECK (macros[0].track == tracks.a);
    CHECK (phazite::getModulatorValue (context, macros[0].id) == doctest::Approx (0.0f));

    // An effect added to it plays in it, and its macros reach it.
    phazite::addEffectToContainer (context, container, tracks.find ("phaziteDelay"));
    const auto inside = phazite::getContainerDevices (context, container);
    REQUIRE (inside.size() == 1);
    CHECK (inside[0].name == juce::String ("Delay"));
    CHECK (phazite::getContainerOf (context, inside[0].id) == container);
    const auto delayParameters = phazite::getAutomationTargets (context, inside[0].id);
    REQUIRE_FALSE (delayParameters.empty());
    CHECK (phazite::canModulate (context, macros[0].id, delayParameters[0].parameter));

    // Another is named apart; the master has one too, and a track that isn't there none.
    CHECK (phazite::addContainer (context, tracks.b).isValid());
    CHECK (tracks.effectNamesOf (tracks.b) == std::vector<juce::String> { "EQ", "Container 2" });
    const auto master = phazite::addContainer (context, {});
    REQUIRE (master.isValid());
    CHECK (phazite::getEffects (context, {}).back().id == master);
    CHECK (phazite::getContainerMacros (context, master)[0].track == phazite::getMasterTrack (context));
    CHECK_FALSE (phazite::addContainer (context, TrackID::fromRaw (999999)).isValid());
    CHECK (tracks.describeUndo() == juce::String ("Undo Add Container")); // nothing added: no step

    // Undo takes each away; redo brings A's back, empty.
    for (int step = 0; step < 4; ++step) // the master's, B's, the delay, A's
        phazite::undo (*tracks.song);

    CHECK (tracks.effectNamesOf (tracks.a) == names);
    CHECK (tracks.effectNamesOf (tracks.b) == std::vector<juce::String> { "EQ" });
    phazite::redo (*tracks.song);
    CHECK (tracks.effectNamesOf (tracks.a).back() == juce::String ("Container"));
    CHECK (phazite::getContainerDevices (context, container).empty());
}

TEST_CASE ("A container's devices move, come and go as the mixer's devices do")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto container = groupEqAndReverb (tracks);

    // An effect added at its end.
    phazite::addEffectToContainer (context, container, tracks.find ("phaziteCompressor"));
    CHECK (tracks.describeUndo() == juce::String ("Undo Add Compressor"));
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, container))
           == std::vector<juce::String> { "EQ", "Reverb", "Compressor" });

    // Moved earlier, among its container's devices only.
    const auto added = phazite::getContainerDevices (context, container)[2].id;
    phazite::moveEffect (context, added, -2);
    CHECK (tracks.describeUndo() == juce::String ("Undo Move Device"));
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, container))
           == std::vector<juce::String> { "Compressor", "EQ", "Reverb" });
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Container", "Compressor" });

    // Turned off, and removed from it.
    phazite::setDeviceOn (context, added, false);
    CHECK_FALSE (phazite::getContainerDevices (context, container)[0].isOn);
    phazite::removeDevice (context, added);
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "EQ", "Reverb" });

    // A container has a mix of its own.
    CHECK (phazite::getEffects (context, tracks.a)[0].hasMix);
    CHECK (phazite::getMix (context, container).getValue() == doctest::Approx (1.0f));

    // Renamed, and removed, which undo brings back.
    phazite::renameContainer (context, container, "  Space  ");
    CHECK (tracks.describeUndo() == juce::String ("Undo Rename Container"));
    CHECK (phazite::getEffects (context, tracks.a)[0].name == juce::String ("Space"));
    phazite::removeDevice (context, container);
    CHECK (tracks.describeUndo() == juce::String ("Undo Remove Space"));
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Compressor" });
    phazite::undo (*tracks.song);
    CHECK (tracks.effectNamesOf (tracks.a) == std::vector<juce::String> { "Space", "Compressor" });
    CHECK (phazite::getContainerDevices (context, container).size() == 2);
}

TEST_CASE ("A container is the song's, with its name, mix, devices and macros")
{
    namespace schema = phazite::schema;
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto effects = tracks.effectsOf (tracks.a);
    const auto eq = effects[0], reverb = effects[2];
    const auto container = groupEqAndReverb (tracks);
    const auto element = phazite::findDeviceState (song, container);

    // The song's container, in the EQ's place, with its name and its one chain, which holds its devices' elements in their
    // order.
    REQUIRE (element.hasType (schema::CONTAINER));
    REQUIRE (phazite::getChainStates (element).size() == 1);
    CHECK (phazite::getChainStates (element)[0][schema::chainName].toString() == juce::String ("Chain 1"));
    CHECK (element[schema::containerName].toString() == juce::String ("Container"));
    CHECK (phazite::getDeviceStates (phazite::findTrackState (song, tracks.a)).size() == 3); // the synth, the container, the compressor
    const auto devicesIn = [&element]
    {
        std::vector<DeviceID> ids;

        for (const auto& state : phazite::getDeviceStates (element))
            ids.push_back (phazite::getDeviceID (state));

        return ids;
    };

    CHECK (devicesIn() == std::vector<DeviceID> { eq, reverb });

    // Its name, its mix, and its devices' switches and values are the song's.
    phazite::renameContainer (context, container, "Space");
    CHECK (element[schema::containerName].toString() == juce::String ("Space"));
    phazite::getMix (context, container).setValue (0.5f);
    CHECK ((float) element[schema::deviceMix] == doctest::Approx (0.5f));
    phazite::setDeviceOn (context, reverb, false);
    CHECK ((bool) phazite::findDeviceState (song, reverb)[schema::deviceOff]);
    CHECK_FALSE (phazite::getContainerDevices (context, container)[1].isOn);
    phazite::getParameter (context, { eq, "band4Gain" }).setValue (6.0f);
    CHECK ((float) phazite::findDeviceState (song, eq).getChildWithName (schema::VALUES)["band4Gain"] == doctest::Approx (6.0f));
    CHECK (phazite::getValueSetByHand (song, ParameterRef { eq, "band4Gain" }) == doctest::Approx (6.0f));

    // Moved in the song; an undo puts it back.
    phazite::moveEffect (context, reverb, -1);
    CHECK (devicesIn() == std::vector<DeviceID> { reverb, eq });
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "Reverb", "EQ" });
    phazite::undo (song);
    CHECK (devicesIn() == std::vector<DeviceID> { eq, reverb });
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, container)) == std::vector<juce::String> { "EQ", "Reverb" });

    // A container the song gains shows on its channel, with its chain's devices and its macros.
    juce::ValueTree made (schema::CONTAINER), chain (schema::CHAIN), delay (schema::DEVICE), macro (schema::MACRO);
    const auto madeID = DeviceID::fromRaw (song.makeID());
    made.setProperty (schema::deviceID, (juce::int64) madeID.getRaw(), nullptr);
    made.setProperty (schema::containerName, "Echoes", nullptr);
    chain.setProperty (schema::chainID, (juce::int64) song.makeID(), nullptr);
    delay.setProperty (schema::deviceID, (juce::int64) song.makeID(), nullptr);
    delay.setProperty (schema::deviceType, "phaziteDelay", nullptr);
    chain.appendChild (delay, nullptr);
    made.appendChild (chain, nullptr);
    macro.setProperty (schema::modulatorID, (juce::int64) song.makeID(), nullptr);
    macro.setProperty (schema::macroName, "Space", nullptr);
    made.appendChild (macro, nullptr);
    phazite::findTrackState (song, tracks.b).appendChild (made, nullptr);
    CHECK (tracks.effectNamesOf (tracks.b) == std::vector<juce::String> { "EQ", "Echoes" });
    CHECK (tracks.namesOf (phazite::getContainerDevices (context, madeID)) == std::vector<juce::String> { "Delay" });
    const auto macros = phazite::getContainerMacros (context, madeID);
    REQUIRE (macros.size() == 1);
    CHECK (macros[0].name == juce::String ("Space"));
}

TEST_CASE ("A container's macros modulate its devices; its track's modulators reach them too")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto container = groupEqAndReverb (tracks);
    const auto inside = phazite::getContainerDevices (context, container);
    const ParameterRef gain { inside[0].id, "band4Gain" };                  // the EQ's, inside
    const ParameterRef makeup { tracks.effectsOf (tracks.a)[1], "makeup" }; // the compressor's, outside
    const auto macro = phazite::getContainerMacros (context, container)[0].id;

    CHECK (phazite::canModulate (context, macro, gain));
    CHECK_FALSE (phazite::canModulate (context, macro, makeup));

    // Turning the macro moves the EQ's gain by the amount, from where it is, as Phazite's engine plays it: at 0, not at all.
    phazite::test::EnginePlayback playback;
    const auto played = [&]
    {
        playback.play (0.05);
        return phazite::getCurrentValue (song, gain, true);
    };

    phazite::modulate (context, macro, gain, 0.5f);
    CHECK (tracks.describeUndo() == juce::String ("Undo Modulate Band 4 Gain"));
    const auto description = phazite::describeParameter (song, gain);
    REQUIRE (description.has_value());
    const auto range = description->range;
    CHECK (played() == doctest::Approx (0.0f).epsilon (0.001));
    auto knob = phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro)[0].id);
    REQUIRE (knob.isValid());
    knob.setValue (1.0f);
    CHECK (tracks.describeUndo() == juce::String ("Undo Change Macro 1"));
    CHECK (played() == doctest::Approx (range.convertFrom0to1 (range.convertTo0to1 (0.0f) + 0.5f)).epsilon (0.01));

    // The track's LFO reaches the devices in its containers, and a control in the container offers the container's
    // macros first.
    const auto lfo = phazite::addModulator (context, tracks.a, ModulatorType::lfo);
    CHECK (phazite::canModulate (context, lfo, gain));
    const auto handle = phazite::getParameter (context, gain);
    const auto offered = phazite::getModulatorsFor (handle);
    REQUIRE (offered.size() == 9);
    CHECK (offered[0].id == macro);
    CHECK (offered[8].id == lfo);
    CHECK (phazite::getModulatorsFor (phazite::getParameter (context, makeup)).size() == 1);

    // A macro is renamed, but stays: the container keeps all eight.
    phazite::renameMacro (context, macro, "Air");
    CHECK (tracks.describeUndo() == juce::String ("Undo Rename Macro"));
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
    TwoTracks tracks;
    auto& context = tracks.context;
    const auto container = groupEqAndReverb (tracks);
    const auto inside = phazite::getContainerDevices (context, container);

    const auto targets = phazite::getAutomationTargets (context, inside[0].id);
    REQUIRE_FALSE (targets.empty());
    CHECK (phazite::getParameter (context, targets[0].parameter).isValid());

    // Its lane shows under the track, and its points play.
    phazite::showAutomationLane (context, targets[0].parameter);
    const auto lanes = phazite::getAutomationLanes (context, tracks.a);
    CHECK (std::any_of (lanes.begin(), lanes.end(), [&] (const auto& lane) { return lane.parameter == targets[0].parameter; }));
}

TEST_CASE ("A duplicated track's containers have devices and macros of their own, which its own modulators modulate")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& song = *tracks.song;
    const auto container = groupEqAndReverb (tracks);
    const auto inside = phazite::getContainerDevices (context, container);
    const ParameterRef gain { inside[0].id, "band4Gain" };
    const auto macro = phazite::getContainerMacros (context, container)[0].id;
    const auto lfo = phazite::addModulator (context, tracks.a, ModulatorType::lfo);
    phazite::modulate (context, macro, gain, 0.5f);
    phazite::modulate (context, lfo, gain, 0.25f);
    phazite::showAutomationLane (context, gain);

    const auto copies = phazite::duplicateTracks (song, { tracks.a }, nullptr);
    REQUIRE (copies.size() == 1);
    const auto copy = copies.front();
    const auto copiedContainer = phazite::getEffects (context, copy)[0];
    REQUIRE (copiedContainer.isContainer);
    CHECK (copiedContainer.id != container);

    // Its own devices, which its own macro and LFO modulate.
    const auto copiedInside = phazite::getContainerDevices (context, copiedContainer.id);
    REQUIRE (copiedInside.size() == 2);
    CHECK (copiedInside[0].id != inside[0].id);
    const ParameterRef copiedGain { copiedInside[0].id, "band4Gain" };
    const auto copiedMacro = phazite::getContainerMacros (context, copiedContainer.id)[0].id;
    const auto copiedLfo = phazite::getModulators (context, copy)[0].id;
    CHECK (copiedMacro != macro);
    CHECK (copiedLfo != lfo);

    std::vector<phazite::ModulatorID> modulators;

    for (const auto& modulation : phazite::getModulations (context, copiedGain))
        modulators.push_back (modulation.modulator);

    // In any order: what modulates a parameter adds up.
    CHECK (modulators.size() == 2);
    CHECK (std::find (modulators.begin(), modulators.end(), copiedMacro) != modulators.end());
    CHECK (std::find (modulators.begin(), modulators.end(), copiedLfo) != modulators.end());
    CHECK (phazite::getModulations (context, gain).size() == 2); // the original's are as they were

    // Its lane shows its own device's parameter.
    const auto lanes = phazite::getAutomationLanes (context, copy);
    CHECK (std::any_of (lanes.begin(), lanes.end(), [&] (const auto& lane) { return lane.parameter == copiedGain; }));

    // A change to one container's device leaves the other's alone.
    phazite::getParameter (context, gain).setValue (6.0f);
    CHECK (phazite::getValueSetByHand (song, copiedGain) == doctest::Approx (0.0f));

    // Deleted, the copy and its container go; undone, they come back, with the copy's devices and modulation still.
    phazite::deleteTracks (song, { copy }, nullptr);
    CHECK (phazite::getTrackIDs (song).size() == 2);
    phazite::undo (song);
    CHECK (phazite::getContainerDevices (context, copiedContainer.id).size() == 2);
    CHECK (phazite::getModulations (context, copiedGain).size() == 2);
}

TEST_CASE ("The master's effects group into a container too, whose macros' track is the master's")
{
    TwoTracks tracks;
    auto& context = tracks.context;

    for (const auto* effect : { "phaziteEq", "phaziteLimiter" })
        phazite::addEffect (context, {}, tracks.find (effect));

    const auto master = phazite::getEffects (context, {});
    REQUIRE (master.size() >= 2);
    const auto eq = master[master.size() - 2].id, limiter = master.back().id;
    const auto container = phazite::groupIntoContainer (context, { eq, limiter });
    REQUIRE (container.isValid());

    const auto after = phazite::getEffects (context, {});
    CHECK (after.back().id == container);
    CHECK (after.back().isContainer);
    CHECK (phazite::getContainerDevices (context, container).size() == 2);

    const auto macro = phazite::getContainerMacros (context, container)[0];
    CHECK (macro.track == phazite::getMasterTrack (context));
    CHECK (phazite::canModulate (context, macro.id, { eq, "band4Gain" }));

    phazite::ungroupContainer (context, container);
    const auto back = phazite::getEffects (context, {});
    CHECK (back[back.size() - 2].id == eq);
    CHECK (back.back().id == limiter);
}

TEST_CASE ("Group into Container and Ungroup Container act on the selected devices, which are selected on their own")
{
    TwoTracks tracks;
    auto& context = tracks.context;
    auto& selection = context.getSongSelection();
    phazite::CommandRegistry registry;
    phazite::addContainerCommands (registry, getSelection (context));
    const auto* group = registry.find ("device.group");
    const auto* ungroup = registry.find ("device.ungroup");
    REQUIRE (group != nullptr);
    REQUIRE (ungroup != nullptr);

    const auto effects = tracks.effectsOf (tracks.a);
    CHECK_FALSE (group->isEnabled());

    // Selecting devices deselects tracks, and selecting a track deselects devices.
    selection.selectOnly (tracks.a);
    selection.add (effects[0]);
    CHECK (selection.getTracks().empty());
    selection.add (effects[1]);
    CHECK (selection.getDevices() == std::vector<DeviceID> { effects[0], effects[1] });
    CHECK (group->isEnabled());
    CHECK_FALSE (ungroup->isEnabled());

    // The container is selected after grouping, and its devices after ungrouping.
    registry.invoke ("device.group");
    CHECK (tracks.describeUndo() == juce::String ("Undo Group into Container"));
    const auto container = tracks.effectsOf (tracks.a)[0];
    CHECK (selection.getDevices() == std::vector<DeviceID> { container });
    CHECK (ungroup->isEnabled());
    CHECK_FALSE (group->isEnabled());
    registry.invoke ("device.ungroup");
    CHECK (tracks.describeUndo() == juce::String ("Undo Ungroup Container"));
    CHECK (selection.getDevices() == std::vector<DeviceID> { effects[0], effects[1] });

    selection.add (tracks.b);
    CHECK (selection.getDevices().empty());
    CHECK (selection.getTracks() == std::vector<TrackID> { tracks.b });
}

TEST_CASE ("A saved project opens with its containers, their devices and their macros")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Containers", error);
    REQUIRE (project != nullptr);

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &project->getSong());
    const auto trackIDs = phazite::getTrackIDs (project->getSong());
    REQUIRE_FALSE (trackIDs.empty());
    const auto track = trackIDs.front();
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

    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    setSong (context, nullptr);
    project = nullptr;

    project = phazite::Project::open (file, false, error);
    REQUIRE (project != nullptr);
    setSong (context, &project->getSong());
    const auto effects = phazite::getEffects (context, track);
    REQUIRE (effects.size() == 1);
    CHECK (effects[0].isContainer);
    CHECK (phazite::getContainerDevices (context, effects[0].id).size() == 2);
    CHECK (phazite::getContainerMacros (context, effects[0].id)[0].name == juce::String ("Air"));
    const auto modulations = phazite::getModulations (context, gain);
    REQUIRE (modulations.size() == 1);
    CHECK (modulations[0].modulator == macro);
    CHECK (modulations[0].amount == doctest::Approx (0.5f));
    setSong (context, nullptr);
}
