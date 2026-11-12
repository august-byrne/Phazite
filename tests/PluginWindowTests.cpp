// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's window's menu (src/app/PluginWindow.h; roadmap step 2.12): Save Preset..., Load Preset with the presets saved
// for the plug-in, and Send All Keys to the Plug-in, which the settings keep for the plug-in in every project. Its "done
// when": plug-in states save as presets, and a name can be typed into a plug-in that draws its own text fields. And beside the
// menu, a plug-in's sidechain's key, picked there.
#include "SandboxedPlugins.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/PluginWindow.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/presets/PresetModel.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "services/Library.h"
#include "services/Settings.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/PluginKeys.h"
#include "ui/mixer/DevicesView.h"

namespace
{
using phazite::test::TestPlugin;

// A project with a plug-in on its first track, open in a context; the user's presets in a folder of their own; and the
// app's plug-in keys and plug-in windows' services, as the main window sets them.
struct Studio
{
    Studio()
    {
        plugin = TestPlugin::add (getSong(), song.firstTrack);
        REQUIRE (phazite::findPluginInstance (getSong(), plugin) != nullptr);
        getSong().getUndoManager().clearUndoHistory();

        phazite::ui::setPluginKeys (&keys);
        phazite::setPluginWindowServices (&services);
        keys.watch (window);
    }

    ~Studio()
    {
        phazite::setPluginWindowServices (nullptr);
        phazite::ui::setPluginKeys (nullptr);
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }

    juce::File presetFile (const juce::String& path) const
    {
        return library.getPresetsFolder().getChildFile (path + phazite::presetExtension);
    }

    TestPlugin testPlugin; // before the project that holds one, and gone after it
    phazite::test::TemporaryFolder temp;
    phazite::test::TestSong song;
    phazite::DeviceID plugin;
    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings, {}, temp.folder.getChildFile ("Presets") };
    phazite::PluginWindowServices services { song.context, library, settings };

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
    auto& song = studio.getSong();
    auto& context = studio.song.context;
    const auto menuBar = phazite::createPluginMenu (context, studio.plugin, studio.window);
    CHECK (menuBar->getMenuBarNames() == juce::StringArray { "Plug-in" });

    // Nothing saved for it yet: Save Preset... only.
    auto menu = menuBar->getMenuForIndex (0, "Plug-in");
    CHECK (item (menu, { "Save Preset..." }).isEnabled);
    CHECK_FALSE (item (menu, { "Load Preset" }).isEnabled);

    // Presets saved for it (as Save Preset... does, once its dialog has a file) in its folder and a folder in it; and one of
    // another device, put there by hand, which it doesn't offer.
    CHECK (phazite::getPresetFolderName (context, studio.plugin) == juce::String ("Test Effect"));
    TestPlugin::setGain (song, studio.plugin, 0.9f);
    REQUIRE (phazite::saveDevicePreset (context, studio.plugin, studio.presetFile ("Test Effect/Loud")));
    TestPlugin::setGain (song, studio.plugin, 0.3f);
    REQUIRE (phazite::saveDevicePreset (context, studio.plugin, studio.presetFile ("Test Effect/Soft/Quiet")));
    const auto eq = studio.song.addEffect (studio.song.firstTrack, "phaziteEq");
    REQUIRE (phazite::saveDevicePreset (context, eq, studio.presetFile ("Test Effect/Not This One")));
    TestPlugin::setGain (song, studio.plugin, 0.5f);
    song.getUndoManager().clearUndoHistory();

    menu = menuBar->getMenuForIndex (0, "Plug-in");
    CHECK (item (menu, { "Load Preset" }).isEnabled);
    CHECK (phazite::test::findMenuItem (menu, { "Load Preset", "Soft", "Quiet" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Load Preset", "Not This One" }) == nullptr);

    // One loaded: its settings, as one undo step named after it, which undo takes back.
    REQUIRE (phazite::test::runMenuItem (menu, { "Load Preset", "Loud" }));
    CHECK (TestPlugin::getGain (song, studio.plugin) == doctest::Approx (0.9f));
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Load Preset Loud"));
    phazite::undo (song);
    CHECK (TestPlugin::getGain (song, studio.plugin) == doctest::Approx (0.5f));

    REQUIRE (phazite::test::runMenuItem (menu, { "Load Preset", "Soft", "Quiet" }));
    CHECK (TestPlugin::getGain (song, studio.plugin) == doctest::Approx (0.3f));
}

TEST_CASE ("A plug-in window's Send All Keys to the Plug-in is kept for the plug-in in the settings, for every project")
{
    Studio studio;
    const auto menuBar = phazite::createPluginMenu (studio.song.context, studio.plugin, studio.window);
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
        phazite::test::TestSong other;
        const auto plugin = TestPlugin::add (*phazite::findSong (other.context), other.firstTrack);
        juce::Component otherEditor;
        phazite::test::TestWindow otherWindow { otherEditor, 300, 200 };
        studio.keys.watch (otherWindow);
        const auto otherMenuBar = phazite::createPluginMenu (other.context, plugin, otherWindow);
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

TEST_CASE ("A plug-in with a sidechain has its key in its window's header, by the Plug-in menu: a track chosen there is one undo step")
{
    // What a plug-in's sidechain listens to is chosen inside the plug-in, as Logic's and Cubase's plug-in windows have it, not in
    // the Devices view's list (the owner, 2026-11-10); and on its panel there, as Phazite's own devices' editors have it (below).
    phazite::test::SandboxedPlugins sandboxed;
    phazite::test::TestSong song;
    const auto tone = song.addTrack ("Tone");
    const auto listening = song.addTrack ("Listens");
    const auto effect = song.addEffect (listening, "PhaziteTestSidechain"); // plays its sidechain in place of its sound
    REQUIRE (phazite::hasSidechain (song.context, effect));
    auto& model = *phazite::findSong (song.context);
    model.getUndoManager().clearUndoHistory();

    juce::Component editor;
    phazite::test::TestWindow window { editor, 400, 300 }; // the plug-in's window
    const auto menu = phazite::createPluginMenu (song.context, effect, window);
    phazite::PluginWindowHeader header (song.context, effect, *menu);
    phazite::test::TestWindow headerWindow { header, 400, 24 };

    // At the header's right end, after the menu: the key, none at first.
    auto* picker = dynamic_cast<phazite::ui::SidechainButton*> (header.getKeyPicker());
    REQUIRE (picker != nullptr);
    CHECK (picker->isShowing());
    CHECK (picker->getTitle() == juce::String ("Sidechain"));
    CHECK (picker->getButtonText() == juce::String ("None"));
    auto* bar = phazite::test::findAll<juce::MenuBarComponent> (header).getFirst();
    REQUIRE (bar != nullptr);
    CHECK (bar->getRight() <= picker->getX());
    phazite::test::checkChildrenApart (header);

    // The tone's track: it listens, one undo step.
    REQUIRE (phazite::test::runMenuItem (picker->getMenu(), { "Tone" }));
    CHECK (phazite::getSidechainSource (song.context, effect) == tone);
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Sidechain from Tone"));
    CHECK (picker->getButtonText() == juce::String ("Tone"));

    // Undone: none again, as the window looks.
    phazite::undo (model);
    header.refresh();
    CHECK (picker->getButtonText() == juce::String ("None"));

    // A plug-in without a sidechain has its menu alone.
    const auto plain = song.addEffect (tone, "PhaziteTestEffect");
    REQUIRE_FALSE (phazite::hasSidechain (song.context, plain));
    const auto plainMenu = phazite::createPluginMenu (song.context, plain, window);
    phazite::PluginWindowHeader plainHeader (song.context, plain, *plainMenu);
    CHECK (plainHeader.getKeyPicker() == nullptr);
}

TEST_CASE ("A plug-in with a sidechain has its key on its panel in the Devices view too, so that one without a window has it")
{
    // Known bug 14 until 2026-11-11: a plug-in's key was picked in its window's header alone, which a plug-in without an editor
    // of its own never opens. Its panel beside the Devices view's column, its key knobs, has it above them, as Phazite's own
    // devices' editors there have theirs.
    phazite::test::SandboxedPlugins sandboxed;
    phazite::test::TestSong song;
    const auto tone = song.addTrack ("Tone");
    const auto listening = song.addTrack ("Listens");
    const auto effect = song.addEffect (listening, "PhaziteTestSidechain");
    REQUIRE (phazite::hasSidechain (song.context, effect));

    phazite::ui::CommandBridge commands { song.commands };
    phazite::ui::DevicesView view { commands, song.context };
    phazite::test::TestWindow window { view, 1200, 380 };
    const auto showingPicker = [&view]() -> phazite::ui::SidechainButton*
    {
        for (auto* each : phazite::test::findAll<phazite::ui::SidechainButton> (view))
            if (each->isShowing())
                return each;

        return nullptr;
    };

    view.showDevice (effect);
    phazite::test::dispatchPendingMessages();
    auto* picker = showingPicker();
    REQUIRE (picker != nullptr);
    CHECK (picker->getTitle() == juce::String ("Sidechain"));
    CHECK (picker->getButtonText() == juce::String ("None"));

    REQUIRE (phazite::test::runMenuItem (picker->getMenu(), { "Tone" }));
    CHECK (phazite::getSidechainSource (song.context, effect) == tone);

    // A plug-in that can't listen to another track has its key knobs alone.
    const auto plain = song.addEffect (tone, "PhaziteTestEffect");
    view.showDevice (plain);
    phazite::test::dispatchPendingMessages();
    CHECK (showingPicker() == nullptr);
}
