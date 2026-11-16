// SPDX-License-Identifier: AGPL-3.0-or-later
// The settings' Shortcuts tab (ui/shell/ShortcutsEditor.h), with the app's commands and keys, worked with the mouse and the
// keys as the user works it: searching by name and by key, giving a command a key another has, and the changes saved.
#include "AppScreen.h"
#include "TestHelpers.h"
#include "TestKeyboardLayouts.h"
#include "UiTestHelpers.h"
#include "services/Settings.h"
#include "ui/input/KeymapStore.h"
#include "ui/shell/SettingsPanel.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
using phazite::ui::ShortcutsEditor;

// The app's commands, two of the test's own that share a key by their priorities, and the Shortcuts tab in a window.
struct Shortcuts
{
    // The tab's computer keyboard, and its keys, on a layout: the US layout's, unless a test gives another.
    explicit Shortcuts (const phazite::ui::KeyboardLayout& layout = phazite::ui::getUsLayout())
        : keyboard ([] (const juce::MidiMessage&) {}, layout)
    {
        keyboard.leaveKeysOf (&screen.commands);
        screen.registry.add ({ .id = "test.tracks",
                               .name = "Test Tracks",
                               .description = "Acts on tracks.",
                               .perform = [this] { ran.add ("test.tracks"); } });
        screen.registry.add ({ .id = "test.notes",
                               .name = "Test Notes",
                               .description = "Acts on the selected notes.",
                               .perform = [this] { ran.add ("test.notes"); },
                               .isEnabled = [this] { return notesSelected; },
                               .keyPriority = phazite::Command::notePriority });
        tab = std::make_unique<phazite::ui::ShortcutSettings> (screen.commands, keyboard, [] (const juce::String&) {});
        window = std::make_unique<phazite::test::TestWindow> (*tab, 900, 640);
        phazite::test::dispatchPendingMessages();
    }

    ~Shortcuts()
    {
        window = nullptr;
        tab = nullptr;
    }

    ShortcutsEditor& editor() { return tab->getEditor(); }

    // Clicks a component of the tab in its middle.
    void click (juce::Component& c)
    {
        phazite::test::Mouse mouse { *window };
        mouse.click (window->getLocalArea (&c, c.getLocalBounds()).getCentre());
        phazite::test::dispatchPendingMessages();
    }

    // Clicks a row of the list, at its command's name (or, with `onPencil`, on its pencil, which shows on the row selected).
    void clickRow (int row, bool onPencil = false)
    {
        auto& list = editor().getList();
        REQUIRE (row >= 0);
        list.scrollToEnsureRowIsOnscreen (row);
        const auto bounds = window->getLocalArea (&list, list.getRowPosition (row, true));
        phazite::test::Mouse mouse { *window };
        mouse.click ({ bounds.getX() + (onPencil ? phazite::ui::Space::s + phazite::ui::ShortcutsTint::pencil / 2 : bounds.getWidth() / 3),
                       bounds.getCentreY() });
        phazite::test::dispatchPendingMessages();
    }

    void press (const juce::KeyPress& key)
    {
        window->pressKey (key);
        phazite::test::dispatchPendingMessages();
    }

    // Searches for the command by its name and clicks its row (of the key, or of the command without one).
    void selectRow (const juce::String& name, const juce::String& id, std::optional<juce::KeyPress> key = {})
    {
        editor().search (name);
        clickRow (editor().findRow (id, key));
        REQUIRE (editor().getList().getSelectedRow() == editor().findRow (id, key));
    }

    // Gives the selected row's command a key, in the card the button opens: the key pressed, then Assign.
    juce::String giveKey (juce::Button& button, const juce::KeyPress& key)
    {
        click (button);
        auto* capture = editor().getKeyCapture();
        REQUIRE (capture != nullptr);
        CHECK (capture->hasKeyboardFocus (false));
        press (key);
        REQUIRE (capture->getKey() == phazite::ui::getShortcutKey (key, keyboard.getLayout()));
        const auto share = capture->getShareText();
        click (capture->getAssignButton());
        CHECK (editor().getKeyCapture() == nullptr);
        return share;
    }

    // A command's keys, saved as MainWindow saves them at each change, and read back by another bridge.
    juce::Array<juce::KeyPress> readBack (const juce::String& id)
    {
        phazite::ui::saveKeySettings (screen.commands, screen.services.settings);
        phazite::ui::CommandBridge reloaded { screen.registry };
        phazite::ui::loadKeySettings (reloaded, screen.services.settings);
        return reloaded.getKeyPresses (id);
    }

    phazite::test::AppScreen screen;
    juce::StringArray ran;
    bool notesSelected = false;
    phazite::ui::ComputerKeyboard keyboard;
    std::unique_ptr<phazite::ui::ShortcutSettings> tab;
    std::unique_ptr<phazite::test::TestWindow> window;
};

juce::KeyPress keyOf (const char* description)
{
    return juce::KeyPress::createFromDescription (description);
}
} // namespace

TEST_CASE ("The shortcuts editor gives a command a key another has, and both keep it where priorities tell them apart")
{
    // Known bug 110 until 2026-11-15: JUCE's editor took a key away from every other command that had it, so the keys the
    // keymaps share on purpose (Esc, Up, Down...) couldn't be set up again.
    Shortcuts s;
    const auto shared = keyOf ("command + shift + K");

    s.selectRow ("Test Tracks", "test.tracks");
    CHECK (s.giveKey (s.editor().getChangeButton(), shared) == "No other command has this key.");

    s.selectRow ("Test Notes", "test.notes");
    const auto share = s.giveKey (s.editor().getChangeButton(), shared);
    CHECK (share.contains ("1 other command has this key: Test Tracks."));
    CHECK (share.contains ("Both keep it: Test Notes runs whenever it can, Test Tracks otherwise."));

    // Both keep it, each shows on its row, and each runs in its place: the notes' while notes are selected.
    CHECK (s.screen.commands.getKeyPresses ("test.tracks").contains (shared));
    CHECK (s.screen.commands.getKeyPresses ("test.notes").contains (shared));
    s.editor().searchByKey (shared);
    CHECK (s.editor().getRows().size() == 2);
    CHECK (s.editor().findRow ("test.tracks", shared) >= 0);
    CHECK (s.editor().findRow ("test.notes", shared) >= 0);
    CHECK (s.editor().getSource (s.editor().getRows()[0]) == "User");

    CHECK (s.screen.shortcutKeys.keyPressed (shared, nullptr));
    s.notesSelected = true;
    s.screen.shortcutKeys.releaseHeldKeys();
    CHECK (s.screen.shortcutKeys.keyPressed (shared, nullptr));
    CHECK (s.ran == juce::StringArray { "test.tracks", "test.notes" });

    // A key the keymap shares already: Up, for the tracks, the notes and the automation points. Giving it to a command of
    // the same priority as the tracks' says which runs where both can.
    s.selectRow ("Test Tracks", "test.tracks", shared);
    const auto upShare = s.giveKey (s.editor().getAddButton(), juce::KeyPress (juce::KeyPress::upKey));
    CHECK (upShare.contains ("other commands have this key"));
    CHECK (upShare.contains ("Neither comes first: where both can run, Select Track Above runs, and Test Tracks only where Select "
                             "Track Above can't."));
    CHECK (s.screen.commands.getKeyPresses ("track.selectPrevious").contains (juce::KeyPress (juce::KeyPress::upKey)));
    CHECK (s.screen.commands.getKeyPresses ("note.transposeUp").contains (juce::KeyPress (juce::KeyPress::upKey)));
    CHECK (s.screen.commands.getKeyPresses ("test.tracks").contains (shared)); // added, not in place of the other
}

TEST_CASE ("A key set, cleared and reset in the shortcuts editor is saved and read back")
{
    Shortcuts s;
    auto& commands = s.screen.commands;
    auto& settings = s.screen.services.settings;
    const auto o = keyOf ("O"), f12 = keyOf ("F12");
    const auto readBack = [&]
    {
        phazite::ui::saveKeySettings (commands, settings); // as MainWindow does at each change
        phazite::ui::CommandBridge reloaded { s.screen.registry };
        phazite::ui::loadKeySettings (reloaded, settings);
        return reloaded.getKeyPresses ("transport.metronome");
    };

    // Set: the metronome's O changed to F12, with the pencil on its row.
    s.selectRow ("Metronome", "transport.metronome", o);
    CHECK (s.editor().getSource (s.editor().getRows()[(size_t) s.editor().getList().getSelectedRow()]) == "Phazite");
    s.clickRow (s.editor().getList().getSelectedRow(), true);
    REQUIRE (s.editor().getKeyCapture() != nullptr);
    s.press (f12);
    s.click (s.editor().getKeyCapture()->getAssignButton());
    CHECK (commands.getKeyPresses ("transport.metronome") == juce::Array<juce::KeyPress> { f12 });
    CHECK (readBack() == juce::Array<juce::KeyPress> { f12 });
    CHECK (s.editor().getSource (s.editor().getRows()[(size_t) s.editor().findRow ("transport.metronome", f12)]) == "User");

    // Cleared: the command has no key, and its row says the keymap's was taken away.
    s.click (s.editor().getRemoveButton());
    CHECK (commands.getKeyPresses ("transport.metronome").isEmpty());
    CHECK (readBack().isEmpty());
    const auto keyless = s.editor().findRow ("transport.metronome");
    REQUIRE (keyless >= 0);
    CHECK (s.editor().getSource (s.editor().getRows()[(size_t) keyless]) == "User");

    // Reset: the keymap's O again, and nothing of the command's left among the changes.
    s.clickRow (keyless);
    CHECK (s.editor().getResetButton().isEnabled());
    s.click (s.editor().getResetButton());
    CHECK (commands.getKeyPresses ("transport.metronome") == juce::Array<juce::KeyPress> { o });
    CHECK (readBack() == juce::Array<juce::KeyPress> { o });
    CHECK_FALSE (phazite::ui::getKeyChanges (commands).contains ("transport.metronome"));

    // Reset All, with a second click: Undo's Ctrl+Z taken away comes back, and the changes are none.
    s.selectRow ("Undo", "edit.undo", keyOf ("command + Z"));
    s.press (juce::KeyPress (juce::KeyPress::deleteKey));
    CHECK (commands.getKeyPresses ("edit.undo").isEmpty());
    s.click (s.editor().getResetAllButton());
    CHECK (commands.getKeyPresses ("edit.undo").isEmpty()); // the first click asks again
    s.click (s.editor().getResetAllButton());
    CHECK (commands.getKeyPresses ("edit.undo").contains (keyOf ("command + Z")));
    CHECK (phazite::ui::getKeyChanges (commands).isEmpty());
}

TEST_CASE ("The shortcuts editor finds commands by name and by key")
{
    Shortcuts s;
    auto& editor = s.editor();

    // By name, as typed: every row's command has the word.
    s.click (editor.getSearchField());
    for (const auto c : juce::String ("metro"))
        s.press (juce::KeyPress ((int) c, {}, c));

    CHECK (editor.getSearchField().getText() == "metro");
    REQUIRE_FALSE (editor.getRows().empty());
    CHECK (editor.findRow ("transport.metronome", keyOf ("O")) >= 0);

    for (const auto& row : editor.getRows())
        CHECK ((row.command->name + row.command->id + row.command->category).containsIgnoreCase ("metro"));

    // By a key's text.
    editor.search ("Ctrl+Z");
    CHECK (editor.findRow ("edit.undo", keyOf ("command + Z")) >= 0);

    // By a key pressed, with Record Keys on: every command on Up, the notes', the points' and the tracks'.
    s.click (editor.getRecordButton());
    REQUIRE (editor.getRecordButton().getToggleState());
    CHECK (editor.getSearchField().hasKeyboardFocus (false));
    s.press (juce::KeyPress (juce::KeyPress::upKey));
    const juce::KeyPress up (juce::KeyPress::upKey);

    for (const auto* id : { "track.selectPrevious", "note.transposeUp", "automation.raisePoints" })
    {
        CAPTURE (id);
        CHECK (editor.findRow (id, up) >= 0);
    }

    for (const auto& row : editor.getRows())
        CHECK (row.key == up);

    // Ctrl+Z pressed: Undo alone; a key no command has: nothing, which the help says.
    s.press (keyOf ("command + Z"));
    REQUIRE (editor.getRows().size() == 1);
    CHECK (editor.getRows()[0].command->id == "edit.undo");
    s.press (keyOf ("command + alt + shift + F12"));
    CHECK (editor.getRows().empty());
    CHECK (editor.getHelpText() == "No command matches the search.");

    // Record Keys off: typing searches again.
    s.click (editor.getRecordButton());
    CHECK_FALSE (editor.getRecordButton().getToggleState());
    CHECK (editor.getRows().size() > 100);
}

TEST_CASE ("The shortcuts editor says what a key the computer keyboard's notes use does while it plays, and works by keys alone")
{
    Shortcuts s;
    auto& editor = s.editor();

    // S plays a note while the computer keyboard plays notes (and is the Slip Tool's): giving it to Test Tracks says so.
    s.selectRow ("Test Tracks", "test.tracks");
    s.click (editor.getChangeButton());
    auto* capture = editor.getKeyCapture();
    REQUIRE (capture != nullptr);
    s.press (keyOf ("S"));
    CHECK (capture->getShareText().contains ("1 other command has this key: Slip Tool."));
    CHECK (capture->getShareText().contains ("While the computer keyboard plays notes, S plays a note"));

    // Shift+S, for Test Tracks, takes Shift and S from the Slip Tool while the keyboard plays notes.
    s.press (keyOf ("shift + S"));
    CHECK (capture->getShareText().contains ("While the computer keyboard plays notes, Shift+S runs Test Tracks, not Slip Tool"));

    // Escape is a key too, while the card has the keys; from its buttons, it closes the card, changing nothing.
    s.press (juce::KeyPress (juce::KeyPress::escapeKey));
    CHECK (capture->getKey() == juce::KeyPress (juce::KeyPress::escapeKey));
    CHECK (capture->getShareText().contains ("Smart Tool"));
    s.press (juce::KeyPress (juce::KeyPress::tabKey));
    CHECK (capture->getAssignButton().hasKeyboardFocus (false));
    s.press (juce::KeyPress (juce::KeyPress::escapeKey));
    CHECK (editor.getKeyCapture() == nullptr);
    CHECK (s.screen.commands.getKeyPresses ("test.tracks").isEmpty());

    // From the list, by keys: Enter opens the card for the selected row, Tab and Enter on Assign give the key.
    editor.getList().grabKeyboardFocus();
    s.press (juce::KeyPress (juce::KeyPress::returnKey));
    capture = editor.getKeyCapture();
    REQUIRE (capture != nullptr);
    s.press (keyOf ("F12"));
    s.press (juce::KeyPress (juce::KeyPress::tabKey));
    REQUIRE (capture->getAssignButton().hasKeyboardFocus (false));
    s.press (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK (editor.getKeyCapture() == nullptr);
    CHECK (s.screen.commands.getKeyPresses ("test.tracks") == juce::Array<juce::KeyPress> { keyOf ("F12") });
}

TEST_CASE ("The shortcuts editor keeps a key as it is matched: without what Caps Lock typed, and with the modifiers of a key that typed /")
{
    // A key kept with its text was another key than the same key typed with Caps Lock, JUCE asserted on one typed with it,
    // and one that typed / was written as "/" alone (QWERTZ's Shift+7).
    Shortcuts s;
    const juce::KeyPress capsA ('Q', {}, 'Q'), plainA ('Q', {}, 'q'), shift7 ('7', juce::ModifierKeys::shiftModifier, '/');

    s.selectRow ("Test Tracks", "test.tracks");
    s.giveKey (s.editor().getChangeButton(), capsA); // no assertion
    CHECK (s.screen.commands.getKeyPresses ("test.tracks")[0].getTextCharacter() == 0);
    CHECK (s.readBack ("test.tracks") == juce::Array<juce::KeyPress> { keyOf ("Q") });
    CHECK (s.screen.shortcutKeys.findCommandForKey (plainA)->id == "test.tracks");

    s.selectRow ("Test Notes", "test.notes");
    s.giveKey (s.editor().getChangeButton(), plainA);
    s.notesSelected = true;
    CHECK (s.screen.shortcutKeys.findCommandForKey (capsA)->id == "test.notes"); // with Caps Lock on, the same key

    s.selectRow ("Test Notes", "test.notes", keyOf ("Q"));
    s.giveKey (s.editor().getAddButton(), shift7);
    CHECK (s.screen.commands.getKeyPresses ("test.notes").contains (keyOf ("shift + 7")));
    CHECK (s.readBack ("test.notes").contains (keyOf ("shift + 7")));
    CHECK (s.readBack ("test.notes").contains (keyOf ("Q")));
}

TEST_CASE ("The shortcuts editor saves and reads back +, a number pad key and a key such as é, and shows é as itself")
{
    Shortcuts s;
    s.selectRow ("Test Tracks", "test.tracks");
    s.giveKey (s.editor().getChangeButton(), juce::KeyPress ('+', {}, '+'));
    s.giveKey (s.editor().getAddButton(), juce::KeyPress (juce::KeyPress::numberPad5));
    s.giveKey (s.editor().getAddButton(), juce::KeyPress (0xe9, {}, 0xe9));

    const auto keys = s.readBack ("test.tracks");
    CHECK (keys.size() == 3);
    CHECK (keys.contains (juce::KeyPress ('+')));
    CHECK (keys.contains (juce::KeyPress (juce::KeyPress::numberPad5)));
    CHECK (keys.contains (juce::KeyPress (0xe9)));

    // A row's screen-reader name says its command, key and source, é as É, not JUCE's "#e9".
    auto* model = s.editor().getList().getListBoxModel(); // what the list's rows tell screen readers
    const auto row = s.editor().findRow ("test.tracks", juce::KeyPress (0xe9));
    REQUIRE (row >= 0);
    REQUIRE (model != nullptr);
    CHECK (model->getNameForRow (row) == juce::String ("Test Tracks, ") + juce::String::charToString (0xc9) + ", User");
    CHECK (phazite::ui::splitKeyCaps ("Ctrl+Numpad +") == juce::StringArray { "Ctrl", "Numpad +" });
    CHECK (phazite::ui::splitKeyCaps ("ctrl + #e9") == juce::StringArray { "ctrl", juce::String::charToString (0xc9) });
}

TEST_CASE ("The key card takes no key without a code, keeps the user's key, and closes for another keymap")
{
    Shortcuts s;
    auto& editor = s.editor();
    const auto undo = keyOf ("command + Z");

    // Pause and the media keys come with no code: Assign stays off, and Undo keeps Ctrl+Z.
    s.selectRow ("Undo", "edit.undo", undo);
    s.click (editor.getChangeButton());
    auto* capture = editor.getKeyCapture();
    REQUIRE (capture != nullptr);
    s.press (juce::KeyPress (0));
    CHECK_FALSE (capture->getKey().has_value());
    CHECK_FALSE (capture->getAssignButton().isEnabled());
    s.click (capture->getAssignButton());
    CHECK (s.screen.commands.getKeyPresses ("edit.undo").contains (undo));
    REQUIRE (editor.getKeyCapture() == capture); // still open, Assign being off

    // Alt+F4 is the system's, to close the window: the card doesn't take it.
    CHECK_FALSE (capture->keyPressed (juce::KeyPress (juce::KeyPress::F4Key, juce::ModifierKeys::altModifier, 0)));
    CHECK_FALSE (capture->getKey().has_value());

    // A click on the card's words gives it the keys back.
    s.press (keyOf ("F12"));
    s.press (juce::KeyPress (juce::KeyPress::tabKey));
    REQUIRE (capture->getAssignButton().hasKeyboardFocus (false));
    s.click (capture->getShareLabel());
    CHECK (capture->hasKeyboardFocus (false));

    // Another keymap chosen while it is open: it closes, and gives nothing.
    s.screen.commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    phazite::test::dispatchPendingMessages();
    CHECK (editor.getKeyCapture() == nullptr);
    CHECK_FALSE (s.screen.commands.getKeyPresses ("edit.undo").contains (keyOf ("F12")));
}

TEST_CASE ("The key card says which command a key runs now by what it types, and takes AZERTY's digit keys as digits")
{
    // US Shift+= zooms in by the + it types; given to Test Tracks, it runs Test Tracks first, which the card says.
    {
        Shortcuts s;
        const juce::KeyPress shiftEquals ('=', juce::ModifierKeys::shiftModifier, '+');
        s.selectRow ("Test Tracks", "test.tracks");
        const auto share = s.giveKey (s.editor().getChangeButton(), shiftEquals);
        CHECK (
            share.contains ("Now this key runs Zoom In, by the + it types; given to Test Tracks, it runs Test Tracks instead, and Zoom In "
                            "loses this key."));
        CHECK (s.screen.shortcutKeys.findCommandForKey (shiftEquals)->id == "test.tracks");

        // Record Keys lists what the key runs: Test Tracks's Shift+=, not Zoom In's + any more.
        s.editor().searchByKey (shiftEquals);
        REQUIRE (s.editor().getRows().size() == 1);
        CHECK (s.editor().getRows()[0].command->id == "test.tracks");
    }

    // AZERTY's Ctrl and & key runs Finer Grid as Ctrl+1: kept as the key printed, Ctrl+&, which then runs Test Tracks first,
    // as the card says.
    {
        const auto azerty = phazite::test::SimulatedLayout::azerty();
        Shortcuts s { azerty };
        s.selectRow ("Test Tracks", "test.tracks");
        const auto share = s.giveKey (s.editor().getChangeButton(), juce::KeyPress ('&', juce::ModifierKeys::commandModifier, 0));
        CHECK (share.contains ("Now this key runs Finer Grid, as Ctrl+1; given to Test Tracks, it runs Test Tracks instead, and Finer "
                               "Grid loses this key."));
        CHECK (s.screen.commands.getKeyPresses ("test.tracks") == juce::Array<juce::KeyPress> { keyOf ("command + &") });
    }
}

TEST_CASE ("The key card doesn't take a key AltGr types a character with, and says to press it with Alt")
{
    // Kept as Alt and the key, it would run only with the left Alt, as AltGr's presses run no shortcut.
    const auto qwertz = phazite::test::SimulatedLayout::qwertz();
    Shortcuts s { qwertz };
    s.selectRow ("Test Tracks", "test.tracks");
    s.click (s.editor().getChangeButton());
    auto* capture = s.editor().getKeyCapture();
    REQUIRE (capture != nullptr);

    s.press (juce::KeyPress ('+', juce::ModifierKeys::altModifier, '~'));
    CHECK_FALSE (capture->getKey().has_value());
    CHECK_FALSE (capture->getAssignButton().isEnabled());
    CHECK (capture->getShareText().contains ("AltGr types ~ with this key"));

    // Alt with it alone is a key.
    s.press (juce::KeyPress ('+', juce::ModifierKeys::altModifier, '+'));
    CHECK (capture->getKey() == keyOf ("alt + +"));
}