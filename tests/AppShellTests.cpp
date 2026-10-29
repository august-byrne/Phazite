// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the session, the
// services, AppShell and the commands, in the app's order. Nothing here may open a dialog, popup or window (the palette,
// the plug-in picker, Detach, New, Open or Save As): Linux CI has no display.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/Keymap.h"

#include <algorithm>
#include <functional>
#include <typeinfo>

namespace
{
using Screen = phazite::test::AppScreen;

// The window's parts, as checkChildrenApart() checks them, except the panels' buttons (maximize, a window of its own,
// hide), which sit in their panel's top right corner on purpose: each lies inside a panel.
void checkPartsApart (juce::Component& shell)
{
    juce::Array<juce::Component*> parts, cornerButtons;

    for (auto* child : shell.getChildren())
        if (child->isVisible() && ! child->getBounds().isEmpty())
            (child->getTitle() == "Panel buttons" ? cornerButtons : parts).add (child);

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

    for (auto* button : cornerButtons)
    {
        CAPTURE (button->getTitle().toStdString());
        CHECK (std::any_of (parts.begin(), parts.end(),
                            [button] (juce::Component* part) { return part->getBounds().contains (button->getBounds()); }));
    }
}

constexpr const char* aboutTheAppCatalogue =
    "# The commands Phazite registers before the main window's own (app.quit, options.*, input.computerKeyboard, the keymaps "
    "and themes, view.interface*, view.fullScreen, view.hoverHelp, help.welcome and help.phaziteSounds, which come after these), "
    "in order, with their keys as Windows and Linux write them. Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";
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

    for (const auto* id :
         { "view.toggleBrowser",   "view.toggleBrowser", "view.toggleInspector", "view.toggleInspector", "view.pianoRoll",
           "view.instrument",      "view.effects",       "view.mixer",           "view.playlist",        "view.toggleEditor",
           "view.toggleEditor",    "view.toggleSong",    "view.toggleSong",      "view.maximizeEditor",  "view.maximizeBrowser",
           "view.maximizeBrowser", "view.maximizePanel", "view.maximizePanel",   "view.layoutBeat",      "view.layoutMix",
           "view.layoutFull" })
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
    auto* dock = dynamic_cast<juce::TabbedComponent*> (&screen.shell.getPanel (phazite::ui::Panel::editor));
    REQUIRE (dock != nullptr);
    CHECK (dock->getTabNames() == juce::StringArray { "Piano roll", "Instrument", "Effects" });

    // F9's command shows the mixer in the centre, F5's the playlist again; the Mix layout shows the mixer over the
    // selected track's effects.
    REQUIRE (screen.registry.invoke ("view.mixer"));
    CHECK (song->getCurrentTabName() == juce::String ("Mixer"));
    REQUIRE (screen.registry.invoke ("view.playlist"));
    CHECK (song->getCurrentTabName() == juce::String ("Playlist"));
    REQUIRE (screen.registry.invoke ("view.layoutMix"));
    CHECK (song->getCurrentTabName() == juce::String ("Mixer"));
    CHECK (dock->getCurrentTabName() == juce::String ("Effects"));
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

TEST_CASE ("Each panel hides, and fills the window until the others are restored, from its corner or its commands")
{
    using phazite::ui::Panel;
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();

    auto& browser = screen.shell.getPanel (Panel::browser);
    auto& song = screen.shell.getPanel (Panel::song);
    auto& editor = screen.shell.getPanel (Panel::editor);
    auto& inspector = screen.shell.getPanel (Panel::inspector);

    // The buttons in a panel's top right corner.
    const auto cornerOf = [&screen] (juce::Component& panel) -> juce::Component&
    {
        for (auto* buttons : phazite::test::findAll<phazite::ui::PanelButtons> (screen.shell))
            if (buttons->isVisible() && panel.getScreenBounds().contains (buttons->getScreenBounds()))
                return *buttons;

        FAIL ("no buttons in the panel's corner");
        return panel;
    };

    const auto click = [] (juce::Component& corner, const juce::String& title)
    {
        auto* button = phazite::test::findComponentWithTitle (corner, title);
        REQUIRE (button != nullptr);
        phazite::test::Mouse mouse (*button);
        mouse.click (button->getLocalBounds().getCentre());
        phazite::test::dispatchPendingMessages();
    };

    for (auto* panel : { &browser, &song, &editor, &inspector })
        CHECK (panel->isShowing());

    const auto browserWidth = browser.getWidth(), editorHeight = editor.getHeight();

    // The playlist's Maximize: it fills the window between the bars, the other panels out of sight, and its first button
    // restores them, as they were.
    click (cornerOf (song), "Maximize");
    CHECK (song.getWidth() == screen.shell.getWidth());
    CHECK_FALSE (browser.isShowing());
    CHECK_FALSE (editor.isShowing());
    CHECK_FALSE (inspector.isShowing());
    CHECK (phazite::getCurrentName (*screen.registry.find ("view.maximizeSong")) == juce::String ("Restore the Panels"));
    checkPartsApart (screen.shell);
    click (cornerOf (song), "Restore the panels");
    CHECK (browser.isShowing());
    CHECK (editor.isShowing());
    CHECK (inspector.isShowing());
    CHECK (browser.getWidth() == browserWidth);
    CHECK (editor.getHeight() == editorHeight);

    // The inspector's Hide, and its command, which shows it again and names what it does.
    click (cornerOf (inspector), "Hide");
    CHECK_FALSE (inspector.isShowing());
    CHECK (phazite::getCurrentName (*screen.registry.find ("view.toggleInspector")) == juce::String ("Show the Inspector"));
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    CHECK (inspector.isShowing());
    CHECK (phazite::getCurrentName (*screen.registry.find ("view.toggleInspector")) == juce::String ("Hide the Inspector"));

    // Hiding the playlist gives the editor the centre; Show the Playlist brings it back.
    click (cornerOf (song), "Hide");
    CHECK_FALSE (song.isShowing());
    CHECK (editor.isShowing());
    CHECK (editor.getHeight() > editorHeight);
    REQUIRE (screen.registry.invoke ("view.playlist"));
    CHECK (song.isShowing());

    // A divider dragged sizes the browser, which keeps that width while it hides and shows, in the settings too.
    auto* divider = phazite::test::findComponentWithTitle (screen.shell, "Browser's edge");
    REQUIRE (divider != nullptr);
    CHECK (divider->getX() == browser.getRight());
    CHECK (browserWidth == phazite::ui::Shell::browserWidth);
    phazite::test::Mouse dividerMouse (screen.shell); // the divider moves with the drag
    const auto middle = divider->getBounds().getCentre();
    dividerMouse.press (middle);
    dividerMouse.dragTo (middle + juce::Point<int> (1, 0));
    dividerMouse.dragTo (middle + juce::Point<int> (40, 0));
    dividerMouse.release();
    const auto dragged = browser.getWidth();
    CHECK (dragged == browserWidth + 40);
    REQUIRE (screen.registry.invoke ("view.toggleBrowser"));
    CHECK_FALSE (browser.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleBrowser"));
    CHECK (browser.getWidth() == dragged);
    CHECK (screen.services.settings.get (phazite::Settings::panelLayout).contains ("browser=" + juce::String (dragged)));

    // The panel in use is the one with the keyboard focus (which the shell hears about after the focus moved).
    browser.grabKeyboardFocus();
    phazite::test::dispatchPendingMessages();
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    REQUIRE (focused != nullptr);
    REQUIRE (browser.isParentOf (focused));
    REQUIRE (screen.registry.invoke ("view.maximizePanel"));
    CHECK (screen.shell.getPanelLayout().isMaximized (Panel::browser));
    CHECK_FALSE (song.isShowing());
    REQUIRE (screen.registry.invoke ("view.maximizePanel"));
    CHECK_FALSE (screen.shell.getPanelLayout().getMaximized().has_value());
    CHECK (song.isShowing());

    // Once that panel hides, the song's views are the ones in use.
    REQUIRE (screen.registry.invoke ("view.toggleBrowser"));
    REQUIRE (screen.registry.invoke ("view.maximizePanel"));
    CHECK (screen.shell.getPanelLayout().isMaximized (Panel::song));
    CHECK_FALSE (browser.isShowing());
}

#if JUCE_WINDOWS
// A panel's own window is a real one, which Linux CI can't open without a display.
TEST_CASE ("A panel moved to a window of its own fills it at once, and keeps filling it")
{
    using phazite::ui::Panel;
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

TEST_CASE ("The window shows Opening... until a project is open, and says when no audio output plays")
{
    Screen screen;
    CHECK (screen.shell.showsOpening());

    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.shell.showsOpening());

    // The tests open no audio device: the hint bar says so, in place of its hints.
    CHECK (screen.shell.getHintBar().getAlert().startsWith ("No audio output"));
}

TEST_CASE ("Every command has help text: a name, a category, and a sentence that says what it does")
{
    Screen screen;

    for (const auto* command : screen.registry.getAll())
    {
        CAPTURE (command->id.toStdString());
        CHECK (command->name.isNotEmpty());
        CHECK (command->category.isNotEmpty());
        CHECK (command->description.length() > command->name.length());
        CHECK (juce::CharacterFunctions::isUpperCase (command->description[0]));
        CHECK (command->description.endsWithChar ('.'));
    }
}

TEST_CASE ("Every command the built-in keymaps give a key to is one the app has")
{
    Screen screen;

    // The main window's own (src/app/MainWindow.cpp), which this screen doesn't make.
    const juce::StringArray mainWindows { "app.quit",
                                          "options.settings",
                                          "options.audioSettings",
                                          "options.midiSettings",
                                          "input.computerKeyboard",
                                          "view.interfaceLarger",
                                          "view.interfaceSmaller",
                                          "view.interfaceNormal",
                                          "view.fullScreen",
                                          "view.hoverHelp" };

    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        CAPTURE (keymap.name.toStdString());

        for (const auto& entry : keymap.keys)
        {
            CAPTURE (entry.first.toStdString());
            CHECK ((screen.registry.find (entry.first) != nullptr || mainWindows.contains (entry.first)));
        }
    }
}

TEST_CASE ("Every control in the window explains itself in the hint bar, with a tooltip or a description")
{
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    juce::StringArray silent;

    // Each visible control, by the titles of the components it is in. A control's own parts (a slider's text box, a list's
    // scroll bar) are its.
    std::function<void (juce::Component&, const juce::String&)> visit = [&silent, &visit] (juce::Component& c, const juce::String& path)
    {
        if (! c.isVisible() || dynamic_cast<juce::ScrollBar*> (&c) != nullptr)
            return;

        const auto here = path + " > " + (c.getTitle().isNotEmpty() ? c.getTitle() : juce::String (typeid (c).name()));

        if (dynamic_cast<juce::Button*> (&c) != nullptr || dynamic_cast<juce::Slider*> (&c) != nullptr
            || dynamic_cast<juce::ComboBox*> (&c) != nullptr || dynamic_cast<juce::TextEditor*> (&c) != nullptr)
        {
            auto* client = dynamic_cast<juce::TooltipClient*> (&c);

            if ((client == nullptr || client->getTooltip().isEmpty()) && c.getDescription().isEmpty())
                silent.addIfNotAlreadyThere (here);

            return;
        }

        for (auto* child : c.getChildren())
            visit (*child, here);
    };

    visit (screen.shell, "Window");
    REQUIRE (screen.registry.invoke ("view.mixer"));
    phazite::test::dispatchPendingMessages();
    visit (screen.shell, "Window, mixer");

    for (const auto* view : { "view.pianoRoll", "view.instrument", "view.effects" })
    {
        REQUIRE (screen.registry.invoke (view));
        phazite::test::dispatchPendingMessages();
        visit (screen.shell, juce::String ("Window, ") + view);
    }

    INFO (silent.joinIntoString ("\n").toStdString());
    CHECK (silent.isEmpty());
}
