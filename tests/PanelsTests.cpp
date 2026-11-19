// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's areas of views (src/ui/shell/Panels.h; the owner, 2026-11-01: "the user should be able to drag the mixer
// tab down into the bottom panel, or the piano roll up"): where each view is, a tab dragged to the other area, the keys that
// show a view wherever it is, an area left without views, and Reset View Places.
#include "AppScreen.h"
#include "services/Settings.h"
#include "ui/components/ToolRow.h"
#include "ui/shell/Panels.h"
#include "ui/theme/LookAndFeel.h"

#include <vector>

using phazite::ui::Panel;
using phazite::ui::View;
using phazite::ui::ViewArea;
using phazite::ui::ViewPlaces;
using phazite::ui::ViewRow;
using phazite::ui::ViewTab;

TEST_CASE ("View places: each view in one area, moved before another or after them, and kept as text")
{
    ViewPlaces places;
    CHECK (places.getPlace (View::mixer) == phazite::ui::ViewPlace { Panel::song, 0 });
    CHECK (places.getPlace (View::devices) == phazite::ui::ViewPlace { Panel::editor, 0 });

    // The mixer down, after the lower area's views; the piano roll up, before the playlist.
    places.move (View::mixer, { Panel::editor, 0 });
    CHECK (places.upper[0] == std::vector { View::playlist });
    CHECK (places.lower[0] == std::vector { View::clipEditor, View::devices, View::mixer });
    places.move (View::clipEditor, { Panel::song, 0 }, 0);
    CHECK (places.upper[0] == std::vector { View::clipEditor, View::playlist });
    CHECK (places.lower[0] == std::vector { View::devices, View::mixer });

    // Along its own area: the devices after the mixer.
    places.move (View::devices, { Panel::editor, 0 }, 2);
    CHECK (places.lower[0] == std::vector { View::mixer, View::devices });

    // As the settings keep them, and back; anything that isn't every view once is where they start.
    CHECK (places.toString() == juce::String ("clipEditor,playlist;mixer,devices"));
    CHECK (ViewPlaces::fromString (places.toString()) == places);
    CHECK (ViewPlaces::fromString ({}) == ViewPlaces());
    CHECK (ViewPlaces::fromString ("playlist,mixer;clipEditor") == ViewPlaces());
    CHECK (ViewPlaces::fromString ("playlist,mixer,mixer;clipEditor,devices") == ViewPlaces());
    CHECK (ViewPlaces::fromString ("playlist,piano;clipEditor,devices,mixer") == ViewPlaces());
    CHECK (ViewPlaces::fromString (";playlist,mixer,clipEditor,devices").isEmpty (Panel::song));
}

TEST_CASE ("A view's tab dragged to the other area goes there and shows, the settings keep it, and its key shows it there")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto* upperRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::song));
    auto* lowerRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::editor));
    REQUIRE (upperRow != nullptr);
    REQUIRE (lowerRow != nullptr);
    auto* upper = &upperRow->getArea (0);
    auto* lower = &lowerRow->getArea (0);
    CHECK (upper->getTabNames() == juce::StringArray { "Playlist", "Mixer" });

    // The mixer's tab, pressed and moved, starts a drag that carries the mixer.
    auto* mixerTab = upper->getTabbedButtonBar().getTabButton (1);
    REQUIRE (mixerTab != nullptr);
    phazite::test::Mouse mouse (*mixerTab);
    mouse.press (mixerTab->getLocalBounds().getCentre());
    mouse.dragTo (mixerTab->getLocalBounds().getCentre() + juce::Point<int> (12, 0));
    CHECK (screen.shell.isDragAndDropActive());
    CHECK (phazite::ui::readViewDrag (screen.shell.getCurrentDragDescription()) == View::mixer);
    mouse.release();
    phazite::test::dispatchPendingMessages();

    // Over the lower area's views, a tint and where it would go; dropped, it is there, after them, and shows.
    const juce::DragAndDropTarget::SourceDetails mixerDrag (phazite::ui::makeViewDrag (View::mixer), nullptr,
                                                            lower->getLocalBounds().getCentre());
    REQUIRE (lower->isInterestedInDragSource (mixerDrag));
    lower->itemDragEnter (mixerDrag);
    CHECK (lower->getDropIndex() == -1);
    CHECK (lower->isShowing());
    lower->itemDropped (mixerDrag);
    CHECK_FALSE (lower->getDropIndex().has_value());
    CHECK (upper->getTabNames() == juce::StringArray { "Playlist" });
    CHECK (lower->getTabNames() == juce::StringArray { "Piano roll", "Instrument", "Effects", "Mixer" });
    CHECK (lower->getShownTab() == ViewTab::mixer);
    CHECK (screen.services.settings.get (phazite::Settings::viewPlaces) == juce::String ("playlist;clipEditor,devices,mixer"));

    // In the lower panel, F9 shows it, or hides the panel while it shows it, as F7 does the piano roll.
    lower->showTab (ViewTab::clipEditor);
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK (lower->getShownTab() == ViewTab::mixer);
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK_FALSE (lower->isShowing());
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK (lower->isShowing());
    CHECK (lower->getShownTab() == ViewTab::mixer);

    // The devices up, dropped on the upper area's first tab: Instrument and Effects go together, before the playlist, and F6
    // shows the instrument there.
    const juce::DragAndDropTarget::SourceDetails devicesDrag (phazite::ui::makeViewDrag (View::devices), nullptr, { 1, 1 });
    upper->itemDropped (devicesDrag);
    CHECK (upper->getTabNames() == juce::StringArray { "Instrument", "Effects", "Playlist" });
    CHECK (lower->getTabNames() == juce::StringArray { "Piano roll", "Mixer" });
    REQUIRE (screen.registry.invoke ("view.instrument"));
    CHECK (upper->getShownTab() == ViewTab::instrument);
}

TEST_CASE ("An area left without views hides, and Reset View Places puts every view back where it starts")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto* upperRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::song));
    auto* lowerRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::editor));
    REQUIRE (upperRow != nullptr);
    REQUIRE (lowerRow != nullptr);
    auto* upper = &upperRow->getArea (0);
    auto* lower = &lowerRow->getArea (0);
    const auto* reset = screen.registry.find ("view.resetViewPlaces");
    REQUIRE (reset != nullptr);
    CHECK_FALSE (reset->isEnabled());

    // Both of the upper area's views down: it hides, and the lower one fills the centre.
    for (const auto view : { View::playlist, View::mixer })
        lower->itemDropped ({ phazite::ui::makeViewDrag (view), nullptr, lower->getLocalBounds().getCentre() });

    CHECK (upper->isEmpty());
    CHECK_FALSE (upper->isShowing());
    CHECK (lower->isShowing());
    CHECK (lowerRow->getY() == 0);
    CHECK (reset->isEnabled());

    REQUIRE (screen.registry.invoke ("view.resetViewPlaces"));
    CHECK (upper->getTabNames() == juce::StringArray { "Playlist", "Mixer" });
    CHECK (lower->getTabNames() == juce::StringArray { "Piano roll", "Instrument", "Effects" });
    CHECK (upper->isShowing());
    CHECK (lower->isShowing());
    CHECK_FALSE (reset->isEnabled());
}

TEST_CASE ("View places: a view beside a row's area, at most two areas a row, a lone area on the left, and kept as text")
{
    ViewPlaces places;
    CHECK (places.hasRoomBeside (Panel::song));

    // The mixer to the right of the playlist: the upper row has two areas, and room for no more.
    places.placeBeside (View::mixer, Panel::song, false);
    CHECK (places.upper[0] == std::vector { View::playlist });
    CHECK (places.upper[1] == std::vector { View::mixer });
    CHECK (places.getPlace (View::mixer) == phazite::ui::ViewPlace { Panel::song, 1 });
    CHECK_FALSE (places.hasRoomBeside (Panel::song));
    CHECK (places.toString() == juce::String ("playlist|mixer;clipEditor,devices"));
    CHECK (ViewPlaces::fromString (places.toString()) == places);

    // The piano roll to the left of the devices, in the lower row; then beside a full row, it joins the area on that side.
    places.placeBeside (View::clipEditor, Panel::editor, true);
    CHECK (places.lower[0] == std::vector { View::clipEditor });
    CHECK (places.lower[1] == std::vector { View::devices });
    places.placeBeside (View::clipEditor, Panel::song, true);
    CHECK (places.upper[0] == std::vector { View::playlist, View::clipEditor });
    CHECK (places.lower[0] == std::vector { View::devices }); // the lone area went left
    CHECK (places.lower[1].empty());

    // The last views out of a row leave it empty; a view placed beside an empty row goes into it.
    places.move (View::devices, { Panel::song, 1 });
    CHECK (places.isEmpty (Panel::editor));
    places.placeBeside (View::mixer, Panel::editor, false);
    CHECK (places.lower[0] == std::vector { View::mixer });
    CHECK (ViewPlaces::fromString ("playlist|mixer|clipEditor;devices") == ViewPlaces()); // three areas in a row
}

TEST_CASE (
    "A tab dropped on an area's side tiles it beside, on its edge toward an empty row brings that row back, and the line between two areas resizes them")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto* upperRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::song));
    auto* lowerRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::editor));
    REQUIRE (upperRow != nullptr);
    REQUIRE (lowerRow != nullptr);
    auto& playlistArea = upperRow->getArea (0);
    auto& pianoArea = lowerRow->getArea (0);

    // The piano roll over the playlist's right edge: the side tinted, and dropped, it shows beside the playlist.
    const auto rightEdge = playlistArea.getLocalBounds().withTrimmedTop (60).getCentre().withX (playlistArea.getWidth() - 10);
    const juce::DragAndDropTarget::SourceDetails toRight (phazite::ui::makeViewDrag (View::clipEditor), nullptr, rightEdge);
    playlistArea.itemDragEnter (toRight);
    CHECK (playlistArea.getDrop() == ViewArea::Drop::right);
    playlistArea.itemDropped (toRight);
    auto& beside = upperRow->getArea (1);
    CHECK (beside.isShowing());
    CHECK (beside.getTabNames() == juce::StringArray { "Piano roll" });
    CHECK (beside.getX() > playlistArea.getRight());
    CHECK (std::abs (beside.getWidth() - playlistArea.getWidth()) <= phazite::ui::Shell::dividerSize + 1); // half each
    CHECK (screen.services.settings.get (phazite::Settings::viewPlaces) == juce::String ("playlist,mixer|clipEditor;devices"));

    // A full row's areas take no more beside them: the side of either is their tabs.
    const juce::DragAndDropTarget::SourceDetails devicesAtSide (phazite::ui::makeViewDrag (View::devices), nullptr, rightEdge);
    playlistArea.itemDragEnter (devicesAtSide);
    CHECK (playlistArea.getDrop() == ViewArea::Drop::tabs);
    playlistArea.itemDragExit (devicesAtSide);

    // The line between them, dragged right, gives the left more of the row.
    const auto leftBefore = playlistArea.getWidth();
    phazite::ui::PanelDivider* line = nullptr;

    for (auto* divider : phazite::test::findAll<phazite::ui::PanelDivider> (*upperRow))
        if (divider->isShowing())
            line = divider;

    REQUIRE (line != nullptr);
    phazite::test::Mouse mouse (*line);
    mouse.press (line->getLocalBounds().getCentre());
    mouse.dragTo (line->getLocalBounds().getCentre() + juce::Point<int> (60, 0));
    mouse.release();
    CHECK (playlistArea.getWidth() == leftBefore + 60);

    // Every view out of the lower row: it hides, and has nothing to drop on. The playlist's lower edge brings it back.
    playlistArea.itemDropped ({ phazite::ui::makeViewDrag (View::devices), nullptr, playlistArea.getLocalBounds().getCentre() });
    CHECK (lowerRow->isEmpty());
    CHECK_FALSE (lowerRow->isShowing());
    const auto lowerEdge = playlistArea.getLocalBounds().getCentre().withY (playlistArea.getHeight() - 10);
    const juce::DragAndDropTarget::SourceDetails toLower (phazite::ui::makeViewDrag (View::mixer), nullptr, lowerEdge);
    playlistArea.itemDragEnter (toLower);
    CHECK (playlistArea.getDrop() == ViewArea::Drop::otherRow);
    playlistArea.itemDropped (toLower);
    CHECK (lowerRow->isShowing());
    CHECK (pianoArea.getTabNames() == juce::StringArray { "Mixer" });
}

TEST_CASE ("Showing a row left without views brings its views back to it, as its toggle and its views' keys do")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto* upperRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::song));
    auto* lowerRow = dynamic_cast<ViewRow*> (&screen.shell.getPanel (Panel::editor));
    REQUIRE (upperRow != nullptr);
    REQUIRE (lowerRow != nullptr);

    // The lower row's views up: it hides, and its toggle brings the piano roll and the devices back to it.
    for (const auto view : { View::clipEditor, View::devices })
        upperRow->getArea (0).itemDropped (
            { phazite::ui::makeViewDrag (view), nullptr, upperRow->getArea (0).getLocalBounds().getCentre() });

    REQUIRE (lowerRow->isEmpty());
    REQUIRE (screen.registry.invoke (phazite::ui::getPanelCommandID ("toggle", Panel::editor)));
    CHECK (lowerRow->isShowing());
    CHECK (lowerRow->getArea (0).getTabNames() == juce::StringArray { "Piano roll", "Instrument", "Effects" });
    CHECK (upperRow->getArea (0).getTabNames() == juce::StringArray { "Playlist", "Mixer" });
}

TEST_CASE ("Each editor's tools are in its area's tab row: all of them at the default window size, the editor in use's tab marked, "
           "and they go with their view")
{
    // The owner's UX audit, 2026-11-12: not in the transport bar, where they would serve two editors at once and had no room,
    // but beside each editor's tabs, as Bitwig 6 puts each panel's tools along its top, winning back the row they took.
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& upper = dynamic_cast<ViewRow&> (screen.shell.getPanel (Panel::song)).getArea (0);
    auto& lower = dynamic_cast<ViewRow&> (screen.shell.getPanel (Panel::editor)).getArea (0);
    REQUIRE (upper.getShownTab() == ViewTab::playlist);
    REQUIRE (lower.getShownTab() == ViewTab::clipEditor);

    // The window at its first size, every panel showing: each row whole, right of its tabs and clear of the corner's buttons.
    for (auto* area : { &upper, &lower })
    {
        auto* tools = dynamic_cast<phazite::ui::ToolRow*> (area->getShownTools());
        REQUIRE (tools != nullptr);
        CHECK (tools->getParentComponent() == area);
        CHECK (tools->getY() == 0);
        CHECK (tools->getHeight() == area->getTabBarDepth());
        CHECK (tools->getX() >= area->getTabbedButtonBar().getRight());
        CHECK (tools->getIdealWidth() <= tools->getWidth());

        // The tabs as wide as they are at their best, none squeezed by the tools (JUCE workaround W29).
        auto& bar = area->getTabbedButtonBar();

        for (int i = 0; i < bar.getNumTabs(); ++i)
            if (auto* tab = bar.getTabButton (i); tab != nullptr && tab->isVisible())
                CHECK (tab->getWidth() == tab->getBestTabLength (bar.getHeight()));

        for (auto* child : tools->getChildren())
            if (child->isVisible())
                CHECK (area->getLocalArea (child, child->getLocalBounds()).getRight() <= tools->getRight());
    }

    CHECK (lower.getShownTools()->getRight()
           <= lower.getWidth() - phazite::ui::Shell::getPanelCornerWidth (phazite::ui::Shell::panelCornerButtons));

    // The playlist is the editor in use at first: its tab marked, the piano roll's not. A press in the piano roll's notes makes
    // it the one.
    const auto inUse = [] (ViewArea& area)
    { return phazite::ui::isTabInUse (*area.getTabbedButtonBar().getTabButton (area.getCurrentTabIndex())); };
    CHECK (inUse (upper));
    CHECK_FALSE (inUse (lower));

    auto* notes = phazite::test::findComponentWithTitle (lower, "Notes");
    REQUIRE (notes != nullptr);
    phazite::test::Mouse mouse (*notes);
    mouse.click ({ 4, 4 }, juce::ModifierKeys::rightButtonModifier); // a right-click on empty grid deletes nothing
    CHECK_FALSE (inUse (upper));
    CHECK (inUse (lower));

    // A tab that shows no editor (the mixer) has no tools beside it, and its area shows the playlist's again with it.
    upper.showTab (ViewTab::mixer);
    CHECK (upper.getShownTools() == nullptr);
    upper.showTab (ViewTab::playlist);
    REQUIRE (upper.getShownTools() != nullptr);

    // The piano roll moved up beside the playlist takes its tools with it.
    auto* pianoTools = lower.getShownTools();
    upper.itemDropped ({ phazite::ui::makeViewDrag (View::clipEditor), nullptr, { 1, 1 } });
    upper.showTab (ViewTab::clipEditor);
    CHECK (upper.getShownTools() == pianoTools);
    CHECK (pianoTools->getParentComponent() == &upper);
    CHECK (lower.getShownTools() == nullptr);
}

TEST_CASE ("A view area's tabs look like tabs and its tools like tools: a hairline stands between them, where there are tools")
{
    // The owner, 2026-11-19: "there is no visual separation or indication that the tabs in a panel (playlist, mixer) are
    // different from the actionable buttons that show up in certain views like the playlist (draw, cut, etc)". As VS Code's
    // editor group: tabs at the left, the shown one joined to its view; after a gap and a line, the actions.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& upper = dynamic_cast<ViewRow&> (screen.shell.getPanel (Panel::song)).getArea (0);
    REQUIRE (upper.getShownTab() == ViewTab::playlist);
    const auto& p = phazite::ui::getPalette (upper);

    // With tools: the line, in the border colour, as tall as the row less an inset at each end, right of the last tab; the first
    // tool right of it.
    auto* tools = upper.getShownTools();
    REQUIRE (tools != nullptr);
    const auto divider = upper.getToolsDivider();
    REQUIRE (! divider.isEmpty());
    auto& bar = upper.getTabbedButtonBar();
    CHECK (divider.getX() > bar.getTabButton (bar.getNumTabs() - 1)->getScreenBounds().getRight() - upper.getScreenX());
    CHECK (divider.getWidth() == phazite::ui::ToolRowTint::divider);
    CHECK (divider.getY() == phazite::ui::ToolRowTint::dividerInset);
    CHECK (divider.getBottom() == upper.getTabBarDepth() - phazite::ui::ToolRowTint::dividerInset);
    CHECK (tools->getX() > divider.getRight());

    for (auto* tool : tools->getChildren())
        if (tool->isVisible())
            CHECK (upper.getLocalArea (tool, tool->getLocalBounds()).getX() > divider.getRight());

    const auto row = upper.createComponentSnapshot ({ 0, 0, upper.getWidth(), upper.getTabBarDepth() });
    CHECK (row.getPixelAt (divider.getX(), divider.getCentreY()) == p.border);

    // The shown tab on the view's own background, with the accent's line along its top; another on the row's.
    auto* shown = bar.getTabButton (upper.getCurrentTabIndex());
    auto* other = bar.getTabButton (upper.getCurrentTabIndex() == 0 ? 1 : 0);
    REQUIRE (shown != nullptr);
    REQUIRE (other != nullptr);
    const auto tabImage = [] (juce::Component& tab) { return tab.createComponentSnapshot (tab.getLocalBounds()); };
    const auto shownTab = shown->getActiveArea();
    CHECK (tabImage (*shown).getPixelAt (shownTab.getX() + 2, shownTab.getBottom() - 6) == p.background);
    CHECK (tabImage (*shown).getPixelAt (shownTab.getCentreX(), shownTab.getY()) == p.accent);
    CHECK (tabImage (*other).getPixelAt (other->getActiveArea().getX() + 2, other->getActiveArea().getBottom() - 6).getAlpha() == 0);

    // Without tools (the mixer), no line.
    upper.showTab (ViewTab::mixer);
    CHECK (upper.getShownTools() == nullptr);
    CHECK (upper.getToolsDivider().isEmpty());
}
