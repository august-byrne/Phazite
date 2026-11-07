// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser's filters (docs/design.md, section 11): under its search field, a chip for each, Type, Loop, Tempo and Key,
// then Clear. A chip opens a menu of the values the sounds the browser shows have, as the other filters and the search leave
// them, each with how many have it (services/BrowserModel.h: FilterOptions); set, it shows its value, and a click on its x
// takes it away. Clear, there while a filter is set or the search has words, takes them all away, the words with them.
#pragma once

#include "services/BrowserModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace phazite::ui
{
class FilterBar : public juce::Component
{
public:
    enum class Filter
    {
        type,
        loops,
        tempo,
        key
    };

    FilterBar();
    ~FilterBar() override;

    // What is set, and whether the search has words, as the chips and Clear show them.
    void show (const BrowserFilters&, bool searchHasWords);
    const BrowserFilters& getFilters() const noexcept { return filters; }

    // Asked as a menu opens: what the filters offer now. Told when a menu or a chip's x changed the filters, and of Clear.
    std::function<FilterOptions()> getOptions;
    std::function<void (const BrowserFilters&)> onChange;
    std::function<void()> onClear;

    // A filter's menu: its values, each with how many have it, the one set ticked (choosing it again takes it away).
    juce::PopupMenu getMenu (Filter);

    // How tall it is at a width, its chips going onto another row where they don't fit.
    int getHeightForWidth (int width) const;

    // For tests: a filter's chip, where its x is while it is set (in the chip), and Clear.
    juce::Button& getChip (Filter);
    juce::Rectangle<int> getClearArea (Filter);
    juce::Button& getClearButton() noexcept { return clearButton; }

    void resized() override;

private:
    class Chip;
    std::array<std::unique_ptr<Chip>, 4> chips;
    juce::TextButton clearButton { "Clear" };
    BrowserFilters filters;
    bool searchHasWords = false;

    void change (const BrowserFilters&);
    void showMenu (Filter);
    std::vector<juce::Rectangle<int>> layOut (int width) const; // the chips', then Clear's while it shows
};
} // namespace phazite::ui
