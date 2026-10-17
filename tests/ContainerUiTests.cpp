// SPDX-License-Identifier: AGPL-3.0-or-later
// Containers and presets in the window (roadmap step 2.4), driven with the mouse as the user would: devices selected by
// Ctrl-clicking their names and grouped with the command, a container's slot with its macros (its macros and devices in
// line: DevicesViewTests.cpp), presets saved by commands or by a track dragged into the browser, and presets dropped on
// a mixer channel.
#include "UiTestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/Preview.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/modulation/ModulationModel.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "services/Library.h"
#include "ui/browser/Browser.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/LibraryDrag.h"
#include "ui/mixer/ChannelStrip.h"

namespace
{
using phazite::DeviceID;
using phazite::TrackID;

// A project in a folder of its own, with a track holding an EQ and a compressor, and the commands that group devices.
struct Song
{
    Song()
    {
        juce::String error;
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Song", error);
        REQUIRE (project != nullptr);
        edit().ensureNumberOfAudioTracks (2);
        setEdit (context, &edit());

        for (const auto* type : { "phaziteEq", "phaziteCompressor" })
            phazite::addEffect (context, track(), find (type));

        phazite::addContainerCommands (registry, getSelection (context));
        edit().getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    te::Edit& edit() const { return project->getEdit(); }
    TrackID track (int index = 0) const { return phazite::getTrackID (*te::getAudioTracks (edit())[index]); }

    phazite::DeviceType find (const char* type) const
    {
        for (const auto& each : phazite::getDeviceTypes (context))
            if (each.description.fileOrIdentifier == juce::String (type))
                return each;

        FAIL ("no such device");
        return {};
    }

    std::vector<DeviceID> effects() const
    {
        std::vector<DeviceID> ids;

        for (const auto& effect : phazite::getEffects (context, track()))
            ids.push_back (effect.id);

        return ids;
    }

    juce::String describeUndo() const { return phazite::describeUndo (&edit()); }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
};

// A drop of a modulator, as JUCE delivers one.
juce::DragAndDropTarget::SourceDetails dragOf (const juce::var& description, juce::Component& source)
{
    return { description, &source, {} };
}
} // namespace

TEST_CASE ("Ctrl-clicking devices' names selects them, Group into Container groups them, and its slot shows its macros")
{
    Song song;
    phazite::ui::ChannelStrip strip { song.commands, song.context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 900 };
    strip.setTrack (song.track());

    auto* eq = phazite::test::findComponentWithTitle (strip, "EQ");
    auto* compressor = phazite::test::findComponentWithTitle (strip, "Compressor");
    REQUIRE (eq != nullptr);
    REQUIRE (compressor != nullptr);

    // A Ctrl-click selects a device (a click shows it in the Devices view); another adds to the selection.
    phazite::test::Mouse eqMouse (*eq), compressorMouse (*compressor);
    eqMouse.click (eq->getLocalBounds().getCentre(), juce::ModifierKeys::commandModifier);
    compressorMouse.click (compressor->getLocalBounds().getCentre(), juce::ModifierKeys::commandModifier);
    CHECK (song.context.getSongSelection().getDevices() == song.effects());

    // Ctrl-clicked again, it leaves the selection.
    eqMouse.click (eq->getLocalBounds().getCentre(), juce::ModifierKeys::commandModifier);
    CHECK (song.context.getSongSelection().getDevices() == std::vector<DeviceID> { song.effects()[1] });
    eqMouse.click (eq->getLocalBounds().getCentre(), juce::ModifierKeys::commandModifier);

    REQUIRE (song.registry.invoke ("device.group"));
    CHECK (song.describeUndo() == juce::String ("Undo Group into Container"));

    // The strip shows the container, with its mix and its first macros as knobs.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return phazite::test::findComponentWithTitle (strip, "Container") != nullptr; }));
    CHECK (phazite::test::findComponentWithTitle (strip, "EQ") == nullptr);
    CHECK (phazite::test::findComponentWithTitle (strip, "Macro 1") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (strip, "Macro 2") != nullptr);
    CHECK (phazite::test::findComponentWithTitle (strip, "Mix") != nullptr);
    phazite::test::checkChildrenApart (strip);
}

TEST_CASE ("Saving presets: the commands for the selected device and track, and a track dragged into the browser")
{
    Song song;
    phazite::Settings settings;
    phazite::SoundPack soundPack { song.temp.folder.getChildFile ("Packs") };
    phazite::Library library { phazite::test::getEngineContext(), settings, {}, song.temp.folder.getChildFile ("Presets") };
    auto previewer = phazite::createPreviewer (phazite::test::getEngine());
    phazite::ui::Browser browser { library, *previewer, soundPack, settings, song.context };
    phazite::test::TestWindow window { browser, 320, 700 };
    phazite::CommandRegistry registry;
    browser.addCommands (registry);

    // Enabled for one selected device, and one selected track that isn't a return or a folder.
    const auto* devicePreset = registry.find ("device.savePreset");
    const auto* trackPreset = registry.find ("track.savePreset");
    REQUIRE (devicePreset != nullptr);
    REQUIRE (trackPreset != nullptr);
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*devicePreset));
    song.context.getSongSelection().selectOnly (song.effects()[0]);
    CHECK (phazite::CommandRegistry::isEnabled (*devicePreset));
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*trackPreset));
    song.context.getSongSelection().selectOnly (song.track());
    CHECK (phazite::CommandRegistry::isEnabled (*trackPreset));
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*devicePreset));
    const auto returnTrack = phazite::addReturnWith (song.context, song.track(), song.find ("phaziteReverb"));
    song.context.getSongSelection().selectOnly (returnTrack);
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*trackPreset));

    // A track dragged in: saved in Tracks, named after it, a second time with a number; the browser shows them.
    juce::Component source;
    const auto drag = dragOf (phazite::ui::makeTrackDrag (song.track()), source);
    REQUIRE (browser.isInterestedInDragSource (drag));
    CHECK_FALSE (browser.isInterestedInDragSource (dragOf (phazite::ui::makeModulatorDrag (phazite::ModulatorID::fromRaw (1)), source)));
    browser.itemDropped (drag);
    browser.itemDropped (drag);

    const auto tracks = library.getPresetsFolder().getChildFile (phazite::trackPresetsFolder);
    const auto files = tracks.findChildFiles (juce::File::findFiles, false, "*" + juce::String (phazite::presetExtension));
    CHECK (files.size() == 2);
    const auto name = phazite::getChannel (song.context, song.track())->name;
    const auto info = phazite::readPresetInfo (tracks.getChildFile (name + phazite::presetExtension));
    REQUIRE (info.has_value());
    CHECK (info->kind == phazite::PresetKind::track);

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            auto& tree = browser.getTree();

            for (int i = 0; i < tree.getNumRowsInTree(); ++i)
                if (tree.getItemOnRow (i)->getAccessibilityName() == name + ", Track")
                    return true;

            return false;
        }));
}

TEST_CASE ("A preset dropped on a mixer channel goes onto it, and a track's makes a track after it")
{
    Song song;
    const auto file = song.temp.folder.getChildFile ("Squash" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveDevicePreset (song.context, song.effects()[1], file));
    const auto trackFile = song.temp.folder.getChildFile ("Lead" + juce::String (phazite::presetExtension));
    REQUIRE (phazite::saveTrackPreset (song.context, song.track(), trackFile));

    phazite::ui::ChannelStrip strip { song.commands, song.context };
    phazite::test::TestWindow window { strip, phazite::ui::ChannelStrip::width, 900 };
    strip.setTrack (song.track (1));

    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings };
    juce::Component source;
    const auto drag = dragOf (phazite::ui::toDragDescription (library.makeFileItem (file, file.getParentDirectory())), source);
    REQUIRE (strip.isInterestedInDragSource (drag));
    strip.itemDropped (drag);
    CHECK (song.describeUndo() == juce::String ("Undo Add Squash"));
    REQUIRE (phazite::getEffects (song.context, song.track (1)).size() == 1);
    CHECK (phazite::getEffects (song.context, song.track (1))[0].name == juce::String ("Compressor"));
    CHECK (phazite::ui::wasPlaced (drag.description));

    strip.itemDropped (dragOf (phazite::ui::toDragDescription (library.makeFileItem (trackFile, trackFile.getParentDirectory())), source));
    CHECK (song.describeUndo() == juce::String ("Undo Add Lead"));
    CHECK (te::getAudioTracks (song.edit()).size() == 3);
}
