// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ProjectSession.h"
#include "model/ComputerKeyboard.h"
#include "ui/AppShell.h"
#include "ui/LookAndFeel.h"
#include "ui/SettingsPanel.h"

#include <optional>

namespace phazite
{
// Phazite's one window: the menu bar and the AppShell. It also owns the dialogs, and the computer
// keyboard, which plays notes while this window has the keyboard focus.
class MainWindow : public juce::DocumentWindow,
                   private juce::MenuBarModel,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    MainWindow (te::Engine&, CommandRegistry&, ProjectSession&, ui::LookAndFeel&);
    ~MainWindow() override;

    ui::AppShell& getShell() noexcept { return *shell; }

    // The window's commands ("options.", "input." and the shell's "view." commands).
    void addCommands();

    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    te::Engine& engine;
    CommandRegistry& commands;
    ProjectSession& session;
    ui::LookAndFeel& lookAndFeel;
    ComputerKeyboard computerKeyboard;
    ui::AppShell* shell = nullptr; // owned by the window as its content

    std::unique_ptr<juce::TooltipWindow> tooltipWindow; // hover help; null when it is turned off
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow, pluginListWindow;
    juce::Component::SafePointer<ui::SettingsPanel> settingsPanel;

    void showSettings (std::optional<ui::SettingsPanel::Tab>); // no tab: Audio, or the open window's tab
    void showPluginList();
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
