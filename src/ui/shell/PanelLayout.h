// SPDX-License-Identifier: AGPL-3.0-or-later
// Which of the main window's panels show, and how large (docs/design.md, section 4): the browser on the left, the song's
// views (the playlist or the mixer) in the centre over the editor dock, and the inspector on the right. Each can be
// hidden, and one at a time can fill the window, maximized, until it is restored: the others then come back as they
// were, at their sizes. Asking to see another panel restores the window first. The centre always shows the song's views,
// the editor dock or both, so hiding the one of them that shows shows the other. A panel in a window of its own
// (detached) takes no room in the main window and can't be maximized; the song's views always stay in it. Plain values,
// which AppShell lays out and the settings keep, so the window opens as it was left (detached panels come back docked).
// And the buttons in a panel's top right corner that change how it shows, and the dividers between panels.
#pragma once

#include "ui/input/CommandBridge.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>

namespace phazite::ui
{
enum class Panel
{
    browser,
    song, // the playlist or the mixer
    editor,
    inspector
};

class PanelLayout
{
public:
    bool isShown (Panel) const noexcept;
    void show (Panel);
    void hide (Panel); // unless canHide()
    void toggle (Panel);

    // The song's views stay while the editor dock is in a window of its own: the centre would be empty. And whether its Show
    // or Hide command can do anything: not while it is in a window of its own (closing that window puts it back).
    bool canHide (Panel) const noexcept;
    bool canToggle (Panel panel) const noexcept { return ! isDetached (panel) && (! takesRoom (panel) || canHide (panel)); }

    std::optional<Panel> getMaximized() const noexcept { return maximized; }
    bool isMaximized (Panel panel) const noexcept { return maximized == panel; }
    bool canMaximize (Panel panel) const noexcept { return ! isDetached (panel); }
    void maximize (Panel); // shows it; unless canMaximize()
    void restore() noexcept { maximized.reset(); }
    void toggleMaximized (Panel);

    void setDetached (Panel, bool); // not the song's views
    bool isDetached (Panel) const noexcept;

    // Whether the panel takes room in the main window now: shown, not in a window of its own, and no other maximized.
    bool takesRoom (Panel) const noexcept;

    // The sizes the dividers were dragged to, kept while panels hide and show: the side panels' widths, in pixels, and
    // the share of the centre the editor dock takes under the song's views.
    int browserWidth = Shell::browserWidth, inspectorWidth = Shell::inspectorWidth;
    double editorShare = Shell::editorShare;

    // The side panels' widths in a window this wide (0 for one that takes no room): as kept, but narrower, down to their
    // least, where the centre would otherwise be narrower than its least; a maximized one, the whole width. And the editor
    // dock's height under the song's views in a centre this tall, each at least its least.
    struct SideWidths
    {
        int browser = 0, inspector = 0;
    };

    SideWidths getSideWidths (int width) const;
    int getEditorHeight (int centreHeight) const;

    // What its commands are called as menus show them now: "Hide the Browser" while it shows here, otherwise "Show the
    // Browser"; and "Restore the Panels" while it fills the window (or, for the panel in use, any does), otherwise nothing
    // (the command's own name).
    juce::String getToggleName (Panel) const;
    juce::String getMaximizeName (Panel) const;
    juce::String getMaximizeInUseName() const;

    // For the settings ("shown=browser,song,editor maximized=song browser=240 inspector=260 editor=0.45"), and back: what
    // a text doesn't say, or says wrongly, stays as it is.
    juce::String toText() const;
    void setFromText (const juce::String&);

private:
    std::array<bool, 4> shown { true, true, true, true };
    std::array<bool, 4> detached {};
    std::optional<Panel> maximized;

    static size_t indexOf (Panel panel) noexcept { return (size_t) panel; }
};

// The name a panel goes by in the settings ("browser", "song", "editor", "inspector"); its commands' IDs ("view." and
// the verb, then "Browser", "Song", "Editor" or "Inspector": view.toggleBrowser); its name in commands ("Browser",
// "Playlist and Mixer"); and what a tooltip calls it ("the browser").
juce::String getPanelKey (Panel);
std::optional<Panel> findPanelByKey (const juce::String&);
juce::String getPanelCommandID (const char* verb, Panel);
juce::String getPanelName (Panel);
juce::String describePanel (Panel);

//==============================================================================
// The buttons in a panel's top right corner, which run its commands: Maximize (or Restore the Panels), a window of its
// own (not for the song's views, which stay in the main window) and Hide.
class PanelButtons : public juce::Component
{
public:
    PanelButtons (CommandBridge&, Panel);

    void update (const PanelLayout&); // the first button maximizes the panel, or restores the others
    void refreshTooltips();           // with the commands' keys, after the keymap changed

    int getPreferredWidth() const noexcept;
    void resized() override;

private:
    CommandBridge& commands;
    Panel panel;
    bool maximized = false;
    IconButton maximizeButton, detachButton, hideButton;
    juce::Array<IconButton*> getButtons();
};

//==============================================================================
// The line between two panels, which a drag moves: across the window for one between columns, up and down for the one
// over the editor dock. It lights while the pointer is on it.
class PanelDivider : public juce::Component
{
public:
    PanelDivider (const juce::String& title, bool betweenColumns);

    std::function<void()> onDragStart;
    std::function<void (int distance)> onDrag; // from where the drag began, right or down

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { repaint(); }

private:
    bool betweenColumns;
};
} // namespace phazite::ui
