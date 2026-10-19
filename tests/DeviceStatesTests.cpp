// SPDX-License-Identifier: AGPL-3.0-or-later
// Devices' states stored in the project outside the undo history (src/model/devices/internal/DeviceStates.h): all that
// Tracktion's own would store, so that it finds nothing left and records no undo step, as saves, duplicates, containers
// and presets store them.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceStates.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/presets/internal/Presets.h"

namespace
{
using phazite::test::TestPlugin;

// The stand-in's gain, as its settings stored in a state hold it.
float getStoredGain (const juce::ValueTree& state)
{
    juce::MemoryBlock settings;
    REQUIRE (settings.fromBase64Encoding (state[te::IDs::state].toString()));
    juce::MemoryInputStream stream (settings, false);
    return stream.readFloat();
}

// The stand-in on a track, not in a container.
te::ExternalPlugin* findStandIn (te::Track& track)
{
    for (auto* device : track.pluginList.getPlugins())
        if (auto* plugin = dynamic_cast<te::ExternalPlugin*> (device))
            return plugin;

    return nullptr;
}

// Moves a parameter where automation would as the song plays, away from its value set by hand.
void automate (te::AutomatableParameter& parameter)
{
    const auto range = parameter.getValueRange();
    const auto value = juce::exactlyEqual (parameter.getCurrentExplicitValue(), range.getStart()) ? range.getEnd() : range.getStart();
    parameter.getCurve().addPoint (te::TimePosition(), value, 0.0f, nullptr);
    parameter.updateToFollowCurve (te::TimePosition());
    REQUIRE_FALSE (juce::exactlyEqual (parameter.getCurrentValue(), parameter.getCurrentExplicitValue()));
}

// A track with something of each kind that a store stores: the stand-in plug-in, whose gain its window turned and whose
// window moved; another in a container; and an EQ and an LFO, a parameter of each moved by automation. Nothing to undo.
struct Devices
{
    Devices()
    {
        plugin = add (TestPlugin::create (*edit));
        inside = add (TestPlugin::create (*edit));
        eq = phazite::insertDevice (track, edit->getPluginCache().createNewPlugin (phazite::devices::Equalizer::typeID, {}));
        REQUIRE (eq != nullptr);
        auto* container = phazite::groupIntoContainer ({ inside });
        REQUIRE (container != nullptr);
        rack = container->type.get();
        const auto modulator = phazite::addModulator (track, phazite::ModulatorType::lfo);
        REQUIRE (modulator.has_value());
        lfo = modulator->modifier.get();
        phazite::test::settle();
        edit->getUndoManager().clearUndoHistory();

        TestPlugin::setGain (*plugin, 0.25f);
        TestPlugin::setGain (*inside, 0.5f);
        plugin->windowState->lastWindowBounds = juce::Rectangle<int> (120, 80, 300, 200);
        automate (*eq->getAutomatableParameter (0));
        automate (*lfo->getAutomatableParameter (0));
    }

    te::ExternalPlugin* add (const te::Plugin::Ptr& device)
    {
        auto* added = dynamic_cast<te::ExternalPlugin*> (phazite::insertDevice (track, device));
        REQUIRE (added != nullptr);
        return added;
    }

    TestPlugin testPlugin; // known while the Edit is
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::AudioTrack& track = *te::getAudioTracks (*edit)[0];
    te::ExternalPlugin* plugin = nullptr; // on the track
    te::ExternalPlugin* inside = nullptr; // in a container on it
    te::RackType* rack = nullptr;         // the container's
    te::Plugin* eq = nullptr;
    te::Modifier* lfo = nullptr;
};
} // namespace

TEST_CASE ("Storing an Edit's devices' states stores all that Tracktion's save would, without an undo step")
{
    Devices devices;
    auto& edit = *devices.edit;
    phazite::storeDeviceStates (edit);
    CHECK_FALSE (edit.getUndoManager().canUndo());

    // The plug-ins' own settings and channel layout, where the window was, and the values set by hand of the parameters
    // that automation moves.
    CHECK (getStoredGain (devices.plugin->state) == doctest::Approx (0.25f));
    CHECK (getStoredGain (devices.inside->state) == doctest::Approx (0.5f));
    CHECK (devices.plugin->state.hasProperty (te::IDs::layout));
    CHECK ((int) devices.plugin->state[te::IDs::windowX] == 120);
    CHECK ((int) devices.plugin->state[te::IDs::windowY] == 80);
    CHECK (devices.eq->state.hasProperty (te::IDs::parameters));
    CHECK (devices.lfo->elementState.hasProperty (te::IDs::parameters));

    // Tracktion's own save then finds nothing more to store: it changes nothing, and records nothing.
    const auto stored = edit.state.createCopy();
    edit.flushState();
    CHECK (edit.state.isEquivalentTo (stored));
    CHECK_FALSE (edit.getUndoManager().canUndo());
}

TEST_CASE ("Storing a device's, a container's or a track's devices' states leaves Tracktion's own nothing to store")
{
    Devices devices;
    auto& undoManager = devices.edit->getUndoManager();

    phazite::storeDeviceState (*devices.plugin);
    devices.plugin->flushPluginStateToValueTree();
    CHECK_FALSE (undoManager.canUndo());

    phazite::storeDeviceStates (*devices.rack);
    devices.rack->flushStateToValueTree();
    CHECK_FALSE (undoManager.canUndo());

    TestPlugin::setGain (*devices.plugin, 0.75f);
    phazite::storeDeviceStates (devices.track);
    devices.track.flushStateToValueTree();
    CHECK_FALSE (undoManager.canUndo());
    CHECK (getStoredGain (devices.plugin->state) == doctest::Approx (0.75f));

    // Tracktion's own, by itself, stores through the undo manager, which is why these exist: if this fails, Tracktion
    // has changed, and they may not be needed any more.
    TestPlugin::setGain (*devices.plugin, 0.1f);
    devices.plugin->flushPluginStateToValueTree();
    CHECK (undoManager.canUndo());
}

TEST_CASE ("Duplicates, containers and presets take devices' states as they are, and only their own steps are undone")
{
    Devices devices;
    auto& edit = *devices.edit;
    auto& undoManager = edit.getUndoManager();
    const auto pluginID = devices.plugin->itemID;
    const auto findPlugin = [&edit, pluginID]
    {
        auto* plugin = dynamic_cast<te::ExternalPlugin*> (te::findPluginForID (edit, pluginID).get());
        REQUIRE (plugin != nullptr);
        return plugin;
    };

    // A duplicated track's copy of the plug-in plays its settings as they are now. Undo takes the copy away and leaves the
    // original's stored settings as they are, with nothing else to undo.
    TestPlugin::setGain (*findPlugin(), 0.75f);
    {
        const phazite::UndoStep step (edit, "Duplicate Track");
        const auto copies = phazite::duplicateTracks ({ &devices.track }, nullptr);
        REQUIRE (copies.size() == 1);
        auto* copied = findStandIn (*copies[0]);
        REQUIRE (copied != nullptr);
        CHECK (TestPlugin::getGain (*copied) == doctest::Approx (0.75f));
    }

    phazite::undo (edit);
    CHECK_FALSE (undoManager.canUndo());
    CHECK (te::getAudioTracks (edit).size() == 1);
    CHECK (getStoredGain (findPlugin()->state) == doctest::Approx (0.75f));

    // Grouped into a container, the plug-in keeps its settings as they are now; undo puts it back on the track.
    TestPlugin::setGain (*findPlugin(), 0.6f);
    {
        const phazite::UndoStep step (edit, "Group into Container");
        REQUIRE (phazite::groupIntoContainer ({ findPlugin() }) != nullptr);
    }

    phazite::undo (edit);
    CHECK_FALSE (undoManager.canUndo());
    CHECK (phazite::findContainerOf (*findPlugin()) == nullptr);
    CHECK (getStoredGain (findPlugin()->state) == doctest::Approx (0.6f));

    // A preset of the plug-in, or of its track, holds its settings as they are now; making one is no undo step.
    TestPlugin::setGain (*findPlugin(), 0.4f);
    const auto devicePreset = phazite::makeDevicePreset (*findPlugin());
    CHECK (getStoredGain (devicePreset.getChildWithName (te::IDs::PLUGIN)) == doctest::Approx (0.4f));
    TestPlugin::setGain (*findPlugin(), 0.3f);
    phazite::makeTrackPreset (devices.track);
    CHECK (getStoredGain (findPlugin()->state) == doctest::Approx (0.3f));
    CHECK_FALSE (undoManager.canUndo());
}
