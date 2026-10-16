// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project as commands and panels find it (src/model/project/ProjectContext.h).
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

TEST_CASE ("The open project's Edit, selection and Tracktion's focus move together")
{
    auto& engine = phazite::test::getEngine();
    auto& focus = phazite::getUIBehaviour (engine);
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto* track = te::getAudioTracks (*edit)[0];

    {
        phazite::ProjectContext context (phazite::test::getEngineContext(), &focus);
        CHECK (getEdit (context) == nullptr);
        CHECK (focus.getCurrentlyFocusedSelectionManager() == &getSelection (context));

        setEdit (context, edit.get());
        CHECK (getEdit (context) == edit.get());
        CHECK (getSelection (context).getEdit() == edit.get()); // the model's commands find it there
        CHECK (focus.getLastFocusedEdit() == edit.get());

        // Closing the project leaves nothing of it selected, and Tracktion's focus with it.
        getSelection (context).selectOnly (track);
        getRangeSelection (context).set ({ te::BeatPosition::fromBeats (0.0), te::BeatPosition::fromBeats (4.0) }, { track });
        REQUIRE (getSelection (context).isSelected (track));
        REQUIRE_FALSE (getRangeSelection (context).isEmpty());

        setEdit (context, nullptr);
        CHECK (getEdit (context) == nullptr);
        CHECK_FALSE (getSelection (context).isSelected (track));
        CHECK (getRangeSelection (context).isEmpty());
        CHECK (focus.getLastFocusedEdit() == nullptr);

        // Opening it again selects nothing; an Edit that is deleted is gone from the context too.
        setEdit (context, edit.get());
        CHECK_FALSE (getSelection (context).isSelected (track));
        edit = nullptr;
        CHECK (getEdit (context) == nullptr);
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

        void projectChanged() override { seen.push_back (getEdit (context)); }

        phazite::ProjectContext& context;
        std::vector<te::Edit*> seen;
    };

    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine);
    phazite::ProjectContext context (phazite::test::getEngineContext());
    View view (context);
    context.addListener (&view);

    setEdit (context, edit.get());
    setEdit (context, nullptr); // while the Edit is still there to let go of
    CHECK (view.seen == std::vector<te::Edit*> { edit.get(), nullptr });

    context.removeListener (&view);
    setEdit (context, edit.get());
    CHECK (view.seen.size() == 2);
    setEdit (context, nullptr);
}

TEST_CASE ("The song's selection names tracks, clips and sections by their IDs")
{
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine); // 120 BPM, 4/4
    edit->ensureNumberOfAudioTracks (2);
    const auto tracks = te::getAudioTracks (*edit);
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setEdit (context, edit.get());
    auto& selection = context.getSongSelection();

    const auto first = phazite::getTrackID (*tracks[0]), second = phazite::getTrackID (*tracks[1]);
    selection.selectOnly (second);
    selection.add (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { first, second }); // in the playlist's order
    CHECK (getSelection (context).isSelected (tracks[0]));                            // what the commands act on
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
    setEdit (context, nullptr);
}

TEST_CASE ("Views hear what changed in the song, in groups, once for the changes made at once")
{
    struct View : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { heard.push_back (changes); }

        std::vector<phazite::SongChanges> heard;
    };

    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine);
    auto* track = te::getAudioTracks (*edit)[0];
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setEdit (context, edit.get());
    View view;
    context.addListener (&view);

    // The groups of what the view hears, a word each. (Tracktion follows some changes up with its own, a moment later.)
    const auto hear = [&view]
    {
        phazite::test::dispatchPendingMessages();
        phazite::SongChanges all;

        for (const auto& changes : std::exchange (view.heard, {}))
        {
            all.tracks = all.tracks || changes.tracks;
            all.devices = all.devices || changes.devices;
            all.clips = all.clips || changes.clips;
            all.sections = all.sections || changes.sections;
            all.tempo = all.tempo || changes.tempo;
            all.view = all.view || changes.view;
        }

        juce::StringArray groups;

        for (const auto& [group, name] :
             { std::pair (all.tracks, "tracks"), std::pair (all.devices, "devices"), std::pair (all.clips, "clips"),
               std::pair (all.sections, "sections"), std::pair (all.tempo, "tempo"), std::pair (all.view, "view") })
            if (group)
                groups.add (name);

        return groups.joinIntoString (" ").toStdString();
    };

    // A track renamed and a device put on it at once: one notification of both.
    track->setName ("Bass");
    track->pluginList.insertPlugin (edit->getPluginCache().createNewPlugin (te::ReverbPlugin::xmlTypeName, {}), 0, nullptr);
    phazite::test::dispatchPendingMessages();
    CHECK (view.heard.size() == 1);
    CHECK (hear() == "tracks devices");

    // Devices and tracks that change places.
    const auto delay = edit->getPluginCache().createNewPlugin (te::DelayPlugin::xmlTypeName, {});
    REQUIRE (delay != nullptr);
    track->pluginList.insertPlugin (delay, 0, nullptr);
    CHECK (hear() == "devices");
    track->state.moveChild (track->state.indexOf (delay->state), track->state.getNumChildren() - 1, nullptr);
    CHECK (hear() == "devices");
    edit->ensureNumberOfAudioTracks (2);
    CHECK (hear() == "tracks devices"); // a new track comes with its fader and meter
    edit->state.moveChild (edit->state.indexOf (track->state), edit->state.indexOf (te::getAudioTracks (*edit)[1]->state), nullptr);
    CHECK (hear() == "tracks");

    // A clip, and its notes: no change to the track (Tracktion sorts the track's clips and parts after a clip is added).
    auto* clip = track->insertMIDIClip ({ te::TimePosition(), te::TimePosition::fromSeconds (2.0) }, nullptr).get();
    REQUIRE (clip != nullptr);
    CHECK (hear() == "clips");
    clip->getSequence().addNote (60, te::BeatPosition(), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);
    CHECK (hear() == "clips");

    // A section; and the tempo, which moves the clips and sections in time.
    REQUIRE (phazite::insertSection (*edit, {}, { .name = "Verse", .length = te::BeatDuration::fromBeats (16.0) }) != nullptr);
    CHECK (juce::String (hear()).contains ("sections"));
    edit->tempoSequence.getTempo (0)->setBpm (100.0);
    CHECK (hear() == "clips sections tempo");

    // What Tracktion writes as it plays is no change to the song; view state, such as how tall a track shows, is a group of
    // its own.
    edit->getTransport().setPosition (te::TimePosition::fromSeconds (1.0));
    phazite::setSavedTrackHeight (*track, 120);
    CHECK (hear() == "view");

    // Changes not yet told are the closing project's: none come after another opens.
    track->setName ("Lead");
    setEdit (context, nullptr);
    CHECK (hear().empty());
    context.removeListener (&view);
}
