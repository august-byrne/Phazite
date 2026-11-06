// SPDX-License-Identifier: AGPL-3.0-or-later
// The column of icons along the window's left edge, as VS Code's activity bar (docs/versions.md: The activity bar): one for
// each of the left panel's views, the browser, the project (what the song holds: ui/project/ProjectView.h) and the versions,
// and at its foot, as VS Code's Accounts and Manage, the account (none yet: its menu says so) and the settings, whose menu
// holds the settings' commands. It is always there: while the left panel is hidden, while another panel fills the window and
// in full screen. A click on the icon of the view the left panel shows hides the panel; on another, or while the panel is
// hidden, it shows that view there (the view.browser, view.project and view.versions commands, AppShell's). The view that
// shows is marked, and while the song has changes since its branch's last version, a badge on the Versions icon counts them.
#pragma once

#include "PanelLayout.h"
#include "model/project/ProjectContext.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace phazite::ui
{
class ActivityBar : public juce::Component,
                    private ProjectContext::Listener
{
public:
    ActivityBar (CommandBridge&, ProjectContext&);
    ~ActivityBar() override;

    // Which view the left panel shows, if it shows (in the main window or a window of its own).
    void showLeftPanel (bool shows, LeftView);
    void refreshTooltips(); // with the commands' keys, after the keymap changed

    // For tests: whether an icon is marked as the view that shows, the count the Versions icon's badge shows (0 while it shows
    // none), and the icons, the views' and the account's and the settings' at the foot.
    bool isMarked (LeftView) const;
    int getVersionsBadge() const;
    juce::Button& getButton (LeftView);
    juce::Button& getAccountButton();
    juce::Button& getSettingsButton();

    // The settings' menu, as its icon opens it: Search Commands, the settings' windows, then the themes and the keymaps.
    juce::PopupMenu makeSettingsMenu();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Item;
    class FootItem;

    CommandBridge& commands;
    ProjectContext& context;
    std::unique_ptr<Item> browserItem, projectItem, versionsItem;
    std::unique_ptr<FootItem> accountItem, settingsItem;

    Item& getItem (LeftView) const;
    void updateBadge();
    void showAccountMenu();
    void projectChanged() override { updateBadge(); }
    void versionsChanged() override { updateBadge(); }
};

// The command that shows or hides one of the left panel's views ("view.browser", "view.project", "view.versions"), and the
// view's name.
juce::String getLeftViewCommandID (LeftView);
juce::String getLeftViewName (LeftView);
} // namespace phazite::ui
