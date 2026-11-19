// SPDX-License-Identifier: AGPL-3.0-or-later
// How a device's editor lays out its parts (docs/design.md, section 9): columns side by side, each a stack of parts, each
// part a title over what it shows (a graph, controls, or both). Its width is the same at any height, so that a shorter dock
// never makes it wider: given less height than a column's parts take, the column shows one part at a time, under tabs of
// their titles (the one chosen stays chosen). A part whose height can shrink (a graph) gives way first, and one that can
// grow takes what is left over. Given a width narrower than its columns side by side, as the dock or a window may give it,
// its graphs narrow first, down to their least; narrower still, the columns wrap into rows, every part showing, where the
// height there is takes them; where it doesn't, they show in pages, as many columns on each as fit side by side, one page at
// a time under a bar of their names (the one chosen stays chosen), each part under its title still (the owner, 2026-11-06:
// every block that works in its own way names itself), so that the editor fits the dock without scrolling. The bar shows the
// pages' names that fit whole; the rest are in its More tab's menu, as a view's tools overflow into More (ToolRow.h). A part
// that can be switched off keeps its switch under tabs, in its tab. Titles are drawn as given, in Title Case.
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
        juce::String title;                     // shown over it, or on its tab, as given: in Title Case ("Sub and Noise")
        juce::Component* content = nullptr;     // the editor's
        int width = 0;                          // its width,
        int height = 0;                         // the height it wants,
        int minimumHeight = 0;                  // and the least it takes (as much, unless it shrinks); titles not included
        bool grows = false;                     // takes what the column has left over (a graph)
        int minimumWidth = 0;                   // the least width it narrows to (a graph's); 0 for one as wide always
        juce::Component* titleSwitch = nullptr; // what switches it on or off (a juce::Button: a PowerSwitch): at its title's right
                                                // end, or under tabs at its tab's left, its title or tab faint while it is off
    };

    using Column = std::vector<Part>;

    // In the editor that shows the parts: it adds their contents, and tab buttons where a column needs them.
    DeviceLayout (juce::Component& editor, std::vector<Column>);
    ~DeviceLayout();

    // At any height: the columns side by side, as wide as their parts want; and the least width that shows them side by
    // side, their graphs narrowed.
    int getWidth() const;
    int getNarrowestWidth() const;

    // The least height: each column's tallest part under its tabs, or its stack if that is all one part. And at a width: as
    // wide as the columns side by side, narrowed, that; narrower, that and the pages' bar, as it shows its columns in pages if
    // it is that short.
    int getMinimumHeight() const;
    int getMinimumHeightForWidth (int width) const;

    // Every part showing: the tallest column's stack, or with the columns wrapped to a narrower width, their rows'.
    int getHeightForWidth (int width) const;

    // The least width that shows every part: the widest column, narrowed, with the columns one under another or in pages.
    int getMinimumWidth() const;

    // Places the parts in an area of the editor, and remembers their titles' places for paint().
    void layOut (juce::Rectangle<int>);
    void paint (juce::Graphics&) const; // the parts' titles, and a line between columns

    // Hides every part, with its tabs, switches and pages' bar, until it is laid out again: for an editor that shows another
    // layout in its place (DeviceEditorBase::setModes()).
    void hide();

    // Whether a column shows its parts under tabs now; and which one it shows (for tests).
    bool usesTabs (int column) const;
    int getShownPart (int column) const;
    void showPart (int column, int part);

    // Its columns and their parts' titles, as given and as drawn (for tests); a part's tab, while its column has tabs, or
    // nullptr; and whether a part is off: it has a switch, off, which shows its title or tab faint.
    int getNumColumns() const noexcept { return (int) columns.size(); }
    int getNumParts (int column) const;
    juce::String getTitle (int column, int part) const;
    juce::Button* getPartTab (int column, int part) const;
    bool isPartOff (int column, int part) const;

    // Whether it shows its columns in pages now, how many, and the one it shows; and another one shown (its bar's click). A
    // page is named by its columns: each by its name, or by its first part's title.
    bool usesPages() const noexcept { return pages.size() > 1; }
    int getNumPages() const noexcept { return (int) pages.size(); }
    int getShownPage() const noexcept { return shownPage; }
    juce::String getPageName (int page) const;
    void showPage (int);

    // The pages' bar: each page's tab, and its More tab, which holds the pages whose tabs don't fit whole (nullptr while it
    // shows no pages); how many pages have tabs of their own now, the first so many; and the More tab's menu: the others, by
    // name, the one shown ticked. While one of them shows, the More tab reads as its name and is lit as a page's tab is.
    juce::Button* getPageTab (int page) const;
    juce::Button* getMoreTab() const noexcept;
    int getNumPagesInBar() const noexcept { return pagesInBar; }
    juce::PopupMenu getMorePagesMenu();

    // Makes a column show one part at a time at any height, with its tabs down its left side, as a list: for many short
    // parts, such as a synth's envelopes, LFOs and modulation.
    void setSideTabs (int column);

    // A column's name, which names its page where it is in pages (a list of side tabs, which has no title of its own).
    void setColumnName (int column, const juce::String&);

    // Makes a column lead every page, where it shows its columns in pages, beside the page's own: what picks which part of the
    // device the others show, such as a multiband's band. A page isn't named by it.
    void setPinned (int column);

    // What a part shows, or nullptr if there is no such part.
    juce::Component* getContent (int column, int part) const;

    static constexpr int titleHeight = 18, sideTabWidth = 76, pageBarHeight = 22;

private:
    struct ColumnState;

    juce::Component& editor;
    std::vector<Column> columns;
    std::vector<std::unique_ptr<ColumnState>> states;
    std::vector<std::vector<size_t>> pages; // the columns on each page, while it shows pages; none otherwise
    int shownPage = 0;
    juce::OwnedArray<juce::Button> pageTabs;
    std::unique_ptr<juce::Button> moreTab;
    int pagesInBar = 0;
    juce::Rectangle<int> laidOut; // the area it was last laid out in

    int getStackHeight (size_t column) const;
    int getStackMinimumHeight (size_t column) const;
    int getColumnMinimumHeight (size_t column) const; // stacked, or under its tabs
    int getWidthOf (size_t column) const;
    int getMinimumWidthOf (size_t column) const;
    std::vector<int> getWidthsOf (const std::vector<size_t>& row, int width) const; // narrowed to share the width
    // The columns in rows as wide, each as many as fit; as pages, each led by the pinned columns.
    std::vector<std::vector<size_t>> split (int width, bool asPages) const;
    bool hasSideTabs (size_t column) const;
    void placeRow (const std::vector<size_t>& row, juce::Rectangle<int>);
    void place (size_t column, juce::Rectangle<int>, bool leads);
    void placeStack (int column, juce::Rectangle<int>);
    void placeTabs (int column, juce::Rectangle<int>);
    void placeSideTabs (int column, juce::Rectangle<int>);
    void hide (size_t column);
    void hideSwitches (size_t column);
    void putSwitchesInTabs (size_t column);
    void setPages (std::vector<std::vector<size_t>>);
    void placePageBar (juce::Rectangle<int>);
    void showMorePagesMenu();
    void moveFocusAlongBar (const juce::Component& from, int step);
};
} // namespace phazite::ui
