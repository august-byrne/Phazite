// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content: top bar, browser, playlist, inspector, editor dock and hint bar.
#pragma once

#include "HintBar.h"
#include "Panels.h"
#include "TopBar.h"

namespace phazite::ui
{
class AppShell : public juce::Component
{
public:
    AppShell (te::Engine&, CommandRegistry&, ComputerKeyboard&, std::function<te::Edit*()> getEdit);

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
    Panel playlist { "Playlist", "Tracks, clips and song sections. Coming in roadmap step 1.1." };
    Panel inspector { "Inspector",
                      "The selected track's or clip's settings, devices and sends. Coming with the mixer, in roadmap step 1.4." };
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

    void updateLayout();
    void showEditor (EditorDock::Tab);
};
} // namespace phazite::ui
