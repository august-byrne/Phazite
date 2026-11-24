// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song's devices store in the history and outside it alike (model/base/internal/DeviceStoreValue.h: a plug-in's
// values set by hand, its state, program, format ID and version, a chain's mute and solo) is an area of the song's values,
// keyed (an Undo puts back only the entries its step changed), with each device's window's place outside the history alone
// (roadmap step S5.20; DeviceStoreArea.h). For each write the model makes, its Undo and its Redo: what the values' listener is
// told in all (SongValues.h: Listener::deviceStoreChanged()), the store handed whole the same as the area's value, what reaches
// the plug-in (the stand-in of TestPlugin.h, its gain read from its instance and from its helper), and nothing told of
// another area. The sequences the keyed area exists for (design J.1, on a bare area in DeviceStoreValueTests.cpp) on a real
// song with the stand-in. And the keeper's safety net, whole state and the loader's rule.
//   phazite_tests.exe --test-case="Song values' device store*"
#include "SongCheck.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/plugins/internal/SandboxedInstance.h"
#include "model/presets/PresetModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace
{
using Change = phazite::SongValues::DeviceStoreChange;
using Kind = Change::Kind;
using phazite::DeviceID;
using phazite::DeviceStore;
using phazite::test::TestPlugin;

// What the values' listener was told of the store, each entry in the order told, and how many calls of the other areas.
struct Told final : phazite::SongValues::Listener
{
    explicit Told (const phazite::Song& s)
        : song (s)
    {
        song.getValues()->addListener (this);
    }

    ~Told() override { song.getValues()->removeListener (this); }

    void deviceStoreChanged (const std::vector<Change>& changes, const DeviceStore& now) override
    {
        ++calls;
        CHECK_FALSE (changes.empty());
        CHECK (now.isSameAs (song.getValues()->deviceStore.get())); // the store handed whole, the area's value

        for (size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            CHECK (
                std::none_of (changes.begin(), changes.begin() + (std::ptrdiff_t) i, [&change] (const Change& other)
                              { return other.item == change.item && other.kind == change.kind && other.parameter == change.parameter; }));
            CHECK ((change.appeared && change.went) == false);
            entries.push_back (change);
        }
    }

    void clipNotesChanged (const std::vector<phazite::ClipID>&) override { ++others; }
    void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++others; }
    void tempoMapChanged() override { ++others; }
    void curvesChanged (const std::vector<phazite::ParameterRef>&) override { ++others; }

    void modulationChanged (const std::vector<phazite::SongValues::ModulationChange>&, const std::vector<phazite::ModulatorPlace>&) override
    {
        ++others;
    }

    void sendsChanged (const std::vector<phazite::SongValues::SendChange>&, const std::vector<phazite::TrackID>&,
                       const phazite::SongSends&) override
    {
        ++others;
    }

    void devicesChanged (const std::vector<phazite::SongValues::DeviceChange>&, const std::vector<phazite::DevicePlace>&,
                         const phazite::SongDevices&) override
    {
        ++devices;
    }

    // What was told since the last take, taken.
    std::vector<Change> take() { return std::exchange (entries, {}); }

    const phazite::Song& song;
    int calls = 0, others = 0, devices = 0;
    std::vector<Change> entries;
};

// The last of what was told of an entry, none for none.
std::optional<Change> findTold (const std::vector<Change>& told, Kind kind, DeviceID item, const juce::String& parameter = {})
{
    for (auto each = told.rbegin(); each != told.rend(); ++each)
        if (each->kind == kind && each->item == item && each->parameter == parameter)
            return *each;

    return std::nullopt;
}

// A song with the stand-in plug-in on its first track, loaded, and nothing in its history.
struct WithPlugin
{
    WithPlugin()
    {
        {
            const phazite::UndoStep step (song, "Plug-in");
            plugin = TestPlugin::add (song, s.firstTrack);
        }

        s.waitForPlugins();
        gain = { plugin, TestPlugin::gainID };
        tone = { plugin, TestPlugin::toneID };
        song.getUndoManager().clearUndoHistory();
    }

    phazite::SandboxedInstance& getInstance() const
    {
        auto* instance = phazite::findPluginInstance (song, plugin);
        REQUIRE (instance != nullptr);
        return *instance;
    }

    float getInstanceGain() const
    {
        auto& instance = getInstance();
        return instance.getParameterValue (instance.findParameter (gain.parameter));
    }

    // The plug-in itself, in its helper, comes to a gain.
    bool helperHas (float value) const
    {
        return phazite::test::dispatchMessagesUntil ([this, value]
                                                     { return std::abs (TestPlugin::getGain (song, plugin) - value) < 1.0e-4f; });
    }

    void inStep (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (song, name);
        change();
    }

    // Every plug-in's state asked for and in, then stored as a save stores it.
    void storeStates()
    {
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [this] { return phazite::getPluginsWithOlderStates (song).isEmpty() && phazite::getPluginsAnswering (song).isEmpty(); }));
        song.storeDeviceStates();
    }

    // The plug-in's tone set as its own window sets it (TestPlugin::setGain() sets its gain so): its state with another tone
    // and its gain as it has it, which it takes as its own, telling its host; waited for until its instance has the tone and
    // Phazite its state as it is then.
    void setToneInWindow (float value) const
    {
        auto& instance = getInstance();
        const auto held = instance.getRemote().getState (phazite::test::hangTimeout);
        REQUIRE (held.has_value());
        juce::MemoryInputStream before (*held, false);
        juce::MemoryOutputStream changed;
        changed.writeFloat (before.readFloat()); // its gain as it is
        changed.writeFloat (value);
        changed.writeFloat (1.0f);
        REQUIRE (instance.getRemote().setState (changed.getMemoryBlock(), phazite::test::hangTimeout));
        const auto index = instance.findParameter (tone.parameter);
        REQUIRE (index >= 0);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return juce::exactlyEqual (instance.getParameterValue (index), value); }));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! instance.isStateUnasked() && ! instance.isAskingState(); }));
    }

    float getInstanceTone() const
    {
        auto& instance = getInstance();
        return instance.getParameterValue (instance.findParameter (tone.parameter));
    }

    const TestPlugin testPlugin; // known while the song is
    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    DeviceID plugin;
    phazite::ParameterRef gain, tone;
};
} // namespace

//==============================================================================
TEST_CASE ("Song values' device store: a plug-in's first value set by hand, another, and one back to its default, each with its "
           "Undo and Redo, told and given to the plug-in as the tree gave it")
{
    WithPlugin p;
    Told told (p.song);
    REQUIRE (p.getInstanceGain() == doctest::Approx (1.0f)); // its gain as it starts

    // The first: what the plug-in held written first outside the history (a held value, no change of the song's), then the
    // song's, which came.
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.3f); });
    auto entries = told.take();
    auto gain = findTold (entries, Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK (gain->appeared);
    CHECK (gain->changesSong);
    CHECK (gain->valueToGive == std::optional (0.3f));
    CHECK (gain->isInSong);
    CHECK (gain->place == phazite::DevicePlace { .track = p.s.firstTrack });
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (0.3f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.3f));
    CHECK (p.helperHas (0.3f));

    // Its Undo: the held value, given back to the plug-in; the song holds none.
    phazite::undo (p.song);
    gain = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK (gain->went);
    CHECK (gain->valueToGive == std::optional (1.0f));
    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).has_value());
    CHECK (p.getInstanceGain() == doctest::Approx (1.0f));
    CHECK (p.helperHas (1.0f));

    // Its Redo: the step's value.
    phazite::redo (p.song);
    gain = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK (gain->appeared);
    CHECK (gain->valueToGive == std::optional (0.3f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.3f));

    // Another: in place.
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.6f); });
    gain = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK_FALSE ((gain->appeared || gain->went));
    CHECK (gain->changesSong);
    CHECK (gain->valueToGive == std::optional (0.6f));
    phazite::undo (p.song);
    CHECK (findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter)->valueToGive == std::optional (0.3f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.3f));
    phazite::redo (p.song);
    CHECK (findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter)->valueToGive == std::optional (0.6f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.6f));

    // Back to its default: the song's taken away, the value the step set left as a held value, which its Redo gives the
    // plug-in again.
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 1.0f); });
    gain = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK (gain->went);
    CHECK (gain->valueToGive == std::optional (1.0f));
    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).has_value());
    CHECK (p.getInstanceGain() == doctest::Approx (1.0f));
    phazite::undo (p.song);
    gain = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK (gain->appeared);
    CHECK (gain->valueToGive == std::optional (0.6f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.6f));
    phazite::redo (p.song);
    gain = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (gain.has_value());
    CHECK (gain->went);
    CHECK (gain->valueToGive == std::optional (1.0f));
    CHECK (p.getInstanceGain() == doctest::Approx (1.0f));
    CHECK (p.helperHas (1.0f));

    CHECK (told.others == 0);
    CHECK (told.devices == 0);
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: J.1 a, a value set in the plug-in's window survives the Undo and Redo of an unrelated step "
           "and of a step that changed another of the device's values")
{
    WithPlugin p;
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.5f); });
    p.inStep ("Turn Device Off", [&] { phazite::setDeviceOn (p.song, p.plugin, false); });
    p.inStep ("Turn Tone", [&] { phazite::setValueSetByHand (p.song, p.tone, 0.2f); });

    // The gain turned in the plug-in's own window: the song's value takes it in, with no step.
    Told told (p.song);
    TestPlugin::setGain (p.song, p.plugin, 0.8f);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));
    const auto window = findTold (told.take(), Kind::pluginValue, p.plugin, p.gain.parameter);
    REQUIRE (window.has_value());
    CHECK (window->changesSong);
    CHECK (phazite::getHistorySteps (p.song).done == juce::StringArray { "Turn Knob", "Turn Device Off", "Turn Tone" });

    // The Undo of the step that changed another of its values, and of the unrelated one: the gain stays, in the song and in
    // the plug-in.
    phazite::undo (p.song);
    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter).has_value());
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));
    phazite::undo (p.song);
    CHECK (phazite::isDeviceOn (p.song, p.plugin));
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.8f));
    phazite::redo (p.song);
    phazite::redo (p.song);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter) == std::optional (0.2f));
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).value_or (-1.0f) == doctest::Approx (0.8f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.8f));
    CHECK (p.helperHas (0.8f));
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: J.1 c, a state a save stored after a preset's step is lost by the preset's Undo, which gives "
           "the plug-in the state before it, and its Redo the preset's")
{
    WithPlugin p;
    phazite::test::TemporaryFolder temp;

    // A preset of the plug-in at a gain of 0.25; then the gain at 0.5, stored as a save stores it.
    TestPlugin::setGain (p.song, p.plugin, 0.25f);
    const auto file = temp.folder.getChildFile ("Quiet" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (p.s.context, p.plugin, file));
    TestPlugin::setGain (p.song, p.plugin, 0.5f);
    Told told (p.song);
    p.song.storeDeviceStates();
    const auto stored = findTold (told.take(), Kind::state, p.plugin);
    REQUIRE (stored.has_value());
    CHECK (stored->changesSong);
    CHECK_FALSE (p.song.getUndoManager().canUndo()); // a store is no step

    // The preset loaded: its state set in the step, given to the plug-in.
    phazite::test::UndoCheck undo (p.s.context);
    undo.check ("Load Preset Quiet", [&] { REQUIRE (phazite::loadPresetInto (p.s.context, p.plugin, file)); });
    CHECK (findTold (told.take(), Kind::state, p.plugin).has_value());
    CHECK (p.helperHas (0.25f));

    // Its knob turned in its window, and that stored: then the preset's Undo puts back the state before the preset (the 0.5
    // stored), as the tree loses a property written since in the step's Undo, and gives the plug-in that.
    TestPlugin::setGain (p.song, p.plugin, 0.75f);
    p.song.storeDeviceStates();
    phazite::undo (p.song);
    CHECK (findTold (told.take(), Kind::state, p.plugin).has_value());
    CHECK (p.helperHas (0.5f));
    phazite::redo (p.song);
    CHECK (p.helperHas (0.25f));
    undo.walkBackAndForth();
    CHECK (told.others == 0);
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a plug-in's program set in a step, told and given to it, and its Undo")
{
    WithPlugin p;
    REQUIRE (p.getInstance().getNumPrograms() == 2);
    Told told (p.song);

    p.inStep ("Program", [&] { phazite::setPluginProgramInStep (p.song, p.plugin, 1); });
    const auto program = findTold (told.take(), Kind::program, p.plugin);
    REQUIRE (program.has_value());
    CHECK (program->appeared);
    CHECK (phazite::getPluginProgram (p.song, p.plugin) == std::optional (1));
    CHECK (p.getInstance().getCurrentProgram() == 1);

    phazite::undo (p.song);
    CHECK (findTold (told.take(), Kind::program, p.plugin)->went);
    CHECK_FALSE (phazite::getPluginProgram (p.song, p.plugin).has_value());
    phazite::redo (p.song);
    CHECK (phazite::getPluginProgram (p.song, p.plugin) == std::optional (1));
    CHECK (p.getInstance().getCurrentProgram() == 1);
    CHECK (told.others == 0);
}

TEST_CASE ("Song values' device store: a chain muted and soloed with no step, removed and duplicated, each told, and its switches "
           "back with the Undo of its removal")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto container = phazite::addContainer (song, s.firstTrack);
    REQUIRE (container.isValid());
    REQUIRE (phazite::addChain (song, container).isValid());
    const auto chains = phazite::getChains (song, container);
    REQUIRE (chains.size() == 2);
    song.getUndoManager().clearUndoHistory();
    Told told (song);

    phazite::setChainMuted (song, chains[0], true);
    phazite::setChainSoloed (song, chains[1], true);
    auto entries = told.take();
    CHECK (findTold (entries, Kind::chainMute, chains[0])->appeared);
    CHECK (findTold (entries, Kind::chainSolo, chains[1])->appeared);
    CHECK (findTold (entries, Kind::chainSolo, chains[1])->place.container == container);
    CHECK_FALSE (song.getUndoManager().canUndo());

    // A copy of the soloed chain is soloed too, its switch set in the step that makes it.
    phazite::test::UndoCheck undo (s.context);
    DeviceID copy;
    undo.checkSeveral ("Duplicate Chain",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Duplicate Chain");
                           copy = phazite::duplicateChain (song, chains[1]);
                       });
    REQUIRE (copy.isValid());
    CHECK (phazite::isChainSoloed (song, copy));
    CHECK (findTold (told.take(), Kind::chainSolo, copy).has_value());

    // The soloed one taken away, then the copy: the chain left alone has its mute taken off in that step; the Undo brings
    // back each with its switch.
    undo.checkSeveral ("Remove Chains",
                       [&]
                       {
                           {
                               const phazite::UndoStep step (song, "Remove Chain");
                               phazite::removeChain (song, chains[1]);
                           }

                           const phazite::UndoStep step (song, "Remove Chain");
                           phazite::removeChain (song, copy);
                       });
    CHECK_FALSE (phazite::isChainMuted (song, chains[0]));
    CHECK (findTold (told.take(), Kind::chainMute, chains[0])->went);
    phazite::undo (song);
    CHECK (phazite::isChainMuted (song, chains[0]));
    CHECK (phazite::isChainSoloed (song, copy));
    phazite::undo (song);
    CHECK (phazite::isChainSoloed (song, chains[1]));
    phazite::redo (song);
    phazite::redo (song);
    CHECK_FALSE (phazite::isChainMuted (song, chains[0]));
    undo.walkBackAndForth();
    CHECK (told.others == 0);
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' device store: J.1 d, a chain soloed, then removed, then the Undo: soloed, and heard so; checked by "
           "UndoCheck")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto container = phazite::addContainer (song, s.firstTrack);
    REQUIRE (phazite::addChain (song, container).isValid());
    REQUIRE (phazite::addChain (song, container).isValid());
    const auto chains = phazite::getChains (song, container);
    REQUIRE (chains.size() == 3);
    phazite::test::UndoCheck undo (s.context);

    phazite::setChainSoloed (song, chains[2], true);
    CHECK_FALSE (phazite::isChainHeard (song, chains[0]));
    undo.checkSeveral ("Remove Chain",
                       [&]
                       {
                           const phazite::UndoStep step (song, "Remove Chain");
                           phazite::removeChain (song, chains[2]);
                       });
    CHECK (phazite::isChainHeard (song, chains[0]));
    phazite::undo (song);
    CHECK (phazite::isChainSoloed (song, chains[2]));
    CHECK_FALSE (phazite::isChainHeard (song, chains[0]));

    // What plays hears it so: the plan's chains, made of the store (SongPlayer: heldStore).
    auto* session = phazite::getSession (s.context);
    REQUIRE (session != nullptr);
    session->update();
    const auto* soloed = session->getPlayer().findChain ((std::int64_t) chains[2].getRaw());
    const auto* other = session->getPlayer().findChain ((std::int64_t) chains[0].getRaw());
    REQUIRE (soloed != nullptr);
    REQUIRE (other != nullptr);
    CHECK (soloed->getFader().isAudible());
    CHECK_FALSE (other->getFader().isAudible());

    // Muted with no step: heard so at once, with no plan.
    phazite::setChainMuted (song, chains[2], true);
    CHECK_FALSE (soloed->getFader().isAudible());
    phazite::redo (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' device store: a plug-in removed keeps what the store holds of it, which its Undo finds; a track "
           "duplicated copies its plug-in's entries by the copy's IDs")
{
    WithPlugin p;
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.4f); });
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&p] { return phazite::getPluginsWithOlderStates (p.song).isEmpty() && phazite::getPluginsAnswering (p.song).isEmpty(); }));
    p.song.storeDeviceStates();
    const auto state = phazite::getPluginState (p.song, p.plugin);
    REQUIRE (state.getSize() > 0);
    Told told (p.song);

    // Taken away: the store keeps its entries (nothing told of it), which no reader gives for a device the song doesn't hold.
    phazite::removeDevice (p.s.context, p.plugin);
    CHECK (told.calls == 0);
    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).has_value());
    CHECK (p.song.getValues()->deviceStore.get().states.find (p.plugin.getRaw()) != nullptr);
    phazite::test::checkSong (p.song);

    // Back by Undo: what it stored, which the plug-in opens from.
    phazite::undo (p.song);
    CHECK (told.calls == 0);
    CHECK (phazite::getPluginState (p.song, p.plugin) == state);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (0.4f));
    p.s.waitForPlugins();
    CHECK (p.getInstanceGain() == doctest::Approx (0.4f));

    // The track duplicated: the copy's plug-in holds its entries under its own ID, set in the step that copies it.
    std::vector<phazite::TrackID> copies;
    p.inStep ("Duplicate", [&] { copies = phazite::duplicateTracks (p.song, { p.s.firstTrack }, nullptr); });
    REQUIRE (copies.size() == 1);
    const auto copied = phazite::getDevices (p.song, copies.front());
    REQUIRE (copied.size() == 1);
    CHECK (copied.front() != p.plugin);
    CHECK (phazite::findPluginValue (p.song, copied.front(), p.gain.parameter) == std::optional (0.4f));
    CHECK (phazite::getPluginState (p.song, copied.front()) == state);
    const auto entries = told.take();
    CHECK (findTold (entries, Kind::state, copied.front())->appeared);
    CHECK (findTold (entries, Kind::pluginValue, copied.front(), p.gain.parameter)->appeared);
    phazite::undo (p.song);
    CHECK_FALSE (phazite::findPluginValue (p.song, copied.front(), p.gain.parameter).has_value());
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: J.1 e, an answer that comes for a plug-in a step took away meanwhile is written by its ID, "
           "and is the device's after the Undo")
{
    WithPlugin p;
    auto* session = phazite::getSession (p.s.context);
    REQUIRE (session != nullptr);
    p.song.storeDeviceStates();
    const auto before = phazite::getPluginStateBytes (p.song, p.plugin);

    // Written for the device after a step took it away (as an answer of its instance's is): outside the history, by its ID.
    phazite::removeDevice (p.s.context, p.plugin);
    juce::MemoryBlock answer ("answer", 6);
    phazite::StoredPlugin stored;
    stored.state = &answer;
    Told told (p.song);
    phazite::storePluginOutsideHistory (p.song, p.plugin, stored);
    const auto written = findTold (told.take(), Kind::state, p.plugin);
    REQUIRE (written.has_value());
    CHECK_FALSE (written->isInSong); // no device of the song's: no change of the song's, counted by no one
    CHECK_FALSE (p.song.getUndoManager().canRedo());
    phazite::test::checkSong (p.song);

    // The Undo of the removal: the answer's state, the store's entry as the answer left it.
    phazite::undo (p.song);
    CHECK (phazite::getPluginState (p.song, p.plugin) == answer);
    CHECK_FALSE (phazite::getPluginStateBytes (p.song, p.plugin).bytes == before.bytes);
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a copy's plug-in keeps a state and a value written for it outside the history after the "
           "duplicate, through the Undo of the duplicate and its Redo")
{
    // What a maker made comes into the song with its entries (a track duplicated, a preset loaded as a device, a version's
    // device): what is written for the copy later, outside the history (its state stored, a value moved in its own window),
    // is what it holds when the duplicate's Redo brings it back, as the copy's element held it when the tree held the store.
    WithPlugin p;
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.4f); });
    p.storeStates();
    const auto first = phazite::getPluginState (p.song, p.plugin);
    REQUIRE (first.getSize() > 0);

    // The track duplicated, checked by UndoCheck; then an unrelated step.
    phazite::test::UndoCheck undo (p.s.context);
    std::vector<phazite::TrackID> copies;
    undo.checkSeveral ("Duplicate",
                       [&]
                       {
                           const phazite::UndoStep step (p.song, "Duplicate");
                           copies = phazite::duplicateTracks (p.song, { p.s.firstTrack }, nullptr);
                       });
    REQUIRE (copies.size() == 1);
    const auto devices = phazite::getDevices (p.song, copies.front());
    REQUIRE (devices.size() == 1);
    const auto copy = devices.front();
    CHECK (phazite::getPluginState (p.song, copy) == first);
    p.s.waitForPlugins();
    p.inStep ("Turn Device Off", [&] { phazite::setDeviceOn (p.song, p.plugin, false); });

    // For the copy: its gain moved in its own window (the song's value takes it in), and its state stored as a save stores it.
    TestPlugin::setGain (p.song, copy, 0.7f);
    CHECK (phazite::findPluginValue (p.song, copy, p.gain.parameter).value_or (-1.0f) == doctest::Approx (0.7f));
    p.storeStates();
    const auto later = phazite::getPluginState (p.song, copy);
    REQUIRE_FALSE (later == first);

    // The two steps undone (the copy gone), and the duplicate redone: the copy back with what was written for it later, which
    // its plug-in opens from.
    phazite::undo (p.song);
    phazite::undo (p.song);
    CHECK_FALSE (phazite::findPluginValue (p.song, copy, p.gain.parameter).has_value());
    phazite::redo (p.song);
    CHECK (phazite::findPluginValue (p.song, copy, p.gain.parameter).value_or (-1.0f) == doctest::Approx (0.7f));
    CHECK (phazite::getPluginState (p.song, copy) == later);
    p.s.waitForPlugins();
    CHECK (phazite::test::dispatchMessagesUntil ([&p, copy] { return std::abs (TestPlugin::getGain (p.song, copy) - 0.7f) < 1.0e-4f; }));
    auto* instance = phazite::findPluginInstance (p.song, copy);
    REQUIRE (instance != nullptr);
    CHECK (instance->getParameterValue (instance->findParameter (p.gain.parameter)) == doctest::Approx (0.7f));
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a copied chain unsoloed after the duplicate stays unsoloed through the Undo of the "
           "duplicate and its Redo")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto container = phazite::addContainer (song, s.firstTrack);
    REQUIRE (container.isValid());
    REQUIRE (phazite::addChain (song, container).isValid());
    const auto chains = phazite::getChains (song, container);
    REQUIRE (chains.size() == 2);
    phazite::setChainSoloed (song, chains[1], true);
    song.getUndoManager().clearUndoHistory();

    DeviceID copy;

    {
        const phazite::UndoStep step (song, "Duplicate Chain");
        copy = phazite::duplicateChain (song, chains[1]);
    }

    REQUIRE (copy.isValid());
    CHECK (phazite::isChainSoloed (song, copy));

    {
        const phazite::UndoStep step (song, "Turn Device Off");
        phazite::setDeviceOn (song, container, false);
    }

    phazite::setChainSoloed (song, copy, false);
    phazite::undo (song);
    phazite::undo (song);
    CHECK (phazite::getChains (song, container).size() == 2);
    phazite::redo (song);
    REQUIRE (phazite::getChains (song, container).size() == 3);
    CHECK_FALSE (phazite::isChainSoloed (song, copy));
    CHECK (phazite::isChainSoloed (song, chains[1]));
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' device store: a preset of a plug-in added as a new device, checked by UndoCheck")
{
    WithPlugin p;
    phazite::test::TemporaryFolder temp;
    TestPlugin::setGain (p.song, p.plugin, 0.25f);
    const auto file = temp.folder.getChildFile ("Quiet" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (p.s.context, p.plugin, file));

    phazite::test::UndoCheck undo (p.s.context);
    undo.check ("Add Quiet", [&] { REQUIRE (phazite::loadPreset (p.s.context, file, p.s.firstTrack)); });
    const auto devices = phazite::getDevices (p.song, p.s.firstTrack);
    REQUIRE (devices.size() == 2);
    CHECK (phazite::getPluginState (p.song, devices.back()).getSize() > 0);
    p.s.waitForPlugins();
    CHECK (phazite::test::dispatchMessagesUntil ([&p, &devices]
                                                 { return std::abs (TestPlugin::getGain (p.song, devices.back()) - 0.25f) < 1.0e-4f; }));
    undo.walkBackAndForth();
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a version brought back in the history gives a plug-in its state, then its values set by "
           "hand, so that the song's value wins over the state's, in the step, its Undo and its Redo")
{
    WithPlugin p;

    // A state that holds a gain of 0.75, and the song's value set by hand to 0.25 since: the version.
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.75f); });
    p.storeStates();
    const auto loud = phazite::getPluginState (p.song, p.plugin);
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.25f); });
    REQUIRE (phazite::getPluginState (p.song, p.plugin) == loud);
    const auto version = phazite::test::copyWholeSong (p.song);

    // The value set to 0.5, and another state stored.
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.5f); });
    p.storeStates();
    REQUIRE_FALSE (phazite::getPluginState (p.song, p.plugin) == loud);

    // The plug-in itself (in its helper) and its instance at the value the song holds.
    const auto holdsSongsValue = [&p] (float value)
    {
        CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (value));
        CHECK (p.helperHas (value));
        CHECK (p.getInstanceGain() == doctest::Approx (value));
    };

    p.inStep ("Bring Back", [&] { phazite::makeSongLike (p.song, version); });
    CHECK (phazite::getPluginState (p.song, p.plugin) == loud);
    holdsSongsValue (0.25f);
    phazite::undo (p.song);
    holdsSongsValue (0.5f);
    phazite::redo (p.song);
    holdsSongsValue (0.25f);
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a gesture on a plug-in's control while its window reports another parameter: the gesture's "
           "Undo takes the dragged one back and keeps the report, and its Redo too")
{
    WithPlugin p;
    p.inStep ("Turn Tone", [&] { phazite::setValueSetByHand (p.song, p.tone, 0.2f); });

    auto handle = phazite::makeParameterHandle (p.song, p.gain);
    REQUIRE (handle.exists());
    handle.beginGesture();
    handle.setValue (0.3f);
    handle.setValue (0.4f);
    REQUIRE (p.helperHas (0.4f)); // the plug-in has the drag's value before its window reports another
    p.setToneInWindow (0.7f);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter).value_or (-1.0f) == doctest::Approx (0.7f));
    handle.setValue (0.5f);
    handle.endGesture();
    CHECK (phazite::getHistorySteps (p.song).done.size() == 2);

    // The gesture's Undo: the gain the song's first value set by hand, so none of the song's, the plug-in given back what it
    // held; the tone as its window reported it.
    phazite::undo (p.song);
    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).has_value());
    CHECK (p.helperHas (1.0f));
    CHECK (p.getInstanceGain() == doctest::Approx (1.0f));
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter).value_or (-1.0f) == doctest::Approx (0.7f));
    CHECK (p.getInstanceTone() == doctest::Approx (0.7f));

    // Its Redo: the gesture's last value, the tone kept.
    phazite::redo (p.song);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (0.5f));
    CHECK (p.helperHas (0.5f));
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter).value_or (-1.0f) == doctest::Approx (0.7f));
    CHECK (p.getInstanceTone() == doctest::Approx (0.7f));
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a gesture on a plug-in's control rolled back (Escape) while its window reported another "
           "parameter: the dragged one back and given to the plug-in, the report kept, nothing to redo")
{
    WithPlugin p;
    p.inStep ("Turn Tone", [&] { phazite::setValueSetByHand (p.song, p.tone, 0.2f); });

    auto handle = phazite::makeParameterHandle (p.song, p.gain);
    REQUIRE (handle.exists());
    handle.beginGesture();
    handle.setValue (0.3f);
    REQUIRE (p.helperHas (0.3f));
    p.setToneInWindow (0.7f);
    handle.setValue (0.5f);
    handle.cancelGesture();

    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter).has_value());
    CHECK (p.helperHas (1.0f));
    CHECK (p.getInstanceGain() == doctest::Approx (1.0f));
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter).value_or (-1.0f) == doctest::Approx (0.7f));
    CHECK (p.getInstanceTone() == doctest::Approx (0.7f));
    CHECK_FALSE (p.song.getUndoManager().canRedo());
    CHECK (phazite::getHistorySteps (p.song).done == juce::StringArray { "Turn Tone" });
    phazite::test::checkSong (p.song);
}

TEST_CASE ("Song values' device store: a window's place is no step and is told no one, kept for a device taken away, and in whole "
           "state")
{
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto equalizer = s.addEffect (s.firstTrack, "phaziteEq");
    song.getUndoManager().clearUndoHistory();
    Told told (song);

    phazite::setWindowPlace (song, equalizer, { .x = 30, .y = 40 });
    CHECK (told.calls == 0);
    CHECK_FALSE (song.getUndoManager().canUndo());
    const auto copy = phazite::test::copyDevice (song, equalizer);
    CHECK ((int) copy[phazite::schema::deviceWindowX] == 30);
    CHECK ((int) copy[phazite::schema::deviceWindowY] == 40);
    phazite::test::checkSong (song); // the tree holds no device, nor its window's place

    phazite::removeDevice (s.context, equalizer);
    CHECK_FALSE (phazite::getWindowPlace (song, equalizer).has_value());
    phazite::undo (song);
    CHECK (phazite::getWindowPlace (song, equalizer) == std::optional (phazite::WindowPlace { .x = 30, .y = 40 }));
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' device store: a version applied makes the store the version's, in the history and outside it, a second "
           "time writing nothing")
{
    WithPlugin p;
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.4f); });
    const auto version = phazite::test::copyWholeSong (p.song);
    p.inStep ("Turn Knob", [&] { phazite::setValueSetByHand (p.song, p.gain, 0.9f); });
    p.inStep ("Turn Tone", [&] { phazite::setValueSetByHand (p.song, p.tone, 0.1f); });
    Told told (p.song);

    {
        const phazite::UndoStep step (p.song, "Bring Back");
        phazite::makeSongLike (p.song, version);
    }

    auto entries = told.take();
    CHECK (findTold (entries, Kind::pluginValue, p.plugin, p.gain.parameter)->valueToGive == std::optional (0.4f));
    CHECK (findTold (entries, Kind::pluginValue, p.plugin, p.tone.parameter)->went);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (0.4f));
    CHECK_FALSE (phazite::findPluginValue (p.song, p.plugin, p.tone.parameter).has_value());
    CHECK (p.getInstanceGain() == doctest::Approx (0.4f));

    // Its Undo, and the same outside the history.
    phazite::undo (p.song);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (0.9f));
    CHECK (p.getInstanceGain() == doctest::Approx (0.9f));
    phazite::makeSongTheSame (p.song, version);
    CHECK (phazite::findPluginValue (p.song, p.plugin, p.gain.parameter) == std::optional (0.4f));
    told.take();
    phazite::makeSongTheSame (p.song, version);
    CHECK (told.calls == 3); // the step, its Undo and the first outside: the second wrote nothing
    phazite::test::checkSong (p.song);
}

//==============================================================================
TEST_CASE ("Song values' device store: a DEVICE that comes into the song with the store's properties and VALUEs has them taken in, "
           "and the DEVICE out of the tree")
{
    const TestPlugin testPlugin;
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    auto* values = song.getValues();
    const auto adopted = values->countDeviceStoreAdopted();

    // A plug-in's element made by hand, with a state, a program and a value, put on a track outside the history (as a track a
    // merge's choice brings in whole holds its devices).
    juce::MemoryBlock state ("state", 5);
    const auto id = DeviceID::fromRaw (song.makeID());
    juce::ValueTree device (phazite::schema::DEVICE);
    device.setProperty (phazite::schema::deviceID, (juce::int64) id.getRaw(), nullptr);
    device.setProperty (phazite::schema::deviceType, phazite::schema::deviceIsPlugin, nullptr);
    device.setProperty (phazite::schema::pluginName, "Test Effect", nullptr);
    device.setProperty (phazite::schema::pluginState, phazite::writeBytes (state), nullptr);
    device.setProperty (phazite::schema::pluginProgram, 1, nullptr);
    device.setProperty (phazite::schema::deviceWindowX, 5, nullptr);
    device.setProperty (phazite::schema::deviceWindowY, 6, nullptr);
    device.appendChild (
        juce::ValueTree (phazite::schema::VALUE, { { phazite::schema::valueParameter, "7" }, { phazite::schema::valueSetByHand, 0.5 } }),
        nullptr);

    {
        const phazite::Song::OutsideHistory outside (song);
        phazite::findTrackState (song, s.firstTrack).appendChild (device, nullptr);
    }

    CHECK (values->countDeviceStoreAdopted() == adopted + 1);
    CHECK_FALSE (phazite::holdsStoreIn (device));
    CHECK (phazite::getPluginState (song, id) == state);
    CHECK (phazite::getPluginProgram (song, id) == std::optional (1));
    CHECK (phazite::findPluginValue (song, id, "7") == std::optional (0.5f));
    CHECK (phazite::getWindowPlace (song, id) == std::optional (phazite::WindowPlace { .x = 5, .y = 6 }));

    // The DEVICE taken out of the tree as it came in (the devices' keeper's safety net, DevicesArea.h), so that nothing can be
    // added to it in the song's tree afterwards.
    CHECK_FALSE (device.getParent().isValid());

    // In whole state, written back: its one VALUE, after its other children.
    const auto copy = phazite::test::copyDevice (song, id);
    REQUIRE (copy.getNumChildren() == 1);
    CHECK (copy.getChild (0)[phazite::schema::valueParameter].toString() == "7");
    CHECK (phazite::readBytes (copy[phazite::schema::pluginState].toString()) == std::optional (state));
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' device store: the loader keeps a plug-in's and a chain's store as the song holds it, a file Phazite wrote "
           "left as it is")
{
    // As held: values sorted, after the other children; a switch written off gone; a state that isn't base 64 and an empty
    // format ID gone; a window's place as both its numbers.
    juce::ValueTree device (phazite::schema::DEVICE,
                            { { phazite::schema::deviceID, 12 }, { phazite::schema::deviceType, phazite::schema::deviceIsPlugin } });
    device.setProperty (phazite::schema::pluginState, "not base 64!", nullptr);
    device.setProperty (phazite::schema::pluginFormatID, "", nullptr);
    device.setProperty (phazite::schema::deviceWindowX, 3, nullptr);

    for (const auto* parameter : { "10", "2", "-1" })
        device.appendChild (juce::ValueTree (phazite::schema::VALUE,
                                             { { phazite::schema::valueParameter, parameter }, { phazite::schema::valueSetByHand, 0.5 } }),
                            nullptr);

    CHECK_FALSE (phazite::isStoreWrittenAsHeld (device));
    CHECK (phazite::keepStoreAsHeld (device));
    CHECK_FALSE (device.hasProperty (phazite::schema::pluginState));
    CHECK_FALSE (device.hasProperty (phazite::schema::pluginFormatID));
    CHECK ((int) device[phazite::schema::deviceWindowY] == 0);
    REQUIRE (device.getNumChildren() == 3);
    CHECK (device.getChild (0)[phazite::schema::valueParameter].toString() == "-1");
    CHECK (device.getChild (1)[phazite::schema::valueParameter].toString() == "2");
    CHECK (device.getChild (2)[phazite::schema::valueParameter].toString() == "10");
    CHECK (phazite::isStoreWrittenAsHeld (device));
    CHECK_FALSE (phazite::keepStoreAsHeld (device)); // as written: left as it is

    juce::ValueTree chain (phazite::schema::CHAIN,
                           { { phazite::schema::chainID, 13 }, { phazite::schema::chainMute, "0" }, { phazite::schema::chainSolo, "1" } });
    CHECK (phazite::keepStoreAsHeld (chain));
    CHECK_FALSE (chain.hasProperty (phazite::schema::chainMute));
    CHECK ((bool) chain[phazite::schema::chainSolo]);

    // Of one of Phazite's own devices: nothing of the store.
    juce::ValueTree own (
        phazite::schema::DEVICE,
        { { phazite::schema::deviceID, 14 }, { phazite::schema::deviceType, "phaziteEq" }, { phazite::schema::pluginState, "AAAA" } });
    CHECK (phazite::keepStoreAsHeld (own));
    CHECK_FALSE (own.hasProperty (phazite::schema::pluginState));
}
