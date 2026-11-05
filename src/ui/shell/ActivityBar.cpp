// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ActivityBar.h"
#include "model/versions/VersionsModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
juce::String getLeftViewCommandID (LeftView view)
{
    return view == LeftView::versions ? "view.versions" : "view.browser";
}

juce::String getLeftViewName (LeftView view)
{
    return view == LeftView::versions ? "Source Control" : "Browser";
}

namespace
{
    // An icon of the bar as VS Code's Modern UI draws them: on a rounded box in the inactive selection's colour, bright,
    // while its view shows; on the hover wash under the pointer; otherwise dimmed (still readable). The area it was drawn in.
    juce::Rectangle<float> drawIcon (juce::Graphics& g, juce::Component& item, juce::Drawable* drawable, juce::Colour& drawnIn, bool on,
                                     bool lit)
    {
        const auto& p = getPalette (item);
        const auto bounds = item.getLocalBounds().toFloat();

        if (on || lit)
        {
            g.setColour (on ? p.selectedInactive : p.hover);
            g.fillRoundedRectangle (bounds.withSizeKeepingCentre (Shell::activityBoxSize, Shell::activityBoxSize), cornerRadius);
        }

        const auto colour = on ? p.textBright : lit ? p.text : p.textMuted;
        const auto area = bounds.withSizeKeepingCentre ((float) Shell::activityIconSize, (float) Shell::activityIconSize);

        if (drawable != nullptr)
        {
            if (colour != drawnIn)
            {
                drawable->replaceColour (drawnIn, colour);
                drawnIn = colour;
            }

            drawable->drawWithin (g, area, juce::RectanglePlacement::centred, 1.0f);
        }

        return area;
    }
} // namespace

//==============================================================================
// An icon for one of the left panel's views: a switch, on while its view shows, which screen readers say so of ("Versions,
// switch, on").
class ActivityBar::Item : public juce::ToggleButton
{
public:
    Item (CommandBridge& bridge, LeftView shownView, Icon icon)
        : juce::ToggleButton (getLeftViewName (shownView)),
          commands (bridge),
          view (shownView),
          drawable (createIcon (icon))
    {
        setTitle (getLeftViewName (view));
        setComponentID (getLeftViewCommandID (view)); // what lessons point at (Lessons::Step::target)
        setClickingTogglesState (false);              // the command decides what shows
        setToggleable (true);
        setMouseClickGrabsKeyboardFocus (false);
        onClick = [this] { commands.invoke (getLeftViewCommandID (view)); };
    }

    void setMarked (bool marked)
    {
        if (marked != getToggleState())
            setToggleState (marked, juce::dontSendNotification);
    }

    // The count on its badge (0: none), which screen readers say after its name ("Source Control, 3 changes").
    void setBadge (int count)
    {
        if (std::exchange (badge, count) == count)
            return;

        setDescription (count > 0 ? juce::String (count) + (count == 1 ? " change" : " changes") : juce::String());
        repaint();
    }

    int getBadge() const noexcept { return badge; }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        const auto& p = getPalette (*this);
        const auto iconArea = drawIcon (g, *this, drawable.get(), drawableColour, getToggleState(), isHighlighted || isDown);

        // As VS Code counts the changes on its Source Control icon: a pill at the icon's lower right, in the accent color.
        if (badge > 0)
        {
            const auto count = badge > Shell::activityBadgeMost ? juce::String (Shell::activityBadgeMost) + "+" : juce::String (badge);
            g.setFont (juce::FontOptions (TextSize::badge, juce::Font::bold));
            const auto width = juce::jmax (Shell::activityBadgeHeight, juce::TextLayout::getStringWidth (g.getCurrentFont(), count)
                                                                           + Shell::activityBadgeHeight / 2.0f);
            const auto pill = juce::Rectangle<float> (width, Shell::activityBadgeHeight)
                                  .withCentre (iconArea.getBottomRight().translated (-1.0f, -2.0f))
                                  .constrainedWithin (getLocalBounds().toFloat());
            g.setColour (p.accent);
            g.fillRoundedRectangle (pill, Shell::activityBadgeHeight / 2.0f);
            g.setColour (p.textOnAccent);
            g.drawText (count, pill, juce::Justification::centred, false);
        }
    }

    void refreshTooltip()
    {
        const auto about =
            view == LeftView::versions
                ? juce::String ("Source Control: commit the song as it is now, see what changed since, and go back. Click again "
                                "to hide the left panel")
                : juce::String ("Browser: sounds, instruments, effects and presets. Click again to hide the left panel");
        setTooltip (withShortcut (commands, about, getLeftViewCommandID (view)));
    }

private:
    CommandBridge& commands;
    const LeftView view;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour drawableColour { juce::Colours::black }; // what the drawing is in now
    int badge = 0;
};

//==============================================================================
// An icon at the bar's foot, which opens its menu.
class ActivityBar::FootItem : public juce::Button
{
public:
    FootItem (const juce::String& name, Icon icon)
        : juce::Button (name),
          drawable (createIcon (icon))
    {
        setTitle (name);
        setMouseClickGrabsKeyboardFocus (false);
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        drawIcon (g, *this, drawable.get(), drawableColour, false, isHighlighted || isDown);
    }

private:
    std::unique_ptr<juce::Drawable> drawable;
    juce::Colour drawableColour { juce::Colours::black };
};

//==============================================================================
ActivityBar::ActivityBar (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      browserItem (std::make_unique<Item> (bridge, LeftView::browser, Icon::library)),
      versionsItem (std::make_unique<Item> (bridge, LeftView::versions, Icon::versions)),
      accountItem (std::make_unique<FootItem> ("Account", Icon::account)),
      settingsItem (std::make_unique<FootItem> ("Settings", Icon::settings))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Activity bar");

    accountItem->onClick = [this] { showAccountMenu(); };
    settingsItem->onClick = [this]
    { makeSettingsMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (settingsItem.get())); };

    for (auto* item :
         std::initializer_list<juce::Component*> { browserItem.get(), versionsItem.get(), accountItem.get(), settingsItem.get() })
        addAndMakeVisible (item);

    refreshTooltips();
    context.addListener (this);
    updateBadge();
}

ActivityBar::~ActivityBar()
{
    context.removeListener (this);
}

ActivityBar::Item& ActivityBar::getItem (LeftView view) const
{
    return view == LeftView::versions ? *versionsItem : *browserItem;
}

void ActivityBar::showLeftPanel (bool shows, LeftView view)
{
    browserItem->setMarked (shows && view == LeftView::browser);
    versionsItem->setMarked (shows && view == LeftView::versions);
}

void ActivityBar::refreshTooltips()
{
    browserItem->refreshTooltip();
    versionsItem->refreshTooltip();
    accountItem->setTooltip ("Account: none yet. Your projects and their history stay on this computer");
    settingsItem->setTooltip (withShortcut (commands, "Settings: audio, MIDI, keys, themes and the other settings", "options.settings"));
}

bool ActivityBar::isMarked (LeftView view) const
{
    return getItem (view).getToggleState();
}

int ActivityBar::getVersionsBadge() const
{
    return versionsItem->getBadge();
}

juce::Button& ActivityBar::getButton (LeftView view)
{
    return getItem (view);
}

juce::Button& ActivityBar::getAccountButton()
{
    return *accountItem;
}

juce::Button& ActivityBar::getSettingsButton()
{
    return *settingsItem;
}

juce::PopupMenu ActivityBar::makeSettingsMenu()
{
    // As VS Code's Manage menu: the command search, then the settings' windows, then the looks and the keys.
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "view.commandPalette" });
    menu.addSeparator();
    addCommandItems (
        menu, commands,
        { "options.settings", "options.audioSettings", "options.midiSettings", "options.keyboardShortcuts", "options.scanPlugins" });
    menu.addSeparator();

    juce::PopupMenu themes, keymaps;

    for (const auto* command : commands.getRegistry().getAll())
    {
        if (command->id.startsWith ("options.theme."))
            addCommandItem (themes, commands, command->id);
        else if (command->id.startsWith ("options.keymap."))
            addCommandItem (keymaps, commands, command->id);
    }

    menu.addSubMenu ("Theme", themes, themes.containsAnyActiveItems());
    menu.addSubMenu ("Keymap", keymaps, keymaps.containsAnyActiveItems());
    return menu;
}

void ActivityBar::showAccountMenu()
{
    // Accounts are to come (docs/versions.md: Later, the cloud on top): what there is to say of them now.
    juce::PopupMenu menu;
    menu.addSectionHeader ("Account");
    menu.addItem ("Accounts aren't built yet", false, false, nullptr);
    menu.addItem ("Your projects and their history stay on this computer", false, false, nullptr);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (accountItem.get()));
}

void ActivityBar::updateBadge()
{
    versionsItem->setBadge (countChanges (context)); // staged or not, each thing once
}

void ActivityBar::paint (juce::Graphics& g)
{
    // As the left panel beside it, in the same card, the line between them its own, as VS Code joins its activity bar to its
    // side bar.
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromRight (1));
}

void ActivityBar::resized()
{
    // The views' icons from the top, the account's and the settings' up from the foot, so far apart as VS Code's.
    auto area = getLocalBounds().withTrimmedRight (1).reduced (0, Shell::activityItemGap / 2);

    for (auto* item : { browserItem.get(), versionsItem.get() })
    {
        item->setBounds (area.removeFromTop (Shell::activityItemHeight));
        area.removeFromTop (Shell::activityItemGap);
    }

    settingsItem->setBounds (area.removeFromBottom (Shell::activityItemHeight));
    area.removeFromBottom (Shell::activityItemGap);
    accountItem->setBounds (area.removeFromBottom (Shell::activityItemHeight));
}
} // namespace phazite::ui
