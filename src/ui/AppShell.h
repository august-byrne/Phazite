// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content: top bar, browser, playlist, inspector, editor dock and hint bar.
#pragma once

#include "ChannelStrip.h"
#include "HintBar.h"
#include "Icons.h"
#include "Panels.h"
#include "Playlist.h"
#include "TopBar.h"

namespace phazite::ui
{
class AppShell : public juce::Component
{
public:
    AppShell (te::Engine&, CommandRegistry&, ComputerKeyboard&, std::function<te::Edit*()> getEdit, te::SelectionManager&, RangeSelection&);
    ~AppShell() override;

    // The open project's Edit, or nullptr while none is open (call this before it closes, too).
    void setEdit (te::Edit*);

    // The panels that can move to a window of their own, for example on a second monitor. Closing
    // that window puts the panel back.
    enum class DetachablePanel
    {
        browser,
        inspector,
        editor
    };

    void setDetached (DetachablePanel, bool);
    bool isDetached (DetachablePanel) const;

    // Layouts arrange the panels; they don't hide features.
    enum class Layout
    {
        beat, // playlist and piano roll, browser open
        mix,  // a large mixer, inspector open
        full  // everything
    };

    void setLayout (Layout);
    void showCommandPalette();

    // Adds the commands for panels, layouts and the palette (the "view." commands).
    void addCommands (CommandRegistry&);

    // Updates tooltips and hints that mention shortcuts, e.g. after the keymap changed.
    void refreshShortcutTexts();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CommandRegistry& commands;

    TopBar topBar;
    Panel browser { "Browser",
                    "Instruments, effects, plug-ins, samples and projects, by folder or by search. Coming in roadmap step 1.5." };
    Playlist playlist;
    Inspector inspector;
    EditorDock editorDock;
    HintBar hintBar;

    bool browserShown = true, inspectorShown = true, editorShown = true;
    double editorShare = 0.4; // of the space below the top bar

    // Holds the playlist, its divider and the editor dock, and lays them out when resized (which is
    // also what the divider triggers when dragged).
    struct Centre : public juce::Component
    {
        std::function<void()> onResize;
        void resized() override
        {
            if (onResize != nullptr)
                onResize();
        }
    };

    juce::StretchableLayoutManager columns, rows;
    juce::StretchableLayoutResizerBar browserDivider { &columns, 1, true };
    juce::StretchableLayoutResizerBar inspectorDivider { &columns, 3, true };
    juce::StretchableLayoutResizerBar editorDivider { &rows, 1, false };
    Centre centre;

    // Each docked panel has a button in its top right corner that moves it to its own window.
    IconButton browserDetachButton { "Move the browser to its own window", Icon::detach },
        inspectorDetachButton { "Move the inspector to its own window", Icon::detach },
        editorDetachButton { "Move the editor to its own window", Icon::detach };

    struct PanelWindow;
    std::unique_ptr<PanelWindow> browserWindow, inspectorWindow, editorWindow; // null while docked

    void updateLayout();
    void showEditor (EditorDock::Tab);
    void placeDetachButtons();

    juce::Component& getPanel (DetachablePanel);
    IconButton& getDetachButton (DetachablePanel);
    std::unique_ptr<PanelWindow>& getWindow (DetachablePanel);
    const std::unique_ptr<PanelWindow>& getWindow (DetachablePanel) const;
};
} // namespace phazite::ui
