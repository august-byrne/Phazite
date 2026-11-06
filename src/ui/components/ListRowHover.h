// SPDX-License-Identifier: AGPL-3.0-or-later
// Which row of a list is under the pointer, for what a row shows only then, as VS Code's lists show a row's actions under the
// pointer: told to the list's owner as it changes (-1 for none).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>

namespace phazite::ui
{
struct ListRowHover : public juce::MouseListener
{
    ListRowHover (juce::ListBox& l, std::function<void (int previous, int row)> changed)
        : list (l),
          onChange (std::move (changed))
    {
        list.addMouseListener (this, true);
    }

    ~ListRowHover() override { list.removeMouseListener (this); }

    void mouseMove (const juce::MouseEvent& e) override { update (e); }
    void mouseEnter (const juce::MouseEvent& e) override { update (e); }
    void mouseDrag (const juce::MouseEvent& e) override { update (e); }
    void mouseExit (const juce::MouseEvent& e) override
    {
        // Out of a row's component into another's is a move; out of the list is none under the pointer.
        if (! list.getLocalBounds().contains (e.getEventRelativeTo (&list).getPosition()))
            setRow (-1);
    }

    int row = -1;

private:
    juce::ListBox& list;
    std::function<void (int, int)> onChange;

    void update (const juce::MouseEvent& e)
    {
        const auto position = e.getEventRelativeTo (&list).getPosition();
        setRow (list.getRowContainingPosition (position.x, position.y));
    }

    void setRow (int newRow)
    {
        if (newRow != row)
            onChange (std::exchange (row, newRow), newRow);
    }

    JUCE_DECLARE_NON_COPYABLE (ListRowHover)
};
} // namespace phazite::ui
