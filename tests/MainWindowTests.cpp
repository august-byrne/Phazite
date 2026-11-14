// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window as the app makes it (src/app/MainWindow.cpp, in Main.cpp's order), on no screen: its menus, its own
// commands (themes, interface sizes, keymaps, full screen), its dialogs and the New Project window, and its close, with and
// without unsaved changes; and the devices' windows (src/app/PluginWindow.cpp). The window and its dialogs are on the
// desktop through peers with no window behind them (MainWindow::Surroundings, createHeadlessPeer()); quitting does what
// Main.cpp's systemRequestedQuit() does, without JUCE's application, which the tests have none of.
//
// Windows only: there the window reads its theme and the screens from the system without a display server, which Linux CI
// lacks; and the devices' windows are real ones, as AppShellTests.cpp's detached panels are.
#include "TestHelpers.h"

#if JUCE_WINDOWS
#include "GoldenFiles.h"
#include "PaintCheck.h"
#include "SandboxedPlugins.h"
#include "TestClips.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/AppCommands.h"
#include "app/AppServices.h"
#include "app/MainWindow.h"
#include "app/PluginWindow.h"
#include "app/ProjectSession.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/NewProject.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/devices/DeviceWindowContent.h"
#include "ui/input/Keymap.h"
#include "ui/input/KeymapStore.h"
#include "ui/theme/LookAndFeel.h"

#include <array>
#include <vector>

namespace
{
using phazite::test::dispatchPendingMessages;

// A dialog of the main window's (or the New Project window), as JUCE's DialogWindow::LaunchOptions::launchAsync() makes it,
// modal and deleted once it is closed, but on the desktop through a peer with no window behind it.
class HeadlessDialog final : public juce::DialogWindow
{
public:
    explicit HeadlessDialog (juce::DialogWindow::LaunchOptions& options)
        : juce::DialogWindow (options.dialogTitle, options.dialogBackgroundColour, options.escapeKeyTriggersCloseButton, false)
    {
        if (options.content.willDeleteObject())
            setContentOwned (options.content.release(), true);
        else
            setContentNonOwned (options.content.release(), true);

        setResizable (options.resizable, options.useBottomRightCornerResizer);
        setUsingNativeTitleBar (options.useNativeTitleBar);
        addToDesktop();
        setVisible (true);
        enterModalState (true, nullptr, true);
    }

    void closeButtonPressed() override { setVisible (false); } // as JUCE's own: hidden, it is dismissed, and deleted

protected:
    juce::ComponentPeer* createNewPeer (const juce::DesktopWindowOptions& options) override
    {
        return phazite::test::createHeadlessPeer (*this, options);
    }
};

// The main window, put on the desktop once it is made, through a peer with no window behind it.
class HeadlessMainWindow final : public phazite::MainWindow
{
public:
    using phazite::MainWindow::MainWindow;

    void putOnDesktop() { addToDesktop(); }

protected:
    juce::ComponentPeer* createNewPeer (const juce::DesktopWindowOptions& options) override
    {
        return phazite::test::createHeadlessPeer (*this, options);
    }
};

// What the app makes, in Main.cpp's order, so that it goes in the app's order (the window, then the session, the commands,
// the look and the services). The project at startup is a new one; nothing asks the user: the save dialog takes what it
// suggests, and the question about unsaved changes gives answerToSave.
struct MainScreen
{
    MainScreen()
    {
        session.chooseSaveFile = [] (const juce::File& suggested, std::function<void (const juce::File&)> callback)
        { callback (suggested); };
        session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (false); };
        session.askToSave =
            [this] (const juce::File&, const juce::String&, std::function<void (phazite::ProjectSession::UnsavedChanges)> answer)
        {
            ++askedToSave;
            answer (answerToSave);
        };
        session.launchDialog = [this] (juce::DialogWindow::LaunchOptions& options) { return launch (options); };

        window = std::make_unique<HeadlessMainWindow> (
            commands, session, lookAndFeel, services,
            phazite::MainWindow::Surroundings { .addToDesktop = false,
                                                .launchDialog = [this] (juce::DialogWindow::LaunchOptions& options)
                                                { return launch (options); },
                                                .quit = [this] { quit(); } });
        window->putOnDesktop();
        phazite::addSongCommands (registry, session.getContext(), *window);
        session.addCommands (registry, *window);
        window->addCommands();
        phazite::ui::loadKeySettings (commands, services.settings);
        window->getShell().refreshShortcutTexts();

        session.openAtStartup();
        dispatchPendingMessages();
        REQUIRE (session.getProject() != nullptr);
    }

    ~MainScreen()
    {
        // As Main.cpp's shutdown(): the window first; then the New Project window, which the session opened.
        window = nullptr;

        for (auto& dialog : dialogs)
            delete dialog.getComponent();

        phazite::test::runTimersUntil ([this] { return ! services.library.isIndexing(); }, 60000);
    }

    // A track with a note on it, so that there is a song to export (an empty one has nothing).
    void addNotes()
    {
        REQUIRE (registry.invoke ("track.add"));
        auto& song = *phazite::findSong (session.getContext());
        const auto clip = phazite::test::addMidiClip (song, phazite::getAllPlaylistTracks (session.getContext()).back(),
                                                      { phazite::test::at (0.0), phazite::test::at (4.0) });
        phazite::test::addMidiNote (song, clip, 60, phazite::test::at (0.0), phazite::test::lasting (1.0));
        dispatchPendingMessages();
    }

    // The dialogs open now, in the order they opened.
    juce::Array<juce::DialogWindow*> getOpenDialogs() const
    {
        juce::Array<juce::DialogWindow*> open;

        for (const auto& dialog : dialogs)
            if (dialog != nullptr && dialog->isVisible())
                open.add (dialog.getComponent());

        return open;
    }

    // What quitting does in the app (Main.cpp: systemRequestedQuit()): the session asks about unsaved changes, then Phazite
    // quits, the window going first.
    void quit()
    {
        session.closeThen (
            [this] (bool closed)
            {
                if (closed)
                    quitting = true;
            });
    }

    juce::DialogWindow* launch (juce::DialogWindow::LaunchOptions& options)
    {
        auto* dialog = new HeadlessDialog (options);
        dialogs.add (dialog);
        return dialog;
    }

    phazite::test::TemporaryFolder temp;
    phazite::ui::LookAndFeel lookAndFeel;

    // Dialogs and menus take the app's look, as Main.cpp sets it; the interface's size is the normal one again after.
    struct Globals
    {
        explicit Globals (juce::LookAndFeel& look) { juce::LookAndFeel::setDefaultLookAndFeel (&look); }

        ~Globals()
        {
            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
            juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);
        }
    } globals { lookAndFeel };

    juce::File updateFile = temp.folder.getChildFile ("Releases/phazite-update.json"); // none: a check fails, offline
    phazite::AppServices services { phazite::test::getEngineContext(),
                                    { .settingsInMemory = true,
                                      .packsFolder = temp.folder.getChildFile ("Packs"),
                                      .presetsFolder = temp.folder.getChildFile ("Presets"),
                                      .updatesFolder = temp.folder.getChildFile ("Updates"),
                                      .updateSource = { .updateFile = juce::URL (updateFile),
                                                        .installerPrefix =
                                                            juce::URL (temp.folder.getChildFile ("Releases")).toString (false) + "/",
                                                        .releasesPage = juce::URL (temp.folder.getChildFile ("Releases")) } } };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ProjectSession session { phazite::test::getEngineContext(), services.recentProjects, temp.folder.getChildFile ("Projects"),
                                      temp.folder.getChildFile ("Unsaved") };
    juce::Array<juce::Component::SafePointer<juce::DialogWindow>> dialogs;
    std::unique_ptr<HeadlessMainWindow> window;

    phazite::ProjectSession::UnsavedChanges answerToSave = phazite::ProjectSession::UnsavedChanges::cancel;
    int askedToSave = 0;
    bool quitting = false;
};

// The window's menus, as its title bar shows them.
juce::MenuBarModel& getMenus (MainScreen& screen)
{
    auto* bar = phazite::test::findAll<juce::MenuBarComponent> (screen.window->getShell()).getFirst();
    REQUIRE (bar != nullptr);
    REQUIRE (bar->getModel() != nullptr);
    return *bar->getModel();
}

// Checks every item of a menu and its submenus: a command of the app's, named, enabled and ticked as the command is now; a
// submenu, with something in it (but Open Recent while there is no recent project); or a recent project. Adds the commands
// it found to found, and counts the recent projects.
void checkMenu (const juce::PopupMenu& menu, const juce::String& path, MainScreen& screen, juce::StringArray& found, int& recent)
{
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();

        if (item.isSeparator)
            continue;

        const auto here = path + " > " + item.text;
        CAPTURE (here.toStdString());

        if (item.subMenu != nullptr)
        {
            if (item.text != "Open Recent")
                CHECK (item.subMenu->getNumItems() > 0);

            checkMenu (*item.subMenu, here, screen, found, recent);
            continue;
        }

        if (item.commandManager == nullptr)
        {
            // Open Recent's items, which the window's menuItemSelected() opens.
            CHECK (path.endsWith ("Open Recent"));
            ++recent;
            continue;
        }

        const phazite::Command* command = nullptr;

        for (const auto* each : screen.registry.getAll())
            if (screen.commands.getCommandID (each->id) == item.itemID)
                command = each;

        REQUIRE (command != nullptr);
        CAPTURE (command->id.toStdString());
        CHECK (item.text == phazite::getCurrentName (*command));
        CHECK (item.isEnabled == phazite::CommandRegistry::isEnabled (*command));
        CHECK (item.isTicked == (command->isTicked != nullptr && command->isTicked()));
        found.add (command->id);
    }
}

// Every menu, as the menu bar asks for it now; the commands found in them, in order.
juce::StringArray checkMenus (MainScreen& screen, int& recent)
{
    auto& menus = getMenus (screen);
    const auto names = menus.getMenuBarNames();
    CHECK (names == phazite::getMainMenuNames());
    juce::StringArray found;
    recent = 0;

    for (int i = 0; i < names.size(); ++i)
        checkMenu (menus.getMenuForIndex (i, names[i]), names[i], screen, found, recent);

    return found;
}

// The dialog a command opens (with none open before), checked: one, named, with its panel.
juce::DialogWindow* openDialog (MainScreen& screen, const juce::String& commandID)
{
    INFO ("opening a dialog with " << commandID);
    REQUIRE (screen.getOpenDialogs().isEmpty());
    REQUIRE (screen.registry.invoke (commandID));
    dispatchPendingMessages();

    const auto open = screen.getOpenDialogs();
    REQUIRE (open.size() == 1);
    auto* dialog = open.getFirst();
    CHECK (dialog->getName().isNotEmpty());
    REQUIRE (dialog->getContentComponent() != nullptr);
    CHECK_FALSE (dialog->getContentComponent()->getBounds().isEmpty());
    CHECK (dialog->isCurrentlyModal());
    return dialog;
}

// Closes a dialog as its close button does, and checks that it went.
void closeDialog (MainScreen& screen, juce::DialogWindow* dialog)
{
    juce::Component::SafePointer<juce::DialogWindow> closing (dialog);
    dialog->closeButtonPressed();
    dispatchPendingMessages();
    CHECK (closing == nullptr);
    CHECK (screen.getOpenDialogs().isEmpty());
}

const std::array<std::pair<const char*, phazite::ui::ThemeName>, 3> themeCommands {
    { { "options.theme.light", phazite::ui::ThemeName::light },
      { "options.theme.highcontrast", phazite::ui::ThemeName::highContrast },
      { "options.theme.dark", phazite::ui::ThemeName::dark } }
};
} // namespace

TEST_CASE ("The main window's menus name the app's commands, each shown as its command is, enabled, ticked and named")
{
    MainScreen screen;
    CHECK (screen.window->isOnDesktop());
    CHECK (screen.window->getName() == "Untitled - Phazite");

    // As it opens: a new project, and no recent one.
    int recent = 0;
    const auto found = checkMenus (screen, recent);
    CHECK (recent == 0);

    // Each menu holds what it is for: a few of each, and the window's own commands.
    for (const auto* id : { "project.new", "project.save", "app.quit", "edit.undo", "note.transposeUp", "track.add", "view.fullScreen",
                            "view.interfaceLarger", "grid.snap", "view.hoverHelp", "help.about", "help.checkForUpdates", "help.welcome" })
    {
        INFO ("command " << std::string (id));
        CHECK (found.contains (id));
    }

    // Another state: a track added and selected, hover help off, and the project saved, so that Open Recent has it.
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.hoverHelp"));
    REQUIRE (screen.registry.invoke ("project.save"));
    dispatchPendingMessages();
    CHECK_FALSE (screen.registry.find ("view.hoverHelp")->isTicked());
    checkMenus (screen, recent);
    CHECK (recent == 1);

    // Open Recent's item opens the project.
    const auto file = screen.session.getProject()->getFile();
    getMenus (screen).menuItemSelected (100, 0);
    dispatchPendingMessages();
    REQUIRE (screen.session.getProject() != nullptr);
    CHECK (screen.session.getProject()->getFile() == file);

    REQUIRE (screen.registry.invoke ("view.hoverHelp"));
    checkMenus (screen, recent);
}

TEST_CASE ("The main window's dialogs open by their commands, show their panels in each theme, and close")
{
    MainScreen screen;
    phazite::test::settle();
    screen.addNotes();
    REQUIRE (screen.registry.invoke ("project.save")); // Clean Up Unused Audio is for a project with a folder of its own

    // Each once, in each theme, then the dark one again; a second time while it is open, it stays the one. Scan for Plug-ins
    // isn't among them: it scans the machine's VST3 folders.
    for (const auto* id : { "options.settings", "project.export", "help.about", "help.checkForUpdates", "chords.progressions",
                            "project.cleanUpAudio", "help.welcome" })
    {
        INFO ("command " << std::string (id));
        auto* dialog = openDialog (screen, id);
        REQUIRE (screen.registry.invoke (id));
        dispatchPendingMessages();
        CHECK (screen.getOpenDialogs().size() == 1);

        for (const auto& [theme, name] : themeCommands)
        {
            REQUIRE (screen.registry.invoke (theme));
            dispatchPendingMessages();
            CHECK (screen.lookAndFeel.getTheme() == name);
            phazite::test::checkPaints (*dialog->getContentComponent(), dialog->getName() + " in " + theme);
        }

        closeDialog (screen, dialog);
    }

    // The settings' other tabs, and the welcome window's other page, while they are open.
    auto* settings = openDialog (screen, "options.settings");

    for (const auto* id : { "options.audioSettings", "options.midiSettings", "options.keyboardShortcuts" })
    {
        INFO ("command " << std::string (id));
        REQUIRE (screen.registry.invoke (id));
        dispatchPendingMessages();
        CHECK (screen.getOpenDialogs().size() == 1);
        phazite::test::checkPaints (*settings->getContentComponent(), id);
    }

    closeDialog (screen, settings);
    auto* sounds = openDialog (screen, "help.phaziteSounds");
    phazite::test::checkPaints (*sounds->getContentComponent(), "Phazite Sounds");
    closeDialog (screen, sounds);

    // The New Project window, last: the session opens it once the open project may close.
    screen.answerToSave = phazite::ProjectSession::UnsavedChanges::discard;
    auto* newProject = openDialog (screen, "project.new");
    CHECK (newProject->getName() == "New Project");
    phazite::test::checkPaints (*newProject->getContentComponent(), "New Project");
    closeDialog (screen, newProject);
}

TEST_CASE ("The main window's commands switch the theme, the interface size, the keymap, hover help and the computer keyboard")
{
    MainScreen screen;
    auto& desktop = juce::Desktop::getInstance();
    const auto ticked = [&screen] (const char* id) { return screen.registry.find (id)->isTicked(); };

    // Each theme, ticked alone, in the settings, and painted; then Follow System, dark or light as Windows is set.
    for (const auto& [id, theme] : themeCommands)
    {
        INFO ("command " << std::string (id));
        REQUIRE (screen.registry.invoke (id));
        dispatchPendingMessages();
        CHECK (screen.lookAndFeel.getTheme() == theme);

        for (const auto& [other, otherTheme] : themeCommands)
            CHECK (ticked (other) == (otherTheme == theme));

        CHECK_FALSE (ticked ("options.theme.followsystem"));
        phazite::test::checkPaints (screen.window->getShell(), id);
    }

    CHECK (screen.services.settings.get (phazite::Settings::theme) == "dark");
    REQUIRE (screen.registry.invoke ("options.theme.followsystem"));
    CHECK (ticked ("options.theme.followsystem"));
    CHECK (screen.lookAndFeel.getTheme() == (desktop.isDarkModeActive() ? phazite::ui::ThemeName::dark : phazite::ui::ThemeName::light));
    CHECK (screen.services.settings.get (phazite::Settings::theme) == "system");
    REQUIRE (screen.registry.invoke ("options.theme.dark"));

    // The interface sizes, larger to the largest and smaller to the smallest, each kept in the settings; painted at both.
    REQUIRE (screen.registry.invoke ("view.interfaceNormal"));
    std::vector<float> sizes;

    while (phazite::CommandRegistry::isEnabled (*screen.registry.find ("view.interfaceLarger")))
    {
        REQUIRE (screen.registry.invoke ("view.interfaceLarger"));
        sizes.push_back (desktop.getGlobalScaleFactor());
        REQUIRE (sizes.size() < 20u);
    }

    phazite::test::checkPaints (screen.window->getShell(), "the largest interface", desktop.getGlobalScaleFactor());

    while (phazite::CommandRegistry::isEnabled (*screen.registry.find ("view.interfaceSmaller")))
    {
        REQUIRE (screen.registry.invoke ("view.interfaceSmaller"));
        sizes.push_back (desktop.getGlobalScaleFactor());
        REQUIRE (sizes.size() < 20u);
    }

    phazite::test::checkPaints (screen.window->getShell(), "the smallest interface", desktop.getGlobalScaleFactor());
    const std::vector<float> expected { 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 1.75f, 1.5f, 1.25f, 1.1f, 1.0f, 0.9f, 0.75f };
    REQUIRE (sizes.size() == expected.size());

    for (size_t i = 0; i < sizes.size(); ++i)
        CHECK (sizes[i] == doctest::Approx (expected[i]));

    CHECK (screen.services.settings.get (phazite::Settings::interfaceScale) == doctest::Approx (0.75));
    REQUIRE (screen.registry.invoke ("view.interfaceNormal"));
    CHECK (desktop.getGlobalScaleFactor() == doctest::Approx (1.0f));

    // Each keymap, ticked alone; then the default again.
    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        const auto id = "options.keymap." + keymap.name.removeCharacters (" ").toLowerCase();
        CAPTURE (id.toStdString());
        REQUIRE (screen.registry.invoke (id));
        CHECK (screen.commands.getKeymap().name == keymap.name);
        CHECK (screen.registry.find (id)->isTicked());
    }

    REQUIRE (
        screen.registry.invoke ("options.keymap." + juce::String (phazite::ui::Keymap::defaultName).removeCharacters (" ").toLowerCase()));

    // Full screen ("view.fullScreen") isn't switched here: Windows' kiosk mode needs a window of its own (JUCE asserts
    // for one on no screen).

    // Hover help and the computer keyboard, off and on again.
    for (const auto* id : { "view.hoverHelp", "input.computerKeyboard" })
    {
        INFO ("command " << std::string (id));
        const auto was = ticked (id);
        REQUIRE (screen.registry.invoke (id));
        CHECK (ticked (id) != was);
        REQUIRE (screen.registry.invoke (id));
        CHECK (ticked (id) == was);
    }
}

TEST_CASE ("Closing the main window quits at once without unsaved changes, and asks first with them")
{
    using Answer = phazite::ProjectSession::UnsavedChanges;

    SUBCASE ("Nothing to save")
    {
        MainScreen screen;
        phazite::test::settle();
        REQUIRE_FALSE (screen.session.getProject()->hasUnsavedChanges());
        screen.window->closeButtonPressed();
        CHECK (screen.quitting);
        CHECK (screen.askedToSave == 0);
    }

    SUBCASE ("Unsaved changes: Cancel keeps it open, Don't Save quits, with dialogs open")
    {
        MainScreen screen;
        screen.addNotes();
        REQUIRE (screen.session.getProject()->hasUnsavedChanges());

        screen.window->closeButtonPressed();
        CHECK (screen.askedToSave == 1);
        CHECK_FALSE (screen.quitting);
        CHECK (screen.session.getProject() != nullptr);

        // The window goes with its dialogs open: an export and the progressions hold the song.
        openDialog (screen, "project.export");
        REQUIRE (screen.registry.invoke ("chords.progressions"));
        dispatchPendingMessages();
        CHECK (screen.getOpenDialogs().size() == 2);

        screen.answerToSave = Answer::discard;
        REQUIRE (screen.registry.invoke ("app.quit"));
        CHECK (screen.askedToSave == 2);
        CHECK (screen.quitting);
    }

    SUBCASE ("Unsaved changes: Save saves them first, where the save dialog says")
    {
        MainScreen screen;
        REQUIRE (screen.registry.invoke ("track.add"));
        screen.answerToSave = Answer::save;
        screen.window->closeButtonPressed();
        CHECK (screen.quitting);
        REQUIRE (screen.session.getProject() != nullptr);
        CHECK_FALSE (screen.session.isUntitled());
        CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());
        CHECK (screen.session.getProject()->getFile().existsAsFile());
    }
}

TEST_CASE ("The windows that hold a project's song close as another project opens; About stays")
{
    MainScreen screen;
    phazite::test::settle();
    screen.addNotes();
    REQUIRE (screen.registry.invoke ("project.save")); // Clean Up Unused Audio is for a project with a folder of its own

    juce::Array<juce::Component::SafePointer<juce::DialogWindow>> projects;

    for (const auto* id : { "project.export", "chords.progressions", "project.cleanUpAudio" })
    {
        REQUIRE (screen.registry.invoke (id));
        dispatchPendingMessages();
        projects.add (screen.getOpenDialogs().getLast());
    }

    REQUIRE (screen.registry.invoke ("help.about"));
    dispatchPendingMessages();
    const juce::Component::SafePointer<juce::DialogWindow> about (screen.getOpenDialogs().getLast());
    REQUIRE (screen.getOpenDialogs().size() == 4);

    // As the New Project window's Create does.
    REQUIRE (screen.session.createProject (phazite::NewProjectSettings {}).wasOk());
    dispatchPendingMessages();

    for (const auto& dialog : projects)
        CHECK (dialog == nullptr);

    CHECK (about != nullptr);
    CHECK (screen.session.getProject() != nullptr);
}

TEST_CASE ("A device's window and a plug-in's window open, show their device, and close when the device goes")
{
    phazite::test::SandboxedPlugins sandboxed;
    phazite::test::TestSong song;
    song.rename (song.firstTrack, "Bass");
    const auto eq = song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
    const auto plugin = song.addEffect (song.firstTrack, "PhaziteTestEffect");
    phazite::DeviceWindows windows (song.context);

    // As the Devices view's window buttons ask for them.
    phazite::showDeviceWindow (song.context, eq);
    phazite::showDeviceWindow (song.context, plugin);
    dispatchPendingMessages();

    auto* eqWindow = windows.findWindow (eq);
    REQUIRE (eqWindow != nullptr);
    CHECK (eqWindow->isShowing());
    CHECK (eqWindow->getName() == "EQ - Bass");
    auto* eqContent = dynamic_cast<phazite::ui::DeviceWindowContent*> (eqWindow->getContentComponent());
    REQUIRE (eqContent != nullptr);
    CHECK (eqContent->getEditor() != nullptr);
    CHECK (phazite::isDeviceWindowOpen (song.context, eq));

    auto* pluginWindow = windows.findWindow (plugin);
    REQUIRE (pluginWindow != nullptr);
    CHECK (pluginWindow->isShowing());
    CHECK (pluginWindow->getName() == phazite::getDeviceName (*phazite::findSong (song.context), plugin));
    CHECK (phazite::test::findComponentWithTitle (*pluginWindow, "Plug-in window's header") != nullptr);
    CHECK (pluginWindow->getContentComponent() != nullptr);

    // Asked again, the same windows.
    phazite::showDeviceWindow (song.context, eq);
    CHECK (windows.findWindow (eq) == eqWindow);

    // Each goes when its device does (the windows look twice a second).
    phazite::removeDevice (song.context, eq);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return windows.findWindow (eq) == nullptr; }));
    CHECK (windows.findWindow (plugin) != nullptr);
    phazite::removeDevice (song.context, plugin);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return windows.findWindow (plugin) == nullptr; }));
    CHECK_FALSE (phazite::isDeviceWindowOpen (song.context, plugin));
}
#endif
