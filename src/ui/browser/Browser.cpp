// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Browser.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
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
        if (const auto* item = getItem(); item != nullptr && BrowserModel::canPlace (*item))
            return toDragDescription (*item);

        return {};
    }

    juce::String getAccessibilityName() override
    {
        auto name = getText();

        if (const auto* item = getItem())
        {
            if (const auto detail = BrowserModel::getDetail (*item); detail.isNotEmpty())
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

            if (const auto detail = BrowserModel::getDetail (*item); detail.isNotEmpty())
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
    std::optional<Icon> getIcon() const override { return getIconFor (item); }
    const LibraryItem* getItem() const override { return &item; }
    bool mightContainSubItems() override { return item.kind == LibraryItem::Kind::folder; }
    juce::String getUniqueName() const override { return item.getKey(); }
    void populate() override { browser.addRows (*this, browser.model.getContents (item)); }
    void clicked() override { browser.preview (item); }

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
// A place in the tree, which lists sounds or devices: at the top, or a type under "Sounds by Type".
struct Browser::PlaceNode : public Node
{
    PlaceNode (Browser& b, BrowserPlace p)
        : Node (b),
          place (std::move (p))
    {
    }

    juce::String getText() const override { return place.getTitle(); }

    std::optional<Icon> getIcon() const override
    {
        switch (place.kind)
        {
            case BrowserPlace::Kind::favorites:
                return Icon::favorite;
            case BrowserPlace::Kind::recent:
                return Icon::recent;
            case BrowserPlace::Kind::type:
                return Icon::sound;
            case BrowserPlace::Kind::instruments:
                return Icon::instrument;
            case BrowserPlace::Kind::effects:
                return Icon::effect;
            case BrowserPlace::Kind::addFolder:
                return Icon::addFolder;
            case BrowserPlace::Kind::types:
            case BrowserPlace::Kind::sounds:
            case BrowserPlace::Kind::folder:
                break;
        }

        return Icon::folder;
    }

    bool mightContainSubItems() override { return place.holdsItems(); }
    juce::String getUniqueName() const override { return "place:" + place.getKey(); }
    void populate() override { browser.addRows (*this, browser.model.getContents (place)); }

    void clicked() override
    {
        if (place.kind == BrowserPlace::Kind::addFolder)
            browser.chooseFolderToAdd();
    }

    void activate() override { clicked(); }

    void showMenu() override
    {
        const auto isFolder = place.kind == BrowserPlace::Kind::folder;

        if (isFolder || (place.kind == BrowserPlace::Kind::sounds && browser.soundPack.isInstalled()))
            browser.showMenu (browser.library.makeFileItem (place.folder, place.folder), isFolder);
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
        settings.set (Settings::browserPreview, previewButton.getToggleState());

        if (! previewButton.getToggleState())
            previewer.stop();

        tree->repaint();
        repaint (statusArea);
    };

    showAsDecibels (volumeFader);
    volumeFader.setValue (settings.get (Settings::browserPreviewVolume, previewer.getVolume()), juce::dontSendNotification);
    volumeFader.setTooltip ("How loud sounds play while you browse. Double-click for -6 dB");
    volumeFader.onValueChange = [this]
    {
        previewer.setVolume ((float) volumeFader.getValue());
        settings.set (Settings::browserPreviewVolume, volumeFader.getValue());
    };
    previewer.setVolume ((float) volumeFader.getValue());

    for (auto* c : std::initializer_list<juce::Component*> { &searchField, tree.get(), &previewButton, &volumeFader })
        addAndMakeVisible (c);

    // Search finds more once the index of the sounds is ready.
    library.onIndexChanged = [this]
    {
        if (searchField.getText().trim().isNotEmpty())
            updateSearch();

        refreshPlace (BrowserPlace::Kind::type);
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
        settings.set (Settings::browserOpenness, openness->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader()));

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

    for (auto& place : model.getPlaces())
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

            const auto reopen = [kind] (juce::TreeViewItem* item)
            {
                if (auto* place = dynamic_cast<PlaceNode*> (item); place != nullptr && place->place.kind == kind && place->isOpen())
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

                if (auto* types = dynamic_cast<PlaceNode*> (place); types != nullptr && types->place.kind == BrowserPlace::Kind::types)
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
    addRows (*searchRoot, model.search (query));

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
        refreshPlace (BrowserPlace::Kind::sounds);
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
    refreshPlace (BrowserPlace::Kind::recent);
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
    if (BrowserModel::canPreview (item) && isPreviewOn())
        previewer.play (item.file, getProject(), BrowserModel::previewsInTime (item));
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
    const juce::Component::SafePointer<Browser> safeThis (this);
    juce::PopupMenu menu;

    if (BrowserModel::canPlace (item))
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
    g.drawText (BrowserModel::getPreviewStatus (isPreviewOn(), previewer), statusArea, juce::Justification::centredLeft, true);
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
