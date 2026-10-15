// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project as commands and panels find it (src/model/project/ProjectContext.h).
#include "TestHelpers.h"
#include "model/engine/EngineSetup.h"
#include "model/project/ProjectContext.h"

TEST_CASE ("The open project's Edit, selection and Tracktion's focus move together")
{
    auto& engine = phazite::test::getEngine();
    auto& focus = phazite::getUIBehaviour (engine);
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto* track = te::getAudioTracks (*edit)[0];

    {
        phazite::ProjectContext context (engine, &focus);
        CHECK (context.getEdit() == nullptr);
        CHECK (focus.getCurrentlyFocusedSelectionManager() == &context.getSelection());

        context.setEdit (edit.get());
        CHECK (context.getEdit() == edit.get());
        CHECK (context.getSelection().getEdit() == edit.get()); // the model's commands find it there
        CHECK (focus.getLastFocusedEdit() == edit.get());

        // Closing the project leaves nothing of it selected, and Tracktion's focus with it.
        context.getSelection().selectOnly (track);
        context.getRangeSelection().set ({ te::BeatPosition::fromBeats (0.0), te::BeatPosition::fromBeats (4.0) }, { track });
        REQUIRE (context.getSelection().isSelected (track));
        REQUIRE_FALSE (context.getRangeSelection().isEmpty());

        context.setEdit (nullptr);
        CHECK (context.getEdit() == nullptr);
        CHECK_FALSE (context.getSelection().isSelected (track));
        CHECK (context.getRangeSelection().isEmpty());
        CHECK (focus.getLastFocusedEdit() == nullptr);

        // Opening it again selects nothing; an Edit that is deleted is gone from the context too.
        context.setEdit (edit.get());
        CHECK_FALSE (context.getSelection().isSelected (track));
        edit = nullptr;
        CHECK (context.getEdit() == nullptr);
    }

    // Gone with the context: Tracktion is left with no selection of Phazite's.
    CHECK (focus.getCurrentlyFocusedSelectionManager() == nullptr);
    CHECK (focus.getLastFocusedEdit() == nullptr);
}
