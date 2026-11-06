// SPDX-License-Identifier: AGPL-3.0-or-later
// A section's heading in a side panel's view, which folds the section, as VS Code's: its chevron, its name and what it is
// about, its count's badge and, under the pointer, its actions, each a command's icon. A group is a row of a tree, as VS
// Code's Source Control shows its groups of changes; a pane is under a pane's header, taller, its name semibold, a line above
// it (SideBar). Its toggle state is whether the section is open; a click switches it, and onClick tells the view to lay
// itself out again.
#pragma once

#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace phazite::ui
{
class SectionHeader : public juce::Button
{
public:
    enum class Style
    {
        group,
        pane
    };

    SectionHeader (const juce::String& name, Style);
    ~SectionHeader() override;

    // Its name, what it is about (muted, after it) and the count its badge shows (none for 0), which screen readers say.
    void set (const juce::String& heading, const juce::String& detail, int count);
    juce::String getText() const { return detail.isEmpty() ? heading : heading + " " + detail; }

    // An action, at its right under the pointer, in the order added from the right; and whether they show then (not while
    // what the section lists can't take them).
    void addAction (Icon, const juce::String& name, std::function<void()> run);
    void setActionsAllowed (bool);

    // Its height, as VS Code's rows and panes' headers are; and whether a pane's header has its line above it: not the first
    // pane's of a view, as in VS Code.
    int getPreferredHeight() const noexcept;
    void setLineAbove (bool);

    void resized() override;
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    struct Hover;

    const Style style;
    juce::String heading, detail;
    int count = 0;
    TintedIcon open { Icon::expanded }, folded { Icon::folded };
    std::vector<std::unique_ptr<IconButton>> actions;
    bool actionsAllowed = true, lineAbove = true;
    std::unique_ptr<Hover> hover;

    juce::Rectangle<int> getContentArea() const; // inside the row's inset, and so far again from its right end
    void showActions (bool);
};

// A pill with a text in it, as VS Code counts a section's rows and names a branch on its last commit, at the right end of an
// area: its width, with or without drawing it.
int getBadgeWidth (const juce::String&);
int drawBadge (juce::Graphics&, juce::Rectangle<int> rightOf, const juce::String&, juce::Colour fill, juce::Colour text);
} // namespace phazite::ui
