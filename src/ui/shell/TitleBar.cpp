// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TitleBar.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
//==============================================================================
// The command search, shaped as a field: a click opens the palette, which hangs from it.
struct TitleBar::SearchBox : public juce::Button
{
    explicit SearchBox (CommandBridge& bridge)
        : juce::Button ("Search commands"),
          commands (bridge),
          icon (createIcon (Icon::search))
    {
        setTitle ("Search commands");
        setMouseClickGrabsKeyboardFocus (false);
        onClick = [this] { commands.invoke ("view.commandPalette"); };
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);
        const auto box = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (isDown ? p.surface : p.surfaceRaised);
        g.fillRoundedRectangle (box, cornerRadius);
        g.setColour (isHighlighted ? p.textMuted : p.border);
        g.drawRoundedRectangle (box, cornerRadius, 1.0f);

        // The icon and "Search commands" in the middle, the palette's key at the right, where there is room.
        const auto colour = p.textMuted.withMultipliedAlpha (dimmed ? Shell::inactiveAlpha : 1.0f);
        const auto font = juce::Font (juce::FontOptions (TextSize::body));
        const auto label = juce::String ("Search commands");
        const auto key = getKeyText (commands, "view.commandPalette");
        auto area = getLocalBounds().reduced (Space::s, 0);
        const auto keyWidth = key.isEmpty() ? 0 : juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, key)) + Space::s;
        const auto textWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, label));
        const auto showsText = area.getWidth() >= Shell::searchIconSize + Space::s + textWidth + 2 * keyWidth;
        const auto showsKey = showsText && keyWidth > 0;

        if (showsKey)
        {
            g.setColour (colour);
            g.setFont (font);
            g.drawText (key, area.removeFromRight (keyWidth), juce::Justification::centredRight, false);
            area.removeFromLeft (keyWidth); // the rest stays centred
        }

        const auto contentWidth = showsText ? Shell::searchIconSize + Space::s + textWidth : Shell::searchIconSize;
        auto content = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), contentWidth), area.getHeight());

        if (icon != nullptr)
        {
            if (iconColour != colour)
            {
                icon->replaceColour (iconColour, colour);
                iconColour = colour;
            }

            icon->drawWithin (g, content.removeFromLeft (Shell::searchIconSize).toFloat(), juce::RectanglePlacement::centred, 1.0f);
        }

        if (showsText)
        {
            content.removeFromLeft (Space::s);
            g.setColour (colour);
            g.setFont (font);
            g.drawText (label, content, juce::Justification::centredLeft, true);
        }
    }

    CommandBridge& commands;
    std::unique_ptr<juce::Drawable> icon;
    juce::Colour iconColour { juce::Colours::black };
    bool dimmed = false;
};

//==============================================================================
// One of the window's own buttons: as wide as Windows 11's, Close red under the pointer as there.
struct TitleBar::WindowButton : public juce::Button
{
    WindowButton (const juce::String& title, Icon iconShown, bool isClose)
        : juce::Button (title),
          closes (isClose)
    {
        setTitle (title);
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (false); // Windows' own keys (Alt+F4, Win+Up) do what they do
        setIcon (iconShown);
    }

    void setIcon (Icon iconShown)
    {
        icon = createIcon (iconShown);
        iconColour = juce::Colours::black;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);
        const auto lit = isHighlighted || isDown;

        if (lit)
            g.fillAll (closes ? juce::Colour (Shell::closeButtonColour) : p.surfaceRaised);

        const auto colour =
            (lit && closes ? juce::Colour (Shell::closeButtonText) : p.text).withMultipliedAlpha (dimmed ? Shell::inactiveAlpha : 1.0f);

        if (icon != nullptr)
        {
            if (iconColour != colour)
            {
                icon->replaceColour (iconColour, colour);
                iconColour = colour;
            }

            icon->drawWithin (g, getLocalBounds().toFloat().withSizeKeepingCentre (Shell::windowGlyphSize, Shell::windowGlyphSize),
                              juce::RectanglePlacement::centred, 1.0f);
        }
    }

    bool closes;
    bool dimmed = false;
    std::unique_ptr<juce::Drawable> icon;
    juce::Colour iconColour { juce::Colours::black };
};

//==============================================================================
// A panel's switch, as VS Code's: a little window with the panel's part filled while it shows.
struct TitleBar::PanelSwitch : public juce::Button
{
    PanelSwitch (CommandBridge& bridge, Panel shownPanel)
        : juce::Button (describePanel (shownPanel)),
          commands (bridge),
          panel (shownPanel)
    {
        setTitle (getTitleOf (shownPanel));
        setClickingTogglesState (false);
        setMouseClickGrabsKeyboardFocus (false);
        onClick = [this] { commands.invoke (getPanelCommandID ("toggle", panel)); };
    }

    static juce::String getTitleOf (Panel panel)
    {
        switch (panel)
        {
            case Panel::browser:
                return "Browser";
            case Panel::editor:
                return "Editor";
            case Panel::inspector:
                return "Inspector";
            case Panel::song:
            default:
                return "Playlist and mixer";
        }
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);

        if (isHighlighted || isDown)
        {
            g.setColour (p.surfaceRaised);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
        }

        const auto colour =
            (getToggleState() ? p.text : p.textMuted).withMultipliedAlpha (dimmed || ! isEnabled() ? Shell::inactiveAlpha : 1.0f);
        const auto frame = getLocalBounds().toFloat().withSizeKeepingCentre (Shell::panelSwitchWidth, Shell::panelSwitchHeight);
        auto part = frame;

        if (panel == Panel::browser)
            part = part.removeFromLeft (frame.getWidth() * Shell::panelSwitchSideShare);
        else if (panel == Panel::inspector)
            part = part.removeFromRight (frame.getWidth() * Shell::panelSwitchSideShare);
        else
            part = part.removeFromBottom (frame.getHeight() * Shell::panelSwitchBottomShare);

        g.setColour (colour);
        g.drawRoundedRectangle (frame.reduced (0.5f), Shell::panelSwitchCorner, 1.0f);

        if (getToggleState())
            g.fillRoundedRectangle (part.reduced (1.5f), Shell::panelSwitchCorner / 2.0f);
        else if (panel == Panel::browser)
            g.drawVerticalLine (juce::roundToInt (part.getRight()), frame.getY(), frame.getBottom());
        else if (panel == Panel::inspector)
            g.drawVerticalLine (juce::roundToInt (part.getX()), frame.getY(), frame.getBottom());
        else
            g.drawHorizontalLine (juce::roundToInt (part.getY()), frame.getX(), frame.getRight());
    }

    CommandBridge& commands;
    Panel panel;
    bool dimmed = false;
};

//==============================================================================
TitleBar::TitleBar (CommandBridge& bridge)
    : commands (bridge),
      searchBox (std::make_unique<SearchBox> (bridge)),
      browserSwitch (std::make_unique<PanelSwitch> (bridge, Panel::browser)),
      editorSwitch (std::make_unique<PanelSwitch> (bridge, Panel::editor)),
      inspectorSwitch (std::make_unique<PanelSwitch> (bridge, Panel::inspector))
{
    setTitle ("Title bar");

    projectName.setFont (juce::FontOptions (TextSize::body));
    projectName.setMinimumHorizontalScale (1.0f);

    menuButton.onClick = [this] { showFoldedMenus(); };
    undoButton.onClick = [this] { commands.invoke ("edit.undo"); };
    redoButton.onClick = [this] { commands.invoke ("edit.redo"); };
    layoutButton.onClick = [this] { showLayoutMenu(); };

    for (auto* button : std::initializer_list<IconButton*> { &menuButton, &undoButton, &redoButton, &layoutButton })
    {
        button->setQuiet (true);
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }

    menuButton.setVisible (false);

    for (auto* c : std::initializer_list<juce::Component*> { &projectName, searchBox.get(), browserSwitch.get(), editorSwitch.get(),
                                                             inspectorSwitch.get() })
        addAndMakeVisible (c);

    lookAndFeelChanged();
    refreshTooltips();
    startTimerHz (4);
}

void TitleBar::lookAndFeelChanged()
{
    projectName.setColour (juce::Label::textColourId, getPalette (*this).textMuted); // the theme changed
}

TitleBar::~TitleBar()
{
    if (menuBar != nullptr)
        menuBar->setModel (nullptr);
}

void TitleBar::setMenus (juce::MenuBarModel* model)
{
    menuModel = model;

    if (model == nullptr)
    {
        menuBar = nullptr;
    }
    else if (menuBar == nullptr)
    {
        menuBar = std::make_unique<juce::MenuBarComponent> (model);
        menuBar->getProperties().set (LookAndFeel::onBarProperty, true); // the bar's background behind it
        addAndMakeVisible (*menuBar);
    }
    else
    {
        menuBar->setModel (model);
    }

    resized();
}

void TitleBar::showMenus()
{
    if (menuBar != nullptr && menuBar->isVisible())
        menuBar->showMenu (0);
    else if (menuModel != nullptr)
        showFoldedMenus();
}

void TitleBar::showFoldedMenus()
{
    if (menuModel == nullptr)
        return;

    // Each menu as a submenu, as the bar would show them.
    juce::PopupMenu menus;
    const auto names = menuModel->getMenuBarNames();

    for (int i = 0; i < names.size(); ++i)
        menus.addSubMenu (names[i], menuModel->getMenuForIndex (i, names[i]));

    menus.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton),
                         [model = menuModel] (int result)
                         {
                             // Commands run themselves; other items (Open Recent's) go to the window.
                             if (result > 0)
                                 model->menuItemSelected (result, 0);
                         });
}

void TitleBar::showLayoutMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "view.layoutBeat", "view.layoutMix", "view.layoutFull" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "view.toggleBrowser", "view.toggleSong", "view.toggleEditor", "view.toggleInspector" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "view.maximizePanel", "view.fullScreen" });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (layoutButton));
}

void TitleBar::setWindowActions (std::optional<WindowActions> actions)
{
    windowActions = std::move (actions);
    minimiseButton = nullptr;
    maximiseButton = nullptr;
    closeButton = nullptr;

    if (windowActions.has_value())
    {
        minimiseButton = std::make_unique<WindowButton> ("Minimise", Icon::minimiseWindow, false);
        maximiseButton = std::make_unique<WindowButton> ("Maximise", Icon::maximiseWindow, false);
        closeButton = std::make_unique<WindowButton> ("Close", Icon::close, true);

        // Windows sends clicks on them to the window (MainWindow); screen readers press them here.
        minimiseButton->onClick = [this] { windowActions->minimise(); };
        maximiseButton->onClick = [this] { windowActions->toggleMaximised(); };
        closeButton->onClick = [this] { windowActions->close(); };

        for (auto* button : { minimiseButton.get(), maximiseButton.get(), closeButton.get() })
            addAndMakeVisible (*button);
    }

    timerCallback();
    resized();
}

void TitleBar::setProjectName (const juce::String& name, bool unsavedChanges)
{
    // A dot after it, as editors show unsaved changes.
    const auto text = unsavedChanges ? name + juce::String::fromUTF8 (" \xe2\x80\xa2") : name;

    if (projectName.getText() == text)
        return;

    projectName.setText (text, juce::dontSendNotification);
    projectName.setTooltip (unsavedChanges ? name + ": changed since it was saved" : name);
    resized();
}

void TitleBar::setWindowActive (bool isActive)
{
    if (active == isActive)
        return;

    active = isActive;
    searchBox->dimmed = ! active;

    for (auto* button : { minimiseButton.get(), maximiseButton.get(), closeButton.get() })
        if (button != nullptr)
            button->dimmed = ! active;

    for (auto* panelSwitch : { browserSwitch.get(), editorSwitch.get(), inspectorSwitch.get() })
        panelSwitch->dimmed = ! active;

    projectName.setAlpha (active ? 1.0f : Shell::inactiveAlpha);
    repaint();
}

void TitleBar::showPanels (const PanelLayout& layout)
{
    for (auto* panelSwitch : { browserSwitch.get(), editorSwitch.get(), inspectorSwitch.get() })
    {
        panelSwitch->setToggleState (layout.takesRoom (panelSwitch->panel), juce::dontSendNotification);
        panelSwitch->setEnabled (layout.canToggle (panelSwitch->panel));
    }
}

juce::Component& TitleBar::getSearchBox() noexcept
{
    return *searchBox;
}

void TitleBar::refreshTooltips()
{
    searchBox->setTooltip (
        withShortcut (commands, "Search commands: find and run any command by typing part of its name", "view.commandPalette"));
    menuButton.setTooltip (withShortcut (commands, "The menus: File, Edit, Add, View, Options and Help", "view.menus"));
    layoutButton.setTooltip ("Layout: arrange the panels, show or hide each, or fill the window with one");

    for (auto* panelSwitch : { browserSwitch.get(), editorSwitch.get(), inspectorSwitch.get() })
        panelSwitch->setTooltip (withShortcut (commands, "Shows or hides " + describePanel (panelSwitch->panel),
                                               getPanelCommandID ("toggle", panelSwitch->panel)));

    timerCallback();
}

void TitleBar::timerCallback()
{
    // Undo and Redo name the step they take, and are greyed while there is none.
    for (const auto& [button, id] : { std::pair<IconButton*, const char*> { &undoButton, "edit.undo" },
                                      std::pair<IconButton*, const char*> { &redoButton, "edit.redo" } })
    {
        const auto* command = commands.getRegistry().find (id);
        button->setEnabled (command != nullptr && CommandRegistry::isEnabled (*command));

        if (const auto tip = command != nullptr ? withShortcut (commands, getCurrentName (*command), id) : juce::String();
            button->getTooltip() != tip)
            button->setTooltip (tip);
    }

    if (maximiseButton != nullptr && windowActions.has_value() && windowActions->isMaximised != nullptr)
    {
        const auto maximised = windowActions->isMaximised();
        const auto title = juce::String (maximised ? "Restore" : "Maximise");

        if (maximiseButton->getTitle() != title)
        {
            maximiseButton->setTitle (title);
            maximiseButton->setIcon (maximised ? Icon::restoreWindow : Icon::maximiseWindow);
        }
    }
}

int TitleBar::getMenusWidth() const
{
    if (menuBar == nullptr || menuModel == nullptr)
        return 0;

    auto width = 0;
    const auto names = menuModel->getMenuBarNames();

    for (int i = 0; i < names.size(); ++i)
        width += getLookAndFeel().getMenuBarItemWidth (*menuBar, i, names[i]);

    return width;
}

juce::Component::WindowControlKind TitleBar::getControlAt (juce::Point<int> point) const
{
    using Kind = juce::Component::WindowControlKind;

    if (! getLocalBounds().contains (point))
        return Kind::client;

    if (minimiseButton != nullptr && minimiseButton->getBounds().contains (point))
        return Kind::minimise;

    if (maximiseButton != nullptr && maximiseButton->getBounds().contains (point))
        return Kind::maximise;

    if (closeButton != nullptr && closeButton->getBounds().contains (point))
        return Kind::close;

    // The bar's controls are the window's content; the rest, the project's name too, drags the window.
    for (const auto* control :
         std::initializer_list<const juce::Component*> { menuBar.get(), &menuButton, &undoButton, &redoButton, searchBox.get(),
                                                         browserSwitch.get(), editorSwitch.get(), inspectorSwitch.get(), &layoutButton })
        if (control != nullptr && control->isVisible() && control->getBounds().contains (point))
            return Kind::client;

    return Kind::caption;
}

void TitleBar::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

void TitleBar::resized()
{
    auto area = getLocalBounds();
    const auto buttonSize = controlHeight;
    const auto centred = [buttonSize] (juce::Rectangle<int> slot) { return slot.withSizeKeepingCentre (buttonSize, buttonSize); };

    // Right: the window's buttons, then the panels' switches and the Layout menu.
    if (closeButton != nullptr)
    {
        closeButton->setBounds (area.removeFromRight (Shell::windowButtonWidth));
        maximiseButton->setBounds (area.removeFromRight (Shell::windowButtonWidth));
        minimiseButton->setBounds (area.removeFromRight (Shell::windowButtonWidth));
    }

    area.removeFromRight (Space::s);
    layoutButton.setBounds (centred (area.removeFromRight (buttonSize)));
    area.removeFromRight (Space::xs);

    for (auto* panelSwitch : { inspectorSwitch.get(), editorSwitch.get(), browserSwitch.get() })
        panelSwitch->setBounds (centred (area.removeFromRight (buttonSize)));

    area.removeFromRight (Space::m);

    // Left: the menus, folded into one button where there isn't room for them beside the search.
    area.removeFromLeft (Space::xs);
    const auto middleWidth = 2 * buttonSize + Space::xs + Space::s + Shell::searchMinimumWidth;
    const auto menusWidth = getMenusWidth();
    const auto foldMenus = menuBar != nullptr && menusWidth + Space::m + middleWidth > area.getWidth();

    if (menuBar != nullptr)
    {
        menuBar->setVisible (! foldMenus);

        if (! foldMenus)
            menuBar->setBounds (area.removeFromLeft (menusWidth).withSizeKeepingCentre (menusWidth, buttonSize));
    }

    menuButton.setVisible (foldMenus);

    if (foldMenus)
        menuButton.setBounds (centred (area.removeFromLeft (buttonSize)));

    area.removeFromLeft (Space::s);

    // The project's name, where there is room for it beside the search.
    const auto nameWidth = juce::jmin (
        Shell::projectNameWidth, juce::roundToInt (juce::GlyphArrangement::getStringWidth (projectName.getFont(), projectName.getText()))
                                     + 2 * projectName.getBorderSize().getLeft() + 1);
    const auto showsName = projectName.getText().isNotEmpty() && area.getWidth() - nameWidth - Space::m >= middleWidth;
    projectName.setVisible (showsName);

    if (showsName)
    {
        projectName.setBounds (area.removeFromLeft (nameWidth));
        area.removeFromLeft (Space::m);
    }

    // The middle: the search, centred on the window if there is room, and Undo and Redo just left of it.
    const auto undoRedoWidth = 2 * buttonSize + Space::xs + Space::s;
    const auto first = area.getX() + undoRedoWidth; // where the search can begin, Undo and Redo before it
    const auto wanted = getWidth() * 2 / 5;
    const auto centredRoom = 2 * juce::jmin (getWidth() / 2 - first, area.getRight() - getWidth() / 2);
    int searchWidth = 0, searchX = 0;

    if (centredRoom >= Shell::searchMinimumWidth)
    {
        // Centred on the window, narrower if that is what keeps it there.
        searchWidth = juce::jlimit (Shell::searchMinimumWidth, juce::jmin (Shell::searchMaximumWidth, centredRoom), wanted);
        searchX = (getWidth() - searchWidth) / 2;
    }
    else
    {
        // As near the middle as the room allows.
        searchWidth = juce::jmin (juce::jmax (0, area.getRight() - first),
                                  juce::jlimit (Shell::searchMinimumWidth, Shell::searchMaximumWidth, wanted));
        searchX = juce::jlimit (first, juce::jmax (first, area.getRight() - searchWidth), (getWidth() - searchWidth) / 2);
    }

    searchBox->setBounds (searchX, (getHeight() - buttonSize) / 2, searchWidth, buttonSize);
    redoButton.setBounds (searchX - Space::s - buttonSize, (getHeight() - buttonSize) / 2, buttonSize, buttonSize);
    undoButton.setBounds (redoButton.getX() - Space::xs - buttonSize, redoButton.getY(), buttonSize, buttonSize);
}
} // namespace phazite::ui
