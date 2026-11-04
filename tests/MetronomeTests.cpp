// SPDX-License-Identifier: AGPL-3.0-or-later
// The metronome and the count-in (src/model/transport/internal/Metronome.h).
#include "TestHelpers.h"
#include "model/base/internal/Selection.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/internal/Project.h"
#include "model/transport/internal/Metronome.h"

TEST_CASE ("The metronome's and the count-in's commands switch them on and off, without undo steps")
{
    auto song = phazite::createSong();
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    phazite::Selection selection;
    selection.setSong (song.get());
    phazite::CommandRegistry registry;
    phazite::addMetronomeCommands (registry, selection, audio);
    song->getUndoManager().clearUndoHistory();

    const auto* metronome = registry.find ("transport.metronome");
    REQUIRE (metronome != nullptr);
    CHECK_FALSE (metronome->isTicked());

    REQUIRE (registry.invoke ("transport.metronome"));
    CHECK (phazite::isMetronomeOn (*song));
    CHECK (metronome->isTicked());
    CHECK_FALSE (song->getUndoManager().canUndo());

    REQUIRE (registry.invoke ("transport.metronome"));
    CHECK_FALSE (phazite::isMetronomeOn (*song));

    // The count-in is the app's: every project has the same, and the song doesn't change.
    const auto* countIn = registry.find ("transport.countIn");
    REQUIRE (countIn != nullptr);
    REQUIRE_FALSE (countIn->isTicked());
    REQUIRE (registry.invoke ("transport.countIn"));
    CHECK (countIn->isTicked());
    CHECK (phazite::isCountInOn (audio));

    auto other = phazite::createSong();
    phazite::Selection otherSelection;
    otherSelection.setSong (other.get());
    phazite::CommandRegistry otherRegistry;
    phazite::addMetronomeCommands (otherRegistry, otherSelection, audio);
    CHECK (otherRegistry.find ("transport.countIn")->isTicked());

    REQUIRE (registry.invoke ("transport.countIn"));
    CHECK_FALSE (phazite::isCountInOn (audio));
    CHECK_FALSE (otherRegistry.find ("transport.countIn")->isTicked());
    CHECK_FALSE (song->getUndoManager().canUndo());
    selection.setSong (nullptr);
    otherSelection.setSong (nullptr);
}

TEST_CASE ("A project keeps its metronome on, as it keeps its loop, though switching it isn't an unsaved change")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Metronome", error);
    REQUIRE (project != nullptr);
    CHECK_FALSE (phazite::isMetronomeOn (project->getSong()));

    // It is about playing the song, not the song (design question 18).
    phazite::setMetronomeOn (project->getSong(), true);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());

    const auto file = project->getFile();
    project = nullptr;

    auto reopened = phazite::Project::open (file, false, error);
    REQUIRE (reopened != nullptr);
    CHECK (phazite::isMetronomeOn (reopened->getSong()));
    CHECK_FALSE (reopened->hasUnsavedChanges());
}
