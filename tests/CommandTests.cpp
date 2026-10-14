// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/Undo.h"
#include "model/commands/UndoCommands.h"

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

// A change to the Edit that goes through its undo manager.
phazite::Command makeEditChange (const juce::String& id, const juce::String& name, te::Edit& edit, int value)
{
    auto command = makeCommand (id, name, [&edit, value] { edit.state.setProperty ("testValue", value, &edit.getUndoManager()); });
    command.undoTarget = [&edit] { return &edit; };
    return command;
}

// A model function that is a change of its own, so it begins its own undo step.
void setValueInOwnStep (te::Edit& edit, int value)
{
    const phazite::UndoStep step (edit, "Set Value");
    edit.state.setProperty ("testValue", value, &edit.getUndoManager());
}
} // namespace

TEST_CASE ("Commands are found, run, and greyed out when disabled")
{
    phazite::CommandRegistry registry;
    int runs = 0;
    bool enabled = false;

    auto command = makeCommand ("test.count", "Count", [&runs] { ++runs; });
    command.isEnabled = [&enabled] { return enabled; };
    const auto commandID = registry.add (std::move (command));

    CHECK (commandID != 0);
    CHECK (registry.getCommandID ("test.count") == commandID);
    REQUIRE (registry.find ("test.count") != nullptr);

    CHECK_FALSE (registry.invoke ("test.count"));
    CHECK (runs == 0);

    enabled = true;
    CHECK (registry.invoke ("test.count"));
    CHECK (registry.getCommandManager().invokeDirectly (commandID, false)); // as a key or menu would
    CHECK (runs == 2);

    CHECK_FALSE (registry.invoke ("test.unknown"));
}

TEST_CASE ("The command palette's search ranks names, synonyms and categories")
{
    phazite::CommandRegistry registry;
    registry.add (makeCommand ("transport.playStop", "Play / Stop"));
    registry.add (makeCommand ("edit.undo", "Undo"));
    registry.add (makeCommand ("edit.redo", "Redo"));

    auto split = makeCommand ("clip.split", "Split Clip");
    split.synonyms = { "cut", "slice" };
    registry.add (std::move (split));

    const auto firstID = [&registry] (const char* query)
    {
        const auto results = registry.search (query);
        return results.isEmpty() ? juce::String() : results.getFirst()->id;
    };

    CHECK (firstID ("pl") == juce::String ("transport.playStop"));
    CHECK (firstID ("undo") == juce::String ("edit.undo"));
    CHECK (firstID ("redo") == juce::String ("edit.redo"));
    CHECK (firstID ("slice") == juce::String ("clip.split")); // a synonym
    CHECK (firstID ("sc") == juce::String ("clip.split"));    // the starts of two words
    CHECK (registry.search ("xyz").isEmpty());
    CHECK (registry.search ("").size() == 4); // an empty search lists everything

    CHECK (phazite::getMatchScore ("pla", "Play / Stop") > phazite::getMatchScore ("pla", "Display All"));
    CHECK (phazite::getMatchScore ("abc", "cab") == 0); // the characters must come in order
}

TEST_CASE ("A command that changes an Edit is one named undo step")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto& undo = edit->getUndoManager();

    // Before the registry, whose undo commands keep a reference to it.
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::CommandRegistry registry;
    registry.add (makeEditChange ("test.one", "Set to One", *edit, 1));
    registry.add (makeEditChange ("test.two", "Set to Two", *edit, 2));

    REQUIRE (registry.invoke ("test.one"));
    CHECK (undo.getUndoDescription() == juce::String ("Set to One"));

    SUBCASE ("A macro runs its commands in order, as one undo step")
    {
        REQUIRE (registry.addMacro ("test.both", "Set Twice", { "test.two", "test.one" }) != 0);
        REQUIRE (registry.invoke ("test.both"));
        CHECK ((int) edit->state["testValue"] == 1);
        CHECK (undo.getUndoDescription() == juce::String ("Set Twice"));

        REQUIRE (undo.undo());
        CHECK ((int) edit->state["testValue"] == 1); // back to before the macro: "Set to One"
        CHECK (undo.getUndoDescription() == juce::String ("Set to One"));
    }

    SUBCASE ("FL Studio's undo-or-redo toggles the last change")
    {
        selection.edit = edit.get();
        phazite::addUndoCommands (registry, selection);

        REQUIRE (registry.invoke ("test.two"));
        REQUIRE (registry.invoke ("edit.undoOrRedo"));
        CHECK ((int) edit->state["testValue"] == 1); // undone
        REQUIRE (registry.invoke ("edit.undoOrRedo"));
        CHECK ((int) edit->state["testValue"] == 2); // redone
        REQUIRE (registry.invoke ("edit.undo"));
        CHECK ((int) edit->state["testValue"] == 1);

        selection.edit = nullptr;
        CHECK_FALSE (registry.invoke ("edit.undo")); // nothing to undo without an Edit
    }

    SUBCASE ("Undo and redo show the name of the step, in the menus too")
    {
        selection.edit = edit.get();
        phazite::addUndoCommands (registry, selection);

        const auto nameOf = [&registry] (const char* id) { return phazite::getCurrentName (*registry.find (id)); };
        CHECK (nameOf ("edit.undo") == juce::String ("Undo Set to One"));

        juce::ApplicationCommandInfo info (registry.getCommandID ("edit.undo"));
        registry.getCommandInfo (info.commandID, info);
        CHECK (info.shortName == juce::String ("Undo Set to One"));

        REQUIRE (registry.invoke ("edit.undo"));
        CHECK (nameOf ("edit.redo") == juce::String ("Redo Set to One"));
        CHECK (nameOf ("edit.undoOrRedo") == juce::String ("Redo Set to One"));

        selection.edit = nullptr;
        CHECK (nameOf ("edit.undo") == juce::String ("Undo"));
    }
}

TEST_CASE ("A model function that begins its own undo step is part of the step of the command that runs it")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto& undo = edit->getUndoManager();
    undo.clearUndoHistory();

    phazite::CommandRegistry registry;
    auto command = makeCommand ("test.setTwice", "Set Twice",
                                [&edit]
                                {
                                    setValueInOwnStep (*edit, 1);
                                    setValueInOwnStep (*edit, 2);
                                });
    command.undoTarget = [&edit] { return edit.get(); };
    registry.add (std::move (command));

    // Run by the command, both changes are the command's one step.
    REQUIRE (registry.invoke ("test.setTwice"));
    CHECK ((int) edit->state["testValue"] == 2);
    CHECK (undo.getUndoDescription() == juce::String ("Set Twice"));

    phazite::undo (*edit);
    CHECK_FALSE (edit->state.hasProperty ("testValue"));
    CHECK_FALSE (undo.canUndo());

    // Called on its own, the function is a step of its own.
    setValueInOwnStep (*edit, 3);
    CHECK (undo.getUndoDescription() == juce::String ("Set Value"));
    phazite::undo (*edit);
    CHECK_FALSE (edit->state.hasProperty ("testValue"));
}

TEST_CASE ("withEdit and ifEdit act only with an Edit")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::SelectionManager selection { phazite::test::getEngine() };

    int runs = 0;
    te::Edit* actedOn = nullptr;
    const auto action = phazite::withEdit (selection,
                                           [&runs, &actedOn] (te::Edit& e)
                                           {
                                               ++runs;
                                               actedOn = &e;
                                           });
    const auto isTrue = phazite::ifEdit (selection, [] (te::Edit&) { return true; });
    const auto isFalse = phazite::ifEdit (selection, [] (te::Edit&) { return false; });

    // Without an Edit, the action does nothing and every test is false.
    action();
    CHECK (runs == 0);
    CHECK_FALSE (isTrue());
    CHECK_FALSE (isFalse());

    // With one, the action runs with it and the tests answer for it.
    selection.edit = edit.get();
    action();
    CHECK (runs == 1);
    CHECK (actedOn == edit.get());
    CHECK (isTrue());
    CHECK_FALSE (isFalse());

    selection.edit = nullptr;
    action();
    CHECK (runs == 1);
    CHECK_FALSE (isTrue());
}

TEST_CASE ("Keymaps load, switch, and keep the user's own changes")
{
    const auto builtIn = phazite::Keymap::getBuiltIn();
    REQUIRE (builtIn.size() == 2);
    REQUIRE (phazite::Keymap::findBuiltIn ("Phazite").has_value());
    const auto flStudio = phazite::Keymap::findBuiltIn ("FL Studio");
    REQUIRE (flStudio.has_value());

    // In FL Studio's keymap, right-click deletes clips and a click on empty space places one, as in FL Studio;
    // otherwise right-click opens a menu and a click selects.
    CHECK (flStudio->rightClickDeletes);
    CHECK (flStudio->clickPlacesClips);
    CHECK_FALSE (phazite::Keymap::findBuiltIn ("Phazite")->rightClickDeletes);
    CHECK_FALSE (phazite::Keymap::findBuiltIn ("Phazite")->clickPlacesClips);

    phazite::CommandRegistry registry;
    registry.add (makeCommand ("transport.playStop", "Play / Stop"));
    registry.add (makeCommand ("edit.undo", "Undo"));
    registry.add (makeCommand ("edit.undoOrRedo", "Undo or Redo"));

    // The default keymap, "Phazite".
    CHECK (registry.getKeymap().name == juce::String ("Phazite"));
    CHECK (registry.getShortcutDescriptions ("transport.playStop").contains (describe ("spacebar")));
    CHECK (registry.getShortcutDescriptions ("edit.undo").contains (describe ("command + Z")));
    CHECK (registry.getShortcutDescriptions ("edit.undoOrRedo").isEmpty());

    // FL Studio's: Ctrl+Z undoes or redoes, Ctrl+Alt+Z undoes.
    registry.setKeymap (*flStudio);
    CHECK (registry.getShortcutDescriptions ("edit.undoOrRedo").contains (describe ("command + Z")));
    CHECK (registry.getShortcutDescriptions ("edit.undo") == juce::StringArray { describe ("command + alt + Z") });

    // A change of the user's own survives saving and loading the settings.
    registry.getCommandManager().getKeyMappings()->addKeyPress (registry.getCommandID ("transport.playStop"),
                                                                juce::KeyPress::createFromDescription ("F2"));

    juce::TemporaryFile settingsFile (".settings");
    juce::PropertiesFile::Options options;
    juce::PropertiesFile settings (settingsFile.getFile(), options);
    registry.saveKeySettings (settings);

    // Changes are kept by command id, so they survive commands being added before them.
    phazite::CommandRegistry reloaded;
    reloaded.add (makeCommand ("test.newer", "A command added in a later version"));
    reloaded.add (makeCommand ("edit.undo", "Undo"));
    reloaded.add (makeCommand ("transport.playStop", "Play / Stop"));
    reloaded.add (makeCommand ("edit.undoOrRedo", "Undo or Redo"));
    reloaded.loadKeySettings (settings);

    CHECK (reloaded.getKeymap().name == juce::String ("FL Studio"));
    const auto playKeys = reloaded.getShortcutDescriptions ("transport.playStop");
    CHECK (playKeys.contains (describe ("spacebar")));
    CHECK (playKeys.contains (describe ("F2")));
    CHECK_FALSE (reloaded.getShortcutDescriptions ("edit.undo").contains (describe ("F2")));
    CHECK (reloaded.getShortcutDescriptions ("test.newer").isEmpty());
}

TEST_CASE ("Shift and a letter run that letter's shortcut, unless they have their own")
{
    int tool = 0, shifted = 0;

    phazite::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.tool"] = { "B" };
    keymap.keys["test.n"] = { "N" };
    keymap.keys["test.shiftN"] = { "shift + N" };

    phazite::CommandRegistry registry;
    registry.setKeymap (keymap);
    registry.add (makeCommand ("test.tool", "Tool", [&tool] { ++tool; }));
    registry.add (makeCommand ("test.n", "N"));
    registry.add (makeCommand ("test.shiftN", "Shift N", [&shifted] { ++shifted; }));

    const auto shift = juce::ModifierKeys (juce::ModifierKeys::shiftModifier);
    CHECK (registry.invokeWithoutShift (juce::KeyPress ('B', shift, 0)));
    CHECK (tool == 1);

    // Shift+N has its own command, which the shortcuts run as usual; this leaves it alone.
    CHECK_FALSE (registry.invokeWithoutShift (juce::KeyPress ('N', shift, 0)));
    CHECK (shifted == 0);

    // Only Shift: not with Ctrl too, and not without it.
    CHECK_FALSE (
        registry.invokeWithoutShift (juce::KeyPress ('B', juce::ModifierKeys::shiftModifier | juce::ModifierKeys::ctrlModifier, 0)));
    CHECK_FALSE (registry.invokeWithoutShift (juce::KeyPress ('B')));
    CHECK_FALSE (registry.invokeWithoutShift (juce::KeyPress ('Q', shift, 0))); // no shortcut at all
    CHECK (tool == 1);
}

TEST_CASE ("When commands share a key, the enabled one with the highest priority runs")
{
    int tracks = 0, notes = 0;
    bool notesSelected = false;

    phazite::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.selectTrack"] = { "cursor up" };
    keymap.keys["test.transpose"] = { "cursor up" };

    phazite::CommandRegistry registry;
    registry.setKeymap (keymap);
    registry.add (makeCommand ("test.selectTrack", "Select Track Above", [&tracks] { ++tracks; }));

    auto transpose = makeCommand ("test.transpose", "Transpose Up", [&notes] { ++notes; });
    transpose.isEnabled = [&notesSelected] { return notesSelected; };
    transpose.keyPriority = phazite::Command::notePriority;
    registry.add (std::move (transpose));

    // Registered later, but it comes first while it is enabled.
    const juce::KeyPress up (juce::KeyPress::upKey);
    CHECK (registry.keyPressed (up, nullptr));
    CHECK (tracks == 1);
    CHECK (notes == 0);

    notesSelected = true;
    CHECK (registry.findCommandForKey (up)->id == juce::String ("test.transpose"));
    CHECK (registry.keyPressed (up, nullptr));
    CHECK (notes == 1);
    CHECK (tracks == 1);

    CHECK_FALSE (registry.keyPressed (juce::KeyPress (juce::KeyPress::downKey), nullptr)); // no command at all
}

TEST_CASE ("A held key runs its command once, unless the command takes a step")
{
    // Holding Space used to start and stop playing over and over as the key repeated.
    int toggles = 0, steps = 0, releases = 0;

    phazite::Keymap keymap;
    keymap.name = "Test";
    keymap.keys["test.toggle"] = { "spacebar" };
    keymap.keys["test.step"] = { "cursor right" };

    phazite::CommandRegistry registry;
    registry.setKeymap (keymap);

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
        CHECK (registry.keyPressed (space, nullptr));
        CHECK (registry.keyPressed (right, nullptr));
    }

    CHECK (toggles == 1);
    CHECK (steps == 3);
    CHECK (releases == 0);

    // Let go (no key is really down while the tests run), and pressed again.
    CHECK_FALSE (registry.keyStateChanged (false, nullptr));
    CHECK (releases == 1);
    CHECK (registry.keyPressed (space, nullptr));
    CHECK (toggles == 2);

    // A window that loses the keyboard lets go of its keys too.
    registry.releaseHeldKeys();
    CHECK (releases == 2);
}

TEST_CASE ("Every key in the built-in keymaps is valid, and used once per keymap, or once more by a note command")
{
    // A note command can share a key with a command of another kind: it takes the key while notes are
    // selected (Command::notePriority).
    for (const auto& keymap : phazite::Keymap::getBuiltIn())
    {
        CAPTURE (keymap.name.toStdString());
        juce::Array<juce::KeyPress> used, usedByNotes;

        for (const auto& entry : keymap.keys)
        {
            auto& keys = entry.first.startsWith ("note.") ? usedByNotes : used;

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
