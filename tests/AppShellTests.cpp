// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the session, the
// services, AppShell and the commands, in the app's order. Nothing here may open a dialog, popup or window (the palette,
// the plug-in picker, Detach, New, Open or Save As): Linux CI has no display.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/Keymap.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/shell/ActivityBar.h"
#include "ui/shell/PanelLayout.h"

#include <algorithm>
#include <functional>
#include <typeinfo>

namespace
{
using Screen = phazite::test::AppScreen;

// The window's parts, as checkChildrenApart() checks them, except the panels' buttons (maximize, a window of its own,
// hide), which sit in their panel's top right corner on purpose: each lies inside a panel; and what lies over them all
// and takes no clicks, the panels' cards' corners and edges (ui::PanelCards).
void checkPartsApart (juce::Component& shell)
{
    juce::Array<juce::Component*> parts, cornerButtons;

    for (auto* child : shell.getChildren())
    {
        bool takesClicks = true, childrenTakeClicks = true;
        child->getInterceptsMouseClicks (takesClicks, childrenTakeClicks);

        if (child->isVisible() && ! child->getBounds().isEmpty() && (takesClicks || childrenTakeClicks))
            (child->getTitle() == "Panel buttons" ? cornerButtons : parts).add (child);
    }

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

    // No recent projects: the first start makes an untitled project, with no tracks.
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    REQUIRE (findSong (screen.session.getContext()) != nullptr);
    phazite::test::settle();
    CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());

    for (const auto* id :
         { "view.toggleLeftPanel",   "view.toggleLeftPanel", "view.toggleInspector", "view.toggleInspector", "view.pianoRoll",
           "view.instrument",        "view.effects",         "view.mixer",           "view.playlist",        "view.toggleEditor",
           "view.toggleEditor",      "view.toggleSong",      "view.toggleSong",      "view.maximizeEditor",  "view.maximizeLeftPanel",
           "view.maximizeLeftPanel", "view.maximizePanel",   "view.maximizePanel",   "view.layoutBeat",      "view.layoutMix",
           "view.layoutFull" })
    {
        INFO ("after " << id);
        CHECK (screen.registry.invoke (id));
        phazite::test::dispatchPendingMessages();
        checkPartsApart (screen.shell);
    }

    CHECK (screen.window.paintEverything().isValid());

    // By default the whole song in the upper panel, as tabs anyone can see, and the selection in the lower panel under it.
    // Automation has no tab: it is edited in its lanes in the playlist.
    auto* song = dynamic_cast<juce::TabbedComponent*> (phazite::test::findComponentWithTitle (screen.shell, "Upper panel"));
    REQUIRE (song != nullptr);
    CHECK (song->getTabNames() == juce::StringArray { "Playlist", "Mixer" });
    auto* dock = dynamic_cast<juce::TabbedComponent*> (
        phazite::test::findComponentWithTitle (screen.shell.getPanel (phazite::ui::Panel::editor), "Lower panel"));
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
    CHECK (screen.session.getProject()->getFile() == screen.temp.folder.getChildFile ("Projects/Untitled/Untitled.phazite"));
    CHECK_FALSE (screen.session.getProject()->hasUnsavedChanges());
}

TEST_CASE ("Each panel hides, and fills the window until the others are restored, from its corner or its commands")
{
    using phazite::ui::Panel;
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();

    auto& browser = screen.shell.getPanel (Panel::left);
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
    // Beside the activity bar's card, with the cards' gaps from the window's sides and between them.
    CHECK (song.getWidth() == screen.shell.getWidth() - phazite::ui::Shell::activityBarWidth - 3 * phazite::ui::Shell::cardGap);
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

    // A divider dragged sizes the left panel, which keeps that width while it hides and shows, in the settings too.
    auto* divider = phazite::test::findComponentWithTitle (screen.shell, "Left panel's edge");
    REQUIRE (divider != nullptr);
    CHECK (divider->getX() == browser.getRight());
    CHECK (browserWidth == phazite::ui::Shell::leftPanelWidth);
    phazite::test::Mouse dividerMouse (screen.shell); // the divider moves with the drag
    const auto middle = divider->getBounds().getCentre();
    dividerMouse.press (middle);
    dividerMouse.dragTo (middle + juce::Point<int> (1, 0));
    dividerMouse.dragTo (middle + juce::Point<int> (40, 0));
    dividerMouse.release();
    const auto dragged = browser.getWidth();
    CHECK (dragged == browserWidth + 40);
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    CHECK_FALSE (browser.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    CHECK (browser.getWidth() == dragged);
    CHECK (screen.services.settings.get (phazite::Settings::panelLayout).contains ("left=" + juce::String (dragged)));

    // The panel in use is the one with the keyboard focus (which the shell hears about after the focus moved).
    browser.grabKeyboardFocus();
    phazite::test::dispatchPendingMessages();
    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    REQUIRE (focused != nullptr);
    REQUIRE (browser.isParentOf (focused));
    REQUIRE (screen.registry.invoke ("view.maximizePanel"));
    CHECK (screen.shell.getPanelLayout().isMaximized (Panel::left));
    CHECK_FALSE (song.isShowing());
    REQUIRE (screen.registry.invoke ("view.maximizePanel"));
    CHECK_FALSE (screen.shell.getPanelLayout().getMaximized().has_value());
    CHECK (song.isShowing());

    // Once that panel hides, the song's views are the ones in use.
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    REQUIRE (screen.registry.invoke ("view.maximizePanel"));
    CHECK (screen.shell.getPanelLayout().isMaximized (Panel::song));
    CHECK_FALSE (browser.isShowing());
}

TEST_CASE ("The panels are cards, as VS Code's Modern UI's: apart on the window's frame, the activity bar one with the left panel")
{
    using phazite::ui::Panel, phazite::ui::Shell::cardGap;
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& shell = screen.shell;
    const auto overlays = phazite::test::findAll<phazite::ui::PanelCards> (shell);
    const auto bars = phazite::test::findAll<phazite::ui::ActivityBar> (shell);
    REQUIRE (overlays.size() == 1);
    REQUIRE (bars.size() == 1);
    const auto& cards = overlays[0]->getCards();
    const auto rail = bars[0]->getBounds();

    // The activity bar and the left panel one card, the inspector one, and the upper and lower panels' areas one each.
    CHECK (cards.size() == 4);
    CHECK (cards.contains (rail.getUnion (shell.getPanel (Panel::left).getBounds())));
    CHECK (cards.contains (shell.getPanel (Panel::inspector).getBounds()));

    // Under the bars, each the gap apart from the others and from the window's sides and the status bar.
    for (int i = 0; i < cards.size(); ++i)
    {
        CAPTURE (i);
        CHECK (cards[i].getY() >= shell.getTitleBar().getHeight() + phazite::ui::Shell::transportBarHeight + cardGap);
        CHECK (shell.getLocalBounds().reduced (cardGap).contains (cards[i]));
        CHECK (cards[i].getBottom() + cardGap <= shell.getStatusBar().getY());

        for (int j = i + 1; j < cards.size(); ++j)
            CHECK_FALSE (cards[i].expanded (cardGap - 1).intersects (cards[j]));
    }

    // Over the panels, it takes no clicks: they still get theirs.
    CHECK (shell.getComponentAt (shell.getPanel (Panel::inspector).getBounds().getCentre()) != overlays[0]);

    // The left panel hidden: the activity bar a card of its own; the cards follow the panels as they change.
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    phazite::test::dispatchPendingMessages();
    CHECK (overlays[0]->getCards().contains (rail));
    CHECK (overlays[0]->getCards().size() == 4);
}

#if JUCE_WINDOWS
// A panel's own window is a real one, which Linux CI can't open without a display.
TEST_CASE ("A panel moved to a window of its own fills it at once, and keeps filling it")
{
    using phazite::ui::Panel;
    Screen screen;

    for (const auto which : { Panel::left, Panel::inspector, Panel::editor })
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

    // With the pointer on nothing, the guide's first step: something to play, as a new project starts empty.
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().contains ("drag an instrument or a sound"); }));

    // Something put into the song from the browser (which tells the guide).
    screen.shell.getGuide().markDone (phazite::ui::Guide::Step::addSound);
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().contains ("on your computer keyboard"); }));

    // A note played on the computer keyboard, as its keys reach it. The guide moves on after the note has gone out, in a
    // message of its own, so that saving the step doesn't delay the note.
    screen.keyboard.update ([] (int keyCode) { return keyCode == 'a'; }, {});
    screen.keyboard.update ([] (int) { return false; }, {});
    CHECK_FALSE (screen.shell.getGuide().isDone (phazite::ui::Guide::Step::playNotes));
    phazite::test::dispatchPendingMessages();
    CHECK (screen.shell.getGuide().isDone (phazite::ui::Guide::Step::playNotes));
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().startsWith ("Try this: press Space to play"); }));
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

TEST_CASE ("A view its key shows gets the keyboard, and a screen reader its name, for working with the keyboard alone")
{
    Screen screen;
    screen.session.openAtStartup();
    REQUIRE (screen.registry.invoke ("help.example.trapBeat"));
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();

    // The view the keyboard went to: the focused component, or the view it is in, by its title.
    const auto focusedView = []
    {
        for (auto* c = juce::Component::getCurrentlyFocusedComponent(); c != nullptr; c = c->getParentComponent())
            for (const auto* title : { "Playlist", "Mixer", "Piano roll", "Devices" })
                if (c->getTitle() == juce::String (title))
                    return juce::String (title);

        return juce::String();
    };

    REQUIRE (screen.registry.invoke ("view.mixer"));
    phazite::test::dispatchPendingMessages();
    CHECK (focusedView() == "Mixer");
    REQUIRE (screen.registry.invoke ("view.playlist"));
    phazite::test::dispatchPendingMessages();
    CHECK (focusedView() == "Playlist");
    REQUIRE (screen.registry.invoke ("view.effects"));
    phazite::test::dispatchPendingMessages();
    CHECK (focusedView() == "Devices");

    // A clip selected for the piano roll: F7 gives it the keys (or, where it showed, hides it: then again shows it).
    auto& context = screen.session.getContext();
    context.getSongSelection().selectOnly (phazite::getClips (context, phazite::getAllPlaylistTracks (context).front()).front().id);
    phazite::test::dispatchPendingMessages();
    const auto findPianoRoll = [&screen]() -> juce::Component*
    {
        phazite::test::dispatchPendingMessages();
        auto* found = phazite::test::findAll<phazite::ui::PianoRoll> (screen.shell).getFirst();
        return found != nullptr && found->isShowing() ? found : nullptr;
    };

    REQUIRE (screen.registry.invoke ("view.pianoRoll"));

    if (findPianoRoll() == nullptr)
        REQUIRE (screen.registry.invoke ("view.pianoRoll"));

    CHECK (findPianoRoll() != nullptr);
    CHECK (focusedView() == "Piano roll");
}

TEST_CASE ("Every control in the window has a name and a role for screen readers, as Narrator reads them")
{
    Screen screen;
    screen.session.openAtStartup();
    REQUIRE (screen.registry.invoke ("help.example.trapBeat")); // tracks, clips and devices to show
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();
    juce::StringArray nameless;

    // Each visible control, and each component that takes the keyboard (which Narrator moves to), by the titles of the
    // components it is in. A control's own parts (a slider's text box, a list's scroll bar) are its.
    std::function<void (juce::Component&, const juce::String&)> visit = [&nameless, &visit] (juce::Component& c, const juce::String& path)
    {
        if (! c.isVisible() || dynamic_cast<juce::ScrollBar*> (&c) != nullptr)
            return;

        const auto here = path + " > " + (c.getTitle().isNotEmpty() ? c.getTitle() : juce::String (typeid (c).name()));
        const auto isControl = dynamic_cast<juce::Button*> (&c) != nullptr || dynamic_cast<juce::Slider*> (&c) != nullptr
                               || dynamic_cast<juce::ComboBox*> (&c) != nullptr || dynamic_cast<juce::TextEditor*> (&c) != nullptr;

        if (isControl || c.getWantsKeyboardFocus())
        {
            const auto handler = c.isAccessible() ? c.createAccessibilityHandler() : nullptr;

            if (handler == nullptr || handler->getTitle().trim().isEmpty() || handler->getRole() == juce::AccessibilityRole::ignored)
                nameless.addIfNotAlreadyThere (here);
        }

        if (isControl)
            return;

        for (auto* child : c.getChildren())
            visit (*child, here);
    };

    visit (screen.shell, "Window");

    for (const auto* view : { "view.mixer", "view.pianoRoll", "view.instrument", "view.effects" })
    {
        REQUIRE (screen.registry.invoke (view));
        phazite::test::dispatchPendingMessages();
        visit (screen.shell, juce::String ("Window, ") + view);
    }

    INFO (nameless.joinIntoString ("\n").toStdString());
    CHECK (nameless.isEmpty());
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
