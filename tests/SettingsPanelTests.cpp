// SPDX-License-Identifier: AGPL-3.0-or-later
// The Settings window as VS Code's Settings editor (src/ui/shell/SettingsPanel.h): one page, its table of contents and its
// search; a setting's row, its modified line and Reset Setting; the preferences bound to what the menus switch; the keys and
// screen readers; and the tokens' sizes in both themes. The Audio rows are in AudioSettingsTests.cpp.
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

struct SettingsScreen
{
    SettingsScreen()
    {
        window.setVisible (true);
        phazite::test::dispatchPendingMessages();
    }

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
    phazite::test::TestWindow window { panel, SettingsTint::windowWidth, SettingsTint::windowHeight };

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

    // Both settings that words beginning so name: the project's Auto Save, and the history's automatic saves.
    s.panel.search ("auto save");
    CHECK (s.getShownTitles() == juce::StringArray { "Auto Save", "Show Automatic Saves" });
    CHECK (s.panel.getListedGroups() == juce::StringArray { SettingsPanel::files, SettingsPanel::versions });
    CHECK (s.panel.getCount (SettingsPanel::files) == 1);
    CHECK (s.panel.getCount (SettingsPanel::versions) == 1);
    CHECK_FALSE (s.panel.showsNoResults());

    // By description, and by group.
    s.panel.search ("sixteenth");
    CHECK (s.getShownTitles() == juce::StringArray ("Quantize Recordings"));
    s.panel.search ("recording");
    CHECK (s.getShownTitles().contains ("Overdub"));
    CHECK (s.getShownTitles().contains ("Count-In"));
    CHECK (s.panel.getCount (SettingsPanel::recording) == 4);

    s.panel.search ("zebra harmonica");
    CHECK (s.getShownTitles().isEmpty());
    CHECK (s.panel.getListedGroups().isEmpty());
    CHECK (s.panel.showsNoResults());

    // Its field types the same.
    s.panel.getSearchField().setText ("", true);
    phazite::test::dispatchPendingMessages(); // a field tells of its change in a message
    CHECK (s.getShownTitles() == all);
    CHECK_FALSE (s.panel.showsNoResults());
    CHECK (s.panel.getCount (SettingsPanel::files) == 0);
}

TEST_CASE ("A setting changed from its default shows the modified line, and Reset Setting puts the default back")
{
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    SettingsScreen s;
    auto& settings = s.screen.services.settings;
    const auto modifiedColour = lookAndFeel.getPalette().modified;
    const auto showsLine = [modifiedColour] (SettingRow& row)
    {
        const auto image = row.createComponentSnapshot (row.getLocalBounds());
        return image.getPixelAt (SettingsTint::rowInset + SettingsTint::modifiedLeft, SettingsTint::modifiedTop + 1) == modifiedColour;
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
    auto* automatic = dynamic_cast<juce::Button*> (s.row ("Show Automatic Saves").getControl());
    REQUIRE (overdub != nullptr);
    REQUIRE (autoSave != nullptr);
    REQUIRE (bounce != nullptr);
    REQUIRE (automatic != nullptr);

    for (const auto& [command, button] : { std::pair { "transport.overdub", overdub }, std::pair { "project.autoSave", autoSave },
                                           std::pair { "version.bounceEach", bounce }, std::pair { "version.showAutomatic", automatic } })
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

TEST_CASE ("Tab goes through the search, the contents and the rows' controls, each with its setting's name and description, and "
           "the list scrolls a focused row into view")
{
    SettingsScreen s;
    s.panel.getSearchField().grabKeyboardFocus();
    REQUIRE (s.panel.getSearchField().hasKeyboardFocus (true));

    const auto focused = [] { return juce::Component::getCurrentlyFocusedComponent(); };
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
        CHECK (s.panel.getContents().getX() == SettingsTint::sideInset);
        CHECK (s.panel.getContents().getWidth() == SettingsTint::tocWidth);

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

        // Every label's text at one of the ramp's sizes (JUCE's own is 15 px).
        for (auto* label : phazite::test::findAll<juce::Label> (s.panel))
        {
            if (! label->isShowing())
                continue;

            INFO (label->getText());
            const auto height = label->getFont().getHeight();
            CHECK ((juce::approximatelyEqual (height, phazite::ui::TextSize::body)
                    || juce::approximatelyEqual (height, phazite::ui::TextSize::small)));
        }

        phazite::test::checkPaints (s.panel, "the settings");
        s.panel.showGroup (SettingsPanel::shortcuts);
        CHECK (s.panel.getShortcuts().getKeymapBox().getHeight() == SettingsTint::controlHeight);
        CHECK (s.panel.getShortcuts().getEditor().getSearchField().getHeight() == SettingsTint::controlHeight);
        phazite::test::checkPaints (s.panel, "the shortcuts");
        s.panel.showGroup (SettingsPanel::audio);
    }
}
