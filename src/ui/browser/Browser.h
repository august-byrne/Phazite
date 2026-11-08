// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser (docs/design.md, section 11; roadmap step 1.5): favorites, recent items, the user's collections, Phazite
// Sounds (or, until they are downloaded, a row that downloads them), the instruments and effects, the user's presets and own
// folders, in a tree as in FL Studio's browser, laid out as VS Code's Explorer under its title (its rows, chevrons, levels
// and their guides); or, while the search field has text, what search finds in all of them. Under the search, the filters
// (FilterBar.h): with any set, each place lists its sounds with their values as one list. Clicking a sound,
// or moving to it with the arrow keys, plays it (a loop in time with the song). Dragging an item puts it into the song:
// onto the playlist (a track, or empty space for a new one) or a mixer channel. So do Enter and a double-click: a sound
// onto the selected track at the play start, a device or a preset onto the selected track. A track dragged in from the
// playlist is saved as a preset (roadmap step 2.4), as are the selected device and track by their commands. While it swaps
// a drum pad's sound or a device's preset (Swap, model/devices/SwapModel.h), what it moves to plays in that place instead.
#pragma once

#include "FilterBar.h"
#include "SoundView.h"
#include "model/commands/CommandRegistry.h"
#include "model/devices/SwapModel.h"
#include "model/engine/Preview.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "services/BrowserModel.h"
#include "ui/components/Controls.h"
#include "ui/components/ViewTitle.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <tuple>

namespace phazite::ui
{
class Browser : public juce::Component,
                public juce::DragAndDropContainer,
                public juce::DragAndDropTarget,
                private juce::TextEditor::Listener,
                private juce::ChangeListener,
                private juce::Timer
{
public:
    // The library's factory sounds are the sound pack's. Loops preview in time with the open project.
    Browser (Library&, Previewer&, SoundPack&, Settings&, const ProjectContext&);
    ~Browser() override;

    // Saving presets of the selected device and the selected track (model/presets/PresetModel.h), each in a dialog that
    // starts in its folder of the presets: "device.savePreset" and "track.savePreset".
    void addCommands (CommandRegistry&);
    bool canSaveDevicePreset() const;
    bool canSaveTrackPreset() const;

    // Saves a preset of a track in the presets' Tracks folder, named after it (with a number if the name is taken), as a
    // track dropped here is; shows it in Presets. Returns the file, or none if nothing was saved.
    juce::File saveTrackPreset (TrackID);

    // A device's presets, as its Presets button in the Devices view shows them: Phazite's for it, by kind of sound, then
    // the user's; the one chosen loads into the device as one undo step (PresetModel.h: loadPresetInto()). And Save
    // Preset..., in a dialog that starts in its folder of the user's presets.
    void showDevicePresetMenu (DeviceID, const juce::String& deviceName, juce::Component& target);

    // A track dragged out of the playlist.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    // Puts an item into the song (Enter, a double-click, or the plug-in picker). Returns whether it went in.
    std::function<bool (const LibraryItem&)> onPlace;

    // Shows the welcome window's Phazite Sounds page: after a download failed, or where Phazite can't download.
    std::function<void()> onShowSoundPack;

    // An item went into the song, placed or dropped there.
    std::function<void (const LibraryItem&)> onUsed;

    // Puts an item into the song with onPlace, and notes it as used, for the recent items.
    bool place (const LibraryItem&);

    // Plays a sound or a MIDI file, unless previews are off; stops the preview for anything else. A click on an item plays
    // it, and stops it if it is a loop playing (togglePreview()).
    void preview (const LibraryItem&);
    void togglePreview (const LibraryItem&);

    // Puts the keyboard focus in the search field, with its text selected.
    void focusSearch();

    // Asks for a folder to add to the browser.
    void chooseFolderToAdd();

    // The filters (FilterBar.h), as the user sets them; and Clear, which takes away the search's words with them.
    void setFilters (const BrowserFilters&);
    const BrowserFilters& getFilters() const noexcept { return filters; }
    void clearFiltersAndSearch();

    // Swap (docs/design.md, section 11; model/devices/SwapModel.h): the sound on a drum sampler's pad, or with pad -1 the
    // preset of one of Phazite's own devices, tried in place as the browser moves to each, heard in the beat; Enter or Keep
    // keeps one as one undo step, Esc or Put Back puts the old one back, and starting another, or anything else that changes
    // the song, keeps what was tried. Meanwhile a strip over the search says what is swapped, previews stay silent, and
    // the tree shows what fits, from its top: a pad's sounds of its type ("Kick", by its sound's name), by the filters; or
    // the device's presets alone, Phazite's and the user's, every folder of them open, so that Down goes from one to the
    // next, the filters hidden (presets have none of their values). Once it ends, the browser is as it was: its search, its
    // filters, its folders open, the row selected and how far it was scrolled. onSwapStarted shows the browser, which then has the keys. Asked again for what it swaps (its Swap
    // button clicked again), it keeps what is in its place.
    void startSwap (DeviceID, int pad = -1);
    bool isSwapping() const;
    void keepSwap();
    void putBackSwap();
    std::function<void()> onSwapStarted;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::TreeView& getTree() noexcept;
    juce::TextEditor& getSearchField() noexcept { return searchField; }
    FilterBar& getFilterBar() noexcept { return filterBar; }
    std::optional<LibraryItem> getSelectedItem() const;
    const SoundView& getSoundView() const noexcept { return soundView; }
    void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;
    const Swap* getSwap() const noexcept { return swap.get(); }
    juce::Component& getSwapStrip();

    // A row's menu, as a right-click shows it: an item's (into the song, favorites, collections, its folder), or a place's
    // (a collection's: rename or delete it). And the field that names a collection, while it shows.
    juce::PopupMenu getItemMenu (const LibraryItem&, bool canRemoveFolder);
    juce::PopupMenu getPlaceMenu (const BrowserPlace&);
    juce::TextEditor* getNameField() noexcept;

    static constexpr int rowHeight = ListRow::height;

private:
    struct Node;
    struct ItemNode;
    struct PlaceNode;
    struct MessageNode;
    struct SoundPackNode;
    struct RootNode;
    struct Tree;
    struct TreeHover;
    struct SwapStrip;
    struct NameField;

    Library& library;
    Previewer& previewer;
    SoundPack& soundPack;
    Settings& settings;
    const ProjectContext& projectContext;
    BrowserModel model;              // what the tree shows
    bool soundPackInstalled = false; // when the tree last showed it

    ViewTitle title { "Browser" };
    SearchField searchField;
    FilterBar filterBar;
    BrowserFilters filters;
    std::unique_ptr<juce::XmlElement> opennessUnfiltered; // the places' folders open before filters were set
    std::unique_ptr<Tree> tree;
    std::unique_ptr<RootNode> placesRoot, searchRoot;
    std::unique_ptr<RootNode> swapRoot;      // while a device's presets are swapped, they alone (makeSwapRoot())
    std::unique_ptr<RootNode> endedSwapRoot; // the last swap's, until the next starts (endSwap())

    // The swap open, the strip that shows it, and the search, the filters and the tree as they were before it (its folders
    // open, the row selected and how far it was scrolled); and which files it can try, by path, as the rows ask at every
    // paint (a preset is read to know).
    std::unique_ptr<Swap> swap;
    std::unique_ptr<SwapStrip> swapStrip;
    juce::String searchBeforeSwap;
    BrowserFilters filtersBeforeSwap;
    std::unique_ptr<juce::XmlElement> treeBeforeSwap;
    int scrollBeforeSwap = 0;
    mutable std::map<juce::String, bool> triable;

    // The field that names a new collection, or renames one, over its row.
    std::unique_ptr<NameField> nameField;

    // The row under the pointer, whether the pointer is over the tree (its levels' guides show then, as VS Code's do), and
    // whether the keys moved the selection last (its outline shows then, not after a click, as in VS Code's Modern UI).
    std::unique_ptr<TreeHover> treeHover;
    int hoveredRow = -1; // its number in the tree
    bool pointerOverTree = false, keysMovedSelection = false;
    IconButton previewButton { "Preview sounds when selected", Icon::preview };
    Fader volumeFader { Fader::Direction::horizontal, "Preview volume", decibelsToFaderPosition (-6.0f) };
    std::unique_ptr<juce::FileChooser> folderChooser, presetChooser;
    SoundView soundView;             // under the tree: the sound selected
    juce::Rectangle<int> statusArea; // under it: what plays
    bool wasPlaying = false;
    bool trackDropping = false;         // a track dragged over it, to save as a preset
    const void* justSelected = nullptr; // the row the click being handled selected, if it did

    // Icons are drawn often, so each is read once for each color it is drawn in.
    std::map<std::tuple<Icon, bool, juce::uint32>, std::unique_ptr<juce::Drawable>> icons;
    void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float>, juce::Colour, bool filled = false);

    void rebuildPlaces();
    void refreshPlace (BrowserPlace::Kind); // after what places of that kind list changed
    void saveSelectedDevicePreset();
    void saveSelectedTrackPreset();
    void choosePresetFile (const juce::String& folder, const juce::String& name, std::function<bool (const juce::File&)> save);
    void presetSaved (const juce::File&);
    void reveal (BrowserPlace::Kind, const juce::File&); // a file in a place: its folders opened, and it selected
    void addRows (juce::TreeViewItem& parent, const BrowserContents&);
    void updateSearch();
    void focusTree();
    Node* findFirstRow() const; // the first row that can be selected; swapping, the first it can try, while one shows
    void layOutTreeNow();       // the tree's rows where they are now, for a scroll to go by (JUCE's tree waits a message)
    void scrollTreeTo (int y);  // the tree scrolled so far down, its rows laid out as they are now
    void showMenu (const LibraryItem&, bool canRemoveFolder);
    void toggleFavorite (const LibraryItem&);
    void noteUsed (const LibraryItem&);
    bool isPreviewing (const LibraryItem&) const;
    bool isPreviewOn() const;
    void selectionMoved(); // the sound view shows the item selected now
    void showFilters();    // the filter bar as the filters and the search are now
    void endSwap();        // once it is kept, put back, or closed by something else: the browser as it was before it
    bool canTryInSwap (const LibraryItem&) const;
    std::unique_ptr<RootNode> makeSwapRoot (DeviceID); // a device's presets, its folders open, or a line that it has none

    // Asks for a collection's name in a field over a row of the tree (from the browser's top while it has none), named as
    // given at first; done gets the name typed, as Enter or a click elsewhere leaves it, unless Esc does.
    void askForName (const juce::TreeViewItem*, const juce::String& name, std::function<void (const juce::String&)> done);
    void newCollection (std::optional<LibraryItem> toAdd); // named as the user types, with an item in it if one is given

    // A search through a big library takes a while: once one has, typing waits for the keys to pause (docs/performance-
    // audit.md, U7). Small ones search at each key.
    struct SearchDelay : juce::Timer
    {
        std::function<void()> search;
        void timerCallback() override
        {
            stopTimer();
            search();
        }
    } searchDelay;
    double lastSearchMilliseconds = 0.0;
    static constexpr double slowSearchMilliseconds = 5.0;

    void textEditorTextChanged (juce::TextEditor&) override;
    void textEditorReturnKeyPressed (juce::TextEditor&) override { focusTree(); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;    // puts back what a swap tried, else empties the search
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the sound pack's download went on
    void timerCallback() override;
};

// A menu of the presets for a device, by its folder's name among the presets (PresetModel.h: getPresetFolderName()) and
// its name as its presets give it: Phazite's, from the factory presets' folder, by kind of sound; then the user's, from
// theirs, with the folders in it as submenus. Choosing one calls load with its file. Empty if there are none.
juce::PopupMenu makeDevicePresetMenu (const Library&, const juce::String& folderName, const juce::String& deviceName,
                                      std::function<void (const juce::File&)> load);

// Asks where to save a preset, in a dialog that starts in a folder of the user's presets (made if it isn't there), with
// the name of what the preset holds. Calls save with the file chosen, with the presets' extension, unless the dialog is
// cancelled or chooser goes meanwhile. The browser's commands use it, and plug-in windows' Save Preset....
void askWhereToSavePreset (std::unique_ptr<juce::FileChooser>& chooser, const juce::File& folder, const juce::String& name,
                           std::function<void (const juce::File&)> save);
} // namespace phazite::ui
