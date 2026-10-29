// SPDX-License-Identifier: AGPL-3.0-or-later
// How a device's editor lays out its parts (docs/design.md, section 9): columns side by side, each a stack of parts, each
// part a title over what it shows (a graph, controls, or both). Its width is the same at any height, so that a shorter dock
// never makes it wider: given less height than a column's parts take, the column shows one part at a time, under tabs of
// their titles (the one chosen stays chosen). A part whose height can shrink (a graph) gives way first, and one that can
// grow takes what is left over. Given a width narrower than its columns side by side, as the Instrument tab or a window
// may give it, the columns wrap into rows instead, every part showing, and the editor is as tall as that takes.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace phazite::ui
{
class DeviceLayout
{
public:
    struct Part
    {
        juce::String title;                 // shown over it, or on its tab
        juce::Component* content = nullptr; // the editor's
        int width = 0;                      // its width,
        int height = 0;                     // the height it wants,
        int minimumHeight = 0;              // and the least it takes (as much, unless it shrinks); titles not included
        bool grows = false;                 // takes what the column has left over (a graph)
    };

    using Column = std::vector<Part>;

    // In the editor that shows the parts: it adds their contents, and tab buttons where a column needs them.
    DeviceLayout (juce::Component& editor, std::vector<Column>);
    ~DeviceLayout();

    // At any height: the columns side by side.
    int getWidth() const;

    // The least height: each column's tallest part under its tabs, or its stack if that is all one part.
    int getMinimumHeight() const;

    // Every part showing: the tallest column's stack, or with the columns wrapped to a narrower width, their rows'.
    int getHeightForWidth (int width) const;

    // The least width that shows every part: the widest column, with the columns one under another.
    int getMinimumWidth() const;

    // Places the parts in an area of the editor, and remembers their titles' places for paint().
    void layOut (juce::Rectangle<int>);
    void paint (juce::Graphics&) const; // the parts' titles, and a line between columns

    // Whether a column shows its parts under tabs now; and which one it shows (for tests).
    bool usesTabs (int column) const;
    int getShownPart (int column) const;
    void showPart (int column, int part);

    // Makes a column show one part at a time at any height, with its tabs down its left side, as a list: for many short
    // parts, such as a synth's envelopes, LFOs and modulation.
    void setSideTabs (int column);

    // What a part shows, or nullptr if there is no such part.
    juce::Component* getContent (int column, int part) const;

    static constexpr int titleHeight = 18, sideTabWidth = 76;

private:
    struct ColumnState;

    juce::Component& editor;
    std::vector<Column> columns;
    std::vector<std::unique_ptr<ColumnState>> states;

    int getStackHeight (size_t column) const;
    int getStackMinimumHeight (size_t column) const;
    int getWidthOf (size_t column) const;
    bool hasSideTabs (size_t column) const;
    void placeStack (int column, juce::Rectangle<int>);
    void placeTabs (int column, juce::Rectangle<int>);
    void placeSideTabs (int column, juce::Rectangle<int>);
};
} // namespace phazite::ui
