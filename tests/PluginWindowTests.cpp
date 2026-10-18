// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's window's menu (src/app/PluginWindow.h; roadmap step 2.12): Save Preset..., Load Preset with the presets saved
// for the plug-in, and Send All Keys to the Plug-in, which the settings keep for the plug-in in every project. Its "done
// when": plug-in states save as presets, and a name can be typed into a plug-in that draws its own text fields.
#include "TestPlugin.h"
#include "UiTestHelpers.h"
#include "app/PluginWindow.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "services/Library.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/PluginKeys.h"

namespace
{
using phazite::test::TestPlugin;

// A project with a plug-in on its first track, open in a context; the user's presets in a folder of their own; and the
// app's plug-in keys and plug-in windows' services, as the main window sets them.
struct Studio
{
    Studio()
    {
        juce::String error;
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Levels", error);
        REQUIRE (project != nullptr);
        auto& edit = project->getEdit();
        edit.ensureNumberOfAudioTracks (1);
        plugin = dynamic_cast<te::ExternalPlugin*> (phazite::insertDevice (*te::getAudioTracks (edit)[0], TestPlugin::create (edit)));
        REQUIRE (plugin != nullptr);
        setEdit (context, &edit);
        edit.getUndoManager().clearUndoHistory();

        phazite::ui::setPluginKeys (&keys);
        phazite::setPluginWindowServices (&services);
        keys.watch (window);
    }

    ~Studio()
    {
        phazite::setPluginWindowServices (nullptr);
        phazite::ui::setPluginKeys (nullptr);
        setEdit (context, nullptr);
    }

    juce::File presetFile (const juce::String& path) const
    {
        return library.getPresetsFolder().getChildFile (path + phazite::presetExtension);
    }

    TestPlugin testPlugin; // before the project that holds one, and gone after it
    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    te::ExternalPlugin* plugin = nullptr;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings, {}, temp.folder.getChildFile ("Presets") };
    phazite::PluginWindowServices services { context, library, settings };

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ui::ComputerKeyboard computerKeyboard { [] (const juce::MidiMessage&) {} };
    phazite::ui::PluginKeys keys { shortcutKeys, computerKeyboard };
    juce::Component editor;
    phazite::test::TestWindow window { editor, 300, 200 }; // the plug-in's window
};

const juce::PopupMenu::Item& item (const juce::PopupMenu& menu, const std::vector<juce::String>& path)
{
    const auto* found = phazite::test::findMenuItem (menu, path);
    REQUIRE (found != nullptr);
    return *found;
}
} // namespace

TEST_CASE ("A plug-in window's menu loads the presets saved for its plug-in into it, as one undo step")
{
    Studio studio;
    auto& edit = studio.project->getEdit();
    const auto menuBar = phazite::createPluginMenu (*studio.plugin, studio.window);
    CHECK (menuBar->getMenuBarNames() == juce::StringArray { "Plug-in" });

    // Nothing saved for it yet: Save Preset... only.
    auto menu = menuBar->getMenuForIndex (0, "Plug-in");
    CHECK (item (menu, { "Save Preset..." }).isEnabled);
    CHECK_FALSE (item (menu, { "Load Preset" }).isEnabled);

    // Presets saved for it (as Save Preset... does, once its dialog has a file) in its folder and a folder in it; and one of
    // another device, put there by hand, which it doesn't offer.
    CHECK (phazite::getPresetFolderName (studio.context, phazite::getDeviceID (*studio.plugin)) == juce::String ("Test Effect"));
    TestPlugin::setGain (*studio.plugin, 0.9f);
    REQUIRE (phazite::saveDevicePreset (studio.context, phazite::getDeviceID (*studio.plugin), studio.presetFile ("Test Effect/Loud")));
    TestPlugin::setGain (*studio.plugin, 0.3f);
    REQUIRE (
        phazite::saveDevicePreset (studio.context, phazite::getDeviceID (*studio.plugin), studio.presetFile ("Test Effect/Soft/Quiet")));
    const auto track = phazite::getTrackID (*te::getAudioTracks (edit)[0]);
    const auto types = phazite::getDeviceTypes (studio.context);
    const auto eq = std::find_if (types.begin(), types.end(),
                                  [] (const phazite::DeviceType& type) { return type.description.fileOrIdentifier == "phaziteEq"; });
    REQUIRE (eq != types.end());
    phazite::addEffect (studio.context, track, *eq);
    REQUIRE (phazite::saveDevicePreset (studio.context, phazite::getEffects (studio.context, track).back().id,
                                        studio.presetFile ("Test Effect/Not This One")));
    TestPlugin::setGain (*studio.plugin, 0.5f);
    edit.getUndoManager().clearUndoHistory();

    menu = menuBar->getMenuForIndex (0, "Plug-in");
    CHECK (item (menu, { "Load Preset" }).isEnabled);
    CHECK (phazite::test::findMenuItem (menu, { "Load Preset", "Soft", "Quiet" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Load Preset", "Not This One" }) == nullptr);

    // One loaded: its settings, as one undo step named after it, which undo takes back.
    REQUIRE (phazite::test::runMenuItem (menu, { "Load Preset", "Loud" }));
    CHECK (TestPlugin::getGain (*studio.plugin) == doctest::Approx (0.9f));
    CHECK (phazite::describeUndo (&edit) == juce::String ("Undo Load Preset Loud"));
    phazite::undo (edit);
    CHECK (TestPlugin::getGain (*studio.plugin) == doctest::Approx (0.5f));

    REQUIRE (phazite::test::runMenuItem (menu, { "Load Preset", "Soft", "Quiet" }));
    CHECK (TestPlugin::getGain (*studio.plugin) == doctest::Approx (0.3f));
}

TEST_CASE ("A plug-in window's Send All Keys to the Plug-in is kept for the plug-in in the settings, for every project")
{
    Studio studio;
    const auto menuBar = phazite::createPluginMenu (*studio.plugin, studio.window);
    auto menu = menuBar->getMenuForIndex (0, "Plug-in");
    CHECK (item (menu, { "Send All Keys to the Plug-in" }).isEnabled);
    CHECK_FALSE (item (menu, { "Send All Keys to the Plug-in" }).isTicked);
    CHECK_FALSE (studio.keys.sendsAllKeys (studio.window));

    // On: the window's keys all go to the plug-in, and the settings keep it for the plug-in, by its format, maker and name.
    REQUIRE (phazite::test::runMenuItem (menu, { "Send All Keys to the Plug-in" }));
    CHECK (studio.keys.sendsAllKeys (studio.window));
    CHECK (studio.settings.get (phazite::Settings::pluginsGettingAllKeys) == juce::StringArray { "Test/Phazite Tests/Test Effect" });
    CHECK (item (menuBar->getMenuForIndex (0, "Plug-in"), { "Send All Keys to the Plug-in" }).isTicked);

    // The same plug-in in another project has it on too.
    {
        auto other = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        const auto plugin = TestPlugin::create (*other);
        te::getAudioTracks (*other)[0]->pluginList.insertPlugin (plugin, 0, nullptr);
        juce::Component otherEditor;
        phazite::test::TestWindow otherWindow { otherEditor, 300, 200 };
        studio.keys.watch (otherWindow);
        const auto otherMenuBar = phazite::createPluginMenu (*plugin, otherWindow);
        const auto otherMenu = otherMenuBar->getMenuForIndex (0, "Plug-in");
        CHECK (item (otherMenu, { "Send All Keys to the Plug-in" }).isTicked);

        // Its presets are the open project's devices' only.
        CHECK_FALSE (item (otherMenu, { "Save Preset..." }).isEnabled);
        CHECK_FALSE (item (otherMenu, { "Load Preset" }).isEnabled);

        // Off there, off for both.
        REQUIRE (phazite::test::runMenuItem (otherMenu, { "Send All Keys to the Plug-in" }));
        CHECK_FALSE (studio.keys.sendsAllKeys (otherWindow));
        CHECK (studio.settings.get (phazite::Settings::pluginsGettingAllKeys).isEmpty());
        CHECK_FALSE (item (menuBar->getMenuForIndex (0, "Plug-in"), { "Send All Keys to the Plug-in" }).isTicked);
        studio.keys.unwatch (otherWindow);
    }

    // Without the main window's services (before it opens), the menu does nothing.
    phazite::setPluginWindowServices (nullptr);
    menu = menuBar->getMenuForIndex (0, "Plug-in");
    CHECK_FALSE (item (menu, { "Save Preset..." }).isEnabled);
    CHECK_FALSE (item (menu, { "Load Preset" }).isEnabled);
    CHECK_FALSE (item (menu, { "Send All Keys to the Plug-in" }).isEnabled);
}
