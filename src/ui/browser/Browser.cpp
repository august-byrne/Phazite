// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Browser.h"
#include "devices/core/Clock.h"
#include "model/base/WholeFile.h"
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

    // A row by its identifier string (juce::TreeViewItem::getItemIdentifierString()), under a row, or none.
    juce::TreeViewItem* findRow (juce::TreeViewItem& under, const juce::String& id)
    {
        const auto own = under.getItemIdentifierString();

        if (own == id)
            return &under;

        if (! id.startsWith (own + "/"))
            return nullptr;

        for (int i = 0; i < under.getNumSubItems(); ++i)
            if (auto* found = findRow (*under.getSubItem (i), id))
                return found;

        return nullptr;
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
        ++rowsMade;
    }

    static inline std::atomic<juce::int64> rowsMade { 0 }; // Browser::countRowsMade()

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

    // A folder or a place, which a click opens and Enter too; not a device whose row holds its presets, which is put into the
    // song as any item, and opens by its chevron (or Right).
    virtual bool isFolder() { return mightContainSubItems(); }

    bool isOnChevron (const juce::MouseEvent& e) const { return e.x < SideBar::rowChevron + SideBar::chevronSize + SideBar::gap; }

    ~Node() override
    {
        // What waits for its rows goes with it: a key pressed on it, a reveal.
        if (reading)
            browser.openingRows.erase (this);

        if (browser.queuedKey.row == this)
            browser.dropQueuedKey();
    }

    void itemOpennessChanged (bool isNowOpen) override
    {
        // Filled each time it opens, so that it shows what is there now; a listing still under way stops as it closes, and
        // what waited for it is dropped.
        stopOpening();
        checker = nullptr;
        wantedOpenness = nullptr;
        afterFill.clear();
        clearSubItems();

        if (isNowOpen)
            populate();
    }

    // Its rows as the model opens it (BrowserModel::open()): at once (the index's), then brought up to date in place if its
    // folder, read on a listing thread, holds something else now; or once its folder is read, a line saying so meanwhile.
    // And, shown open, its folder looked at again (BrowserModel::check()): read on a listing thread alone, nothing taken from
    // the index here, its rows brought up to date in place only where the folder differs; at most once in a few seconds.
    using Opening = std::function<std::unique_ptr<Library::FolderListing> (std::function<void (BrowserContents)>)>;
    void openWith (const Opening& open, Opening check);
    void show (BrowserContents);       // what its listing gave: its first rows, or brought up to date in place
    void relist (const Opening& open); // filled, listed again (from the index as it is now), its rows brought up to date
    void lookAgain (bool whateverItsLastLook = false);
    void sayIfLate();                         // its folder still being read, its drive not answering for a few seconds: its line says so
    void filledWith (const BrowserContents&); // its first rows, and what waited for them (openness asked meanwhile, a reveal)

    void stopOpening() // never runs what waits: it may be in the middle of a change of the tree
    {
        listing = nullptr;

        if (std::exchange (reading, false))
            browser.openingRows.erase (this);
    }

    Opening checker;                                  // how it is looked at again, while it is open
    std::unique_ptr<Library::FolderListing> listing;  // while its folder is read
    bool filled = false, reading = false;             // its rows shown; its first rows still to come
    std::unique_ptr<juce::XmlElement> wantedOpenness; // asked of it while it read (Browser::restoreOpenness())
    std::vector<std::function<void()>> afterFill;     // what waits for its first rows (a reveal)
    double checkedAt = 0.0;                           // when it was last looked at again (devices::Clock)
    bool saidLate = false;                            // its line says its drive isn't answering

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

        if (! browser.restoringRows)
            browser.userActed(); // the selection moved: a key or a selection waiting for rows was for another

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
        browser.userActed(); // a key or a selection waiting for rows: the user did something else first

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

        // A folder opens with one click, as in FL Studio's browser; a device's presets by its chevron, as in Ableton Live's.
        if (isFolder() || (mightContainSubItems() && isOnChevron (e)))
            setOpen (! isOpen());

        clicked();
    }

    void itemDoubleClicked (const juce::MouseEvent& e) override
    {
        if (! isFolder() && ! (mightContainSubItems() && isOnChevron (e))) // a folder opened with the first click
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

void Browser::Node::openWith (const Opening& open, Opening check)
{
    checker = std::move (check);
    filled = false;
    saidLate = false;
    checkedAt = devices::Clock::getMilliseconds(); // read as it opens: not again at once
    listing = open ([this] (BrowserContents contents) { show (std::move (contents)); });

    // Its first rows still to come: a line says so.
    if (! filled && listing != nullptr)
    {
        reading = true;
        browser.openingRows.insert (this);
        addSubItem (new MessageNode (browser, "Reading this folder..."));
    }
}

void Browser::Node::show (BrowserContents contents)
{
    // What several folders gave while one of them doesn't answer: its line says so, under them, until the rest comes.
    if (listing != nullptr && listing->isPartial())
        contents.messages.add ("This drive isn't answering...");

    if (filled)
        browser.updateRows (*this, contents); // what changed since the index's rows, in place
    else
        filledWith (contents);
}

void Browser::Node::relist (const Opening& open)
{
    // Listed again in place: what is open and selected in it stays.
    listing = nullptr;
    listing = open ([this] (BrowserContents contents) { show (std::move (contents)); });
}

void Browser::Node::sayIfLate()
{
    if (! reading || saidLate || listing == nullptr || ! listing->isLate())
        return;

    saidLate = true;
    clearSubItems();
    addSubItem (new MessageNode (browser, "This drive isn't answering..."));
}

void Browser::Node::filledWith (const BrowserContents& contents)
{
    filled = true;
    clearSubItems();
    browser.addRows (*this, contents);

    if (std::exchange (reading, false))
        browser.openingRows.erase (this);

    // The folders asked to be open within it while it read, opened now, and their selection.
    if (auto wanted = std::move (wantedOpenness))
        browser.restoreOpenness (*this, *wanted);

    browser.selectWanted();

    // What waited for it (a key pressed on it, a reveal): later, once nothing is changing the tree.
    if (browser.queuedKey.row == this)
        browser.replayQueuedKey();

    for (auto& then : std::exchange (afterFill, {}))
        juce::MessageManager::callAsync (std::move (then));
}

void Browser::Node::lookAgain (bool whateverItsLastLook)
{
    const auto now = devices::Clock::getMilliseconds();

    if (checker == nullptr || ! filled || (! whateverItsLastLook && now - checkedAt < lookAgainMilliseconds))
        return;

    checkedAt = now;
    listing = checker ([this] (BrowserContents contents) { browser.updateRows (*this, contents); });
}

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
// A sound, MIDI file, preset, device or folder. A device's row opens to its presets, while it has some.
struct Browser::ItemNode : public Node
{
    ItemNode (Browser& b, LibraryItem i, int presetsHeld = 0)
        : Node (b),
          item (std::move (i)),
          presets (presetsHeld)
    {
    }

    juce::String getText() const override { return item.name; }
    juce::String getDetail() const override { return BrowserModel::getDetail (item); }
    std::optional<Icon> getIcon() const override { return getIconFor (item); }
    const LibraryItem* getItem() const override { return &item; }
    bool mightContainSubItems() override { return item.kind == LibraryItem::Kind::folder || presets != 0; }
    bool isFolder() override { return item.kind == LibraryItem::Kind::folder; }
    juce::String getUniqueName() const override { return item.getKey(); }

    // How many presets its row holds now (BrowserContents::presets): one that has none any more closes; one open with none
    // before (left open last time) shows them; one open with others before (a preset saved from a plug-in's window, or one
    // that came from outside) lists them again, what is open and selected in it kept.
    void setPresets (int held)
    {
        const auto before = std::exchange (presets, held);

        if (before == held)
            return;

        if (held == 0 && isOpen())
            setOpen (false);
        else if (before == 0 && isOpen())
            populate();
        else if (isOpen() && filled)
            relist ([this] (std::function<void (BrowserContents)> opened) { return browser.model.open (item, std::move (opened)); });

        treeHasChanged();
    }

    void populate() override
    {
        // A device's row with no presets any more, asked to be open (as the browser was left last time): nothing in it.
        if (! mightContainSubItems())
            return;

        openWith ([this] (std::function<void (BrowserContents)> opened) { return browser.model.open (item, std::move (opened)); },
                  [this] (std::function<void (BrowserContents)> opened) { return browser.model.check (item, std::move (opened)); });
    }
    void clicked() override { browser.togglePreview (item); }

    void activate() override
    {
        if (BrowserModel::canPlace (item))
            browser.place (item);
    }

    void showMenu() override { browser.showMenu (item, false); }
    juce::String getTooltip() override { return BrowserModel::getTooltip (item); }

    const LibraryItem item;
    int presets = 0; // a device's: how many presets its row holds, -1 while that isn't known
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
            case BrowserPlace::Kind::instrumentContainers:
                return Icon::instrument;
            case BrowserPlace::Kind::effects:
            case BrowserPlace::Kind::containers:
                return Icon::effect;
            case BrowserPlace::Kind::addFolder:
                return Icon::addFolder;
            case BrowserPlace::Kind::newCollection:
                return Icon::plus;
            case BrowserPlace::Kind::collections: // a folder of sets, not the browser's own icon
            case BrowserPlace::Kind::sounds:
            case BrowserPlace::Kind::trackPresets: // a folder of the user's presets
            case BrowserPlace::Kind::otherPresets:
            case BrowserPlace::Kind::folder:
            case BrowserPlace::Kind::collection:
                break;
        }

        return Icon::folder;
    }

    bool mightContainSubItems() override { return place.holdsItems(); }
    juce::String getUniqueName() const override { return "place:" + place.getKey(); }
    void populate() override
    {
        // With filters, as the places were listed; without, as its folder holds it now.
        if (! browser.listedPlaces.filters.isEmpty())
            browser.addRows (*this, browser.getPlaceContents (place));
        else
            openWith ([this] (std::function<void (BrowserContents)> opened) { return browser.model.open (place, std::move (opened)); },
                      [this] (std::function<void (BrowserContents)> opened) { return browser.model.check (place, std::move (opened)); });
    }

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
    // Its rows are set from outside, and stay, also as layOutTreeNow() gives the tree its root again (which closes and
    // opens it). JUCE workaround W19 (docs/juce-workarounds.md).
    void itemOpennessChanged (bool) override {}
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

        // Moving or Enter on a folder whose rows are still being read, or while the rows to come aren't in (a swap's first
        // places, the newest text's results): on those, once they are, never on the rows they replace; and only if nothing
        // else was done first and they came within a moment (Browser::queueKey()).
        const auto moves =
            key.isKeyCode (juce::KeyPress::downKey) || key.isKeyCode (juce::KeyPress::upKey) || key.isKeyCode (juce::KeyPress::returnKey);

        if (moves)
        {
            if (auto* selected = dynamic_cast<Node*> (getSelectedItem (0)); selected != nullptr && selected->reading)
            {
                browser.queueKey (key, selected);
                return true;
            }

            if (browser.isWaitingForRows())
            {
                browser.queueKey (key, nullptr);
                return true;
            }
        }

        browser.userActed(); // another key acts at once: one waiting is dropped, as is a selection waiting for rows

        auto* before = dynamic_cast<Node*> (getSelectedItem (0));

        // Enter puts an item into the song (a device whose row holds presets too); on a folder, it opens or closes it.
        if (key == juce::KeyPress::returnKey && before != nullptr && ! before->isFolder())
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
    searchField.onNavigationKey = [this] (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress::downKey)
            return false;

        focusTreeAfterSearch(); // to the newest text's results, which are milliseconds away
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
    // The tree takes the keys itself: one stop for Tab, not two, as JUCE's tree and its viewport would each take the keys.
    // JUCE workaround W20 (docs/juce-workarounds.md).
    tree->getViewport()->setWantsKeyboardFocus (false);

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

    // Escape during a drag: the fader shows the level from before, which the previews take again.
    volumeFader.onDragCancelled = [this] { volumeFader.onValueChange(); };
    previewer.setVolume ((float) volumeFader.getValue());

    // The filters under the search, what each offers as the search and the others leave it, counted in the background as a
    // menu is to open, and Clear. What shows changing (the search's words, the filters, the index) cancels a count still
    // under way: its menu doesn't open.
    filterBar.askOptions = [this] (std::function<void (FilterOptions)> then)
    { model.startFilterOptions (searchField.getText(), filters, std::move (then)); };
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
        model.cancelFilterOptions(); // counted in the index before
        searchShown.clear();         // the rows shown are of the index before: the same text finds them again

        if (! filters.isEmpty())
            rebuildPlaces(); // listed again from this index, what was being listed from the one before cancelled

        if (searchField.getText().trim().isNotEmpty())
            updateSearch (true);

        // A folder being read from the disk, opened again: from this index, if it holds the folder whole now.
        // Each still among them as its turn comes (one opened before may have taken another away), with what waited for it
        // kept; nothing that waited run here, in the middle of the change.
        for (auto* row : std::vector<Node*> (openingRows.begin(), openingRows.end()))
        {
            if (! openingRows.count (row))
                continue;

            auto wanted = std::move (row->wantedOpenness);
            auto waiting = std::exchange (row->afterFill, {}); // emptied, not moved from: more is added to it below
            row->setOpen (false);
            row->setOpen (true);

            if (wanted != nullptr)
                restoreOpenness (*row, *wanted);

            for (auto& then : waiting)
                row->afterFill.push_back (std::move (then));
        }

        // The devices' rows as many presets as the index counts now, and Other Presets as it holds now.
        refreshDevices();
        refreshOtherPresets();

        // A preset just saved, shown where it belongs, selected, once the index has it (another change of it, a folder
        // patched, may come first).
        if (revealWhenIndexed.has_value() && library.getWholeIndex()->find (revealWhenIndexed->getFullPathName()).has_value())
            reveal (*std::exchange (revealWhenIndexed, std::nullopt));
    };

    soundPack.addChangeListener (this);
    deviceFolders = model.getDeviceFolders();
    rebuildPlaces();
    lookAndFeelChanged();
}

Browser::~Browser()
{
    soundPack.removeChangeListener (this);
    library.onIndexChanged = nullptr;
    model.cancelSearch(); // which would call back into it
    model.cancelFilterOptions();
    model.cancelFilteredPlaces();

    // What a swap tried stays, as anything else that changes the song leaves it.
    if (swap != nullptr && swap->isOpen())
        swap->keep();

    // The folders open now open again next time: as they were without the filters, if any are set.
    if (const auto openness = opennessUnfiltered != nullptr ? std::move (opennessUnfiltered) : getOpenness (*placesRoot))
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
        parent.addSubItem (new ItemNode (*this, item, contents.getPresets (item)));

    for (const auto& message : contents.messages)
        parent.addSubItem (new MessageNode (*this, message));
}

void Browser::updateRows (juce::TreeViewItem& parent, const BrowserContents& contents)
{
    // The rows it should have, in addRows()' order, by their unique names (the nodes' getUniqueName()): those that went,
    // gone; those that stay, kept as they are (selected, open), moved where they go now; those that came, made where they
    // go. Nothing made, moved or taken away where nothing differs.
    enum class Kind
    {
        pack,
        place,
        item,
        message
    };

    struct Wanted
    {
        juce::String name;
        Kind kind;
        size_t index;
    };

    std::vector<Wanted> wanted;
    wanted.reserve (contents.places.size() + contents.items.size() + (size_t) contents.messages.size() + 1);

    if (contents.offersSoundPack)
        wanted.push_back ({ "soundPack", Kind::pack, 0 });

    for (size_t i = 0; i < contents.places.size(); ++i)
        wanted.push_back ({ "place:" + contents.places[i].getKey(), Kind::place, i });

    for (size_t i = 0; i < contents.items.size(); ++i)
        wanted.push_back ({ contents.items[i].getKey(), Kind::item, i });

    for (int i = 0; i < contents.messages.size(); ++i)
        wanted.push_back ({ "message:" + contents.messages[i], Kind::message, (size_t) i });

    std::set<juce::String> wantedNames;

    for (const auto& each : wanted)
        wantedNames.insert (each.name);

    for (int i = parent.getNumSubItems(); --i >= 0;)
        if (! wantedNames.count (parent.getSubItem (i)->getUniqueName()))
            parent.removeSubItem (i);

    // A device's row that stays holds as many presets as it does now.
    const auto keep = [&contents] (juce::TreeViewItem* row)
    {
        if (auto* kept = dynamic_cast<ItemNode*> (row))
            kept->setPresets (contents.getPresets (kept->item));
    };

    for (int i = 0; i < (int) wanted.size(); ++i)
    {
        const auto& each = wanted[(size_t) i];

        if (i < parent.getNumSubItems() && parent.getSubItem (i)->getUniqueName() == each.name)
        {
            keep (parent.getSubItem (i));
            continue;
        }

        // One that stays, further down: moved here, as it is.
        auto moved = false;

        for (int j = i + 1; j < parent.getNumSubItems() && ! moved; ++j)
        {
            if (auto* row = parent.getSubItem (j); row->getUniqueName() == each.name)
            {
                parent.removeSubItem (j, false);
                parent.addSubItem (row, i);
                keep (row);
                moved = true;
            }
        }

        if (moved)
            continue;

        juce::TreeViewItem* made = nullptr;

        switch (each.kind)
        {
            case Kind::pack:
                made = new SoundPackNode (*this);
                break;
            case Kind::place:
                made = new PlaceNode (*this, contents.places[each.index]);
                break;
            case Kind::item:
                made = new ItemNode (*this, contents.items[each.index], contents.getPresets (contents.items[each.index]));
                break;
            case Kind::message:
                made = new MessageNode (*this, contents.messages[(int) each.index]);
                break;
        }

        parent.addSubItem (made, i);
    }
}

juce::int64 Browser::countRowsMade() noexcept
{
    return Node::rowsMade.load();
}

int Browser::lookAgainAtOpenFolders (bool cameToFront)
{
    // Every open folder of the rows shown, those in view first, as many as mostLookedAgain, each at most once in
    // lookAgainMilliseconds: what changed in it outside Phazite since, brought up to date in place. Phazite come to the
    // front (the user back from Explorer, where they may have just changed it), those in view whatever their last look.
    // Nothing read here: each is read on a listing thread.
    std::vector<Node*> open;
    std::function<void (juce::TreeViewItem*)> collect = [&collect, &open] (juce::TreeViewItem* item)
    {
        if (item == nullptr || ! item->isOpen())
            return;

        if (auto* node = dynamic_cast<Node*> (item); node != nullptr && node->filled && node->checker != nullptr)
            open.push_back (node);

        for (int i = 0; i < item->getNumSubItems(); ++i)
            collect (item->getSubItem (i));
    };

    collect (tree->getRootItem());

    const auto* viewport = tree->getViewport();
    const auto top = viewport->getViewPositionY(), bottom = top + viewport->getViewHeight();
    const auto inView = [top, bottom] (const Node* node)
    {
        const auto area = const_cast<Node*> (node)->getItemPosition (false);
        return area.getBottom() >= top && area.getY() <= bottom;
    };

    std::stable_partition (open.begin(), open.end(), inView);

    auto looked = 0;
    const auto now = devices::Clock::getMilliseconds();

    for (auto* node : open)
    {
        if (looked >= mostLookedAgain)
            break;

        const auto always = cameToFront && inView (node);

        if (! always && now - node->checkedAt < lookAgainMilliseconds)
            continue;

        node->lookAgain (always);
        ++looked;
    }

    return looked;
}

void Browser::queueKey (const juce::KeyPress& key, Node* row)
{
    queuedKey = { key, row, devices::Clock::getMilliseconds() };
}

void Browser::dropQueuedKey()
{
    queuedKey = {};
    ++keyGeneration;
}

void Browser::userActed()
{
    // What waited for rows to come is for what the user looked at before: dropped.
    dropQueuedKey();
    wantedSelection.clear();
}

void Browser::replayQueuedKey()
{
    const auto queued = std::exchange (queuedKey, {});

    if (! queued.key.has_value() || devices::Clock::getMilliseconds() - queued.at > keyWaitMilliseconds)
        return; // none, or waited for too long: it would act on what the user no longer looks at

    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<Browser> (this), key = *queued.key, generation = keyGeneration]
        {
            if (safeThis != nullptr && safeThis->keyGeneration == generation)
                safeThis->tree->keyPressed (key);
        });
}

std::unique_ptr<juce::XmlElement> Browser::getOpenness (const juce::TreeViewItem& item) const
{
    // As JUCE's: an open row as OPEN with what is open within it, a closed one as nothing; the root always open.
    const auto isRoot = dynamic_cast<const RootNode*> (&item) != nullptr;

    if (! isRoot && ! item.isOpen())
        return nullptr;

    auto element = std::make_unique<juce::XmlElement> ("OPEN");
    element->setAttribute ("id", item.getUniqueName());

    // A folder still being read: what was asked of what it holds, kept.
    if (const auto* node = dynamic_cast<const Node*> (&item); node != nullptr && node->reading && node->wantedOpenness != nullptr)
    {
        for (const auto* child : node->wantedOpenness->getChildIterator())
            element->addChildElement (new juce::XmlElement (*child));

        return element;
    }

    for (int i = 0; i < item.getNumSubItems(); ++i)
        if (auto child = getOpenness (*item.getSubItem (i)))
            element->addChildElement (child.release());

    return element;
}

void Browser::restoreOpenness (juce::TreeViewItem& item, const juce::XmlElement& state)
{
    // As JUCE's TreeViewItem::restoreOpennessState(), but a folder still being read keeps what is asked of its rows until
    // they are in.
    if (state.hasTagName ("CLOSED"))
    {
        item.setOpen (false);
        return;
    }

    if (! state.hasTagName ("OPEN"))
        return;

    if (dynamic_cast<RootNode*> (&item) == nullptr)
        item.setOpen (true);

    if (auto* node = dynamic_cast<Node*> (&item); node != nullptr && node->reading)
    {
        node->wantedOpenness = std::make_unique<juce::XmlElement> (state);
        return;
    }

    juce::Array<juce::TreeViewItem*> rows;

    for (int i = 0; i < item.getNumSubItems(); ++i)
        rows.add (item.getSubItem (i));

    for (const auto* child : state.getChildIterator())
    {
        const auto id = child->getStringAttribute ("id");

        for (int i = 0; i < rows.size(); ++i)
        {
            if (rows[i]->getUniqueName() == id)
            {
                restoreOpenness (*rows[i], *child);
                rows.remove (i);
                break;
            }
        }
    }

    // What wasn't mentioned, closed (JUCE's default openness).
    for (auto* row : rows)
        if (row->isOpen())
            row->setOpen (false);
}

std::unique_ptr<juce::XmlElement> Browser::getTreeState() const
{
    auto* root = tree->getRootItem();

    if (root == nullptr)
        return nullptr;

    auto state = getOpenness (*root);

    for (int i = 0; i < tree->getNumSelectedItems(); ++i)
        if (auto* selected = tree->getSelectedItem (i))
            state->createNewChildElement ("SELECTED")->setAttribute ("id", selected->getItemIdentifierString());

    for (const auto& id : wantedSelection)
        state->createNewChildElement ("SELECTED")->setAttribute ("id", id);

    return state;
}

void Browser::restoreTreeState (const juce::XmlElement& state)
{
    auto* root = tree->getRootItem();

    if (root == nullptr)
        return;

    wantedSelection.clear();
    tree->clearSelectedItems();

    for (const auto* selected : state.getChildWithTagNameIterator ("SELECTED"))
        wantedSelection.addIfNotAlreadyThere (selected->getStringAttribute ("id"));

    restoreOpenness (*root, state);
    selectWanted();
}

void Browser::selectWanted()
{
    const juce::ScopedValueSetter<bool> restoring (restoringRows, true); // no key waiting is dropped for it
    // The rows asked to be selected that are in now: selected (a folder being read keeps the others until its rows come).
    auto* root = tree->getRootItem();

    if (root == nullptr)
        return;

    for (int i = wantedSelection.size(); --i >= 0;)
    {
        if (auto* row = findRow (*root, wantedSelection[i]))
        {
            row->setSelected (true, true); // one row at a time: nothing else selected meanwhile stays
            wantedSelection.remove (i);
        }
    }
}

void Browser::rebuildPlaces()
{
    // With filters set, the places of the index are gone through on the library's thread: the tree shows the places it
    // showed until the newest filters' are in, then those, whole, never a mix. What waits for the rows (Enter, Down, a
    // swap's end) waits for them too (afterSearch()).
    if (! filters.isEmpty())
    {
        placesPending = true;
        model.startFilteredPlaces (filters,
                                   [this] (BrowserModel::FilteredPlaces found)
                                   {
                                       placesPending = false;
                                       listedPlaces = std::move (found); // what the new places' rows list, as they open
                                       showPlaces (listedPlaces.places);
                                       runWhenSearchShown();
                                   });
        return;
    }

    model.cancelFilteredPlaces();
    placesPending = false;
    listedPlaces = {};
    showPlaces (model.getPlaces());
    runWhenSearchShown();
}

BrowserContents Browser::getPlaceContents (const BrowserPlace& place) const
{
    // As listed with the filters of the places the tree shows: what they were listed with, or for a collection under
    // Collections (a list of the user's own), its sounds with those filters' values.
    if (const auto found = listedPlaces.contents.find (place.getKey()); found != listedPlaces.contents.end())
        return found->second;

    return model.getContents (place, listedPlaces.filters);
}

void Browser::showPlaces (std::vector<BrowserPlace> places)
{
    // Folders open now stay open; at first, those open when Phazite last closed, or else Phazite's sounds.
    std::unique_ptr<juce::XmlElement> openness;

    if (placesRoot != nullptr)
        openness = getOpenness (*placesRoot);
    else
        openness = juce::parseXML (settings.get (Settings::browserOpenness));

    auto root = std::make_unique<RootNode> (*this);
    PlaceNode* sounds = nullptr;

    for (auto& place : places)
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
        restoreOpenness (*placesRoot, *openness);
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
            const auto reopen = [kind, &browser = *safeThis] (juce::TreeViewItem* item)
            {
                if (auto* place = dynamic_cast<PlaceNode*> (item); place != nullptr && place->place.kind == kind && place->isOpen())
                {
                    const auto openness = browser.getOpenness (*place);
                    place->setOpen (false);
                    place->setOpen (true);

                    if (openness != nullptr)
                        browser.restoreOpenness (*place, *openness);
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

void Browser::refreshDevices()
{
    // Instruments and Effects open, without filters (with them, they don't show): brought up to date in place, so that what
    // is open within them stays.
    if (placesRoot == nullptr || ! listedPlaces.filters.isEmpty())
        return;

    for (int i = 0; i < placesRoot->getNumSubItems(); ++i)
        if (auto* place = dynamic_cast<PlaceNode*> (placesRoot->getSubItem (i));
            place != nullptr && place->isOpen() && place->filled
            && (place->place.kind == BrowserPlace::Kind::instruments || place->place.kind == BrowserPlace::Kind::effects))
            updateRows (*place, model.getContents (place->place));
}

void Browser::refreshOtherPresets()
{
    // Without filters: Other Presets shows while it holds any (the places listed again where it came or went), and holds
    // what no device's row lists (its rows listed again where the devices changed: a plug-in scanned, whose folder it held,
    // shows under the plug-in's row now). The places' open folders stay open.
    if (placesRoot == nullptr || ! filters.isEmpty() || ! listedPlaces.filters.isEmpty())
        return;

    const auto folders = model.getDeviceFolders();
    const auto devicesChanged = std::exchange (deviceFolders, folders) != folders;
    juce::StringArray shown, wanted;

    for (int i = 0; i < placesRoot->getNumSubItems(); ++i)
        shown.add (placesRoot->getSubItem (i)->getUniqueName());

    for (const auto& place : model.getPlaces())
        wanted.add ("place:" + place.getKey());

    if (shown != wanted)
        rebuildPlaces();
    else if (devicesChanged)
        refreshPlace (BrowserPlace::Kind::otherPresets);
}

void Browser::textEditorTextChanged (juce::TextEditor&)
{
    // Typed on after Enter or Down: the field keeps the keys, and the results to come don't take them. A filter's menu still
    // waiting for its counts doesn't open: they were of the words before.
    focusWhenShown = false;
    userActed(); // text typed: a key or a selection waiting for rows is dropped
    model.cancelFilterOptions();
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
        opennessUnfiltered = getOpenness (*placesRoot);

    model.cancelFilterOptions(); // counted for the filters before
    filters = changed;
    showFilters();
    rebuildPlaces();

    if (filters.isEmpty() && opennessUnfiltered != nullptr)
    {
        restoreOpenness (*placesRoot, *opennessUnfiltered);
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

void Browser::updateSearch (bool again)
{
    const auto query = searchField.getText().trim();

    // The places; or while a device's presets are swapped, they alone.
    if (query.isEmpty())
    {
        model.cancelSearch();
        searchPending = false;
        tree->setRootItem (swapRoot != nullptr ? swapRoot.get() : placesRoot.get());
        runWhenSearchShown();
        return;
    }

    // The rows of the same search, as they were (the search before a swap, back as it ends, its row selected again).
    if (! again && query == searchShown && filters == filtersShown && searchRoot->getNumSubItems() > 0)
    {
        model.cancelSearch();
        searchPending = false;
        tree->setRootItem (searchRoot.get());
        runWhenSearchShown();
        return;
    }

    // In the background, so that no key typed waits for it, each search cancelling the one before (known bug 128).
    searchPending = true;
    model.startSearch (query, filters,
                       [this, query, searched = filters] (const BrowserContents& found)
                       {
                           searchRoot->clearSubItems();
                           addRows (*searchRoot, found);
                           searchShown = query;
                           filtersShown = searched;
                           tree->setRootItem (searchRoot.get());
                           searchPending = false;
                           runWhenSearchShown();
                       });
}

void Browser::afterSearch (std::function<void()> then)
{
    if (! isWaitingForRows())
    {
        then();
        return;
    }

    whenSearchShown = [before = std::move (whenSearchShown), then = std::move (then)]
    {
        if (before != nullptr)
            before();

        then();
    };
}

void Browser::runWhenSearchShown()
{
    // Once the search's results and the filters' places are both in.
    if (isWaitingForRows())
        return;

    if (const auto then = std::exchange (whenSearchShown, nullptr))
        then();

    if (std::exchange (focusWhenShown, false))
        focusTree();

    // A key pressed while they came: on them now (later, on the message loop), if it is still wanted.
    if (queuedKey.key.has_value() && queuedKey.row == nullptr)
        replayQueuedKey();
}

void Browser::focusTreeAfterSearch()
{
    if (isWaitingForRows())
        focusWhenShown = true;
    else
        focusTree();
}

void Browser::textEditorReturnKeyPressed (juce::TextEditor&)
{
    focusTreeAfterSearch(); // the newest text's results, which are milliseconds away
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
    // Once Phazite Sounds are installed, their place lists them, and search and the filters find them. Asked of the disk
    // only as the download's state changes, not at each of its progress's messages.
    if (const auto state = soundPack.getState(); state != packState)
    {
        packState = state;
        model.soundPackChanged();
    }

    if (const auto installed = model.isSoundPackInstalled(); installed != soundPackInstalled)
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

    // A sound or a MIDI file is noted once its import has put it into the song (notePlaced()).
    if (item.kind != LibraryItem::Kind::sound && item.kind != LibraryItem::Kind::midi)
        noteUsed (item);

    return true;
}

void Browser::notePlaced (const juce::File& file)
{
    if (const auto item = library.findItem (file.getFullPathName()))
        noteUsed (*item);
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
        wasInView = isInView(); // so that it stops as the browser goes out of view, even before the timer's next look
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

    // A folder's, Phazite Sounds' and the user's folder of track presets, of containers' or of other presets, as the items'
    // are; the user's folders can go from the browser.
    const auto isFolder = place.kind == BrowserPlace::Kind::folder;
    const auto isPresets =
        (model.getShelf (place).has_value() || place.kind == BrowserPlace::Kind::otherPresets) && place.folder.isDirectory();

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
        // Its sound copied into the project first (in the background): the next swap starts once it is kept, as none
        // opens meanwhile (SwapModel.h).
        const auto pending = swap->isOpen()
                             && swap->keep (
                                 [safeThis = juce::Component::SafePointer<Browser> (this), device, pad, same]
                                 {
                                     if (safeThis != nullptr && ! same)
                                         safeThis->startSwap (device, pad);
                                 });

        endSwap();

        if (pending)
            return;
    }

    endedSwapRoot = nullptr;

    if (same)
        return;

    auto& context = const_cast<ProjectContext&> (projectContext); // trying something in place changes the open project
    swap = std::make_unique<Swap> (context, device, pad);
    triable.clear();

    if (! swap->isOpen())
    {
        // Held back by another swap's keep, which copies its sounds in (SwapModel.h), only when code asks at the wrong time:
        // the window takes no click or key from that copy's start (app/InputHold.h, model/arrangement/internal/AudioWork.h),
        // and the swap asked for by what began the keep waits for it (above). Said, so that what asks so fails clearly,
        // rather than the swap it asked for being dropped without a word.
        jassert (! swap->isHeldBack());
        swap = nullptr;
        return;
    }

    previewer.stop();
    treeBeforeSwap = getTreeState(); // with the row selected
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

        // The first in the Type filter's order that some sounds are, by the index's counts (none of it gone through here).
        for (const auto& type : Library::getSoundTypes())
            if (fitting.type.isEmpty() && types.contains (type) && model.countSoundsOfType (type) > 0)
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

    // Onto the rows of the search before the swap, once they are back (found again if the index changed meanwhile).
    afterSearch (
        [this, before = std::shared_ptr<juce::XmlElement> (std::move (treeBeforeSwap)), scroll = scrollBeforeSwap]
        {
            if (before != nullptr)
                restoreTreeState (*before);

            // At once, though a folder still being read has fewer rows than it will: the scroll can fall short of where it
            // was, and isn't made again as its rows come (docs/backlog.md: "The browser's scroll after a Swap ...").
            scrollTreeTo (scroll);
            selectionMoved();
        });

    filterBar.setVisible (true);
    showFilters();
    resized();
    tree->repaint();
    repaint (statusArea);
}

void Browser::layOutTreeNow()
{
    // JUCE's tree lays its rows out on the next message after they change, and a scroll before then goes by where they were,
    // or is lost while the old rows fit in view: given its root again, it lays them out at once. JUCE workaround W19
    // (docs/juce-workarounds.md).
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
    // As its row under Instruments or Effects lists them: Phazite's and the user's as one, every folder of them open, so that
    // every preset is there for Down to go to (those of a folder still being read, once it is in).
    auto root = std::make_unique<RootNode> (*this);
    const auto shelf = PresetShelf::ofDevice (getPresetFolderName (projectContext, device), swap->getTargetName());
    auto* opened = root.get();
    opened->openWith ([this, shelf] (std::function<void (BrowserContents)> then) { return model.open (shelf, std::move (then)); },
                      [this, shelf] (std::function<void (BrowserContents)> then) { return model.check (shelf, std::move (then)); });

    if (opened->filled)
        openWithFolders (*opened);
    else
        opened->afterFill.push_back (
            [safeThis = juce::Component::SafePointer<Browser> (this), opened]
            {
                if (safeThis != nullptr && safeThis->swapRoot.get() == opened)
                    openWithFolders (*opened);
            });

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
    // Saved once the plug-ins that said they changed have given their states, where it is chosen (known bug 166).
    askWhereToSavePreset (presetChooser, library.getPresetsFolder().getChildFile (folder), name,
                          [safeThis = juce::Component::SafePointer<Browser> (this), onChosen = std::move (save)] (const juce::File& file)
                          {
                              if (safeThis == nullptr || file == juce::File())
                                  return;

                              const_cast<ProjectContext&> (safeThis->projectContext)
                                  .withPluginStates (
                                      [safeThis, onChosen, file] (bool go)
                                      {
                                          if (go && safeThis != nullptr && onChosen (file))
                                              safeThis->presetSaved (file);
                                      });
                          });
}

void Browser::showDevicePresetMenu (DeviceID device, const juce::String& name, juce::Component& target)
{
    const auto folder = getPresetFolderName (projectContext, device);

    if (folder.isEmpty())
        return;

    auto& context = const_cast<ProjectContext&> (projectContext); // loading into the device changes the open project
    const juce::Component::SafePointer<Browser> safeThis (this);
    auto presets = makeDevicePresetMenu (library, folder, name, [&context, device] (const juce::File& file)
                                         { loadPresetWithPluginStates (context, device, file); });

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
    chooser = std::make_unique<juce::FileChooser> ("Save a Preset", folder.getChildFile (makeUsableFileName (name) + presetExtension),
                                                   "*" + juce::String (presetExtension));

    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [onChosen = std::move (save)] (const juce::FileChooser& dialog)
                          {
                              if (const auto chosen = dialog.getResult(); chosen != juce::File())
                                  onChosen (chosen.withFileExtension (presetExtension));
                          });
}

void Browser::saveTrackPreset (TrackID track)
{
    const auto channel = getChannel (projectContext, track);

    if (! channel.has_value() || channel->kind != ChannelKind::track)
        return;

    // Once the plug-ins that said they changed have given their states, where the track is dropped (known bug 166).
    const_cast<ProjectContext&> (projectContext)
        .withPluginStates (
            [safeThis = juce::Component::SafePointer<Browser> (this), track, name = channel->name] (bool go)
            {
                if (! go || safeThis == nullptr)
                    return;

                const auto folder = safeThis->library.getPresetsFolder().getChildFile (trackPresetsFolder);
                folder.createDirectory();
                const auto file = folder.getNonexistentChildFile (makeUsableFileName (name), presetExtension, true);

                if (phazite::saveTrackPreset (safeThis->projectContext, track, file))
                    safeThis->presetSaved (file);
            });
}

void Browser::presetSaved (const juce::File& file)
{
    // Search finds it, and its place shows it, selected, once the index has it: under its device, the containers' row or
    // Track Presets (BrowserModel::findHome()).
    revealWhenIndexed = file;
    library.notePresetSaved();
}

void Browser::reveal (const juce::File& file)
{
    const auto home = model.findHome (file);

    if (placesRoot == nullptr || ! home.has_value())
        return;

    // The place, listed anew with its folders open as they were (its rows as the index has them now: the device's row with
    // its presets), and opened down to the file.
    for (int i = 0; i < placesRoot->getNumSubItems(); ++i)
    {
        auto* place = dynamic_cast<PlaceNode*> (placesRoot->getSubItem (i));

        if (place == nullptr || place->place.kind != home->place)
            continue;

        const auto openness = getOpenness (*place);
        place->setOpen (false);
        place->setOpen (true);

        if (openness != nullptr)
            restoreOpenness (*place, *openness);

        revealIn (file);
        return;
    }
}

void Browser::revealIn (const juce::File& file)
{
    // Down to the file, a row at a time, each once its rows are in (a folder read on a listing thread).
    if (isWaitingForRows())
    {
        afterSearch (
            [safeThis = juce::Component::SafePointer<Browser> (this), file]
            {
                if (safeThis != nullptr)
                    safeThis->revealIn (file);
            });
        return;
    }

    const auto home = model.findHome (file);

    if (placesRoot == nullptr || ! home.has_value())
        return;

    // Whether a row leads to the file: the containers' row it is in, its device's row, or a folder it is in.
    const auto leads = [this, &home, &file] (Node& node)
    {
        if (const auto* place = dynamic_cast<PlaceNode*> (&node))
            return home->container.has_value() && place->place.kind == *home->container;

        const auto* item = node.getItem();

        if (item == nullptr)
            return false;

        if (item->device.has_value())
            return home->deviceFolder.isNotEmpty() && BrowserModel::getShelf (*item)->folderName == home->deviceFolder;

        return model.leadsTo (*item, file);
    };

    for (int i = 0; i < placesRoot->getNumSubItems(); ++i)
    {
        auto* place = dynamic_cast<PlaceNode*> (placesRoot->getSubItem (i));

        if (place == nullptr || place->place.kind != home->place || ! place->isOpen())
            continue;

        // On once a folder being read has its rows (dropped if it closes or goes first).
        const auto onceFilled = [this, file] (Node& reading)
        {
            reading.afterFill.push_back (
                [safeThis = juce::Component::SafePointer<Browser> (this), file]
                {
                    if (safeThis != nullptr)
                        safeThis->revealIn (file);
                });
        };

        if (place->reading)
        {
            onceFilled (*place);
            return;
        }

        for (juce::TreeViewItem* parent = place; parent != nullptr;)
        {
            juce::TreeViewItem* next = nullptr;

            for (int j = 0; j < parent->getNumSubItems() && next == nullptr; ++j)
            {
                auto* node = dynamic_cast<Node*> (parent->getSubItem (j));

                if (node == nullptr)
                    continue;

                if (const auto* item = node->getItem(); item != nullptr && item->file == file)
                {
                    node->setSelected (true, true);
                    layOutTreeNow(); // the rows just opened
                    tree->scrollToKeepItemVisible (node);
                    return;
                }

                if (! node->mightContainSubItems() || ! leads (*node))
                    continue;

                if (! node->isOpen())
                    node->setOpen (true);

                if (node->reading)
                {
                    onceFilled (*node);
                    return;
                }

                next = node;
            }

            parent = next;
        }

        return;
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
        // Its open folders were read as they opened: looked at again only once the keys or the front are taken again.
        wasFocused = hasKeyboardFocus (true);
        wasInFront = juce::Process::isForegroundProcess();
        startTimerHz (10);
    }
    else
    {
        stopTimer();
        stopWhileHidden();
    }
}

void Browser::stopWhileHidden()
{
    previewer.stop();

    if (isSwapping())
        keepSwap();
}

bool Browser::isInView() const
{
    // Visible, and so is everything it is in; a window minimized still counts, as its previews go on.
    for (const auto* c = static_cast<const juce::Component*> (this); c != nullptr; c = c->getParentComponent())
        if (! c->isVisible())
            return false;

    return true;
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

    // Hidden with what holds it (the left panel closed, dragged shut, another panel maximized), which visibilityChanged()
    // doesn't hear: as hidden, once, as it goes (what plays after, the startup sound, isn't the browser's to stop).
    if (const auto inView = isInView(); inView != wasInView)
    {
        wasInView = inView;

        if (! inView)
            stopWhileHidden();
    }

    previewer.keepInTime (projectContext);

    // A folder whose read has waited on its drive for a few seconds: its line says the drive isn't answering.
    for (auto* node : openingRows)
        node->sayIfLate();

    // The browser taking the keys again, or Phazite coming to the front (the user back from Explorer or another program):
    // its open folders looked at again, what changed in them outside brought up to date. No folder is watched meanwhile.
    const auto focused = hasKeyboardFocus (true);
    const auto inFront = juce::Process::isForegroundProcess();

    if (inFront && ! wasInFront)
        lookAgainAtOpenFolders (true);
    else if (focused && ! wasFocused)
        lookAgainAtOpenFolders (false);

    wasFocused = focused;
    wasInFront = inFront;

    // A one-shot ends by itself.
    if (const auto playing = previewer.isPlaying(); playing != wasPlaying)
    {
        wasPlaying = playing;
        tree->repaint();
        repaint (statusArea);
    }
}
} // namespace phazite::ui
