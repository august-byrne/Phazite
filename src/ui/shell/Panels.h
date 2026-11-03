// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's tabbed areas of views (docs/design.md, section 4): two rows, one over the other, each one area or two side by
// side, so at most two by two; by default the whole song in the upper row and the selection, in detail, in the lower one.
// Their views move between them (the owner, 2026-11-01: "the user should be able to drag the mixer tab down into the bottom
// panel, or the piano roll up"; 2026-11-03: "drag a window to one side of the window space and have it tile next to the
// other window horizontally"), as editors' tabs move between groups in VS Code: a tab dragged onto another area's tabs or
// middle goes there, onto its left or right edge beside it, and onto its edge toward a row without views into that row; its
// right-click menu moves it too, and the settings keep where each view is. An area left without views hides, and so does a
// row. The views belong to the window; the areas only show them.
#pragma once

#include "PanelLayout.h"
#include "model/arrangement/PlaylistModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
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

// An area: its row, the upper one (the panel PanelLayout calls Panel::song) or the lower one (Panel::editor), and its column
// in the row, the left (0) or the right (1).
struct ViewPlace
{
    Panel row = Panel::song;
    int column = 0;

    bool operator== (const ViewPlace&) const = default;
};

// Which views each area holds, in their tabs' order.
struct ViewPlaces
{
    // A row's areas, left to right: the right one without views while the row has one area.
    using Row = std::array<std::vector<View>, 2>;

    Row upper { std::vector { View::playlist, View::mixer }, std::vector<View> {} };
    Row lower { std::vector { View::clipEditor, View::devices }, std::vector<View> {} };

    ViewPlace getPlace (View) const;
    const Row& getRow (Panel) const;
    const std::vector<View>& getViews (ViewPlace) const;
    bool isEmpty (Panel row) const;       // without views: it takes no room
    bool hasRoomBeside (Panel row) const; // one area at most, so that another can go beside it

    // Moves a view into an area, before the view at an index there (after its views for -1, or an index past them); or into
    // an area of its own beside a row's one, left or right of it (into the row, if it is empty; into the area on that side,
    // if the row has two already).
    void move (View, ViewPlace, int index = -1);
    void placeBeside (View, Panel row, bool left);

    // As the settings keep them: the rows split by ";" and each row's areas by "|", "playlist|mixer;clipEditor,devices".
    // Anything but every view once reads as where they start.
    juce::String toString() const;
    static ViewPlaces fromString (const juce::String&);

    bool operator== (const ViewPlaces&) const = default;

private:
    Row& getRow (Panel);
    void tidy(); // in each row, a lone area on the left
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
// One of the areas: a tab for each view it holds, one showing at a time.
class ViewArea : public juce::TabbedComponent,
                 public juce::DragAndDropTarget
{
public:
    // The views, which any area can show.
    struct Views
    {
        juce::Component& playlist;
        juce::Component& mixer;
        ClipEditorView& clipEditor;
        DevicesView& devices;
    };

    ViewArea (ViewPlace, const Views&);
    ~ViewArea() override;

    ViewPlace getPlace() const noexcept { return place; }
    Panel getArea() const noexcept { return place.row; } // its row

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

    // Where a view dragged over it can go besides its tabs (the shell says, as the places change): beside it, while its row
    // has room; and into the other row, while that has no views. And how many of the row's panel buttons its tab bar leaves
    // room for in its corner: the row's right area holds them.
    void setDropSides (bool beside, bool otherRow);
    void setCornerButtons (int);

    // A view dragged here, or moved here from a tab's menu: into an area, before the view at an index there, or after them
    // (-1), or beside a row's area, left or right of it; the shell moves it (ViewPlaces). And the tab menu's Reset View Places.
    std::function<void (View, ViewPlace, int index)> onMove;
    std::function<void (View, Panel row, bool left)> onPlaceBeside;
    std::function<void()> onReset;

    // While a view is dragged over it, where it would go: among its tabs, before the view at an index here, or after them
    // (-1); or beside it, or into the other row. For tests.
    enum class Drop
    {
        tabs,
        left,
        right,
        otherRow
    };

    std::optional<Drop> getDrop() const noexcept { return drop; }
    std::optional<int> getDropIndex() const noexcept { return drop == Drop::tabs ? dropIndex : std::nullopt; }

    void resized() override;                           // its tabs leave the panel's buttons their corner
    void paintOverChildren (juce::Graphics&) override; // a view dragged over it: a tint where it would go, and a line among the tabs

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    class TabButton; // which drags its view to another area

    // A tab's content: the view, while the tab shows (so that clearing the tabs never hides a view another area shows).
    struct Holder : public juce::Component
    {
        void resized() override;
    };

    const ViewPlace place;
    const Views views;
    std::vector<ViewTab> tabs; // in the tabs' order
    std::array<Holder, 5> holders;
    const CommandBridge* shortcuts = nullptr;
    bool canGoBeside = false, canMakeRow = false;
    int cornerButtons = 0;
    std::optional<Drop> drop;
    std::optional<int> dropIndex;

    // A tab's name, and what its tooltip says it shows.
    struct TabText
    {
        juce::String name, about;
    };

    TabText describeTab (ViewTab) const;
    juce::Component& getViewComponent (ViewTab) const;
    Drop getDropAt (juce::Point<int>) const;
    int getDropIndexAt (juce::Point<int>) const;
    float getDropLineX (int index) const;
    juce::TabBarButton* createTabButton (const juce::String& name, int index) override;
    void popupMenuClickOnTab (int index, const juce::String& name) override;
    void currentTabChanged (int, const juce::String&) override; // the tab's view moves into its holder
};

//==============================================================================
// A row of areas: its one area, or two side by side with the line between them, which a drag moves (each area at least
// Shell::minimumAreaWidth wide). The panel buttons go in the corner of its right area.
class ViewRow : public juce::Component
{
public:
    ViewRow (Panel row, const ViewArea::Views&);

    Panel getRow() const noexcept { return row; }
    ViewArea& getArea (int column) noexcept { return *areas[(size_t) juce::jlimit (0, 1, column)]; }

    void setViews (const ViewPlaces::Row&);
    bool isEmpty() const noexcept;
    bool holds (ViewTab) const;
    ViewArea& getAreaOf (ViewTab); // the one that holds it, or the left one

    // The left area's part of the row's width while it has two, which the line's drag changes; the settings keep it.
    double share = 0.5;
    std::function<void()> onShareChanged;

    void resized() override;

private:
    const Panel row;
    std::array<std::unique_ptr<ViewArea>, 2> areas;
    PanelDivider divider;
    double shareAtDragStart = 0.5;
};
} // namespace phazite::ui
