// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's state in a project file (src/model/base/internal/Schema.h).
#include "TestHelpers.h"
#include "model/base/internal/Schema.h"

TEST_CASE ("Reading Phazite's state doesn't add it")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->getUndoManager().clearUndoHistory();
    const auto numChildren = edit->state.getNumChildren();

    // A new Edit has none. Finding it adds nothing (adding counts as an unsaved change): the result is invalid, and
    // reads from it give the defaults.
    const auto found = phazite::findPhaziteState (*edit);
    CHECK_FALSE (found.isValid());
    CHECK ((bool) found.getProperty (phazite::schema::loopFollowsSong, true));
    CHECK (edit->state.getNumChildren() == numChildren);
    CHECK_FALSE (edit->state.getChildWithName (phazite::schema::PHAZITE).isValid());

    // Getting it adds it once, without an undo step: the same element every time, which finding it then returns.
    const auto state = phazite::getPhaziteState (*edit);
    REQUIRE (state.isValid());
    CHECK (state.hasType (phazite::schema::PHAZITE));
    CHECK (state.getParent() == edit->state);
    CHECK (phazite::getPhaziteState (*edit) == state);
    CHECK (phazite::findPhaziteState (*edit) == state);
    CHECK (edit->state.getNumChildren() == numChildren + 1);
    CHECK_FALSE (edit->getUndoManager().canUndo());
}
