// SPDX-License-Identifier: AGPL-3.0-or-later
// The activity bar down the window's left edge and the left panel's two views, the browser and the versions
// (docs/versions.md: What the user sees), in the whole window as the app builds it.
#include "AppScreen.h"
#include "TestClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/mixer/MixerModel.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
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

    // Save Version clicked with a name typed (or none), and the version's id once it is written.
    juce::String saveVersion (const juce::String& name = {})
    {
        auto& context = session.getContext();
        const auto before = phazite::getVersions (context).size();
        auto& view = getVersionsView();
        view.getNameField().setText (name);
        phazite::test::dispatchPendingMessages(); // a text field tells what changed in it after the change
        click (view.getSaveButton());
        REQUIRE (phazite::test::dispatchMessagesUntil (
            [&context, before] { return phazite::getVersions (context).size() > before && ! phazite::isSavingVersion (context); }));
        waitForVersions();
        return phazite::getLastVersion (context)->id;
    }

    // A command run on the branch the branch button's menu picks, or on the version the history selects (none for Now), as
    // the menus run them.
    void onBranch (const juce::String& name, const char* command)
    {
        for (const auto& branch : phazite::getBranches (session.getContext()))
            if (branch.name == name)
                phazite::selectBranch (session.getContext(), branch.id);

        REQUIRE (registry.invoke (command));
        waitForVersions();
    }

    void onVersion (const juce::String& version, const char* command)
    {
        phazite::selectVersion (session.getContext(), version);
        REQUIRE (registry.invoke (command));
        waitForVersions();
    }

    void setFirstTrackVolume (float value)
    {
        auto& context = session.getContext();
        auto volume = phazite::getVolume (context, phazite::getTrackIDs (*phazite::findSong (context)).front());
        volume.setValue (value);
    }
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

TEST_CASE ("A version selected in the history is what Bring Back and Take from This Version act on")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    view.getNameField().setText ("One");
    phazite::test::dispatchPendingMessages();
    screen.click (view.getSaveButton());
    screen.waitForVersions();
    view.getNameField().setText ("Two");
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("track.add"));
    screen.click (view.getSaveButton());
    screen.waitForVersions();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumHistoryRows() == 3; }));

    // A click on the older version selects it; Take from This Version lists what differs from it, until Done.
    auto& history = *phazite::test::findAll<juce::ListBox> (view).getLast();
    phazite::test::Mouse mouse (history);
    mouse.click (history.getRowPosition (2, true).getCentre());
    phazite::test::dispatchPendingMessages();
    REQUIRE (view.describeHistoryRow (2).startsWith ("One"));
    CHECK (phazite::getSelectedVersion (context) == phazite::getVersions (context).back().id);
    CHECK (phazite::getCurrentName (*screen.registry.find ("version.bringBack")) == "Bring Back \"One\"");

    REQUIRE (screen.registry.invoke ("version.takeFrom"));
    CHECK (view.getChangesHeading().startsWith ("TAKE FROM \"One\""));
    CHECK (view.getDoneButton().isShowing());
    CHECK (view.getNumChanges() == 1); // the track added since
    screen.click (view.getDoneButton());
    CHECK (view.getChangesHeading().startsWith ("CHANGES SINCE \"Two\""));
    CHECK_FALSE (view.getDoneButton().isShowing());

    // Bring Back: the song as it was then, one undo step.
    REQUIRE (screen.registry.invoke ("version.bringBack"));
    CHECK (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; }));
    CHECK (screen.registry.invoke ("edit.undo"));
}

TEST_CASE ("The branch button names the branch the song is on, and the history shows each version on its branch")
{
    Screen screen;
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    CHECK_FALSE (view.getBranchButton().isVisible()); // before the first version
    screen.saveVersion ("First");
    CHECK (view.getBranchButton().isShowing());
    CHECK (view.getBranchButton().getButtonText().startsWith ("Main"));

    // A branch from Now: the song goes on there, and its version is on its line.
    screen.onVersion ({}, "version.newBranch");
    CHECK (view.getBranchButton().getButtonText().startsWith ("Idea 2"));
    CHECK (screen.shell.getStatusBar().getBranchText() == "Idea 2");
    screen.setFirstTrackVolume (0.4f);
    screen.saveVersion ("Quieter");
    REQUIRE (view.getNumHistoryRows() == 3);
    CHECK (view.describeHistoryRow (1).startsWith ("Quieter, now, on Idea 2"));
    CHECK (view.describeHistoryRow (2).startsWith ("First, now, on Main"));

    // Back on Main, the same view shows it.
    screen.onBranch ("Main", "version.switchBranch");
    CHECK (view.getBranchButton().getButtonText().startsWith ("Main"));
    CHECK (screen.getVersionsView().getNumHistoryRows() == 3);
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds())); // its lines and dots drawn
}

TEST_CASE ("A merge's lines show ticked in the Versions view: a click leaves one out, and Merge takes the others")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    screen.saveVersion ("First");
    screen.onVersion ({}, "version.newBranch");
    screen.setFirstTrackVolume (0.4f);
    const auto quieter = screen.saveVersion ("Quieter");
    screen.onBranch ("Main", "version.switchBranch");

    screen.onVersion (quieter, "version.merge");
    REQUIRE (view.getLines() == phazite::ui::VersionsView::Lines::merge);
    CHECK (view.getChangesHeading().startsWith ("MERGE \"Quieter\" INTO Main"));
    CHECK (view.getMergeButton().isShowing());
    CHECK (view.getCancelButton().isShowing());
    REQUIRE (view.getNumChanges() == 1);
    CHECK (view.describeChange (0).startsWith ("Taken: "));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds()));

    auto& list = view.getChangesList();
    phazite::test::Mouse mouse (list);
    mouse.click (list.getRowPosition (0, true).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (view.describeChange (0).startsWith ("Left out: "));
    mouse.click (list.getRowPosition (0, true).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (view.describeChange (0).startsWith ("Taken: "));

    // Merge: the changes again, the merged line among them, until the version that finishes it.
    screen.click (view.getMergeButton());
    CHECK (view.getLines() == phazite::ui::VersionsView::Lines::changes);
    CHECK_FALSE (view.getMergeButton().isVisible());
    CHECK (phazite::isMerging (context));
    CHECK (view.getNumChanges() == 1);
    screen.saveVersion();
    CHECK_FALSE (phazite::isMerging (context));
    CHECK (view.describeHistoryRow (1).startsWith ("Merge \"Quieter\", now, on Main"));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds()));

    // Cancel closes a merge's list, taking nothing.
    screen.onVersion ({}, "version.newBranch");
    screen.setFirstTrackVolume (0.8f);
    const auto louder = screen.saveVersion ("Louder");
    screen.onBranch ("Main", "version.switchBranch");
    screen.onVersion (louder, "version.merge");
    REQUIRE (view.getLines() == phazite::ui::VersionsView::Lines::merge);
    screen.click (view.getCancelButton());
    CHECK (view.getLines() == phazite::ui::VersionsView::Lines::changes);
    CHECK_FALSE (phazite::isMerging (context));
    CHECK (view.getNumChanges() == 0);
}

TEST_CASE ("A merge's clashes list in the Versions view after Merge, each with whose value the song has, until the next version")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    screen.saveVersion ("First");
    screen.onVersion ({}, "version.newBranch");
    screen.setFirstTrackVolume (0.2f);
    const auto theirs = screen.saveVersion ("Much quieter");
    screen.onBranch ("Main", "version.switchBranch");
    screen.setFirstTrackVolume (0.4f);
    screen.saveVersion ("Quieter");

    screen.onVersion (theirs, "version.merge");
    REQUIRE (view.getNumChanges() == 1);
    CHECK (view.describeChange (0).endsWith (", clashes"));
    screen.click (view.getMergeButton());

    REQUIRE (view.getLines() == phazite::ui::VersionsView::Lines::clashes);
    CHECK (view.getChangesHeading() == "CLASHES  1");
    CHECK (view.describeChange (0).contains (", This branch: "));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds()));

    // A value chosen, as the clash's menu chooses it.
    REQUIRE (phazite::chooseClash (context, 0, 2));
    CHECK (view.describeChange (0).contains (", Idea 2: "));

    // The next version finishes the merge.
    screen.saveVersion();
    CHECK (view.getLines() == phazite::ui::VersionsView::Lines::changes);
    CHECK (view.getNumChanges() == 0);
}

TEST_CASE ("With a bounce, each version saved is bounced; its play mark hears it, the transport bar says so, and the room shows")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("view.versions"));
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto track = phazite::getTrackIDs (*phazite::findSong (context)).front();
    REQUIRE (
        phazite::test::addMidiClip (*phazite::findSong (context), track, { phazite::test::at (0.0), phazite::test::at (4.0) }).isValid());
    auto& view = screen.getVersionsView();

    // The box, which the settings keep.
    CHECK_FALSE (view.getBounceBox().getToggleState());
    screen.click (view.getBounceBox());
    CHECK (screen.services.settings.get (phazite::Settings::versionBounce));
    CHECK (phazite::getBounceWithVersion (context));

    screen.saveVersion ("Bounced");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.renders.finished.isEmpty(); }, 60000));
    CHECK (screen.renders.finished[0] == "Bouncing \"Bounced\": ok");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.describeHistoryRow (1).contains (", with a bounce"); }));
    CHECK (view.getRoomText().startsWith ("Versions take "));
    CHECK (view.getRoomText().contains (", bounces "));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds())); // the play marks drawn

    // Its play mark hears it, and the transport bar says so; Now's, the song again.
    auto& history = view.getHistoryList();
    phazite::test::Mouse mouse (history);
    mouse.click (view.getPlayMarkPosition (1).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getHeardVersion (context) == phazite::getLastVersion (context)->id);
    CHECK (view.describeHistoryRow (1).contains (", heard"));

    const auto hearing = [&screen]
    {
        for (auto* label : phazite::test::findAll<juce::Label> (screen.shell))
            if (label->getTitle() == "Hearing")
                return label->isVisible() ? label->getText() : juce::String();

        return juce::String ("(none)");
    };

    CHECK (hearing() == "Hearing: Bounced");
    mouse.click (view.getPlayMarkPosition (0).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::getHeardVersion (context).isEmpty());
    CHECK (hearing().isEmpty());
    CHECK (view.describeHistoryRow (0) == "Now, heard");
    phazite::stopAndReturn (*phazite::findSong (context));

    // An older version without one: bounced in the background, its row saying so while it renders.
    screen.click (view.getBounceBox()); // off
    REQUIRE (screen.registry.invoke ("track.add"));
    const auto third = screen.saveVersion ("Third");
    REQUIRE (screen.registry.invoke ("track.add"));
    screen.saveVersion ("Fourth");
    phazite::selectVersion (context, third);
    REQUIRE (screen.registry.invoke ("version.bounce"));
    REQUIRE (view.describeHistoryRow (2).startsWith ("Third"));
    CHECK (view.describeHistoryRow (2).contains (", bouncing"));
    CHECK (phazite::test::dispatchMessagesUntil ([&view] { return view.describeHistoryRow (2).contains (", with a bounce"); }, 60000));
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
