// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ProjectSession.h"
#include "ui/AppShell.h"
#include "ui/LookAndFeel.h"

namespace phazite
{
// Phazite's one window: the menu bar and the AppShell. It also owns the settings dialogs.
class MainWindow : public juce::DocumentWindow,
                   private juce::MenuBarModel,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    MainWindow (te::Engine&, CommandRegistry&, ProjectSession&, ui::LookAndFeel&);
    ~MainWindow() override;

    ui::AppShell& getShell() noexcept { return *shell; }

    // The window's commands ("options." and the shell's "view." commands).
    void addCommands();

    void closeButtonPressed() override;

private:
    te::Engine& engine;
    CommandRegistry& commands;
    ProjectSession& session;
    ui::LookAndFeel& lookAndFeel;
    ui::AppShell* shell = nullptr; // owned by the window as its content

    std::unique_ptr<juce::TooltipWindow> tooltipWindow; // hover help; null when it is turned off
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow, pluginListWindow, shortcutsWindow;

    void showAudioSettings();
    void showPluginList();
    void showKeyboardShortcuts();
    void setTheme (ui::ThemeName);
    void setHoverHelp (bool);
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
