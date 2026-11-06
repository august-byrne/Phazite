// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceLayout.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <numeric>

namespace phazite::ui
{
namespace
{
    constexpr int partGap = Space::s, columnGap = Space::m;
    constexpr int tabBarHeight = 22, sideTabHeight = 24;

    // A page's tab, as a view area's tabs are (LookAndFeel::drawTabButton()), smaller: a pill behind the page that shows and
    // under the pointer, its name brighter.
    class PageTab : public juce::Button
    {
    public:
        explicit PageTab (const juce::String& name)
            : juce::Button (name)
        {
            setTitle (name);
            setTooltip ("Shows " + name);
            setClickingTogglesState (true);
        }

        int getIdealWidth() const
        {
            return juce::roundToInt (juce::GlyphArrangement::getStringWidth (getFont(), getButtonText())) + 2 * TabTint::pageTextInset;
        }

        void paintButton (juce::Graphics& g, bool isMouseOver, bool isMouseDown) override
        {
            const auto& p = getPalette (*this);
            const auto front = getToggleState();
            const auto area = getLocalBounds().toFloat();
            const auto pill = area.withSizeKeepingCentre (area.getWidth(), std::min (TabTint::pagePillHeight, area.getHeight()));

            if (front || isMouseOver || isMouseDown)
            {
                g.setColour (front ? p.selectedInactive : p.hover);
                g.fillRoundedRectangle (pill, cornerRadius);
            }

            if (hasKeyboardFocus (false))
            {
                g.setColour (p.focus);
                g.drawRoundedRectangle (pill.reduced (0.5f), cornerRadius, 1.0f);
            }

            g.setColour (front ? p.textBright : isMouseOver ? p.text : p.textMuted);
            g.setFont (getFont());
            g.drawText (getButtonText(), pill, juce::Justification::centred, false);
        }

    private:
        static juce::Font getFont() { return juce::Font (juce::FontOptions (TextSize::small)); }
    };
} // namespace

struct DeviceLayout::ColumnState
{
    juce::OwnedArray<juce::TextButton> tabs;
    std::vector<juce::Rectangle<int>> titles; // the parts' titles, while stacked
    juce::Rectangle<int> area;                // none while its page doesn't show
    juce::String name;                        // its page's, where it leads one
    int shown = 0;
    bool tabbed = false, sideTabs = false;
    bool leads = false;  // first in its row or on its page: no line before it
    bool pinned = false; // leads every page
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

            if (part.titleSwitch != nullptr)
                editor.addChildComponent (part.titleSwitch);

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

void DeviceLayout::setColumnName (int column, const juce::String& name)
{
    if (juce::isPositiveAndBelow (column, (int) columns.size()))
        states[(size_t) column]->name = name;
}

void DeviceLayout::setPinned (int column)
{
    if (juce::isPositiveAndBelow (column, (int) columns.size()))
        states[(size_t) column]->pinned = true;
}

juce::Component* DeviceLayout::getContent (int column, int part) const
{
    if (! juce::isPositiveAndBelow (column, (int) columns.size())
        || ! juce::isPositiveAndBelow (part, (int) columns[(size_t) column].size()))
        return nullptr;

    return columns[(size_t) column][(size_t) part].content;
}

int DeviceLayout::getWidthOf (size_t column) const
{
    auto width = 0;

    for (const auto& part : columns[column])
        width = std::max (width, part.width);

    return width + (hasSideTabs (column) ? sideTabWidth + Space::xs : 0);
}

int DeviceLayout::getMinimumWidthOf (size_t column) const
{
    auto width = 0;

    for (const auto& part : columns[column])
        width = std::max (width, part.minimumWidth > 0 ? std::min (part.minimumWidth, part.width) : part.width);

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

    for (size_t n = 0; n < columns[column].size(); ++n)
        height += (n > 0 ? partGap : 0) + titleHeight + columns[column][n].height;

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

    for (size_t n = 0; n < columns[column].size(); ++n)
        height += (n > 0 ? partGap : 0) + titleHeight + columns[column][n].minimumHeight;

    return height;
}

int DeviceLayout::getColumnMinimumHeight (size_t column) const
{
    if (hasSideTabs (column) || columns[column].size() < 2)
        return getStackMinimumHeight (column);

    auto tallest = 0;

    for (const auto& part : columns[column])
        tallest = std::max (tallest, part.minimumHeight);

    return std::min (getStackMinimumHeight (column), tabBarHeight + Space::xs + tallest);
}

int DeviceLayout::getWidth() const
{
    auto width = 0;

    for (size_t i = 0; i < columns.size(); ++i)
        width += (i > 0 ? columnGap : 0) + getWidthOf (i);

    return width;
}

int DeviceLayout::getNarrowestWidth() const
{
    auto width = 0;

    for (size_t i = 0; i < columns.size(); ++i)
        width += (i > 0 ? columnGap : 0) + getMinimumWidthOf (i);

    return width;
}

int DeviceLayout::getMinimumHeight() const
{
    auto height = 0;

    for (size_t i = 0; i < columns.size(); ++i)
        height = std::max (height, getColumnMinimumHeight (i));

    return height;
}

int DeviceLayout::getMinimumHeightForWidth (int width) const
{
    if (columns.size() < 2 || width >= getNarrowestWidth())
        return getMinimumHeight();

    // In pages, under their bar.
    auto height = 0;

    for (size_t i = 0; i < columns.size(); ++i)
        height = std::max (height, getColumnMinimumHeight (i));

    return pageBarHeight + Space::xs + height;
}

int DeviceLayout::getMinimumWidth() const
{
    // The widest page: its column beside the pinned ones.
    auto pinned = 0, widest = 0;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (states[i]->pinned)
            pinned += getMinimumWidthOf (i) + columnGap;
        else
            widest = std::max (widest, getMinimumWidthOf (i));
    }

    return widest > 0 ? pinned + widest : std::max (0, pinned - columnGap);
}

std::vector<std::vector<size_t>> DeviceLayout::split (int width, bool asPages) const
{
    // Pages share the room the pinned columns leave them, which lead each.
    std::vector<size_t> pinned;
    auto room = width;

    for (size_t i = 0; i < columns.size() && asPages; ++i)
    {
        if (states[i]->pinned)
        {
            pinned.push_back (i);
            room -= getMinimumWidthOf (i) + columnGap;
        }
    }

    std::vector<std::vector<size_t>> rows;
    auto used = 0;

    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (asPages && states[i]->pinned)
            continue;

        const auto least = getMinimumWidthOf (i);

        if (! rows.empty() && used + columnGap + least <= room)
        {
            rows.back().push_back (i);
            used += columnGap + least;
        }
        else
        {
            rows.push_back ({ i });
            used = least;
        }
    }

    for (auto& row : rows)
        row.insert (row.begin(), pinned.begin(), pinned.end());

    return rows;
}

int DeviceLayout::getHeightForWidth (int width) const
{
    if (columns.size() < 2 || width >= getNarrowestWidth())
    {
        auto height = 0;

        for (size_t i = 0; i < columns.size(); ++i)
            height = std::max (height, getStackHeight (i));

        return height;
    }

    // Wrapped: rows of columns, each as tall as its tallest stack.
    auto total = 0;

    for (const auto& row : split (width, false))
    {
        auto rowHeight = 0;

        for (const auto i : row)
            rowHeight = std::max (rowHeight, getStackHeight (i));

        total += (total > 0 ? partGap : 0) + rowHeight;
    }

    return total;
}

std::vector<int> DeviceLayout::getWidthsOf (const std::vector<size_t>& row, int width) const
{
    // Each at least its least; what the width has to spare widens those that narrowed, alike, back to their own.
    auto least = 0, narrowed = 0;

    for (size_t n = 0; n < row.size(); ++n)
    {
        least += (n > 0 ? columnGap : 0) + getMinimumWidthOf (row[n]);
        narrowed += getWidthOf (row[n]) - getMinimumWidthOf (row[n]);
    }

    const auto spare = std::max (0, width - least);
    std::vector<int> widths;

    for (const auto i : row)
    {
        const auto most = getWidthOf (i) - getMinimumWidthOf (i);
        widths.push_back (getMinimumWidthOf (i) + (spare >= narrowed ? most : (int) ((juce::int64) spare * most / narrowed)));
    }

    return widths;
}

void DeviceLayout::placeRow (const std::vector<size_t>& row, juce::Rectangle<int> area)
{
    const auto widths = getWidthsOf (row, area.getWidth());

    for (size_t n = 0; n < row.size(); ++n)
    {
        if (n > 0)
            area.removeFromLeft (columnGap);

        place (row[n], area.removeFromLeft (widths[n]), n == 0);
    }
}

void DeviceLayout::place (size_t i, juce::Rectangle<int> columnArea, bool leads)
{
    states[i]->leads = leads;

    if (hasSideTabs (i))
        placeSideTabs ((int) i, columnArea);
    else if (columns[i].size() > 1 && columnArea.getHeight() < getStackMinimumHeight (i))
        placeTabs ((int) i, columnArea);
    else
        placeStack ((int) i, columnArea);
}

void DeviceLayout::layOut (juce::Rectangle<int> area)
{
    laidOut = area;
    std::vector<size_t> all (columns.size());
    std::iota (all.begin(), all.end(), size_t (0));

    // Side by side, the graphs narrowed as far as the width needs, where they fit.
    if (columns.size() < 2 || area.getWidth() >= getNarrowestWidth())
    {
        setPages ({});
        placeRow (all, area);
        return;
    }

    // Wrapped into rows, every part showing at the height it wants, where the height there is takes them.
    if (getHeightForWidth (area.getWidth()) <= area.getHeight())
    {
        setPages ({});

        for (const auto& row : split (area.getWidth(), false))
        {
            auto rowHeight = 0;

            for (const auto i : row)
                rowHeight = std::max (rowHeight, getStackHeight (i));

            placeRow (row, area.removeFromTop (rowHeight));
            area.removeFromTop (partGap);
        }

        return;
    }

    // In pages: as many columns on each as fit side by side, one page at a time under the bar of their names, each part under
    // its title still.
    setPages (split (area.getWidth(), true));

    if (pages.empty()) // one page: a column wider than the room, beside the pinned ones
    {
        placeRow (all, area);
        return;
    }

    auto bar = area.removeFromTop (pageBarHeight);
    area.removeFromTop (Space::xs);

    for (int i = 0; i < pageTabs.size(); ++i)
    {
        auto* tab = pageTabs[i];
        const auto width = static_cast<PageTab*> (tab)->getIdealWidth();
        tab->setToggleState (i == shownPage, juce::dontSendNotification);
        tab->setVisible (true);
        tab->setBounds (bar.removeFromLeft (std::min (width, std::max (0, bar.getWidth()))));
        bar.removeFromLeft (Space::xs);
    }

    const auto& shown = pages[(size_t) shownPage];

    for (size_t i = 0; i < columns.size(); ++i)
        if (std::find (shown.begin(), shown.end(), i) == shown.end())
            hide (i);

    placeRow (shown, area);
}

void DeviceLayout::hide (size_t index)
{
    auto& state = *states[index];
    state.area = {};
    state.titles.clear();

    for (auto* tab : state.tabs)
        tab->setVisible (false);

    for (auto& part : columns[index])
        if (part.content != nullptr)
            part.content->setVisible (false);

    hideSwitches (index);
}

void DeviceLayout::hideSwitches (size_t index)
{
    // Under tabs, a part has no title to hold its switch, nor while its page doesn't show.
    for (auto& part : columns[index])
        if (part.titleSwitch != nullptr)
            part.titleSwitch->setVisible (false);
}

void DeviceLayout::setPages (std::vector<std::vector<size_t>> newPages)
{
    if (newPages.size() < 2)
        newPages.clear();

    if (newPages == pages)
        return;

    pages = std::move (newPages);
    pageTabs.clear();
    shownPage = juce::jlimit (0, std::max (0, (int) pages.size() - 1), shownPage);

    static int nextRadioGroup = 0x1a6e0000; // each layout's page tabs a group of their own
    const auto group = ++nextRadioGroup;

    for (int i = 0; i < (int) pages.size(); ++i)
    {
        auto* tab = pageTabs.add (new PageTab (getPageName (i)));
        tab->setRadioGroupId (group);
        tab->onClick = [this, i] { showPage (i); };
        editor.addChildComponent (tab);
    }
}

juce::String DeviceLayout::getPageName (int page) const
{
    if (! juce::isPositiveAndBelow (page, (int) pages.size()))
        return {};

    juce::StringArray names;

    for (const auto column : pages[(size_t) page])
        if (! states[column]->pinned)
            names.add (states[column]->name.isNotEmpty() ? states[column]->name
                       : columns[column].empty()         ? juce::String()
                                                         : columns[column].front().title);

    names.removeEmptyStrings();
    return names.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 "));
}

void DeviceLayout::showPage (int page)
{
    if (! juce::isPositiveAndBelow (page, (int) pages.size()) || page == shownPage)
        return;

    shownPage = page;
    layOut (laidOut);
    editor.repaint();
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

    for (size_t n = 0; n < column.size(); ++n)
    {
        const auto& part = column[n];
        auto height = part.height;

        if (room < wanted && shrinkable > 0)
            height -= (wanted - room) * (part.height - part.minimumHeight) / shrinkable;
        else if (room > wanted && part.grows)
            height += (room - wanted) / growing;

        state.titles.push_back (area.removeFromTop (titleHeight));

        // Its switch at its title's right end.
        if (part.titleSwitch != nullptr)
        {
            part.titleSwitch->setVisible (true);
            part.titleSwitch->setBounds (
                state.titles.back().removeFromRight (titleHeight + Space::xs).withSizeKeepingCentre (titleHeight, titleHeight));
        }

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
    hideSwitches ((size_t) index);
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
    hideSwitches ((size_t) index);
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

        // A line before each column but the first in its row or on its page.
        if (! state.leads && ! state.area.isEmpty())
        {
            g.setColour (p.border);
            g.fillRect (state.area.getX() - columnGap / 2, state.area.getY(), 1, state.area.getHeight());
        }

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));

        for (size_t t = 0; t < state.titles.size() && t < columns[i].size(); ++t)
            if (! state.titles[t].isEmpty())
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
