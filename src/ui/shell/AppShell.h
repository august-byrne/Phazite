// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content: top bar, browser, playlist, inspector, editor dock and hint bar.
#pragma once

#include "Guide.h"
#include "HintBar.h"
#include "PanelLayout.h"
#include "Panels.h"
#include "TopBar.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/browser/Browser.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/mixer/DevicesView.h"
#include "ui/mixer/Inspector.h"
#include "ui/mixer/MixerView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/Icons.h"

namespace phazite::ui
{
class AppShell : public juce::Component,
                 private juce::FocusChangeListener,
                 private juce::ChangeListener,
                 private ProjectContext::Listener
{
public:
    // The commands, as menus and keys see them, the open project and the services (settings, Phazite Sounds, the library
    // and the previewer: app/AppServices.h) are the app's, and outlive this. Detached panels' windows run shortcuts through
    // the dispatcher. The views show the project the context has open, and none while it has none.
    AppShell (EngineContext&, CommandBridge&, KeyDispatcher&, ComputerKeyboard&, ProjectContext&, Settings&, SoundPack&, Library&,
              Previewer&);
    ~AppShell() override;

    // A panel (not the song's views) can move to a window of its own, for example on a second monitor. Closing that
    // window puts the panel back.
    void setDetached (Panel, bool);
    bool isDetached (Panel) const;
    juce::Component& getPanel (Panel); // docked here, or in its own window

    // Which panels show, which fills the window, and their sizes (PanelLayout.h); the settings keep them.
    const PanelLayout& getPanelLayout() const noexcept { return panels; }

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

    // Adds the commands for panels, layouts and the palette (the "view." commands), the plug-in picker, and saving
    // presets, which the browser shows.
    void addCommands (CommandRegistry&);

    // Updates tooltips and hints that mention shortcuts, e.g. after the keymap changed.
    void refreshShortcutTexts();

    // For tests.
    HintBar& getHintBar() noexcept { return hintBar; }
    bool showsOpening() const noexcept { return openingLabel.isVisible(); } // for tests
    Guide& getGuide() noexcept { return guide; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    EngineContext& engineContext;
    CommandBridge& commands;
    KeyDispatcher& shortcutKeys; // for the windows of detached panels
    ProjectContext& projectContext;
    Settings& settings;   // keeps the panels' layout
    Library& library;     // what the browser and the plug-in picker show
    Previewer& previewer; // the browser's
    ComputerKeyboard& computerKeyboard;

    TopBar topBar;
    Browser browser;
    Playlist playlist;
    Inspector inspector;
    PianoRoll pianoRoll;
    AudioClipEditor audioClipEditor;
    MixerView mixer;
    DevicesView devices;                     // the selected track's
    SongViews songViews { playlist, mixer }; // the centre
    EditorDock editorDock { projectContext, pianoRoll, audioClipEditor, devices };
    HintBar hintBar;
    Guide guide;              // the hint bar's next step for newcomers
    juce::Label openingLabel; // over the centre until a project is open: what the window shows as Phazite starts

    // The editor used last, which the time line's commands (zoom, grid, tools, nudge) act on.
    TimelineEditor* activeEditor = nullptr;

    PanelLayout panels;
    Panel panelInUse = Panel::song; // the one the keyboard focus was in last, which "Maximize the Panel in Use" maximizes

    // Holds the song's views (the playlist or the mixer), the divider and the editor dock, and lays them out when
    // resized.
    struct Centre : public juce::Component
    {
        std::function<void()> onResize;
        void resized() override
        {
            if (onResize != nullptr)
                onResize();
        }
    };

    // Dragged, they size the panels beside them, which keep those sizes (PanelLayout).
    PanelDivider browserDivider { "Browser's edge", true }, inspectorDivider { "Inspector's edge", true },
        editorDivider { "Editor's edge", false };
    Centre centre;
    int sizeAtDragStart = 0; // of the panel beside the divider dragged

    // In each docked panel's top right corner: Maximize, a window of its own, and Hide.
    PanelButtons browserButtons { commands, Panel::browser }, songButtons { commands, Panel::song },
        editorButtons { commands, Panel::editor }, inspectorButtons { commands, Panel::inspector };

    struct PanelWindow;
    std::unique_ptr<PanelWindow> browserWindow, inspectorWindow, editorWindow; // null while docked

    void updateLayout(); // after the panels changed: lays them out, and keeps the layout in the settings
    void layOutCentre(); // the song's views over the editor dock
    void dragDivider (Panel, int distance);
    void togglePanel (Panel);
    void toggleMaximized (Panel);
    void maximizePanelInUse(); // or restores the panels
    void toggleMixer();        // the mixer in the centre, or the playlist again if it shows ("view.mixer")

    // The editor the time line's commands act on: the one used last, while it shows; or one that shows, the playlist first.
    TimelineEditor* getTimelineEditorInUse();

    // "No audio output" in the hint bar while none plays, which a click on it takes to the audio settings (docs/performance-
    // audit.md, R3): the device failed to open, or went (unplugged, or Bluetooth that disconnected).
    void updateAudioAlert();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateAudioAlert(); } // the audio devices changed

    void showEditor (EditorDock::Tab);   // or hides it if it is showing, as FL Studio's F7 does
    void showSongView (SongViews::Tab);  // in the centre
    void revealEditor (EditorDock::Tab); // shows it, even if it is showing already
    void showSelectedDevice();           // in the Devices view (the "device.show" command)
    void placePanelButtons();
    void setActiveEditor (TimelineEditor*);
    bool placeItem (const LibraryItem&);                 // from the browser or the plug-in picker, onto the selected track
    juce::String getDefaultHint() const;                 // the guide's next step, or how to search for commands
    void globalFocusChanged (juce::Component*) override; // makes the editor that has the focus the active one
    void projectChanged() override;                      // stops a preview, which may be in time with the project

    PanelButtons& getButtons (Panel);
    std::unique_ptr<PanelWindow>& getWindow (Panel); // a detachable panel's (not the song's views)
};
} // namespace phazite::ui
