// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite tells without asking anything (src/ui/shell/Notices.h; the owner's UX audit, 2026-11-12): news and problems in
// cards over the window's corner, where VS Code shows its notifications, rather than dialogs that stop everything.
#include "AppScreen.h"
#include "ui/shell/Notices.h"
#include "ui/theme/LookAndFeel.h"

TEST_CASE ("A notice tells news or a problem with a button for what to do next; news goes by itself, a problem stays")
{
    phazite::ui::Notices notices;
    phazite::test::TestWindow window (notices, 500, 600);
    auto changes = 0;
    notices.onChange = [&changes] { ++changes; };
    CHECK (notices.getNumNotices() == 0);
    CHECK (notices.getHeightFor (400) == 0);

    // A song exported: its title, what it says, and Show in Folder, which does that and closes it.
    auto shownInFolder = false;
    notices.show ({ .title = "Exported My Song",
                    .text = "Phazite wrote My Song.wav into the Exports folder.",
                    .actionName = "Show in Folder",
                    .action = [&shownInFolder] { shownInFolder = true; } });
    REQUIRE (notices.getNumNotices() == 1);
    CHECK (notices.isVisible());
    CHECK (changes == 1);
    CHECK (notices.getTitle (0) == "Exported My Song");
    CHECK_FALSE (notices.isProblem (0));
    CHECK (notices.getHeightFor (400) > 0);
    auto* action = notices.getActionButton (0);
    REQUIRE (action != nullptr);
    CHECK (action->getButtonText() == "Show in Folder");
    phazite::test::Mouse (*action).click (action->getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (shownInFolder);
    CHECK (notices.getNumNotices() == 0);
    CHECK_FALSE (notices.isVisible());

    // A problem, with no button but its X, and news over it; time passing takes the news and leaves the problem.
    notices.show ({ .title = "Autosave failed", .text = "Phazite couldn't save your changes automatically.", .isProblem = true });
    notices.show ({ .title = "Imported a.mid", .text = "Its notes are on a new track." });
    REQUIRE (notices.getNumNotices() == 2);
    CHECK (notices.getActionButton (0) == nullptr);
    CHECK (notices.isProblem (0));
    notices.countShown (phazite::ui::NoticeTint::shownSeconds + 1.0);
    REQUIRE (notices.getNumNotices() == 1);
    CHECK (notices.getTitle (0) == "Autosave failed");

    // Its X closes it.
    auto& close = notices.getCloseButton (0);
    phazite::test::Mouse (close).click (close.getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (notices.getNumNotices() == 0);

    // The same notice again takes the old one's place; at most three show, the oldest news going first, problems staying.
    notices.show ({ .title = "Save failed", .text = "The disk is full.", .isProblem = true });
    notices.show ({ .title = "One", .text = "News." });
    notices.show ({ .title = "One", .text = "News." });
    CHECK (notices.getNumNotices() == 2);
    notices.show ({ .title = "Two", .text = "News." });
    notices.show ({ .title = "Three", .text = "News." });
    REQUIRE (notices.getNumNotices() == phazite::ui::NoticeTint::mostShown);
    CHECK (notices.getTitle (0) == "Save failed");
    CHECK (notices.getTitle (1) == "Two");
    CHECK (notices.getTitle (2) == "Three");

    for (auto* card : notices.getChildren())
        phazite::test::checkChildrenApart (*card);
}

TEST_CASE ("The window's notices show over its bottom right corner, above the status bar")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& notices = screen.shell.getNotices();
    notices.show ({ .title = "Recordings recovered", .text = "1 recording that wasn't in the song yet is on a new track." });
    REQUIRE (notices.isVisible());
    CHECK (notices.getRight() <= screen.shell.getWidth());
    CHECK (notices.getBottom() <= screen.shell.getStatusBar().getY());
    CHECK (notices.getHeight() == notices.getHeightFor (notices.getWidth()));
}
