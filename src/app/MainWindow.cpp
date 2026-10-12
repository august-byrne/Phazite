// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include <juce_audio_utils/juce_audio_utils.h> // AudioDeviceSelectorComponent, PluginListComponent

namespace phazite
{
namespace
{
    constexpr const char* themeSettingKey = "phaziteTheme";
    constexpr const char* hoverHelpSettingKey = "phaziteHoverHelp";
    constexpr const char* keymapSettingKey = "phaziteKeymap"; // also read by CommandRegistry::loadKeySettings
    constexpr int quitItemID = 1;
    constexpr int firstRecentItemID = 100;

    juce::String toString (ui::ThemeName theme)
    {
        switch (theme)
        {
            case ui::ThemeName::light:
                return "light";
            case ui::ThemeName::highContrast:
                return "highContrast";
            case ui::ThemeName::dark:
            default:
                return "dark";
        }
    }

    ui::ThemeName themeFromString (const juce::String& name)
    {
        if (name == "light")
            return ui::ThemeName::light;

        if (name == "highContrast")
            return ui::ThemeName::highContrast;

        return ui::ThemeName::dark;
    }
} // namespace

MainWindow::MainWindow (te::Engine& e, CommandRegistry& r, ProjectSession& s, ui::LookAndFeel& lf)
    : juce::DocumentWindow ("Phazite", lf.getPalette().background, juce::DocumentWindow::allButtons),
      engine (e),
      commands (r),
      session (s),
      lookAndFeel (lf)
{
    setUsingNativeTitleBar (true);

    auto content = std::make_unique<ui::AppShell> (engine, commands, [&s] { return s.getEdit(); });
    shell = content.get();
    setContentOwned (content.release(), false);

    setResizable (true, false);
    setResizeLimits (800, 500, 16384, 16384);
    centreWithSize (1280, 800);

    auto& settings = engine.getPropertyStorage().getPropertiesFile();
    setTheme (themeFromString (settings.getValue (themeSettingKey, "dark")));
    setHoverHelp (settings.getBoolValue (hoverHelpSettingKey, true));

    setMenuBar (this);
    setApplicationCommandManagerToWatch (&commands.getCommandManager());
    addKeyListener (commands.getCommandManager().getKeyMappings()); // shortcuts work anywhere in the window
    commands.getCommandManager().getKeyMappings()->addChangeListener (this);

    session.dialogParent = this;
    session.onProjectChanged = [this]
    {
        updateTitle();
        menuItemsChanged(); // the recent projects
    };

    updateTitle();
    startTimerHz (1); // the title shows unsaved changes
    setVisible (true);
}

MainWindow::~MainWindow()
{
    stopTimer();
    session.onProjectChanged = nullptr;
    session.dialogParent = nullptr;

    // Close the dialogs: they point into the Engine and the commands, which are destroyed after
    // this window.
    delete audioSettingsWindow.getComponent();
    delete pluginListWindow.getComponent();
    delete shortcutsWindow.getComponent();

    commands.getCommandManager().getKeyMappings()->removeChangeListener (this);
    removeKeyListener (commands.getCommandManager().getKeyMappings());
    setMenuBar (nullptr);
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit(); // asks to save unsaved changes first
}

void MainWindow::addCommands()
{
    shell->addCommands (commands);

    commands.add ({ .id = "options.audioSettings",
                    .name = "Audio Settings",
                    .category = "Options",
                    .description = "Chooses the audio device, sample rate and buffer size.",
                    .synonyms = { "device", "asio", "latency", "buffer", "sound card", "interface" },
                    .perform = [this] { showAudioSettings(); } });

    commands.add (
        { .id = "options.scanPlugins",
          .name = "Scan for Plug-ins",
          .category = "Options",
          .description = "Finds your VST3 plug-ins. Scanning runs in a separate process, so a crashing plug-in can't take Phazite down.",
          .synonyms = { "vst", "vst3", "plugins", "instruments", "effects" },
          .perform = [this] { showPluginList(); } });

    commands.add ({ .id = "options.keyboardShortcuts",
                    .name = "Keyboard Shortcuts",
                    .category = "Options",
                    .description = "Changes the keys of any command. Each keymap keeps its own changes.",
                    .synonyms = { "keys", "hotkeys", "keymap", "bindings", "rebind" },
                    .perform = [this] { showKeyboardShortcuts(); } });

    for (const auto& keymap : Keymap::getBuiltIn())
    {
        const auto name = keymap.name;
        commands.add ({ .id = "options.keymap." + name.removeCharacters (" ").toLowerCase(),
                        .name = "Keymap: " + name,
                        .category = "Options",
                        .description = keymap.description,
                        .synonyms = { "shortcuts", "keys", "keyboard", "hotkeys" },
                        .perform = [this, name] { setKeymap (name); },
                        .isTicked = [this, name] { return commands.getKeymap().name == name; } });
    }

    struct ThemeChoice
    {
        ui::ThemeName theme;
        const char* name;
        const char* description;
    };

    for (const auto& choice :
         { ThemeChoice { ui::ThemeName::dark, "Dark", "Light text on dark panels." },
           ThemeChoice { ui::ThemeName::light, "Light", "Dark text on light panels." },
           ThemeChoice { ui::ThemeName::highContrast, "High Contrast", "White and yellow on black, for the most contrast." } })
    {
        const auto theme = choice.theme;
        commands.add ({ .id = "options.theme." + juce::String (choice.name).removeCharacters (" ").toLowerCase(),
                        .name = "Theme: " + juce::String (choice.name),
                        .category = "Options",
                        .description = choice.description,
                        .synonyms = { "colours", "colors", "appearance", "dark mode", "accessibility" },
                        .perform = [this, theme] { setTheme (theme); },
                        .isTicked = [this, theme] { return lookAndFeel.getTheme() == theme; } });
    }

    commands.add ({ .id = "view.hoverHelp",
                    .name = "Hover Help",
                    .category = "View",
                    .description = "Shows what a control does when the pointer rests on it. The hint bar always shows it.",
                    .synonyms = { "tooltips", "help" },
                    .perform = [this] { setHoverHelp (tooltipWindow == nullptr); },
                    .isTicked = [this] { return tooltipWindow != nullptr; } });
}

//==============================================================================
void MainWindow::showAudioSettings()
{
    if (audioSettingsWindow != nullptr)
    {
        audioSettingsWindow->toFront (true);
        return;
    }

    // 0-512 inputs and 1-512 outputs, no MIDI options yet (roadmap step 0.7), channels in stereo
    // pairs, advanced options behind a button. The Engine saves changes in its settings.
    auto& deviceManager = engine.getDeviceManager().deviceManager;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Audio Settings";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.content.setOwned (new juce::AudioDeviceSelectorComponent (deviceManager, 0, 512, 1, 512, false, false, true, true));
    o.content->setSize (420, 560);
    o.componentToCentreAround = this;
    o.useNativeTitleBar = true;

    audioSettingsWindow = o.launchAsync();
}

void MainWindow::showPluginList()
{
    if (pluginListWindow != nullptr)
    {
        pluginListWindow->toFront (true);
        return;
    }

    auto& pm = engine.getPluginManager();

    auto* list = new juce::PluginListComponent (pm.pluginFormatManager, pm.knownPluginList,
                                                engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
                                                &engine.getPropertyStorage().getPropertiesFile(),
                                                true); // allow plug-ins that load asynchronously (AUv3)

    // One scanning thread (JUCE's default here): Tracktion's scanner talks to its one child process
    // without a lock, so a second thread would race it.
    list->setNumberOfThreadsForScanning (1);
    list->setSize (800, 600);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Plug-ins";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.useBottomRightCornerResizer = true;
    o.componentToCentreAround = this;
    o.content.setOwned (list);

    pluginListWindow = o.launchAsync();

    // Start a VST3 scan, as "Options... > Scan for new or updated VST3 plug-ins" would.
    for (auto* format : pm.pluginFormatManager.getFormats())
        if (format->getName() == "VST3")
            list->scanFor (*format);
}

void MainWindow::showKeyboardShortcuts()
{
    if (shortcutsWindow != nullptr)
    {
        shortcutsWindow->toFront (true);
        return;
    }

    // JUCE's editor for the command manager's keys, grouped by category. Its "reset to defaults"
    // button goes back to the current keymap's keys, which is what the commands register as defaults.
    auto* editor = new juce::KeyMappingEditorComponent (*commands.getCommandManager().getKeyMappings(), true);
    editor->setSize (520, 640);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Keyboard Shortcuts";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.content.setOwned (editor);

    shortcutsWindow = o.launchAsync();
}

void MainWindow::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Keys changed, in the shortcuts editor or with another keymap: keep them, and update the
    // tooltips and hints that show them.
    commands.saveKeySettings (engine.getPropertyStorage().getPropertiesFile());
    shell->refreshShortcutTexts();
}

void MainWindow::setTheme (ui::ThemeName theme)
{
    lookAndFeel.setTheme (theme);
    setBackgroundColour (lookAndFeel.getPalette().background);
    sendLookAndFeelChange();
    repaint();

    auto& settings = engine.getPropertyStorage().getPropertiesFile();
    settings.setValue (themeSettingKey, toString (theme));
    settings.saveIfNeeded();
    menuItemsChanged();
}

void MainWindow::setHoverHelp (bool on)
{
    tooltipWindow = on ? std::make_unique<juce::TooltipWindow> (nullptr, 700) : nullptr;

    auto& settings = engine.getPropertyStorage().getPropertiesFile();
    settings.setValue (hoverHelpSettingKey, on);
    settings.saveIfNeeded();
    menuItemsChanged();
}

void MainWindow::setKeymap (const juce::String& name)
{
    auto& settings = engine.getPropertyStorage().getPropertiesFile();
    commands.saveKeySettings (settings); // keeps the user's changes to the old keymap
    settings.setValue (keymapSettingKey, name);
    commands.loadKeySettings (settings); // the new keymap, with the user's changes to it
    commands.saveKeySettings (settings);

    shell->refreshShortcutTexts();
    menuItemsChanged();
}

void MainWindow::updateTitle()
{
    juce::String title ("Phazite");

    if (auto* project = session.getProject())
        title = project->getFile().getFileNameWithoutExtension() + (project->hasUnsavedChanges() ? "*" : "") + " - Phazite";

    if (getName() != title)
        setName (title);
}

//==============================================================================
juce::StringArray MainWindow::getMenuBarNames()
{
    return { "File", "Edit", "Transport", "View", "Options" };
}

juce::PopupMenu MainWindow::getMenuForIndex (int menuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    auto& commandManager = commands.getCommandManager();

    const auto add = [this, &commandManager] (juce::PopupMenu& m, const juce::String& id)
    {
        if (const auto commandID = commands.getCommandID (id); commandID != 0)
            m.addCommandItem (&commandManager, commandID);
    };

    switch (menuIndex)
    {
        case 0:
        {
            add (menu, "project.new");
            add (menu, "project.open");

            juce::PopupMenu recentMenu;
            const auto recent = session.getRecentProjects();

            for (int i = 0; i < recent.size(); ++i)
                recentMenu.addItem (firstRecentItemID + i, recent[i].getFileNameWithoutExtension());

            menu.addSubMenu ("Open Recent", recentMenu, ! recent.isEmpty());
            menu.addSeparator();
            add (menu, "project.save");
            menu.addSeparator();
            menu.addItem (quitItemID, "Quit");
            break;
        }

        case 1:
            add (menu, "edit.undo");
            add (menu, "edit.redo");
            add (menu, "edit.undoOrRedo");
            break;

        case 2:
            add (menu, "transport.playStop");
            break;

        case 3:
            add (menu, "view.commandPalette");
            menu.addSeparator();

            for (const auto* id : { "view.toggleBrowser", "view.toggleInspector", "view.toggleEditor", "view.pianoRoll", "view.mixer" })
                add (menu, id);

            menu.addSeparator();

            for (const auto* id : { "view.layoutBeat", "view.layoutMix", "view.layoutFull" })
                add (menu, id);

            menu.addSeparator();
            add (menu, "view.hoverHelp");
            break;

        case 4:
        {
            add (menu, "options.audioSettings");
            add (menu, "options.scanPlugins");
            add (menu, "options.keyboardShortcuts");
            menu.addSeparator();

            juce::PopupMenu keymaps, themes;

            for (const auto* command : commands.getAll())
            {
                if (command->id.startsWith ("options.keymap."))
                    add (keymaps, command->id);
                else if (command->id.startsWith ("options.theme."))
                    add (themes, command->id);
            }

            menu.addSubMenu ("Keymap", keymaps);
            menu.addSubMenu ("Theme", themes);
            break;
        }

        default:
            break;
    }

    return menu;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    if (menuItemID == quitItemID)
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }

    if (const auto recent = session.getRecentProjects(); juce::isPositiveAndBelow (menuItemID - firstRecentItemID, recent.size()))
        session.openProject (recent[menuItemID - firstRecentItemID]);
}
} // namespace phazite
