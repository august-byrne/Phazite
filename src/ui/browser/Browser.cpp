// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Browser.h"
#include "model/mixer/ContainerModel.h"
#include "model/notes/NoteValues.h"
#include "ui/components/ValueText.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

#include <utility>

namespace phazite::ui
{
namespace
{
    constexpr int iconSize = 16;
    constexpr int starWidth = 22;
    constexpr int statusHeight = 20;
    constexpr int soundViewHeight = 64; // a waveform, and a line of what the sound is

    // How deep a row is: 0 for the first level (the hidden root's rows).
    int getDepth (const juce::TreeViewItem& item)
    {
        auto depth = -1;

        for (auto* parent = item.getParentItem(); parent != nullptr; parent = parent->getParentItem())
            ++depth;

        return juce::jmax (0, depth);
    }

    // Opens a row, and each folder in it and in those, as their rows are made.
    void openWithFolders (juce::TreeViewItem& row)
    {
        row.setOpen (true);

        for (int i = 0; i < row.getNumSubItems(); ++i)
            if (auto* inside = row.getSubItem (i); inside->mightContainSubItems())
                openWithFolders (*inside);
    }

    Icon getIconFor (const LibraryItem& item)
    {
        switch (item.kind)
        {
            case LibraryItem::Kind::folder:
                return Icon::folder;
            case LibraryItem::Kind::midi:
                return Icon::midi;
            case LibraryItem::Kind::instrument:
                return Icon::instrument;
            case LibraryItem::Kind::effect:
                return Icon::effect;
            case LibraryItem::Kind::preset: // as what it holds: an instrument, an effect or a container, or a track's sound
                if (item.preset.has_value() && item.preset->kind == PresetKind::track)
                    return Icon::sound;

                return item.preset.has_value() && item.preset->isInstrument ? Icon::instrument : Icon::effect;
            case LibraryItem::Kind::sound:
                break;
        }

        return Icon::sound;
    }
} // namespace

//==============================================================================
// A row of the tree.
struct Browser::Node : public juce::TreeViewItem
{
    explicit Node (Browser& b)
        : browser (b)
    {
        setDrawsInLeftMargin (true); // its row's wash, and its levels' guides, from the tree's left edge
    }

    virtual juce::String getText() const = 0;
    virtual juce::String getDetail() const { return {}; } // after its name, muted: a sound's key and tempo, a place's count
    virtual std::optional<Icon> getIcon() const { return std::nullopt; }
    virtual const LibraryItem* getItem() const { return nullptr; } // a sound, MIDI file, device or folder
    virtual void populate() {}                                     // adds the rows under it, as it opens
    virtual void clicked() {}
    virtual void activate() {} // Enter, or a double-click
    virtual void showMenu() {}

    bool mightContainSubItems() override { return false; }
    int getItemHeight() const override { return rowHeight; }

    void itemOpennessChanged (bool isNowOpen) override
    {
        // Filled each time it opens, so that it shows what is there now.
        clearSubItems();

        if (isNowOpen)
            populate();
    }

    void itemSelectionChanged (bool isNowSelected) override
    {
        // The sound view follows, once the tree is done changing (a selection can change as rows go).
        juce::MessageManager::callAsync (
            [safeBrowser = juce::Component::SafePointer<Browser> (&browser)]
            {
                if (safeBrowser != nullptr)
                    safeBrowser->selectionMoved();
            });

        if (! isNowSelected)
            return;

        // Until the click that selected it, if a click did, has been handled.
        browser.justSelected = this;
        juce::MessageManager::callAsync (
            [safeBrowser = juce::Component::SafePointer<Browser> (&browser)]
            {
                if (safeBrowser != nullptr)
                    safeBrowser->justSelected = nullptr;
            });
    }

    void itemClicked (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showMenu();
            return;
        }

        if (e.getNumberOfClicks() > 1)
            return; // a double-click's second click: itemDoubleClicked() follows

        // The star: a favorite's, or that of a row selected before this click (so that a click on a row's far right doesn't
        // star it by accident).
        if (const auto* item = getItem(); item != nullptr && e.x >= getItemPosition (false).getWidth() - starWidth - Space::xs
                                          && (browser.justSelected != this || browser.library.isFavorite (*item)))
        {
            browser.toggleFavorite (*item);
            return;
        }

        // A folder opens with one click, as in FL Studio's browser.
        if (mightContainSubItems())
            setOpen (! isOpen());

        clicked();
    }

    void itemDoubleClicked (const juce::MouseEvent&) override
    {
        if (! mightContainSubItems()) // a folder opened with the first click
            activate();
    }

    juce::var getDragSourceDescription() override
    {
        if (const auto* item = getItem(); item != nullptr && BrowserModel::canPlace (*item))
            return toDragDescription (*item);

        return {};
    }

    juce::String getAccessibilityName() override
    {
        auto name = getText();

        if (const auto detail = getDetail(); detail.isNotEmpty())
            name << ", " << detail;

        if (const auto* item = getItem(); item != nullptr && browser.library.isFavorite (*item))
            name << ", favorite";

        return name;
    }

    void paintItem (juce::Graphics& g, int width, int height) override
    {
        const auto* owner = getOwnerView();

        if (owner == nullptr)
            return;

        const auto& p = getPalette (*owner);
        const auto* item = getItem();
        const auto focused = owner->hasKeyboardFocus (true);
        const auto depth = getDepth (*this);

        // The whole row, from the tree's left edge (it draws in its left margin, its own level's indent), as VS Code's rows:
        // the selected one in the selection's wash, while the tree has the keys, else in the inactive selection's, outlined
        // in the focus colour while the keys moved to it (not after a click); the one under the pointer in the hover wash.
        const auto row = juce::Rectangle<int> (-depth * SideBar::indent, 0, width + depth * SideBar::indent, height)
                             .toFloat()
                             .reduced (ListRow::inset, 0.0f);

        if (isSelected())
        {
            g.setColour (focused ? p.selected : p.selectedInactive);
            g.fillRoundedRectangle (row, cornerRadius);

            if (focused && browser.keysMovedSelection)
            {
                g.setColour (p.focus);
                g.drawRoundedRectangle (row.reduced (0.5f), cornerRadius, 1.0f);
            }
        }
        else if (getRowNumberInTree() == browser.hoveredRow)
        {
            g.setColour (p.hover);
            g.fillRoundedRectangle (row, cornerRadius);
        }

        paintGuides (g, depth, height);

        const auto textColour = isSelected() ? p.textBright : p.text;
        const auto mutedColour = p.textMuted;
        auto area = juce::Rectangle<int> (width, height).withTrimmedLeft (SideBar::rowChevron).withTrimmedRight ((int) ListRow::inset);

        // Its chevron, a folder's, pointing down while it is open; a row without one leaves its column empty, so that the
        // icons of a level line up.
        const auto chevron = area.removeFromLeft (SideBar::chevronSize).toFloat();

        if (mightContainSubItems())
            browser.drawIcon (g, isOpen() ? Icon::expanded : Icon::folded,
                              chevron.withSizeKeepingCentre ((float) SideBar::chevronSize, (float) SideBar::chevronSize), p.text);

        area.removeFromLeft (SideBar::gap);

        // Its icon: in the focus colour while it plays.
        if (const auto icon = getIcon())
        {
            const auto playing = item != nullptr && browser.isPreviewing (*item);
            browser.drawIcon (g, *icon, area.removeFromLeft (iconSize).toFloat().withSizeKeepingCentre ((float) iconSize, (float) iconSize),
                              playing ? p.focus : mutedColour);
            area.removeFromLeft (SideBar::gap);
        }

        if (item != nullptr)
        {
            // A favorite's star, filled, and the selected row's, to click.
            const auto favorite = browser.library.isFavorite (*item);
            const auto star = area.removeFromRight (starWidth);

            if (favorite || isSelected())
                browser.drawIcon (g, Icon::favorite, star.toFloat().withSizeKeepingCentre (14.0f, 14.0f),
                                  favorite ? p.warning : mutedColour, favorite);
        }

        if (const auto detail = getDetail(); detail.isNotEmpty())
        {
            const juce::Font font { juce::FontOptions (TextSize::small) };
            g.setFont (font);
            g.setColour (mutedColour);
            const auto detailWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, detail)) + Space::s;
            g.drawText (detail, area.removeFromRight (detailWidth), juce::Justification::centredRight, false);
        }

        // While a swap is open, what it can't try is muted (folders lead to what it can).
        const auto unmuted =
            item == nullptr || ! browser.isSwapping() || item->kind == LibraryItem::Kind::folder || browser.canTryInSwap (*item);
        g.setFont (juce::FontOptions (TextSize::body));
        g.setColour (canBeSelected() && unmuted ? textColour : mutedColour);
        g.drawText (getText(), area, juce::Justification::centredLeft, true);
    }

    // A line down each level the row is in, under the chevron of the folder it is in, as VS Code draws its trees' indent
    // guides: the selected row's folder's (and an open folder's own, while it is selected) always; the others only while the
    // pointer is over the tree, fainter.
    void paintGuides (juce::Graphics& g, int depth, int height)
    {
        const auto& p = getPalette (*getOwnerView());
        auto* selectedRow = getOwnerView()->getSelectedItem (0);
        const auto* selectedFolder = selectedRow == nullptr                                         ? nullptr
                                     : selectedRow->isOpen() && selectedRow->mightContainSubItems() ? selectedRow
                                                                                                    : selectedRow->getParentItem();
        auto level = depth;

        for (const auto* folder = getParentItem(); folder != nullptr && --level >= 0; folder = folder->getParentItem())
        {
            const auto active = folder == selectedFolder;

            if (! active && ! browser.pointerOverTree)
                continue;

            // In the row's own coordinates, which start at its level's indent.
            const auto x = SideBar::rowChevron + SideBar::chevronSize / 2 + (level - depth) * SideBar::indent;
            g.setColour (active ? p.indentGuide : p.indentGuide.withMultipliedAlpha (ListRow::inactiveGuide));
            g.fillRect (x, 0, 1, height);
        }
    }

    Browser& browser;
};

//==============================================================================
// A line of text that can't be selected: a hint, or that nothing was found.
struct Browser::MessageNode : public Node
{
    MessageNode (Browser& b, juce::String message)
        : Node (b),
          text (std::move (message))
    {
    }

    juce::String getText() const override { return text; }
    bool canBeSelected() const override { return false; }
    juce::String getUniqueName() const override { return "message:" + text; }
    juce::String getTooltip() override { return text; } // for a message too long for the row

    const juce::String text;
};

//==============================================================================
// Phazite Sounds, until they are installed: a row that downloads them, then shows how far the download is.
struct Browser::SoundPackNode : public Node
{
    using Node::Node;

    juce::String getText() const override { return browser.model.getSoundPackText(); }
    std::optional<Icon> getIcon() const override { return Icon::download; }
    juce::String getUniqueName() const override { return "soundPack"; }

    // A click downloads them; while they download, or after a download failed, it shows the details.
    void clicked() override
    {
        if (browser.model.canStartSoundPackDownload())
            browser.soundPack.download(); // whose change messages repaint the row
        else if (browser.onShowSoundPack != nullptr)
            browser.onShowSoundPack();
    }

    // Enter, or a double-click, whose first click has started the download.
    void activate() override
    {
        if (! browser.model.isSoundPackBusy())
            clicked();
    }

    juce::String getTooltip() override { return browser.model.getSoundPackTooltip(); }
};

//==============================================================================
// A sound, MIDI file, device or folder.
struct Browser::ItemNode : public Node
{
    ItemNode (Browser& b, LibraryItem i)
        : Node (b),
          item (std::move (i))
    {
    }

    juce::String getText() const override { return item.name; }
    juce::String getDetail() const override { return BrowserModel::getDetail (item); }
    std::optional<Icon> getIcon() const override { return getIconFor (item); }
    const LibraryItem* getItem() const override { return &item; }
    bool mightContainSubItems() override { return item.kind == LibraryItem::Kind::folder; }
    juce::String getUniqueName() const override { return item.getKey(); }
    void populate() override { browser.addRows (*this, browser.model.getContents (item)); }
    void clicked() override { browser.togglePreview (item); }

    void activate() override
    {
        if (BrowserModel::canPlace (item))
            browser.place (item);
    }

    void showMenu() override { browser.showMenu (item, false); }
    juce::String getTooltip() override { return BrowserModel::getTooltip (item); }

    const LibraryItem item;
};

//==============================================================================
// A place in the tree, which lists sounds or devices: at the top, or a collection under "Collections"; while filters are
// set, with how many of its sounds have their values.
struct Browser::PlaceNode : public Node
{
    PlaceNode (Browser& b, BrowserPlace p)
        : Node (b),
          place (std::move (p))
    {
    }

    juce::String getText() const override { return place.name; }
    juce::String getDetail() const override { return place.count >= 0 ? juce::String (place.count) : juce::String(); }

    std::optional<Icon> getIcon() const override
    {
        switch (place.kind)
        {
            case BrowserPlace::Kind::favorites:
                return Icon::favorite;
            case BrowserPlace::Kind::recent:
                return Icon::recent;
            case BrowserPlace::Kind::instruments:
                return Icon::instrument;
            case BrowserPlace::Kind::effects:
                return Icon::effect;
            case BrowserPlace::Kind::addFolder:
                return Icon::addFolder;
            case BrowserPlace::Kind::newCollection:
                return Icon::plus;
            case BrowserPlace::Kind::collections: // a folder of sets, not the browser's own icon
            case BrowserPlace::Kind::sounds:
            case BrowserPlace::Kind::folder:
            case BrowserPlace::Kind::factoryPresets:
            case BrowserPlace::Kind::presets: // a folder of the user's presets
            case BrowserPlace::Kind::collection:
                break;
        }

        return Icon::folder;
    }

    bool mightContainSubItems() override { return place.holdsItems(); }
    juce::String getUniqueName() const override { return "place:" + place.getKey(); }
    void populate() override { browser.addRows (*this, browser.model.getContents (place, browser.filters)); }

    void clicked() override
    {
        if (place.kind == BrowserPlace::Kind::addFolder)
            browser.chooseFolderToAdd();
        else if (place.kind == BrowserPlace::Kind::newCollection)
            browser.newCollection (std::nullopt);
    }

    void activate() override { clicked(); }

    void showMenu() override
    {
        if (auto menu = browser.getPlaceMenu (place); menu.getNumItems() > 0)
            menu.showMenuAsync (juce::PopupMenu::Options());
    }

    juce::String getTooltip() override { return place.getTooltip(); }

    const BrowserPlace place;
};

//==============================================================================
// The tree's hidden root: the places, or the search results.
struct Browser::RootNode : public Node
{
    explicit RootNode (Browser& b)
        : Node (b)
    {
    }

    juce::String getText() const override { return {}; }
    bool mightContainSubItems() override { return true; }
    juce::String getUniqueName() const override { return "root"; }
    void itemOpennessChanged (bool) override {} // its rows are set from outside, and stay
};

//==============================================================================
struct Browser::Tree : public juce::TreeView
{
    explicit Tree (Browser& b)
        : browser (b)
    {
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        // The keys in use: the selection's outline shows.
        if (! std::exchange (browser.keysMovedSelection, true))
            repaint();

        auto* before = dynamic_cast<Node*> (getSelectedItem (0));

        // Enter puts an item into the song; on a folder, it opens or closes it.
        if (key == juce::KeyPress::returnKey && before != nullptr && ! before->mightContainSubItems())
        {
            before->activate();
            return true;
        }

        // Shift+Enter, or Right on a sound, plays it again, as in Ableton Live.
        const auto replays = key == juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::shiftModifier, 0)
                             || (key == juce::KeyPress::rightKey && before != nullptr && ! before->mightContainSubItems());

        if (replays && before != nullptr)
        {
            if (const auto* item = before->getItem())
                browser.preview (*item);

            return true;
        }

        // Esc puts back what a swap tried; else it stops a preview (and does what it does elsewhere when none plays).
        if (key == juce::KeyPress::escapeKey && browser.isSwapping())
        {
            browser.putBackSwap();
            return true;
        }

        if (key == juce::KeyPress::escapeKey && browser.previewer.isPlaying())
        {
            browser.previewer.stop();
            repaint();
            return true;
        }

        // Down with nothing selected goes to the first row that can be (swapping, the first it can try), and plays it, or
        // tries it.
        if (key == juce::KeyPress::downKey && before == nullptr)
        {
            if (auto* first = browser.findFirstRow())
            {
                first->setSelected (true, true);
                scrollToKeepItemVisible (first);

                if (const auto* item = first->getItem())
                    browser.preview (*item);

                return true;
            }
        }

        if (! juce::TreeView::keyPressed (key))
            return false;

        // Moving to a sound plays it.
        if (auto* after = dynamic_cast<Node*> (getSelectedItem (0)); after != nullptr && after != before)
            if (const auto* item = after->getItem())
                browser.preview (*item);

        return true;
    }

    void focusGained (FocusChangeType cause) override
    {
        // Given the keys by the keys (Tab, or Down from the search), the selection's outline shows; by a click, not.
        if (cause != focusChangedByMouseClick)
            browser.keysMovedSelection = true;

        repaint();
    }

    void focusLost (FocusChangeType) override { repaint(); }

    Browser& browser;
};

//==============================================================================
// Hears the pointer over the tree and its rows: the row it is on, and whether it is over the tree at all; and a press, after
// which the selection's outline hides until the keys are used again.
struct Browser::TreeHover : public juce::MouseListener
{
    explicit TreeHover (Browser& b)
        : browser (b)
    {
        browser.tree->addMouseListener (this, true);
    }

    ~TreeHover() override { browser.tree->removeMouseListener (this); }

    void mouseEnter (const juce::MouseEvent& e) override { update (e); }
    void mouseMove (const juce::MouseEvent& e) override { update (e); }
    void mouseExit (const juce::MouseEvent& e) override { update (e); }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (std::exchange (browser.keysMovedSelection, false))
            browser.tree->repaint();
    }

    void update (const juce::MouseEvent& e)
    {
        auto& view = *browser.tree;
        const auto position = e.getEventRelativeTo (&view).getPosition();
        const auto over = view.getLocalBounds().contains (position) && view.isMouseOver (true);
        const auto* item = over ? view.getItemAt (position.y) : nullptr;
        const auto row = item != nullptr ? item->getRowNumberInTree() : -1;

        if (over != browser.pointerOverTree || row != browser.hoveredRow)
        {
            browser.pointerOverTree = over;
            browser.hoveredRow = row;
            view.repaint();
        }
    }

    Browser& browser;
};

//==============================================================================
// Over the search while a swap is open: what it swaps, and what is in its place now; then Keep and Put Back.
struct Browser::SwapStrip : public juce::Component
{
    explicit SwapStrip (Browser& b)
        : browser (b)
    {
        setTitle ("Swap");
        keepButton.setTooltip ("Keeps what is in its place now, as one step to undo (Enter)");
        keepButton.onClick = [this] { browser.keepSwap(); };
        putBackButton.setTooltip ("Puts back what was there before (Esc)");
        putBackButton.onClick = [this] { browser.putBackSwap(); };
        addAndMakeVisible (keepButton);
        addAndMakeVisible (putBackButton);
    }

    // "Swap Pad 1", and "Kick 03 in place of Kick", or what to do while nothing is tried yet.
    juce::String getTitleText() const
    {
        const auto* open = browser.swap.get();
        return open != nullptr ? "Swap " + open->getTargetName() : juce::String();
    }

    juce::String getStatusText() const
    {
        const auto* open = browser.swap.get();

        if (open == nullptr)
            return {};

        const auto tried = open->getTried().getFileNameWithoutExtension();
        const auto old = open->getOldName();
        const auto what = open->getPad() >= 0 ? juce::String ("sound") : juce::String ("preset");

        if (tried.isEmpty())
            return "Move to a " + what + " to try it" + (old.isNotEmpty() ? " in place of " + old : juce::String());

        return old.isNotEmpty() ? tried + " in place of " + old : tried;
    }

    void update()
    {
        setDescription (getTitleText() + ": " + getStatusText());
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat();
        g.setColour (p.accent.withAlpha (SwapTint::fill));
        g.fillRoundedRectangle (area, cornerRadius);
        g.setColour (p.accent);
        g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);

        // Its title, bold, then what is in place, muted.
        auto line = getLocalBounds()
                        .reduced (SwapTint::inset, 0)
                        .withTrimmedTop (SwapTint::inset)
                        .withHeight (juce::roundToInt (TextSize::body) + Space::xs);
        const juce::Font bold { juce::FontOptions (TextSize::small, juce::Font::bold) };
        const auto titleText = getTitleText().toUpperCase();
        const auto titleWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (bold, titleText)) + Space::s;
        g.setFont (bold);
        g.setColour (p.text);
        g.drawText (titleText, line.removeFromLeft (titleWidth), juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (p.textMuted);
        g.drawText (getStatusText(), line, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto row = getLocalBounds().reduced (SwapTint::inset).removeFromBottom (SwapTint::buttonHeight);
        keepButton.setBounds (row.removeFromLeft (SwapTint::buttonWidth));
        row.removeFromLeft (Space::xs);
        putBackButton.setBounds (row.removeFromLeft (SwapTint::buttonWidth));
    }

    Browser& browser;
    juce::TextButton keepButton { "Keep" }, putBackButton { "Put Back" };
};

//==============================================================================
// The field that names a collection, over its row: Enter, or a click elsewhere, names it; Esc doesn't.
struct Browser::NameField : public juce::TextEditor
{
    std::function<void (const juce::String&)> done;
    bool finished = false;
};

//==============================================================================
Browser::Browser (Library& l, Previewer& p, SoundPack& pack, Settings& s, const ProjectContext& context)
    : library (l),
      previewer (p),
      soundPack (pack),
      settings (s),
      projectContext (context),
      model (l, pack),
      soundPackInstalled (pack.isInstalled()),
      tree (std::make_unique<Tree> (*this)),
      searchRoot (std::make_unique<RootNode> (*this))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Browser");

    // Its icon gives it the keys (and a screen reader its name), not its search field, so that Space plays on; Tab goes on into
    // its parts.
    setWantsKeyboardFocus (true);

    searchField.setTitle ("Search sounds, instruments and effects");
    searchField.setTooltip ("Type to find sounds, presets, instruments and effects, the best matches first; Down or Enter goes to them");
    searchField.setFont (juce::FontOptions (TextSize::body));
    searchField.addListener (this);
    searchDelay.search = [this] { updateSearch(); };
    searchField.onNavigationKey = [this] (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress::downKey)
            return false;

        focusTree();
        return true;
    };

    // Each level so far in as VS Code's trees; the rows draw their own chevrons, in its sizes (Node::paintItem()).
    tree->setTitle ("Sounds, instruments and effects");
    tree->setRootItemVisible (false);
    tree->setIndentSize (SideBar::indent);
    tree->setOpenCloseButtonsVisible (false);
    tree->setDefaultOpenness (false);
    tree->setMultiSelectEnabled (false);
    tree->setColour (juce::TreeView::selectedItemBackgroundColourId, juce::Colours::transparentBlack); // the rows paint it
    tree->getViewport()->setWantsKeyboardFocus (false); // the tree takes the keys itself: one stop for Tab, not two

    previewButton.setClickingTogglesState (true);
    previewButton.setToggleState (isPreviewOn(), juce::dontSendNotification);
    previewButton.setTooltip ("Plays each sound as you click it or move to it with the arrow keys; Esc stops it");
    previewButton.onClick = [this]
    {
        settings.set (Settings::browserPreview, previewButton.getToggleState());

        if (! previewButton.getToggleState())
            previewer.stop();

        tree->repaint();
        repaint (statusArea);
    };

    showAsDecibels (volumeFader);
    volumeFader.setValue (settings.get (Settings::browserPreviewVolume, previewer.getVolume()), juce::dontSendNotification);
    volumeFader.setTooltip ("How loud sounds play while you browse. Click it and type a level, or double-click for -6 dB");
    volumeFader.onValueChange = [this]
    {
        previewer.setVolume ((float) volumeFader.getValue());
        settings.set (Settings::browserPreviewVolume, volumeFader.getValue());
    };
    previewer.setVolume ((float) volumeFader.getValue());

    // The filters under the search, what each offers as the search and the others leave it, and Clear.
    filterBar.getOptions = [this] { return model.getFilterOptions (searchField.getText(), filters); };
    filterBar.onChange = [this] (const BrowserFilters& changed) { setFilters (changed); };
    filterBar.onClear = [this] { clearFiltersAndSearch(); };

    swapStrip = std::make_unique<SwapStrip> (*this);
    addChildComponent (*swapStrip); // while a swap is open

    for (auto* c : std::initializer_list<juce::Component*> { &title, &searchField, &filterBar, tree.get(), &previewButton, &volumeFader })
        addAndMakeVisible (c);

    treeHover = std::make_unique<TreeHover> (*this);

    addChildComponent (soundView); // while a sound is selected

    // Search finds more once the index of the sounds is ready, and the filters count what it found.
    library.onIndexChanged = [this]
    {
        if (searchField.getText().trim().isNotEmpty())
            updateSearch();
        else if (! filters.isEmpty())
            rebuildPlaces();
    };

    soundPack.addChangeListener (this);
    rebuildPlaces();
    lookAndFeelChanged();
}

Browser::~Browser()
{
    soundPack.removeChangeListener (this);
    library.onIndexChanged = nullptr;

    // What a swap tried stays, as anything else that changes the song leaves it.
    if (swap != nullptr && swap->isOpen())
        swap->keep();

    // The folders open now open again next time: as they were without the filters, if any are set.
    if (const auto openness = opennessUnfiltered != nullptr ? std::move (opennessUnfiltered) : placesRoot->getOpennessState())
        settings.set (Settings::browserOpenness, openness->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

    if (nameField != nullptr)
        nameField->finished = true; // nothing is named as it goes

    tree->setRootItem (nullptr);
}

juce::TreeView& Browser::getTree() noexcept
{
    return *tree;
}

//==============================================================================
void Browser::addRows (juce::TreeViewItem& parent, const BrowserContents& contents)
{
    if (contents.offersSoundPack)
        parent.addSubItem (new SoundPackNode (*this));

    for (const auto& place : contents.places)
        parent.addSubItem (new PlaceNode (*this, place));

    for (const auto& item : contents.items)
        parent.addSubItem (new ItemNode (*this, item));

    for (const auto& message : contents.messages)
        parent.addSubItem (new MessageNode (*this, message));
}

void Browser::rebuildPlaces()
{
    // Folders open now stay open; at first, those open when Phazite last closed, or else Phazite's sounds.
    std::unique_ptr<juce::XmlElement> openness;

    if (placesRoot != nullptr)
        openness = placesRoot->getOpennessState();
    else
        openness = juce::parseXML (settings.get (Settings::browserOpenness));

    auto root = std::make_unique<RootNode> (*this);
    PlaceNode* sounds = nullptr;

    for (auto& place : model.getPlaces (filters))
    {
        const auto isSounds = place.kind == BrowserPlace::Kind::sounds;
        auto* node = new PlaceNode (*this, std::move (place));
        root->addSubItem (node);

        if (isSounds)
            sounds = node;
    }

    const auto showing = placesRoot == nullptr || tree->getRootItem() == placesRoot.get();

    if (showing)
        tree->setRootItem (nullptr);

    placesRoot = std::move (root);

    if (openness != nullptr)
        placesRoot->restoreOpennessState (*openness);
    else if (sounds != nullptr)
        sounds->setOpen (true);

    if (showing && searchField.getText().trim().isEmpty())
        tree->setRootItem (placesRoot.get());
}

void Browser::refreshPlace (BrowserPlace::Kind kind)
{
    // Later: what asked may be one of the rows this replaces, such as a favorite unstarred in Favorites.
    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<Browser> (this), kind]
        {
            if (safeThis == nullptr || safeThis->placesRoot == nullptr)
                return;

            // While filters are set, every place's count may have changed with it.
            if (! safeThis->filters.isEmpty())
            {
                safeThis->rebuildPlaces();
                return;
            }

            // Its folders open now stay open.
            const auto reopen = [kind] (juce::TreeViewItem* item)
            {
                if (auto* place = dynamic_cast<PlaceNode*> (item); place != nullptr && place->place.kind == kind && place->isOpen())
                {
                    const auto openness = place->getOpennessState();
                    place->setOpen (false);
                    place->setOpen (true);

                    if (openness != nullptr)
                        place->restoreOpennessState (*openness);
                }
            };

            // The places at the top, and the collections under "Collections".
            auto& root = *safeThis->placesRoot;

            for (int i = 0; i < root.getNumSubItems(); ++i)
            {
                auto* place = root.getSubItem (i);
                reopen (place);

                if (auto* holder = dynamic_cast<PlaceNode*> (place);
                    holder != nullptr && holder->place.kind == BrowserPlace::Kind::collections)
                    for (int j = 0; j < holder->getNumSubItems(); ++j)
                        reopen (holder->getSubItem (j));
            }
        });
}

void Browser::textEditorTextChanged (juce::TextEditor&)
{
    if (lastSearchMilliseconds > slowSearchMilliseconds && searchField.getText().trim().isNotEmpty())
        searchDelay.startTimer (150);
    else
        updateSearch();

    showFilters(); // Clear, while the search has words
}

void Browser::showFilters()
{
    const auto before = filterBar.getHeight();
    filterBar.show (filters, searchField.getText().trim().isNotEmpty());

    if (filterBar.getHeightForWidth (filterBar.getWidth()) != before)
        resized();
}

void Browser::setFilters (const BrowserFilters& changed)
{
    if (changed == filters)
        return;

    // The folders open without filters, kept while they are set, open again once they are all taken away.
    if (filters.isEmpty() && ! changed.isEmpty() && placesRoot != nullptr)
        opennessUnfiltered = placesRoot->getOpennessState();

    filters = changed;
    showFilters();
    rebuildPlaces();

    if (filters.isEmpty() && opennessUnfiltered != nullptr)
    {
        placesRoot->restoreOpennessState (*opennessUnfiltered);
        opennessUnfiltered = nullptr;
    }

    if (searchField.getText().trim().isNotEmpty())
        updateSearch();

    selectionMoved();
}

void Browser::clearFiltersAndSearch()
{
    searchField.setText ({}, false);
    setFilters ({});
    updateSearch();
    showFilters();
}

void Browser::updateSearch()
{
    searchDelay.stopTimer();
    const auto query = searchField.getText().trim();

    // The places; or while a device's presets are swapped, they alone.
    if (query.isEmpty())
    {
        tree->setRootItem (swapRoot != nullptr ? swapRoot.get() : placesRoot.get());
        return;
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();
    searchRoot->clearSubItems();
    addRows (*searchRoot, model.search (query, filters));
    lastSearchMilliseconds = juce::Time::getMillisecondCounterHiRes() - started;

    tree->setRootItem (searchRoot.get());
}

void Browser::focusSearch()
{
    searchField.grabKeyboardFocus();
    searchField.selectAll();
}

void Browser::focusTree()
{
    // The first row that can be selected (swapping, the first it can try), unless one is.
    if (tree->getNumSelectedItems() == 0)
    {
        if (auto* first = findFirstRow())
        {
            first->setSelected (true, true);
            tree->scrollToKeepItemVisible (first);
        }
    }

    tree->grabKeyboardFocus();

    if (auto* node = dynamic_cast<Node*> (tree->getSelectedItem (0)))
        if (const auto* item = node->getItem())
            preview (*item);
}

Browser::Node* Browser::findFirstRow() const
{
    Node* first = nullptr;

    for (int row = 0; row < tree->getNumRowsInTree(); ++row)
    {
        auto* node = dynamic_cast<Node*> (tree->getItemOnRow (row));

        if (node == nullptr || ! node->canBeSelected())
            continue;

        if (! isSwapping())
            return node;

        if (const auto* item = node->getItem(); item != nullptr && canTryInSwap (*item))
            return node;

        if (first == nullptr)
            first = node;
    }

    return first;
}

void Browser::textEditorEscapeKeyPressed (juce::TextEditor&)
{
    if (isSwapping())
        putBackSwap();
    else
        searchField.setText ({}, true);
}

void Browser::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Once Phazite Sounds are installed, their place lists them, and search and the filters find them.
    if (const auto installed = soundPack.isInstalled(); installed != soundPackInstalled)
    {
        soundPackInstalled = installed;
        library.refresh();
        refreshPlace (BrowserPlace::Kind::sounds);
    }

    tree->repaint(); // the download's row
}

//==============================================================================
bool Browser::place (const LibraryItem& item)
{
    // Swapping, Enter or a double-click keeps it, tried first if it wasn't yet.
    if (isSwapping())
    {
        if (! canTryInSwap (item))
            return false;

        if (swap->getTried() != item.file)
            swap->tryFile (item.file);

        noteUsed (item);
        keepSwap();
        return true;
    }

    if (onPlace == nullptr || ! onPlace (item))
        return false;

    noteUsed (item);
    return true;
}

void Browser::noteUsed (const LibraryItem& item)
{
    library.noteUsed (item);
    refreshPlace (BrowserPlace::Kind::recent);

    if (onUsed != nullptr)
        onUsed (item);
}

void Browser::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails& details)
{
    // Dropped where it went into the song.
    if (wasPlaced (details.description))
        if (const auto item = library.findItem (getDraggedKey (details.description)))
            noteUsed (*item);
}

void Browser::preview (const LibraryItem& item)
{
    // Swapping, it plays in place instead, through the track, whether previews are on or not: that is the swap.
    if (isSwapping())
    {
        previewer.stop();

        if (canTryInSwap (item))
            swap->tryFile (item.file);

        swapStrip->update();
    }
    else if (BrowserModel::canPreview (item) && isPreviewOn())
    {
        previewer.play (item.file, projectContext, BrowserModel::previewsInTime (item));
    }
    else
    {
        previewer.stop();
    }

    wasPlaying = previewer.isPlaying();
    tree->repaint();
    repaint (statusArea);
}

void Browser::togglePreview (const LibraryItem& item)
{
    // A one-shot plays again from its start, as clicked again and again to hear it; a loop, which plays until something
    // stops it, stops. Swapping, a click tries it again.
    if (isSwapping() || ! isPreviewing (item) || ! previewer.isInTime())
    {
        preview (item);
        return;
    }

    previewer.stop();
    wasPlaying = false;
    tree->repaint();
    repaint (statusArea);
}

bool Browser::isPreviewing (const LibraryItem& item) const
{
    if (isSwapping())
        return item.isFile() && item.file == swap->getTried();

    return item.isFile() && item.file == previewer.getFile() && previewer.isPlaying();
}

bool Browser::isPreviewOn() const
{
    return settings.get (Settings::browserPreview);
}

void Browser::toggleFavorite (const LibraryItem& item)
{
    library.setFavorite (item, ! library.isFavorite (item));
    refreshPlace (BrowserPlace::Kind::favorites);
    tree->repaint();
}

void Browser::showMenu (const LibraryItem& item, bool canRemoveFolder)
{
    getItemMenu (item, canRemoveFolder).showMenuAsync (juce::PopupMenu::Options());
}

juce::PopupMenu Browser::getItemMenu (const LibraryItem& item, bool canRemoveFolder)
{
    const juce::Component::SafePointer<Browser> safeThis (this);
    juce::PopupMenu menu;

    if (BrowserModel::canPlace (item) && ! isSwapping())
        menu.addItem ("Add to the Song",
                      [safeThis, item]
                      {
                          if (safeThis != nullptr)
                              safeThis->place (item);
                      });

    menu.addItem (library.isFavorite (item) ? "Remove from Favorites" : "Add to Favorites",
                  [safeThis, item]
                  {
                      if (safeThis != nullptr)
                          safeThis->toggleFavorite (item);
                  });

    // Its collections, each ticked while it is in it (choosing one again takes it out), and a new one with it in it.
    juce::PopupMenu collections;

    for (const auto& name : library.getCollections())
        collections.addItem (name, true, library.isInCollection (item, name),
                             [safeThis, item, name]
                             {
                                 if (safeThis == nullptr)
                                     return;

                                 auto& lib = safeThis->library;
                                 lib.setInCollection (item, name, ! lib.isInCollection (item, name));
                                 safeThis->refreshPlace (BrowserPlace::Kind::collection);
                             });

    if (collections.getNumItems() > 0)
        collections.addSeparator();

    collections.addItem ("New Collection...",
                         [safeThis, item]
                         {
                             if (safeThis != nullptr)
                                 safeThis->newCollection (item);
                         });
    menu.addSubMenu ("Add to Collection", collections);

    if (item.isFile())
        menu.addItem (getShowInFolderText(), [file = item.file] { file.revealToUser(); });

    if (canRemoveFolder)
        menu.addItem ("Remove from the Browser",
                      [safeThis, folder = item.file]
                      {
                          if (safeThis == nullptr)
                              return;

                          safeThis->library.removeUserFolder (folder);
                          safeThis->rebuildPlaces();
                      });

    return menu;
}

juce::PopupMenu Browser::getPlaceMenu (const BrowserPlace& place)
{
    const juce::Component::SafePointer<Browser> safeThis (this);

    // A collection's: its name typed anew over its row, or it deleted, once asked (its sounds stay where they are).
    if (place.kind == BrowserPlace::Kind::collection)
    {
        juce::PopupMenu menu;
        menu.addItem ("Rename...",
                      [safeThis, name = place.name]
                      {
                          if (safeThis == nullptr)
                              return;

                          const juce::TreeViewItem* row = nullptr;
                          auto& places = *safeThis->tree;

                          for (int i = 0; i < places.getNumRowsInTree() && row == nullptr; ++i)
                              if (auto* node = dynamic_cast<PlaceNode*> (places.getItemOnRow (i));
                                  node != nullptr && node->place.kind == BrowserPlace::Kind::collection && node->place.name == name)
                                  row = node;

                          safeThis->askForName (row, name,
                                                [safeThis, name] (const juce::String& newName)
                                                {
                                                    if (safeThis != nullptr && safeThis->library.renameCollection (name, newName))
                                                        safeThis->refreshPlace (BrowserPlace::Kind::collections);
                                                });
                      });
        menu.addItem ("Delete Collection",
                      [safeThis, name = place.name]
                      {
                          if (safeThis == nullptr)
                              return;

                          juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                            .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                                            .withTitle ("Delete the collection " + name + "?")
                                                            .withMessage ("Its sounds, presets and devices stay where they are.")
                                                            .withButton ("Delete", 1)
                                                            .withButton ("Cancel", 0)
                                                            .withAssociatedComponent (safeThis.getComponent()),
                                                        [safeThis, name] (int chosen)
                                                        {
                                                            if (safeThis == nullptr || chosen != 1)
                                                                return;

                                                            safeThis->library.removeCollection (name);
                                                            safeThis->refreshPlace (BrowserPlace::Kind::collections);
                                                        });
                      });
        return menu;
    }

    // A folder's, Phazite Sounds' and the presets' folder's, as the items' are; the user's folders can go from the browser.
    const auto isFolder = place.kind == BrowserPlace::Kind::folder;
    const auto isPresets = place.kind == BrowserPlace::Kind::presets && place.folder.isDirectory();

    if (isFolder || isPresets || (place.kind == BrowserPlace::Kind::sounds && soundPack.isInstalled()))
        return getItemMenu (library.makeFileItem (place.folder, place.folder), isFolder);

    return {};
}

void Browser::askForName (const juce::TreeViewItem* row, const juce::String& name, std::function<void (const juce::String&)> done)
{
    nameField = std::make_unique<NameField>();
    auto& field = *nameField;
    field.setTitle ("Name");
    field.setFont (juce::FontOptions (TextSize::body));
    field.setText (name, false);
    field.done = std::move (done);

    // Named once: Enter, or the keys going elsewhere, names it; Esc doesn't. The field goes after the key or the click that
    // ended it.
    const juce::Component::SafePointer<Browser> safeThis (this);
    const auto finish = [safeThis] (bool named)
    {
        if (safeThis == nullptr || safeThis->nameField == nullptr || std::exchange (safeThis->nameField->finished, true))
            return;

        const auto text = safeThis->nameField->getText().trim();
        auto then = safeThis->nameField->done;
        safeThis->nameField->setVisible (false);
        juce::MessageManager::callAsync (
            [safeThis]
            {
                if (safeThis != nullptr && safeThis->nameField != nullptr && safeThis->nameField->finished)
                    safeThis->nameField = nullptr;
            });

        if (named && text.isNotEmpty() && then != nullptr)
            then (text);

        if (safeThis != nullptr)
            safeThis->tree->grabKeyboardFocus();
    };

    field.onReturnKey = [finish] { finish (true); };
    field.onEscapeKey = [finish] { finish (false); };
    field.onFocusLost = [finish] { finish (true); };

    // Over its row, or the tree's first while it has none.
    auto area = tree->getBounds().withHeight (rowHeight);

    if (row != nullptr)
        area = row->getItemPosition (true).withHeight (row->getItemHeight()) + tree->getPosition();

    addAndMakeVisible (field);
    field.setBounds (area.withTrimmedLeft (SideBar::rowChevron + SideBar::chevronSize));
    field.grabKeyboardFocus();
    field.selectAll();
}

void Browser::newCollection (std::optional<LibraryItem> toAdd)
{
    // Over its row in Collections, if that is open; else at the tree's top.
    const juce::TreeViewItem* row = nullptr;

    for (int i = 0; i < tree->getNumRowsInTree() && row == nullptr; ++i)
        if (auto* node = dynamic_cast<PlaceNode*> (tree->getItemOnRow (i));
            node != nullptr && node->place.kind == BrowserPlace::Kind::newCollection)
            row = node;

    const juce::Component::SafePointer<Browser> safeThis (this);
    askForName (row, "New Collection",
                [safeThis, toAdd] (const juce::String& name)
                {
                    if (safeThis == nullptr)
                        return;

                    const auto made = safeThis->library.addCollection (name);

                    if (made.isNotEmpty() && toAdd.has_value())
                        safeThis->library.setInCollection (*toAdd, made, true);

                    safeThis->refreshPlace (BrowserPlace::Kind::collections);
                });
}

//==============================================================================
bool Browser::isSwapping() const
{
    return swap != nullptr && swap->isOpen();
}

bool Browser::canTryInSwap (const LibraryItem& item) const
{
    if (! isSwapping() || ! item.isFile() || item.kind == LibraryItem::Kind::folder)
        return false;

    const auto [known, isNew] = triable.try_emplace (item.file.getFullPathName(), false);

    if (isNew)
        known->second = swap->canTry (item.file);

    return known->second;
}

juce::Component& Browser::getSwapStrip()
{
    return *swapStrip;
}

juce::TextEditor* Browser::getNameField() noexcept
{
    return nameField.get();
}

void Browser::startSwap (DeviceID device, int pad)
{
    // Asked again for what it swaps, it keeps what is in its place (a Swap button clicked again); asked for another, it keeps
    // what it tried first, as anything else does.
    const auto same = isSwapping() && swap->getDevice() == device && swap->getPad() == pad;

    if (swap != nullptr)
    {
        if (swap->isOpen())
            swap->keep();

        endSwap();
    }

    endedSwapRoot = nullptr;

    if (same)
        return;

    auto& context = const_cast<ProjectContext&> (projectContext); // trying something in place changes the open project
    swap = std::make_unique<Swap> (context, device, pad);
    triable.clear();

    if (! swap->isOpen())
    {
        swap = nullptr;
        return;
    }

    previewer.stop();
    treeBeforeSwap = tree->getOpennessState (false); // with the row selected
    scrollBeforeSwap = tree->getViewport()->getViewPositionY();
    searchBeforeSwap = searchField.getText();
    filtersBeforeSwap = filters;
    searchField.setText ({}, false);

    // What fits, from the tree's top: a pad's sounds of its sound's type, where there are some, by the filters; a device's
    // presets alone, every folder of them open, as Ableton Live's Hot-Swap opens the device's folder, and with nothing
    // else around them to get lost in (they are made once the filters are gone, which would hide them).
    BrowserFilters fitting;

    if (pad >= 0)
    {
        // An empty pad's, the drum General MIDI puts on its note.
        const auto old = swap->getOldName();
        const auto types = getSoundTypesIn (old.isNotEmpty() || pad < 35 || pad > 81 ? old : getDrumName (pad));
        const auto options = model.getFilterOptions ({}, {});

        for (const auto& [type, count] : options.types)
            if (fitting.type.isEmpty() && types.contains (type) && count > 0)
                fitting.type = type;
    }

    setFilters (fitting);

    if (pad < 0)
        swapRoot = makeSwapRoot (device);

    updateSearch();
    scrollTreeTo (0);
    tree->clearSelectedItems();
    selectionMoved();
    filterBar.setVisible (swapRoot == nullptr); // presets have none of the filters' values, which would hide them
    swapStrip->update();
    swapStrip->setVisible (true);
    resized();

    if (onSwapStarted != nullptr)
        onSwapStarted();

    tree->grabKeyboardFocus();
    tree->repaint();
}

void Browser::keepSwap()
{
    if (swap != nullptr)
        swap->keep();

    endSwap();
}

void Browser::putBackSwap()
{
    if (swap != nullptr)
        swap->putBack();

    endSwap();
}

void Browser::endSwap()
{
    if (swap == nullptr)
        return;

    swap = nullptr; // puts back what is tried, unless it was kept
    swapStrip->setVisible (false);

    // As the browser was before it: its search and its filters, and its tree as it was, its folders open, the row selected
    // and how far it was scrolled; a device's presets gone from it, their rows kept until the next swap, as it can end in
    // one of their own callbacks (Enter or a double-click on a preset keeps it).
    endedSwapRoot = std::move (swapRoot);
    searchField.setText (searchBeforeSwap, false);
    setFilters (filtersBeforeSwap);
    updateSearch();

    if (const auto before = std::move (treeBeforeSwap))
        tree->restoreOpennessState (*before, true);

    scrollTreeTo (scrollBeforeSwap);
    selectionMoved();
    filterBar.setVisible (true);
    showFilters();
    resized();
    tree->repaint();
    repaint (statusArea);
}

void Browser::layOutTreeNow()
{
    // JUCE's tree lays its rows out on the next message after they change, and a scroll before then goes by where they were,
    // or is lost while the old rows fit in view: given its root again, it lays them out at once.
    if (auto* root = tree->getRootItem())
    {
        tree->setRootItem (nullptr);
        tree->setRootItem (root);
    }
}

void Browser::scrollTreeTo (int y)
{
    layOutTreeNow();
    tree->getViewport()->setViewPosition (0, y);
}

std::unique_ptr<Browser::RootNode> Browser::makeSwapRoot (DeviceID device)
{
    auto root = std::make_unique<RootNode> (*this);
    const auto places = model.getPresetPlaces (getPresetFolderName (projectContext, device));

    for (const auto& place : places)
    {
        auto* node = new PlaceNode (*this, place);
        root->addSubItem (node);
        openWithFolders (*node); // so that every preset is there for Down to go to
    }

    if (root->getNumSubItems() == 0)
        root->addSubItem (new MessageNode (*this, "No presets for " + swap->getTargetName() + " yet"));

    return root;
}

void Browser::chooseFolderToAdd()
{
    folderChooser = std::make_unique<juce::FileChooser> ("Add a folder of sounds to the browser",
                                                         juce::File::getSpecialLocation (juce::File::userMusicDirectory));

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [safeThis = juce::Component::SafePointer<Browser> (this)] (const juce::FileChooser& chooser)
                                {
                                    const auto chosen = chooser.getResult();

                                    if (safeThis == nullptr || ! chosen.isDirectory())
                                        return;

                                    safeThis->library.addUserFolder (chosen);
                                    safeThis->rebuildPlaces();
                                });
}

//==============================================================================
void Browser::addCommands (CommandRegistry& registry)
{
    registry.add ({ .id = "device.savePreset",
                    .name = "Save Device Preset...",
                    .category = "Device",
                    .description = "Saves the selected device's settings as a preset, to use again in any project: a container's with "
                                   "its devices and macros.",
                    .synonyms = { "preset", "save settings", "patch", "store", "rack preset", "fx chain" },
                    .perform = [this] { saveSelectedDevicePreset(); },
                    .isEnabled = [this] { return canSaveDevicePreset(); } });

    registry.add ({ .id = "track.savePreset",
                    .name = "Save Track Preset...",
                    .category = "Track",
                    .description = "Saves the selected track's devices, mix and modulators as a preset, which makes a new track with "
                                   "them in any project. Dragging a track into the browser does too.",
                    .synonyms = { "preset", "template", "channel strip", "patch", "store" },
                    .perform = [this] { saveSelectedTrackPreset(); },
                    .isEnabled = [this] { return canSaveTrackPreset(); } });
}

bool Browser::canSaveDevicePreset() const
{
    return projectContext.getSongSelection().getDevices().size() == 1;
}

bool Browser::canSaveTrackPreset() const
{
    const auto tracks = projectContext.getSongSelection().getTracks();
    const auto channel = tracks.size() == 1 ? getChannel (projectContext, tracks.front()) : std::nullopt;
    return channel.has_value() && channel->kind == ChannelKind::track; // not a folder's or a return's
}

void Browser::saveSelectedDevicePreset()
{
    if (! canSaveDevicePreset())
        return;

    // In its device's folder, or Containers, named after it.
    const auto device = projectContext.getSongSelection().getDevices().front();
    const auto folder = getPresetFolderName (projectContext, device);
    const auto container = getContainerName (projectContext, device);
    choosePresetFile (folder, container.isNotEmpty() ? container : folder,
                      [this, device] (const juce::File& file) { return saveDevicePreset (projectContext, device, file); });
}

void Browser::saveSelectedTrackPreset()
{
    if (! canSaveTrackPreset())
        return;

    const auto track = projectContext.getSongSelection().getTracks().front();
    choosePresetFile (trackPresetsFolder, getChannel (projectContext, track)->name,
                      [this, track] (const juce::File& file) { return phazite::saveTrackPreset (projectContext, track, file); });
}

void Browser::choosePresetFile (const juce::String& folder, const juce::String& name, std::function<bool (const juce::File&)> save)
{
    askWhereToSavePreset (presetChooser, library.getPresetsFolder().getChildFile (folder), name,
                          [safeThis = juce::Component::SafePointer<Browser> (this), onChosen = std::move (save)] (const juce::File& file)
                          {
                              if (safeThis != nullptr && onChosen (file))
                                  safeThis->presetSaved (file);
                          });
}

void Browser::showDevicePresetMenu (DeviceID device, const juce::String& name, juce::Component& target)
{
    const auto folder = getPresetFolderName (projectContext, device);

    if (folder.isEmpty())
        return;

    auto& context = const_cast<ProjectContext&> (projectContext); // loading into the device changes the open project
    const juce::Component::SafePointer<Browser> safeThis (this);
    auto presets = makeDevicePresetMenu (library, folder, name,
                                         [&context, device] (const juce::File& file) { loadPresetInto (context, device, file); });

    // First, one of Phazite's own devices' presets tried in place from the browser, each heard as the browser moves to it.
    juce::PopupMenu menu;

    if (getDeviceHandle (context, device).isValid())
    {
        menu.addItem ("Swap: Try Them in Place",
                      [safeThis, device]
                      {
                          if (safeThis != nullptr)
                              safeThis->startSwap (device);
                      });
        menu.addSeparator();
    }

    for (juce::PopupMenu::MenuItemIterator item (presets); item.next();)
        menu.addItem (item.getItem());

    if (presets.getNumItems() == 0)
        menu.addItem ("No presets for " + name + " yet", false, false, nullptr);

    menu.addSeparator();
    menu.addItem ("Save Preset...",
                  [safeThis, device, folder, name]
                  {
                      if (safeThis != nullptr)
                          safeThis->choosePresetFile (
                              folder, name, [safeThis, device] (const juce::File& file)
                              { return safeThis != nullptr && saveDevicePreset (safeThis->projectContext, device, file); });
                  });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

namespace
{
    // The presets for a device in a folder and the folders in it, each a submenu.
    juce::PopupMenu makePresetFolderMenu (const Library& library, const juce::File& folder, const juce::String& deviceName,
                                          const std::function<void (const juce::File&)>& load)
    {
        juce::PopupMenu menu;

        for (const auto& item : library.listFolder (folder))
        {
            if (item.kind == LibraryItem::Kind::folder)
            {
                if (auto inside = makePresetFolderMenu (library, item.file, deviceName, load); inside.getNumItems() > 0)
                    menu.addSubMenu (item.name, inside);
            }
            else if (item.preset.has_value() && item.preset->kind == PresetKind::device && item.preset->device == deviceName)
            {
                menu.addItem (item.name, [load, file = item.file] { load (file); });
            }
        }

        return menu;
    }
} // namespace

juce::PopupMenu makeDevicePresetMenu (const Library& library, const juce::String& folderName, const juce::String& deviceName,
                                      std::function<void (const juce::File&)> load)
{
    juce::PopupMenu menu;
    const auto factory = makePresetFolderMenu (library, library.getFactoryPresetsFolder().getChildFile (folderName), deviceName, load);
    const auto own = makePresetFolderMenu (library, library.getPresetsFolder().getChildFile (folderName), deviceName, load);

    // Phazite's by kind of sound, each a submenu, as their folders are; then the user's.
    for (juce::PopupMenu::MenuItemIterator item (factory); item.next();)
        menu.addItem (item.getItem());

    if (own.getNumItems() > 0)
    {
        menu.addSectionHeader ("Your Presets");

        for (juce::PopupMenu::MenuItemIterator item (own); item.next();)
            menu.addItem (item.getItem());
    }

    return menu;
}

void askWhereToSavePreset (std::unique_ptr<juce::FileChooser>& chooser, const juce::File& folder, const juce::String& name,
                           std::function<void (const juce::File&)> save)
{
    folder.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save a Preset",
                                                   folder.getChildFile (juce::File::createLegalFileName (name) + presetExtension),
                                                   "*" + juce::String (presetExtension));

    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [onChosen = std::move (save)] (const juce::FileChooser& dialog)
                          {
                              if (const auto chosen = dialog.getResult(); chosen != juce::File())
                                  onChosen (chosen.withFileExtension (presetExtension));
                          });
}

juce::File Browser::saveTrackPreset (TrackID track)
{
    const auto channel = getChannel (projectContext, track);

    if (! channel.has_value() || channel->kind != ChannelKind::track)
        return {};

    const auto folder = library.getPresetsFolder().getChildFile (trackPresetsFolder);
    folder.createDirectory();
    const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (channel->name), presetExtension, true);

    if (! phazite::saveTrackPreset (projectContext, track, file))
        return {};

    presetSaved (file);
    return file;
}

void Browser::presetSaved (const juce::File& file)
{
    // Search finds it, and Presets shows it, selected.
    library.notePresetSaved();

    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<Browser> (this), file]
        {
            if (safeThis != nullptr)
                safeThis->reveal (BrowserPlace::Kind::presets, file);
        });
}

void Browser::reveal (BrowserPlace::Kind kind, const juce::File& file)
{
    if (placesRoot == nullptr)
        return;

    // The place, listed anew with its folders open as they were, and opened down to the file.
    for (int i = 0; i < placesRoot->getNumSubItems(); ++i)
    {
        auto* place = dynamic_cast<PlaceNode*> (placesRoot->getSubItem (i));

        if (place == nullptr || place->place.kind != kind)
            continue;

        const auto openness = place->getOpennessState();
        place->setOpen (false);
        place->setOpen (true);

        if (openness != nullptr)
            place->restoreOpennessState (*openness);

        for (juce::TreeViewItem* parent = place; parent != nullptr;)
        {
            juce::TreeViewItem* next = nullptr;

            for (int j = 0; j < parent->getNumSubItems() && next == nullptr; ++j)
            {
                auto* node = dynamic_cast<Node*> (parent->getSubItem (j));
                const auto* item = node != nullptr ? node->getItem() : nullptr;

                if (item == nullptr || (item->file != file && ! file.isAChildOf (item->file)))
                    continue;

                if (item->file == file)
                {
                    node->setSelected (true, true);
                    layOutTreeNow(); // the folders just opened
                    tree->scrollToKeepItemVisible (node);
                    return;
                }

                node->setOpen (true);
                next = node;
            }

            parent = next;
        }
    }
}

bool Browser::isInterestedInDragSource (const SourceDetails& details)
{
    return projectContext.hasProject() && getDraggedTrack (details.description).has_value();
}

void Browser::itemDragEnter (const SourceDetails&)
{
    trackDropping = true;
    repaint();
}

void Browser::itemDragExit (const SourceDetails&)
{
    trackDropping = false;
    repaint();
}

void Browser::itemDropped (const SourceDetails& details)
{
    itemDragExit (details);

    if (const auto track = getDraggedTrack (details.description))
        saveTrackPreset (*track);
}

//==============================================================================
std::optional<LibraryItem> Browser::getSelectedItem() const
{
    if (auto* node = dynamic_cast<Node*> (tree->getSelectedItem (0)))
        if (const auto* item = node->getItem())
            return *item;

    return std::nullopt;
}

void Browser::drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour, bool filled)
{
    auto& drawable = icons[{ icon, filled, colour.getARGB() }];

    if (drawable == nullptr)
    {
        drawable = createIcon (icon, filled);

        if (drawable == nullptr)
            return;

        drawable->replaceColour (juce::Colours::black, colour);
    }

    drawable->drawWithin (g, area, juce::RectanglePlacement::centred, 1.0f);
}

void Browser::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (isSwapping() ? juce::String ("Swapping: Enter keeps it, Esc puts the old one back")
                             : BrowserModel::getPreviewStatus (isPreviewOn(), previewer),
                statusArea, juce::Justification::centredLeft, true);

    // A track about to be saved as a preset, as a mixer channel shows a device about to be dropped on it.
    if (trackDropping)
    {
        g.setColour (p.accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), cornerRadius, 2.0f);
    }
}

void Browser::resized()
{
    // As VS Code's side bar lays out a view (SideBar): its title; the search in a row of its own, as its search view's field;
    // the tree as wide as the panel, its rows inset; and the fields and buttons at the foot from the panel's sides.
    auto area = getLocalBounds();
    const auto fromSides = [] (juce::Rectangle<int> row)
    { return row.withTrimmedLeft (SideBar::fieldLeft).withTrimmedRight (SideBar::fieldRight); };

    title.setBounds (area.removeFromTop (SideBar::titleHeight));

    // While a swap is open, what it swaps, over the search.
    if (swapStrip->isVisible())
    {
        swapStrip->setBounds (fromSides (area.removeFromTop (SwapTint::height)));
        area.removeFromTop (Space::xs);
    }

    const auto search = fromSides (area.removeFromTop (SideBar::inputRowHeight));
    searchField.setBounds (search.withSizeKeepingCentre (search.getWidth(), SideBar::controlHeight));

    // The filters under it, in as many rows as they need, while they show.
    if (filterBar.isVisible())
    {
        const auto filtersWidth = fromSides (area).getWidth();
        filterBar.setBounds (fromSides (area.removeFromTop (filterBar.getHeightForWidth (filtersWidth))));
        area.removeFromTop (Space::xs);
    }

    auto footer = fromSides (area.removeFromBottom (SideBar::buttonRowHeight));
    footer = footer.withSizeKeepingCentre (footer.getWidth(), SideBar::controlHeight);
    previewButton.setBounds (footer.removeFromLeft (SideBar::controlHeight));
    footer.removeFromLeft (Space::s);
    volumeFader.setBounds (footer);

    statusArea = fromSides (area.removeFromBottom (statusHeight));

    // The sound selected, under the tree, while one is.
    if (soundView.showsSound())
    {
        area.removeFromBottom (Space::xs);
        soundView.setBounds (fromSides (area.removeFromBottom (soundViewHeight)));
    }

    area.removeFromBottom (Space::xs);
    tree->setBounds (area);
}

void Browser::selectionMoved()
{
    if (tree == nullptr)
        return;

    const auto before = soundView.showsSound();
    soundView.show (getSelectedItem());
    soundView.setVisible (soundView.showsSound());

    if (soundView.showsSound() != before)
        resized();
}

void Browser::visibilityChanged()
{
    // A hidden browser plays nothing, and swaps nothing: what a swap tried stays, as anything else leaves it.
    if (isVisible())
    {
        startTimerHz (10);
    }
    else
    {
        stopTimer();
        previewer.stop();

        if (isSwapping())
            keepSwap();
    }
}

void Browser::lookAndFeelChanged()
{
    searchField.setTextToShowWhenEmpty ("Search sounds and devices", getPalette (*this).textMuted);
}

void Browser::timerCallback()
{
    // A swap something else ended (a change that kept what it tried, its device gone, another project): the browser as it
    // was before it.
    if (swap != nullptr && ! swap->isOpen())
        endSwap();

    previewer.keepInTime (projectContext);

    // A one-shot ends by itself.
    if (const auto playing = previewer.isPlaying(); playing != wasPlaying)
    {
        wasPlaying = playing;
        tree->repaint();
        repaint (statusArea);
    }
}
} // namespace phazite::ui
