// SPDX-License-Identifier: AGPL-3.0-or-later
// Shortcuts (src/ui/input): the keymaps and the user's changes to them, the bridge between the commands and JUCE's
// command manager, and the keys that run commands.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestKeyboardLayouts.h"
#include "model/commands/CommandRegistry.h"
#include "services/Settings.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/CommandUI.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/KeymapStore.h"

#include <set>

namespace
{
phazite::Command makeCommand (const juce::String& id, const juce::String& name, std::function<void()> perform = [] {})
{
    phazite::Command command;
    command.id = id;
    command.name = name;
    command.category = "Test";
    command.perform = std::move (perform);
    return command;
}

juce::String describe (const char* keyDescription)
{
    return juce::KeyPress::createFromDescription (keyDescription).getTextDescription();
}
} // namespace

TEST_CASE ("Keymaps load, switch, and keep the user's own changes")
{
    const auto builtIn = phazite::ui::Keymap::getBuiltIn();
    REQUIRE (builtIn.size() == 3);
    REQUIRE (phazite::ui::Keymap::findBuiltIn ("Phazite").has_value());
    const auto flStudio = phazite::ui::Keymap::findBuiltIn ("FL Studio");
    REQUIRE (flStudio.has_value());
    const auto live = phazite::ui::Keymap::findBuiltIn ("Ableton Live");
    REQUIRE (live.has_value());

    // In FL Studio's keymap, right-click deletes clips and a click on empty space places one, as in FL Studio;
    // otherwise right-click opens a menu and a click selects, as in Live.
    CHECK (flStudio->rightClickDeletes);
    CHECK (flStudio->clickPlacesClips);
    CHECK_FALSE (phazite::ui::Keymap::findBuiltIn ("Phazite")->rightClickDeletes);
    CHECK_FALSE (phazite::ui::Keymap::findBuiltIn ("Phazite")->clickPlacesClips);
    CHECK_FALSE (live->rightClickDeletes);
    CHECK_FALSE (live->clickPlacesClips);
    CHECK_FALSE (live->selectionLoops);

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    registry.add (makeCommand ("transport.playStop", "Play / Stop"));
    registry.add (makeCommand ("edit.undo", "Undo"));
    registry.add (makeCommand ("edit.undoOrRedo", "Undo or Redo"));

    // The default keymap, "Phazite".
    CHECK (commands.getKeymap().name == juce::String ("Phazite"));
    CHECK (commands.getShortcutDescriptions ("transport.playStop").contains (describe ("spacebar")));
    CHECK (commands.getShortcutDescriptions ("edit.undo").contains (describe ("command + Z")));
    CHECK (commands.getShortcutDescriptions ("edit.undoOrRedo").isEmpty());

    // FL Studio's: Ctrl+Z undoes or redoes, Ctrl+Alt+Z undoes.
    commands.setKeymap (*flStudio);
    CHECK (commands.getShortcutDescriptions ("edit.undoOrRedo").contains (describe ("command + Z")));
    CHECK (commands.getShortcutDescriptions ("edit.undo") == juce::StringArray { describe ("command + alt + Z") });

    // A change of the user's own survives saving and loading the settings.
    commands.getKeyMappings().addKeyPress (commands.getCommandID ("transport.playStop"), juce::KeyPress::createFromDescription ("F2"));

    phazite::Settings settings; // in memory
    phazite::ui::saveKeySettings (commands, settings);
    CHECK (settings.get (phazite::Settings::keymap) == "FL Studio");

    // Kept by command id, in the format earlier versions saved.
    const auto saved = juce::parseXML (settings.getKeyChanges ("FL Studio"));
    REQUIRE (saved != nullptr);
    CHECK (saved->hasTagName ("PHAZITEKEYCHANGES"));
    CHECK (saved->getChildByAttribute ("id", "transport.playStop") != nullptr);

    // Changes are kept by command id, so they survive commands being added before them.
    phazite::CommandRegistry reloadedCommands;
    reloadedCommands.add (makeCommand ("test.newer", "A command added in a later version"));
    reloadedCommands.add (makeCommand ("edit.undo", "Undo"));
    reloadedCommands.add (makeCommand ("transport.playStop", "Play / Stop"));
    reloadedCommands.add (makeCommand ("edit.undoOrRedo", "Undo or Redo"));
    phazite::ui::CommandBridge reloaded { reloadedCommands };
    phazite::ui::loadKeySettings (reloaded, settings);

    CHECK (reloaded.getKeymap().name == juce::String ("FL Studio"));
    const auto playKeys = reloaded.getShortcutDescriptions ("transport.playStop");
    CHECK (playKeys.contains (describe ("spacebar")));
    CHECK (playKeys.contains (describe ("F2")));
    CHECK_FALSE (reloaded.getShortcutDescriptions ("edit.undo").contains (describe ("F2")));
    CHECK (reloaded.getShortcutDescriptions ("test.newer").isEmpty());

    // Each keymap keeps its own changes: Phazite's has none, and FL Studio's has F2 again.
    phazite::ui::switchKeymap (reloaded, "Phazite", settings);
    CHECK (reloaded.getKeymap().name == juce::String ("Phazite"));
    CHECK (settings.get (phazite::Settings::keymap) == "Phazite");
    CHECK (reloaded.getShortcutDescriptions ("transport.playStop") == juce::StringArray { describe ("spacebar") });

    phazite::ui::switchKeymap (reloaded, "FL Studio", settings);
    CHECK (reloaded.getKeymap().name == juce::String ("FL Studio"));
    CHECK (reloaded.getShortcutDescriptions ("transport.playStop").contains (describe ("F2")));
}

TEST_CASE ("Each built-in keymap gives a key to every core action")
{
    // What making a song needs over and over (roadmap step 3.7): playing, recording and the metronome; undo; the clipboard
    // and the edits; opening, saving and exporting; the command search, zooming and the views; the Smart and Draw tools;
    // moving notes; and the computer keyboard.
    const juce::StringArray core { "transport.playStop", "transport.record",   "transport.metronome",
                                   "edit.undo",          "edit.cut",           "edit.copy",
                                   "edit.paste",         "edit.delete",        "edit.duplicate",
                                   "edit.selectAll",     "edit.split",         "edit.quantize",
                                   "edit.rename",        "project.open",       "project.save",
                                   "project.saveAs",     "project.export",     "view.commandPalette",
                                   "view.zoomIn",        "view.zoomOut",       "view.mixer",
                                   "view.pianoRoll",     "tool.smart",         "tool.draw",
                                   "note.transposeUp",   "note.transposeDown", "input.computerKeyboard" };

    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        CAPTURE (keymap.name.toStdString());

        for (const auto& id : core)
        {
            CAPTURE (id.toStdString());
            const auto found = keymap.keys.find (id);
            CHECK ((found != keymap.keys.end() && ! found->second.isEmpty()));
        }

        // Redo: a key of its own, or Undo's, which redoes as FL Studio's Ctrl+Z does.
        CHECK ((keymap.keys.count ("edit.redo") > 0 || keymap.keys.count ("edit.undoOrRedo") > 0));
    }
}

TEST_CASE ("JUCE's command manager has every command, added before or after the bridge, with its name, state and keys")
{
    int runs = 0;
    bool enabled = true, ticked = false;
    juce::String name = "Old Name";

    phazite::CommandRegistry registry;
    auto before = makeCommand ("test.before", "Before", [&runs] { ++runs; });
    before.currentName = [&name] { return name; };
    before.isEnabled = [&enabled] { return enabled; };
    before.isTicked = [&ticked] { return ticked; };
    REQUIRE (registry.add (std::move (before)));

    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.before"] = { "F5" };
    keymap.keys["test.after"] = { "command + F6" };

    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);
    REQUIRE (registry.add (makeCommand ("test.after", "After", [&runs] { runs += 10; })));

    // JUCE's numbers follow the registry's order.
    auto& manager = commands.getCommandManager();
    CHECK (manager.getNumCommands() == 2);
    const auto beforeID = commands.getCommandID ("test.before");
    const auto afterID = commands.getCommandID ("test.after");
    CHECK (beforeID != 0);
    CHECK (afterID == beforeID + 1);
    CHECK (commands.getCommandID ("test.unknown") == 0);

    // The keymap's keys, for the command there before the bridge and the one added after it.
    CHECK (commands.getKeyMappings().findCommandForKeyPress (juce::KeyPress::createFromDescription ("F5")) == beforeID);
    CHECK (commands.getKeyMappings().findCommandForKeyPress (juce::KeyPress::createFromDescription ("command + F6")) == afterID);

    // Menus and JUCE's keys run them.
    CHECK (manager.invokeDirectly (beforeID, false));
    CHECK (manager.invokeDirectly (afterID, false));
    CHECK (runs == 11);

    // Menus show a command's name and state as they are now.
    const auto infoOf = [&commands] (juce::CommandID commandID)
    {
        juce::ApplicationCommandInfo info (commandID);
        commands.getCommandInfo (commandID, info);
        return info;
    };

    const auto first = infoOf (beforeID);
    CHECK (first.shortName == juce::String ("Old Name"));
    CHECK ((first.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
    CHECK ((first.flags & juce::ApplicationCommandInfo::isTicked) == 0);

    name = "New Name";
    enabled = false;
    ticked = true;

    const auto later = infoOf (beforeID);
    CHECK (later.shortName == juce::String ("New Name"));
    CHECK ((later.flags & juce::ApplicationCommandInfo::isDisabled) != 0);
    CHECK ((later.flags & juce::ApplicationCommandInfo::isTicked) != 0);

    CHECK_FALSE (manager.invokeDirectly (beforeID, false)); // greyed out
    CHECK (runs == 11);
}

TEST_CASE ("Shift and a letter run that letter's shortcut, unless they have their own")
{
    int tool = 0, shifted = 0;

    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.tool"] = { "B" };
    keymap.keys["test.n"] = { "N" };
    keymap.keys["test.shiftN"] = { "shift + N" };

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);
    phazite::ui::KeyDispatcher keys { commands, phazite::ui::getUsLayout() };
    registry.add (makeCommand ("test.tool", "Tool", [&tool] { ++tool; }));
    registry.add (makeCommand ("test.n", "N"));
    registry.add (makeCommand ("test.shiftN", "Shift N", [&shifted] { ++shifted; }));

    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    CHECK (keys.invokeWithoutShift (juce::KeyPress ('B', shift, 0)));
    CHECK (tool == 1);

    // Shift+N has its own command, which the shortcuts run as usual; this leaves it alone.
    CHECK_FALSE (keys.invokeWithoutShift (juce::KeyPress ('N', shift, 0)));
    CHECK (shifted == 0);

    // Only Shift: not with Ctrl too, and not without it.
    CHECK_FALSE (keys.invokeWithoutShift (juce::KeyPress ('B', juce::ModifierKeys::shiftModifier | juce::ModifierKeys::ctrlModifier, 0)));
    CHECK_FALSE (keys.invokeWithoutShift (juce::KeyPress ('B')));
    CHECK_FALSE (keys.invokeWithoutShift (juce::KeyPress ('Q', shift, 0))); // no shortcut at all
    CHECK (tool == 1);
}

TEST_CASE ("When commands share a key, the enabled one with the highest priority runs")
{
    int tracks = 0, notes = 0;
    bool notesSelected = false;

    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.selectTrack"] = { "cursor up" };
    keymap.keys["test.transpose"] = { "cursor up" };

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);
    phazite::ui::KeyDispatcher keys { commands, phazite::ui::getUsLayout() };
    registry.add (makeCommand ("test.selectTrack", "Select Track Above", [&tracks] { ++tracks; }));

    auto transpose = makeCommand ("test.transpose", "Transpose Up", [&notes] { ++notes; });
    transpose.isEnabled = [&notesSelected] { return notesSelected; };
    transpose.keyPriority = phazite::Command::notePriority;
    registry.add (std::move (transpose));

    // Registered later, but it comes first while it is enabled.
    const juce::KeyPress up (juce::KeyPress::upKey);
    CHECK (keys.keyPressed (up, nullptr));
    CHECK (tracks == 1);
    CHECK (notes == 0);

    notesSelected = true;
    CHECK (keys.findCommandForKey (up)->id == juce::String ("test.transpose"));
    CHECK (keys.keyPressed (up, nullptr));
    CHECK (notes == 1);
    CHECK (tracks == 1);

    CHECK_FALSE (keys.keyPressed (juce::KeyPress (juce::KeyPress::downKey), nullptr)); // no command at all
}

TEST_CASE ("On other layouts the digit row's keys are its digits, and the zoom keys are the keys that type = and +")
{
    // Known bug 112 until 2026-11-15: AZERTY types the digits with Shift, so the tools' digits and Ctrl+1 to 5 couldn't be
    // reached, and QWERTZ types = with Shift, so Zoom In and its like never matched. The keys are those JUCE gives on
    // Windows (the key code what the layout types at the key without Shift, the text what was typed), and on Linux (the key
    // code what was typed).
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry }; // Phazite's keymap

    for (const auto* id : { "tool.smart", "tool.mute", "tool.select", "tool.delete", "grid.finer", "view.zoomIn", "view.zoomOut",
                            "view.interfaceLarger", "track.taller", "view.zoomToFit", "view.hoverHelp" })
        registry.add (makeCommand (id, id));

    const auto none = juce::ModifierKeys(), shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier),
               ctrl = juce::ModifierKeys (juce::ModifierKeys::commandModifier), alt = juce::ModifierKeys (juce::ModifierKeys::altModifier);
    const auto run = [] (const phazite::ui::KeyDispatcher& keys, juce::KeyPress key)
    {
        const auto* command = keys.findCommandForKey (key);
        return command != nullptr ? command->id : juce::String();
    };

    // AZERTY: the & key is 1, alone and with Ctrl; Shift with it types 1, which is 1 too. What is printed on a key comes
    // first: its - key zooms out, and Shift with it, which types 6, is the Mute tool's.
    const auto azerty = phazite::test::SimulatedLayout::azerty();
    const phazite::ui::KeyDispatcher onAzerty { commands, azerty };
    CHECK (run (onAzerty, juce::KeyPress ('&', none, '&')) == "tool.smart");
    CHECK (run (onAzerty, juce::KeyPress ('&', ctrl, 0)) == "grid.finer");
    CHECK (run (onAzerty, juce::KeyPress ('1', shift, '1')) == "tool.smart");
    CHECK (run (onAzerty, juce::KeyPress ('-', none, '-')) == "view.zoomOut");
    CHECK (run (onAzerty, juce::KeyPress ('6', shift, '6')) == "tool.mute");
    CHECK (run (onAzerty, juce::KeyPress ('=', none, '=')) == "view.zoomIn");

    // QWERTZ: the + key zooms in, and so does Shift+0, which types =; Ctrl and Alt with the + key are Ctrl++ and Alt++.
    const auto qwertz = phazite::test::SimulatedLayout::qwertz();
    const phazite::ui::KeyDispatcher onQwertz { commands, qwertz };
    CHECK (run (onQwertz, juce::KeyPress ('+', none, '+')) == "view.zoomIn");
    CHECK (run (onQwertz, juce::KeyPress ('0', shift, '=')) == "view.zoomIn");
    CHECK (run (onQwertz, juce::KeyPress ('=', shift, '=')) == "view.zoomIn"); // as Linux gives it
    CHECK (run (onQwertz, juce::KeyPress ('+', ctrl, 0)) == "view.interfaceLarger");
    CHECK (run (onQwertz, juce::KeyPress ('+', alt, 0)) == "track.taller");
    CHECK (run (onQwertz, juce::KeyPress (0xdf, none, 0xdf)).isEmpty()); // ß has no command

    // AltGr, which JUCE reports as Alt, types a character of its own with the + key (~): no shortcut runs. Alt with it
    // alone is Alt++. AZERTY's AltGr and = (}) the same.
    CHECK (run (onQwertz, juce::KeyPress ('+', alt, '~')).isEmpty());
    CHECK (run (onQwertz, juce::KeyPress ('+', alt, '+')) == "track.taller");
    CHECK (run (onAzerty, juce::KeyPress ('=', alt, '}')).isEmpty());
    CHECK (run (onAzerty, juce::KeyPress ('=', alt, 0)) == "track.taller");

    // Czech: the digit row's first key is printed + and zooms in; Shift with it types 1, the Smart Tool's.
    auto czech = phazite::test::SimulatedLayout();
    czech.differs = { { '1', { '+', '1' } }, { '2', { 0x11b, '2' } } };
    const phazite::ui::KeyDispatcher onCzech { commands, czech };
    CHECK (run (onCzech, juce::KeyPress ('+', none, '+')) == "view.zoomIn");
    CHECK (run (onCzech, juce::KeyPress ('1', shift, '1')) == "tool.smart");

    // The US layout: = zooms in, and so does Shift with it, which types +.
    const phazite::ui::KeyDispatcher onUs { commands, phazite::ui::getUsLayout() };
    CHECK (run (onUs, juce::KeyPress ('=', none, '=')) == "view.zoomIn");
    CHECK (run (onUs, juce::KeyPress ('=', shift, '+')) == "view.zoomIn");
    CHECK (run (onUs, juce::KeyPress ('+', none, '+')) == "view.zoomIn"); // the number pad's +, as JUCE gives it on Windows
    CHECK (run (onUs, juce::KeyPress ('-', none, '-')) == "view.zoomOut");

    // Linux gives the number pad's * and / as themselves, with no Shift: not the 8 and the 7 that Shift types them on.
    CHECK (run (onUs, juce::KeyPress ('*', none, '*')).isEmpty());
    CHECK (run (onQwertz, juce::KeyPress ('/', none, '/')).isEmpty());

    // FL Studio's Shift+4, as Linux gives it ($), and Ableton Live's Shift+/ for ?, which QWERTZ types with Shift+ß.
    commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("FL Studio"));
    CHECK (run (onUs, juce::KeyPress ('$', shift, '$')) == "view.zoomToFit");
    CHECK (run (onUs, juce::KeyPress ('4', shift, '$')) == "view.zoomToFit");
    commands.setKeymap (*phazite::ui::Keymap::findBuiltIn ("Ableton Live"));
    CHECK (run (onQwertz, juce::KeyPress (0xdf, shift, '?')) == "view.hoverHelp");
    CHECK (run (onUs, juce::KeyPress ('/', shift, '?')) == "view.hoverHelp");
}

TEST_CASE ("The way that finds a key's shortcuts decides among every command, and only those found are asked if they are enabled")
{
    // A key whose command can't run now does nothing, rather than run what it means another way: US Shift+= given to a
    // command that is off doesn't zoom in by the + it types.
    int asked = 0;
    bool canFit = false;
    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["view.zoomIn"] = { "+" };
    keymap.keys["test.fit"] = { "shift + =" };
    keymap.keys["test.other"] = { "F5" };

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);

    for (int i = 0; i < 50; ++i)
    {
        auto command = makeCommand ("test.filler" + juce::String (i), "Filler");
        command.isEnabled = [&asked] { return ++asked > 0; };
        registry.add (std::move (command));
    }

    auto fit = makeCommand ("test.fit", "Fit");
    fit.isEnabled = [&canFit, &asked]
    {
        ++asked;
        return canFit;
    };
    registry.add (std::move (fit));
    registry.add (makeCommand ("view.zoomIn", "Zoom In"));
    registry.add (makeCommand ("test.other", "Other"));

    const phazite::ui::KeyDispatcher keys { commands, phazite::ui::getUsLayout() };
    const juce::KeyPress shiftEquals ('=', juce::ModifierKeys::shiftModifier, '+');
    asked = 0; // the bridge asked each command as it was added, for JUCE's menus
    CHECK (keys.findCommandForKey (shiftEquals) == nullptr);
    CHECK (asked == 1); // the one command found, not the 50 others
    CHECK (keys.findShortcutOf (shiftEquals)->id == "test.fit");
    canFit = true;
    CHECK (keys.findCommandForKey (shiftEquals)->id == "test.fit");

    // A key of no command asks none.
    asked = 0;
    CHECK (keys.findCommandForKey (juce::KeyPress (juce::KeyPress::F6Key)) == nullptr);
    CHECK (asked == 0);
}

TEST_CASE ("Shift with a digit key, where the layout types digits with Shift, is the digit typed with Shift on Windows and Linux alike")
{
    // On AZERTY Windows gives Ctrl, Shift and the & key as & with Ctrl and Shift, Linux as 1 with them: both are Ctrl+Shift+1,
    // which a user's own key for it then gets.
    const auto azerty = phazite::test::SimulatedLayout::azerty();
    const auto ctrlShift = juce::ModifierKeys (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
    const auto expected = juce::KeyPress ('1', ctrlShift, 0);
    CHECK (phazite::ui::getShortcutKey (juce::KeyPress ('&', ctrlShift, 0), azerty) == expected);   // Windows
    CHECK (phazite::ui::getShortcutKey (juce::KeyPress ('1', ctrlShift, '1'), azerty) == expected); // Linux

    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.mine"] = { "command + shift + 1" };
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);
    registry.add (makeCommand ("test.mine", "Mine"));
    const phazite::ui::KeyDispatcher keys { commands, azerty };
    CHECK (keys.findCommandForKey (juce::KeyPress ('&', ctrlShift, 0)) != nullptr);
    CHECK (keys.findCommandForKey (juce::KeyPress ('1', ctrlShift, '1')) != nullptr);
}

TEST_CASE ("AltGr's characters are told apart from Alt with a key, with Shift too and whatever case Windows gives the code in")
{
    const auto qwertz = phazite::test::SimulatedLayout::qwertz();
    const auto alt = juce::ModifierKeys (juce::ModifierKeys::altModifier);
    const auto altShift = juce::ModifierKeys (juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier);

    CHECK_FALSE (phazite::ui::isTypedWithAltGr (juce::KeyPress (0xd6, alt, 0xf6), qwertz)); // Alt and ö, its code Ö
    CHECK_FALSE (phazite::ui::isTypedWithAltGr (juce::KeyPress ('+', alt, '+'), qwertz));
    CHECK_FALSE (phazite::ui::isTypedWithAltGr (juce::KeyPress ('+', altShift, '*'), qwertz)); // Alt, Shift and the + key
    CHECK (phazite::ui::isTypedWithAltGr (juce::KeyPress ('+', alt, '~'), qwertz));
    CHECK (phazite::ui::isTypedWithAltGr (juce::KeyPress ('+', altShift, 0xaf), qwertz)); // AltGr, Shift and the + key: ¯
}

TEST_CASE ("Shift and a letter whose own command can't run now don't run the letter's")
{
    int letter = 0;
    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.n"] = { "N" };
    keymap.keys["test.shiftN"] = { "shift + N" };

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);
    phazite::ui::KeyDispatcher keys { commands, phazite::ui::getUsLayout() };
    registry.add (makeCommand ("test.n", "N", [&letter] { ++letter; }));
    auto shifted = makeCommand ("test.shiftN", "Shift N");
    shifted.isEnabled = [] { return false; };
    registry.add (std::move (shifted));

    CHECK_FALSE (keys.invokeWithoutShift (juce::KeyPress ('N', juce::ModifierKeys::shiftModifier, 'N')));
    CHECK (letter == 0);
}

TEST_CASE ("A key that is + itself is written as one")
{
    CHECK (juce::KeyPress::createFromDescription ("+") == juce::KeyPress ('+'));
    CHECK (juce::KeyPress::createFromDescription ("command + +") == juce::KeyPress ('+', juce::ModifierKeys::commandModifier, 0));
    CHECK (phazite::ui::formatKey (juce::KeyPress ('+', juce::ModifierKeys::commandModifier, 0).getTextDescription()) == "Ctrl++");
    CHECK (phazite::ui::formatKey ("+") == "+");
    CHECK (phazite::ui::formatKey ("ctrl + shift + R") == "Ctrl+Shift+R");
    CHECK (phazite::ui::formatKey ("numpad +") == "Numpad +");
}

TEST_CASE ("A held key runs its command once, unless the command takes a step")
{
    // Holding Space used to start and stop playing over and over as the key repeated.
    int toggles = 0, steps = 0, releases = 0;

    phazite::ui::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.toggle"] = { "spacebar" };
    keymap.keys["test.step"] = { "cursor right" };

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    commands.setKeymap (keymap);
    phazite::ui::KeyDispatcher keys { commands, phazite::ui::getUsLayout() };

    auto toggle = makeCommand ("test.toggle", "Toggle", [&toggles] { ++toggles; });
    toggle.keyReleased = [&releases] { ++releases; };
    registry.add (std::move (toggle));

    auto step = makeCommand ("test.step", "Step", [&steps] { ++steps; });
    step.repeatsWhileHeld = true;
    registry.add (std::move (step));

    // A held key repeats: the same key again before it is let go. The repeat is used up either way.
    const juce::KeyPress space (juce::KeyPress::spaceKey), right (juce::KeyPress::rightKey);

    for (int i = 0; i < 3; ++i)
    {
        CHECK (keys.keyPressed (space, nullptr));
        CHECK (keys.keyPressed (right, nullptr));
    }

    CHECK (toggles == 1);
    CHECK (steps == 3);
    CHECK (releases == 0);

    // Let go (no key is really down while the tests run), and pressed again.
    CHECK_FALSE (keys.keyStateChanged (false, nullptr));
    CHECK (releases == 1);
    CHECK (keys.keyPressed (space, nullptr));
    CHECK (toggles == 2);

    // A window that loses the keyboard lets go of its keys too.
    keys.releaseHeldKeys();
    CHECK (releases == 2);
}

TEST_CASE ("Every key in the built-in keymaps is valid, and used once per keymap, or once more by a command for selected "
           "notes, one for selected automation points and one for selected devices")
{
    // A command for selected notes, automation points or devices can share a key with a command of another kind: it takes
    // the key while its notes, points or devices are selected (Command::notePriority), and those are never selected
    // together.
    const auto isPointCommand = [] (const juce::String& id)
    { return id.startsWith ("automation.raise") || id.startsWith ("automation.lower"); };
    const auto isDeviceCommand = [] (const juce::String& id) { return id == "device.group" || id == "device.ungroup"; };

    // Escape closes a transform being previewed, and otherwise deselects the notes (note.cancelTransform comes first), as
    // Escape backs out of one thing at a time.
    const auto isDeselectCommand = [] (const juce::String& id) { return id == "note.deselectAll"; };

    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        CAPTURE (keymap.name.toStdString());
        juce::Array<juce::KeyPress> used, usedByNotes, usedByPoints, usedByDevices, usedByDeselecting;

        for (const auto& entry : keymap.keys)
        {
            auto& keys = isDeselectCommand (entry.first)    ? usedByDeselecting
                         : entry.first.startsWith ("note.") ? usedByNotes
                         : isPointCommand (entry.first)     ? usedByPoints
                         : isDeviceCommand (entry.first)    ? usedByDevices
                                                            : used;

            for (const auto& description : entry.second)
            {
                CAPTURE (description.toStdString());
                const auto key = juce::KeyPress::createFromDescription (description);
                CHECK (key.isValid());
                CHECK_FALSE (keys.contains (key));
                keys.add (key);
            }
        }
    }

    // Number-pad keys are read as such, not as the character they type.
    CHECK (juce::KeyPress::createFromDescription ("command + numpad +")
           == juce::KeyPress (juce::KeyPress::numberPadAdd, juce::ModifierKeys::commandModifier, 0));
}

namespace
{
// How JUCE gives key presses: on Windows a key's code is what is printed on it (a digit typed stays the digit), its text
// what it types without Ctrl or Alt, a dead key gives no press without Ctrl or Alt, and the number pad's digits are
// numberPad0 to 9 (docs/juce-workarounds.md, W47); on Linux the code is what the key types, Shift included (with Ctrl,
// the keysym at that level), a dead key gives no press at all, and the number pad's digits are the digits (W48).
enum class System
{
    onWindows,
    onLinux
};

// The key presses a layout can make: each key of the main block (and the ISO key) with no modifier, Shift, Ctrl, Alt and
// their mixes, Ctrl and Alt being AltGr's (left Ctrl and left Alt type AltGr's character where the layout has one, and
// give no press where it is a dead key); and the keys every layout has (Space, the arrows, the F keys, the number pad,
// whose + - * / are those characters).
std::vector<juce::KeyPress> getKeyPressesOf (const phazite::test::SimulatedLayout& layout, System system)
{
    using Mods = juce::ModifierKeys;
    const int combos[] = { 0,
                           Mods::shiftModifier,
                           Mods::ctrlModifier,
                           Mods::ctrlModifier | Mods::shiftModifier,
                           Mods::altModifier,
                           Mods::altModifier | Mods::shiftModifier,
                           Mods::ctrlModifier | Mods::altModifier,
                           Mods::ctrlModifier | Mods::altModifier | Mods::shiftModifier };
    std::vector<juce::KeyPress> presses;

    for (const auto place : juce::String ("1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./<"))
    {
        const auto p = (char) place;
        const auto plain = layout.getCharacter (p, false), shifted = layout.getCharacter (p, true);

        if (plain == 0)
            continue;

        for (const auto flags : combos)
        {
            const Mods mods (flags);
            const auto shift = mods.isShiftDown();
            const auto ctrl = mods.isCtrlDown(), alt = mods.isAltDown();
            const auto altGr = system == System::onWindows && ctrl && alt && ! shift; // Linux has an AltGr key of its own
            const auto deadAtLevel = shift ? layout.deadShifted.count (p) > 0 : layout.dead.count (p) > 0;
            const auto level = shift && shifted != 0 ? shifted : plain;

            // A dead key gives Windows no press without Ctrl or Alt, nor with AltGr where AltGr's level is dead too; Linux
            // none at all.
            if (altGr ? layout.deadAltGr.count (p) > 0 : deadAtLevel && (system == System::onLinux || (! ctrl && ! alt)))
                continue;

            if (system == System::onWindows)
            {
                const auto text = altGr ? layout.getAltGrCharacter (p) : ! ctrl && ! alt ? level : (juce::juce_wchar) 0;
                const auto code = juce::CharacterFunctions::isDigit (text) && ! altGr ? text : plain;
                presses.emplace_back ((int) code, mods, text);
            }
            else
            {
                presses.emplace_back ((int) level, mods, ctrl ? (juce::juce_wchar) 0 : level);
            }
        }
    }

    std::vector<int> everywhere { juce::KeyPress::spaceKey,
                                  juce::KeyPress::returnKey,
                                  juce::KeyPress::escapeKey,
                                  juce::KeyPress::backspaceKey,
                                  juce::KeyPress::deleteKey,
                                  juce::KeyPress::insertKey,
                                  juce::KeyPress::homeKey,
                                  juce::KeyPress::endKey,
                                  juce::KeyPress::pageUpKey,
                                  juce::KeyPress::pageDownKey,
                                  juce::KeyPress::leftKey,
                                  juce::KeyPress::rightKey,
                                  juce::KeyPress::upKey,
                                  juce::KeyPress::downKey,
                                  '+',
                                  '-',
                                  '*',
                                  '/' };

    for (int f = 0; f < 12; ++f)
        everywhere.push_back (juce::KeyPress::F1Key + f);

    for (int n = 0; n < 10; ++n)
        everywhere.push_back (system == System::onWindows ? juce::KeyPress::numberPad0 + n : '0' + n);

    for (const auto code : everywhere)
    {
        for (const auto flags : combos)
        {
            const Mods mods (flags);
            const auto typesItself = code == ' ' || (code >= '*' && code <= '9');
            presses.emplace_back (code, mods, typesItself && ! mods.isCtrlDown() && ! mods.isAltDown() ? (juce::juce_wchar) code : 0);
        }
    }

    return presses;
}

// A key press as the golden files write it: its modifiers and key, and what it types.
juce::String describePress (const juce::KeyPress& press)
{
    const auto mods = press.getModifiers();
    const auto code = press.getKeyCode();
    juce::String text;

    if (mods.isCtrlDown())
        text << "ctrl+";

    if (mods.isAltDown())
        text << "alt+";

    if (mods.isShiftDown())
        text << "shift+";

    text << (code > ' ' && code < 0x7f ? juce::String::charToString ((juce::juce_wchar) code) : juce::KeyPress (code).getTextDescription());

    if (const auto typed = press.getTextCharacter(); typed > ' ' && typed < 0x7f)
        text << " '" << juce::String::charToString (typed) << "'";
    else if (typed > 0x7f)
        text << " '#" << juce::String::toHexString ((int) typed) << "'";

    return text;
}

struct NamedLayout
{
    const char* name;
    phazite::test::SimulatedLayout layout;
};

std::vector<NamedLayout> getLayoutStandIns()
{
    return { { "US", {} },
             { "AZERTY", phazite::test::SimulatedLayout::azerty() },
             { "QWERTZ", phazite::test::SimulatedLayout::qwertz() },
             { "Czech", phazite::test::SimulatedLayout::czech() },
             { "Spanish", phazite::test::SimulatedLayout::spanish() },
             { "Swedish", phazite::test::SimulatedLayout::swedish() } };
}
} // namespace

TEST_CASE ("On every layout stand-in, every command a keymap gives a key has a key press that runs it")
{
    // What the layouts' keys can't reach, by keymap and layout, with why: a key that layout doesn't have.
    struct Unreachable
    {
        const char* keymap;
        const char* layout;
        const char* command;
        const char* why;
    };

    static const std::vector<Unreachable> known {
        { "Ableton Live", "Spanish", "view.instrument",
          "Ctrl+Alt+4: AltGr and 4 is a dead key (~), which gives no key press, and Shift with it is another key (Ctrl+Alt+Shift+4)" },
    };

    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        phazite::CommandRegistry registry;
        phazite::ui::CommandBridge commands { registry };
        commands.setKeymap (keymap);

        for (const auto& entry : keymap.keys)
            registry.add (makeCommand (entry.first, entry.first));

        for (const auto& named : getLayoutStandIns())
        {
            for (const auto system : { System::onWindows, System::onLinux })
            {
                const phazite::ui::KeyDispatcher keys { commands, named.layout };
                std::set<juce::String> reached;

                for (const auto& press : getKeyPressesOf (named.layout, system))
                    for (const auto& match : keys.findMatches (press))
                        reached.insert (match.command->id);

                juce::StringArray unreachable;

                for (const auto& entry : keymap.keys)
                {
                    const auto isKnown = std::any_of (
                        known.begin(), known.end(), [&] (const Unreachable& u)
                        { return keymap.name == u.keymap && juce::String (named.name) == u.layout && entry.first == u.command; });

                    if (! entry.second.isEmpty() && reached.count (entry.first) == 0 && ! isKnown)
                        unreachable.add (entry.first + " (" + entry.second.joinIntoString (", ") + ")");
                }

                CAPTURE (keymap.name.toStdString());
                CAPTURE (std::string (named.name));
                CAPTURE (std::string (system == System::onWindows ? "Windows" : "Linux"));
                INFO (unreachable.joinIntoString ("; ").toStdString());
                CHECK (unreachable.isEmpty());
            }
        }
    }
}

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden keys: what every key press of the US layout runs, in each keymap, as Windows and Linux give the presses")
    {
        // A change that makes a key of the US layout run another command, or none, changes these files: it must be meant.
        for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
        {
            phazite::CommandRegistry registry;
            phazite::ui::CommandBridge commands { registry };
            commands.setKeymap (keymap);

            for (const auto& entry : keymap.keys)
                registry.add (makeCommand (entry.first, entry.first));

            const phazite::test::SimulatedLayout us;
            const phazite::ui::KeyDispatcher keys { commands, us };
            juce::StringArray lines { "# Every key press of the US layout that runs a command in the " + keymap.name
                                      + " keymap: the press (its modifiers, its key code, what it types), the way it matched "
                                        "(as it is, as its digit, by what it types) and the commands of that way. Regenerate "
                                        "with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md)." };

            for (const auto system : { System::onWindows, System::onLinux })
            {
                lines.add (system == System::onWindows ? "[Windows]" : "[Linux]");

                for (const auto& press : getKeyPressesOf (us, system))
                {
                    const auto matches = keys.findMatches (press);

                    if (matches.empty())
                        continue;

                    juce::StringArray ids;

                    for (const auto& match : matches)
                        ids.add (match.command->id);

                    const auto way = matches[0].way == phazite::ui::KeyDispatcher::Way::asItIs    ? "as it is"
                                     : matches[0].way == phazite::ui::KeyDispatcher::Way::asDigit ? "as its digit"
                                                                                                  : "by what it types";
                    lines.add (describePress (press) + " | " + way + " | " + ids.joinIntoString (", "));
                }
            }

            const auto file = "keys/us-" + keymap.name.toLowerCase().replace (" ", "-") + ".txt";
            phazite::test::checkGoldenText (file, lines.joinIntoString ("\n") + "\n",
                                            [] (const juce::String& expected, const juce::String& actual)
                                            { return phazite::test::compareLines (expected, actual, 40); });
        }
    }
}