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
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

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

TEST_CASE ("Views hear modulators and macros added, removed and modulating as their group, their values as no change, and a track "
           "that goes and comes back with one as its tracks' and their group")
{
    // The modulators are heard from the song's values (SongValues.h: Listener::modulationChanged()). A value set by hand, the
    // first one too, and a macro turned are no change that views show, as the tree's callbacks took a parameter's value; a
    // track that goes or comes with a modulator tells it taken out or added, so its group comes beside the tracks'.
    struct View : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { heard.push_back (changes); }

        std::vector<phazite::SongChanges> heard;
    };

    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto delay = phazite::insertDeviceState (*song, tracks[0], phazite::makeOwnDeviceState (*song, phazite::devices::Delay::typeID));
    REQUIRE (delay.isValid());
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setSong (context, song.get());
    View view;
    context.addListener (&view);

    // The groups of what the view hears of an edit made as one step (an Undo: none), a word each.
    const auto hear = [&view, &song] (const juce::String& name, const std::function<void()>& change)
    {
        phazite::test::dispatchPendingMessages();
        view.heard.clear();

        if (name.isEmpty())
        {
            change();
        }
        else
        {
            const phazite::UndoStep step (*song, name);
            change();
        }

        phazite::test::dispatchPendingMessages();
        phazite::SongChanges all;

        for (const auto& changes : std::exchange (view.heard, {}))
        {
            all.tracks = all.tracks || changes.tracks;
            all.devices = all.devices || changes.devices;
            all.clips = all.clips || changes.clips;
            all.automation = all.automation || changes.automation;
            all.modulation = all.modulation || changes.modulation;
            all.view = all.view || changes.view;
        }

        juce::StringArray groups;

        for (const auto& [group, word] :
             { std::pair (all.tracks, "tracks"), std::pair (all.devices, "devices"), std::pair (all.clips, "clips"),
               std::pair (all.automation, "automation"), std::pair (all.modulation, "modulation"), std::pair (all.view, "view") })
            if (group)
                groups.add (word);

        return groups.joinIntoString (" ").toStdString();
    };

    const auto ref = [] (phazite::ModulatorID modulator, const juce::String& parameter)
    { return phazite::ParameterRef { phazite::DeviceID::fromRaw (modulator.getRaw()), parameter }; };

    phazite::ModulatorID lfo, macro;
    CHECK (hear ("Add LFO", [&] { lfo = phazite::addModulator (*song, tracks[0], phazite::ModulatorType::lfo); }) == "modulation");
    CHECK (hear ("Add Macro", [&] { macro = phazite::addModulator (*song, tracks[0], phazite::ModulatorType::macro); }) == "modulation");
    REQUIRE (lfo.isValid());
    REQUIRE (macro.isValid());

    // Its values: none, the first one set by hand too, as a parameter's value.
    CHECK (hear ("Change Rate", [&] { phazite::setValueSetByHand (*song, ref (lfo, "rate"), 2.0f); }).empty());
    CHECK (hear ("Change Rate", [&] { phazite::setValueSetByHand (*song, ref (lfo, "rate"), 3.0f); }).empty());
    CHECK (hear ("Change Macro", [&] { phazite::setValueSetByHand (*song, ref (macro, juce::String (macro.getRaw())), 0.5f); }).empty());

    // A modulation made, and a modulator removed.
    CHECK (hear ("Modulate Feedback", [&] { phazite::setModulation (*song, macro, { delay, "feedback" }, 0.5f); }) == "modulation");
    CHECK (hear ("Remove LFO", [&] { phazite::removeModulator (*song, lfo); }) == "modulation");

    // A track with a modulator removed, and brought back by its Undo: the tracks', and the modulators' it took and brought.
    REQUIRE (phazite::addModulator (*song, tracks[1], phazite::ModulatorType::random).isValid());
    CHECK (hear ("Delete Track", [&] { phazite::deleteTracks (*song, { tracks[1] }, nullptr); }) == "tracks modulation");
    CHECK (hear ({}, [&] { phazite::undo (*song); }) == "tracks modulation");

    setSong (context, nullptr);
    context.removeListener (&view);
}

TEST_CASE ("Views hear where the changes of clips and devices were: the clips, their tracks, the channels")
{
    struct View : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { heard.push_back (changes); }

        std::vector<phazite::SongChanges> heard;
    };

    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto reverb = phazite::addReturnTrack (*song, "Reverb", nullptr);
    REQUIRE (reverb.isValid());
    phazite::ProjectContext context (phazite::test::getEngineContext());
    setSong (context, song.get());
    View view;
    context.addListener (&view);

    // What the view heard of one turn of the message loop.
    const auto take = [&view]
    {
        phazite::test::dispatchPendingMessages();
        REQUIRE (view.heard.size() == 1);
        return std::exchange (view.heard, {}).front();
    };

    using Tracks = std::vector<phazite::TrackID>;
    const auto sorted = [] (Tracks ids)
    {
        std::sort (ids.begin(), ids.end());
        return ids;
    };

    // A clip moved to another track: the clip, and both tracks, each once however many changes the move made.
    const auto clip = phazite::test::addMidiClip (*song, tracks[0], { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) });
    phazite::test::dispatchPendingMessages();
    view.heard.clear();
    phazite::moveClip (*song, clip, phazite::BeatPosition::fromBeats (8.0), tracks[2]);
    auto changes = take();
    CHECK (changes.clips);
    CHECK_FALSE (changes.changedClips.any);
    CHECK (changes.changedClips.named == std::vector<phazite::ClipID> { clip });
    CHECK_FALSE (changes.clipTracks.any);
    CHECK (changes.clipTracks.named == sorted ({ tracks[0], tracks[2] }));

    // A send: its track, and the return it goes to; and, as it changes where sound can go (every strip's send knobs follow
    // it), any channel.
    const auto send = phazite::addSend (*song, tracks[1], reverb, false);
    REQUIRE (send.isValid());
    changes = take();
    CHECK (changes.devices);
    CHECK (changes.deviceChannels.any);
    CHECK (changes.deviceChannels.named == sorted ({ tracks[1], reverb }));

    // Switched before the fader: where sound goes is the same; its track and its return alone.
    phazite::setPreFader (*song, send, true);
    changes = take();
    CHECK_FALSE (changes.deviceChannels.any);
    CHECK (changes.deviceChannels.named == sorted ({ tracks[1], reverb }));

    // An effect added: its track alone.
    REQUIRE (phazite::insertDeviceState (*song, tracks[2], phazite::makeOwnDeviceState (*song, phazite::devices::Delay::typeID)).isValid());
    changes = take();
    CHECK_FALSE (changes.deviceChannels.any);
    CHECK (changes.deviceChannels.named == Tracks { tracks[2] });

    // A plug-in's helper gone, or reloaded: the plug-in host doesn't say whose, so any channel.
    phazite::getPluginHost (phazite::test::getEngineContext()).sendSynchronousChangeMessage();
    changes = take();
    CHECK (changes.devices);
    CHECK (changes.deviceChannels.any);

    setSong (context, nullptr);
    context.removeListener (&view);
}

TEST_CASE ("Views hear a note edit, its Undo and its Redo as a change of its clip, every alias of it and their tracks, and of nothing "
           "else")
{
    // ProjectContext.h, SongChanges: clips "added, changed (their notes included)", with "the clips added, changed ... and the
    // tracks they were on". A lone clip on the third track; a clip on the first with an alias on the second.
    struct View : phazite::ProjectContext::Listener
    {
        void projectChanged() override {}
        void songChanged (const phazite::SongChanges& changes) override { heard.push_back (changes); }

        std::vector<phazite::SongChanges> heard;
    };

    using phazite::test::at;
    using phazite::test::lasting;
    using Clips = std::vector<phazite::ClipID>;
    using Tracks = std::vector<phazite::TrackID>;

    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto lone = phazite::test::addMidiClip (*song, tracks[2], { at (0.0), at (4.0) });
    const auto clip = phazite::test::addMidiClip (*song, tracks[0], { at (0.0), at (4.0) });
    const auto aliases = phazite::copyClips (*song, { clip }, true);
    REQUIRE (aliases.size() == 1);
    const auto alias = aliases.front();
    phazite::moveClip (*song, alias, at (8.0), tracks[1]);
    const auto note = phazite::test::addMidiNote (*song, clip, 60, at (0.0), lasting (1.0), 100);
    const auto loneNote = phazite::test::addMidiNote (*song, lone, 60, at (0.0), lasting (1.0), 100);
    REQUIRE (note.isValid());
    REQUIRE (loneNote.isValid());
    song->getUndoManager().clearUndoHistory();

    phazite::ProjectContext context (phazite::test::getEngineContext());
    setSong (context, song.get());
    View view;
    context.addListener (&view);
    phazite::test::dispatchPendingMessages();
    view.heard.clear();

    const auto sortedClips = [] (Clips ids)
    {
        std::sort (ids.begin(), ids.end());
        return ids;
    };

    const auto sortedTracks = [] (Tracks ids)
    {
        std::sort (ids.begin(), ids.end());
        return ids;
    };

    // What the view hears of one turn of the message loop: the clips' group alone, naming these clips and their tracks.
    const auto hears = [&] (const juce::String& when, const Clips& clips, const Tracks& clipTracks)
    {
        INFO (when.toStdString());
        phazite::test::dispatchPendingMessages();
        REQUIRE (view.heard.size() == 1);
        const auto changes = std::exchange (view.heard, {}).front();
        CHECK (changes.clips);
        CHECK_FALSE (changes.tracks);
        CHECK_FALSE (changes.devices);
        CHECK_FALSE (changes.automation);
        CHECK_FALSE (changes.modulation);
        CHECK_FALSE (changes.sections);
        CHECK_FALSE (changes.chords);
        CHECK_FALSE (changes.tempo);
        CHECK_FALSE (changes.view);
        CHECK_FALSE (changes.notes);
        CHECK_FALSE (changes.midiLinks);
        CHECK_FALSE (changes.changedClips.any);
        CHECK (sortedClips (changes.changedClips.named) == sortedClips (clips));
        CHECK_FALSE (changes.clipTracks.any);
        CHECK (sortedTracks (changes.clipTracks.named) == sortedTracks (clipTracks));
        CHECK_FALSE (changes.deviceChannels.any);
        CHECK (changes.deviceChannels.named.empty());
    };

    // An edit as one undo step, heard; then its Undo and its Redo, each heard the same.
    const auto checkEdit = [&] (const juce::String& name, const std::function<void()>& edit, const Clips& clips, const Tracks& clipTracks)
    {
        {
            const phazite::UndoStep step (*song, name);
            edit();
        }

        hears (name, clips, clipTracks);
        phazite::undo (*song);
        hears ("Undo " + name, clips, clipTracks);
        phazite::redo (*song);
        hears ("Redo " + name, clips, clipTracks);
    };

    checkEdit ("Add Note", [&] { REQUIRE (phazite::addNote (*song, lone, 62, at (1.0), lasting (1.0), 100).isValid()); }, { lone },
               { tracks[2] });
    checkEdit ("Move Note", [&] { phazite::moveNote (*song, loneNote, at (2.0), 64); }, { lone }, { tracks[2] });
    checkEdit ("Add Control", [&] { phazite::addControl (*song, lone, at (1.0), phazite::ControlKind::controller, 1, 64, 1); }, { lone },
               { tracks[2] });
    checkEdit ("Delete Note", [&] { phazite::deleteNotes (*song, { loneNote }); }, { lone }, { tracks[2] });

    // In a clip with an alias, through either: both clips and both tracks.
    checkEdit ("Move Note", [&] { phazite::moveNote (*song, { alias, note.note }, at (9.0), 62); }, { clip, alias },
               { tracks[0], tracks[1] });
    checkEdit ("Add Note", [&] { REQUIRE (phazite::addNote (*song, clip, 67, at (2.0), lasting (1.0), 100).isValid()); }, { clip, alias },
               { tracks[0], tracks[1] });
    checkEdit ("Add Control", [&] { phazite::addControl (*song, alias, at (9.5), phazite::ControlKind::controller, 1, 64, 1); },
               { clip, alias }, { tracks[0], tracks[1] });
    checkEdit ("Delete Note", [&] { phazite::deleteNotes (*song, { note }); }, { clip, alias }, { tracks[0], tracks[1] });

    setSong (context, nullptr);
    context.removeListener (&view);
}
