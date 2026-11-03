// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content: the title bar, the transport bar, the browser, two areas of views one over the other (by
// default the playlist and the mixer over the clip editor and the devices), the inspector and the status bar.
#pragma once

#include "Guide.h"
#include "LessonCard.h"
#include "Lessons.h"
#include "PanelLayout.h"
#include "Panels.h"
#include "StatusBar.h"
#include "TitleBar.h"
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
                 public juce::DragAndDropContainer, // a view's tab dragged from one area to the other (Panels.h)
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

    // A panel (not the upper area of views) can move to a window of its own, for example on a second monitor. Closing that
    // window puts the panel back.
    void setDetached (Panel, bool);
    bool isDetached (Panel) const;
    juce::Component& getPanel (Panel); // docked here, or in its own window

    // Which panels show, which fills the window, and their sizes (PanelLayout.h); the settings keep them.
    const PanelLayout& getPanelLayout() const noexcept { return panels; }

    // The bars along the top and the bottom. The window gives the title bar its menus and its own buttons (Windows), and
    // asks it what its frame does where.
    TitleBar& getTitleBar() noexcept { return titleBar; }
    StatusBar& getStatusBar() noexcept { return statusBar; }
    juce::Component::WindowControlKind getWindowControlAt (juce::Point<int>) const;

    // The open project's name and whether it is saved, which the window knows: the title bar and the status bar show them.
    void showProject (const juce::String& name, bool unsavedChanges, bool neverSaved);

    // How far the panels keep from the window's sides: the band there that resizes a restored window on Windows, whose
    // scroll bars it would take (app/MainWindow.h). The bars across the top and the bottom reach the sides.
    void setEdgeInset (int);

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
    HintBar& getHintBar() noexcept { return statusBar.getHintBar(); }
    bool showsOpening() const noexcept { return openingLabel.isVisible(); } // for tests
    Guide& getGuide() noexcept { return guide; }
    Lessons& getLessons() noexcept { return lessons; }
    LessonCard& getLessonCard() noexcept { return lessonCard; }

    // The control a lesson's step names (Lessons::Step::target), if it shows: a component with that ID, the first found depth
    // first, or one inside another ("pianoRoll/tool.draw").
    juce::Component* findLessonTarget (const juce::String& target);

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

    TitleBar titleBar;
    TopBar topBar;
    Browser browser;
    Playlist playlist;
    Inspector inspector;
    PianoRoll pianoRoll;
    AudioClipEditor audioClipEditor;
    MixerView mixer;
    DevicesView devices;                                                      // the selected track's
    ClipEditorView clipEditor { projectContext, pianoRoll, audioClipEditor }; // the selected clip's
    ViewPlaces viewPlaces;                                                    // which views each area holds; the settings keep it
    ViewRow upperRow { Panel::song, { playlist, mixer, clipEditor, devices } };
    ViewRow lowerRow { Panel::editor, { playlist, mixer, clipEditor, devices } };
    StatusBar statusBar;
    Guide guide;     // the hint bar's next step for newcomers
    Lessons lessons; // Help > Lessons, in the lesson card while one runs
    LessonCard lessonCard { lessons, [this] (const juce::String& target) { return findLessonTarget (target); } };
    juce::Label openingLabel; // over the centre until a project is open: what the window shows as Phazite starts

    // The editor used last, which the time line's commands (zoom, grid, tools, nudge) act on.
    TimelineEditor* activeEditor = nullptr;

    PanelLayout panels;
    Panel panelInUse = Panel::song; // the one the keyboard focus was in last, which "Maximize the Panel in Use" maximizes
    int edgeInset = 0;              // setEdgeInset()

    // Holds the two areas of views, one over the other, and the divider between them, and lays them out when resized.
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
    void maximizePanelInUse();            // or restores the panels
    void toggleMixer();                   // the mixer, or the playlist again if it shows beside it ("view.mixer")
    void setScrollingContinuously (bool); // in every editor that follows playback, kept in the settings

    // The editor the time line's commands act on: the one used last, while it shows; or one that shows, the playlist first.
    TimelineEditor* getTimelineEditorInUse();

    // The audio output in the status bar, and "No audio output" in its hint bar while none plays, which a click on it takes
    // to the audio settings (docs/performance-audit.md, R3): the device failed to open, or went (unplugged, or Bluetooth
    // that disconnected).
    void updateAudioAlert();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updateAudioAlert(); } // the audio devices changed

    ViewArea& getAreaOf (ViewTab);              // the area that holds it
    bool takesRoom (Panel) const;               // PanelLayout::takesRoom(), and for the two areas, holding a view
    void showView (ViewTab);                    // in its area; in the lower panel, or the panel hidden if it shows it, as FL Studio's F7
    void revealView (ViewTab);                  // in its area, even if it shows already
    void moveView (View, ViewPlace, int index); // into that area (ViewPlaces::move()), where it shows
    void placeViewBeside (View, Panel row, bool left); // in an area of its own beside a row's (ViewPlaces::placeBeside())
    void revealMoved (View);                           // where it went
    void bringViewsBack (Panel row);                   // to a row without views, as it is shown: its own, as they start
    std::array<ViewArea*, 4> getAreas();               // the upper row's two, then the lower one's
    void resetViewPlaces();                            // every view back where it starts
    void placeViews();                                 // in the areas, as viewPlaces says
    void showSelectedDevice();                         // in the Devices view (the "device.show" command)
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
