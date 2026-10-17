// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser (docs/design.md, section 11; roadmap step 1.5): favorites, recent items, Phazite Sounds (or, until they
// are downloaded, a row that downloads them), the instruments and effects, the user's presets and own folders, in a tree
// as in FL Studio's browser; or, while the search field has text, what search finds in all of them. Clicking a sound,
// or moving to it with the arrow keys, plays it (a loop in time with the song). Dragging an item puts it into the song:
// onto the playlist (a track, or empty space for a new one) or a mixer channel. So do Enter and a double-click: a sound
// onto the selected track at the play start, a device or a preset onto the selected track. A track dragged in from the
// playlist is saved as a preset (roadmap step 2.4), as are the selected device and track by their commands.
#pragma once

#include "model/commands/CommandRegistry.h"
#include "model/engine/Preview.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "services/BrowserModel.h"
#include "ui/components/Controls.h"
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

    // Plays a sound or a MIDI file, unless previews are off; stops the preview for anything else.
    void preview (const LibraryItem&);

    // Puts the keyboard focus in the search field, with its text selected.
    void focusSearch();

    // Asks for a folder to add to the browser.
    void chooseFolderToAdd();

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void lookAndFeelChanged() override;

    // For tests.
    juce::TreeView& getTree() noexcept;
    juce::TextEditor& getSearchField() noexcept { return searchField; }
    std::optional<LibraryItem> getSelectedItem() const;
    void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;

    static constexpr int rowHeight = 26;

private:
    struct Node;
    struct ItemNode;
    struct PlaceNode;
    struct MessageNode;
    struct SoundPackNode;
    struct RootNode;
    struct Tree;

    Library& library;
    Previewer& previewer;
    SoundPack& soundPack;
    Settings& settings;
    const ProjectContext& projectContext;
    BrowserModel model;              // what the tree shows
    bool soundPackInstalled = false; // when the tree last showed it

    SearchField searchField;
    std::unique_ptr<Tree> tree;
    std::unique_ptr<RootNode> placesRoot, searchRoot;
    IconButton previewButton { "Preview sounds when selected", Icon::preview };
    Fader volumeFader { Fader::Direction::horizontal, "Preview volume", decibelsToFaderPosition (-6.0f) };
    std::unique_ptr<juce::FileChooser> folderChooser, presetChooser;
    juce::Rectangle<int> statusArea; // under the tree: what plays
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
    void showMenu (const LibraryItem&, bool canRemoveFolder);
    void toggleFavorite (const LibraryItem&);
    void noteUsed (const LibraryItem&);
    bool isPreviewing (const LibraryItem&) const;
    bool isPreviewOn() const;

    void textEditorTextChanged (juce::TextEditor&) override { updateSearch(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { focusTree(); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the sound pack's download went on
    void timerCallback() override;
};
} // namespace phazite::ui
