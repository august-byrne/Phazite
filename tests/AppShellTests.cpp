// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the session, the
// services, AppShell and the commands, in the app's order. Nothing here may open a dialog, popup or window (the palette,
// the plug-in picker, Detach, New, Open or Save As): Linux CI has no display.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

namespace
{
using Screen = phazite::test::AppScreen;

// The window's parts, as checkChildrenApart() checks them, except the panels' detach buttons ("Move the browser to its
// own window"), which sit in their panel's top right corner on purpose: each lies inside a panel.
void checkPartsApart (juce::Component& shell)
{
    juce::Array<juce::Component*> parts, detachButtons;

    for (auto* child : shell.getChildren())
        if (child->isVisible() && ! child->getBounds().isEmpty())
            (child->getTitle().endsWith (" to its own window") ? detachButtons : parts).add (child);

    for (int i = 0; i < parts.size(); ++i)
    {
        CAPTURE (parts[i]->getTitle().toStdString());
        CHECK (shell.getLocalBounds().contains (parts[i]->getBounds()));

        for (int j = i + 1; j < parts.size(); ++j)
        {
            CAPTURE (parts[j]->getTitle().toStdString());
            CHECK_FALSE (parts[i]->getBounds().intersects (parts[j]->getBounds()));
        }
    }

    for (auto* button : detachButtons)
    {
        CAPTURE (button->getTitle().toStdString());
        CHECK (std::any_of (parts.begin(), parts.end(),
                            [button] (juce::Component* part) { return part->getBounds().contains (button->getBounds()); }));
    }
}

constexpr const char* aboutTheAppCatalogue =
    "# The commands Phazite registers before the main window's own (app.quit, options.*, input.computerKeyboard, the keymaps "
    "and themes, view.interface*, view.hoverHelp, help.welcome and help.phaziteSounds, which come after these), in order, with "
    "their keys as Windows and Linux write them. Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";
} // namespace

TEST_CASE ("The window's content works without a screen: a project opens, the panels and layouts change, and commands are found")
{
    Screen screen;

    // No recent projects: the first start makes My First Song.
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    REQUIRE (getEdit (screen.session.getContext()) != nullptr);
    phazite::test::settle();
    CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());

    for (const auto* id : { "view.toggleBrowser", "view.toggleBrowser", "view.toggleInspector", "view.toggleInspector", "view.pianoRoll",
                            "view.devices", "view.mixer", "view.playlist", "view.toggleEditor", "view.toggleEditor", "view.layoutBeat",
                            "view.layoutMix", "view.layoutFull" })
    {
        INFO ("after " << id);
        CHECK (screen.registry.invoke (id));
        phazite::test::dispatchPendingMessages();
        checkPartsApart (screen.shell);
    }

    CHECK (screen.window.paintEverything().isValid());

    // The whole song in the centre, as tabs anyone can see, and the selection in the editor dock under it. Automation has
    // no tab: it is edited in its lanes in the playlist.
    auto* song = dynamic_cast<juce::TabbedComponent*> (phazite::test::findComponentWithTitle (screen.shell, "Song"));
    REQUIRE (song != nullptr);
    CHECK (song->getTabNames() == juce::StringArray { "Playlist", "Mixer" });
    auto* dock = dynamic_cast<juce::TabbedComponent*> (&screen.shell.getPanel (phazite::ui::AppShell::DetachablePanel::editor));
    REQUIRE (dock != nullptr);
    CHECK (dock->getTabNames() == juce::StringArray { "Piano roll", "Devices" });

    // F9's command shows the mixer in the centre, F5's the playlist again; the Mix layout shows the mixer over the
    // selected track's devices.
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK (song->getCurrentTabName() == juce::String ("Mixer"));
    REQUIRE (screen.registry.invoke ("view.playlist"));
    CHECK (song->getCurrentTabName() == juce::String ("Playlist"));
    REQUIRE (screen.registry.invoke ("view.layoutMix"));
    CHECK (song->getCurrentTabName() == juce::String ("Mixer"));
    CHECK (dock->getCurrentTabName() == juce::String ("Devices"));
    REQUIRE (screen.registry.invoke ("view.layoutFull"));
    CHECK (song->getCurrentTabName() == juce::String ("Playlist"));

    const auto found = screen.registry.search ("mixer");
    REQUIRE_FALSE (found.isEmpty());
    CHECK (found.getFirst()->id == "view.mixer");

    // Its first save goes where the save dialog says (here, what it suggests).
    const auto* save = screen.registry.find ("project.save");
    REQUIRE (save != nullptr);
    CHECK (save->isEnabled());
    REQUIRE (screen.registry.invoke ("project.save"));
    CHECK_FALSE (screen.session.isUntitled());
    CHECK (screen.session.getProject()->getFile() == screen.temp.folder.getChildFile ("Projects/My First Song/My First Song.phazite"));
    CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());
}

#if JUCE_WINDOWS
// A panel's own window is a real one, which Linux CI can't open without a display.
TEST_CASE ("A panel moved to a window of its own fills it at once, and keeps filling it")
{
    using Panel = phazite::ui::AppShell::DetachablePanel;
    Screen screen;

    for (const auto which : { Panel::browser, Panel::inspector, Panel::editor })
    {
        auto& panel = screen.shell.getPanel (which);
        screen.shell.setDetached (which, true);
        REQUIRE (screen.shell.isDetached (which));

        auto* window = panel.getParentComponent();
        REQUIRE (window != nullptr);
        CHECK (panel.isVisible());
        CHECK (panel.getWidth() > 0);
        CHECK (panel.getHeight() > 0);
        const auto bounds = panel.getBounds();

        // The main window laying itself out again leaves the panel as its window has it.
        screen.shell.setSize (screen.shell.getWidth() - 10, screen.shell.getHeight());
        CHECK (panel.getBounds() == bounds);

        screen.shell.setDetached (which, false);
        CHECK_FALSE (screen.shell.isDetached (which));
    }
}
#endif

TEST_CASE ("The hint bar shows the guide's next step, which a note on the computer keyboard moves on")
{
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& hints = screen.shell.getHintBar();

    // With the pointer on nothing, the guide's first step.
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().startsWith ("Try this: press Space to play"); }));

    REQUIRE (screen.registry.invoke ("transport.playStop"));
    REQUIRE (screen.registry.invoke ("transport.playStop"));
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().contains ("on your computer keyboard"); }));

    // A note played on the computer keyboard, as its keys reach it. The guide moves on after the note has gone out, in a
    // message of its own, so that saving the step doesn't delay the note.
    screen.keyboard.update ([] (int keyCode) { return keyCode == 'a'; }, {});
    screen.keyboard.update ([] (int) { return false; }, {});
    CHECK_FALSE (screen.shell.getGuide().isDone (phazite::ui::Guide::Step::playNotes));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.shell.getGuide().isDone (phazite::ui::Guide::Step::playNotes));
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().contains ("drag a sound from the browser"); }));
}

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden commands: the app registers its commands in the same order, with the same keys")
    {
        Screen screen;
        juce::StringArray lines;

        for (const auto* command : screen.registry.getAll())
        {
            auto line = command->id;

            if (const auto keys = screen.commands.getShortcutDescriptions (command->id); ! keys.isEmpty())
                line << " | " << keys.joinIntoString (", ");

            lines.add (line);
        }

        const auto text = juce::String (aboutTheAppCatalogue) + "\n" + lines.joinIntoString ("\n") + "\n";
        phazite::test::checkGoldenText ("commands/app-catalogue.txt", text, [] (const juce::String& expected, const juce::String& actual)
                                        { return phazite::test::compareLines (expected, actual, 40); });
    }
}
