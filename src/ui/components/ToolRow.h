// SPDX-License-Identifier: AGPL-3.0-or-later
// A row of controls, each as wide as it asks, laid out from the left in the order added: a view's controls as they sit in its
// area's tab row, right of its tabs (ui/shell/Panels.h), as Bitwig 6 puts each panel's tools along its top. Where the row is
// too narrow, the least needed of them go, one at a time, into the More button at its end, whose menu offers each in its
// place, as VS Code's editor actions overflow into More Actions and Microsoft's command bars keep their most important
// commands in sight: nothing drops off its edge. The More button also offers what the row's owner always has there (the
// piano roll's notes menu); with that, it always shows. The controls are the owner's; the row only places them.
#pragma once

#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace phazite::ui
{
class ToolRow : public juce::Component
{
public:
    ToolRow();

    struct Item
    {
        juce::Component* control = nullptr;
        std::function<int()> getWidth;                    // as wide as it is now (a button's text can change)
        std::function<void (juce::PopupMenu&)> addToMenu; // what the More menu offers in its place while it doesn't fit
        int priority = 0;                                 // the least needed (lowest) leaves the row first
        int gapBefore = 0;                                // room between it and the control before it in the row
        std::function<bool()> isWanted;                   // whether it belongs in the row now at all; always, if unset
    };

    // Adds a control at the row's end; the row shows it and places it.
    void add (Item);

    // What the More button offers after what doesn't fit, whenever it is set: with it, the button always shows.
    void setMoreItems (std::function<void (juce::PopupMenu&)>, const juce::String& moreTooltip);

    // Lays the row out again, after a control's width, or whether it is wanted, changed.
    void update();

    // Whether a control shows in the row now, rather than in the More menu (for tests).
    bool isInRow (const juce::Component&) const;
    juce::Button& getMoreButton() noexcept { return moreButton; }

    // How wide the row is with every control it wants in it, and the More button if it always shows.
    int getIdealWidth() const;

    void resized() override;

private:
    std::vector<Item> items;
    std::vector<bool> inRow; // each item's, as laid out last
    std::function<void (juce::PopupMenu&)> moreItems;
    IconButton moreButton { "More", Icon::moreActions };

    bool isWanted (size_t index) const;
    void showMoreMenu();
};
} // namespace phazite::ui
