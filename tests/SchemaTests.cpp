// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's state in a project file (src/model/base/internal/Schema.h).
#include "TestHelpers.h"
#include "model/base/internal/Schema.h"

TEST_CASE ("Reading Phazite's state doesn't add it")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine()); // Tracktion's, not a song's (Project.h: createSongEdit())
    const auto numChildren = edit->state.getNumChildren();

    // A new Edit has none. Finding it adds nothing (adding counts as an unsaved change): the result is invalid, and
    // reads from it give the defaults.
    const auto found = phazite::findPhaziteState (*edit);
    CHECK_FALSE (found.isValid());
    CHECK ((bool) found.getProperty (phazite::schema::loopFollowsSong, true));
    CHECK (edit->state.getNumChildren() == numChildren);
    CHECK_FALSE (edit->state.getChildWithName (phazite::schema::PHAZITE).isValid());

    // Getting it adds it once, without an undo step: the same element every time, which finding it then returns.
    const auto state = phazite::getSong (*edit).getState();
    REQUIRE (state.isValid());
    CHECK (state.hasType (phazite::schema::PHAZITE));
    CHECK (state.getParent() == edit->state);
    CHECK (phazite::getSong (*edit).getState() == state);
    CHECK (phazite::findPhaziteState (*edit) == state);
    CHECK (edit->state.getNumChildren() == numChildren + 1);
    CHECK_FALSE (phazite::getSong (*edit).getUndoManager().canUndo()); // which getting the song (Song.h) makes with it
    CHECK (&phazite::getSong (*edit).getUndoManager() == &phazite::getSong (*edit).getUndoManager());
}

TEST_CASE ("A song numbers its items from its own count, past Tracktion's while that mirrors it, and tells the mirror what changed")
{
    phazite::Song song { juce::ValueTree (phazite::schema::PHAZITE) };
    CHECK (song.makeID() == 1);
    CHECK (song.makeID() == 2);
    CHECK ((juce::int64) song.getState()[phazite::schema::nextID] == 3);

    // Tracktion's count behind the song's is taken past it; ahead, the song's moves on to it.
    std::uint64_t tracktions = 0;
    song.setTracktionIDs ([&tracktions] { return ++tracktions; });
    CHECK (song.makeID() == 3);
    tracktions = 10;
    CHECK (song.makeID() == 11);
    CHECK (song.makeID() == 12);

    // Without a mirror, reconciling does nothing; with one, it hears which part changed.
    song.reconcile (phazite::Song::Part::tempoMap);
    std::vector<phazite::Song::Part> told;
    song.setMirror ([&told] (phazite::Song::Part part) { told.push_back (part); });
    song.reconcile (phazite::Song::Part::tracks);
    CHECK (told == std::vector { phazite::Song::Part::tracks });
}
