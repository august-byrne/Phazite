// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser (docs/design.md, section 11; roadmap step 1.5): favorites, recent items, Phazite Sounds (or, until they
// are downloaded, a row that downloads them), the instruments and effects, and the user's own folders, in a tree as in
// FL Studio's browser; or, while the search field has text, what search finds in all of them. Clicking a sound, or
// moving to it with the arrow keys, plays it (a loop in time with the song). Dragging an item puts it into the song:
// onto the playlist (a track, or empty space for a new one) or a mixer channel. So do Enter and a double-click: a sound
// onto the selected track at the play start, a device onto the selected track.
#pragma once

#include "Controls.h"
#include "Icons.h"
#include "model/Library.h"
#include "model/Preview.h"
#include "model/SoundPack.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <tuple>

namespace phazite::ui
{
class Browser : public juce::Component,
                public juce::DragAndDropContainer,
                private juce::TextEditor::Listener,
                private juce::ChangeListener,
                private juce::Timer
{
public:
    // The library's factory sounds are the sound pack's.
    Browser (Library&, Previewer&, SoundPack&, std::function<te::Edit*()> getEdit);
    ~Browser() override;

    // Puts an item into the song (Enter, a double-click, or the plug-in picker). Returns whether it went in.
    std::function<bool (const LibraryItem&)> onPlace;

    // Shows the welcome window's Phazite Sounds page: after a download failed, or where Phazite can't download.
    std::function<void()> onShowSoundPack;

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
    std::function<te::Edit*()> getEdit;
    bool soundPackInstalled = false; // when the tree last showed it

    SearchField searchField;
    std::unique_ptr<Tree> tree;
    std::unique_ptr<RootNode> placesRoot, searchRoot;
    IconButton previewButton { "Preview sounds when selected", Icon::preview };
    Fader volumeFader { Fader::Direction::horizontal, "Preview volume", te::decibelsToVolumeFaderPosition (-6.0f) };
    std::unique_ptr<juce::FileChooser> folderChooser;
    juce::Rectangle<int> statusArea; // under the tree: what plays
    bool wasPlaying = false;
    const void* justSelected = nullptr; // the row the click being handled selected, if it did

    // Icons are drawn often, so each is read once for each color it is drawn in.
    std::map<std::tuple<Icon, bool, juce::uint32>, std::unique_ptr<juce::Drawable>> icons;
    void drawIcon (juce::Graphics&, Icon, juce::Rectangle<float>, juce::Colour, bool filled = false);

    te::Edit* getProject() const { return getEdit != nullptr ? getEdit() : nullptr; }
    void rebuildPlaces();
    void refreshPlace (int kind); // a PlaceNode::Kind, after what it lists changed
    void updateSearch();
    void focusTree();
    void showMenu (const LibraryItem&, bool canRemoveFolder);
    void toggleFavorite (const LibraryItem&);
    void noteUsed (const LibraryItem&);
    bool isPreviewing (const LibraryItem&) const;
    bool isPreviewOn() const;
    juce::String getStatus() const;

    void textEditorTextChanged (juce::TextEditor&) override { updateSearch(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { focusTree(); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the sound pack's download went on
    void timerCallback() override;
};
} // namespace phazite::ui
