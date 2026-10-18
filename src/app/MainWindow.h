// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "AppServices.h"
#include "ClipWaveforms.h"
#include "ProjectSession.h"
#include "model/mixer/FreezeModel.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/PluginKeys.h"
#include "ui/input/WindowKeys.h"
#include "ui/shell/AppShell.h"
#include "ui/shell/SettingsPanel.h"
#include "ui/shell/Welcome.h"
#include "ui/theme/LookAndFeel.h"

#include <optional>

namespace phazite
{
// Phazite's one window: the menu bar and the AppShell. It also owns the dialogs, and the computer
// keyboard, which plays notes while this window has the keyboard focus. Freezes and bounces render in a progress window
// over it (TrackRenderHost).
class MainWindow : public juce::DocumentWindow,
                   public TrackRenderHost,
                   private juce::MenuBarModel,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    MainWindow (te::Engine&, ui::CommandBridge&, ProjectSession&, ui::LookAndFeel&, AppServices&);
    ~MainWindow() override;

    // A freeze or a bounce, in a progress window with Cancel; and what came of it, an error if it failed.
    void runRender (std::unique_ptr<TrackRender>) override;
    void renderFinished (const juce::String& title, const juce::Result&) override;

    ui::AppShell& getShell() noexcept { return *shell; }

    // The window's commands ("options.", "input.", "help." and the shell's "view." commands).
    void addCommands();

    // The welcome window, the first time Phazite opens.
    void showWelcomeOnFirstRun();

    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    te::Engine& engine;
    ui::CommandBridge& commands;
    ProjectSession& session;
    ui::LookAndFeel& lookAndFeel;
    AppServices& services;
    ui::ComputerKeyboard computerKeyboard;
    ui::KeyDispatcher shortcutKeys;             // runs commands from keys, in this window and over plug-in windows
    std::unique_ptr<ui::WindowKeys> windowKeys; // the window's shortcuts and the computer keyboard, in the order they get keys
    std::unique_ptr<ui::PluginKeys> pluginKeys; // play, stop and the computer keyboard over plug-in windows
    ui::AppShell* shell = nullptr;              // owned by the window as its content

    std::unique_ptr<juce::TooltipWindow> tooltipWindow; // hover help; null when it is turned off
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow, pluginListWindow, welcomeWindow, exportWindow, progressionsWindow,
        renderWindow;
    juce::Component::SafePointer<ui::SettingsPanel> settingsPanel;
    juce::Component::SafePointer<ui::WelcomePanel> welcomePanel;

    void showSettings (std::optional<ui::SettingsPanel::Tab>); // no tab: Audio, or the open window's tab
    void showPluginList();
    void showWelcome (ui::WelcomePanel::Page);
    void showExport();       // for the open project's song
    void showProgressions(); // the chord progressions, for the open project's chord lane
    void setTheme (ui::ThemeName);
    void setHoverHelp (bool);
    void setInterfaceScale (float);
    void stepInterfaceScale (int direction); // to the next larger (1) or smaller (-1) size
    void setKeymap (const juce::String& name);
    void updateTitle();

    void timerCallback() override { updateTitle(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the keys changed

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String& menuName) override;
    void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace phazite
