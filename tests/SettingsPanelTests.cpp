// SPDX-License-Identifier: AGPL-3.0-or-later
// The Settings as VS Code's Settings editor (src/ui/shell/SettingsPanel.h): one page, its table of contents, its sash and its
// search, with its count, Clear and Filter's @modified; a setting's row, its modified line, the row focused last and Reset
// Setting; the preferences bound to what the menus switch; the keys and screen readers; and the tokens' sizes in both themes.
// The Audio rows are in AudioSettingsTests.cpp; the card they show in over the window in SettingsModalTests.cpp.
#include "AppScreen.h"
#include "PaintCheck.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/base/Song.h"
#include "services/Settings.h"
#include "ui/shell/SettingsPanel.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
using phazite::Settings;
using phazite::ui::SettingRow;
using phazite::ui::SettingsPanel;
namespace SettingsTint = phazite::ui::SettingsTint;

// The settings as large as their card holds them in a window of 1280 by 800 (SettingsModalTests.cpp).
struct SettingsScreen
{
    SettingsScreen()
    {
        window.setVisible (true);
        phazite::test::dispatchPendingMessages();
    }

    static constexpr int width = 1022, height = 570;
    phazite::test::AppScreen screen;
    float scale = 1.0f; // what the Interface Size row sets, as MainWindow::setInterfaceScale() would
    SettingsPanel panel { screen.services.engineContext,
                          screen.commands,
                          screen.keyboard,
                          screen.session.getContext(),
                          screen.services.settings,
                          { [] (const juce::String&) {},
                            [this] (float newScale)
                            {
                                scale = newScale;
                                screen.services.settings.set (Settings::interfaceScale, newScale);
                            } } };
    phazite::test::TestWindow window { panel, width, height };

    SettingRow& row (const juce::String& title)
    {
        auto* found = panel.findRow (title);
        INFO ("no row " << title);
        REQUIRE (found != nullptr);
        return *found;
    }

    juce::StringArray getShownTitles() const
    {
        juce::StringArray titles;

        for (auto* shown : panel.getShownRows())
            titles.add (shown->title);

        return titles;
    }
};

bool isRowInView (SettingsPanel& panel, const SettingRow& row)
{
    const auto top = panel.getList().getViewPositionY();
    return row.getY() >= top && row.getBottom() <= top + panel.getList().getViewHeight();
}
} // namespace

TEST_CASE ("The settings are one page: the contents list the groups, a click scrolls the list to one and marks it, and so does "
           "scrolling")
{
    SettingsScreen s;
    const auto groups = s.panel.getListedGroups();

    for (const auto* group : { SettingsPanel::audio, SettingsPanel::midi, SettingsPanel::recording, SettingsPanel::appearance,
                               SettingsPanel::editing, SettingsPanel::files, SettingsPanel::versions })
    {
        INFO (group);
        CHECK (groups.contains (group));
    }

    CHECK (groups[0] == SettingsPanel::audio);
    CHECK (s.panel.getMarkedGroup() == SettingsPanel::audio);
    CHECK (s.panel.getCount (SettingsPanel::files) == 0); // counts show only while searching

    // A click on an entry: the list scrolled to the group's title (or as far as it goes), the entry marked.
    phazite::test::Mouse mouse (s.panel);
    const auto& list = s.panel.getList();
    const auto maxScroll = juce::jmax (0, list.getViewedComponent()->getHeight() - list.getViewHeight());

    for (const auto* group : { SettingsPanel::editing, SettingsPanel::midi, SettingsPanel::versions })
    {
        INFO (group);
        const auto entry = s.panel.getContentsEntry (group);
        REQUIRE (! entry.isEmpty());
        mouse.click (entry.getCentre());
        mouse.pause();
        CHECK (list.getViewPositionY() == juce::jmin (maxScroll, s.panel.getGroupTop (group)));
        CHECK (s.panel.getMarkedGroup() == group);
    }

    // Scrolling marks the group in view.
    s.panel.getList().setViewPosition (0, 0);
    CHECK (s.panel.getMarkedGroup() == SettingsPanel::audio);
    s.panel.getList().setViewPosition (0, s.panel.getGroupTop (SettingsPanel::recording) + 1);
    CHECK (s.panel.getMarkedGroup() == SettingsPanel::recording);

    // Shortcuts, its own page in the list's place, and back with a search.
    mouse.click (s.panel.getContentsEntry (SettingsPanel::shortcuts).getCentre());
    CHECK (s.panel.showsShortcuts());
    CHECK (s.panel.getShortcuts().isShowing());
    CHECK_FALSE (s.panel.getList().isVisible());
    CHECK (s.panel.getMarkedGroup() == SettingsPanel::shortcuts);
    s.panel.search ("overdub");
    CHECK_FALSE (s.panel.showsShortcuts());
    CHECK (s.panel.getList().isShowing());

    // The commands that open the settings at a group.
    s.panel.search ({});
    s.panel.showGroup (SettingsPanel::files);
    CHECK (s.panel.getMarkedGroup() == SettingsPanel::files);
    s.panel.showGroup (SettingsPanel::shortcuts);
    CHECK (s.panel.showsShortcuts());
}

TEST_CASE ("A search filters the settings by their names, descriptions and groups, the contents count what is left, and no match "
           "says so")
{
    SettingsScreen s;
    const auto all = s.getShownTitles();

    CHECK (s.panel.getCountText().isEmpty());
    CHECK_FALSE (s.panel.getClearButton().isEnabled());
    s.panel.search ("auto save");
    CHECK (s.getShownTitles() == juce::StringArray ("Auto Save"));
    CHECK (s.panel.getListedGroups() == juce::StringArray (SettingsPanel::files));
    CHECK (s.panel.getCount (SettingsPanel::files) == 1);
    CHECK_FALSE (s.panel.showsNoResults());
    CHECK (s.panel.getCountText() == "1 Setting Found");

    // By description, and by group.
    s.panel.search ("sixteenth");
    CHECK (s.getShownTitles() == juce::StringArray ("Quantize Recordings"));
    s.panel.search ("recording");
    CHECK (s.getShownTitles().contains ("Overdub"));
    CHECK (s.getShownTitles().contains ("Count-In"));
    CHECK (s.panel.getCount (SettingsPanel::recording) == 4);
    CHECK (s.panel.getCountText() == juce::String (s.getShownTitles().size()) + " Settings Found");

    s.panel.search ("zebra harmonica");
    CHECK (s.getShownTitles().isEmpty());
    CHECK (s.panel.getListedGroups().isEmpty());
    CHECK (s.panel.showsNoResults());
    CHECK (s.panel.getCountText() == "No Settings Found");
    CHECK (s.panel.getClearButton().isEnabled());

    // Its field types the same.
    s.panel.getSearchField().setText ("", true);
    phazite::test::dispatchPendingMessages(); // a field tells of its change in a message
    CHECK (s.getShownTitles() == all);
    CHECK_FALSE (s.panel.showsNoResults());
    CHECK (s.panel.getCount (SettingsPanel::files) == 0);
    CHECK (s.panel.getCountText().isEmpty());

    // Clear empties it, the keys in the field.
    s.panel.search ("overdub");
    phazite::test::Mouse mouse (s.panel);
    mouse.click (s.panel.getClearButton().getBounds().getCentre());
    CHECK (s.panel.getSearchField().isEmpty());
    CHECK (s.getShownTitles() == all);
    CHECK (s.panel.getSearchField().hasKeyboardFocus (false));
    CHECK_FALSE (s.panel.getClearButton().isEnabled());
}

TEST_CASE ("Filter's Modified puts @modified in the search, which lists exactly the settings that differ from their defaults, and "
           "the count says how many")
{
    SettingsScreen s;
    auto& settings = s.screen.services.settings;
    const auto modifiedTitles = [&s]
    {
        juce::StringArray titles;

        for (const auto& title : s.getShownTitles())
            if (s.row (title).isModifiedNow())
                titles.add (title);

        return titles;
    };

    // Those that differ as it starts (on this machine: a MIDI input switched off, say), then two more.
    const auto before = modifiedTitles();
    REQUIRE (phazite::test::runMenuItem (s.panel.getFilterMenu(), { "Modified" }));
    CHECK (s.panel.getSearchField().getText() == SettingsPanel::modifiedFilter);
    CHECK (s.panel.getSearchField().hasKeyboardFocus (false));
    CHECK (s.getShownTitles() == before);
    CHECK_FALSE (before.contains ("Undo Steps"));
    CHECK_FALSE (before.contains ("Scroll Continuously"));

    settings.set (Settings::undoSteps, 500.0);
    REQUIRE (s.screen.registry.invoke ("view.scrollContinuously"));
    phazite::test::dispatchPendingMessages();
    s.panel.search ({});
    const auto after = modifiedTitles();
    CHECK (after.size() == before.size() + 2);
    s.panel.search (SettingsPanel::modifiedFilter);
    CHECK (s.getShownTitles() == after);
    CHECK (s.getShownTitles().contains ("Scroll Continuously"));
    CHECK (s.getShownTitles().contains ("Undo Steps"));
    CHECK (s.panel.getCountText() == juce::String (after.size()) + " Settings Found");
    CHECK (s.panel.getCount (SettingsPanel::editing) == 2);

    s.panel.search ("undo");
    REQUIRE (phazite::test::runMenuItem (s.panel.getFilterMenu(), { "Modified" }));
    CHECK (s.panel.getSearchField().getText() == "undo @modified");
    CHECK (s.getShownTitles() == juce::StringArray ("Undo Steps"));
    CHECK (s.panel.getCountText() == "1 Setting Found");

    // Modified twice puts it there once; a setting put back leaves the list.
    REQUIRE (phazite::test::runMenuItem (s.panel.getFilterMenu(), { "Modified" }));
    CHECK (s.panel.getSearchField().getText() == "undo @modified");
    CHECK (phazite::test::runMenuItem (s.row ("Undo Steps").getMenu(), { "Reset Setting" }));
    phazite::test::dispatchPendingMessages();
    CHECK (s.getShownTitles().isEmpty());

    // Without the filter, every setting again.
    s.panel.search ({});
    CHECK (s.getShownTitles().contains ("Undo Steps"));
    REQUIRE (s.screen.registry.invoke ("view.scrollContinuously"));
}

TEST_CASE ("The sash between the table of contents and the list sizes it, at least 100 px and the list at least 500, goes back to "
           "200 px on a double-click, and its width comes back in new settings")
{
    auto s = std::make_unique<SettingsScreen>();
    auto& settings = s->screen.services.settings;
    auto& sash = s->panel.getSash();
    REQUIRE (s->panel.getContentsWidth() == SettingsTint::tocWidth);
    CHECK (sash.isVisible());
    CHECK (sash.getBounds().contains (SettingsTint::tocWidth, s->panel.getList().getY() + 10));

    // Dragged: the list follows, the width kept as the drag ends.
    phazite::test::Mouse mouse (s->panel);
    const auto start = sash.getBounds().getCentre();
    mouse.press (start);
    mouse.dragTo (start + juce::Point<int> (60, 0));
    CHECK (s->panel.getContentsWidth() == SettingsTint::tocWidth + 60);
    CHECK (s->panel.getList().getX() == SettingsTint::tocWidth + 60);
    CHECK (s->panel.getContents().getRight() == SettingsTint::tocWidth + 60);
    mouse.release();
    CHECK (settings.get (Settings::settingsContentsWidth) == doctest::Approx (SettingsTint::tocWidth + 60));

    // No narrower than its least, nor the list.
    mouse.pause();
    mouse.press (sash.getBounds().getCentre());
    mouse.dragTo ({ 0, start.y });
    CHECK (s->panel.getContentsWidth() == SettingsTint::tocMinimumWidth);
    mouse.dragTo ({ SettingsScreen::width, start.y });
    CHECK (s->panel.getContentsWidth() == SettingsScreen::width - SettingsTint::listMinimumWidth);
    mouse.release();
    mouse.pause();

    // A double-click: 200 again.
    mouse.doubleClick (sash.getBounds().getCentre());
    CHECK (s->panel.getContentsWidth() == SettingsTint::tocWidth);
    CHECK (settings.get (Settings::settingsContentsWidth) == doctest::Approx (SettingsTint::tocWidth));

    // Kept: the settings opened again take it.
    mouse.pause();
    mouse.press (sash.getBounds().getCentre());
    mouse.dragTo (sash.getBounds().getCentre() + juce::Point<int> (100, 0));
    mouse.release();
    REQUIRE (s->panel.getContentsWidth() == SettingsTint::tocWidth + 100);
    SettingsPanel again { s->screen.services.engineContext, s->screen.commands, s->screen.keyboard,
                          s->screen.session.getContext(),   settings,           {} };
    phazite::test::TestWindow window { again, SettingsScreen::width, SettingsScreen::height };
    CHECK (again.getContentsWidth() == SettingsTint::tocWidth + 100);
    CHECK (again.getList().getX() == SettingsTint::tocWidth + 100);
}

TEST_CASE ("The row focused last keeps its wash until another row is focused, outlined only while the keys are in it; Shift+F9 opens "
           "its menu as Shift+F10 does")
{
    SettingsScreen s;
    auto& undo = s.row ("Undo Steps");
    auto& scroll = s.row ("Scroll Continuously");
    REQUIRE (undo.getControl() != nullptr);
    REQUIRE (scroll.getControl() != nullptr);
    const auto washed = [] (SettingRow& row)
    {
        // The middle of its contents, where no text is: the hover wash at 60 % or 30 %, or nothing.
        const auto image = row.createComponentSnapshot (row.getLocalBounds());
        return image.getPixelAt (row.getWidth() - SettingsTint::rowInset - 4, row.getHeight() - 4).getAlpha();
    };

    undo.getControl()->grabKeyboardFocus();
    CHECK (undo.isFocusedRow());
    CHECK (undo.isHighlighted());
    CHECK (undo.getGearButton().isVisible());
    const auto focusWash = washed (undo);
    CHECK (focusWash > 0);

    // The keys to the search: still the focused row, washed as much, its gear shown.
    s.panel.getSearchField().grabKeyboardFocus();
    CHECK (undo.isFocusedRow());
    CHECK (washed (undo) == focusWash);
    CHECK (undo.getGearButton().isVisible());

    // Another row focused: that one.
    scroll.getControl()->grabKeyboardFocus();
    CHECK_FALSE (undo.isFocusedRow());
    CHECK (scroll.isFocusedRow());
    CHECK (washed (undo) == 0);
    CHECK_FALSE (undo.getGearButton().isVisible());

    // Shift+F9 and Shift+F10, from the row's control, show its menu, which has Reset Setting (the menu is really shown, and may
    // be gone again at once where the program isn't the foreground one: the row counts what it showed).
    const phazite::test::ScopedDisplay display;
    CHECK (phazite::test::findMenuItem (scroll.getMenu(), { "Reset Setting" }) != nullptr);

    for (const auto key : { juce::KeyPress::F9Key, juce::KeyPress::F10Key })
    {
        const auto shown = scroll.getMenusShown();
        scroll.getControl()->grabKeyboardFocus();
        s.window.pressKey (juce::KeyPress (key, juce::ModifierKeys::shiftModifier, 0));
        CHECK (scroll.getMenusShown() == shown + 1);
        CHECK ((juce::Component::getCurrentlyModalComponent() != nullptr || ! juce::Process::isForegroundProcess()));
        juce::PopupMenu::dismissAllActiveMenus();
        phazite::test::dispatchPendingMessages();
    }

    // Another key shows nothing.
    const auto shown = scroll.getMenusShown();
    s.window.pressKey (juce::KeyPress (juce::KeyPress::F9Key));
    CHECK (scroll.getMenusShown() == shown);
}

TEST_CASE ("A setting changed from its default shows the modified line, and Reset Setting puts the default back")
{
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    SettingsScreen s;
    auto& settings = s.screen.services.settings;

    // The line is VS Code's amber at 40 % (settings.modifiedItemIndicator), over whatever wash the row has.
    CHECK (lookAndFeel.getPalette().modified == juce::Colour (0x66bb8009));
    const auto showsLine = [] (SettingRow& row)
    {
        const auto image = row.createComponentSnapshot (row.getLocalBounds());
        const auto pixel = image.getPixelAt (SettingsTint::rowInset + SettingsTint::modifiedLeft, SettingsTint::modifiedTop + 1);
        return pixel.getAlpha() >= 0x66 && pixel.getRed() > pixel.getBlue() + 60;
    };

    // A number: Undo Steps, which songs made from now on take (app/AppServices.h).
    auto& undo = s.row ("Undo Steps");
    CHECK_FALSE (undo.isModified());
    CHECK_FALSE (showsLine (undo));
    CHECK_FALSE (phazite::test::findMenuItem (undo.getMenu(), { "Reset Setting" })->isEnabled);

    auto* field = dynamic_cast<juce::Slider*> (undo.getControl());
    REQUIRE (field != nullptr);
    field->setValue (500.0, juce::sendNotificationSync);
    CHECK (settings.get (Settings::undoSteps) == doctest::Approx (500.0));
    CHECK (phazite::Song::getUndoSteps() == 500);
    CHECK (undo.isModified());
    CHECK (showsLine (undo));

    CHECK (phazite::test::runMenuItem (undo.getMenu(), { "Reset Setting" }));
    CHECK (settings.get (Settings::undoSteps) == doctest::Approx (Settings::undoSteps.defaultValue));
    CHECK (phazite::Song::getUndoSteps() == juce::roundToInt (Settings::undoSteps.defaultValue));
    CHECK (field->getValue() == doctest::Approx (Settings::undoSteps.defaultValue));
    CHECK_FALSE (undo.isModified());
    CHECK_FALSE (showsLine (undo));

    // A switch, by a click on its sentence: the command the View menu ticks.
    auto& scroll = s.row ("Scroll Continuously");
    s.panel.showGroup (SettingsPanel::editing);
    REQUIRE (scroll.isSwitch());
    auto* button = dynamic_cast<juce::Button*> (scroll.getControl());
    REQUIRE (button != nullptr);
    CHECK_FALSE (button->getToggleState());
    phazite::test::Mouse mouse (scroll);
    mouse.click ({ button->getRight() + SettingsTint::tickGap + 10, button->getBounds().getCentreY() });
    CHECK (button->getToggleState());
    CHECK (settings.get (Settings::scrollContinuously));
    CHECK (s.screen.registry.find ("view.scrollContinuously")->isTicked());
    CHECK (scroll.isModified());
    CHECK (showsLine (scroll));

    CHECK (phazite::test::runMenuItem (scroll.getMenu(), { "Reset Setting" }));
    CHECK_FALSE (settings.get (Settings::scrollContinuously));
    CHECK_FALSE (button->getToggleState());
    CHECK_FALSE (showsLine (scroll));

    // A choice: the interface's size, through what the window's owner does.
    auto& size = s.row ("Interface Size");
    auto* box = dynamic_cast<juce::ComboBox*> (size.getControl());
    REQUIRE (box != nullptr);
    CHECK (box->getText() == "100 %");
    box->setText ("125 %", juce::sendNotificationSync);
    CHECK (s.scale == doctest::Approx (1.25f));
    CHECK (size.isModified());
    CHECK (phazite::test::runMenuItem (size.getMenu(), { "Reset Setting" }));
    CHECK (s.scale == doctest::Approx (1.0f));
    CHECK (box->getText() == "100 %");
}

TEST_CASE ("A preference switched in a menu shows in its row at once, as the menus show the row's")
{
    SettingsScreen s;
    s.screen.session.onAutoSaveChanged = [&s] (bool on)
    { s.screen.services.settings.set (Settings::autoSave, on); }; // as MainWindow keeps it
    auto* overdub = dynamic_cast<juce::Button*> (s.row ("Overdub").getControl());
    auto* autoSave = dynamic_cast<juce::Button*> (s.row ("Auto Save").getControl());
    auto* bounce = dynamic_cast<juce::Button*> (s.row ("Bounce Each Version").getControl());
    REQUIRE (overdub != nullptr);
    REQUIRE (autoSave != nullptr);
    REQUIRE (bounce != nullptr);

    for (const auto& [command, button] : { std::pair { "transport.overdub", overdub }, std::pair { "project.autoSave", autoSave },
                                           std::pair { "version.bounceEach", bounce } })
    {
        INFO (juce::String (command).toStdString());
        const auto ticked = [&s, command = juce::String (command)] { return s.screen.registry.find (command)->isTicked(); };
        CHECK (button->getToggleState() == ticked());

        REQUIRE (s.screen.registry.invoke (command));
        phazite::test::dispatchPendingMessages();
        CHECK (button->getToggleState() == ticked());

        button->setToggleState (! button->getToggleState(), juce::sendNotificationSync);
        phazite::test::dispatchPendingMessages();
        CHECK (button->getToggleState() == ticked());
    }

    // The main window's own (a theme, Hover Help, the startup sound, the update check) aren't registered here: no rows for them.
    CHECK (s.panel.findRow ("Theme") == nullptr);
    CHECK (s.panel.findRow ("Startup Sound") == nullptr);
}

TEST_CASE ("Tab goes through the search, its buttons as one stop, the contents and the rows' controls, each with its setting's name "
           "and description, and the list scrolls a focused row into view")
{
    SettingsScreen s;
    s.panel.getSearchField().grabKeyboardFocus();
    REQUIRE (s.panel.getSearchField().hasKeyboardFocus (true));
    auto& clear = s.panel.getClearButton();
    auto& filter = s.panel.getFilterButton();
    CHECK (clear.getTitle() == "Clear Settings Search Input"); // VS Code's words
    CHECK (filter.getTitle() == "Filter Settings");

    // Nothing searched: Clear is off, and Tab goes to Filter, Left and Right stay there, Space opens its menu (dismissed here).
    const auto focused = [] { return juce::Component::getCurrentlyFocusedComponent(); };
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    CHECK (focused() == &filter);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::leftKey));
    CHECK (focused() == &filter);

    // Something searched: Tab from the search lands on Clear, Right goes to Filter and Left back, and Shift+Tab from either goes
    // back to the search.
    s.panel.search ("audio");
    s.panel.getSearchField().grabKeyboardFocus();
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    CHECK (focused() == &clear);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::rightKey));
    CHECK (focused() == &filter);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::rightKey)); // round, as VS Code's action bar goes
    CHECK (focused() == &clear);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::leftKey));
    CHECK (focused() == &filter);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0));
    CHECK (focused() == &s.panel.getSearchField());
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    CHECK (focused() == &clear); // the stop is Clear again once the keys have left them

    // Space on Clear presses it: the search cleared, the keys in the field; Enter presses too.
    s.window.pressKey (juce::KeyPress (juce::KeyPress::spaceKey));
    phazite::test::dispatchPendingMessages();
    CHECK (s.panel.getSearchField().isEmpty());
    CHECK (focused() == &s.panel.getSearchField());
    s.panel.search ("audio");
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    REQUIRE (focused() == &clear);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    phazite::test::dispatchPendingMessages();
    CHECK (s.panel.getSearchField().isEmpty());

    // From the buttons, Tab goes on to the contents.
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    REQUIRE (focused() == &filter);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));
    REQUIRE (focused() != nullptr);
    CHECK (focused()->getParentComponent() == &s.panel.getContents());
    CHECK (focused()->getTitle() == SettingsPanel::audio);

    // Down and Up go through the contents too.
    s.window.pressKey (juce::KeyPress (juce::KeyPress::downKey));
    CHECK (focused()->getTitle() == SettingsPanel::midi);
    s.window.pressKey (juce::KeyPress (juce::KeyPress::upKey));
    CHECK (focused()->getTitle() == SettingsPanel::audio);

    // On past the contents' entries, to the first row's control.
    for (int i = 0; i < 20 && focused() != nullptr && focused()->findParentComponentOfClass<SettingRow>() == nullptr; ++i)
        s.window.pressKey (juce::KeyPress (juce::KeyPress::tabKey));

    REQUIRE (focused() != nullptr);
    auto* row = focused()->findParentComponentOfClass<SettingRow>();
    REQUIRE (row != nullptr);
    CHECK (row == s.panel.getShownRows().getFirst());
    CHECK (row->isHighlighted());

    // Every control has its setting's name and description.
    for (auto* shown : s.panel.getShownRows())
    {
        if (auto* control = shown->getControl())
        {
            INFO (shown->title);
            CHECK (control->getTitle() == shown->title);
            CHECK (control->getDescription() == shown->getDescriptionText());
        }
    }

    // A row far down, focused: the list scrolls to it.
    auto& last = *s.panel.getShownRows().getLast();
    REQUIRE (last.getControl() != nullptr);
    CHECK_FALSE (isRowInView (s.panel, last));
    last.getControl()->grabKeyboardFocus();
    CHECK (isRowInView (s.panel, last));
    CHECK (last.isHighlighted());
}

TEST_CASE ("The settings keep to the tokens' sizes and fonts, in the dark theme and the light one")
{
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    SettingsScreen s;

    for (const auto theme : { phazite::ui::ThemeName::dark, phazite::ui::ThemeName::light })
    {
        lookAndFeel.setTheme (theme);
        phazite::ui::refreshTheme (s.window);
        CAPTURE (theme == phazite::ui::ThemeName::dark ? "dark" : "light");

        CHECK (s.panel.getSearchField().getHeight() == SettingsTint::searchHeight);

        // The table of contents 200 px wide with its entries 24 px in, the list beside it; a group's title 42 px tall.
        CHECK (s.panel.getContentsWidth() == SettingsTint::tocWidth);
        CHECK (s.panel.getContents().getX() == SettingsTint::sideInset);
        CHECK (s.panel.getContents().getRight() == SettingsTint::tocWidth);
        CHECK (s.panel.getList().getX() == SettingsTint::tocWidth);
        CHECK (s.panel.getShownRows().getFirst()->getY() == 42);

        // The search's count, Clear and Filter at its right end, inside it.
        const auto field = s.panel.getSearchField().getBounds();
        CHECK (s.panel.getFilterButton().getBounds() == juce::Rectangle<int> (field.getRight() - 24, field.getY() + 2, 22, 22));
        CHECK (s.panel.getClearButton().getBounds() == juce::Rectangle<int> (field.getRight() - 46, field.getY() + 2, 22, 22));

        for (auto* entry : s.panel.getContents().getChildren())
            if (entry->isVisible())
                CHECK (entry->getHeight() == SettingsTint::tocRowHeight);

        for (auto* row : s.panel.getShownRows())
        {
            INFO (row->title);
            CHECK (row->getHeight() == row->getHeightForWidth (row->getWidth()));

            auto* control = row->getControl();

            if (control == nullptr)
                continue;

            CHECK (control->getX() == SettingsTint::rowInset + SettingsTint::padSide);

            if (row->isSwitch())
                CHECK (control->getHeight() == juce::roundToInt (phazite::ui::ButtonTint::tickBoxSize));
            else if (dynamic_cast<juce::ComboBox*> (control) != nullptr || dynamic_cast<juce::Slider*> (control) != nullptr
                     || dynamic_cast<juce::TextButton*> (control) != nullptr)
                CHECK (control->getHeight() == SettingsTint::controlHeight);

            if (auto* box = dynamic_cast<juce::ComboBox*> (control))
            {
                CHECK (box->getWidth() == SettingsTint::choiceWidth);
                CHECK (lookAndFeel.getComboBoxFont (*box).getHeight() == doctest::Approx (phazite::ui::TextSize::body));
            }
        }

        // Every label's text at one of the ramp's sizes (JUCE's own is 15 px), or the search's count's (VS Code's badges').
        for (auto* label : phazite::test::findAll<juce::Label> (s.panel))
        {
            if (! label->isShowing())
                continue;

            INFO (label->getText());
            const auto height = label->getFont().getHeight();
            CHECK ((juce::approximatelyEqual (height, phazite::ui::TextSize::body)
                    || juce::approximatelyEqual (height, phazite::ui::TextSize::small)
                    || juce::approximatelyEqual (height, SettingsTint::countTextSize)));
        }

        phazite::test::checkPaints (s.panel, "the settings");
        s.panel.showGroup (SettingsPanel::shortcuts);
        CHECK (s.panel.getShortcuts().getKeymapBox().getHeight() == SettingsTint::controlHeight);
        CHECK (s.panel.getShortcuts().getEditor().getSearchField().getHeight() == SettingsTint::controlHeight);
        phazite::test::checkPaints (s.panel, "the shortcuts");
        s.panel.showGroup (SettingsPanel::audio);
    }
}
