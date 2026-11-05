// SPDX-License-Identifier: AGPL-3.0-or-later
// The column of icons along the window's left edge, as VS Code's activity bar (docs/versions.md: The activity bar): one for
// each of the left panel's views, the browser and the versions. It is always there: while the left panel is hidden, while
// another panel fills the window and in full screen. A click on the icon of the view the left panel shows hides the panel;
// on the other, or while the panel is hidden, it shows that view there (the view.browser and view.versions commands,
// AppShell's). The view that shows is marked, and while the song has changes since its branch's last version, a dot on
// the Versions icon says so.
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

    // For tests: whether an icon is marked as the view that shows, and whether the Versions icon has its dot.
    bool isMarked (LeftView) const;
    bool showsVersionsDot() const;
    juce::Button& getButton (LeftView);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Item;

    CommandBridge& commands;
    ProjectContext& context;
    std::unique_ptr<Item> browserItem, versionsItem;

    Item& getItem (LeftView) const;
    void updateDot();
    void projectChanged() override { updateDot(); }
    void versionsChanged() override { updateDot(); }
};

// The command that shows or hides one of the left panel's views ("view.browser", "view.versions"), and the view's name.
juce::String getLeftViewCommandID (LeftView);
juce::String getLeftViewName (LeftView);
} // namespace phazite::ui
