// SPDX-License-Identifier: AGPL-3.0-or-later
// The names Phazite writes into project files (src/model/base/internal/Schema.h). Projects saved before hold them, so a renamed
// one would lose what it held.
#include "TestHelpers.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"

#include <utility>

TEST_CASE ("The names Phazite writes into project files stay the same")
{
    namespace schema = phazite::schema;

    const std::pair<juce::Identifier, const char*> names[] = {
        { schema::PHAZITE, "PHAZITE" },
        { schema::schemaVersion, "schemaVersion" },
        { schema::loopFollowsSong, "loopFollowsSong" },
        { schema::outline, "outline" },
        { schema::gridSnap, "gridSnap" },
        { schema::gridAdaptive, "gridAdaptive" },
        { schema::gridOffset, "gridOffset" },
        { schema::gridStep, "gridStep" },
        { schema::gridTriplets, "gridTriplets" },
        { schema::PIANOROLL, "PIANOROLL" },
        { schema::LASTKEY, "LASTKEY" },
        { schema::phaziteHeight, "phaziteHeight" },
        { schema::phaziteCollapsed, "phaziteCollapsed" },
        { schema::phazitePinned, "phazitePinned" },
        { schema::phaziteKeyRoot, "phaziteKeyRoot" },
        { schema::phaziteKeyScale, "phaziteKeyScale" },
        { schema::phaziteDrumMode, "phaziteDrumMode" },
        { schema::phaziteDrumOrder, "phaziteDrumOrder" },
        { schema::phaziteDrumHidden, "phaziteDrumHidden" },
        { schema::phaziteBus, "phaziteBus" },
        { schema::phaziteRole, "phaziteRole" },
        { schema::phaziteTip, "phaziteTip" },
        { schema::phaziteEnergy, "phaziteEnergy" },
        { schema::phaziteNoteID, "phaziteNoteID" },
        { schema::PHAZITELANE, "PHAZITELANE" },
        { schema::laneDevice, "device" },
        { schema::laneParameter, "parameter" },
        { schema::laneHeight, "height" },
        { schema::laneCollapsed, "collapsed" },
    };

    for (const auto& [name, saved] : names)
        CHECK (name.toString() == juce::String (saved));

    // The loudness meter's plug-in type, which every project's master has.
    CHECK (juce::String (schema::phaziteLoudnessMeter) == juce::String ("phaziteLoudnessMeter"));
    CHECK (juce::String (phazite::LoudnessMeterPlugin::xmlTypeName) == juce::String ("phaziteLoudnessMeter"));

    // The format version projects are saved in now (Migrations.h upgrades older ones).
    CHECK (schema::currentVersion == 6);
}

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
