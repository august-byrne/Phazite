// SPDX-License-Identifier: AGPL-3.0-or-later
// The activity bar down the window's left edge and the left panel's two views, the browser and the versions
// (docs/versions.md: What the user sees), in the whole window as the app builds it.
#include "AppScreen.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/mixer/MixerModel.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "ui/shell/ActivityBar.h"
#include "ui/versions/VersionsView.h"

namespace
{
using phazite::ui::LeftView;
using phazite::ui::Panel;

struct Screen : public phazite::test::AppScreen
{
    Screen()
    {
        session.openAtStartup();
        phazite::test::dispatchPendingMessages();
        waitForVersions();
    }

    phazite::ui::ActivityBar& getBar()
    {
        const auto bars = phazite::test::findAll<phazite::ui::ActivityBar> (shell);
        REQUIRE (bars.size() == 1);
        return *bars[0];
    }

    phazite::ui::VersionsView& getVersionsView()
    {
        // In the left panel, here or in its own window.
        const auto views = phazite::test::findAll<phazite::ui::VersionsView> (shell.getPanel (Panel::left));
        REQUIRE (views.size() == 1);
        return *views[0];
    }

    void click (juce::Component& component)
    {
        phazite::test::Mouse mouse (component);
        mouse.click (component.getLocalBounds().getCentre());
        phazite::test::dispatchPendingMessages();
    }

    void waitForVersions()
    {
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [this]
            {
                const auto* history = phazite::getVersionHistory (session.getContext());
                return history == nullptr || ! history->isBusy();
            }));
    }

    bool leftShows (LeftView view) { return shell.getPanel (Panel::left).isShowing() && shell.getPanelLayout().leftView == view; }
};
} // namespace

TEST_CASE ("The activity bar's icons show or hide their views in the left panel, and the bar stays")
{
    Screen screen;
    auto& bar = screen.getBar();
    CHECK (bar.isShowing());
    CHECK (bar.getX() == 0);
    CHECK (bar.getWidth() == phazite::ui::Shell::activityBarWidth);
    CHECK (screen.leftShows (LeftView::browser));
    CHECK (bar.isMarked (LeftView::browser));
    CHECK_FALSE (bar.isMarked (LeftView::versions));

    // The other icon: its view in the panel.
    screen.click (bar.getButton (LeftView::versions));
    CHECK (screen.leftShows (LeftView::versions));
    CHECK (screen.getVersionsView().isShowing());
    CHECK (bar.isMarked (LeftView::versions));
    CHECK_FALSE (bar.isMarked (LeftView::browser));

    // The icon of the view that shows: the panel hides, and no icon is marked; the bar stays.
    screen.click (bar.getButton (LeftView::versions));
    CHECK_FALSE (screen.shell.getPanel (Panel::left).isShowing());
    CHECK_FALSE (bar.isMarked (LeftView::versions));
    CHECK_FALSE (bar.isMarked (LeftView::browser));
    CHECK (bar.isShowing());

    // While it is hidden, any icon shows its view.
    screen.click (bar.getButton (LeftView::browser));
    CHECK (screen.leftShows (LeftView::browser));

    // The panel's own switch hides the view, and shows it again as it was.
    REQUIRE (screen.registry.invoke ("view.versions"));
    CHECK (screen.leftShows (LeftView::versions));
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    CHECK_FALSE (screen.shell.getPanel (Panel::left).isShowing());
    CHECK (bar.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    CHECK (screen.leftShows (LeftView::versions));

    // Another panel filling the window: the bar stays, and an icon brings the panels back.
    REQUIRE (screen.registry.invoke ("view.maximizeSong"));
    CHECK_FALSE (screen.shell.getPanel (Panel::left).isShowing());
    CHECK (bar.isShowing());
    CHECK_FALSE (bar.isMarked (LeftView::versions));
    screen.click (bar.getButton (LeftView::versions));
    CHECK (screen.leftShows (LeftView::versions));
    CHECK_FALSE (screen.shell.getPanelLayout().getMaximized().has_value());

    // The settings keep which view the panel shows.
    CHECK (screen.services.settings.get (phazite::Settings::panelLayout).contains ("leftView=versions"));
}

TEST_CASE ("The left panel in a window of its own shows the view an icon picks, there")
{
    Screen screen;
    auto& bar = screen.getBar();
    screen.shell.setDetached (Panel::left, true);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.shell.isDetached (Panel::left));

    screen.click (bar.getButton (LeftView::versions));
    CHECK (screen.shell.getPanelLayout().leftView == LeftView::versions);
    CHECK (screen.getVersionsView().isVisible());
    CHECK (bar.isMarked (LeftView::versions));

    // The same icon again leaves it in its window: closing the window puts the panel back.
    screen.click (bar.getButton (LeftView::versions));
    CHECK (screen.shell.isDetached (Panel::left));
    screen.shell.setDetached (Panel::left, false);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.leftShows (LeftView::versions));
}

TEST_CASE ("Save Version saves the project and a version, named as typed, and the history lists it")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("view.versions"));
    REQUIRE (screen.registry.invoke ("track.add")); // a new project starts with none
    auto& view = screen.getVersionsView();
    CHECK (view.getNumHistoryRows() == 1); // Now
    CHECK (view.describeHistoryRow (0) == "Now");

    view.getNameField().setText ("First idea");
    view.getNoteField().setText ("Drums only");
    phazite::test::dispatchPendingMessages(); // a text field tells what changed in it after the change
    CHECK (phazite::getVersionDraftName (context) == "First idea");
    screen.click (view.getSaveButton());
    screen.waitForVersions();
    phazite::test::dispatchMessagesUntil ([&view] { return view.getNumHistoryRows() == 2; });

    REQUIRE (view.getNumHistoryRows() == 2);
    CHECK (view.describeHistoryRow (1).startsWith ("First idea, now, on Main. Drums only"));
    CHECK (view.getNameField().getText().isEmpty()); // ready for the next one
    CHECK_FALSE (screen.session.isUntitled());       // its first save asked where it goes
    CHECK (screen.session.getProject()->getFolder().getChildFile ("Versions").isDirectory());
    CHECK (screen.shell.getStatusBar().getBranchText() == "Main");

    // A change since: the changes list names it, the Versions icon has its dot, and a click on it selects its track.
    const auto tracks = phazite::getTrackIDs (*phazite::findSong (context));
    REQUIRE_FALSE (tracks.empty());
    auto volume = phazite::getVolume (context, tracks.front());
    volume.setValue (0.4f);
    phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; });
    REQUIRE (view.getNumChanges() == 1);
    CHECK (view.describeChange (0).endsWith (": volume"));
    CHECK (view.getChangesHeading().startsWith ("CHANGES SINCE \"First idea\""));
    CHECK (screen.getBar().showsVersionsDot());

    context.getSongSelection().selectOnly (phazite::TrackID());
    auto& list = view.getChangesList();
    phazite::test::Mouse mouse (list);
    mouse.click (list.getRowPosition (0, true).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (context.getSongSelection().getTracks() == std::vector<phazite::TrackID> { tracks.front() });
}

TEST_CASE ("The activity bar's icons are switches screen readers name, with tooltips that name their keys")
{
    Screen screen;
    auto& bar = screen.getBar();

    for (const auto view : { LeftView::browser, LeftView::versions })
    {
        auto& button = bar.getButton (view);
        CHECK (button.getTitle() == phazite::ui::getLeftViewName (view));
        CHECK (button.isToggleable());
        CHECK (button.getTooltip().isNotEmpty());
        const auto handler = static_cast<juce::Component&> (button).createAccessibilityHandler(); // public as a component's
        REQUIRE (handler != nullptr);
        CHECK (handler->getRole() == juce::AccessibilityRole::toggleButton);
    }

    CHECK (bar.getButton (LeftView::browser).getToggleState());
}
