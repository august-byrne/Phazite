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
    CHECK (registry.add (std::move (command)));
    CHECK (registry.indexOf ("test.count") == 0);
    REQUIRE (registry.find ("test.count") != nullptr);

    CHECK_FALSE (registry.invoke ("test.count"));
    CHECK (runs == 0);

    enabled = true;
    CHECK (registry.invoke ("test.count"));
    CHECK (runs == 1);

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
        REQUIRE (registry.addMacro ("test.both", "Set Twice", { "test.two", "test.one" }));
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

    SUBCASE ("Undo and redo show the name of the step")
    {
        selection.edit = edit.get();
        phazite::addUndoCommands (registry, selection);

        const auto nameOf = [&registry] (const char* id) { return phazite::getCurrentName (*registry.find (id)); };
        CHECK (nameOf ("edit.undo") == juce::String ("Undo Set to One"));

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

TEST_CASE ("The registry tells its listeners about each command it adds, in order")
{
    struct Recorder final : phazite::CommandRegistry::Listener
    {
        juce::StringArray ids;
        void commandAdded (const phazite::Command& command) override { ids.add (command.id); }
    };

    phazite::CommandRegistry registry;
    REQUIRE (registry.add (makeCommand ("test.before", "Added before the listener")));

    Recorder recorder;
    registry.addListener (&recorder);
    REQUIRE (registry.add (makeCommand ("test.first", "First")));
    REQUIRE (registry.addMacro ("test.both", "Both", { "test.before", "test.first" }));
    CHECK (recorder.ids == juce::StringArray { "test.first", "test.both" });

    // In the order they were added.
    CHECK (registry.getNumCommands() == 3);
    REQUIRE (registry.getCommand (1) != nullptr);
    CHECK (registry.getCommand (1)->id == juce::String ("test.first"));
    CHECK (registry.indexOf ("test.both") == 2);
    CHECK (registry.indexOf ("test.unknown") == -1);
    CHECK (registry.getCommand (3) == nullptr);
    CHECK (registry.getCommand (-1) == nullptr);

    registry.removeListener (&recorder);
    REQUIRE (registry.add (makeCommand ("test.after", "Added after the listener left")));
    CHECK (recorder.ids.size() == 2);
}
