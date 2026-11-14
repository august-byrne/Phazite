// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "AppServices.h"
#include "ClipWaveforms.h"
#include "PluginWindow.h"
#include "ProjectSession.h"
#include "model/mixer/FreezeModel.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/PluginKeys.h"
#include "ui/input/WindowKeys.h"
#include "ui/shell/AppShell.h"
#include "ui/shell/SettingsPanel.h"
#include "ui/shell/Welcome.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <optional>

namespace phazite
{
// Phazite's one window: the AppShell, whose title bar shows this window's menus. It also owns the dialogs, the devices'
// windows, and the computer keyboard, which plays notes while this window has the keyboard focus. Freezes and bounces
// render in a progress window over it (TrackRenderHost).
// How the main window meets the system: by default as the app runs, on the screen and in JUCE's application. The tests
// (tests/MainWindowTests.cpp) build it with neither: they put it on the desktop themselves, as a window on no screen, open
// its dialogs the same way, and say what quitting does.
struct MainWindowSurroundings
{
    bool addToDesktop = true; // false: whoever made it adds it, once it is made
    std::function<juce::DialogWindow*(juce::DialogWindow::LaunchOptions&)> launchDialog {}; // null: launchAsync()
    std::function<void()> quit {}; // null: the application's systemRequestedQuit(), which asks to save first (Main.cpp)
    std::function<juce::Array<juce::Rectangle<int>>()> screens {}; // the screens' areas for windows; null: the desktop's
};

class MainWindow : public juce::DocumentWindow,
                   public TrackRenderHost,
                   private juce::MenuBarModel,
                   private juce::ChangeListener,
                   private juce::DarkModeSettingListener,
                   private juce::Timer
{
public:
    // How the window meets the system (MainWindowSurroundings, above: outside the class, as a default argument can't be
    // made of a class's own struct with default member initializers while the class is still being defined).
    using Surroundings = MainWindowSurroundings;

    MainWindow (ui::CommandBridge&, ProjectSession&, ui::LookAndFeel&, AppServices&, Surroundings = {});
    ~MainWindow() override;

    // A freeze or a bounce, in a progress window with Cancel; and what came of it, an error if it failed.
    void runRender (std::unique_ptr<TrackRender>) override;
    void renderFinished (const juce::String& title, const juce::Result&) override;
    void renderWrote (const juce::String& title, const juce::File&) override;

    ui::AppShell& getShell() noexcept { return *shell; }

    // The window's commands ("options.", "input.", "help." and the shell's "view." commands), with Check for Updates... and
    // About Phazite.
    void addCommands();

    // The welcome window, the first time Phazite opens.
    void showWelcomeOnFirstRun();

    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Phazite's own title bar, on Windows (ui/shell/TitleBar.h): the content fills the window, edges and all, the bar's empty
    // parts move it, and Windows hears that it has the buttons the bar draws.
    WindowControlKind findControlAtPoint (juce::Point<float>) const override;
    int getDesktopWindowStyleFlags() const override;
    void resized() override;                // the panels keep out of the band along a restored window's sides; and keepPlace()
    void moved() override;                  // keepPlace()
    void parentHierarchyChanged() override; // put on the desktop: maximized if it was closed so
    juce::BorderSize<int> getBorderThickness() const override { return {}; }

    static constexpr int defaultWidth = 1280, defaultHeight = 800; // as it first opens, centred on the main screen

private:
    const Surroundings surroundings;
    ui::CommandBridge& commands;
    ProjectSession& session;
    ui::LookAndFeel& lookAndFeel;
    AppServices& services;
    ui::ComputerKeyboard computerKeyboard;
    ui::KeyDispatcher shortcutKeys;               // runs commands from keys, in this window and over plug-in windows
    std::unique_ptr<ui::WindowKeys> windowKeys;   // the window's shortcuts and the computer keyboard, in the order they get keys
    std::unique_ptr<ui::PluginKeys> pluginKeys;   // play, stop and the computer keyboard over plug-in windows
    PluginWindowServices pluginWindowServices;    // what plug-in windows' menus work with
    std::unique_ptr<DeviceWindows> deviceWindows; // the open project's devices' windows, which use the keys above
    ui::AppShell* shell = nullptr;                // owned by the window as its content

    // Hover help: tips inside this window, on frosted glass, and those of the other windows (ui::Tooltips); null when it is
    // turned off.
    std::unique_ptr<ui::Tooltips> tipsInside, tipsElsewhere;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow, pluginListWindow, welcomeWindow, exportWindow, progressionsWindow,
        renderWindow, cleanUpWindow, aboutWindow, updatesWindow;
    juce::Component::SafePointer<ui::SettingsPanel> settingsPanel;
    juce::Component::SafePointer<ui::WelcomePanel> welcomePanel;

    juce::DialogWindow* launch (juce::DialogWindow::LaunchOptions&); // a dialog, modal, over this window
    void requestQuit();                                              // as the close button and Quit do

    // Where the window opens: where the settings say it was closed, maximized if it was, if that is on a screen that still
    // exists (placeWindow()); otherwise its default size, centred on the main screen, fitted to it. And the place kept in the
    // settings as it moves or changes size, or is maximized (not while it is minimized or fills the screen).
    void restorePlace();
    void keepPlace();
    bool placeRestored = false;         // keepPlace() keeps nothing until then
    bool maximizeWhenOnDesktop = false; // closed maximized: maximized once it is on the desktop
    void maximizeIfWaiting();
    juce::Array<juce::Rectangle<int>> getScreens() const;

    void showSettings (std::optional<ui::SettingsPanel::Tab>); // no tab: Audio, or the open window's tab
    void showPluginList();                                     // the plug-ins scanning found, with a scan for VST3s
    void showWelcome (ui::WelcomePanel::Page);
    void showExport();       // for the open project's song
    void showCleanUp();      // Clean Up Unused Audio..., for the open project
    void showProgressions(); // the chord progressions, for the open project's chord lane
    void showAbout();        // Phazite's version, license and notices (ui/shell/AboutPanel.h)

    // The Updates window (ui/shell/UpdatePanel.h), which looks for a newer release unless one was found already; Install and
    // Restart in it saves the project if the user wants, starts the installer, and quits.
    void showUpdates();
    void installUpdate();
    void showUpdateState(); // the settings' icon's badge and the update command's name, as the update check is now
    juce::String shownUpdateName;
    void setTheme (ui::ThemeName);

    // Theme: Follow System: dark or light as the operating system is set, changing with it, as Ableton Live 12's Follow System,
    // VS Code's and Figma's do (the owner's UX audit, 2026-11-12); the settings keep it as "system".
    void followSystemTheme();
    bool followsSystemTheme = false;
    void applyTheme (ui::ThemeName); // the palette, in every window
    void darkModeSettingChanged() override;
    void setHoverHelp (bool);
    void setInterfaceScale (float);
    void stepInterfaceScale (int direction); // to the next larger (1) or smaller (-1) size
    bool isFullScreenMode() const;           // the window fills the screen ("view.fullScreen")
    void setFullScreenMode (bool);
    void setStartupSound (bool); // whether Phazite plays its sound once it has opened ("options.startupSound")
    void setKeymap (const juce::String& name);
    void updateTitle();

    // Tells something without asking anything: news, or a problem, with a button for what to do next if there is one, in a
    // notice card over the window's corner (ui/shell/Notices.h); in a dialog while the window has no content yet.
    void tell (const juce::String& title, const juce::String& message, bool isProblem, const juce::String& actionName = {},
               std::function<void()> action = {});
    ui::TitleBar::WindowActions getWindowActions(); // what the title bar's own window buttons do

    void timerCallback() override { updateTitle(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the keys changed, or the update check

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

// The window's menus, as its title bar names them (MainWindow's, and the README's picture's: tests/ReadmePicture.cpp).
juce::StringArray getMainMenuNames();

// Where a window that was at saved opens, given the screens' areas for windows (each display's, without its task bar): as it
// was, if it lies on them (across two side by side, too); fitted to the screen that holds most of it (no larger, and moved
// onto it) if part of it is off them; and nowhere (the caller's default) if too little of it is on any screen to take it by
// its title bar (windowShownAtLeast pixels each way), as when the screen it was on is gone, or if saved is empty.
std::optional<juce::Rectangle<int>> placeWindow (juce::Rectangle<int> saved, const juce::Array<juce::Rectangle<int>>& screens);
inline constexpr int windowShownAtLeast = 64;
} // namespace phazite
