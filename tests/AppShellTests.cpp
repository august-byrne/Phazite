// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the session, the
// services, AppShell and the commands, in the app's order. Nothing here may open a dialog, popup or window (the palette,
// the plug-in picker, Detach, New, Open or Save As): Linux CI has no display.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "PaintCheck.h"
#include "RepresentativeSong.h"
#include "SandboxedPlugins.h"
#include "TestClips.h"
#include "TestPlugin.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/browser/Browser.h"
#include "ui/input/Keymap.h"
#include "ui/mixer/DevicesView.h"
#include "ui/mixer/Inspector.h"
#include "ui/mixer/MixerView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"
#include "ui/project/ProjectView.h"
#include "ui/shell/ActivityBar.h"
#include "ui/shell/PanelLayout.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/versions/VersionsView.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <typeinfo>
#include <utility>
#include <vector>

namespace
{
using Screen = phazite::test::AppScreen;

// What a part of the window takes of it: a divider takes drags a little way into the cards beside it, over their edges, on
// purpose (PanelDivider::place()), so it takes its gap alone.
juce::Rectangle<int> getTakenArea (const juce::Component& part)
{
    const auto bounds = part.getBounds();

    if (dynamic_cast<const phazite::ui::PanelDivider*> (&part) == nullptr)
        return bounds;

    return bounds.getWidth() < bounds.getHeight() ? bounds.reduced (phazite::ui::Shell::dividerGrab, 0)
                                                  : bounds.reduced (0, phazite::ui::Shell::dividerGrab);
}

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
            CHECK_FALSE (getTakenArea (*parts[i]).intersects (getTakenArea (*parts[j])));
        }
    }

    for (auto* button : cornerButtons)
    {
        CAPTURE (button->getTitle().toStdString());
        CHECK (std::any_of (parts.begin(), parts.end(),
                            [button] (juce::Component* part) { return part->getBounds().contains (button->getBounds()); }));
    }
}

// What a walk of the window found that a user can't use alone: controls a screen reader has no name or role for, and
// controls that don't explain themselves in the hint bar (with a tooltip or a description); each by the titles of the
// components it is in.
struct Unexplained
{
    juce::StringArray nameless, silent;
};

// Walks what shows of a component: each visible control (a button, slider, combo box or text field, whose own parts, a
// slider's text box say, are its) and each component that takes the keyboard (which Narrator moves to). A list's scroll
// bar is the list's.
void walkControls (juce::Component& c, const juce::String& path, Unexplained& found)
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
            found.nameless.addIfNotAlreadyThere (here);
    }

    if (! isControl)
    {
        for (auto* child : c.getChildren())
            walkControls (*child, here, found);

        return;
    }

    auto* client = dynamic_cast<juce::TooltipClient*> (&c);

    if ((client == nullptr || client->getTooltip().isEmpty()) && c.getDescription().isEmpty())
        found.silent.addIfNotAlreadyThere (here);
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

    // At the smallest size the window can be (MainWindow's resize limits), at a large screen's, and at the size it opens at.
    for (const auto& size : { juce::Point<int> (800, 500), juce::Point<int> (1920, 1080), juce::Point<int> (1280, 800) })
    {
        CAPTURE (size.toString().toStdString());
        screen.window.setSize (size.x, size.y);

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

TEST_CASE ("The lower panel's corner maximizes and hides it, as VS Code's panel's; the inspector's X hides it; the other "
           "panels have no buttons there")
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

    // The buttons in a panel's top right corner: the lower panel's and the inspector's only.
    const auto cornerOf = [&screen] (juce::Component& panel) -> juce::Component*
    {
        for (auto* buttons : phazite::test::findAll<phazite::ui::PanelButtons> (screen.shell))
            if (buttons->isShowing() && panel.getScreenBounds().contains (buttons->getScreenBounds()))
                return buttons;

        return nullptr;
    };

    for (auto* panel : { &browser, &song })
        CHECK (cornerOf (*panel) == nullptr);

    REQUIRE (cornerOf (editor) != nullptr);
    REQUIRE (cornerOf (inspector) != nullptr);

    const auto clickIn = [&cornerOf] (juce::Component& panel, const juce::String& title)
    {
        auto* corner = cornerOf (panel);
        REQUIRE (corner != nullptr);
        auto* button = phazite::test::findComponentWithTitle (*corner, title);
        REQUIRE (button != nullptr);
        phazite::test::Mouse mouse (*button);
        mouse.click (button->getLocalBounds().getCentre());
        phazite::test::dispatchPendingMessages();
    };

    const auto click = [&clickIn, &editor] (const juce::String& title) { clickIn (editor, title); };

    for (auto* panel : { &browser, &song, &editor, &inspector })
        CHECK (panel->isShowing());

    const auto browserWidth = browser.getWidth(), editorHeight = editor.getHeight(), songHeight = song.getHeight();

    // Its Maximize: it fills the window between the bars, the other panels out of sight, and its first button restores
    // them, as they were.
    click ("Maximize");
    // Beside the activity bar's card, with the cards' gaps from the window's sides and between them.
    CHECK (editor.getWidth() == screen.shell.getWidth() - phazite::ui::Shell::activityBarWidth - 3 * phazite::ui::Shell::cardGap);
    CHECK_FALSE (browser.isShowing());
    CHECK_FALSE (song.isShowing());
    CHECK_FALSE (inspector.isShowing());
    CHECK (phazite::getCurrentName (*screen.registry.find ("view.maximizeEditor")) == juce::String ("Restore the Panels"));
    checkPartsApart (screen.shell);
    click ("Restore the panels");
    CHECK (browser.isShowing());
    CHECK (song.isShowing());
    CHECK (inspector.isShowing());
    CHECK (browser.getWidth() == browserWidth);
    CHECK (editor.getHeight() == editorHeight);

    // Its X hides it, giving the upper panel the centre; its command shows it again and names what it does.
    click ("Hide");
    CHECK_FALSE (editor.isShowing());
    CHECK (song.getHeight() > songHeight);
    CHECK (phazite::getCurrentName (*screen.registry.find ("view.toggleEditor")) == juce::String ("Show the Lower Panel"));
    REQUIRE (screen.registry.invoke ("view.toggleEditor"));
    CHECK (editor.isShowing());
    CHECK (editor.getHeight() == editorHeight);

    // The inspector's X, alone at the right end of its title (which stops short of it), hides it, as its command and its
    // switch in the title bar do; they show it again (2026-11-07).
    auto* inspectorCorner = cornerOf (inspector);
    REQUIRE (inspectorCorner != nullptr);
    CHECK (phazite::test::findComponentWithTitle (*inspectorCorner, "Maximize") == nullptr);
    CHECK (inspectorCorner->getScreenBounds().getRight() == inspector.getScreenBounds().getRight() - phazite::ui::Space::xs);

    for (auto* label : phazite::test::findAll<juce::Label> (inspector))
        if (label->isShowing())
            CHECK_FALSE (label->getScreenBounds().intersects (inspectorCorner->getScreenBounds()));

    clickIn (inspector, "Hide");
    CHECK_FALSE (inspector.isShowing());
    CHECK (phazite::getCurrentName (*screen.registry.find ("view.toggleInspector")) == juce::String ("Show the Inspector"));
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    CHECK (inspector.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    CHECK_FALSE (inspector.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    CHECK (inspector.isShowing());

    // In a window of its own, it has no X: closing the window puts it back.
    screen.shell.setDetached (Panel::inspector, true);
    phazite::test::dispatchPendingMessages();
    CHECK (cornerOf (inspector) == nullptr);
    screen.shell.setDetached (Panel::inspector, false);
    phazite::test::dispatchPendingMessages();
    CHECK (cornerOf (inspector) != nullptr);

    // A divider dragged sizes the left panel, which keeps that width while it hides and shows, in the settings too.
    auto* divider = phazite::test::findComponentWithTitle (screen.shell, "Left panel's edge");
    REQUIRE (divider != nullptr);
    CHECK (divider->getX() + phazite::ui::Shell::dividerGrab == browser.getRight()); // it takes drags a little way into the card
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

TEST_CASE ("A divider dragged far enough closes its panel, as VS Code's sashes do, and a double-click puts its size back")
{
    using phazite::ui::Panel, phazite::ui::Shell::minimumLeftPanelWidth;
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();

    auto& browser = screen.shell.getPanel (Panel::left);
    auto& song = screen.shell.getPanel (Panel::song);
    auto& editor = screen.shell.getPanel (Panel::editor);
    auto& inspector = screen.shell.getPanel (Panel::inspector);
    phazite::test::Mouse mouse (screen.shell);

    // A divider by its title, and its middle in the shell (the one over the lower panel is in the centre).
    const auto findDivider = [&screen] (const char* title)
    {
        auto* divider = phazite::test::findComponentWithTitle (screen.shell, title);
        REQUIRE (divider != nullptr);
        return divider;
    };

    const auto middleOf = [&screen] (juce::Component& divider)
    { return screen.shell.getLocalArea (divider.getParentComponent(), divider.getBounds()).getCentre(); };

    const auto drag = [&mouse] (juce::Point<int> from, std::initializer_list<juce::Point<int>> by, bool release = true)
    {
        mouse.press (from);

        for (const auto distance : by)
            mouse.dragTo (from + distance);

        if (release)
        {
            mouse.release();
            mouse.pause();
        }
    };

    // The left panel's edge: dragged until the panel would be under half its least, the panel closes; the edge stays while
    // the drag goes on, and dragged back, it opens again. Let go there, it stays closed.
    auto* left = findDivider ("Left panel's edge");
    const auto width = browser.getWidth();
    const auto start = middleOf (*left);
    drag (start, { { -1, 0 }, { minimumLeftPanelWidth / 2 - 4 - width, 0 } }, false);
    CHECK_FALSE (browser.isShowing());
    CHECK (left->isShowing());
    mouse.dragTo (start + juce::Point<int> (-20, 0));
    CHECK (browser.isShowing());
    CHECK (browser.getWidth() == width - 20);
    mouse.dragTo (start + juce::Point<int> (-width, 0));
    mouse.release();
    mouse.pause();
    CHECK_FALSE (browser.isShowing());
    CHECK_FALSE (left->isVisible());

    // Shown again by its switch, at the width it had before that drag.
    REQUIRE (screen.registry.invoke ("view.toggleLeftPanel"));
    CHECK (browser.getWidth() == width);

    // Double-clicked, the edge puts the panel back at the width it starts at.
    drag (middleOf (*left), { { 1, 0 }, { 40, 0 } });
    CHECK (browser.getWidth() == width + 40);
    mouse.doubleClick (middleOf (*left));
    CHECK (browser.getWidth() == phazite::ui::Shell::leftPanelWidth);

    // The inspector's edge, dragged right past it, closes it.
    auto* right = findDivider ("Inspector's edge");
    const auto inspectorWidth = inspector.getWidth();
    drag (middleOf (*right), { { 1, 0 }, { inspectorWidth, 0 } });
    CHECK_FALSE (inspector.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleInspector"));
    CHECK (inspector.getWidth() == inspectorWidth);

    // The edge between the upper and lower panels: dragged down, the lower panel closes; dragged on up, past the upper one's
    // least, the lower one opens again and the upper one closes. Shown again, each has the height it had.
    auto* between = findDivider ("Editor's edge");
    const auto editorHeight = editor.getHeight(), songHeight = song.getHeight();
    const auto middle = middleOf (*between);
    drag (middle, { { 0, 1 }, { 0, editorHeight } }, false);
    CHECK_FALSE (editor.isShowing());
    CHECK (song.isShowing());
    CHECK (between->isShowing());
    mouse.dragTo (middle - juce::Point<int> (0, songHeight));
    CHECK (editor.isShowing());
    CHECK_FALSE (song.isShowing());
    mouse.release();
    mouse.pause();
    CHECK_FALSE (song.isShowing());
    REQUIRE (screen.registry.invoke ("view.toggleSong"));
    CHECK (song.getHeight() == songHeight);
    CHECK (editor.getHeight() == editorHeight);

    // Double-clicked, it gives the lower panel its first share of the height again.
    drag (middleOf (*between), { { 0, 1 }, { 0, 30 } });
    CHECK (editor.getHeight() == editorHeight - 30);
    mouse.doubleClick (middleOf (*between));
    CHECK (editor.getHeight() == editorHeight);
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

TEST_CASE ("No built-in keymap gives Shift and a letter another command than the letter alone")
{
    // While the computer keyboard plays notes, Shift and a letter run that letter's single-key shortcut (CLAUDE.md, "Code
    // conventions"): a keymap's own Shift+B would take B's command from anyone playing. Shift and a letter that has no key
    // of its own may have a command (FL Studio's Shift+Q).
    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        CAPTURE (keymap.name.toStdString());
        std::map<int, juce::StringArray> letters, shiftedLetters; // by the letter, in capitals, the commands on it

        for (const auto& [id, descriptions] : keymap.keys)
        {
            for (const auto& description : descriptions)
            {
                // The key is a letter by its name, the description's last word ("shift + B"): a function key's code can be a
                // letter's (Windows' F6 is 'u').
                const auto key = juce::KeyPress::createFromDescription (description);
                const auto name = description.fromLastOccurrenceOf ("+", false, false).trim();
                const auto code = juce::CharacterFunctions::toUpperCase (name[0]);

                if (! key.isValid() || name.length() != 1 || ! juce::CharacterFunctions::isLetter (code))
                    continue;

                if (key.getModifiers() == juce::ModifierKeys())
                    letters[(int) code].addIfNotAlreadyThere (id);
                else if (key.getModifiers() == juce::ModifierKeys (juce::ModifierKeys::shiftModifier))
                    shiftedLetters[(int) code].addIfNotAlreadyThere (id);
            }
        }

        for (const auto& [code, commands] : shiftedLetters)
        {
            CAPTURE (juce::String::charToString ((juce::juce_wchar) code).toStdString());
            CAPTURE (commands.joinIntoString (", ").toStdString());

            if (const auto alone = letters.find (code); alone != letters.end())
            {
                auto shifted = commands, plain = alone->second;
                shifted.sort (false);
                plain.sort (false);
                CHECK (shifted == plain);
            }
        }
    }
}

TEST_CASE ("A view its key shows gets the keyboard, and a screen reader its name, for working with the keyboard alone")
{
    Screen screen;
    screen.session.openAtStartup();
    REQUIRE (screen.registry.invoke ("help.example.exodus"));
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

    // A clip selected for the piano roll, the first track's that has clips (the first is a folder): F7 gives it the keys (or,
    // where it showed, hides it: then again shows it).
    auto& context = screen.session.getContext();
    std::vector<phazite::ClipInfo> clips;

    for (const auto track : phazite::getAllPlaylistTracks (context))
        if (clips = phazite::getClips (context, track); ! clips.empty())
            break;

    REQUIRE_FALSE (clips.empty());
    context.getSongSelection().selectOnly (clips.front().id);
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

TEST_CASE ("Every control in the window has a name and a role for screen readers and explains itself in the hint bar, in each "
           "theme and at each size, over a song with something of everything")
{
    // The song has the tests' stand-in plug-ins, and every kind of track, clip, lane and strip (RepresentativeSong.h), and a
    // track for each of Phazite's own devices. Each look walks the window and each view in turn, every one checked showing
    // before it is walked: the dark theme at the size the window opens at, the light one at the smallest it can be
    // (MainWindow's resize limits), where tool rows fold into their More menus, and high contrast on a large screen.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    const phazite::test::TestPlugin testPlugin;
    const phazite::test::SandboxedPlugins sandboxedPlugins;
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& context = screen.session.getContext();
    phazite::test::addRepresentativeSong (*screen.session.getProject(), context, screen.temp.folder.getChildFile ("Sounds"));
    const auto devices = screen.addEveryOwnDevice();
    phazite::test::settle();

    // A MIDI clip and an audio clip, for the piano roll and the audio clip editor.
    std::optional<phazite::ClipID> notes, audio;

    for (const auto track : phazite::getAllPlaylistTracks (context))
        for (const auto& clip : phazite::getClips (context, track))
            (clip.kind == phazite::ClipType::audio ? audio : notes) = clip.id;

    REQUIRE (notes.has_value());
    REQUIRE (audio.has_value());

    struct Look
    {
        phazite::ui::ThemeName theme;
        int width, height;
    };

    for (const auto& look : { Look { phazite::ui::ThemeName::dark, 1280, 800 }, Look { phazite::ui::ThemeName::light, 800, 500 },
                              Look { phazite::ui::ThemeName::highContrast, 1920, 1080 } })
    {
        const auto where = "Window at " + juce::String (look.width) + " x " + juce::String (look.height);
        CAPTURE (where.toStdString());
        lookAndFeel.setTheme (look.theme);
        phazite::ui::refreshTheme (screen.window);
        screen.window.setSize (look.width, look.height);
        phazite::test::dispatchPendingMessages();
        Unexplained found;
        const auto walk = [&found, &where] (juce::Component& view, const juce::String& what)
        { walkControls (view, where + ", " + what, found); };

        walk (screen.shell, "as it opens");
        walk (screen.showView<phazite::ui::Playlist> ("view.playlist"), "playlist");
        walk (screen.showView<phazite::ui::MixerView> ("view.mixer"), "mixer");
        context.getSongSelection().selectOnly (*notes);
        walk (screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll"), "piano roll");
        context.getSongSelection().selectOnly (*audio);
        walk (screen.showView<phazite::ui::AudioClipEditor> ("view.pianoRoll"), "audio clip editor");
        walk (screen.showView<phazite::ui::Inspector> ("view.toggleInspector"), "inspector, an audio clip");
        walk (screen.showView<phazite::ui::Browser> ("view.browser"), "browser");
        walk (screen.showView<phazite::ui::ProjectView> ("view.project"), "project");
        walk (screen.showView<phazite::ui::VersionsView> ("view.versions"), "source control");

        for (const auto device : devices)
            walk (screen.showInDevicesView (device), "Devices view, " + phazite::getDeviceName (*phazite::findSong (context), device));

        // The controls known not to explain themselves yet, by the end of where they are, each found in each look (an entry
        // that no control matches any more fails, so that the list goes once they explain themselves).
        static const std::vector<std::pair<juce::String, juce::String>> knownSilent {
            { "Project > Notes", "the project's notes field has no tooltip (src/ui/project/ProjectView.cpp)" },
            { "Source Control > Graph", "the Graph pane's header is set with no help text (src/ui/versions/VersionsView.cpp)" },
            { "EqEditor > Band On", "the EQ's band controls have no tooltips (src/ui/devices/EqEditor.cpp)" },
            { "EqEditor > Band Shape", "as Band On" },
            { "EqEditor > Slope", "as Band On" },
            { "EqEditor > Frequency", "as Band On" },
            { "EqEditor > Gain", "as Band On" },
            { "EqEditor > Q", "as Band On" },
            { "EqEditor > Output", "as Band On" },
            { "Vinyl > Record > Low Cut", "the knobs at the band's ends have no tooltips (src/ui/devices/VinylEditor.cpp: makeKnob())" },
            { "Vinyl > Record > High Cut", "as Vinyl's Low Cut" },
            { "Delay > Echoes > Low Cut", "as Vinyl's (src/ui/devices/DelayEditor.cpp)" },
            { "Delay > Echoes > High Cut", "as Vinyl's" },
        };

        juce::StringArray silent;

        for (const auto& control : found.silent)
            if (std::none_of (knownSilent.begin(), knownSilent.end(),
                              [&control] (const auto& known) { return control.endsWith (known.first); }))
                silent.add (control);

        for (const auto& known : knownSilent)
        {
            CAPTURE (known.first.toStdString());
            CHECK (std::any_of (found.silent.begin(), found.silent.end(),
                                [&known] (const auto& control) { return control.endsWith (known.first); }));
        }

        INFO (found.nameless.joinIntoString ("\n").toStdString());
        CHECK (found.nameless.isEmpty());
        INFO (silent.joinIntoString ("\n").toStdString());
        CHECK (silent.isEmpty());
    }

    screen.window.setSize (1280, 800);
}

TEST_CASE ("Switching the theme puts what the window shows already in the new theme's colours")
{
    // Known bug 8 until 2026-11-06: after a switch from the dark theme to the light one, the text already in a field stayed
    // light grey on white, and a button asking for an instrument kept the dark theme's tint.
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();

    // A track with notes and no instrument, selected: the inspector shows its name in a field, and the Devices view asks for
    // an instrument.
    REQUIRE (screen.registry.invoke ("track.add"));
    auto& context = screen.session.getContext();
    auto& song = *phazite::findSong (context);
    const auto track = phazite::getAllPlaylistTracks (context).back();
    const auto clip = phazite::test::addMidiClip (song, track, { phazite::test::at (0.0), phazite::test::at (4.0) });
    phazite::test::addMidiNote (song, clip, 60, phazite::test::at (0.0), phazite::test::lasting (1.0));
    context.getSongSelection().selectOnly (track);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("view.instrument"));

    if (phazite::test::findAll<phazite::ui::DevicesView> (screen.shell).getFirst() == nullptr
        || ! phazite::test::findAll<phazite::ui::DevicesView> (screen.shell).getFirst()->isShowing())
        REQUIRE (screen.registry.invoke ("view.instrument"));

    phazite::test::dispatchPendingMessages();
    const auto name = phazite::getTrackRow (context, track)->name;
    juce::TextEditor* nameField = nullptr;

    for (auto* field : phazite::test::findAll<juce::TextEditor> (screen.shell))
        if (field->isShowing() && field->getText() == name)
            nameField = field;

    const auto tinted = [&screen]
    {
        juce::Array<juce::TextButton*> found;

        for (auto* button : phazite::test::findAll<juce::TextButton> (screen.shell))
            if (button->isShowing() && button->getButtonText() == "Choose instrument"
                && ! button->findColour (juce::TextButton::buttonColourId).isTransparent())
                found.add (button);

        return found;
    };

    REQUIRE (nameField != nullptr);
    REQUIRE_FALSE (tinted().isEmpty());

    // Each theme in turn, from the one before: the button's tint is the theme's, and the name is in the theme's text colour.
    // Its pixel that stands out most from the field's background (the colour most of the field is) is nearer the theme's
    // text than the last theme's: light grey on white was the bug; and, from the second switch on, the first switch's text
    // colour, which refreshing a field made its own colour (black on black in high contrast after the light theme).
    auto previousText = lookAndFeel.getPalette().text;

    for (const auto theme : { phazite::ui::ThemeName::light, phazite::ui::ThemeName::highContrast, phazite::ui::ThemeName::dark })
    {
        CAPTURE (static_cast<int> (theme));
        lookAndFeel.setTheme (theme);
        phazite::ui::refreshTheme (screen.window);
        phazite::test::dispatchPendingMessages();

        const auto& palette = lookAndFeel.getPalette();
        const auto attention = palette.surfaceRaised.interpolatedWith (palette.warning, phazite::ui::attentionTint);
        REQUIRE_FALSE (tinted().isEmpty());

        for (auto* button : tinted())
            CHECK (button->findColour (juce::TextButton::buttonColourId) == attention);

        const auto image = phazite::test::paintComponent (*nameField);
        const auto inside = image.getBounds().reduced (3); // not its border, nor the corners outside its rounded box
        std::map<juce::uint32, int> counts;

        for (int y = inside.getY(); y < inside.getBottom(); ++y)
            for (int x = inside.getX(); x < inside.getRight(); ++x)
                ++counts[image.getPixelAt (x, y).getARGB()];

        REQUIRE_FALSE (counts.empty());
        const auto background = juce::Colour (
            std::max_element (counts.begin(), counts.end(), [] (const auto& a, const auto& b) { return a.second < b.second; })->first);
        auto text = background;

        for (const auto& [argb, count] : counts)
            if (phazite::ui::getContrastRatio (juce::Colour (argb), background) > phazite::ui::getContrastRatio (text, background))
                text = juce::Colour (argb);

        const auto distance = [] (juce::Colour a, juce::Colour b)
        {
            return std::abs ((int) a.getRed() - (int) b.getRed()) + std::abs ((int) a.getGreen() - (int) b.getGreen())
                   + std::abs ((int) a.getBlue() - (int) b.getBlue());
        };

        CAPTURE (text.toDisplayString (false).toStdString());
        CHECK (distance (text, palette.text) < distance (text, previousText));
        previousText = palette.text;
    }
}
