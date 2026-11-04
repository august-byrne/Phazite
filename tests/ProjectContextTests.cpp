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
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

TEST_CASE ("The open project's song and selection move together")
{
    auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song).front();

    phazite::ProjectContext context (phazite::test::getEngineContext());
    CHECK (findSong (context) == nullptr);

    setSong (context, song.get());
    CHECK (findSong (context) == song.get());
    CHECK (getSelection (context).getSong() == song.get()); // the model's commands find it there

    // Closing the project leaves nothing of it selected.
    getSelection (context).selectOnly (track);
    getRangeSelection (context).set ({ phazite::BeatPosition::fromBeats (0.0), phazite::BeatPosition::fromBeats (4.0) }, { track });
    REQUIRE (getSelection (context).isSelected (track));
    REQUIRE_FALSE (getRangeSelection (context).isEmpty());

    setSong (context, nullptr);
    CHECK (findSong (context) == nullptr);
    CHECK_FALSE (getSelection (context).isSelected (track));
    CHECK (getRangeSelection (context).isEmpty());

    // Opening it again selects nothing; the context keeps the song it holds open.
    setSong (context, song.get());
    CHECK_FALSE (getSelection (context).isSelected (track));
    auto* const held = song.get();
    song = nullptr;
    CHECK (findSong (context) == held);
    setSong (context, nullptr);
}

TEST_CASE ("Views hear when another project opens, or none, before the one that closes goes")
{
    struct View : phazite::ProjectContext::Listener
    {
        explicit View (phazite::ProjectContext& c)
            : context (c)
        {
        }

        void projectChanged() override { seen.push_back (findSong (context)); }

        phazite::ProjectContext& context;
        std::vector<phazite::Song*> seen;
    };

    auto song = phazite::createSong();
    phazite::ProjectContext context (phazite::test::getEngineContext());
    View view (context);
    context.addListener (&view);

    setSong (context, song.get());
    setSong (context, nullptr); // while the song is still there to let go of
    CHECK (view.seen == std::vector<phazite::Song*> { song.get(), nullptr });

    context.removeListener (&view);
    setSong (context, song.get());
    CHECK (view.seen.size() == 2);
    setSong (context, nullptr);
}

TEST_CASE ("The song's selection names tracks, clips and sections by their IDs")
{
    auto song = phazite::createSong(); // 120 BPM, 4/4
    const auto tracks = phazite::ensureTracks (*song, 2);
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setSong (context, song.get());
    auto& selection = context.getSongSelection();

    const auto first = tracks[0], second = tracks[1];
    selection.selectOnly (second);
    selection.add (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { first, second }); // in the playlist's order
    CHECK (getSelection (context).isSelected (first));                                // what the commands act on
    selection.remove (first);
    CHECK (selection.getTracks() == std::vector<phazite::TrackID> { second });

    // Clips by theirs, left to right; a section is no clip.
    const auto later = phazite::insertMidiClip (
        *song, first, { phazite::BeatPosition::fromBeats (8.0), phazite::BeatPosition::fromBeats (12.0) }, nullptr);
    const auto earlier =
        phazite::insertMidiClip (*song, first, { phazite::BeatPosition::fromBeats (0.0), phazite::BeatPosition::fromBeats (4.0) }, nullptr);
    const auto section = phazite::insertSection (*song, {}, { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) });
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
    setSong (context, nullptr);
}

TEST_CASE ("Views hear what changed in the song, in groups, once for the changes made at once")
{
    struct View : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { heard.push_back (changes); }

        std::vector<phazite::SongChanges> heard;
    };

    auto song = phazite::createSong();
    const auto track = phazite::getTrackIDs (*song).front();
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setSong (context, song.get());
    View view;
    context.addListener (&view);

    // The groups of what the view hears, a word each.
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
    phazite::setTrackName (*song, track, "Bass");
    REQUIRE (phazite::insertDeviceState (*song, track, phazite::makeOwnDeviceState (*song, phazite::devices::Reverb::typeID)).isValid());
    phazite::test::dispatchPendingMessages();
    CHECK (view.heard.size() == 1);
    CHECK (hear() == "tracks devices");

    // Devices and tracks that change places.
    const auto delay = phazite::insertDeviceState (*song, track, phazite::makeOwnDeviceState (*song, phazite::devices::Delay::typeID));
    REQUIRE (delay.isValid());
    CHECK (hear() == "devices");
    phazite::moveDevice (*song, delay, 0);
    CHECK (hear() == "devices");
    const auto tracks = phazite::ensureTracks (*song, 2);
    CHECK (hear() == "tracks"); // a new track, with its fader on its element
    phazite::moveTracksAfter (*song, { track }, tracks[1]);
    CHECK (hear() == "tracks");

    // A clip, and its notes: no change to the track.
    const auto clip = phazite::test::addMidiClip (*song, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) });
    REQUIRE (clip.isValid());
    CHECK (hear() == "clips");
    phazite::test::addMidiNote (*song, clip, 60, phazite::BeatPosition(), phazite::BeatDuration::fromBeats (1.0), 100);
    CHECK (hear() == "clips");

    // A section; and the tempo, which leaves the clips on their beats.
    REQUIRE (phazite::insertSection (*song, {}, { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) }).isValid());
    CHECK (juce::String (hear()).contains ("sections"));
    phazite::setTempo (*song, 100.0);
    CHECK (hear() == "tempo");

    // Where the playhead is put is no change to the song; view state, such as how tall a track shows, is a group of its own.
    // Where a device's window was shows in no view: a window moved doesn't make the mixer show its devices again.
    phazite::movePlayhead (*song, phazite::BeatPosition::fromBeats (2.0));
    phazite::setSavedTrackHeight (*song, track, 120);
    CHECK (hear() == "view");
    phazite::rememberWindowPosition (*song, delay, { 200, 120 });
    CHECK (hear().empty());

    // Nor is a parameter's value, which a knob's drag sets at each move: controls read it as they show it. A device
    // switched off is a change.
    phazite::setValueSetByHand (*song, { delay, "feedback" }, 0.7f);
    phazite::setChannelVolume (*song, track, -6.0f);
    CHECK (hear().empty());
    phazite::setDeviceOn (*song, delay, false);
    CHECK (hear() == "devices");

    // Changes not yet told are the closing project's: none come after another opens.
    phazite::setTrackName (*song, track, "Lead");
    setSong (context, nullptr);
    CHECK (hear().empty());
    context.removeListener (&view);
}
