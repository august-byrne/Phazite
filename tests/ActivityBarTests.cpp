// SPDX-License-Identifier: AGPL-3.0-or-later
// The activity bar down the window's left edge and the left panel's two views, the browser and the versions
// (docs/versions.md: What the user sees), in the whole window as the app builds it.
#include "AppScreen.h"
#include "TestClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "ui/shell/ActivityBar.h"
#include "ui/versions/BranchPicker.h"
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
        view.getMessageField().setText (name);
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
    CHECK (bar.getX() == phazite::ui::Shell::cardGap); // in its card, the cards' gap from the window's side
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

TEST_CASE ("A view an icon shows has the keys; hidden with them, they go to the playlist, never to the title bar's buttons")
{
    Screen screen;
    auto& bar = screen.getBar();
    auto& left = screen.shell.getPanel (Panel::left);

    for (const auto view : { LeftView::browser, LeftView::versions })
    {
        CAPTURE ((int) view);

        // Shown by its icon, the view has the keys (the browser's search, the commit's message).
        if (! screen.leftShows (view))
            screen.click (bar.getButton (view));

        if (! juce::Component::getCurrentlyFocusedComponent() || ! left.isParentOf (juce::Component::getCurrentlyFocusedComponent()))
        {
            screen.click (bar.getButton (view)); // hides it
            screen.click (bar.getButton (view)); // and shows it again, with the keys
        }

        REQUIRE (screen.leftShows (view));
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        REQUIRE (focused != nullptr);
        CHECK (left.isParentOf (focused));

        // Hidden by its icon while it has them: the playlist, the editor in use, takes them, and no button of the title bar
        // shows a focus ring.
        screen.click (bar.getButton (view));
        REQUIRE_FALSE (left.isShowing());
        focused = juce::Component::getCurrentlyFocusedComponent();
        REQUIRE (focused != nullptr);
        CHECK_FALSE (screen.shell.getTitleBar().isParentOf (focused));
        CHECK (dynamic_cast<phazite::ui::Playlist*> (focused) != nullptr);
    }
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

TEST_CASE ("Commit saves the project and commits, named as typed, and the graph lists it after the initial commit")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("view.versions"));
    REQUIRE (screen.registry.invoke ("track.add")); // a new project starts with none
    auto& view = screen.getVersionsView();
    CHECK (view.getNumHistoryRows() == 2); // the uncommitted changes, and the initial commit
    CHECK (view.describeHistoryRow (0) == "Uncommitted Changes");
    CHECK (view.describeHistoryRow (1).startsWith ("Initial commit, now, on main"));

    view.getMessageField().setText ("First idea\nDrums only");
    phazite::test::dispatchPendingMessages(); // a text field tells what changed in it after the change
    CHECK (phazite::getVersionDraftName (context) == "First idea");
    screen.click (view.getSaveButton());
    screen.waitForVersions();
    phazite::test::dispatchMessagesUntil ([&view] { return view.getNumHistoryRows() == 3; });

    REQUIRE (view.getNumHistoryRows() == 3);
    CHECK (view.describeHistoryRow (1).startsWith ("First idea, now, on main. Drums only"));
    CHECK (view.getMessageField().getText().isEmpty()); // ready for the next one
    CHECK_FALSE (screen.session.isUntitled());          // its first save asked where it goes
    CHECK (screen.session.getProject()->getFolder().getChildFile ("Versions").isDirectory());
    CHECK (screen.shell.getStatusBar().getBranchText() == "main");

    // A change since: the changes list names it with its letter, the Versions icon counts it, and a click on it selects its track.
    const auto tracks = phazite::getTrackIDs (*phazite::findSong (context));
    REQUIRE_FALSE (tracks.empty());
    auto volume = phazite::getVolume (context, tracks.front());
    volume.setValue (0.4f);
    phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; });
    REQUIRE (view.getNumChanges() == 1);
    CHECK (view.describeChange (0).endsWith (": volume"));
    CHECK (view.getChangeLetter (0) == "M");
    CHECK (view.getChangesHeading() == "Changes");
    CHECK (view.getChangesSection().getDescription() == "1"); // its badge's count
    CHECK (screen.getBar().getVersionsBadge() == 1);

    context.getSongSelection().selectOnly (phazite::TrackID());
    auto& list = view.getChangesList();
    phazite::test::Mouse mouse (list);
    mouse.click (list.getRowPosition (0, true).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (context.getSongSelection().getTracks() == std::vector<phazite::TrackID> { tracks.front() });
}

TEST_CASE ("A commit selected in the graph is what Restore and Compare with Working Tree act on")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    view.getMessageField().setText ("One");
    phazite::test::dispatchPendingMessages();
    screen.click (view.getSaveButton());
    screen.waitForVersions();
    view.getMessageField().setText ("Two");
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("track.add"));
    screen.click (view.getSaveButton());
    screen.waitForVersions();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumHistoryRows() == 4; }));

    // A click on the older commit selects it; Compare with Working Tree lists what differs from it, until Done.
    auto& history = view.getHistoryList();
    phazite::test::Mouse mouse (history);
    mouse.click (history.getRowPosition (2, true).getCentre());
    phazite::test::dispatchPendingMessages();
    REQUIRE (view.describeHistoryRow (2).startsWith ("One"));
    CHECK (phazite::getSelectedVersion (context) == phazite::getVersions (context)[1].id);
    CHECK (phazite::getCurrentName (*screen.registry.find ("version.bringBack")) == "Restore from \"One\"");

    REQUIRE (screen.registry.invoke ("version.takeFrom"));
    CHECK (view.getChangesHeading() == "Compare with \"One\"");
    CHECK (view.getDoneButton().isShowing());
    CHECK (view.getNumChanges() == 1); // the track added since
    screen.click (view.getDoneButton());
    CHECK (view.getChangesHeading() == "Changes");
    CHECK_FALSE (view.getDoneButton().isShowing());

    // Restore: the song as it was then, one undo step.
    REQUIRE (screen.registry.invoke ("version.bringBack"));
    CHECK (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; }));
    CHECK (screen.registry.invoke ("edit.undo"));
}

TEST_CASE ("The branch button names the branch the song is on from the start, and the graph shows each commit on its branch")
{
    Screen screen;
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    CHECK (view.getBranchButton().isShowing()); // main, from the initial commit
    screen.saveVersion ("First");
    CHECK (view.getBranchButton().getButtonText().startsWith ("main"));

    // A branch from Now: the song goes on there, and its version is on its line.
    screen.onVersion ({}, "version.newBranch");
    CHECK (view.getBranchButton().getButtonText().startsWith ("idea-2"));
    CHECK (screen.shell.getStatusBar().getBranchText() == "idea-2");
    screen.setFirstTrackVolume (0.4f);
    screen.saveVersion ("Quieter");
    REQUIRE (view.getNumHistoryRows() == 4);
    CHECK (view.describeHistoryRow (1).startsWith ("Quieter, now, on idea-2"));
    CHECK (view.describeHistoryRow (2).startsWith ("First, now, on main"));

    // Back on Main, the same view shows it.
    screen.onBranch ("main", "version.switchBranch");
    CHECK (view.getBranchButton().getButtonText().startsWith ("main"));
    CHECK (screen.getVersionsView().getNumHistoryRows() == 4);
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
    screen.onBranch ("main", "version.switchBranch");

    screen.onVersion (quieter, "version.merge");
    REQUIRE (view.getLines() == phazite::ui::VersionsView::Lines::merge);
    CHECK (view.getChangesHeading() == "Merge \"Quieter\" into main");
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
    CHECK (view.describeHistoryRow (1).startsWith ("Merge branch 'idea-2', now, on main"));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds()));

    // Abort closes a merge's list, taking nothing.
    screen.onVersion ({}, "version.newBranch");
    screen.setFirstTrackVolume (0.8f);
    const auto louder = screen.saveVersion ("Louder");
    screen.onBranch ("main", "version.switchBranch");
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
    screen.onBranch ("main", "version.switchBranch");
    screen.setFirstTrackVolume (0.4f);
    screen.saveVersion ("Quieter");

    screen.onVersion (theirs, "version.merge");
    REQUIRE (view.getNumChanges() == 1);
    CHECK (view.describeChange (0).endsWith (", conflict"));
    screen.click (view.getMergeButton());

    REQUIRE (view.getLines() == phazite::ui::VersionsView::Lines::clashes);
    CHECK (view.getChangesHeading() == "Merge Changes");
    CHECK (view.getChangesSection().getDescription() == "1"); // its badge's count
    CHECK (view.describeChange (0).contains (", Current: "));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds()));

    // A value chosen, as the clash's menu chooses it.
    REQUIRE (phazite::chooseClash (context, 0, 2));
    CHECK (view.describeChange (0).contains (", Incoming (idea-2): "));

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

    // Bounce Each Version, in Save Version's menu, which the settings keep.
    CHECK_FALSE (screen.registry.find ("version.bounceEach")->isTicked());
    REQUIRE (screen.registry.invoke ("version.bounceEach"));
    CHECK (view.getSaveButton().getButtonText() == "Commit and Bounce");
    CHECK (screen.services.settings.get (phazite::Settings::versionBounce));
    CHECK (phazite::getBounceWithVersion (context));

    screen.saveVersion ("Bounced");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.renders.finished.isEmpty(); }, 60000));
    CHECK (screen.renders.finished[0] == "Bouncing \"Bounced\": ok");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.describeHistoryRow (1).contains (", with a bounce"); }));
    CHECK (view.getRoomText().startsWith ("Commits take "));
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
    CHECK (view.describeHistoryRow (0) == "Uncommitted Changes, heard");
    phazite::stopAndReturn (*phazite::findSong (context));

    // An older version without one: bounced in the background, its row saying so while it renders.
    REQUIRE (screen.registry.invoke ("version.bounceEach")); // off
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

    for (const auto view : { LeftView::browser, LeftView::project, LeftView::versions })
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

TEST_CASE ("The account and the settings sit at the foot of the activity bar, the settings' menu holding the settings' commands")
{
    Screen screen;
    auto& bar = screen.getBar();
    auto& account = bar.getAccountButton();
    auto& settings = bar.getSettingsButton();
    CHECK (settings.getBottom() == bar.getHeight() - phazite::ui::Shell::activityItemGap / 2);
    CHECK (account.getBottom() + phazite::ui::Shell::activityItemGap == settings.getY());
    CHECK (account.getTitle() == "Account");
    CHECK (settings.getTitle() == "Settings");
    CHECK (account.getTooltip().isNotEmpty());
    CHECK (settings.getTooltip().isNotEmpty());

    // As VS Code's Manage menu: the command search, the settings' windows (the app's, which this screen hasn't), the looks.
    const auto menu = bar.makeSettingsMenu();
    CHECK (phazite::test::findMenuItem (menu, { "Search Commands" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Theme" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Keymap" }) != nullptr);
}

TEST_CASE ("Ctrl+Enter in the message saves a version, its first line the name; a change's Put Back shows under the pointer")
{
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    screen.saveVersion ("First");

    // A change: its row, its letter, and its Put Back only under the pointer (or selected).
    screen.setFirstTrackVolume (0.4f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; }));
    CHECK (view.getChangeLetter (0) == "M");
    CHECK (view.getPutBackPosition (0).isEmpty());
    auto& list = view.getChangesList();
    phazite::test::Mouse mouse (list);
    mouse.moveTo (list.getRowPosition (0, true).getCentre());
    REQUIRE_FALSE (view.getPutBackPosition (0).isEmpty());
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds())); // the row lit, its arrow drawn

    // The message: Enter makes a new line, and Ctrl+Enter saves it, named by its first line, noted by the rest.
    auto& message = view.getMessageField();
    message.grabKeyboardFocus();
    message.setText ("Quieter");
    phazite::test::dispatchPendingMessages(); // JUCE sets its text again from what it was given, a message later
    message.moveCaretToEnd();
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    message.insertTextAtCaret ("The verse was too loud");
    phazite::test::dispatchPendingMessages();
    CHECK (message.getText() == "Quieter\nThe verse was too loud");
    const auto before = phazite::getVersions (context).size();
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey, juce::ModifierKeys::commandModifier, 0));
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&context, before] { return phazite::getVersions (context).size() > before && ! phazite::isSavingVersion (context); }));
    screen.waitForVersions();
    CHECK (phazite::getLastVersion (context)->name == "Quieter");
    CHECK (phazite::getLastVersion (context)->note == "The verse was too loud");
    CHECK (message.getText().isEmpty());

    // Another change, put back by its arrow: one undo step, and no changes since.
    screen.setFirstTrackVolume (0.8f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; }));
    mouse.moveTo (list.getRowPosition (0, true).getCentre());
    mouse.click (view.getPutBackPosition (0).getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 0; }));
    CHECK (phazite::describeUndo (phazite::findSong (context)).startsWith ("Undo Discard Changes in"));

    // The sections fold under their headings.
    screen.click (view.getHistorySection());
    CHECK (view.getHistoryList().getBounds().isEmpty());
    screen.click (view.getHistorySection());
    CHECK_FALSE (view.getHistoryList().getBounds().isEmpty());
}

TEST_CASE ("The branch picker switches branches, starts one from now named as typed, and renames the branch the song is on")
{
    using phazite::ui::BranchPicker;
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    screen.saveVersion ("First");
    screen.onVersion ({}, "version.newBranch"); // idea-2, the song on it
    const auto type = [] (BranchPicker& picker, const juce::String& text)
    {
        picker.getSearchField().setText (text);
        phazite::test::dispatchPendingMessages(); // a text field tells what changed in it after the change
    };

    // Create new branch first, as VS Code's, then the branch the song is on, then the others.
    {
        BranchPicker picker (screen.commands, context, BranchPicker::Mode::pick);
        REQUIRE (picker.getNumRows() == 3);
        CHECK (picker.getNameForRow (0) == "Create new branch..., from the current commit, with the changes");
        CHECK (picker.getNameForRow (1) == "idea-2, current");
        CHECK (picker.getNameForRow (2) == "main, First, now");

        // What is typed narrows the branches; main, checked out.
        type (picker, "mai");
        REQUIRE (picker.getNumRows() == 2);
        CHECK (picker.getNameForRow (0).startsWith ("Create new branch 'mai'"));
        picker.choose (1);
        phazite::test::dispatchPendingMessages();
        screen.waitForVersions();
    }

    CHECK (phazite::getCurrentBranch (context)->name == "main");

    // A new branch, named as typed, as git keeps it.
    {
        BranchPicker picker (screen.commands, context, BranchPicker::Mode::pick);
        type (picker, "Half time");
        REQUIRE (picker.getNumRows() == 1);
        picker.choose (0);
        phazite::test::dispatchPendingMessages();
        screen.waitForVersions();
    }

    CHECK (phazite::getCurrentBranch (context)->name == "Half-time");
    CHECK (phazite::getBranches (context).size() == 3);

    // A name a branch has: that branch, and no new one of that name.
    {
        BranchPicker picker (screen.commands, context, BranchPicker::Mode::pick);
        type (picker, "main");
        REQUIRE (picker.getNumRows() == 1);
        CHECK (picker.getNameForRow (0).startsWith ("main, "));
    }

    // Renamed: not to another branch's name.
    {
        BranchPicker picker (screen.commands, context, BranchPicker::Mode::rename);
        CHECK (picker.getSearchField().getText() == "Half-time");
        type (picker, "idea 2");
        CHECK (picker.getNameForRow (0).endsWith ("another branch has that name, not now"));
        type (picker, "Half-time feel");
        picker.choose (0);
        phazite::test::dispatchPendingMessages();
    }

    CHECK (phazite::getCurrentBranch (context)->name == "Half-time-feel");
    CHECK (screen.shell.getStatusBar().getBranchText() == "Half-time-feel");
    CHECK_FALSE (phazite::renameBranch (context, phazite::getCurrentBranch (context)->id, "MAIN"));
}

TEST_CASE ("A change's plus stages it into Staged Changes, whose minus unstages it, as in VS Code")
{
    Screen screen;
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));
    auto& view = screen.getVersionsView();
    screen.saveVersion ("First");
    screen.setFirstTrackVolume (0.4f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumChanges() == 1; }));
    CHECK_FALSE (view.getStagedSection().isVisible()); // while nothing is staged

    // Its plus, under the pointer: staged, its letter M as git marks it, said as VS Code says it.
    auto& list = view.getChangesList();
    phazite::test::Mouse mouse (list);
    mouse.moveTo (list.getRowPosition (0, true).getCentre());
    REQUIRE_FALSE (view.getStagePosition (0).isEmpty());
    mouse.click (view.getStagePosition (0).getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumStaged() == 1; }));
    CHECK (view.getStagedSection().isVisible());
    CHECK (view.getNumChanges() == 0);
    CHECK (view.getStagedLetter (0) == "M");
    CHECK (view.describeStaged (0).endsWith (", Index Modified: volume"));
    juce::ignoreUnused (view.createComponentSnapshot (view.getLocalBounds())); // both lists drawn

    // Its minus: back in the changes.
    auto& staged = view.getStagedList();
    phazite::test::Mouse stagedMouse (staged);
    stagedMouse.moveTo (staged.getRowPosition (0, true).getCentre());
    REQUIRE_FALSE (view.getUnstagePosition (0).isEmpty());
    stagedMouse.click (view.getUnstagePosition (0).getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getNumStaged() == 0; }));
    CHECK_FALSE (view.getStagedSection().isVisible());
    CHECK (view.getNumChanges() == 1);
    CHECK (view.describeChange (0).endsWith (", Modified: volume"));
}
