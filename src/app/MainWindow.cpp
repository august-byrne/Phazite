// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"
#include "model/arrangement/Outlines.h"
#include "model/engine/LiveInput.h"
#include "model/project/internal/Export.h"
#include "ui/input/KeymapStore.h"
#include "ui/shell/ExportPanel.h"

#include <juce_audio_utils/juce_audio_utils.h> // PluginListComponent

#include <array>

namespace phazite
{
namespace
{
    // Interface sizes, on top of the operating system's display scaling.
    constexpr std::array<float, 8> interfaceScales { 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f };
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

MainWindow::MainWindow (te::Engine& e, ui::CommandBridge& bridge, ProjectSession& s, ui::LookAndFeel& lf, AppServices& appServices)
    : juce::DocumentWindow ("Phazite", lf.getPalette().background, juce::DocumentWindow::allButtons),
      engine (e),
      commands (bridge),
      session (s),
      lookAndFeel (lf),
      services (appServices),
      computerKeyboard ([&e] (const juce::MidiMessage& m) { playIntoAllMidiInputs (e, m); }),
      shortcutKeys (bridge)
{
    setUsingNativeTitleBar (true);
    setInterfaceScale ((float) services.settings.get (Settings::interfaceScale));

    auto content = std::make_unique<ui::AppShell> (*services.engineContext, commands, shortcutKeys, computerKeyboard, s.getContext(),
                                                   services.settings, services.soundPack, services.library, *services.previewer);
    shell = content.get();
    setContentOwned (content.release(), false);

    setResizable (true, false);
    setResizeLimits (800, 500, 16384, 16384);
    centreWithSize (1280, 800);

    setTheme (themeFromString (services.settings.get (Settings::theme)));
    setHoverHelp (services.settings.get (Settings::hoverHelp));

    computerKeyboard.setEnabled (services.settings.get (Settings::computerKeyboard));
    computerKeyboard.onChange = [this]
    {
        services.settings.set (Settings::computerKeyboard, computerKeyboard.isEnabled());
        menuItemsChanged();
    };

    setMenuBar (this);
    setApplicationCommandManagerToWatch (&commands.getCommandManager());

    // Shortcuts work anywhere in the window, and the computer keyboard gets its letters before they do.
    windowKeys = std::make_unique<ui::WindowKeys> (*this, shortcutKeys, computerKeyboard);
    commands.getCommandManager().getKeyMappings()->addChangeListener (this);

    // Play and stop, and the computer keyboard, work over plug-in windows too.
    pluginKeys = std::make_unique<ui::PluginKeys> (shortcutKeys, computerKeyboard);
    ui::setPluginKeys (pluginKeys.get());

    // The playlist's clips draw their waveforms with Tracktion's thumbnails.
    ui::setClipWaveformMaker (getClipWaveformMaker());

    session.dialogParent = this;
    session.onExport = [this] { showExport(); };
    session.onProjectClosing = [this]
    {
        delete exportWindow.getComponent(); // its export holds the Edit
    };
    session.onProjectChanged = [this]
    {
        // The views follow the project context by themselves.
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
    session.onProjectClosing = nullptr;
    session.onProjectChanged = nullptr;
    session.onExport = nullptr;
    session.dialogParent = nullptr;

    // Close the dialogs: they point into the Engine and the commands, which are destroyed after
    // this window, and an export into the project's Edit.
    delete exportWindow.getComponent();
    delete settingsWindow.getComponent();
    delete pluginListWindow.getComponent();
    delete welcomeWindow.getComponent();

    ui::setPluginKeys (nullptr);
    ui::setClipWaveformMaker ({});
    pluginKeys = nullptr;
    commands.getCommandManager().getKeyMappings()->removeChangeListener (this);
    windowKeys = nullptr;
    computerKeyboard.onChange = nullptr;
    setMenuBar (nullptr);
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit(); // asks to save unsaved changes first
}

void MainWindow::activeWindowStatusChanged()
{
    juce::DocumentWindow::activeWindowStatusChanged();

    // Keys released in another window would never reach this one.
    if (! isActiveWindow() && windowKeys != nullptr)
        windowKeys->releaseKeys();
}

bool MainWindow::keyPressed (const juce::KeyPress& key)
{
    if (windowKeys != nullptr && windowKeys->keyPressedAfterListeners (key))
        return true;

    return juce::DocumentWindow::keyPressed (key);
}

void MainWindow::addCommands()
{
    auto& registry = commands.getRegistry();
    shell->addCommands (registry);

    registry.add ({ .id = "app.quit",
                    .name = "Quit Phazite",
                    .category = "App",
                    .description = "Closes Phazite. It asks first if the project has unsaved changes.",
                    .synonyms = { "exit", "close", "leave" },
                    .perform = [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); } });

    registry.add ({ .id = "options.settings",
                    .name = "Settings",
                    .category = "Options",
                    .description = "The audio device, MIDI inputs and keyboard shortcuts.",
                    .synonyms = { "preferences", "options", "setup" },
                    .perform = [this] { showSettings (std::nullopt); } });

    registry.add ({ .id = "options.audioSettings",
                    .name = "Audio Settings",
                    .category = "Options",
                    .description = "Chooses the audio device (ASIO on Windows, for an audio interface), sample rate and buffer size.",
                    .synonyms = { "device", "asio", "latency", "buffer", "sound card", "interface", "output" },
                    .perform = [this] { showSettings (ui::SettingsPanel::audio); } });

    registry.add ({ .id = "options.midiSettings",
                    .name = "MIDI Settings",
                    .category = "Options",
                    .description = "Turns MIDI inputs and the computer keyboard on or off.",
                    .synonyms = { "midi keyboard", "controller", "inputs", "devices" },
                    .perform = [this] { showSettings (ui::SettingsPanel::midi); } });

    registry.add ({ .id = "input.computerKeyboard",
                    .name = "Play Notes with the Computer Keyboard",
                    .category = "Input",
                    .description = "A S D F G H J K L ; play the white keys, W E T Y U O P the black keys; Z and X change "
                                   "the octave, C and V the velocity. Hold Shift for single-key shortcuts.",
                    .synonyms = { "typing keyboard", "qwerty", "musical typing", "piano", "midi" },
                    .perform = [this] { computerKeyboard.setEnabled (! computerKeyboard.isEnabled()); },
                    .isTicked = [this] { return computerKeyboard.isEnabled(); } });

    registry.add (
        { .id = "options.scanPlugins",
          .name = "Scan for Plug-ins",
          .category = "Options",
          .description = "Finds your VST3 plug-ins. Scanning runs in a separate process, so a crashing plug-in can't take Phazite down.",
          .synonyms = { "vst", "vst3", "plugins", "instruments", "effects" },
          .perform = [this] { showPluginList(); } });

    registry.add ({ .id = "options.keyboardShortcuts",
                    .name = "Keyboard Shortcuts",
                    .category = "Options",
                    .description = "Changes the keys of any command. Each keymap keeps its own changes.",
                    .synonyms = { "keys", "hotkeys", "keymap", "bindings", "rebind" },
                    .perform = [this] { showSettings (ui::SettingsPanel::shortcuts); } });

    for (const auto& keymap : ui::Keymap::getBuiltIn())
    {
        const auto name = keymap.name;
        registry.add ({ .id = "options.keymap." + name.removeCharacters (" ").toLowerCase(),
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
        registry.add ({ .id = "options.theme." + juce::String (choice.name).removeCharacters (" ").toLowerCase(),
                        .name = "Theme: " + juce::String (choice.name),
                        .category = "Options",
                        .description = choice.description,
                        .synonyms = { "colours", "colors", "appearance", "dark mode", "accessibility" },
                        .perform = [this, theme] { setTheme (theme); },
                        .isTicked = [this, theme] { return lookAndFeel.getTheme() == theme; } });
    }

    registry.add ({ .id = "view.interfaceLarger",
                    .name = "Larger Interface",
                    .category = "View",
                    .description = "Makes everything in Phazite's windows bigger, up to twice the normal size.",
                    .synonyms = { "zoom in", "scale", "bigger", "text size", "accessibility" },
                    .perform = [this] { stepInterfaceScale (1); },
                    .isEnabled = [] { return juce::Desktop::getInstance().getGlobalScaleFactor() < interfaceScales.back() - 0.01f; } });

    registry.add ({ .id = "view.interfaceSmaller",
                    .name = "Smaller Interface",
                    .category = "View",
                    .description = "Makes everything in Phazite's windows smaller, down to three quarters of the normal size.",
                    .synonyms = { "zoom out", "scale", "text size" },
                    .perform = [this] { stepInterfaceScale (-1); },
                    .isEnabled = [] { return juce::Desktop::getInstance().getGlobalScaleFactor() > interfaceScales.front() + 0.01f; } });

    registry.add ({ .id = "view.interfaceNormal",
                    .name = "Normal Interface Size",
                    .category = "View",
                    .description = "Shows Phazite at its normal size again.",
                    .synonyms = { "zoom", "reset", "100%", "scale" },
                    .perform = [this] { setInterfaceScale (1.0f); } });

    registry.add ({ .id = "view.hoverHelp",
                    .name = "Hover Help",
                    .category = "View",
                    .description = "Shows what a control does when the pointer rests on it. The hint bar always shows it.",
                    .synonyms = { "tooltips", "help" },
                    .perform = [this] { setHoverHelp (tooltipWindow == nullptr); },
                    .isTicked = [this] { return tooltipWindow != nullptr; } });

    registry.add ({ .id = "help.welcome",
                    .name = "Welcome",
                    .category = "Help",
                    .description = "Opens the welcome window: the shortcuts of the app you know, the audio output with a test sound, "
                                   "and Phazite Sounds.",
                    .synonyms = { "getting started", "onboarding", "setup", "first run", "start", "coming from", "test sound" },
                    .perform = [this] { showWelcome (ui::WelcomePanel::comingFrom); } });

    registry.add (
        { .id = "help.phaziteSounds",
          .name = "Phazite Sounds",
          .category = "Help",
          .description = "Downloads Phazite Sounds, 85 free drums, loops and effects to start with, or installs them from a file.",
          .synonyms = { "samples", "sample pack", "sound pack", "library", "download", "drums", "loops", "content" },
          .perform = [this] { showWelcome (ui::WelcomePanel::sounds); } });
}

//==============================================================================
void MainWindow::showSettings (std::optional<ui::SettingsPanel::Tab> tab)
{
    if (settingsWindow != nullptr)
    {
        if (tab.has_value() && settingsPanel != nullptr)
            settingsPanel->showTab (*tab);

        settingsWindow->toFront (true);
        return;
    }

    auto* panel = new ui::SettingsPanel (*services.engineContext, commands, computerKeyboard, session.getContext(),
                                         [this] (const juce::String& name) { setKeymap (name); });
    panel->setSize (640, 600);
    panel->showTab (tab.value_or (ui::SettingsPanel::audio));
    settingsPanel = panel;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Settings";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    settingsWindow = o.launchAsync();
}

void MainWindow::showWelcome (ui::WelcomePanel::Page page)
{
    if (welcomeWindow != nullptr)
    {
        if (welcomePanel != nullptr)
            welcomePanel->showPage (page);

        welcomeWindow->toFront (true);
        return;
    }

    auto* panel = new ui::WelcomePanel (services.soundPack, engine.getDeviceManager().deviceManager, commands.getKeymap().name,
                                        [this] (const juce::String& name) { setKeymap (name); });
    panel->setSize (600, 560);
    panel->showPage (page);
    welcomePanel = panel;

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Welcome to Phazite";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    welcomeWindow = o.launchAsync();
}

void MainWindow::showExport()
{
    if (exportWindow != nullptr)
    {
        exportWindow->toFront (true);
        return;
    }

    auto* project = session.getProject();

    if (project == nullptr)
        return;

    // The song goes into the project's Renders folder (or, while it is untitled, Documents/Phazite/Exports), named after
    // the project, unless the user chooses another file.
    auto& edit = project->getEdit();
    const auto defaultFile = session.getExportFolder().getChildFile (project->getFile().getFileNameWithoutExtension() + ".wav");
    auto* panel = new ui::ExportPanel (
        services.settings, defaultFile, getExportRange (edit).getLength().inSeconds(),
        [&edit] (const ExportSettings& settings, const juce::File& file, std::function<void (const ExportReport&)> onFinished)
        { return startExport (edit, file, settings, std::move (onFinished)); });
    panel->setSize (560, 600);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Export Song";
    o.dialogBackgroundColour = lookAndFeel.getPalette().surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.content.setOwned (panel);

    exportWindow = o.launchAsync();
}

void MainWindow::showWelcomeOnFirstRun()
{
    if (services.settings.get (Settings::welcomeShown))
        return;

    services.settings.set (Settings::welcomeShown, true);

    // Once the window is on the screen.
    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<MainWindow> (this)]
        {
            if (safeThis != nullptr)
                safeThis->showWelcome (ui::WelcomePanel::comingFrom);
        });
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

void MainWindow::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Keys changed, in the shortcuts editor or with another keymap: keep them, and update the
    // tooltips and hints that show them.
    ui::saveKeySettings (commands, services.settings);
    shell->refreshShortcutTexts();
}

void MainWindow::setTheme (ui::ThemeName theme)
{
    lookAndFeel.setTheme (theme);
    setBackgroundColour (lookAndFeel.getPalette().background);

    // This window (even before it is on the screen), and every other one: panels in their own
    // windows, dialogs and tooltips.
    sendLookAndFeelChange();
    auto& desktop = juce::Desktop::getInstance();

    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* window = desktop.getComponent (i); window != nullptr && window != this)
            window->sendLookAndFeelChange();

    repaint();

    services.settings.set (Settings::theme, toString (theme));
    menuItemsChanged();
}

void MainWindow::setHoverHelp (bool on)
{
    tooltipWindow = on ? std::make_unique<juce::TooltipWindow> (nullptr, 700) : nullptr;

    services.settings.set (Settings::hoverHelp, on);
    menuItemsChanged();
}

void MainWindow::setInterfaceScale (float scale)
{
    scale = juce::jlimit (interfaceScales.front(), interfaceScales.back(), scale);
    juce::Desktop::getInstance().setGlobalScaleFactor (scale);
    services.settings.set (Settings::interfaceScale, scale);
}

void MainWindow::stepInterfaceScale (int direction)
{
    const auto current = juce::Desktop::getInstance().getGlobalScaleFactor();

    if (direction > 0)
    {
        for (const auto scale : interfaceScales)
            if (scale > current + 0.01f)
                return setInterfaceScale (scale);
    }
    else
    {
        for (auto it = interfaceScales.rbegin(); it != interfaceScales.rend(); ++it)
            if (*it < current - 0.01f)
                return setInterfaceScale (*it);
    }
}

void MainWindow::setKeymap (const juce::String& name)
{
    ui::switchKeymap (commands, name, services.settings);

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
    // As in FL Studio, Live and Bitwig: no Track, Song or Transport menu. Their commands are in Add, Edit and
    // View, as well as on the top bar, the keys, the right-click menus and the command palette.
    return { "File", "Edit", "Add", "View", "Options", "Help" };
}

juce::PopupMenu MainWindow::getMenuForIndex (int menuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    auto& commandManager = commands.getCommandManager();

    const auto addOne = [this, &commandManager] (juce::PopupMenu& m, const juce::String& id)
    {
        if (const auto commandID = commands.getCommandID (id); commandID != 0)
            m.addCommandItem (&commandManager, commandID);
    };

    const auto add = [addOne] (juce::PopupMenu& m, std::initializer_list<const char*> ids)
    {
        for (const auto* id : ids)
            addOne (m, id);
    };

    switch (menuIndex)
    {
        case 0: // File
        {
            add (menu, { "project.new", "project.open" });

            juce::PopupMenu recentMenu;
            const auto recent = session.getRecentProjects();

            for (int i = 0; i < recent.size(); ++i)
                recentMenu.addItem (firstRecentItemID + i, recent[i].getFileNameWithoutExtension());

            menu.addSubMenu ("Open Recent", recentMenu, ! recent.isEmpty());
            menu.addSeparator();
            add (menu, { "project.save", "project.saveAs" });
            menu.addSeparator();
            add (menu, { "project.export" });
            menu.addSeparator();
            add (menu, { "app.quit" });
            break;
        }

        case 1: // Edit
        {
            add (menu, { "edit.undo", "edit.redo", "edit.undoOrRedo" });
            menu.addSeparator();
            add (menu, { "edit.delete", "edit.duplicate", "edit.duplicateAsAlias", "edit.rename" });
            menu.addSeparator();
            add (menu, { "edit.selectAll", "edit.deselectAll" });
            menu.addSeparator();
            add (menu, { "edit.quantize" });

            // The piano roll's edits, which act on the selected notes.
            juce::PopupMenu notes;
            add (notes, { "note.transposeUp", "note.transposeDown", "note.octaveUp", "note.octaveDown" });
            notes.addSeparator();
            add (notes, { "note.lengthen", "note.shorten", "note.legato", "note.chop" });
            notes.addSeparator();
            add (notes, { "note.quantizeSettings", "note.legatoSettings", "note.chopSettings", "note.transposeSettings" });
            menu.addSubMenu ("Notes", notes);

            // The selected tracks' edits, which their right-click menu has too.
            juce::PopupMenu tracks;
            add (tracks, { "track.color", "track.mute", "track.solo" });
            tracks.addSeparator();
            add (tracks, { "track.groupIntoFolder", "track.ungroup" });
            tracks.addSeparator();
            add (tracks, { "track.moveUp", "track.moveDown", "track.selectPrevious", "track.selectNext" });
            menu.addSubMenu ("Tracks", tracks);

            menu.addSeparator();
            add (menu, { "edit.insertTime", "edit.removeTime" });
            break;
        }

        case 2: // Add
        {
            add (menu, { "track.add", "track.addFolder", "track.addReturn" });
            menu.addSeparator();
            add (menu, { "section.add" });

            juce::PopupMenu outlines;

            for (const auto& outline : getBuiltInOutlines())
                addOne (outlines, getOutlineCommandID (outline.id));

            menu.addSubMenu ("Song Outline", outlines);
            break;
        }

        case 3: // View
        {
            add (menu, { "view.toggleBrowser", "view.toggleInspector", "view.toggleEditor", "view.pianoRoll", "view.mixer" });
            menu.addSeparator();
            add (menu, { "view.zoomIn", "view.zoomOut", "view.zoomToFit", "view.centreOnPlayhead", "view.followPlayback" });
            add (menu, { "transport.goToStart", "transport.goToEnd" });

            // Where playback loops: the song, unless something else is looped on purpose.
            juce::PopupMenu loopMenu;
            add (loopMenu, { "transport.loop" });
            loopMenu.addSeparator();
            add (loopMenu, { "transport.loopSong", "transport.loopSelection", "section.loop" });
            menu.addSubMenu ("Loop", loopMenu);

            juce::PopupMenu gridMenu;
            add (gridMenu, { "grid.snap" });
            gridMenu.addSeparator();
            add (gridMenu, { "grid.adaptive", "grid.finer", "grid.coarser", "grid.triplets" });
            menu.addSubMenu ("Grid", gridMenu);

            menu.addSeparator();
            add (menu, { "view.focusMode", "track.pin", "track.collapse", "track.taller", "track.shorter" });
            menu.addSeparator();
            add (menu, { "view.detachBrowser", "view.detachInspector", "view.detachEditor" });
            menu.addSeparator();
            add (menu, { "view.layoutBeat", "view.layoutMix", "view.layoutFull" });
            menu.addSeparator();
            add (menu, { "view.interfaceLarger", "view.interfaceSmaller", "view.interfaceNormal" });
            break;
        }

        case 4: // Options
        {
            add (menu, { "options.settings", "options.audioSettings", "options.midiSettings", "options.keyboardShortcuts",
                         "options.scanPlugins" });
            menu.addSeparator();
            add (menu, { "input.computerKeyboard" });
            menu.addSeparator();

            juce::PopupMenu keymaps, themes;

            for (const auto* command : commands.getRegistry().getAll())
            {
                if (command->id.startsWith ("options.keymap."))
                    addOne (keymaps, command->id);
                else if (command->id.startsWith ("options.theme."))
                    addOne (themes, command->id);
            }

            menu.addSubMenu ("Keymap", keymaps);
            menu.addSubMenu ("Theme", themes);
            break;
        }

        case 5: // Help: where to find things
            add (menu, { "view.commandPalette", "options.keyboardShortcuts", "view.hoverHelp", "help.guide" });
            menu.addSeparator();
            add (menu, { "help.phaziteSounds", "help.welcome" });
            break;

        default:
            break;
    }

    return menu;
}

void MainWindow::menuItemSelected (int menuItemID, int)
{
    // The Open Recent submenu's items (the palette has "Open Recent Project" for them).
    if (const auto recent = session.getRecentProjects(); juce::isPositiveAndBelow (menuItemID - firstRecentItemID, recent.size()))
        session.openProject (recent[menuItemID - firstRecentItemID]);
}
} // namespace phazite
