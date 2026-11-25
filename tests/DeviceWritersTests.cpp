// SPDX-License-Identifier: AGPL-3.0-or-later
// The devices' writers on values (model/mixer/internal/Mixer.h): every writer of a
// device, a container or a chain works its next value out and sets it through the devices' setters (setDevice(),
// setNewDevices(), eraseDevice(), setDeviceListOrder() and the narrow ones beside them), which while the song's tree holds the
// devices write it by what differs. Their own cases (a set equal to what the song holds writes nothing; an empty SOUNDS and an
// empty VALUES go; a device moved between lists is the same element, its window's place, its plug-in's value and instance
// kept through each move and its Undo; a write outside the history makes no undo action; a value written off its steps is
// written as the song holds it, a lone automation point too; a value whose ID the song holds as another kind of item is
// refused); and the writers moved onto them, each one exact undo step with its name, the song checked (checkSong()), and
// each leaving the song written as Phazite wrote it before (a copy written as its original, but for the IDs; a round trip
// back to the text it started from): a container made around three devices and taken apart, a chain duplicated, moved and
// removed, an instrument replaced, a container's preset added (its macros set as values),
// Swap's trial kept and put back, a track with a container duplicated (its devices before its lanes, as the
// original's, its MIDI links not copied), a bus folder duplicated (its children in the original's order), a device turned
// off and on, a mix set, a chain's fader and ranges, a
// split and its crossovers, effects moved in and between chains, an instrument replaced in a chain, an instrument container
// added, a duck, an effect replaced, a return with an effect, a preset loaded and tried, the sidechains forgotten, devices
// removed, and a bus made plain (its plug-ins' states stored first: a guard for an Undo after a bus with a plug-in is made
// plain).
//   phazite_tests.exe --source-file=*DeviceWritersTests*
#include "SongCheck.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UndoCheck.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationValues.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/SwapModel.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
namespace dev = phazite::devices;
namespace schema = phazite::schema;
using phazite::DeviceID;
using phazite::DeviceValue;
using phazite::TrackID;

// A step of the test's own, around what isn't a change of its own (a setter, a writer that joins its caller's step).
std::function<void()> inStep (phazite::Song& song, const juce::String& name, std::function<void()> change)
{
    return [&song, name, change = std::move (change)]
    {
        const phazite::UndoStep step (song, name);
        change();
    };
}

// An element with every ID it holds or names written as the order in which it first appears ("#0", "#1"...), as text: what
// two elements hold alike but for the IDs a copy gives (Schema.h: giveNewIDs()) and in the same order, compared. Written as a
// project file writes it (SongText.h: writeProjectText()), each element's properties in the order of their names, so that the
// order they were set in is no difference (an element written from its value, a copy's, sets them in its writer's order); its
// children in their order, which is.
juce::String withIDsInOrder (const std::vector<juce::ValueTree>& elements)
{
    std::map<std::uint64_t, int> order;
    juce::String text;

    const std::function<void (juce::ValueTree)> rename = [&order, &rename] (juce::ValueTree tree)
    {
        for (int i = 0; i < tree.getNumProperties(); ++i)
        {
            const auto property = tree.getPropertyName (i);

            if (schema::holdsID (tree, property) || schema::namesID (tree, property))
            {
                const auto [found, added] = order.try_emplace (phazite::readID (tree[property]), (int) order.size());
                juce::ignoreUnused (added);
                tree.setProperty (property, "#" + juce::String (found->second), nullptr);
            }
        }

        for (const auto& child : tree)
            rename (child);
    };

    for (const auto& element : elements)
    {
        auto copy = element.createCopy();
        rename (copy);
        juce::MemoryOutputStream written;
        phazite::writeProjectText (written, copy);
        text << written.toString();
    }

    return text;
}

// A channel's devices and containers in the song written whole (copyWholeSong()), in their order.
std::vector<juce::ValueTree> getWrittenDevices (const phazite::Song& song, TrackID channel)
{
    std::vector<juce::ValueTree> devices;
    const auto whole = phazite::test::copyWholeSong (song);

    const std::function<juce::ValueTree (const juce::ValueTree&)> findTrack = [&findTrack, channel] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::TRACK) && phazite::readID (child[schema::trackID]) == channel.getRaw())
                return child;

            if (const auto found = findTrack (child); found.isValid())
                return found;
        }

        return juce::ValueTree();
    };

    for (const auto& child : findTrack (whole))
        if (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER))
            devices.push_back (child.createCopy());

    return devices;
}

juce::String writeWhole (const phazite::Song& song)
{
    return phazite::test::writeSong (song, phazite::test::SongPart::whole);
}

// A parameter of a description, by its kind of unit, the first; REQUIRE'd there.
const dev::ParameterInfo& findParameterOf (const dev::DeviceInfo& info, dev::Unit unit)
{
    for (const auto& parameter : info.parameters)
        if (parameter.unit == unit && (unit != dev::Unit::choice || parameter.choices.size() >= 3))
            return parameter;

    FAIL ("no parameter of that unit");
    return info.parameters.front();
}

// A change checked as one undo step of its name (its Undo and Redo exact: UndoCheck), and the song checked after it.
void checkOneStep (phazite::test::UndoCheck& undo, phazite::Song& song, const juce::String& name, const std::function<void()>& change)
{
    const auto steps = undo.getNumSteps();
    undo.check (name, change);
    CHECK (undo.getNumSteps() == steps + 1);
    CHECK (song.getUndoManager().getUndoDescription() == name);
    phazite::test::checkSong (song);
}

// A device as the song holds it, REQUIRE'd there.
DeviceValue findHeld (const phazite::Song& song, DeviceID id)
{
    const auto* device = phazite::findDevice (song, id);
    REQUIRE (device != nullptr);
    return *device;
}

// A track's element in the song written whole (copyWholeSong()), and the types of its children in their order.
juce::ValueTree getWrittenTrack (const phazite::Song& song, TrackID track)
{
    const std::function<juce::ValueTree (const juce::ValueTree&)> find = [&find, track] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::TRACK) && phazite::readID (child[schema::trackID]) == track.getRaw())
                return child;

            if (const auto found = find (child); found.isValid())
                return found;
        }

        return juce::ValueTree();
    };

    return find (phazite::test::copyWholeSong (song));
}

juce::StringArray getChildTypes (const juce::ValueTree& element)
{
    juce::StringArray types;

    for (const auto& child : element)
        types.add (child.getType().toString());

    return types;
}

// A parameter's index in a description of a device's, by its ID.
std::uint32_t indexOf (const dev::DeviceInfo& info, std::string_view parameter)
{
    const auto index = dev::findParameter (info, parameter);
    REQUIRE (index >= 0);
    return (std::uint32_t) index;
}
} // namespace

TEST_CASE ("Device writers: the setters write nothing for a value the song holds, take an empty SOUNDS and VALUES away, move "
           "a device as the same element and write outside the history without an undo action")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, dev::Synth::typeID);
    const auto eq = s.addEffect (track, dev::Equalizer::typeID);
    auto& undo = song.getUndoManager();
    undo.clearUndoHistory();

    // (a) A value equal to what the song holds: nothing written, no action, through each setter.
    for (const auto id : { synth, eq })
    {
        const auto* held = phazite::findDevice (song, id);
        REQUIRE (held != nullptr);
        const auto before = writeWhole (song);
        {
            const phazite::UndoStep step (song, "Nothing");
            CHECK_FALSE (phazite::setDevice (song, *held));
            CHECK_FALSE (phazite::setDeviceFields (song, id, [] (DeviceValue&) {}));
            CHECK_FALSE (phazite::setDeviceListOrder (song, held->place, { synth, eq }));
            CHECK_FALSE (phazite::moveInDeviceList (song, id, id == synth ? 0 : 1));
        }
        CHECK_FALSE (undo.canUndo());
        CHECK (writeWhole (song) == before);
    }

    // (b) A sound set and taken away again leaves no SOUNDS, a value set and set back no VALUES: the file as before.
    const auto beforeSounds = writeWhole (song);
    inStep (song, "Table", [&] { phazite::setDeviceSound (song, synth, schema::getOscillatorWavetable (0), "Pulse Width"); })();
    REQUIRE (phazite::test::copyDevice (song, synth).getChildWithName (schema::SOUNDS).isValid());
    inStep (song, "No Table", [&] { phazite::setDeviceSound (song, synth, schema::getOscillatorWavetable (0), {}); })();
    CHECK_FALSE (phazite::test::copyDevice (song, synth).getChildWithName (schema::SOUNDS).isValid());
    CHECK (writeWhole (song) == beforeSounds);
    phazite::test::checkSong (song);

    const auto gain = dev::Equalizer::getParameterIndex (2, dev::Equalizer::gain);
    inStep (song, "Gain", [&] { CHECK (phazite::setDeviceValue (song, eq, gain, 3.0f)); })();
    REQUIRE (phazite::test::copyDevice (song, eq).getChildWithName (schema::VALUES).isValid());
    inStep (song, "Gain Back",
            [&] { CHECK (phazite::setDeviceValue (song, eq, gain, dev::Equalizer::info.parameters[(size_t) gain].defaultValue)); })();
    CHECK_FALSE (phazite::test::copyDevice (song, eq).getChildWithName (schema::VALUES).isValid());
    CHECK (writeWhole (song) == beforeSounds);

    // The wide setter alike: a value with a sound and a value, then without them.
    {
        auto value = *phazite::findDevice (song, synth);
        value.sounds = value.sounds.set (0, "Pulse Width");
        // The first of its parameters whose top isn't its default, at its top.
        std::uint32_t index = 0;

        while (! phazite::keepHeldValue (dev::Synth::info.parameters[index], dev::Synth::info.parameters[index].maximum).has_value())
            ++index;

        value.values = value.values.set (
            index, *phazite::keepHeldValue (dev::Synth::info.parameters[index], dev::Synth::info.parameters[index].maximum));
        inStep (song, "Set", [&] { CHECK (phazite::setDevice (song, value)); })();
        CHECK (*phazite::findDevice (song, synth) == value);
        value.sounds = {};
        value.values = {};
        inStep (song, "Set Back", [&] { CHECK (phazite::setDevice (song, value)); })();
        CHECK (writeWhole (song) == beforeSounds);
        phazite::test::checkSong (song);
    }

    // (d) Outside the history: written, no action.
    undo.clearUndoHistory();
    {
        const phazite::Song::OutsideHistory outside (song);
        CHECK (phazite::setDeviceFields (song, eq, [] (DeviceValue& value) { value.off = true; }));
        CHECK (phazite::setDeviceValue (song, eq, gain, 2.0f));
    }
    CHECK_FALSE (undo.canUndo());
    CHECK_FALSE (phazite::isDeviceOn (song, eq));
    phazite::test::checkSong (song);
}

TEST_CASE ("Device writers: a plug-in grouped into a container, moved to another chain and ungrouped is the same element at "
           "each move, its Undo and its Redo: its window's place, its value set in its window and its instance kept")
{
    // A window's place set after a move is on the element moved alone, as view state outside the history: were a move to put
    // a copy of the element in, the Undo would bring back the element from before the move, without that place.
    using phazite::test::TestPlugin;
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);
    DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Add Device");
        plugin = TestPlugin::add (song, s.firstTrack);
    }

    s.waitForPlugins();
    session->update();
    auto* instance = phazite::findPluginInstance (song, plugin);
    REQUIRE (instance != nullptr);

    // Its window's place set, and its gain turned in its window, which nothing stored.
    phazite::setWindowPlace (song, plugin, { .x = 10, .y = 20 });
    TestPlugin::setGain (song, plugin, 0.75f);

    const auto kept = [&] (phazite::WindowPlace place)
    {
        session->update();
        CHECK (phazite::findPluginInstance (song, plugin) == instance);
        CHECK (phazite::getWindowPlace (song, plugin) == std::optional (place));
        CHECK (TestPlugin::getGain (song, plugin) == doctest::Approx (0.75f));
    };

    phazite::test::UndoCheck undo (s.context);
    const auto undoAndRedo =
        [&] (phazite::WindowPlace place, const std::function<void()>& afterUndo, const std::function<void()>& afterRedo)
    {
        undo.stepBackOrForth ("Undo", [&] { phazite::undo (song); });
        kept (place);
        afterUndo();
        undo.stepBackOrForth ("Redo", [&] { phazite::redo (song); });
        kept (place);
        afterRedo();
    };

    // Grouped: into the container's chain.
    DeviceID container;
    undo.check ("Group into Container",
                inStep (song, "Group into Container", [&] { container = phazite::groupIntoContainer (song, { plugin }); }));
    REQUIRE (container.isValid());
    const auto first = phazite::getChains (song, container).front();
    kept ({ .x = 10, .y = 20 });
    CHECK (phazite::findChainOf (song, plugin) == first);
    phazite::setWindowPlace (song, plugin, { .x = 11, .y = 21 });
    undoAndRedo (
        { .x = 11, .y = 21 }, [&] { CHECK_FALSE (phazite::findContainerOf (song, plugin).isValid()); },
        [&] { CHECK (phazite::findChainOf (song, plugin) == first); });

    // Moved to the container's second chain.
    DeviceID second;
    undo.check ("Add Chain", inStep (song, "Add Chain", [&] { second = phazite::addChain (song, container); }));
    REQUIRE (second.isValid());
    undo.check ("Move Device", inStep (song, "Move Device", [&] { phazite::moveToChain (song, plugin, second); }));
    kept ({ .x = 11, .y = 21 });
    CHECK (phazite::findChainOf (song, plugin) == second);
    phazite::setWindowPlace (song, plugin, { .x = 12, .y = 22 });
    undoAndRedo (
        { .x = 12, .y = 22 }, [&] { CHECK (phazite::findChainOf (song, plugin) == first); },
        [&] { CHECK (phazite::findChainOf (song, plugin) == second); });

    // Ungrouped: back on the track, the container gone with its other chain.
    undo.check ("Remove Chain", inStep (song, "Remove Chain", [&] { phazite::removeChain (song, first); }));
    undo.check ("Ungroup Container", inStep (song, "Ungroup Container", [&] { phazite::ungroupContainer (song, container); }));
    kept ({ .x = 12, .y = 22 });
    CHECK_FALSE (phazite::findContainerOf (song, plugin).isValid());
    phazite::setWindowPlace (song, plugin, { .x = 13, .y = 23 });
    undoAndRedo (
        { .x = 13, .y = 23 }, [&] { CHECK (phazite::findChainOf (song, plugin) == second); },
        [&] { CHECK_FALSE (phazite::findContainerOf (song, plugin).isValid()); });

    phazite::test::checkSong (song);
    undo.walkBackAndForth();
    kept ({ .x = 13, .y = 23 });
}

TEST_CASE ("Device writers: a value set off its steps is written on them, as the song holds it, and reads the same opened again")
{
    // A compressor's toggle and choice, each set by hand to a value between its steps.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto compressor = s.addEffect (s.firstTrack, dev::Compressor::typeID);
    const auto& toggle = findParameterOf (dev::Compressor::info, dev::Unit::toggle);
    const auto& choice = findParameterOf (dev::Compressor::info, dev::Unit::choice);
    const auto set = [&] (const dev::ParameterInfo& parameter, float value)
    {
        inStep (song, "Set", [&]
                { phazite::setValueSetByHand (song, { compressor, juce::String (parameter.id.data(), parameter.id.size()) }, value); })();
    };

    const auto toggleOn = dev::constrain (toggle, toggle.defaultValue > 0.5f ? 0.25f : 0.75f);
    set (toggle, toggle.defaultValue > 0.5f ? 0.25f : 0.75f);
    set (choice, choice.minimum + 1.6f);

    const auto* device = phazite::findDevice (song, compressor);
    REQUIRE (device != nullptr);
    const auto toggleIndex = (std::uint32_t) (&toggle - dev::Compressor::info.parameters.data());
    const auto choiceIndex = (std::uint32_t) (&choice - dev::Compressor::info.parameters.data());
    CHECK (device->getParameterValue ((int) toggleIndex) == doctest::Approx (toggleOn));
    CHECK (device->getParameterValue ((int) choiceIndex) == doctest::Approx (choice.minimum + 2.0f));
    CHECK (phazite::getValueSetByHand (song, { compressor, juce::String (choice.id.data(), choice.id.size()) })
           == doctest::Approx (choice.minimum + 2.0f));
    phazite::test::checkSong (song); // the lenient reader and the as-held one agree

    // Saved and opened again: the same text.
    const auto text = writeWhole (song);
    const auto opened = phazite::makeSong (phazite::test::copyWholeSong (song));
    REQUIRE (opened != nullptr);
    CHECK (phazite::test::writeSong (*opened, phazite::test::SongPart::whole) == text);
}

TEST_CASE ("Device writers: a container made around three devices and taken apart, its chains duplicated, moved and removed, "
           "each one exact step, the song as Phazite wrote it")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto drums = s.addTrack ("Drums");
    const auto synth = s.setInstrument (track, dev::Synth::typeID);
    const auto delay = s.addEffect (track, dev::Delay::typeID);
    const auto compressor = s.addEffect (track, dev::Compressor::typeID);
    s.setParameter (delay, "mix", 0.4f);
    phazite::setSidechainSource (s.context, compressor, drums);
    REQUIRE (phazite::getSidechainSource (song, compressor) == drums);
    const auto eq = s.addEffect (track, dev::Equalizer::typeID);
    phazite::test::UndoCheck undo (s.context);

    // The three effects grouped: the container in the first's place, its chain holding them as they were but for the
    // compressor's sidechain, and its eight macros.
    const auto before = phazite::test::writeSong (song, phazite::test::SongPart::undoable); // the count of IDs aside
    const auto devicesBefore = getWrittenDevices (song, track);
    DeviceID container;
    undo.check ("Group into Container",
                inStep (song, "Group into Container", [&] { container = phazite::groupIntoContainer (song, { delay, compressor, eq }); }));
    REQUIRE (container.isValid());
    CHECK (phazite::getContainerMacros (song, container).size() == 8);
    CHECK (phazite::getDevices (song, track) == std::vector<DeviceID> { synth, container });
    const auto chain = phazite::getChains (song, container).front();
    CHECK (phazite::getChainDevices (song, chain) == std::vector<DeviceID> { delay, compressor, eq });
    CHECK_FALSE (phazite::getSidechainSource (song, compressor).isValid());
    {
        // The chain's devices written as they were on the channel, the compressor without its sidechain.
        auto expected = std::vector<juce::ValueTree> (devicesBefore.begin() + 1, devicesBefore.end());
        expected[1].removeProperty (schema::deviceSidechain, nullptr);
        const auto inside = getWrittenDevices (song, track)[1].getChildWithName (schema::CHAIN);
        std::vector<juce::ValueTree> now;

        for (const auto& each : inside)
            now.push_back (each);

        CHECK (withIDsInOrder (now) == withIDsInOrder (expected));
    }
    phazite::test::checkSong (song);

    // A chain duplicated (its copy as its original but for the IDs, after it), moved before it, and removed.
    DeviceID copy;
    undo.check ("Duplicate Chain", inStep (song, "Duplicate Chain", [&] { copy = phazite::duplicateChain (song, chain); }));
    REQUIRE (copy.isValid());
    CHECK (phazite::getChains (song, container) == std::vector<DeviceID> { chain, copy });
    {
        const auto chains = getWrittenDevices (song, track)[1];
        CHECK (withIDsInOrder ({ chains.getChild (0) }) == withIDsInOrder ({ chains.getChild (1) }));
    }
    undo.check ("Move Chain", inStep (song, "Move Chain", [&] { phazite::moveChain (song, copy, 0); }));
    CHECK (phazite::getChains (song, container) == std::vector<DeviceID> { copy, chain });
    undo.check ("Remove Chain", inStep (song, "Remove Chain", [&] { phazite::removeChain (song, copy); }));
    CHECK (phazite::getChains (song, container) == std::vector<DeviceID> { chain });

    // Taken apart: the devices back in its place, the song as before the grouping but for the sidechain it turned off.
    undo.check ("Ungroup Container", inStep (song, "Ungroup Container", [&] { phazite::ungroupContainer (song, container); }));
    CHECK (phazite::getDevices (song, track) == std::vector<DeviceID> { synth, delay, compressor, eq });
    phazite::setSidechainSource (s.context, compressor, drums);
    CHECK (phazite::test::writeSong (song, phazite::test::SongPart::undoable) == before);
    phazite::test::checkSong (song);

    // An instrument replaced, in its place.
    undo.check ("Replace Instrument", [&] { phazite::chooseInstrument (song, track, s.findDeviceType (dev::Synth::typeID)); });
    CHECK (phazite::getDevices (song, track).size() == 4);
    CHECK (phazite::getDevices (song, track).front() != synth);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Device writers: a container's preset and a track's preset come in with their macros set as values, "
           "and as the container they were saved from")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto& host = phazite::getPluginHost (phazite::test::getEngineContext());
    const auto container = phazite::addContainer (s.context, track);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (song, container).front();
    phazite::addEffectToChain (s.context, chain, s.findDeviceType (dev::Delay::typeID));
    inStep (song, "Volume", [&] { phazite::setChainVolume (song, chain, -6.0f); })();
    const auto macro = phazite::getContainerMacros (song, container).front();
    s.setMacroValue (macro, 0.25f);
    s.modulate (macro, phazite::ParameterRef { phazite::getChainDevices (song, chain).front(), "mix" }, 0.5f);
    const auto preset = phazite::makeDevicePreset (song, container);
    phazite::test::UndoCheck undo (s.context);

    const auto other = s.addTrack ("Other");
    DeviceID made;
    undo.check ("Add Device",
                inStep (song, "Add Device", [&] { made = phazite::addDeviceFromPreset (song, host, other, preset, nullptr); }));
    REQUIRE (made.isValid());
    const auto macros = phazite::getContainerMacros (song, made);
    REQUIRE (macros.size() == 8);
    CHECK (phazite::getModulatorValue (song, macros.front())->value == doctest::Approx (0.25f));
    CHECK (phazite::getModulatorValue (song, macros.front())->place.track == other);
    CHECK (withIDsInOrder (getWrittenDevices (song, other)) == withIDsInOrder (getWrittenDevices (song, track)));
    phazite::test::checkSong (song);

    // A track's preset with the container as an effect.
    const auto trackPreset = phazite::makeTrackPreset (song, track);
    TrackID added;
    undo.check ("Add Track",
                inStep (song, "Add Track", [&] { added = phazite::addTrackFromPreset (song, host, other, trackPreset, nullptr); }));
    REQUIRE (added.isValid());
    CHECK (withIDsInOrder (getWrittenDevices (song, added)) == withIDsInOrder (getWrittenDevices (song, track)));
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Device writers: a track with a container duplicated, its copy's devices as its own but for their IDs, with its "
           "macros, its automation and its window's places, and a sidechain within the copy listening to the copy")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto drums = s.addTrack ("Drums");
    const auto keys = s.addTrack ("Keys");
    const auto bus = s.addBus ("Bus", { drums, keys });
    s.setInstrument (keys, dev::Synth::typeID);
    const auto compressor = s.addEffect (keys, dev::Compressor::typeID);
    phazite::setSidechainSource (s.context, compressor, drums);
    const auto container = phazite::addContainer (s.context, keys);
    const auto chain = phazite::getChains (song, container).front();
    phazite::addEffectToChain (s.context, chain, s.findDeviceType (dev::Reverb::typeID));
    const auto reverb = phazite::getChainDevices (song, chain).front();
    phazite::setWindowPlace (song, compressor, { .x = 30, .y = 40 });
    s.addAutomation ({ reverb, "mix" }, { { phazite::test::at (0.0), 0.25f }, { phazite::test::at (4.0), 0.75f } });
    const auto macro = phazite::getContainerMacros (song, container).front();
    s.modulate (macro, { reverb, "mix" }, 0.5f);
    phazite::test::UndoCheck undo (s.context);

    std::vector<TrackID> copies;
    undo.check ("Duplicate Track", inStep (song, "Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { bus }, nullptr); }));
    REQUIRE (copies.size() == 1);
    const auto inside = phazite::getTracksInFolder (song, copies.front());
    REQUIRE (inside.size() == 2);
    const auto keysCopy = inside[1];

    // Its devices written as the original's, but for the IDs (and the sidechain, which names the copy of the drums).
    CHECK (withIDsInOrder (getWrittenDevices (song, keysCopy)) == withIDsInOrder (getWrittenDevices (song, keys)));
    const auto copiedDevices = phazite::getDevices (song, keysCopy);
    REQUIRE (copiedDevices.size() == 3);
    CHECK (phazite::getSidechainSource (song, copiedDevices[1]) == inside[0]);
    CHECK (phazite::getWindowPlace (song, copiedDevices[1]).has_value());
    const auto copiedChain = phazite::getChains (song, copiedDevices[2]).front();
    const auto copiedReverb = phazite::getChainDevices (song, copiedChain).front();
    CHECK (phazite::getContainerMacros (song, copiedDevices[2]).size() == 8);
    CHECK (phazite::findCurve (song, { copiedReverb, "mix" }) != nullptr);
    CHECK_FALSE (phazite::getModulations (song, phazite::ParameterRef { copiedReverb, "mix" }).empty());
    phazite::test::checkSong (song);
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: Swap's trial tried twice and kept, and tried twice and put back, writes a pad's sound as the song "
           "held it before")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto drums = s.setInstrument (s.firstTrack, dev::DrumSampler::typeID);
    const auto kick = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Kick.wav"), 0.1);
    const auto snare = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Snare.wav"), 0.2);
    phazite::test::UndoCheck undo (s.context);

    const auto before = writeWhole (song);
    {
        phazite::Swap swap (s.context, drums, 38);
        REQUIRE (swap.isOpen());
        REQUIRE (swap.tryFile (kick));
        REQUIRE (swap.tryFile (snare));
        swap.putBack();
    }
    CHECK (writeWhole (song) == before);

    undo.checkSeveral ("Swap kept",
                       [&]
                       {
                           phazite::Swap swap (s.context, drums, 38);
                           REQUIRE (swap.tryFile (kick));
                           REQUIRE (swap.tryFile (snare));
                           swap.keep();
                           phazite::test::waitForAudioWork (s.context);
                       });
    CHECK (phazite::test::copyDevice (song, drums).getChildWithName (schema::SOUNDS).hasProperty (schema::getPadSoundFile (38, 0)));
    phazite::test::checkSong (song);
}

TEST_CASE ("Device writers: a bus made plain stores its plug-ins' states first, so that its Undo brings them back as they were")
{
    const phazite::test::TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);
    const auto bus = s.addBus ("Bus", { s.firstTrack });
    phazite::DeviceID plugin;

    {
        const phazite::UndoStep step (song, "Add Device");
        plugin = phazite::test::TestPlugin::add (song, bus);
    }

    s.waitForPlugins();
    session->update();
    REQUIRE (phazite::findPluginInstance (song, plugin) != nullptr);

    // Its gain turned in its window to a quarter, the song's devices stored, then to three quarters, which nothing stored.
    phazite::test::TestPlugin::setGain (song, plugin, 0.25f);
    song.storeDeviceStates();
    phazite::test::TestPlugin::setGain (song, plugin, 0.75f);

    {
        const phazite::UndoStep step (song, "Make Folder Plain");
        phazite::makePlainFolder (song, bus);
    }

    CHECK_FALSE (phazite::isBus (song, bus));
    CHECK (phazite::getDevices (song, bus).empty());
    session->update();
    REQUIRE (phazite::findPluginInstance (song, plugin) == nullptr);
    phazite::test::checkSong (song);

    phazite::undo (song);
    s.waitForPlugins();
    REQUIRE (phazite::findPluginInstance (song, plugin) != nullptr);
    CHECK (phazite::test::TestPlugin::getGain (song, plugin) == doctest::Approx (0.75f));
}

TEST_CASE ("Device writers: a track duplicated holds its devices before its lanes, as the original does, its lanes naming the "
           "copy's devices, in one step whose Undo and Redo are exact")
{
    // Whole state writes a track's devices right after its clips, before its other children (DevicesArea.h:
    // writeDevicesIn()), whichever was made first: a lane shown before the devices were added, or after.
    const auto lanesType = schema::LANES.getName().toString();

    for (const auto laneShownFirst : { false, true })
    {
        CAPTURE (laneShownFirst);
        phazite::test::TestSong s;
        auto& song = *phazite::findSong (s.context);
        const auto track = s.firstTrack;

        if (laneShownFirst)
            phazite::showLane (song, track, s.getVolumeParameter (track));

        s.setInstrument (track, dev::Synth::typeID);
        const auto delay = s.addEffect (track, dev::Delay::typeID);
        REQUIRE (phazite::addContainer (s.context, track).isValid());
        phazite::showLane (song, track, { delay, "mix" });

        // The original's devices and its container before its lanes.
        const auto original = getChildTypes (getWrittenTrack (song, track));
        const auto lanesAt = original.indexOf (lanesType);
        REQUIRE (lanesAt >= 0);
        CHECK (original.indexOf (schema::DEVICE.getName().toString()) < lanesAt);
        CHECK (original.indexOf (schema::CONTAINER.getName().toString()) < lanesAt);

        phazite::test::UndoCheck undo (s.context);
        std::vector<TrackID> copies;
        checkOneStep (undo, song, "Duplicate Track",
                      inStep (song, "Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); }));
        REQUIRE (copies.size() == 1);
        const auto copy = copies.front();

        // Its children the original's, kind for kind: its devices and its container, then its lanes.
        const auto copied = getChildTypes (getWrittenTrack (song, copy));
        CHECK (copied == original);
        CHECK (copied.indexOf (schema::CONTAINER.getName().toString()) < copied.indexOf (lanesType));

        // Its lanes name its own devices (and its own volume).
        const auto lanes = phazite::getLanes (song, copy);
        const auto copiedDevices = phazite::getDevices (song, copy);
        REQUIRE (lanes.size() == (laneShownFirst ? 2u : 1u));
        REQUIRE (copiedDevices.size() == 3);
        CHECK (lanes.back().parameter == phazite::ParameterRef { copiedDevices[1], "mix" });

        if (laneShownFirst)
            CHECK (lanes.front().parameter == s.getVolumeParameter (copy));

        undo.walkBackAndForth();
        CHECK (getChildTypes (getWrittenTrack (song, copy)) == original);
    }
}

TEST_CASE ("Device writers: a bus folder duplicated holds its children in the original's order: its devices, then its tracks, "
           "then its lanes")
{
    // A bus with an effect, its lane shown while it held one track, then a second track moved into it: its children in the
    // order whole state writes them, its devices first, then its tracks, then its lanes (a folder's LANES has no place among
    // its tracks: the song holds no LANES element, its lanes are the view's, written after the folder's tracks).
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto second = s.addTrack ("Second");
    const auto bus = s.addBus ("Bus", { s.firstTrack });
    s.addEffect (bus, dev::Delay::typeID);
    phazite::showLane (song, bus, s.getVolumeParameter (bus));

    {
        const phazite::UndoStep step (song, "Into the Folder");
        phazite::moveTrackIntoFolder (song, second, bus);
    }

    const auto original = getWrittenTrack (song, bus);
    const auto types = getChildTypes (original);
    const auto track = schema::TRACK.getName().toString();
    REQUIRE (types == juce::StringArray { schema::DEVICE.getName().toString(), track, track, schema::LANES.getName().toString() });

    phazite::test::UndoCheck undo (s.context);
    std::vector<TrackID> copies;
    checkOneStep (undo, song, "Duplicate Track",
                  inStep (song, "Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { bus }, nullptr); }));
    REQUIRE (copies.size() == 1);

    // The copy whole, the original's but for the IDs: its children in the original's order.
    CHECK (getChildTypes (getWrittenTrack (song, copies.front())) == types);
    CHECK (withIDsInOrder ({ getWrittenTrack (song, copies.front()) }) == withIDsInOrder ({ original }));

    undo.walkBackAndForth();
    CHECK (withIDsInOrder ({ getWrittenTrack (song, copies.front()) }) == withIDsInOrder ({ original }));
}

TEST_CASE ("Device writers: a track duplicated holds no copy of its devices' MIDI links")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    const auto delay = s.addEffect (track, dev::Delay::typeID);
    inStep (song, "Link", [&] { phazite::linkMidiController (song, { .channel = 1, .number = 20 }, { delay, "mix" }); })();
    REQUIRE (phazite::getMidiLinks (song).size() == 1);
    phazite::test::UndoCheck undo (s.context);

    std::vector<TrackID> copies;
    checkOneStep (undo, song, "Duplicate Track",
                  inStep (song, "Duplicate Track", [&] { copies = phazite::duplicateTracks (song, { track }, nullptr); }));
    REQUIRE (copies.size() == 1);
    const auto copiedDevices = phazite::getDevices (song, copies.front());
    REQUIRE (copiedDevices.size() == 2);
    CHECK (phazite::getMidiLinks (song).size() == 1);
    CHECK (phazite::findMidiLink (song, { delay, "mix" }).has_value());
    CHECK_FALSE (phazite::findMidiLink (song, { copiedDevices[1], "mix" }).has_value());
}

TEST_CASE ("Device writers: a device turned off and on, and a device's and a container's mix set and set back, each one named "
           "step, the device as the song held it but for what changed")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    const auto delay = s.addEffect (track, dev::Delay::typeID);
    const auto container = phazite::addContainer (s.context, track);
    REQUIRE (container.isValid());
    phazite::test::UndoCheck undo (s.context);

    // Off, then on.
    auto expected = findHeld (song, delay);
    expected.off = true;
    checkOneStep (undo, song, "Turn Device Off", [&] { phazite::setDeviceOn (song, delay, false); });
    CHECK (findHeld (song, delay) == expected);
    CHECK_FALSE (phazite::isDeviceOn (song, delay));
    expected.off = false;
    checkOneStep (undo, song, "Turn Device On", [&] { phazite::setDeviceOn (song, delay, true); });
    CHECK (findHeld (song, delay) == expected);

    // A device's mix is its mix parameter's value, held as the song holds it; set back to its default, none.
    const auto before = writeWhole (song);
    const auto mix = indexOf (dev::Delay::info, dev::mixParameterID);
    const auto& mixInfo = dev::Delay::info.parameters[mix];
    const auto wet = juce::approximatelyEqual (mixInfo.defaultValue, 0.55f) ? 0.65f : 0.55f;
    checkOneStep (undo, song, "Change Mix", inStep (song, "Change Mix", [&] { phazite::setMix (song, delay, wet); }));
    const auto held = phazite::keepHeldValue (mixInfo, wet);
    REQUIRE (held.has_value());
    expected.values = expected.values.set (mix, *held);
    CHECK (findHeld (song, delay) == expected);
    CHECK (phazite::getMix (song, delay) == doctest::Approx (wet));
    checkOneStep (undo, song, "Change Mix", inStep (song, "Change Mix", [&] { phazite::setMix (song, delay, mixInfo.defaultValue); }));
    CHECK (writeWhole (song) == before);

    // A container's mix is its own field, none (1) left out of the file.
    auto container0 = findHeld (song, container);
    container0.mix = 0.4f;
    checkOneStep (undo, song, "Change Mix", inStep (song, "Change Mix", [&] { phazite::setMix (song, container, 0.4f); }));
    CHECK (findHeld (song, container) == container0);
    checkOneStep (undo, song, "Change Mix", inStep (song, "Change Mix", [&] { phazite::setMix (song, container, 1.0f); }));
    CHECK (writeWhole (song) == before);
}

TEST_CASE ("Device writers: a chain's volume, pan, keys and velocities set, each one named step, the chain as the song held it "
           "but for what changed")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    const auto container = phazite::addInstrumentContainer (s.context, track);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (song, container).front();
    phazite::test::UndoCheck undo (s.context);
    auto expected = findHeld (song, chain);

    checkOneStep (undo, song, "Change Chain Volume",
                  inStep (song, "Change Chain Volume", [&] { phazite::setChainVolume (song, chain, -6.5f); }));
    expected.volume = -6.5f;
    CHECK (findHeld (song, chain) == expected);

    checkOneStep (undo, song, "Change Chain Pan", inStep (song, "Change Chain Pan", [&] { phazite::setChainPan (song, chain, -0.25f); }));
    expected.pan = -0.25f;
    CHECK (findHeld (song, chain) == expected);

    checkOneStep (undo, song, "Change Key Range", [&] { phazite::setChainKeys (s.context, chain, { 36, 72 }); });
    expected.keys = { 36, 72 };
    CHECK (findHeld (song, chain) == expected);

    checkOneStep (undo, song, "Change Velocity Range", [&] { phazite::setChainVelocities (s.context, chain, { 20, 100 }); });
    expected.velocities = { 20, 100 };
    CHECK (findHeld (song, chain) == expected);
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: a container split into bands keeps its chains' crossovers in order as a chain is added and as a "
           "crossover set past its neighbour is put back between them, each one named step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    s.setInstrument (track, dev::Synth::typeID);
    const auto container = phazite::addContainer (s.context, track);
    REQUIRE (container.isValid());
    phazite::test::UndoCheck undo (s.context);

    const auto crossoversOf = [&song, container]
    {
        std::vector<std::optional<float>> crossovers;

        for (const auto chain : phazite::getChains (song, container))
            crossovers.push_back (findHeld (song, chain).crossover);

        return crossovers;
    };

    const auto same = [] (const std::vector<std::optional<float>>& a, const std::vector<std::optional<float>>& b)
    {
        return a.size() == b.size()
               && std::equal (a.begin(), a.end(), b.begin(), [] (const std::optional<float>& x, const std::optional<float>& y)
                              { return x.has_value() == y.has_value() && (! x.has_value() || juce::approximatelyEqual (*x, *y)); });
    };

    // Two chains in bands: the second from its place's default, the first from the bottom.
    checkOneStep (undo, song, "Add Chain", [&] { phazite::addChain (s.context, container); });
    checkOneStep (undo, song, "Split Container",
                  [&] { phazite::setContainerSplit (s.context, container, phazite::ContainerSplit::bands); });
    CHECK (findHeld (song, container).split == phazite::ContainerSplit::bands);
    CHECK (same (crossoversOf(), { std::nullopt, phazite::getDefaultCrossover (1, 2) }));

    // A third added above them, from its place's default.
    checkOneStep (undo, song, "Add Chain", [&] { phazite::addChain (s.context, container); });
    CHECK (same (crossoversOf(), { std::nullopt, phazite::getDefaultCrossover (1, 2), phazite::getDefaultCrossover (2, 3) }));

    // The second's set past the third's: a split into bands again puts the third, below it now, between it and the top, in
    // octaves.
    const auto chains = phazite::getChains (song, container);
    REQUIRE (chains.size() == 3);
    checkOneStep (undo, song, "Change Crossover",
                  inStep (song, "Change Crossover", [&] { phazite::setChainCrossover (song, chains[1], 5000.0f); }));
    CHECK (same (crossoversOf(), { std::nullopt, 5000.0f, phazite::getDefaultCrossover (2, 3) }));
    checkOneStep (undo, song, "Split Container",
                  [&] { phazite::setContainerSplit (s.context, container, phazite::ContainerSplit::layers); });
    checkOneStep (undo, song, "Split Container",
                  [&] { phazite::setContainerSplit (s.context, container, phazite::ContainerSplit::bands); });
    CHECK (same (crossoversOf(), { std::nullopt, 5000.0f, std::sqrt (5000.0f * phazite::highestCrossover) }));
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: effects moved within a chain and to another chain, an instrument replaced in a chain and an "
           "instrument container added, each one named step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, dev::Synth::typeID);
    phazite::test::UndoCheck undo (s.context);

    // An instrument container in the synth's place, the synth first in its chain, the same device; its macros set as values.
    auto synthBefore = findHeld (song, synth);
    DeviceID container;
    checkOneStep (undo, song, "Add Instrument Container", [&] { container = phazite::addInstrumentContainer (s.context, track); });
    REQUIRE (container.isValid());
    CHECK (phazite::getDevices (song, track) == std::vector<DeviceID> { container });
    CHECK (findHeld (song, container).instrument);
    CHECK (phazite::getContainerMacros (song, container).size() == 8);
    const auto chain = phazite::getChains (song, container).front();
    CHECK (phazite::getChainDevices (song, chain) == std::vector<DeviceID> { synth });
    synthBefore.place = { .track = track, .container = container, .chain = chain };
    CHECK (findHeld (song, synth) == synthBefore);

    // Two effects after it.
    phazite::addEffectToChain (s.context, chain, s.findDeviceType (dev::Delay::typeID));
    phazite::addEffectToChain (s.context, chain, s.findDeviceType (dev::Equalizer::typeID));
    auto devices = phazite::getChainDevices (song, chain);
    REQUIRE (devices.size() == 3);
    const auto delay = devices[1], eq = devices[2];

    // The equalizer moved before the delay, never before the instrument.
    checkOneStep (undo, song, "Move Device", [&] { phazite::moveEffect (s.context, eq, -1); });
    CHECK (phazite::getChainDevices (song, chain) == std::vector<DeviceID> { synth, eq, delay });

    // The delay moved into a second chain, the same device in its new place.
    DeviceID second;
    checkOneStep (undo, song, "Add Chain", [&] { second = phazite::addChain (s.context, container); });
    auto delayBefore = findHeld (song, delay);
    checkOneStep (undo, song, "Move Device", [&] { phazite::moveEffectToChain (s.context, delay, second, -1); });
    CHECK (phazite::getChainDevices (song, chain) == std::vector<DeviceID> { synth, eq });
    CHECK (phazite::getChainDevices (song, second) == std::vector<DeviceID> { delay });
    delayBefore.place.chain = second;
    CHECK (findHeld (song, delay) == delayBefore);

    // The synth replaced in its chain: a new one first, the equalizer after it.
    checkOneStep (undo, song, "Replace Instrument with Synth",
                  [&] { phazite::chooseChainInstrument (s.context, chain, s.findDeviceType (dev::Synth::typeID)); });
    devices = phazite::getChainDevices (song, chain);
    REQUIRE (devices.size() == 2);
    CHECK (devices[0] != synth);
    CHECK (devices[1] == eq);
    CHECK_FALSE (phazite::hasDevice (song, synth));
    CHECK (findHeld (song, devices[0]).type == juce::String (dev::Synth::typeID));
    CHECK (findHeld (song, devices[0]).values.empty());
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: a track ducked under another, an effect replaced, a return with an effect and devices removed, "
           "each one named step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto lead = s.firstTrack;
    const auto drums = s.addTrack ("Drums");
    const auto other = s.addTrack ("Other");
    s.rename (lead, "Lead");
    s.setInstrument (lead, dev::Synth::typeID);
    s.setInstrument (drums, dev::Synth::typeID);
    const auto delay = s.addEffect (lead, dev::Delay::typeID);
    const auto eq = s.addEffect (lead, dev::Equalizer::typeID);
    phazite::test::UndoCheck undo (s.context);

    // Ducked: a compressor after its effects, listening to the drums, set as a compressor given that sidechain is.
    DeviceID compressor;
    checkOneStep (undo, song, "Duck Lead Under Drums", [&] { compressor = phazite::duckUnder (s.context, lead, drums); });
    REQUIRE (compressor.isValid());
    CHECK (phazite::getDevices (song, lead).back() == compressor);
    const auto likeIt = s.addEffect (other, dev::Compressor::typeID);
    phazite::setSidechainSource (s.context, likeIt, drums);
    const auto ducking = findHeld (song, compressor);
    CHECK (ducking.type == juce::String (dev::Compressor::typeID));
    CHECK (ducking.sidechain == drums);
    CHECK (ducking.values == findHeld (song, likeIt).values);

    // The delay replaced by a reverb, in its place.
    const auto reverbType = s.findDeviceType (dev::Reverb::typeID);
    checkOneStep (undo, song, "Replace " + phazite::getDeviceName (song, delay) + " with " + reverbType.name,
                  [&] { phazite::replaceEffect (s.context, delay, reverbType); });
    const auto devices = phazite::getDevices (song, lead);
    REQUIRE (devices.size() == 4);
    CHECK_FALSE (phazite::hasDevice (song, delay));
    const auto reverb = devices[1];
    CHECK (findHeld (song, reverb).type == juce::String (dev::Reverb::typeID));
    CHECK (findHeld (song, reverb).values.empty());
    CHECK (devices[2] == eq);

    // A return holding a reverb at a mix of 100 %, fed by a send from the lead.
    TrackID added;
    checkOneStep (undo, song, "Add " + reverbType.name + " Return", [&] { added = phazite::addReturnWith (s.context, lead, reverbType); });
    REQUIRE (added.isValid());
    const auto onReturn = phazite::getDevices (song, added);
    REQUIRE (onReturn.size() == 1);
    const auto mix = indexOf (dev::Reverb::info, dev::mixParameterID);
    const auto returned = findHeld (song, onReturn.front());
    CHECK (returned.type == juce::String (dev::Reverb::typeID));
    CHECK (returned.getParameterValue ((int) mix) == doctest::Approx (1.0f));
    CHECK (phazite::getSends (song, lead).size() == 1);

    // The equalizer removed from the lead, and an effect removed from a container's chain.
    checkOneStep (undo, song, "Remove " + phazite::getDeviceName (song, eq), [&] { phazite::removeDevice (s.context, eq); });
    CHECK_FALSE (phazite::hasDevice (song, eq));
    CHECK (phazite::getDevices (song, lead) == std::vector<DeviceID> { devices[0], reverb, compressor });

    const auto container = phazite::addContainer (s.context, other);
    const auto chain = phazite::getChains (song, container).front();
    phazite::addEffectToChain (s.context, chain, s.findDeviceType (dev::Delay::typeID));
    phazite::addEffectToChain (s.context, chain, s.findDeviceType (dev::Equalizer::typeID));
    const auto inChain = phazite::getChainDevices (song, chain);
    REQUIRE (inChain.size() == 2);
    checkOneStep (undo, song, "Remove " + phazite::getDeviceName (song, inChain[0]),
                  [&] { phazite::removeDevice (s.context, inChain[0]); });
    CHECK (phazite::getChainDevices (song, chain) == std::vector<DeviceID> { inChain[1] });
    CHECK_FALSE (phazite::hasDevice (song, inChain[0]));
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: the sidechains of a track deleted, and of a device grouped into a container, are forgotten in "
           "their step, the device as it was but for its sidechain")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto drums = s.addTrack ("Drums");
    const auto bass = s.addTrack ("Bass");
    s.setInstrument (track, dev::Synth::typeID);
    s.setInstrument (drums, dev::Synth::typeID);
    s.setInstrument (bass, dev::Synth::typeID);
    const auto first = s.addEffect (track, dev::Compressor::typeID);
    const auto second = s.addEffect (track, dev::Compressor::typeID);
    phazite::setSidechainSource (s.context, first, drums);
    phazite::setSidechainSource (s.context, second, bass);
    phazite::test::UndoCheck undo (s.context);

    // The drums deleted: the first listens to what passes through it again.
    auto expected = findHeld (song, first);
    expected.sidechain = {};
    checkOneStep (undo, song, "Delete Track", inStep (song, "Delete Track", [&] { phazite::deleteTracks (song, { drums }, nullptr); }));
    CHECK (findHeld (song, first) == expected);
    CHECK (findHeld (song, second).sidechain == bass);

    // The second grouped into a container: a device in a container has no sidechain.
    expected = findHeld (song, second);
    expected.sidechain = {};
    DeviceID container;
    checkOneStep (undo, song, "Group into Container", [&] { container = phazite::groupIntoContainer (s.context, { second }); });
    REQUIRE (container.isValid());
    expected.place = { .track = track, .container = container, .chain = phazite::getChains (song, container).front() };
    CHECK (findHeld (song, second) == expected);
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: a preset loaded into a device, and tried in one, replaces its values and its sounds, one named step")
{
    phazite::test::TemporaryFolder temp;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto other = s.addTrack ("Other");
    const auto third = s.addTrack ("Third");
    const auto source = s.setInstrument (track, dev::Synth::typeID);
    const auto target = s.setInstrument (other, dev::Synth::typeID);
    const auto tried = s.setInstrument (third, dev::Synth::typeID);

    // The preset's synth: a table and two values of its own; the others: other values, no table.
    const auto& parameters = dev::Synth::info.parameters;
    const auto valueOf = [&parameters] (size_t index, float part)
    { return parameters[index].minimum + part * (parameters[index].maximum - parameters[index].minimum); };

    inStep (song, "Set", [&] { phazite::setDeviceSound (song, source, schema::getOscillatorWavetable (0), "Pulse Width"); })();
    inStep (song, "Set",
            [&]
            {
                phazite::setDeviceValue (song, source, 0, valueOf (0, 0.3f));
                phazite::setDeviceValue (song, source, 1, valueOf (1, 0.6f));
                phazite::setDeviceValue (song, target, 2, valueOf (2, 0.8f));
                phazite::setDeviceValue (song, tried, 3, valueOf (3, 0.7f));
            })();
    const auto preset = findHeld (song, source);
    REQUIRE_FALSE (preset.values.empty());
    REQUIRE_FALSE (preset.sounds.empty());
    const auto file = temp.folder.getChildFile ("Bright" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (s.context, source, file));
    phazite::test::UndoCheck undo (s.context);

    auto expected = findHeld (song, target);
    expected.values = preset.values;
    expected.sounds = preset.sounds;
    checkOneStep (undo, song, "Load Preset Bright", [&] { REQUIRE (phazite::loadPresetInto (s.context, target, file)); });
    CHECK (findHeld (song, target) == expected);

    expected = findHeld (song, tried);
    expected.values = preset.values;
    expected.sounds = preset.sounds;
    checkOneStep (undo, song, "Try Preset", inStep (song, "Try Preset", [&] { REQUIRE (phazite::tryPresetIn (song, tried, file)); }));
    CHECK (findHeld (song, tried) == expected);
    undo.walkBackAndForth();
}

TEST_CASE ("Device writers: a value whose ID the song holds as another kind of item (a device, a container, a chain) is "
           "refused, the song as it was and no undo action")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto track = s.firstTrack;
    const auto synth = s.setInstrument (track, dev::Synth::typeID);
    const auto container = phazite::addContainer (s.context, track);
    REQUIRE (container.isValid());
    const auto chain = phazite::getChains (song, container).front();
    song.getUndoManager().clearUndoHistory();
    const auto before = writeWhole (song);

    // Each item's value given another kind's form: a device as a chain and as a container, a chain as a device and as a
    // container, a container as a device and as a chain.
    for (const auto id : { synth, chain, container })
    {
        for (const auto form :
             { DeviceValue::Form::own, DeviceValue::Form::plugin, DeviceValue::Form::container, DeviceValue::Form::chain })
        {
            auto value = findHeld (song, id);
            const auto isDevice = value.form == DeviceValue::Form::own || value.form == DeviceValue::Form::plugin;

            if (value.form == form || (isDevice && (form == DeviceValue::Form::own || form == DeviceValue::Form::plugin)))
                continue;

            CAPTURE ((int) form);
            value.form = form;
            value.name = "Other";

            {
                const phazite::UndoStep step (song, "Nothing");
                CHECK_FALSE (phazite::setDevice (song, value));
            }

            CHECK_FALSE (song.getUndoManager().canUndo());
            CHECK (writeWhole (song) == before);
        }
    }

    phazite::test::checkSong (song);
}

TEST_CASE ("Device writers: a toggle with a curve of one point, set by hand between its steps, moves the point to its step")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto compressor = s.addEffect (s.firstTrack, dev::Compressor::typeID);
    const auto& toggle = findParameterOf (dev::Compressor::info, dev::Unit::toggle);
    const phazite::ParameterRef ref { compressor, juce::String (toggle.id.data(), toggle.id.size()) };
    s.addAutomation (ref, { { phazite::test::at (2.0), toggle.defaultValue } });

    // Set to a value between its steps: the device holds the step, and so does the curve's one point.
    const auto offStep = toggle.defaultValue > 0.5f ? 0.25f : 0.75f;
    const auto stepped = dev::constrain (toggle, offStep);
    REQUIRE_FALSE (juce::approximatelyEqual (stepped, offStep));
    inStep (song, "Set", [&] { phazite::setValueSetByHand (song, ref, offStep); })();

    const auto* curve = phazite::findCurve (song, ref);
    REQUIRE (curve != nullptr);
    REQUIRE (curve->points.size() == 1);
    const auto travel = phazite::getControlTravel (song, ref);
    REQUIRE (travel.has_value());
    CHECK (curve->points[0].position == doctest::Approx (phazite::toControlPosition (*travel, stepped)));
    CHECK_FALSE (curve->points[0].position == doctest::Approx (phazite::toControlPosition (*travel, offStep)));
    CHECK (phazite::getValueSetByHand (song, ref) == doctest::Approx (stepped));
    phazite::test::checkSong (song);
}
