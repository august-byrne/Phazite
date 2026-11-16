// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window as the app makes it (src/app/MainWindow.cpp, in Main.cpp's order), on no screen: its menus, its own
// commands (themes, interface sizes, keymaps, full screen), its dialogs and the New Project window, and its close, with and
// without unsaved changes; and the devices' windows (src/app/PluginWindow.cpp). The window and its dialogs are on the
// desktop through peers with no window behind them (MainWindow::Surroundings, createHeadlessPeer()); quitting does what
// Main.cpp's systemRequestedQuit() does, without JUCE's application, which the tests have none of.
//
// On every system: the window takes the screens through MainWindow::Surroundings, and what else it reads of the system
// (its dark mode, the keyboard's layout, the transparency setting) is there without a display server, as Linux CI has
// none. On Windows also what needs a real window, as AppShellTests.cpp's detached panels do: the devices' windows, and
// Full Screen, which Windows' kiosk mode makes of the window's own style; on Linux, Full Screen fills the display JUCE
// knows of, which ScopedDisplay gives a machine without one.
#include "TestHelpers.h"

#include "GoldenFiles.h"
#include "PaintCheck.h"
#include "SandboxedPlugins.h"
#include "TestClips.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "app/AppCommands.h"
#include "app/AppServices.h"
#include "app/InputHold.h"
#include "app/MainWindow.h"
#include "app/PluginWindow.h"
#include "app/ProjectSession.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/NewProject.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "ui/devices/DeviceWindowContent.h"
#include "ui/input/Keymap.h"
#include "ui/input/KeymapStore.h"
#include "ui/shell/ExportPanel.h"
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

// The main window, put on the desktop once it is made, through a peer with no window behind it, or a real one where a
// test needs one (never shown).
class HeadlessMainWindow final : public phazite::MainWindow
{
public:
    using phazite::MainWindow::MainWindow;

    void putOnDesktop (bool onNoScreen)
    {
        headless = onNoScreen;
        addToDesktop();
    }

protected:
    juce::ComponentPeer* createNewPeer (const juce::DesktopWindowOptions& options) override
    {
        return headless ? phazite::test::createHeadlessPeer (*this, options) : phazite::MainWindow::createNewPeer (options);
    }

private:
    bool headless = true;
};

// What the app makes, in Main.cpp's order, so that it goes in the app's order (the window, then the session, the commands,
// the look and the services). The project at startup is a new one; nothing asks the user: the save dialog takes what it
// suggests, and the question about unsaved changes gives answerToSave.
struct MainScreen
{
    enum class Peer
    {
        onNoScreen,
        real // a window of the system's own, never shown
    };

    // setUp: what to do before the window is made (the settings it starts with); screens: the screens' areas it sees (none:
    // one of 1920 by 1040, so that the window is as large on every machine).
    explicit MainScreen (std::function<void (phazite::Settings&)> setUp = {}, juce::Array<juce::Rectangle<int>> screensToUse = {},
                         Peer peer = Peer::onNoScreen)
        : screens (screensToUse.isEmpty() ? juce::Array<juce::Rectangle<int>> { juce::Rectangle<int> (0, 0, 1920, 1040) }
                                          : std::move (screensToUse))
    {
        if (setUp != nullptr)
            setUp (services.settings);

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
                                                .quit = [this] { quit(); },
                                                .screens = [this] { return screens; } });
        window->putOnDesktop (peer == Peer::onNoScreen);
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

        phazite::test::runTimersUntil ([this] { return ! services.library.isIndexing(); });
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
    juce::Array<juce::Rectangle<int>> screens;
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
    // What a command may do can depend on what another thread is doing (Clean Up Unused Audio waits for a commit or a snapshot
    // being written): the menus are compared once the versions' writes are in, so that both sides read the same state.
    REQUIRE (phazite::test::waitForVersionHistory (screen.session.getContext()));
    phazite::test::waitForVersionsWrites();
    dispatchPendingMessages();

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
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.session.isUntitled(); })); // copied in the background
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
    phazite::test::dispatchPendingMessages();
    screen.addNotes();
    REQUIRE (screen.registry.invoke ("project.save")); // Clean Up Unused Audio is for a project with a folder of its own
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.session.isUntitled(); }));

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

    // Full Screen ("view.fullScreen") is switched in a case of its own, below: Windows' kiosk mode needs a real window.

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

TEST_CASE ("Full Screen fills the screen the window is on, and puts it back where it was; the place kept stays")
{
    // Known bug 150: switched in no test, as Windows' kiosk mode changes a real window's style (JUCE asserts for one on no
    // screen); Linux's fills the display JUCE knows of, which a machine without a display has none of.
#if JUCE_WINDOWS
    MainScreen screen ({}, {}, MainScreen::Peer::real);
#else
    const phazite::test::ScopedDisplay display;
    MainScreen screen;
#endif
    auto& desktop = juce::Desktop::getInstance();
    auto& window = *screen.window;
    const auto ticked = [&screen] { return screen.registry.find ("view.fullScreen")->isTicked(); };
    using phazite::Settings;

    const auto restored = window.getBounds();
    const auto place = screen.services.settings.get (Settings::windowPlace);
    const auto* onScreen = desktop.getDisplays().getDisplayForRect (window.getScreenBounds());
    REQUIRE (onScreen != nullptr);
    const auto whole = onScreen->logicalBounds.getSmallestIntegerContainer();
    REQUIRE (whole != restored);
    CHECK_FALSE (ticked());
    CHECK (desktop.getKioskModeComponent() == nullptr);

    // On: the whole screen, the menus ticked so, painted; the place kept for the next start is where it was.
    REQUIRE (screen.registry.invoke ("view.fullScreen"));
    dispatchPendingMessages();
    CHECK (ticked());
    CHECK (desktop.getKioskModeComponent() == &window);
    CHECK (window.getBounds() == whole);
    CHECK (screen.services.settings.get (Settings::windowPlace) == place);
    CHECK_FALSE (screen.services.settings.get (Settings::windowMaximized));
    int recent = 0;
    checkMenus (screen, recent);
    phazite::test::checkPaints (window.getShell(), "full screen");

    // Off: back where it was.
    REQUIRE (screen.registry.invoke ("view.fullScreen"));
    dispatchPendingMessages();
    CHECK_FALSE (ticked());
    CHECK (desktop.getKioskModeComponent() == nullptr);
    CHECK (window.getBounds() == restored);
    CHECK (screen.services.settings.get (Settings::windowPlace) == place);

    // Closed while it fills the screen: it lets go of it as it goes, and the place kept is still the one it had.
    REQUIRE (screen.registry.invoke ("view.fullScreen"));
    REQUIRE (desktop.getKioskModeComponent() == &window);
    screen.window = nullptr;
    CHECK (desktop.getKioskModeComponent() == nullptr);
    CHECK (screen.services.settings.get (Settings::windowPlace) == place);
}

TEST_CASE ("Closing the main window quits at once without unsaved changes, and asks first with them")
{
    using Answer = phazite::ProjectSession::UnsavedChanges;

    SUBCASE ("Nothing to save")
    {
        MainScreen screen;
        phazite::test::dispatchPendingMessages();
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
        CHECK (phazite::test::dispatchMessagesUntil ([&screen] { return screen.quitting; })); // once its copy is in place
        REQUIRE (screen.session.getProject() != nullptr);
        CHECK_FALSE (screen.session.isUntitled());
        CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());
        CHECK (screen.session.getProject()->getFile().existsAsFile());
    }
}

TEST_CASE ("The windows that hold a project's song close as another project opens; About stays")
{
    MainScreen screen;
    phazite::test::dispatchPendingMessages();
    screen.addNotes();
    REQUIRE (screen.registry.invoke ("project.save")); // Clean Up Unused Audio is for a project with a folder of its own
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.session.isUntitled(); }));

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

#if JUCE_WINDOWS
// Real windows, which Linux CI can't open without a display.
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

TEST_CASE ("Escape while the song exports asks before it stops; the window closed while it exports stops it and says so")
{
    // Escape closed the export's window, which threw a long export away without a word (the audit, ui-shell-browser 8).
    MainScreen screen;
    screen.addNotes();
    auto* dialog = openDialog (screen, "project.export");
    auto* panel = dynamic_cast<phazite::ui::ExportPanel*> (dialog->getContentComponent());
    REQUIRE (panel != nullptr);
    int asked = 0;
    auto stop = false;
    panel->askToStop = [&asked, &stop] (std::function<void (bool)> answer)
    {
        ++asked;
        answer (stop);
    };
    panel->askToReplace = [] (const juce::File&, std::function<void (bool)> answer) { answer (true); };

    const auto pressEscape = [] (juce::DialogWindow& window)
    {
        REQUIRE (window.getPeer() != nullptr);
        window.getPeer()->handleKeyPress (juce::KeyPress (juce::KeyPress::escapeKey));
    };

    // Exporting (nothing is dispatched, so it is still going): Escape asks, and kept, it goes on, its window open.
    panel->getExportButton().onClick();
    REQUIRE (panel->isExporting());
    pressEscape (*dialog);
    CHECK (asked == 1);
    CHECK (panel->isExporting());
    CHECK (dialog->isVisible());

    // Told to stop: stopped, and the window says so.
    stop = true;
    pressEscape (*dialog);
    CHECK (asked == 2);
    CHECK_FALSE (panel->isExporting());
    CHECK (panel->getStatus() == "Export cancelled.");
    CHECK (dialog->isVisible());

    // With nothing exporting, Escape closes the window, as any dialog's.
    juce::Component::SafePointer<juce::DialogWindow> closing (dialog);
    pressEscape (*dialog);
    dispatchPendingMessages();
    CHECK (asked == 2);
    CHECK (closing == nullptr);

    // Its window gone while it exports (as its close button, or another project opening, closes it): stopped, and a notice
    // says so.
    auto& notices = screen.window->getShell().getNotices();
    const auto before = notices.getNumNotices();
    dialog = openDialog (screen, "project.export");
    panel = dynamic_cast<phazite::ui::ExportPanel*> (dialog->getContentComponent());
    REQUIRE (panel != nullptr);
    panel->askToReplace = [] (const juce::File&, std::function<void (bool)> answer) { answer (true); };
    panel->getExportButton().onClick();
    REQUIRE (panel->isExporting());
    delete dialog;
    REQUIRE (notices.getNumNotices() == before + 1);
    CHECK (notices.getTitle (before) == "Export cancelled");
    CHECK_FALSE (notices.isProblem (before));
}

TEST_CASE ("The main window opens where it was closed, maximized if it was, and only on a screen that still exists")
{
    // Roadmap S1.15: it opened at 1280 by 800 on the main screen every time.
    using phazite::Settings;
    const juce::Array<juce::Rectangle<int>> twoScreens { juce::Rectangle<int> (0, 0, 1920, 1040),
                                                         juce::Rectangle<int> (1920, 0, 1280, 984) };
    const auto startingWith = [] (const juce::String& place, bool maximized)
    {
        return [place, maximized] (Settings& settings)
        {
            settings.set (Settings::windowPlace, place);
            settings.set (Settings::windowMaximized, maximized);
        };
    };

    juce::String place;
    auto maximized = false;

    {
        // The first time: its default size, centred on the main screen.
        MainScreen screen ({}, twoScreens);
        CHECK (screen.window->getBounds() == juce::Rectangle<int> (320, 120, 1280, 800));
        CHECK_FALSE (screen.window->isFullScreen());

        // Moved onto the second screen, and smaller: kept as it goes.
        screen.window->setBounds (2000, 40, 1000, 700);
        CHECK (screen.services.settings.get (Settings::windowPlace) == "2000 40 1000 700");

        // Maximized there, and closed so: the place it goes back to stays.
        screen.window->setFullScreen (true);
        screen.window = nullptr;
        place = screen.services.settings.get (Settings::windowPlace);
        maximized = screen.services.settings.get (Settings::windowMaximized);
        CHECK (place == "2000 40 1000 700");
        CHECK (maximized);
    }

    {
        // Opened again: maximized, going back to where it was.
        MainScreen screen (startingWith (place, maximized), twoScreens);
        CHECK (screen.window->isFullScreen());
        REQUIRE (screen.window->getPeer() != nullptr);
        CHECK (screen.window->getPeer()->getNonFullScreenBounds() == juce::Rectangle<int> (2000, 40, 1000, 700));
    }

    {
        // The second screen gone: the default place on the main one.
        MainScreen screen (startingWith (place, false), { twoScreens.getFirst() });
        CHECK (screen.window->getBounds() == juce::Rectangle<int> (320, 120, 1280, 800));
    }

    {
        // Larger than the screen it is mostly on, and partly off it: fitted to it.
        MainScreen screen (startingWith ("-50 -30 2500 1400", false), { twoScreens.getFirst() });
        CHECK (screen.window->getBounds() == twoScreens.getFirst());
    }
}

namespace
{
// A render of the test's own, which renders nothing, for the progress window to show.
struct StandInRender final : phazite::TrackRender::Impl
{
    explicit StandInRender (juce::String name)
        : title (std::move (name))
    {
    }

    float getProgress() const override { return 0.0f; }
    juce::String getTitle() const override { return title; }

    const juce::String title;
};

bool isShowing (const MainScreen& screen, const juce::String& title)
{
    for (auto* dialog : screen.getOpenDialogs())
        if (dialog->getName() == title)
            return true;

    return false;
}

// Holds every job's read where it starts until let go (known bug 123's review: a slow read).
struct ReadGate
{
    explicit ReadGate (phazite::AudioWork& w)
        : work (w)
    {
        work.setOnReadStep (
            [this] (int, const phazite::BackgroundJobs::ShouldStop& stop)
            {
                while (! letGo && ! stop())
                    juce::Thread::sleep (1);
            });
    }

    ~ReadGate() { work.setOnReadStep (nullptr); }

    phazite::AudioWork& work;
    std::atomic<bool> letGo { false };
};
} // namespace

TEST_CASE ("While audio work reads, the window takes no input: an undo, a click, a drag and the wheel change nothing and wait for "
           "nothing, and the work's step comes right after the step before it")
{
    MainScreen screen;
    auto& context = screen.session.getContext();
    auto& song = *phazite::findSong (context);
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto track = phazite::getAllPlaylistTracks (context).back();
    const auto clips =
        phazite::importFiles (context, { phazite::test::writeSound (screen.temp.folder.getChildFile ("Tone.wav"), 2.0).getFullPathName() },
                              track, phazite::test::at (0.0));
    REQUIRE (clips.size() == 1);
    dispatchPendingMessages();

    const auto stepBefore = phazite::describeUndo (&song);
    const auto songBefore = phazite::test::writeSong (song, phazite::test::SongPart::whole);
    ReadGate gate (phazite::getAudioWork (context));
    phazite::normalizeClip (context, clips[0]);
    CHECK (phazite::InputHold::isHold (juce::Component::getCurrentlyModalComponent()));

    // In the first second: Ctrl+Z, a click and a drag in the middle of the window, and the wheel there.
    auto* peer = screen.window->getPeer();
    REQUIRE (peer != nullptr);
    peer->handleKeyPress (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
    phazite::test::Mouse mouse (*screen.window);
    const auto middle = screen.window->getLocalBounds().getCentre();
    mouse.click (middle);
    mouse.press (middle);
    mouse.dragTo (middle + juce::Point<int> (80, 30));
    mouse.release();
    mouse.wheel (middle, 0.5f);
    dispatchPendingMessages();
    CHECK (phazite::describeUndo (&song) == stepBefore);
    CHECK (phazite::test::writeSong (song, phazite::test::SongPart::whole) == songBefore);

    // Let go: its step, and nothing of what was tried meanwhile after it.
    gate.letGo = true;
    phazite::test::waitForAudioWork (context);
    dispatchPendingMessages();
    CHECK (juce::Component::getCurrentlyModalComponent() == nullptr);
    CHECK (phazite::describeUndo (&song) == juce::String ("Undo Normalize Clip"));
    phazite::undo (song);
    CHECK (phazite::describeUndo (&song) == stepBefore);
    CHECK (phazite::test::writeSong (song, phazite::test::SongPart::whole) == songBefore);
}

TEST_CASE ("A render asked for while another's window shows waits its turn, whichever came first, and neither is cancelled")
{
    MainScreen screen;
    auto& context = screen.session.getContext();
    auto& work = phazite::getAudioWork (context);
    work.setShowAfter (0);
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto track = phazite::getAllPlaylistTracks (context).back();
    const auto sound = phazite::test::writeSound (screen.temp.folder.getChildFile ("Take.wav"), 2.0);

    SUBCASE ("The import's window first: a save's wait asked for meanwhile waits for it")
    {
        ReadGate gate (work);
        REQUIRE (phazite::importFilesInBackground (context, { sound.getFullPathName() }, track, phazite::test::at (0.0)));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return isShowing (screen, "Importing Take.wav"); }));

        screen.window->runRender (std::make_unique<phazite::TrackRender> (std::make_unique<StandInRender> ("Waiting for the plug-ins")));
        dispatchPendingMessages();
        CHECK (isShowing (screen, "Importing Take.wav"));
        CHECK_FALSE (isShowing (screen, "Waiting for the plug-ins"));

        gate.letGo = true;
        phazite::test::waitForAudioWork (context);
        CHECK (phazite::getClips (context, track).size() == 1); // not cancelled: it landed
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return isShowing (screen, "Waiting for the plug-ins"); }));
        screen.window->renderFinished ("Waiting for the plug-ins", juce::Result::ok());
    }

    SUBCASE ("Another's window first: the import's waits for it, and reads meanwhile")
    {
        screen.window->runRender (std::make_unique<phazite::TrackRender> (std::make_unique<StandInRender> ("Freezing Bass")));
        dispatchPendingMessages();
        REQUIRE (isShowing (screen, "Freezing Bass"));

        ReadGate gate (work);
        REQUIRE (phazite::importFilesInBackground (context, { sound.getFullPathName() }, track, phazite::test::at (0.0)));
        dispatchPendingMessages();
        CHECK (isShowing (screen, "Freezing Bass"));
        CHECK_FALSE (isShowing (screen, "Importing Take.wav"));

        screen.window->renderFinished ("Freezing Bass", juce::Result::ok());
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return isShowing (screen, "Importing Take.wav"); }));
        gate.letGo = true;
        phazite::test::waitForAudioWork (context);
        CHECK (phazite::getClips (context, track).size() == 1);
    }

    work.setShowAfter (phazite::AudioWork::showAfterMilliseconds);
}

TEST_CASE ("While audio work holds the window, the transport's keys pass, each press of them, and every other key waits for "
           "nothing")
{
    MainScreen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto track = phazite::getAllPlaylistTracks (context).back();
    const auto clips =
        phazite::importFiles (context, { phazite::test::writeSound (screen.temp.folder.getChildFile ("Tone.wav"), 2.0).getFullPathName() },
                              track, phazite::test::at (0.0));
    REQUIRE (clips.size() == 1);
    dispatchPendingMessages();

    // What the keys run.
    struct Invoked final : phazite::CommandRegistry::Listener
    {
        void commandAdded (const phazite::Command&) override {}
        void commandInvoked (const phazite::Command& command) override { ids.add (command.id); }
        juce::StringArray ids;
    } invoked;

    screen.registry.addListener (&invoked);
    auto* peer = screen.window->getPeer();
    REQUIRE (peer != nullptr);
    const juce::KeyPress space (juce::KeyPress::spaceKey);
    auto& song = *phazite::findSong (context);
    ReadGate gate (phazite::getAudioWork (context));
    const auto hold = [&]
    {
        phazite::normalizeClip (context, clips[0]);
        REQUIRE (phazite::InputHold::isHold (juce::Component::getCurrentlyModalComponent()));
    };
    const auto letGo = [&]
    {
        gate.letGo = true;
        phazite::test::waitForAudioWork (context);
        dispatchPendingMessages();
        REQUIRE (juce::Component::getCurrentlyModalComponent() == nullptr);
    };

    SUBCASE ("Space starts the song, let go and pressed again it stops it; Ctrl+Z waits for nothing")
    {
        hold();
        peer->handleKeyPress (space);
        peer->handleKeyPress (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
        CHECK (invoked.ids == juce::StringArray { "transport.playStop" });
        CHECK (phazite::isSongPlaying (song));

        peer->handleKeyUpOrDown (false);
        peer->handleKeyPress (space);
        CHECK (invoked.ids == juce::StringArray { "transport.playStop", "transport.playStop" });
        CHECK_FALSE (phazite::isSongPlaying (song));
        letGo();
    }

    SUBCASE ("Space pressed before the hold and let go during it: pressed again during it, it stops the song")
    {
        peer->handleKeyPress (space);
        REQUIRE (phazite::isSongPlaying (song));
        hold();
        peer->handleKeyUpOrDown (false);
        peer->handleKeyPress (space);
        CHECK (invoked.ids == juce::StringArray { "transport.playStop", "transport.playStop" });
        CHECK_FALSE (phazite::isSongPlaying (song));
        letGo();
    }

    SUBCASE ("Space pressed during the hold and let go after it: pressed again after it, it stops the song")
    {
        hold();
        peer->handleKeyPress (space);
        REQUIRE (phazite::isSongPlaying (song));
        letGo();
        peer->handleKeyUpOrDown (false);
        peer->handleKeyPress (space);
        CHECK (invoked.ids == juce::StringArray { "transport.playStop", "transport.playStop" });
        CHECK_FALSE (phazite::isSongPlaying (song));
    }

    screen.registry.removeListener (&invoked);

    if (phazite::isSongPlaying (song))
        REQUIRE (screen.registry.invoke ("transport.playStop"));
}

namespace
{
// A song with two branches, for a read of the versions to hold the window: main, and idea-2, the song on it.
juce::String makeBranches (phazite::ProjectContext& context)
{
    REQUIRE (phazite::test::waitForVersionHistory (context));
    REQUIRE (phazite::getLastVersion (context).has_value());
    const auto main = phazite::getCurrentBranch (context)->id;
    REQUIRE (phazite::newBranch (context, {}, [] (std::function<void (bool)> then) { then (true); }));
    return main;
}

const auto noSave = [] (std::function<void (bool)> then) { then (true); };
} // namespace

TEST_CASE ("A read of the versions holds the window with the one hold: Space plays and stops, and Escape before its window cancels it")
{
    // The last step of known bug 126: the versions' reads hold the window with the head's InputHold, as audio work does.
    MainScreen screen;
    auto& context = screen.session.getContext();
    const auto main = makeBranches (context);
    const auto idea = phazite::getCurrentBranch (context)->id;
    auto& song = *phazite::findSong (context);
    auto* peer = screen.window->getPeer();
    REQUIRE (peer != nullptr);
    const juce::KeyPress space (juce::KeyPress::spaceKey);
    phazite::VersionHistory::holdReadsForTests = true; // the read held where it starts

    REQUIRE (phazite::switchBranch (context, main, noSave));
    REQUIRE (phazite::InputHold::isHold (juce::Component::getCurrentlyModalComponent()));
    CHECK (screen.session.isHoldingInput (juce::Component::getCurrentlyModalComponent()));

    // Space plays, let go and pressed again it stops.
    peer->handleKeyPress (space);
    CHECK (phazite::isSongPlaying (song));
    peer->handleKeyUpOrDown (false);
    peer->handleKeyPress (space);
    CHECK_FALSE (phazite::isSongPlaying (song));

    // Escape, before a progress window shows: the read's Cancel, the song and the branch as they were.
    peer->handleKeyPress (juce::KeyPress (juce::KeyPress::escapeKey));
    CHECK (juce::Component::getCurrentlyModalComponent() == nullptr);
    phazite::VersionHistory::holdReadsForTests = false;
    REQUIRE (phazite::test::waitForVersionHistory (context));
    CHECK (phazite::getCurrentBranch (context)->id == idea);
}

TEST_CASE ("A read of the versions and audio work, one after the other: each window waits its turn, and a quit stops both")
{
    // The last step of known bug 126: their progress windows go through the main window's render queue, so that they never
    // stack; one call stops every kind of work that holds the window (ProjectSession::cancelWork()).
    MainScreen screen;
    auto& context = screen.session.getContext();
    const auto main = makeBranches (context);
    const auto idea = phazite::getCurrentBranch (context)->id;
    auto& work = phazite::getAudioWork (context);
    work.setShowAfter (0);
    screen.session.setWorkShownAfter (0);
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto track = phazite::getAllPlaylistTracks (context).back();
    const auto sound = phazite::test::writeSound (screen.temp.folder.getChildFile ("Take.wav"), 2.0);
    ReadGate gate (work);
    phazite::VersionHistory::holdReadsForTests = true;
    const auto readsWindow = "Reading branch 'main'";

    SUBCASE ("The import's window first: the read's waits for it")
    {
        REQUIRE (phazite::importFilesInBackground (context, { sound.getFullPathName() }, track, phazite::test::at (0.0)));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return isShowing (screen, "Importing Take.wav"); }));
        REQUIRE (phazite::switchBranch (context, main, noSave));
        dispatchPendingMessages();
        CHECK (isShowing (screen, "Importing Take.wav"));
        CHECK_FALSE (isShowing (screen, readsWindow));
    }

    SUBCASE ("The read's window first: the import's waits for it")
    {
        REQUIRE (phazite::switchBranch (context, main, noSave));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen, readsWindow] { return isShowing (screen, readsWindow); }));
        REQUIRE (phazite::importFilesInBackground (context, { sound.getFullPathName() }, track, phazite::test::at (0.0)));
        dispatchPendingMessages();
        CHECK (isShowing (screen, readsWindow));
        CHECK_FALSE (isShowing (screen, "Importing Take.wav"));
    }

    // A quit, the other waiting its turn: both stopped by the one call, nothing changed, the window free.
    screen.session.cancelWork();
    dispatchPendingMessages();
    CHECK_FALSE (isShowing (screen, readsWindow));
    CHECK_FALSE (isShowing (screen, "Importing Take.wav"));
    CHECK (juce::Component::getCurrentlyModalComponent() == nullptr);
    phazite::VersionHistory::holdReadsForTests = false;
    gate.letGo = true;
    phazite::test::waitForAudioWork (context);
    REQUIRE (phazite::test::waitForVersionHistory (context));
    CHECK (phazite::getCurrentBranch (context)->id == idea);
    CHECK (phazite::getClips (context, track).empty());
    work.setShowAfter (phazite::AudioWork::showAfterMilliseconds);
}
