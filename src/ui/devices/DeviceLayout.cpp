// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceLayout.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int partGap = Space::s, columnGap = Space::m;
    constexpr int tabBarHeight = 22, sideTabHeight = 24;
} // namespace

struct DeviceLayout::ColumnState
{
    juce::OwnedArray<juce::TextButton> tabs;
    std::vector<juce::Rectangle<int>> titles; // the parts' titles, while stacked
    juce::Rectangle<int> area;
    int shown = 0;
    bool tabbed = false, sideTabs = false;
};

DeviceLayout::DeviceLayout (juce::Component& e, std::vector<Column> c)
    : editor (e),
      columns (std::move (c))
{
    static int nextRadioGroup = 0x5e7a0000;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        auto state = std::make_unique<ColumnState>();
        const auto group = ++nextRadioGroup;

        for (size_t p = 0; p < columns[i].size(); ++p)
        {
            auto& part = columns[i][p];

            // Its title names what it shows, for screen readers.
            if (part.content != nullptr)
            {
                if (part.content->getTitle().isEmpty())
                    part.content->setTitle (part.title);

                editor.addAndMakeVisible (part.content);
            }

            // Tabs only for a column of several parts.
            if (columns[i].size() > 1)
            {
                auto* tab = state->tabs.add (new juce::TextButton (part.title));
                tab->setTitle (part.title);
                tab->setTooltip ("Shows " + part.title);
                tab->setClickingTogglesState (true);
                tab->setRadioGroupId (group);
                tab->setToggleState (p == 0, juce::dontSendNotification);
                tab->onClick = [this, column = (int) i, index = (int) p] { showPart (column, index); };
                editor.addChildComponent (tab);
            }
        }

        states.push_back (std::move (state));
    }
}

DeviceLayout::~DeviceLayout() = default;

bool DeviceLayout::hasSideTabs (size_t column) const
{
    return states[column]->sideTabs;
}

void DeviceLayout::setSideTabs (int column)
{
    if (! juce::isPositiveAndBelow (column, (int) columns.size()) || columns[(size_t) column].size() < 2)
        return;

    states[(size_t) column]->sideTabs = true;
}

int DeviceLayout::getWidthOf (size_t column) const
{
    auto width = 0;

    for (const auto& part : columns[column])
        width = std::max (width, part.width);

    return width + (hasSideTabs (column) ? sideTabWidth + Space::xs : 0);
}

int DeviceLayout::getStackHeight (size_t column) const
{
    auto height = 0;

    // A list of tabs: as tall as its tallest part, or its tabs.
    if (hasSideTabs (column))
    {
        for (const auto& part : columns[column])
            height = std::max (height, part.height);

        return std::max (height, (int) columns[column].size() * sideTabHeight);
    }

    for (const auto& part : columns[column])
        height += (height > 0 ? partGap : 0) + titleHeight + part.height;

    return height;
}

int DeviceLayout::getStackMinimumHeight (size_t column) const
{
    auto height = 0;

    if (hasSideTabs (column))
    {
        for (const auto& part : columns[column])
            height = std::max (height, part.minimumHeight);

        return std::max (height, (int) columns[column].size() * sideTabHeight);
    }

    for (const auto& part : columns[column])
        height += (height > 0 ? partGap : 0) + titleHeight + part.minimumHeight;

    return height;
}

int DeviceLayout::getWidth() const
{
    auto width = 0;

    for (size_t i = 0; i < columns.size(); ++i)
        width += (width > 0 ? columnGap : 0) + getWidthOf (i);

    return width;
}

int DeviceLayout::getMinimumHeight() const
{
    auto height = 0;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (hasSideTabs (i))
        {
            height = std::max (height, getStackMinimumHeight (i));
            continue;
        }

        auto tallest = 0;

        for (const auto& part : columns[i])
            tallest = std::max (tallest, part.minimumHeight);

        const auto tabbed = columns[i].size() > 1 ? tabBarHeight + Space::xs + tallest : getStackMinimumHeight (i);
        height = std::max (height, std::min (getStackMinimumHeight (i), tabbed));
    }

    return height;
}

int DeviceLayout::getHeightForWidth (int width) const
{
    if (width >= getWidth())
    {
        auto height = 0;

        for (size_t i = 0; i < columns.size(); ++i)
            height = std::max (height, getStackHeight (i));

        return height;
    }

    // Wrapped: rows of columns, each as tall as its tallest stack.
    auto total = 0, rowWidth = 0, rowHeight = 0;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto columnWidth = getWidthOf (i);

        if (rowWidth > 0 && rowWidth + columnGap + columnWidth > width)
        {
            total += (total > 0 ? partGap : 0) + rowHeight;
            rowWidth = rowHeight = 0;
        }

        rowWidth += (rowWidth > 0 ? columnGap : 0) + columnWidth;
        rowHeight = std::max (rowHeight, getStackHeight (i));
    }

    return total + (total > 0 ? partGap : 0) + rowHeight;
}

void DeviceLayout::layOut (juce::Rectangle<int> area)
{
    const auto place = [this] (size_t i, juce::Rectangle<int> columnArea)
    {
        if (hasSideTabs (i))
            placeSideTabs ((int) i, columnArea);
        else if (columns[i].size() > 1 && columnArea.getHeight() < getStackMinimumHeight (i))
            placeTabs ((int) i, columnArea);
        else
            placeStack ((int) i, columnArea);
    };

    if (area.getWidth() >= getWidth())
    {
        for (size_t i = 0; i < columns.size(); ++i)
        {
            if (i > 0)
                area.removeFromLeft (columnGap);

            place (i, area.removeFromLeft (getWidthOf (i)));
        }

        return;
    }

    // Wrapped, every part showing at the height it wants.
    auto rowTop = area.getY(), x = area.getX(), rowHeight = 0;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto columnWidth = getWidthOf (i);

        if (x > area.getX() && x + columnWidth > area.getRight())
        {
            rowTop += rowHeight + partGap;
            x = area.getX();
            rowHeight = 0;
        }

        const auto height = getStackHeight (i);
        place (i, { x, rowTop, columnWidth, height });
        x += columnWidth + columnGap;
        rowHeight = std::max (rowHeight, height);
    }
}

void DeviceLayout::placeStack (int index, juce::Rectangle<int> area)
{
    auto& column = columns[(size_t) index];
    auto& state = *states[(size_t) index];
    state.tabbed = false;
    state.area = area;
    state.titles.clear();

    for (auto* tab : state.tabs)
        tab->setVisible (false);

    // What the parts want, then shrunk as far as they can, or grown where they can, to the height there is.
    const auto titles = (int) column.size() * titleHeight + std::max (0, (int) column.size() - 1) * partGap;
    const auto room = std::max (0, area.getHeight() - titles);
    auto wanted = 0, shrinkable = 0, growing = 0;

    for (const auto& part : column)
    {
        wanted += part.height;
        shrinkable += part.height - part.minimumHeight;
        growing += part.grows ? 1 : 0;
    }

    for (const auto& part : column)
    {
        auto height = part.height;

        if (room < wanted && shrinkable > 0)
            height -= (wanted - room) * (part.height - part.minimumHeight) / shrinkable;
        else if (room > wanted && part.grows)
            height += (room - wanted) / growing;

        state.titles.push_back (area.removeFromTop (titleHeight));

        if (part.content != nullptr)
        {
            part.content->setVisible (true);
            part.content->setBounds (area.removeFromTop (std::max (0, height)));
        }
        else
        {
            area.removeFromTop (std::max (0, height));
        }

        area.removeFromTop (partGap);
    }
}

void DeviceLayout::placeTabs (int index, juce::Rectangle<int> area)
{
    auto& column = columns[(size_t) index];
    auto& state = *states[(size_t) index];
    state.tabbed = true;
    state.area = area;
    state.titles.clear();

    // The tabs share the column's width.
    auto bar = area.removeFromTop (tabBarHeight).reduced (0, 1);
    const auto tabWidth = bar.getWidth() / std::max (1, state.tabs.size());

    for (int i = 0; i < state.tabs.size(); ++i)
    {
        auto* tab = state.tabs[i];
        tab->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                                | (i + 1 < state.tabs.size() ? juce::Button::ConnectedOnRight : 0));
        tab->setVisible (true);
        tab->setToggleState (i == state.shown, juce::dontSendNotification);
        tab->setBounds (i + 1 < state.tabs.size() ? bar.removeFromLeft (tabWidth) : bar);
    }

    area.removeFromTop (Space::xs);

    for (size_t p = 0; p < column.size(); ++p)
    {
        auto& part = column[p];

        if (part.content == nullptr)
            continue;

        const auto shown = (int) p == state.shown;
        part.content->setVisible (shown);

        if (shown)
            part.content->setBounds (area.withHeight (part.grows ? area.getHeight() : std::min (area.getHeight(), part.height)));
    }
}

void DeviceLayout::placeSideTabs (int index, juce::Rectangle<int> area)
{
    auto& column = columns[(size_t) index];
    auto& state = *states[(size_t) index];
    state.tabbed = true;
    state.area = area;
    state.titles.clear();

    // The tabs down the left, as a list; the part shown beside them.
    auto list = area.removeFromLeft (sideTabWidth);
    area.removeFromLeft (Space::xs);

    for (int i = 0; i < state.tabs.size(); ++i)
    {
        auto* tab = state.tabs[i];
        tab->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnTop : 0)
                                | (i + 1 < state.tabs.size() ? juce::Button::ConnectedOnBottom : 0));
        tab->setVisible (true);
        tab->setToggleState (i == state.shown, juce::dontSendNotification);
        tab->setBounds (list.removeFromTop (sideTabHeight));
    }

    for (size_t p = 0; p < column.size(); ++p)
    {
        auto& part = column[p];

        if (part.content == nullptr)
            continue;

        const auto shown = (int) p == state.shown;
        part.content->setVisible (shown);

        if (shown)
            part.content->setBounds (area.withHeight (part.grows ? area.getHeight() : std::min (area.getHeight(), part.height)));
    }
}

void DeviceLayout::paint (juce::Graphics& g) const
{
    const auto& p = getPalette (editor);

    for (size_t i = 0; i < columns.size(); ++i)
    {
        const auto& state = *states[i];

        // A line before each column but the first.
        if (i > 0 && ! state.area.isEmpty())
        {
            g.setColour (p.border);
            g.fillRect (state.area.getX() - columnGap / 2, state.area.getY(), 1, state.area.getHeight());
        }

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));

        for (size_t t = 0; t < state.titles.size() && t < columns[i].size(); ++t)
            g.drawText (columns[i][t].title.toUpperCase(), state.titles[t], juce::Justification::centredLeft, true);
    }
}

bool DeviceLayout::usesTabs (int column) const
{
    return juce::isPositiveAndBelow (column, (int) states.size()) && states[(size_t) column]->tabbed;
}

int DeviceLayout::getShownPart (int column) const
{
    return juce::isPositiveAndBelow (column, (int) states.size()) ? states[(size_t) column]->shown : 0;
}

void DeviceLayout::showPart (int column, int part)
{
    if (! juce::isPositiveAndBelow (column, (int) states.size())
        || ! juce::isPositiveAndBelow (part, (int) columns[(size_t) column].size()))
        return;

    auto& state = *states[(size_t) column];
    state.shown = part;

    if (state.tabbed)
    {
        if (state.sideTabs)
            placeSideTabs (column, state.area);
        else
            placeTabs (column, state.area);

        editor.repaint();
    }
}
} // namespace phazite::ui
