// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project as commands and panels find it (src/model/project/ProjectContext.h).
#include "TestClips.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/reverb/Reverb.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

TEST_CASE ("The open project's Edit, selection and Tracktion's focus move together")
{
    auto& engine = phazite::test::getEngine();
    auto& focus = phazite::getUIBehaviour (engine);
    auto edit = phazite::createSongEdit (engine);
    auto* track = te::getAudioTracks (*edit)[0];

    {
        phazite::ProjectContext context (phazite::test::getEngineContext(), &focus);
        CHECK (getEdit (context) == nullptr);
        CHECK (focus.getCurrentlyFocusedSelectionManager() == nullptr); // Phazite keeps what is selected itself

        setEdit (context, edit.get());
        CHECK (getEdit (context) == edit.get());
        CHECK (getSelection (context).getEdit() == edit.get()); // the model's commands find it there
        CHECK (focus.getLastFocusedEdit() == edit.get());

        // Closing the project leaves nothing of it selected, and Tracktion's focus with it.
        getSelection (context).selectOnly (phazite::getTrackID (*track));
        getRangeSelection (context).set ({ phazite::BeatPosition::fromBeats (0.0), phazite::BeatPosition::fromBeats (4.0) },
                                         { phazite::getTrackID (*track) });
        REQUIRE (getSelection (context).isSelected (phazite::getTrackID (*track)));
        REQUIRE_FALSE (getRangeSelection (context).isEmpty());

        setEdit (context, nullptr);
        CHECK (getEdit (context) == nullptr);
        CHECK_FALSE (getSelection (context).isSelected (phazite::getTrackID (*track)));
        CHECK (getRangeSelection (context).isEmpty());
        CHECK (focus.getLastFocusedEdit() == nullptr);

        // Opening it again selects nothing; an Edit that is deleted is gone from the context too.
        setEdit (context, edit.get());
        CHECK_FALSE (getSelection (context).isSelected (phazite::getTrackID (*track)));
        edit = nullptr;
        CHECK (getEdit (context) == nullptr);
    }

    // Gone with the context.
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
    auto edit = phazite::createSongEdit (engine);
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
    auto edit = phazite::createSongEdit (engine); // 120 BPM, 4/4
    phazite::ensureTracks (*edit, 2);
    const auto tracks = te::getAudioTracks (*edit);
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setEdit (context, edit.get());
    auto& selection = context.getSongSelection();

    const auto first = phazite::getTrackID (*tracks[0]), second = phazite::getTrackID (*tracks[1]);
    selection.selectOnly (second);
    selection.add (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { first, second }); // in the playlist's order
    CHECK (getSelection (context).isSelected (phazite::getTrackID (*tracks[0])));     // what the commands act on
    selection.remove (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { second });

    // Clips by theirs, left to right; a section is no clip.
    const auto later = phazite::insertMidiClip (
        phazite::getSong (*edit), first, { phazite::BeatPosition::fromBeats (8.0), phazite::BeatPosition::fromBeats (12.0) }, nullptr);
    const auto earlier = phazite::insertMidiClip (
        phazite::getSong (*edit), first, { phazite::BeatPosition::fromBeats (0.0), phazite::BeatPosition::fromBeats (4.0) }, nullptr);
    const auto section =
        phazite::insertSection (phazite::getSong (*edit), {}, { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) });
    REQUIRE (later.isValid());
    REQUIRE (earlier.isValid());
    REQUIRE (section.isValid());
    selection.selectOnly (later);
    selection.add (earlier);
    CHECK (selection.getClips() == std::vector<phazite::ClipID> { earlier, later });
    CHECK (selection.isSelected (later));
    CHECK_FALSE (selection.isSelected (second));
    CHECK_FALSE (selection.isSelected (phazite::ClipID::fromRaw (section.getRaw())));

    selection.selectOnly (section);
    CHECK (selection.getSections() == std::vector<phazite::SectionID> { section });
    CHECK (selection.getClips().empty());

    // An ID of nothing, or of something gone, does nothing.
    selection.selectOnly (phazite::TrackID());
    CHECK (selection.isSelected (section));
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
    auto edit = phazite::createSongEdit (engine);
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
    phazite::setTrackName (*track, "Bass");
    phazite::insertDevice (*track, edit->getPluginCache().createNewPlugin (phazite::devices::Reverb::typeID, {}));
    phazite::test::dispatchPendingMessages();
    CHECK (view.heard.size() == 1);
    CHECK (hear() == "tracks devices");

    // Devices and tracks that change places.
    const auto delay = edit->getPluginCache().createNewPlugin (phazite::devices::Delay::typeID, {});
    REQUIRE (delay != nullptr);
    phazite::insertDevice (*track, delay);
    CHECK (hear() == "devices");
    phazite::moveDevice (*delay, 0);
    CHECK (hear() == "devices");
    phazite::ensureTracks (*edit, 2);
    CHECK (hear() == "tracks devices"); // a new track comes with its fader and meter
    edit->state.moveChild (edit->state.indexOf (track->state), edit->state.indexOf (te::getAudioTracks (*edit)[1]->state), nullptr);
    CHECK (hear() == "tracks");

    // A clip, and its notes: no change to the track (Tracktion sorts the track's clips and parts after a clip is added).
    const auto clip = phazite::test::addMidiClip (phazite::getSong (*edit), phazite::getTrackID (*track),
                                                  { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) });
    REQUIRE (clip.isValid());
    CHECK (hear() == "clips");
    phazite::test::addMidiNote (phazite::getSong (*edit), clip, 60, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (1.0), 100);
    CHECK (hear() == "clips");

    // A section; and the tempo, which moves the clips in time (Tracktion keeps them in seconds; sections are in beats).
    REQUIRE (phazite::insertSection (phazite::getSong (*edit), {}, { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) })
                 .isValid());
    CHECK (juce::String (hear()).contains ("sections"));
    phazite::setTempo (phazite::getSong (*edit), 100.0);
    CHECK (hear() == "clips tempo");

    // What Tracktion writes as it plays is no change to the song; view state, such as how tall a track shows, is a group of
    // its own. Where a device's window was shows in no view: a window moved doesn't make the mixer show its devices again.
    edit->getTransport().setPosition (te::TimePosition::fromSeconds (1.0));
    phazite::setSavedTrackHeight (*track, 120);
    CHECK (hear() == "view");
    phazite::rememberWindowPosition (*delay, { 200, 120 });
    CHECK (hear().empty());

    // Nor is a parameter's value, which a knob's drag sets at each move: controls read it as they show it. A device
    // switched off is a change.
    phazite::setDeviceValue (*delay->getAutomatableParameterByID ("feedback"), 0.7f);
    phazite::setChannelVolume (*track, -6.0f);
    CHECK (hear().empty());
    phazite::setDeviceOn (*delay, false);
    CHECK (hear() == "devices");

    // Changes not yet told are the closing project's: none come after another opens.
    phazite::setTrackName (*track, "Lead");
    setEdit (context, nullptr);
    CHECK (hear().empty());
    context.removeListener (&view);
}
