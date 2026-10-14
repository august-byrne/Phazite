// SPDX-License-Identifier: AGPL-3.0-or-later
// Undo, redo and the undo convention (src/model/commands/Undo.h): a change of its own begins a named UndoStep, which
// joins a step already in progress for the same Edit.
#include "TestHelpers.h"
#include "model/commands/Undo.h"

namespace
{
std::unique_ptr<te::Edit> createEdit()
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->getUndoManager().clearUndoHistory();
    return edit;
}

// A change that goes through the Edit's undo manager.
void change (te::Edit& edit, const char* property, int value)
{
    edit.state.setProperty (property, value, &edit.getUndoManager());
}
} // namespace

TEST_CASE ("An undo step begun inside another for the same Edit joins it")
{
    auto edit = createEdit();
    auto& undo = edit->getUndoManager();

    SUBCASE ("Inside another for the same Edit: one step, named after the outer one")
    {
        {
            const phazite::UndoStep outer (*edit, "Outer");
            change (*edit, "a", 1);

            {
                const phazite::UndoStep inner (*edit, "Inner");
                change (*edit, "b", 2);
            }

            change (*edit, "c", 3);
        }

        CHECK (undo.getUndoDescription() == juce::String ("Outer"));
        CHECK (undo.getNumActionsInCurrentTransaction() == 3);

        // One undo reverts all of it.
        phazite::undo (*edit);
        CHECK_FALSE (edit->state.hasProperty ("a"));
        CHECK_FALSE (edit->state.hasProperty ("b"));
        CHECK_FALSE (edit->state.hasProperty ("c"));
        CHECK_FALSE (undo.canUndo());
    }

    SUBCASE ("One after the other: two named steps")
    {
        {
            const phazite::UndoStep first (*edit, "First");
            change (*edit, "a", 1);
        }

        {
            const phazite::UndoStep second (*edit, "Second");
            change (*edit, "b", 2);
        }

        CHECK (undo.getUndoDescription() == juce::String ("Second"));
        phazite::undo (*edit);
        CHECK (edit->state.hasProperty ("a"));
        CHECK_FALSE (edit->state.hasProperty ("b"));
        CHECK (undo.getUndoDescription() == juce::String ("First"));
        phazite::undo (*edit);
        CHECK_FALSE (edit->state.hasProperty ("a"));
        CHECK_FALSE (undo.canUndo());
    }

    SUBCASE ("Inside a step for another Edit: a step of its own")
    {
        auto other = createEdit();

        {
            const phazite::UndoStep outer (*edit, "Outer");
            change (*edit, "a", 1);

            {
                const phazite::UndoStep inner (*other, "Inner");
                change (*other, "b", 2);

                // Inside that one again, a step for the first Edit still joins the outer one.
                const phazite::UndoStep innermost (*edit, "Innermost");
                change (*edit, "c", 3);
            }
        }

        CHECK (undo.getUndoDescription() == juce::String ("Outer"));
        CHECK (undo.getNumActionsInCurrentTransaction() == 2);
        CHECK (other->getUndoManager().getUndoDescription() == juce::String ("Inner"));

        phazite::undo (*other);
        CHECK_FALSE (other->state.hasProperty ("b"));
        CHECK (edit->state.hasProperty ("a")); // the other Edit's undo leaves this one alone
        CHECK (edit->state.hasProperty ("c"));
    }
}

TEST_CASE ("Undo and redo say what they would undo")
{
    auto edit = createEdit();

    // Nothing to undo or redo: the bare verbs.
    CHECK_FALSE (phazite::canUndo (*edit));
    CHECK_FALSE (phazite::canRedo (*edit));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (edit.get()) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (edit.get()) == juce::String ("Undo"));

    {
        const phazite::UndoStep step (*edit, "Set Value");
        change (*edit, "testValue", 1);
    }

    CHECK (phazite::canUndo (*edit));
    CHECK_FALSE (phazite::canRedo (*edit));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Set Value"));
    CHECK (phazite::describeRedo (edit.get()) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (edit.get()) == juce::String ("Undo Set Value"));

    // FL Studio's undo-or-redo undoes, then redoes what it undid.
    phazite::undoOrRedo (*edit);
    CHECK_FALSE (edit->state.hasProperty ("testValue"));
    CHECK_FALSE (phazite::canUndo (*edit));
    CHECK (phazite::canRedo (*edit));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (edit.get()) == juce::String ("Redo Set Value"));
    CHECK (phazite::describeUndoOrRedo (edit.get()) == juce::String ("Redo Set Value"));

    phazite::undoOrRedo (*edit);
    CHECK ((int) edit->state["testValue"] == 1);
    CHECK (phazite::describeUndoOrRedo (edit.get()) == juce::String ("Undo Set Value"));

    // Without an Edit.
    CHECK (phazite::describeUndo (nullptr) == juce::String ("Undo"));
    CHECK (phazite::describeRedo (nullptr) == juce::String ("Redo"));
    CHECK (phazite::describeUndoOrRedo (nullptr) == juce::String ("Undo"));

    // An unnamed step, such as the one Tracktion begins after a pause: the bare verb.
    edit->getUndoManager().beginNewTransaction();
    change (*edit, "testValue", 2);
    CHECK (phazite::canUndo (*edit));
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo"));
    CHECK (phazite::describeUndoOrRedo (edit.get()) == juce::String ("Undo"));
}
