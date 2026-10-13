// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Browser.h"
#include "LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr const char* previewOnSetting = "phaziteBrowserPreview";
    constexpr const char* previewVolumeSetting = "phaziteBrowserPreviewVolume";
    constexpr const char* opennessSetting = "phaziteBrowserOpenness";
    constexpr int iconSize = 16;
    constexpr int starWidth = 24;
    constexpr int detachButtonRoom = 30; // the panel's detach button, in its top right corner
    constexpr int statusHeight = 20;

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
            case LibraryItem::Kind::sound:
                break;
        }

        return Icon::sound;
    }

    // What a row says after an item's name: a loop's tempo, or what a device is.
    juce::String getDetail (const LibraryItem& item)
    {
        if (item.device.has_value())
            return item.device->category;

        for (const auto& tag : item.tags)
            if (tag.endsWith (" BPM"))
                return tag;

        return {};
    }

    bool canPreview (const LibraryItem& item)
    {
        return item.kind == LibraryItem::Kind::sound || item.kind == LibraryItem::Kind::midi;
    }

    bool canPlace (const LibraryItem& item)
    {
        return item.kind != LibraryItem::Kind::folder;
    }
} // namespace

//==============================================================================
// A row of the tree.
struct Browser::Node : public juce::TreeViewItem
{
    explicit Node (Browser& b)
        : browser (b)
    {
    }

    virtual juce::String getText() const = 0;
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
        if (const auto* item = getItem(); item != nullptr && canPlace (*item))
            return toDragDescription (*item);

        return {};
    }

    juce::String getAccessibilityName() override
    {
        auto name = getText();

        if (const auto* item = getItem())
        {
            if (const auto detail = getDetail (*item); detail.isNotEmpty())
                name << ", " << detail;

            if (browser.library.isFavorite (*item))
                name << ", favorite";
        }

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
        const auto onAccent = isSelected() && focused;
        auto area = juce::Rectangle<int> (width, height);

        // The selected row, in the accent color while the tree has the keyboard focus.
        if (isSelected())
        {
            g.setColour (focused ? p.accent : p.accent.withAlpha (0.3f));
            g.fillRoundedRectangle (area.toFloat().reduced (1.0f), cornerRadius);
        }

        const auto textColour = onAccent ? p.textOnAccent : p.text;
        const auto mutedColour = onAccent ? p.textOnAccent : p.textMuted;
        area.reduce (Space::xs, 0);

        // Its icon: in the accent color while it plays.
        if (const auto icon = getIcon())
        {
            const auto playing = item != nullptr && browser.isPreviewing (*item);
            browser.drawIcon (g, *icon, area.removeFromLeft (iconSize).toFloat().withSizeKeepingCentre ((float) iconSize, (float) iconSize),
                              playing && ! onAccent ? p.accent : mutedColour);
            area.removeFromLeft (Space::s);
        }

        if (item != nullptr)
        {
            // A favorite's star, filled, and the selected row's, to click.
            const auto favorite = browser.library.isFavorite (*item);
            const auto star = area.removeFromRight (starWidth);

            if (favorite || isSelected())
                browser.drawIcon (g, Icon::favorite, star.toFloat().withSizeKeepingCentre (14.0f, 14.0f),
                                  favorite && ! onAccent ? p.warning : mutedColour, favorite);

            if (const auto detail = getDetail (*item); detail.isNotEmpty())
            {
                const juce::Font font { juce::FontOptions (TextSize::small) };
                g.setFont (font);
                g.setColour (mutedColour);
                const auto detailWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, detail)) + Space::s;
                g.drawText (detail, area.removeFromRight (detailWidth), juce::Justification::centredRight, false);
            }
        }

        g.setFont (juce::FontOptions (TextSize::body));
        g.setColour (canBeSelected() ? textColour : mutedColour);
        g.drawText (getText(), area, juce::Justification::centredLeft, true);
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

    juce::String getText() const override
    {
        const auto& pack = browser.soundPack;

        switch (pack.getState())
        {
            case SoundPack::State::downloading:
                return "Downloading... " + juce::String (juce::roundToInt (pack.getProgress() * 100.0)) + "%";
            case SoundPack::State::installing:
                return "Installing...";
            case SoundPack::State::failed:
                return "The download didn't finish";
            case SoundPack::State::notInstalled:
            case SoundPack::State::installed: // but its folder has gone
                break;
        }

        const auto size = juce::File::descriptionOfSizeInBytes (pack.getRelease().size);
        return SoundPack::canDownload() ? "Download (" + size + ")" : "Get Them (" + size + ")...";
    }

    std::optional<Icon> getIcon() const override { return Icon::download; }
    juce::String getUniqueName() const override { return "soundPack"; }

    // A click downloads them; while they download, or after a download failed, it shows the details.
    void clicked() override
    {
        if (isIdle() && SoundPack::canDownload())
            browser.soundPack.download(); // whose change messages repaint the row
        else if (browser.onShowSoundPack != nullptr)
            browser.onShowSoundPack();
    }

    // Enter, or a double-click, whose first click has started the download.
    void activate() override
    {
        if (const auto state = browser.soundPack.getState();
            state != SoundPack::State::downloading && state != SoundPack::State::installing)
            clicked();
    }

    juce::String getTooltip() override
    {
        const auto& pack = browser.soundPack;

        switch (pack.getState())
        {
            case SoundPack::State::downloading:
            case SoundPack::State::installing:
                return "Click to see the download, or to cancel it";
            case SoundPack::State::failed:
                return pack.getError() + " Click for more.";
            case SoundPack::State::notInstalled:
            case SoundPack::State::installed:
                break;
        }

        if (! SoundPack::canDownload())
            return "Opens a page that tells how to download them in a browser, and install them from the file";

        return "Downloads free drums, loops and effects, all public domain (CC0), into "
               + pack.getFolder().getParentDirectory().getFullPathName();
    }

    bool isIdle() const
    {
        const auto state = browser.soundPack.getState();
        return state == SoundPack::State::notInstalled || state == SoundPack::State::installed;
    }
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
    std::optional<Icon> getIcon() const override { return getIconFor (item); }
    const LibraryItem* getItem() const override { return &item; }
    bool mightContainSubItems() override { return item.kind == LibraryItem::Kind::folder; }
    juce::String getUniqueName() const override { return item.getKey(); }

    void populate() override
    {
        auto items = browser.library.listFolder (item.file);

        for (auto& child : items)
            addSubItem (new ItemNode (browser, std::move (child)));

        if (items.empty())
            addSubItem (new MessageNode (browser, "No sounds here"));
    }

    void clicked() override { browser.preview (item); }

    void activate() override
    {
        if (canPlace (item))
            browser.place (item);
    }

    void showMenu() override { browser.showMenu (item, false); }

    juce::String getTooltip() override
    {
        switch (item.kind)
        {
            case LibraryItem::Kind::sound:
                return "Click to hear it. Drag it onto a track, or below the tracks for a new one; Enter puts it on the selected track";
            case LibraryItem::Kind::midi:
                return "Notes: click to hear them on a simple synth. Drag them onto a track, or press Enter for the selected track";
            case LibraryItem::Kind::instrument:
                return "Drag it onto a track or a mixer channel, or press Enter to give it to the selected track";
            case LibraryItem::Kind::effect:
                return "Drag it onto a track or a mixer channel, or press Enter to add it to the selected track";
            case LibraryItem::Kind::folder:
                break;
        }

        return {};
    }

    const LibraryItem item;
};

//==============================================================================
// A place in the tree, which lists sounds or devices: at the top, or a type under "Sounds by Type".
struct Browser::PlaceNode : public Node
{
    enum Kind
    {
        favorites,
        recent,
        types,  // "Sounds by Type", which holds a place for each type
        type,   // the sounds of a type (its name), from every library
        sounds, // the ones that come with Phazite
        instruments,
        effects,
        folder, // one of the user's
        addFolder
    };

    PlaceNode (Browser& b, Kind k, juce::String placeName, juce::File placeFolder = {})
        : Node (b),
          kind (k),
          name (std::move (placeName)),
          file (std::move (placeFolder))
    {
    }

    juce::String getText() const override
    {
        // A type's sounds, as plurals where English has them: Kicks, Hats, Loops; Percussion, Bass, FX, Foley.
        if (kind == type && ! juce::StringArray { "Percussion", "Bass", "FX", "Foley" }.contains (name))
            return name + "s";

        return name;
    }

    std::optional<Icon> getIcon() const override
    {
        switch (kind)
        {
            case favorites:
                return Icon::favorite;
            case recent:
                return Icon::recent;
            case type:
                return Icon::sound;
            case instruments:
                return Icon::instrument;
            case effects:
                return Icon::effect;
            case addFolder:
                return Icon::addFolder;
            case types:
            case sounds:
            case folder:
                break;
        }

        return Icon::folder;
    }

    bool mightContainSubItems() override { return kind != addFolder; }

    juce::String getUniqueName() const override { return "place:" + juce::String ((int) kind) + ":" + name + ":" + file.getFullPathName(); }

    void populate() override
    {
        std::vector<LibraryItem> items;
        juce::String hint;

        switch (kind)
        {
            case favorites:
                items = browser.library.getFavorites();
                hint = "Nothing yet: click the star next to a sound or device";
                break;
            case recent:
                items = browser.library.getRecent();
                hint = "What you put into the song shows here";
                break;
            case types:
                for (const auto& soundType : Library::getSoundTypes())
                    addSubItem (new PlaceNode (browser, type, soundType));

                return;
            case type:
                items = browser.library.listByTag (name);
                hint = browser.library.isIndexing() ? "Still looking through your sounds..." : "None yet";
                break;
            case sounds:
                if (! browser.soundPack.isInstalled())
                {
                    addSubItem (new SoundPackNode (browser));
                    addSubItem (new MessageNode (browser, "Free drums, loops and effects to start with"));
                    return;
                }

                items = browser.library.listFolder (file);
                hint = "No sounds here";
                break;
            case folder:
                if (file.isDirectory())
                    items = browser.library.listFolder (file);

                hint = file.isDirectory() ? "No sounds here" : "This folder isn't there any more";
                break;
            case instruments:
            case effects:
                items = browser.library.listDevices (kind == instruments ? LibraryItem::Kind::instrument : LibraryItem::Kind::effect);
                hint = "Your plug-ins show here once Options > Scan for Plug-ins has found them";
                break;
            case addFolder:
                break;
        }

        // Instruments and effects always have the built-in ones; the hint says where the plug-ins are until there are some.
        const auto showHint =
            items.empty()
            || ((kind == instruments || kind == effects)
                && std::all_of (items.begin(), items.end(), [] (const LibraryItem& i)
                                { return i.device.has_value() && te::PluginManager::isBuiltInPlugin (i.device->description); }));

        for (auto& item : items)
            addSubItem (new ItemNode (browser, std::move (item)));

        if (showHint && hint.isNotEmpty())
            addSubItem (new MessageNode (browser, hint));
    }

    void clicked() override
    {
        if (kind == addFolder)
            browser.chooseFolderToAdd();
    }

    void activate() override { clicked(); }

    void showMenu() override
    {
        if (kind == folder || (kind == sounds && browser.soundPack.isInstalled()))
            browser.showMenu (browser.library.makeFileItem (file, file), kind == folder);
    }

    juce::String getTooltip() override
    {
        switch (kind)
        {
            case favorites:
                return "What you starred: click the star next to a sound or device, or right-click it";
            case recent:
                return "What you put into the song lately, newest first";
            case types:
                return "Every sound, from Phazite's and your folders, by what it is: from the words in its name and folders";
            case type:
                return "Every sound tagged " + name + ", from Phazite's and your folders";
            case sounds:
                return "Phazite's free sounds: drums, loops, effects and foley, all public domain (CC0)";
            case instruments:
                return "Instruments turn a track's notes into sound";
            case effects:
                return "Effects change the sound of a track";
            case folder:
                return file.getFullPathName();
            case addFolder:
                return "Adds a folder of your own sounds to the browser";
        }

        return {};
    }

    const Kind kind;
    const juce::String name;
    const juce::File file;
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

        // Esc stops a preview (and does what it does elsewhere when none plays).
        if (key == juce::KeyPress::escapeKey && browser.previewer.isPlaying())
        {
            browser.previewer.stop();
            repaint();
            return true;
        }

        if (! juce::TreeView::keyPressed (key))
            return false;

        // Moving to a sound plays it.
        if (auto* after = dynamic_cast<Node*> (getSelectedItem (0)); after != nullptr && after != before)
            if (const auto* item = after->getItem())
                browser.preview (*item);

        return true;
    }

    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    Browser& browser;
};

//==============================================================================
Browser::Browser (Library& l, Previewer& p, SoundPack& pack, std::function<te::Edit*()> editGetter)
    : library (l),
      previewer (p),
      soundPack (pack),
      getEdit (std::move (editGetter)),
      soundPackInstalled (pack.isInstalled()),
      tree (std::make_unique<Tree> (*this)),
      searchRoot (std::make_unique<RootNode> (*this))
{
    setTitle ("Browser");

    searchField.setTitle ("Search sounds, instruments and effects");
    searchField.setFont (juce::FontOptions (TextSize::body));
    searchField.addListener (this);
    searchField.onNavigationKey = [this] (const juce::KeyPress& key)
    {
        if (key != juce::KeyPress::downKey)
            return false;

        focusTree();
        return true;
    };

    tree->setTitle ("Sounds, instruments and effects");
    tree->setRootItemVisible (false);
    tree->setIndentSize (14);
    tree->setDefaultOpenness (false);
    tree->setMultiSelectEnabled (false);
    tree->setColour (juce::TreeView::selectedItemBackgroundColourId, juce::Colours::transparentBlack); // the rows paint it

    previewButton.setClickingTogglesState (true);
    previewButton.setToggleState (isPreviewOn(), juce::dontSendNotification);
    previewButton.setTooltip ("Plays each sound as you click it or move to it with the arrow keys; Esc stops it");
    previewButton.onClick = [this]
    {
        library.getSettings().setValue (previewOnSetting, previewButton.getToggleState());

        if (! previewButton.getToggleState())
            previewer.stop();

        tree->repaint();
        repaint (statusArea);
    };

    showAsDecibels (volumeFader);
    volumeFader.setValue (library.getSettings().getDoubleValue (previewVolumeSetting, previewer.getVolume()), juce::dontSendNotification);
    volumeFader.setTooltip ("How loud sounds play while you browse. Double-click for -6 dB");
    volumeFader.onValueChange = [this]
    {
        previewer.setVolume ((float) volumeFader.getValue());
        library.getSettings().setValue (previewVolumeSetting, volumeFader.getValue());
    };
    previewer.setVolume ((float) volumeFader.getValue());

    for (auto* c : std::initializer_list<juce::Component*> { &searchField, tree.get(), &previewButton, &volumeFader })
        addAndMakeVisible (c);

    // Search finds more once the index of the sounds is ready.
    library.onIndexChanged = [this]
    {
        if (searchField.getText().trim().isNotEmpty())
            updateSearch();

        refreshPlace (PlaceNode::type);
    };

    soundPack.addChangeListener (this);
    rebuildPlaces();
    lookAndFeelChanged();
}

Browser::~Browser()
{
    soundPack.removeChangeListener (this);
    library.onIndexChanged = nullptr;

    // The folders open now open again next time.
    if (const auto openness = placesRoot->getOpennessState())
        library.getSettings().setValue (opennessSetting, openness->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

    tree->setRootItem (nullptr);
}

juce::TreeView& Browser::getTree() noexcept
{
    return *tree;
}

//==============================================================================
void Browser::rebuildPlaces()
{
    // Folders open now stay open; at first, those open when Phazite last closed, or else Phazite's sounds.
    std::unique_ptr<juce::XmlElement> openness;

    if (placesRoot != nullptr)
        openness = placesRoot->getOpennessState();
    else
        openness = juce::parseXML (library.getSettings().getValue (opennessSetting));

    auto root = std::make_unique<RootNode> (*this);
    root->addSubItem (new PlaceNode (*this, PlaceNode::favorites, "Favorites"));
    root->addSubItem (new PlaceNode (*this, PlaceNode::recent, "Recent"));
    root->addSubItem (new PlaceNode (*this, PlaceNode::types, "Sounds by Type"));
    auto* sounds = new PlaceNode (*this, PlaceNode::sounds, "Phazite Sounds", library.getFactorySounds());
    root->addSubItem (sounds);
    root->addSubItem (new PlaceNode (*this, PlaceNode::instruments, "Instruments"));
    root->addSubItem (new PlaceNode (*this, PlaceNode::effects, "Effects"));

    for (const auto& folder : library.getUserFolders())
        root->addSubItem (new PlaceNode (*this, PlaceNode::folder, folder.getFileName(), folder));

    root->addSubItem (new PlaceNode (*this, PlaceNode::addFolder, "Add Folder..."));

    const auto showing = placesRoot == nullptr || tree->getRootItem() == placesRoot.get();

    if (showing)
        tree->setRootItem (nullptr);

    placesRoot = std::move (root);

    if (openness != nullptr)
        placesRoot->restoreOpennessState (*openness);
    else
        sounds->setOpen (true);

    if (showing && searchField.getText().trim().isEmpty())
        tree->setRootItem (placesRoot.get());
}

void Browser::refreshPlace (int kind)
{
    // Later: what asked may be one of the rows this replaces, such as a favorite unstarred in Favorites.
    juce::MessageManager::callAsync (
        [safeThis = juce::Component::SafePointer<Browser> (this), kind]
        {
            if (safeThis == nullptr || safeThis->placesRoot == nullptr)
                return;

            const auto reopen = [kind] (juce::TreeViewItem* item)
            {
                if (auto* place = dynamic_cast<PlaceNode*> (item); place != nullptr && place->kind == kind && place->isOpen())
                {
                    place->setOpen (false);
                    place->setOpen (true);
                }
            };

            // The places at the top, and the types under "Sounds by Type".
            auto& root = *safeThis->placesRoot;

            for (int i = 0; i < root.getNumSubItems(); ++i)
            {
                auto* place = root.getSubItem (i);
                reopen (place);

                if (auto* types = dynamic_cast<PlaceNode*> (place); types != nullptr && types->kind == PlaceNode::types)
                    for (int j = 0; j < types->getNumSubItems(); ++j)
                        reopen (types->getSubItem (j));
            }
        });
}

void Browser::updateSearch()
{
    const auto query = searchField.getText().trim();

    if (query.isEmpty())
    {
        tree->setRootItem (placesRoot.get());
        return;
    }

    searchRoot->clearSubItems();
    auto results = library.search (query);

    for (auto& item : results)
        searchRoot->addSubItem (new ItemNode (*this, std::move (item)));

    if (results.empty())
        searchRoot->addSubItem (new MessageNode (*this, library.isIndexing() ? juce::String ("Still looking through your sounds...")
                                                                             : "Nothing matches \"" + query + "\""));

    if (library.isIndexFull())
        searchRoot->addSubItem (new MessageNode (*this, "Search covers the first " + juce::String (Library::maxIndexed) + " sounds"));

    tree->setRootItem (searchRoot.get());
}

void Browser::focusSearch()
{
    searchField.grabKeyboardFocus();
    searchField.selectAll();
}

void Browser::focusTree()
{
    // The first row that can be selected, unless one is.
    if (tree->getNumSelectedItems() == 0)
    {
        for (int row = 0; row < tree->getNumRowsInTree(); ++row)
        {
            if (auto* item = tree->getItemOnRow (row); item != nullptr && item->canBeSelected())
            {
                item->setSelected (true, true);
                tree->scrollToKeepItemVisible (item);
                break;
            }
        }
    }

    tree->grabKeyboardFocus();

    if (auto* node = dynamic_cast<Node*> (tree->getSelectedItem (0)))
        if (const auto* item = node->getItem())
            preview (*item);
}

void Browser::textEditorEscapeKeyPressed (juce::TextEditor&)
{
    searchField.setText ({}, true);
}

void Browser::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Once Phazite Sounds are installed, their place lists them, and search and Sounds by Type find them.
    if (const auto installed = soundPack.isInstalled(); installed != soundPackInstalled)
    {
        soundPackInstalled = installed;
        library.refresh();
        refreshPlace (PlaceNode::sounds);
    }

    tree->repaint(); // the download's row
}

//==============================================================================
bool Browser::place (const LibraryItem& item)
{
    if (onPlace == nullptr || ! onPlace (item))
        return false;

    noteUsed (item);
    return true;
}

void Browser::noteUsed (const LibraryItem& item)
{
    library.noteUsed (item);
    refreshPlace (PlaceNode::recent);
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
    if (canPreview (item) && isPreviewOn())
        previewer.play (item.file, getProject(), item.kind == LibraryItem::Kind::midi || item.tags.contains ("Loop"));
    else
        previewer.stop();

    wasPlaying = previewer.isPlaying();
    tree->repaint();
    repaint (statusArea);
}

bool Browser::isPreviewing (const LibraryItem& item) const
{
    return item.isFile() && item.file == previewer.getFile() && previewer.isPlaying();
}

bool Browser::isPreviewOn() const
{
    return library.getSettings().getBoolValue (previewOnSetting, true);
}

void Browser::toggleFavorite (const LibraryItem& item)
{
    library.setFavorite (item, ! library.isFavorite (item));
    refreshPlace (PlaceNode::favorites);
    tree->repaint();
}

void Browser::showMenu (const LibraryItem& item, bool canRemoveFolder)
{
    const juce::Component::SafePointer<Browser> safeThis (this);
    juce::PopupMenu menu;

    if (canPlace (item))
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

    if (item.isFile())
    {
#if JUCE_MAC
        const juce::String showText ("Show in Finder");
#elif JUCE_WINDOWS
        const juce::String showText ("Show in Explorer");
#else
        const juce::String showText ("Show in File Manager");
#endif
        menu.addItem (showText, [file = item.file] { file.revealToUser(); });
    }

    if (canRemoveFolder)
        menu.addItem ("Remove from the Browser",
                      [safeThis, folder = item.file]
                      {
                          if (safeThis == nullptr)
                              return;

                          safeThis->library.removeUserFolder (folder);
                          safeThis->rebuildPlaces();
                      });

    menu.showMenuAsync (juce::PopupMenu::Options());
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
std::optional<LibraryItem> Browser::getSelectedItem() const
{
    if (auto* node = dynamic_cast<Node*> (tree->getSelectedItem (0)))
        if (const auto* item = node->getItem())
            return *item;

    return std::nullopt;
}

juce::String Browser::getStatus() const
{
    if (! isPreviewOn())
        return "Previews are off";

    if (! previewer.isPlaying())
        return "Click a sound to hear it, drag it into the song";

    const auto name = previewer.getFile().getFileNameWithoutExtension();
    return previewer.isInTime() ? name + ", in time with the song" : name;
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
    g.drawText (getStatus(), statusArea, juce::Justification::centredLeft, true);
}

void Browser::resized()
{
    auto area = getLocalBounds().reduced (Space::s);

    auto top = area.removeFromTop (controlHeight);
    top.removeFromRight (detachButtonRoom);
    searchField.setBounds (top);
    area.removeFromTop (Space::s);

    auto footer = area.removeFromBottom (controlHeight);
    previewButton.setBounds (footer.removeFromLeft (controlHeight));
    footer.removeFromLeft (Space::s);
    volumeFader.setBounds (footer);

    statusArea = area.removeFromBottom (statusHeight);
    area.removeFromBottom (Space::xs);
    tree->setBounds (area);
}

void Browser::visibilityChanged()
{
    // A hidden browser plays nothing.
    if (isVisible())
    {
        startTimerHz (10);
    }
    else
    {
        stopTimer();
        previewer.stop();
    }
}

void Browser::lookAndFeelChanged()
{
    searchField.setTextToShowWhenEmpty ("Search sounds and devices", getPalette (*this).textMuted);
}

void Browser::timerCallback()
{
    previewer.keepInTime (getProject());

    // A one-shot ends by itself.
    if (const auto playing = previewer.isPlaying(); playing != wasPlaying)
    {
        wasPlaying = playing;
        tree->repaint();
        repaint (statusArea);
    }
}
} // namespace phazite::ui
