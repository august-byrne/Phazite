// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ToolRow.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <optional>

namespace phazite::ui
{
ToolRow::ToolRow()
{
    // Clicks between the controls go to what is behind the row: its area's tab bar.
    setInterceptsMouseClicks (false, true);

    moreButton.setQuiet (true);
    moreButton.setTooltip ("More: what doesn't fit in this row");
    moreButton.setMouseClickGrabsKeyboardFocus (false);
    moreButton.onClick = [this] { showMoreMenu(); };
    addChildComponent (moreButton);
}

void ToolRow::add (Item item)
{
    jassert (item.control != nullptr && item.getWidth != nullptr);
    addChildComponent (*item.control);
    items.push_back (std::move (item));
    inRow.push_back (false);
    resized();
}

void ToolRow::setMoreItems (std::function<void (juce::PopupMenu&)> addItems, const juce::String& moreTooltip)
{
    moreItems = std::move (addItems);
    moreButton.setTooltip (moreTooltip);
    resized();
}

void ToolRow::update()
{
    resized();
}

bool ToolRow::isWanted (size_t index) const
{
    return items[index].isWanted == nullptr || items[index].isWanted();
}

bool ToolRow::isInRow (const juce::Component& control) const
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].control == &control)
            return inRow[i] && control.isVisible();

    return false;
}

int ToolRow::getIdealWidth() const
{
    auto width = 0;
    auto first = true;

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (! isWanted (i))
            continue;

        width += (first ? 0 : items[i].gapBefore) + std::max (0, items[i].getWidth());
        first = false;
    }

    if (moreItems != nullptr)
        width += (first ? 0 : ToolRowTint::gap) + ToolRowTint::button;

    return width;
}

void ToolRow::resized()
{
    // Every control it wants; then, while they and the More button don't fit, the least needed leaves the row (the rightmost
    // of equals first), for the More menu.
    const auto count = items.size();
    std::vector<bool> wanted (count);
    std::vector<int> widths (count);
    inRow.assign (count, false);

    for (size_t i = 0; i < count; ++i)
    {
        wanted[i] = isWanted (i);
        inRow[i] = wanted[i];
        widths[i] = wanted[i] ? std::max (0, items[i].getWidth()) : 0;
    }

    const auto needsMore = [&]
    {
        if (moreItems != nullptr)
            return true;

        for (size_t i = 0; i < count; ++i)
            if (wanted[i] && ! inRow[i])
                return true;

        return false;
    };

    const auto rowWidth = [&]
    {
        auto width = 0;
        auto first = true;

        for (size_t i = 0; i < count; ++i)
        {
            if (! inRow[i])
                continue;

            width += (first ? 0 : items[i].gapBefore) + widths[i];
            first = false;
        }

        return needsMore() ? width + (first ? 0 : ToolRowTint::gap) + ToolRowTint::button : width;
    };

    while (rowWidth() > getWidth())
    {
        std::optional<size_t> out;

        for (size_t i = 0; i < count; ++i)
            if (inRow[i] && (! out.has_value() || items[i].priority <= items[*out].priority))
                out = i;

        if (! out.has_value())
            break;

        inRow[*out] = false;
    }

    // From the left, each in the middle of the row's height, the More button after them.
    const auto y = (getHeight() - ToolRowTint::button) / 2;
    auto x = 0;
    auto first = true;

    for (size_t i = 0; i < count; ++i)
    {
        auto& control = *items[i].control;
        control.setVisible (inRow[i]);

        if (! inRow[i])
            continue;

        x += first ? 0 : items[i].gapBefore;
        control.setBounds (x, y, widths[i], ToolRowTint::button);
        x += widths[i];
        first = false;
    }

    const auto moreX = x + (first ? 0 : ToolRowTint::gap);
    moreButton.setBounds (moreX, y, ToolRowTint::button, ToolRowTint::button);
    moreButton.setVisible (needsMore() && moreX + ToolRowTint::button <= getWidth());
}

void ToolRow::showMoreMenu()
{
    // What doesn't fit, each as the menu offers it, then what the owner always offers here.
    juce::PopupMenu menu;

    for (size_t i = 0; i < items.size(); ++i)
        if (! inRow[i] && isWanted (i) && items[i].addToMenu != nullptr)
            items[i].addToMenu (menu);

    if (moreItems != nullptr)
    {
        if (menu.getNumItems() > 0)
            menu.addSeparator();

        moreItems (menu);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton));
}
} // namespace phazite::ui
