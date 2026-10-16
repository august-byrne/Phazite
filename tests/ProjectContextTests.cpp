// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project as commands and panels find it (src/model/project/ProjectContext.h).
#include "TestHelpers.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Sections.h"
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

TEST_CASE ("Views hear when another project opens, or none, before the one that closes goes")
{
    struct View : phazite::ProjectContext::Listener
    {
        explicit View (phazite::ProjectContext& c)
            : context (c)
        {
        }

        void projectChanged() override { seen.push_back (context.getEdit()); }

        phazite::ProjectContext& context;
        std::vector<te::Edit*> seen;
    };

    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine);
    phazite::ProjectContext context (engine);
    View view (context);
    context.addListener (&view);

    context.setEdit (edit.get());
    context.setEdit (nullptr); // while the Edit is still there to let go of
    CHECK (view.seen == std::vector<te::Edit*> { edit.get(), nullptr });

    context.removeListener (&view);
    context.setEdit (edit.get());
    CHECK (view.seen.size() == 2);
    context.setEdit (nullptr);
}

TEST_CASE ("The song's selection names tracks, clips and sections by their IDs")
{
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine); // 120 BPM, 4/4
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);
    phazite::ProjectContext context (engine);
    context.setEdit (edit.get());
    auto& selection = context.getSongSelection();

    const auto first = phazite::getTrackID (*tracks[0]), second = phazite::getTrackID (*tracks[1]);
    selection.selectOnly (second);
    selection.add (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { first, second }); // in the playlist's order
    CHECK (context.getSelection().isSelected (tracks[0]));                            // what the commands act on
    selection.remove (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { second });

    // Clips by theirs, left to right; a section is no clip.
    auto* later = phazite::insertMidiClip (*tracks[0], { te::BeatPosition::fromBeats (8.0), te::BeatPosition::fromBeats (12.0) }, nullptr);
    auto* earlier = phazite::insertMidiClip (*tracks[0], { te::BeatPosition::fromBeats (0.0), te::BeatPosition::fromBeats (4.0) }, nullptr);
    auto* section = phazite::insertSection (*edit, {}, { .name = "Verse", .length = te::BeatDuration::fromBeats (16.0) });
    REQUIRE (later != nullptr);
    REQUIRE (earlier != nullptr);
    REQUIRE (section != nullptr);
    selection.selectOnly (phazite::getClipID (*later));
    selection.add (phazite::getClipID (*earlier));
    CHECK (selection.getClips() == std::vector<phazite::ClipID> { phazite::getClipID (*earlier), phazite::getClipID (*later) });
    CHECK (selection.isSelected (phazite::getClipID (*later)));
    CHECK_FALSE (selection.isSelected (second));
    CHECK_FALSE (selection.isSelected (phazite::ClipID::fromRaw (phazite::getSectionID (*section).getRaw())));

    selection.selectOnly (phazite::getSectionID (*section));
    CHECK (selection.getSections() == std::vector<phazite::SectionID> { phazite::getSectionID (*section) });
    CHECK (selection.getClips().empty());

    // An ID of nothing, or of something gone, does nothing.
    selection.selectOnly (phazite::TrackID());
    CHECK (selection.isSelected (phazite::getSectionID (*section)));
    selection.clear();
    CHECK (selection.getSections().empty());
    context.setEdit (nullptr);
}
