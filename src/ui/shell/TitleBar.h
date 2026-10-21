// SPDX-License-Identifier: AGPL-3.0-or-later
// The bar along the top of the window (docs/design.md, section 4), as VS Code's: the menus and the project's name on the
// left, Undo and Redo beside the command search in the middle, and the panels' switches and the Layout menu on the right;
// and, where it is the window's own title bar (Windows), the window's buttons, its empty parts dragging the window
// (app/MainWindow.h asks getControlAt()). On Linux the system's title bar stays above it, and on macOS the menus are in
// the menu bar at the top of the screen. In a narrow window the menus fold into one button, then the project's name
// goes, and the search narrows.
#pragma once

#include "PanelLayout.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>

namespace phazite::ui
{
class TitleBar : public juce::Component,
                 private juce::Timer
{
public:
    explicit TitleBar (CommandBridge&);
    ~TitleBar() override;

    // The window's menus, which the bar shows (none on macOS, and in tests); and the first of them, open, for the keys
    // to move through ("view.menus").
    void setMenus (juce::MenuBarModel*);
    bool hasMenus() const noexcept { return menuModel != nullptr; }
    void showMenus();

    // The window's own buttons, where this bar is its title bar.
    struct WindowActions
    {
        std::function<void()> minimise, toggleMaximised, close;
        std::function<bool()> isMaximised;
    };

    void setWindowActions (std::optional<WindowActions>);
    bool hasWindowButtons() const noexcept { return windowActions.has_value(); }

    void setProjectName (const juce::String& name, bool unsavedChanges);
    juce::String getProjectName() const { return projectName.getText(); } // with its dot, while it has unsaved changes
    void setWindowActive (bool);                                          // dims it while another window is in front
    void showPanels (const PanelLayout&);                                 // the panels' switches, lit for those that show

    juce::Component& getSearchBox() noexcept; // the palette and the plug-in picker hang from it
    void refreshTooltips();                   // with the keys of the keymap in use

    // What the window's frame does at a point, in this bar's coordinates: drag the window (the bar's empty parts and the
    // project's name), act as one of the window's buttons, or let the bar's own controls have it.
    juce::Component::WindowControlKind getControlAt (juce::Point<int>) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

private:
    struct SearchBox;
    struct WindowButton;
    struct PanelSwitch;

    CommandBridge& commands;
    juce::MenuBarModel* menuModel = nullptr;
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    IconButton menuButton { "Menus", Icon::menu }; // the menus, folded, in a narrow window
    juce::Label projectName;
    IconButton undoButton { "Undo", Icon::undo }, redoButton { "Redo", Icon::redo }, layoutButton { "Layout", Icon::layout };
    std::unique_ptr<SearchBox> searchBox;
    std::unique_ptr<PanelSwitch> browserSwitch, editorSwitch, inspectorSwitch;
    std::unique_ptr<WindowButton> minimiseButton, maximiseButton, closeButton;
    std::optional<WindowActions> windowActions;
    bool active = true;

    void timerCallback() override; // Undo's and Redo's step and whether there is one, and the maximise button, 4 times a second
    void showFoldedMenus();
    void showLayoutMenu();
    int getMenusWidth() const;
};
} // namespace phazite::ui
