// SPDX-License-Identifier: AGPL-3.0-or-later
// The metronome and the count-in (src/model/transport/Metronome.h).
#include "TestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/project/Project.h"
#include "model/transport/Metronome.h"

TEST_CASE ("The metronome's and the count-in's commands switch them on and off, without undo steps")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    te::SelectionManager selection (phazite::test::getEngine());
    selection.edit = edit.get();
    phazite::CommandRegistry registry;
    phazite::addMetronomeCommands (registry, selection);
    edit->getUndoManager().clearUndoHistory();

    const auto* metronome = registry.find ("transport.metronome");
    REQUIRE (metronome != nullptr);
    CHECK_FALSE (metronome->isTicked());

    REQUIRE (registry.invoke ("transport.metronome"));
    CHECK (phazite::isMetronomeOn (*edit));
    CHECK (metronome->isTicked());
    CHECK (edit->clickTrackEmphasiseBars.get()); // each bar's first beat louder
    CHECK_FALSE (edit->getUndoManager().canUndo());

    REQUIRE (registry.invoke ("transport.metronome"));
    CHECK_FALSE (phazite::isMetronomeOn (*edit));

    // The count-in is a bar of the time signature, and the app's: every project has the same.
    const auto* countIn = registry.find ("transport.countIn");
    REQUIRE (countIn != nullptr);
    REQUIRE_FALSE (countIn->isTicked());
    REQUIRE (registry.invoke ("transport.countIn"));
    CHECK (countIn->isTicked());
    CHECK (edit->getNumCountInBeats() == 4);

    auto other = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    CHECK (phazite::isCountInOn (*other));

    REQUIRE (registry.invoke ("transport.countIn"));
    CHECK_FALSE (phazite::isCountInOn (*other));
    CHECK_FALSE (edit->getUndoManager().canUndo());
    selection.edit = nullptr;
}

TEST_CASE ("A project keeps its metronome on, as it keeps its loop, though switching it isn't an unsaved change")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Metronome", error);
    REQUIRE (project != nullptr);
    CHECK_FALSE (phazite::isMetronomeOn (project->getEdit()));

    // It is about playing the song, not the song (design question 18).
    phazite::setMetronomeOn (project->getEdit(), true);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());

    const auto file = project->getFile();
    project = nullptr;

    auto reopened = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (reopened != nullptr);
    CHECK (phazite::isMetronomeOn (reopened->getEdit()));
    CHECK_FALSE (reopened->hasUnsavedChanges());
}
