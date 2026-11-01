// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's two tabbed areas, one over the other (docs/design.md, section 4): by default the whole song in the upper one
// and the selection, in detail, in the lower one. Their views move between them (the owner, 2026-11-01: "the user should be
// able to drag the mixer tab down into the bottom panel, or the piano roll up"), as editors' tabs move between groups in
// VS Code: a tab dragged onto the other area goes there, its right-click menu moves it too, and the settings keep where each
// view is. An area left without views hides. The views belong to the window; the areas only show them.
#pragma once

#include "PanelLayout.h"
#include "model/arrangement/PlaylistModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace phazite::ui
{
class AudioClipEditor;
class CommandBridge;
class DevicesView;
class PianoRoll;
class TimelineEditor;

//==============================================================================
// What moves between the areas: the playlist, the mixer, the selected clip's editor, and the selected track's devices, whose
// Instrument and Effects tabs are two pages of one view (DevicesView.h), so that they move together.
enum class View
{
    playlist,
    mixer,
    clipEditor,
    devices
};

// The areas' tabs: a view's, or one of the devices' two.
enum class ViewTab
{
    playlist,
    mixer,
    clipEditor,
    instrument,
    effects
};

View getView (ViewTab);

// Which views each area holds, in their tabs' order. The upper area is the panel PanelLayout calls Panel::song, the lower
// one Panel::editor.
struct ViewPlaces
{
    std::vector<View> upper { View::playlist, View::mixer }, lower { View::clipEditor, View::devices };

    Panel getArea (View) const;
    const std::vector<View>& getViews (Panel area) const;

    // Moves a view into an area, before the view at an index there (after its views for -1, or an index past them).
    void move (View, Panel area, int index = -1);

    // As the settings keep them: "playlist,mixer;clipEditor,devices". Anything but every view once reads as where they start.
    juce::String toString() const;
    static ViewPlaces fromString (const juce::String&);

    bool operator== (const ViewPlaces&) const = default;
};

// What a view's tab carries while it is dragged (DragAndDropContainer's description), and the view it names, if it is one.
juce::var makeViewDrag (View);
std::optional<View> readViewDrag (const juce::var&);

//==============================================================================
// The selected clip's editor: the piano roll for MIDI clips, the audio clip editor for audio clips, as the song's selection
// asks (model/arrangement/AudioClipModel.h: getClipTypeToEdit()); its tab is named for the one it shows.
class ClipEditorView : public juce::Component,
                       private juce::ChangeListener
{
public:
    ClipEditorView (ProjectContext&, PianoRoll&, AudioClipEditor&);
    ~ClipEditorView() override;

    // Opens a clip: a MIDI clip in the piano roll, an audio clip in the audio clip editor.
    void openClip (ClipID);

    TimelineEditor& getEditor() const noexcept;
    bool showsAudioClip() const noexcept { return showingAudio; }

    std::function<void()> onEditorChanged; // the other editor shows: its tab's name and tooltip follow

    void resized() override;

private:
    ProjectContext& context;
    PianoRoll& pianoRoll;
    AudioClipEditor& audioClipEditor;
    bool showingAudio = false;

    void show (ClipType);
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed: what it asks to show
};

//==============================================================================
// One of the two areas: a tab for each view it holds, one showing at a time.
class ViewArea : public juce::TabbedComponent,
                 public juce::DragAndDropTarget
{
public:
    // The views, which either area can show.
    struct Views
    {
        juce::Component& playlist;
        juce::Component& mixer;
        ClipEditorView& clipEditor;
        DevicesView& devices;
    };

    ViewArea (Panel area, const Views&);
    ~ViewArea() override;

    Panel getArea() const noexcept { return area; }

    // Shows these views' tabs, in this order; the tab that showed still shows if it is here.
    void setViews (const std::vector<View>&);
    bool holds (ViewTab) const;
    bool isEmpty() const noexcept { return tabs.empty(); }

    void showTab (ViewTab); // one it holds
    std::optional<ViewTab> getShownTab() const;

    // What each tab shows, with its key in the keymap in use (AppShell::refreshShortcutTexts()), and the clip editor's
    // name as it changes.
    void refreshTooltips (const CommandBridge&);
    void refreshTabs();

    // A view dragged here, or moved here from a tab's menu, to go before the view at an index here, or after them (-1): the
    // shell moves it (ViewPlaces). And the tab menu's Reset View Places.
    std::function<void (View, Panel area, int index)> onMove;
    std::function<void()> onReset;

    // The mixer's Routing switch, in the tab bar after the tabs while the mixer shows here, so that it takes none of the
    // mixer's height (the owner, 2026-11-01): on, every channel's wires show in place of the strips (MixerView::showRouting()).
    std::function<void (bool)> onRoutingChanged;
    juce::Button& getRoutingButton() noexcept { return routingButton; }

    // While a view is dragged over it, where it would go: before the view at an index here, or after them (-1). For tests.
    std::optional<int> getDropIndex() const noexcept { return dropIndex; }

    void resized() override;                           // its tabs leave the panel's buttons their corner, and the switch its place
    void paintOverChildren (juce::Graphics&) override; // a view dragged over it: a tint, and a line where its tab would go

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class TabButton; // which drags its view to the other area

    // A tab's content: the view, while the tab shows (so that clearing the tabs never hides a view the other area shows).
    struct Holder : public juce::Component
    {
        void resized() override;
    };

    const Panel area;
    const Views views;
    std::vector<ViewTab> tabs; // in the tabs' order
    std::array<Holder, 5> holders;
    juce::TextButton routingButton { "Routing" };
    const CommandBridge* shortcuts = nullptr;
    std::optional<int> dropIndex;

    // A tab's name, and what its tooltip says it shows.
    struct TabText
    {
        juce::String name, about;
    };

    TabText describeTab (ViewTab) const;
    juce::Component& getViewComponent (ViewTab) const;
    int getDropIndexAt (juce::Point<int>) const;
    float getDropLineX (int index) const;
    juce::TabBarButton* createTabButton (const juce::String& name, int index) override;
    void popupMenuClickOnTab (int index, const juce::String& name) override;
    void currentTabChanged (int, const juce::String&) override; // the tab's view moves into its holder
};
} // namespace phazite::ui
