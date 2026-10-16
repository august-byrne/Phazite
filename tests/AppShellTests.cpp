// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the session, the
// services, AppShell and the commands, in the app's order. Nothing here may open a dialog, popup or window (the palette,
// the plug-in picker, Detach, New, Open or Save As): Linux CI has no display.
#include "GoldenFiles.h"
#include "UiTestHelpers.h"
#include "app/AppCommands.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/shell/AppShell.h"

#include <algorithm>

namespace
{
// Members in the order the app makes them, so that they go in the order the app destroys them.
struct Screen
{
    Screen()
    {
        // As Main.cpp and MainWindow do.
        phazite::addSongCommands (registry, session.getContext());
        session.addCommands (registry);
        shell.addCommands (registry);
        session.onProjectClosing = [this] { shell.setEdit (nullptr); };
        session.onProjectChanged = [this] { shell.setEdit (session.getContext().getEdit()); };
        phazite::test::dispatchPendingMessages();
    }

    ~Screen()
    {
        // As MainWindow's destructor does.
        session.onProjectClosing = nullptr;
        session.onProjectChanged = nullptr;
        shell.setEdit (nullptr);
        phazite::test::runTimersUntil ([this] { return ! services.library.isIndexing(); }, 10000);
    }

    te::Engine& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::AppServices services { engine, { .settingsInMemory = true, .packsFolder = temp.folder.getChildFile ("Packs") } };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::KeyDispatcher shortcutKeys { commands };
    phazite::ProjectSession session { engine, phazite::getUIBehaviour (engine), services.recentProjects,
                                      temp.folder.getChildFile ("Projects") };
    phazite::ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {} };
    phazite::ui::AppShell shell {
        engine,           commands,          shortcutKeys, keyboard, session.getContext(), services.settings, services.soundPack,
        services.library, services.previewer
    };
    phazite::test::TestWindow window { shell, 1280, 800 };
};

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
    REQUIRE (screen.session.getContext().getEdit() != nullptr);
    phazite::test::settle();
    CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());

    for (const auto* id :
         { "view.toggleBrowser", "view.toggleBrowser", "view.toggleInspector", "view.toggleInspector", "view.pianoRoll", "view.mixer",
           "view.toggleEditor", "view.toggleEditor", "view.layoutBeat", "view.layoutMix", "view.layoutFull" })
    {
        INFO ("after " << id);
        CHECK (screen.registry.invoke (id));
        phazite::test::dispatchPendingMessages();
        checkPartsApart (screen.shell);
    }

    CHECK (screen.window.paintEverything().isValid());

    const auto found = screen.registry.search ("mixer");
    REQUIRE_FALSE (found.isEmpty());
    CHECK (found.getFirst()->id == "view.mixer");

    const auto* save = screen.registry.find ("project.save");
    REQUIRE (save != nullptr);
    CHECK (save->isEnabled());
    REQUIRE (screen.registry.invoke ("project.save"));
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

    // A note played on the computer keyboard, as its keys reach it.
    screen.keyboard.update ([] (int keyCode) { return keyCode == 'a'; }, {});
    screen.keyboard.update ([] (int) { return false; }, {});
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
