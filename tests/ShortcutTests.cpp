// SPDX-License-Identifier: AGPL-3.0-or-later
// Shortcuts (src/ui/input): the keymaps and the user's changes to them, the bridge between the commands and JUCE's
// command manager, and the keys that run commands.
#include "TestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "services/Settings.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/input/KeymapStore.h"

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
    phazite::ui::KeyDispatcher keys { commands };
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
    phazite::ui::KeyDispatcher keys { commands };
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
    phazite::ui::KeyDispatcher keys { commands };

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

    for (const auto& keymap : phazite::ui::Keymap::getBuiltIn())
    {
        CAPTURE (keymap.name.toStdString());
        juce::Array<juce::KeyPress> used, usedByNotes, usedByPoints, usedByDevices;

        for (const auto& entry : keymap.keys)
        {
            auto& keys = entry.first.startsWith ("note.") ? usedByNotes
                         : isPointCommand (entry.first)   ? usedByPoints
                         : isDeviceCommand (entry.first)  ? usedByDevices
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
