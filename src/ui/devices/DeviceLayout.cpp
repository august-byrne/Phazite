// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceLayout.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <utility>

namespace phazite::ui
{
namespace
{
    constexpr int partGap = Space::s, columnGap = Space::m;
    constexpr int tabBarHeight = 22, sideTabHeight = 24;

    // A page's tab, as a view area's tabs are (LookAndFeel::drawTabButton()), smaller: a pill behind the page that shows and
    // under the pointer, its name brighter. Never narrower than its name. The arrows move the keyboard focus along the bar, and
    // Space clicks it, as Enter does. The bar's More tab shows its icon after its name, which is the page shown while that is
    // one of those in its menu, and none otherwise.
    class PageTab : public juce::Button
    {
    public:
        PageTab (const juce::String& name, bool isMoreTab)
            : juce::Button (name),
              more (isMoreTab)
        {
            setWantsKeyboardFocus (true);
            setMouseClickGrabsKeyboardFocus (false); // a click leaves the keys where they were: Space still plays

            if (more)
            {
                setTitle ("More Pages");
                setTooltip ("More pages: those whose tabs don't fit");
            }
            else
            {
                setTitle (name);
                setTooltip ("Shows " + name);
                setClickingTogglesState (true);
            }
        }

        std::function<void (int)> onArrow; // -1 left, 1 right

        int getIdealWidth() const
        {
            const auto textWidth =
                getButtonText().isEmpty() ? 0 : juce::roundToInt (juce::GlyphArrangement::getStringWidth (getFont(), getButtonText()));
            const auto iconWidth = more ? juce::roundToInt (TabTint::pageMoreIcon) + (textWidth > 0 ? Space::xs : 0) : 0;
            return textWidth + iconWidth + 2 * TabTint::pageTextInset;
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key.isKeyCode (juce::KeyPress::leftKey) || key.isKeyCode (juce::KeyPress::rightKey))
            {
                if (onArrow != nullptr)
                    onArrow (key.isKeyCode (juce::KeyPress::leftKey) ? -1 : 1);

                return true;
            }

            if (key.isKeyCode (juce::KeyPress::spaceKey))
            {
                triggerClick();
                return true;
            }

            return juce::Button::keyPressed (key);
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

            const auto ink = front ? p.textBright : isMouseOver ? p.text : p.textMuted;
            auto inside = pill.reduced ((float) TabTint::pageTextInset, 0.0f);

            if (more)
                icon.draw (
                    g, inside.removeFromRight (TabTint::pageMoreIcon).withSizeKeepingCentre (TabTint::pageMoreIcon, TabTint::pageMoreIcon),
                    ink);

            g.setColour (ink);
            g.setFont (getFont());
            g.drawText (getButtonText(), more ? inside : pill, juce::Justification::centred, true);
        }

    private:
        bool more;
        TintedIcon icon { Icon::moreActions }; // as a view's tools' More button (ToolRow.h)

        static juce::Font getFont() { return juce::Font (juce::FontOptions (TextSize::small)); }
    };

    // A part's tab, where its column shows its parts one at a time: a button of a row or a list, as a choice's. A part that can
    // be switched off has its switch here, at the tab's left, as small as in its title, and its name faint while it is off; a
    // click on the switch switches it, and one beside it shows the part.
    class PartTab : public juce::TextButton
    {
    public:
        PartTab (const juce::String& name, juce::Button* switchOfPart)
            : juce::TextButton (name),
              partSwitch (switchOfPart)
        {
            setTitle (name);
            setTooltip ("Shows " + name);
        }

        bool isPartOff() const { return partSwitch != nullptr && ! partSwitch->getToggleState(); }

        // Its part's switch, if it has one, into it, as it shows.
        void holdSwitch()
        {
            if (partSwitch == nullptr)
                return;

            addAndMakeVisible (*partSwitch);
            resized();
        }

        void resized() override
        {
            if (partSwitch != nullptr && partSwitch->getParentComponent() == this)
                partSwitch->setBounds (getSwitchArea());
        }

        void paintButton (juce::Graphics& g, bool isMouseOver, bool isMouseDown) override
        {
            auto& lf = getLookAndFeel();
            lf.drawButtonBackground (g, *this, findColour (getToggleState() ? buttonOnColourId : buttonColourId), isMouseOver, isMouseDown);

            auto area = getLocalBounds().reduced (Space::xs, 0);

            if (partSwitch != nullptr)
                area.setLeft (getSwitchArea().getRight() + Space::xs);

            g.setFont (lf.getTextButtonFont (*this, getHeight()));
            g.setColour (findColour (getToggleState() ? textColourOnId : textColourOffId)
                             .withMultipliedAlpha (isPartOff() ? SwitchTint::partOff : 1.0f));
            g.drawFittedText (getButtonText(), area, juce::Justification::centred, 1);
        }

    private:
        juce::Component::SafePointer<juce::Button> partSwitch;

        juce::Rectangle<int> getSwitchArea() const
        {
            const auto size = std::min (DeviceLayout::titleHeight, getHeight());
            return getLocalBounds().withTrimmedLeft (Space::xs).removeFromLeft (size).withSizeKeepingCentre (size, size);
        }
    };

    // A part's switch heard as it switches (a click, an undo, automation as the ticker brings it up to date): its title or
    // tab shows it.
    struct SwitchWatch : private juce::Value::Listener
    {
        SwitchWatch (juce::Button& button, std::function<void()> switched)
            : state (button.getToggleStateValue()), // the button's own, not the song's: the same value, shared
              onSwitched (std::move (switched))
        {
            state.addListener (this);
        }

        ~SwitchWatch() override { state.removeListener (this); }

    private:
        juce::Value state;
        std::function<void()> onSwitched;

        void valueChanged (juce::Value&) override { onSwitched(); }
    };
} // namespace

struct DeviceLayout::ColumnState
{
    juce::OwnedArray<PartTab> tabs;
    std::vector<std::unique_ptr<SwitchWatch>> switchWatches;
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

            auto* partSwitch = dynamic_cast<juce::Button*> (part.titleSwitch);
            jassert ((partSwitch == nullptr) == (part.titleSwitch == nullptr)); // a switch is a button

            if (part.titleSwitch != nullptr)
                editor.addChildComponent (part.titleSwitch);

            // As it switches, its title, or its tab, faint or not.
            if (partSwitch != nullptr)
                state->switchWatches.push_back (std::make_unique<SwitchWatch> (*partSwitch,
                                                                               [this, column = i, index = p]
                                                                               {
                                                                                   const auto& switched = *states[column];

                                                                                   if (index < switched.titles.size())
                                                                                       editor.repaint (switched.titles[index]);

                                                                                   if (auto* tab = switched.tabs[(int) index])
                                                                                       tab->repaint();
                                                                               }));

            // Tabs only for a column of several parts.
            if (columns[i].size() > 1)
            {
                auto* tab = state->tabs.add (new PartTab (part.title, partSwitch));
                tab->setClickingTogglesState (true);
                tab->setRadioGroupId (group);
                tab->setToggleState (p == 0, juce::dontSendNotification);
                tab->onClick = [this, column = (int) i, index = (int) p] { showPart (column, index); };
                editor.addChildComponent (tab);
            }
        }

        states.push_back (std::move (state));
    }

    // The pages' More tab, which shows as the bar runs out of room.
    auto more = std::make_unique<PageTab> (juce::String(), true);
    more->onClick = [this] { showMorePagesMenu(); };
    more->onArrow = [this, tab = more.get()] (int step) { moveFocusAlongBar (*tab, step); };
    editor.addChildComponent (*more);
    moreTab = std::move (more);
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

    placePageBar (area.removeFromTop (pageBarHeight));
    area.removeFromTop (Space::xs);

    const auto& shown = pages[(size_t) shownPage];

    for (size_t i = 0; i < columns.size(); ++i)
        if (std::find (shown.begin(), shown.end(), i) == shown.end())
            hide (i);

    placeRow (shown, area);
}

void DeviceLayout::hide()
{
    for (size_t i = 0; i < columns.size(); ++i)
        hide (i);

    for (auto* tab : pageTabs)
        tab->setVisible (false);

    moreTab->setVisible (false);
}

void DeviceLayout::placePageBar (juce::Rectangle<int> bar)
{
    // Every page's tab, each as wide as its name, where they all fit; otherwise as many as fit before the More tab, which holds
    // the rest, and reads as the page shown while that is one of them.
    const auto count = pageTabs.size();
    std::vector<int> widths;
    auto all = 0;

    for (int i = 0; i < count; ++i)
    {
        widths.push_back (static_cast<PageTab*> (pageTabs[i])->getIdealWidth());
        all += (i > 0 ? Space::xs : 0) + widths.back();
    }

    auto* more = static_cast<PageTab*> (moreTab.get());
    const auto fitting = [&] (int moreWidth)
    {
        auto used = moreWidth, n = 0;

        while (n < count && used + widths[(size_t) n] + Space::xs <= bar.getWidth())
            used += widths[(size_t) n++] + Space::xs;

        return n;
    };

    pagesInBar = count;
    more->setButtonText ({});

    if (all > bar.getWidth())
    {
        pagesInBar = fitting (more->getIdealWidth());

        if (shownPage >= pagesInBar)
        {
            more->setButtonText (getPageName (shownPage));
            pagesInBar = fitting (more->getIdealWidth());
        }
    }

    for (int i = 0; i < count; ++i)
    {
        auto* tab = pageTabs[i];
        tab->setToggleState (i == shownPage, juce::dontSendNotification);
        tab->setVisible (i < pagesInBar);

        if (i < pagesInBar)
        {
            tab->setBounds (bar.removeFromLeft (widths[(size_t) i]));
            bar.removeFromLeft (Space::xs);
        }
    }

    const auto overflows = pagesInBar < count;
    const auto hiddenShown = overflows && shownPage >= pagesInBar;
    more->setTitle (hiddenShown ? "More Pages: " + getPageName (shownPage) : juce::String ("More Pages"));
    more->setToggleState (hiddenShown, juce::dontSendNotification);
    more->setVisible (overflows);

    if (overflows)
        more->setBounds (bar.removeFromLeft (std::min (more->getIdealWidth(), bar.getWidth())));
}

juce::PopupMenu DeviceLayout::getMorePagesMenu()
{
    // Its items call back into the layout once the menu has closed: only while its More tab, which it owns, is there.
    juce::PopupMenu menu;
    const juce::Component::SafePointer<juce::Component> tab (moreTab.get());

    for (int i = pagesInBar; i < getNumPages(); ++i)
        menu.addItem (getPageName (i), true, i == shownPage,
                      [this, tab, i]
                      {
                          if (tab != nullptr)
                              showPage (i);
                      });

    return menu;
}

void DeviceLayout::showMorePagesMenu()
{
    getMorePagesMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (moreTab.get()));
}

void DeviceLayout::moveFocusAlongBar (const juce::Component& from, int step)
{
    // The bar's tabs in order, the More tab last, those showing.
    std::vector<juce::Component*> bar;

    for (auto* tab : pageTabs)
        if (tab->isVisible())
            bar.push_back (tab);

    if (moreTab->isVisible())
        bar.push_back (moreTab.get());

    const auto at = std::find (bar.begin(), bar.end(), &from);

    if (at == bar.end())
        return;

    const auto index = (int) std::distance (bar.begin(), at) + step;

    if (juce::isPositiveAndBelow (index, (int) bar.size()))
        bar[(size_t) index]->grabKeyboardFocus();
}

juce::Button* DeviceLayout::getPageTab (int page) const
{
    return pageTabs[page];
}

juce::Button* DeviceLayout::getMoreTab() const noexcept
{
    return pages.empty() ? nullptr : moreTab.get();
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
    // While its page doesn't show.
    for (auto& part : columns[index])
        if (part.titleSwitch != nullptr)
            part.titleSwitch->setVisible (false);
}

void DeviceLayout::putSwitchesInTabs (size_t index)
{
    // Under tabs, a part's switch is in its tab, which shows whether it is on.
    for (auto* tab : states[index]->tabs)
        tab->holdSwitch();
}

void DeviceLayout::setPages (std::vector<std::vector<size_t>> newPages)
{
    if (newPages.size() < 2)
        newPages.clear();

    if (newPages == pages)
        return; // the bar as it is: its More tab keeps the focus it may have

    moreTab->setVisible (false); // until the bar is placed, if it shows

    pages = std::move (newPages);
    pageTabs.clear();
    pagesInBar = 0;
    shownPage = juce::jlimit (0, std::max (0, (int) pages.size() - 1), shownPage);

    static int nextRadioGroup = 0x1a6e0000; // each layout's page tabs a group of their own
    const auto group = ++nextRadioGroup;

    for (int i = 0; i < (int) pages.size(); ++i)
    {
        auto* tab = new PageTab (getPageName (i), false);
        pageTabs.add (tab);
        tab->setRadioGroupId (group);
        tab->onClick = [this, i] { showPage (i); };
        tab->onArrow = [this, tab] (int step) { moveFocusAlongBar (*tab, step); };
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

        // Its switch at its title's right end (back from its tab, if it was under tabs).
        if (part.titleSwitch != nullptr)
        {
            if (part.titleSwitch->getParentComponent() != &editor)
                editor.addChildComponent (part.titleSwitch);

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
    putSwitchesInTabs ((size_t) index);
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
    putSwitchesInTabs ((size_t) index);
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

        // Its parts' titles as given, in Title Case, as the look's pane headers are (SectionHeader.h): semibold, in the text
        // colour; faint while the part is off.
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));

        for (size_t t = 0; t < state.titles.size() && t < columns[i].size(); ++t)
        {
            if (state.titles[t].isEmpty())
                continue;

            g.setColour (p.text.withMultipliedAlpha (isPartOff ((int) i, (int) t) ? SwitchTint::partOff : 1.0f));
            g.drawText (getTitle ((int) i, (int) t), state.titles[t], juce::Justification::centredLeft, true);
        }
    }
}

int DeviceLayout::getNumParts (int column) const
{
    return juce::isPositiveAndBelow (column, (int) columns.size()) ? (int) columns[(size_t) column].size() : 0;
}

juce::String DeviceLayout::getTitle (int column, int part) const
{
    if (! juce::isPositiveAndBelow (part, getNumParts (column)))
        return {};

    return columns[(size_t) column][(size_t) part].title;
}

juce::Button* DeviceLayout::getPartTab (int column, int part) const
{
    return juce::isPositiveAndBelow (column, (int) states.size()) ? states[(size_t) column]->tabs[part] : nullptr;
}

bool DeviceLayout::isPartOff (int column, int part) const
{
    if (! juce::isPositiveAndBelow (part, getNumParts (column)))
        return false;

    const auto* partSwitch = dynamic_cast<const juce::Button*> (columns[(size_t) column][(size_t) part].titleSwitch);
    return partSwitch != nullptr && ! partSwitch->getToggleState();
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
