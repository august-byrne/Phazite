// SPDX-License-Identifier: AGPL-3.0-or-later
// The bar along the top of the window (src/ui/shell/TitleBar.h), in the window's content as the app builds it: Undo and
// Redo, the command search, the panels' switches, the menus folding in a narrow window, the project's name, and what the
// window's frame does where (the window itself, with its frame, is the app's: app/MainWindow.h).
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "model/arrangement/PlaylistModel.h"
#include "ui/shell/TitleBar.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
using Screen = phazite::test::AppScreen;
using Kind = juce::Component::WindowControlKind;

// Menus as the main window's are named, each empty.
struct Menus : public juce::MenuBarModel
{
    juce::StringArray getMenuBarNames() override { return { "File", "Edit", "Add", "View", "Options", "Help" }; }
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
    void menuItemSelected (int, int) override {}
};

// The window's buttons, counting what they are asked to do.
struct WindowActions
{
    int minimised = 0, toggled = 0, closed = 0;
    bool maximised = false;

    phazite::ui::TitleBar::WindowActions get()
    {
        return { .minimise = [this] { ++minimised; },
                 .toggleMaximised = [this] { ++toggled; },
                 .close = [this] { ++closed; },
                 .isMaximised = [this] { return maximised; } };
    }
};

juce::Component* findButton (juce::Component& bar, const juce::String& title)
{
    for (auto* button : phazite::test::findAll<juce::Button> (bar))
        if (button->getTitle() == title && button->isVisible())
            return button;

    return nullptr;
}
} // namespace

TEST_CASE ("The title bar's Undo and Redo name the step they take, and the search opens from the middle")
{
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();
    auto& bar = screen.shell.getTitleBar();
    auto* undo = dynamic_cast<juce::Button*> (findButton (bar, "Undo"));
    auto* redo = dynamic_cast<juce::Button*> (findButton (bar, "Redo"));
    REQUIRE (undo != nullptr);
    REQUIRE (redo != nullptr);
    phazite::test::runTimersUntil ([undo] { return ! undo->isEnabled(); }, 2000);
    CHECK_FALSE (undo->isEnabled());
    CHECK_FALSE (redo->isEnabled());

    // A track added: Undo names it, and takes it away; then Redo can.
    const auto tracks = phazite::getAllPlaylistTracks (screen.session.getContext()).size();
    REQUIRE (screen.registry.invoke ("track.add"));
    phazite::test::runTimersUntil ([undo] { return undo->isEnabled(); }, 2000);
    CHECK (undo->getTooltip().startsWith ("Undo Add Track"));
    phazite::test::Mouse mouse (*undo);
    mouse.click (undo->getLocalBounds().getCentre());
    CHECK (phazite::getAllPlaylistTracks (screen.session.getContext()).size() == tracks);
    phazite::test::runTimersUntil ([redo] { return redo->isEnabled(); }, 2000);
    CHECK (redo->isEnabled());

    // The search in the middle of the bar, with Undo and Redo just left of it.
    auto& search = bar.getSearchBox();
    CHECK (std::abs (search.getBounds().getCentreX() - bar.getWidth() / 2) <= 1);
    CHECK (redo->getRight() < search.getX());
    CHECK (undo->getRight() < redo->getX());
    CHECK (search.getTitle() == juce::String ("Search commands"));
}

TEST_CASE ("The title bar's switches show which panels show, and show or hide them")
{
    Screen screen;
    auto& bar = screen.shell.getTitleBar();
    auto* browserSwitch = dynamic_cast<juce::Button*> (findButton (bar, "Browser"));
    REQUIRE (browserSwitch != nullptr);
    CHECK (browserSwitch->getToggleState());

    phazite::test::Mouse mouse (*browserSwitch);
    mouse.click (browserSwitch->getLocalBounds().getCentre());
    CHECK_FALSE (screen.shell.getPanelLayout().isShown (phazite::ui::Panel::browser));
    CHECK_FALSE (browserSwitch->getToggleState());
    mouse.pause();
    mouse.click (browserSwitch->getLocalBounds().getCentre());
    CHECK (screen.shell.getPanel (phazite::ui::Panel::browser).isShowing());
    CHECK (browserSwitch->getToggleState());

    // A maximized panel leaves the others' switches unlit: they are out of sight.
    REQUIRE (screen.registry.invoke ("view.maximizeSong"));
    CHECK_FALSE (browserSwitch->getToggleState());
}

TEST_CASE ("The title bar's menus fold into one button in a narrow window, and it says where the window is dragged")
{
    Menus menus; // before the window, which shows them, so that they outlive it
    WindowActions actions;
    phazite::ui::LookAndFeel lookAndFeel; // the app's, which sizes the menus
    Screen screen;
    screen.window.setLookAndFeel (&lookAndFeel);
    auto& bar = screen.shell.getTitleBar();
    bar.setMenus (&menus);
    bar.setWindowActions (actions.get());
    bar.setProjectName ("My Song", true);
    CHECK (bar.getProjectName() == juce::String::fromUTF8 ("My Song \xe2\x80\xa2"));

    auto* menuBar = phazite::test::findAll<juce::MenuBarComponent> (bar).getFirst();
    REQUIRE (menuBar != nullptr);
    CHECK (menuBar->isVisible());
    CHECK (findButton (bar, "Menus") == nullptr);
    phazite::test::checkChildrenApart (bar);

    // The search stays in the middle of the window, narrower beside the menus and the name.
    CHECK (std::abs (bar.getSearchBox().getBounds().getCentreX() - bar.getWidth() / 2) <= 1);
    CHECK (bar.getSearchBox().getWidth() >= phazite::ui::Shell::searchMinimumWidth);

    // The window's buttons and the bar's own controls, and between them, the window's caption.
    auto* close = findButton (bar, "Close");
    auto* maximise = findButton (bar, "Maximise");
    auto* minimise = findButton (bar, "Minimise");
    REQUIRE (close != nullptr);
    REQUIRE (maximise != nullptr);
    REQUIRE (minimise != nullptr);
    CHECK (close->getRight() == bar.getWidth());
    CHECK (bar.getControlAt (close->getBounds().getCentre()) == Kind::close);
    CHECK (bar.getControlAt (maximise->getBounds().getCentre()) == Kind::maximise);
    CHECK (bar.getControlAt (minimise->getBounds().getCentre()) == Kind::minimise);
    CHECK (bar.getControlAt (bar.getSearchBox().getBounds().getCentre()) == Kind::client);
    CHECK (bar.getControlAt (menuBar->getBounds().getCentre()) == Kind::client);
    CHECK (bar.getControlAt ({ (menuBar->getRight() + findButton (bar, "Undo")->getX()) / 2, bar.getHeight() / 2 }) == Kind::caption);

    // Screen readers press the window's buttons as buttons.
    dynamic_cast<juce::Button*> (close)->triggerClick();
    phazite::test::dispatchPendingMessages();
    CHECK (actions.closed == 1);

    // At the window's least width the menus fold into one button, and nothing overlaps.
    screen.window.setSize (800, 600);
    CHECK_FALSE (menuBar->isVisible());
    CHECK (findButton (bar, "Menus") != nullptr);
    phazite::test::checkChildrenApart (bar);

    // Without the window's buttons (Linux and macOS keep the system's title bar), the bar has none.
    bar.setWindowActions (std::nullopt);
    CHECK (findButton (bar, "Close") == nullptr);
    bar.setMenus (nullptr);
    screen.window.setLookAndFeel (nullptr);
}

TEST_CASE ("The panels keep out of the band along a restored window's sides, and the bars reach them")
{
    using phazite::ui::Panel;
    Screen screen;
    auto& shell = screen.shell;
    const auto inset = phazite::ui::Shell::edgeInset;

    shell.setEdgeInset (inset); // as the main window sets it on Windows, while it isn't maximized
    CHECK (shell.getPanel (Panel::browser).getX() == inset);
    CHECK (shell.getPanel (Panel::inspector).getRight() == shell.getWidth() - inset);
    CHECK (shell.getTitleBar().getWidth() == shell.getWidth());
    CHECK (shell.getStatusBar().getWidth() == shell.getWidth());

    shell.setEdgeInset (0); // maximized
    CHECK (shell.getPanel (Panel::browser).getX() == 0);
    CHECK (shell.getPanel (Panel::inspector).getRight() == shell.getWidth());
}
