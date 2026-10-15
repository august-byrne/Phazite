// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CommandUI.h"
#include "model/arrangement/Tracks.h"

namespace phazite::ui
{
juce::String withShortcut (const CommandBridge& commands, const juce::String& text, const juce::String& commandID)
{
    // Up to two keys, such as a tool's letter and its digit.
    const auto keys = commands.getShortcutDescriptions (commandID);
    return keys.isEmpty() ? text : text + " (" + keys.joinIntoString (" or ", 0, 2) + ")";
}

void addCommandItem (juce::PopupMenu& menu, CommandBridge& commands, const juce::String& id)
{
    if (const auto commandID = commands.getCommandID (id); commandID != 0)
        menu.addCommandItem (&commands.getCommandManager(), commandID);
}

void addCommandItems (juce::PopupMenu& menu, CommandBridge& commands, std::initializer_list<const char*> ids)
{
    for (const auto* id : ids)
        addCommandItem (menu, commands, id);
}

void showColourMenu (juce::Component& target, juce::Colour current, std::function<void (juce::Colour)> onChosen)
{
    const auto& colours = getTrackColours();
    juce::PopupMenu menu;

    for (size_t i = 0; i < colours.size(); ++i)
    {
        auto swatch = std::make_unique<juce::DrawableRectangle>();
        swatch->setRectangle (juce::Parallelogram<float> (juce::Rectangle<float> (16.0f, 16.0f)));
        swatch->setCornerSize ({ 3.0f, 3.0f });
        swatch->setFill (colours[i].colour);

        juce::PopupMenu::Item item (colours[i].name);
        item.itemID = (int) i + 1;
        item.isTicked = colours[i].colour == current;
        item.image = std::move (swatch);
        menu.addItem (std::move (item));
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                        [safeTarget = juce::Component::SafePointer<juce::Component> (&target), callback = std::move (onChosen)] (int result)
                        {
                            if (safeTarget != nullptr && result > 0 && result <= (int) getTrackColours().size()) // 0: dismissed
                                callback (getTrackColours()[(size_t) result - 1].colour);
                        });
}
} // namespace phazite::ui
