// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content: top bar, browser, playlist, inspector, editor dock and hint bar.
#pragma once

#include "HintBar.h"
#include "Panels.h"
#include "TopBar.h"
#include "ui/browser/Browser.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/MixerView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/Icons.h"

namespace phazite::ui
{
class AppShell : public juce::Component,
                 private juce::FocusChangeListener
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
    void showPluginPicker(); // F8

    // Adds the commands for panels, layouts and the palette (the "view." commands).
    void addCommands (CommandRegistry&);

    // Updates tooltips and hints that mention shortcuts, e.g. after the keymap changed.
    void refreshShortcutTexts();

    SoundPack& getSoundPack() noexcept { return soundPack; } // Phazite Sounds, which the browser shows

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CommandRegistry& commands;
    te::SelectionManager& selection;
    std::function<te::Edit*()> getEdit;
    SoundPack soundPack; // Phazite Sounds, downloaded when the user asks
    Library library;     // what the browser and the plug-in picker show
    Previewer previewer; // the browser's

    TopBar topBar;
    Browser browser;
    Playlist playlist;
    Inspector inspector;
    PianoRoll pianoRoll;
    Mixer mixer;
    EditorDock editorDock { pianoRoll, mixer };
    HintBar hintBar;

    // The editor used last, which the time line's commands (zoom, grid, tools, nudge) act on.
    TimelineEditor* activeEditor = nullptr;

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
    void showEditor (EditorDock::Tab);   // or hides it if it is showing, as FL Studio's F7 and F9 do
    void revealEditor (EditorDock::Tab); // shows it, even if it is showing already
    void placeDetachButtons();
    void setActiveEditor (TimelineEditor*);
    bool placeItem (const LibraryItem&);                 // from the browser or the plug-in picker, onto the selected track
    void globalFocusChanged (juce::Component*) override; // makes the editor that has the focus the active one

    juce::Component& getPanel (DetachablePanel);
    IconButton& getDetachButton (DetachablePanel);
    std::unique_ptr<PanelWindow>& getWindow (DetachablePanel);
    const std::unique_ptr<PanelWindow>& getWindow (DetachablePanel) const;
};
} // namespace phazite::ui
