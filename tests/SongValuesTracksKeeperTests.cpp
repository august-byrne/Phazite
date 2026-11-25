// SPDX-License-Identifier: AGPL-3.0-or-later
// The tracks are values of the song (roadmap step S5.20, the tracks' switch: model/base/internal/SongValues.h, tracks;
// TracksArea.h). What takes, moves, copies or renders a track is one exact undo step, its Undo and Redo putting every area
// back with it (UndoCheck.h); a drag rolled back and a trial put back leave the tracks as they were; and what plays follows
// a folder moved.
//   phazite_tests.exe --test-case="Song values' tracks' keeper*"
#include "EnginePlayback.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/fm/FmSynth.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/DeviceType.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"

#include <functional>
#include <optional>
#include <vector>

namespace
{
namespace schema = phazite::schema;
using phazite::TrackID;

// A song with a track that holds a clip, a synth, an effect, a modulator and a send to a return, beside a folder with another.
struct Held
{
    phazite::test::TestSong s;
    phazite::Song& song = *phazite::findSong (s.context);
    TrackID track, other, folder, returnTrack;
    phazite::ClipID clip;
    phazite::ModulatorID lfo;

    Held()
    {
        track = s.firstTrack;
        s.setInstrument (track, phazite::devices::Synth::typeID);
        clip = s.addMidiClip (track, 0.0, 4.0);
        s.addNote (clip, 60, 0.0, 1.0);
        s.addEffect (track, "phaziteDelay");
        lfo = s.addModulator (track, phazite::ModulatorType::lfo);
        returnTrack = s.addReturn (track, "phaziteReverb", -6.0f);
        other = s.addTrack ("Other");
        folder = s.addBus ("Folder", { other });
        phazite::toggleTracksMuted (song, { other });
        phazite::setSavedTrackHeight (song, track, 96);
        song.getUndoManager().clearUndoHistory();
    }
};
} // namespace

TEST_CASE ("Song values' tracks' keeper: a track moved into a folder and back, a folder ungrouped, a folder of two duplicated, a "
           "track with what it holds deleted, each one exact undo step")
{
    Held h;
    auto& song = h.song;
    phazite::test::UndoCheck undo (h.s.context);

    undo.check ("Into Folder",
                [&]
                {
                    const phazite::UndoStep step (song, "Into Folder");
                    phazite::moveTrackIntoFolder (song, h.track, h.folder);
                });
    CHECK (phazite::getParentFolder (song, h.track) == h.folder);
    CHECK (phazite::getClips (song, h.track).size() == 1);

    undo.check ("Out of Folder", [&] { phazite::moveTracksAfter (song, { h.track }, h.folder); });
    CHECK_FALSE (phazite::getParentFolder (song, h.track).isValid());

    // A folder of two duplicated, with what each holds.
    undo.check ("Into Folder Again",
                [&]
                {
                    const phazite::UndoStep step (song, "Into Folder Again");
                    phazite::moveTrackIntoFolder (song, h.track, h.folder);
                });
    std::vector<TrackID> copies;
    undo.check ("Duplicate Folder",
                [&]
                {
                    const phazite::UndoStep step (song, "Duplicate Folder");
                    copies = phazite::duplicateTracks (song, { h.folder }, nullptr);
                });
    REQUIRE (copies.size() == 1);
    CHECK (phazite::getTracksInFolder (song, copies.front()).size() == 2);

    // Ungrouped: its tracks where it was, the folder gone with its bus's holdings.
    undo.check ("Ungroup",
                [&]
                {
                    const phazite::UndoStep step (song, "Ungroup");
                    phazite::ungroupFolders (song, { h.folder });
                });
    CHECK_FALSE (phazite::hasTrack (song, h.folder));
    CHECK_FALSE (phazite::getParentFolder (song, h.track).isValid());

    // The track with its clip, devices, modulator and send, deleted: its Undo brings every area back with it.
    undo.check ("Delete",
                [&]
                {
                    const phazite::UndoStep step (song, "Delete");
                    phazite::deleteTracks (song, { h.track }, nullptr);
                });
    CHECK_FALSE (phazite::hasTrack (song, h.track));
    phazite::undo (song);
    CHECK (phazite::hasTrack (song, h.track));
    CHECK (phazite::getClips (song, h.track).size() == 1);
    CHECK (phazite::getModulators (song, h.track) == std::vector { h.lfo });
    CHECK (phazite::getSends (song, h.track).size() == 1);
    phazite::redo (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks' keeper: a version's line brought back and the master's ID a version gives anew, each one exact "
           "undo step")
{
    Held h;
    auto& song = h.song;
    const auto version = phazite::test::copyWholeSong (song);
    phazite::renameTrack (song, h.track, "Since");
    phazite::moveTrackIntoFolder (song, h.track, h.folder);
    song.getUndoManager().clearUndoHistory();
    phazite::test::UndoCheck undo (h.s.context);

    undo.check ("Line",
                [&]
                {
                    const phazite::UndoStep step (song, "Line");
                    phazite::makeTrackLike (song, h.track, version);
                });
    CHECK (phazite::test::getTrackProperty (song, h.track, schema::trackName)
           == phazite::test::findTrackIn (version, h.track)[schema::trackName]);

    // A version whose master has another ID: given in place, and back by Undo.
    const auto master = phazite::getMasterID (song);
    auto other = phazite::test::copyWholeSong (song);
    const auto newID = song.makeID();
    other.getChildWithName (schema::MASTER).setProperty (schema::trackID, (juce::int64) newID, nullptr);
    undo.check ("Master's ID",
                [&]
                {
                    const phazite::UndoStep step (song, "Master's ID");
                    phazite::makeSongLike (song, other);
                });
    CHECK (phazite::getMasterID (song).getRaw() == newID);
    phazite::undo (song);
    CHECK (phazite::getMasterID (song) == master);
    phazite::redo (song);
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks' keeper: Bounce in Place is one exact undo step")
{
    Held h;
    auto& song = h.song;
    h.s.context.getSongSelection().selectOnly (h.track);
    phazite::test::UndoCheck undo (h.s.context);
    undo.check ("Bounce in Place",
                [&h]
                {
                    h.s.run ("track.bounceInPlace");
                    REQUIRE (phazite::test::dispatchMessagesUntil ([&h] { return ! h.s.renders.finished.isEmpty(); }));
                });
    CHECK (phazite::getTrackIDs (song).size() > 4);
    CHECK (phazite::isTrackMuted (song, h.track, false));
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks' keeper: a drag of a track's volume that a track added meanwhile joins, rolled back, leaves the "
           "tracks as they were and nothing to redo")
{
    // A gesture's roll back (Undo.h: Gesture::rollBack()) undoes the tracks' area's actions in its step, the drag's and the
    // added track's, as a step's Undo does: the area's value before the gesture again, the history's redo steps as they were.
    Held h;
    auto& song = h.song;
    const auto tracksBefore = phazite::getTracksValue (song); // a copy, which shares what it holds
    const auto volumeBefore = phazite::getChannelVolume (song, h.track);
    phazite::test::UndoCheck undo (h.s.context);
    TrackID added;

    undo.check ("Drag Volume, Rolled Back",
                [&]
                {
                    phazite::Gesture gesture (song, "Drag Volume");
                    phazite::setChannelVolume (song, h.track, -12.0f);

                    {
                        const phazite::UndoStep step (song, "Add Track");
                        added = phazite::insertTrack (song, h.track, nullptr);
                    }

                    phazite::setChannelVolume (song, h.track, -18.0f);
                    REQUIRE (phazite::hasTrack (song, added));
                    gesture.rollBack();
                });

    CHECK (undo.getNumSteps() == 0);
    CHECK_FALSE (phazite::hasTrack (song, added));
    CHECK (juce::exactlyEqual (phazite::getChannelVolume (song, h.track), volumeBefore));
    CHECK (phazite::getTracksValue (song) == tracksBefore);
    CHECK_FALSE (song.getUndoManager().canRedo());
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks' keeper: a trial across a track added and its instrument swapped, tried again from the start, is "
           "kept as one step; another is put back, leaving the tracks as they were and nothing to redo")
{
    // A trial (Undo.h: Trial) holds the tracks' area's actions in its step as it holds the devices': tried again from the
    // start, what it tried before (a track and its instrument) is taken back first, the area's value and the devices' with it.
    Held h;
    auto& song = h.song;
    const auto synth = h.s.findDeviceType (phazite::devices::Synth::typeID);
    const auto fm = h.s.findDeviceType (phazite::devices::FmSynth::typeID);
    phazite::test::UndoCheck undo (h.s.context);
    const auto tracksAtFirst = phazite::getTrackIDs (song);
    TrackID first, kept;

    undo.check ("Kept Track",
                [&]
                {
                    phazite::Trial trial (song, "Try", nullptr);
                    trial.apply (
                        [&]
                        {
                            first = phazite::insertTrack (song, h.track, nullptr);
                            phazite::chooseInstrument (h.s.context, first, synth);
                            phazite::chooseInstrument (h.s.context, first, fm); // swapped
                        });
                    REQUIRE (phazite::hasTrack (song, first));
                    REQUIRE (phazite::getInstrument (h.s.context, first).has_value());

                    trial.apply (
                        [&]
                        {
                            kept = phazite::insertTrack (song, h.track, nullptr);
                            phazite::chooseInstrument (h.s.context, kept, fm);
                            phazite::chooseInstrument (h.s.context, kept, synth); // swapped
                        },
                        true);
                    CHECK_FALSE (phazite::hasTrack (song, first));
                    trial.keep ("Kept Track");
                });

    CHECK (undo.getNumSteps() == 1);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Kept Track"));
    CHECK_FALSE (phazite::hasTrack (song, first));
    REQUIRE (phazite::hasTrack (song, kept));
    const auto instrument = phazite::getInstrument (h.s.context, kept);
    REQUIRE (instrument.has_value());
    CHECK (instrument->name == synth.name);
    CHECK (phazite::getTrackIDs (song).size() == tracksAtFirst.size() + 1);

    // Another, put back: the tracks as they were, nothing to redo.
    const auto tracksBefore = phazite::getTracksValue (song); // a copy, which shares what it holds
    TrackID putBack;
    undo.check ("Put Back",
                [&]
                {
                    phazite::Trial trial (song, "Try", nullptr);
                    trial.apply (
                        [&]
                        {
                            putBack = phazite::insertTrack (song, kept, nullptr);
                            phazite::chooseInstrument (h.s.context, putBack, fm);
                        });
                    REQUIRE (phazite::hasTrack (song, putBack));
                    trial.putBack();
                });

    CHECK (undo.getNumSteps() == 1);
    CHECK_FALSE (phazite::hasTrack (song, putBack));
    CHECK (phazite::getTracksValue (song) == tracksBefore);
    CHECK_FALSE (song.getUndoManager().canRedo());
    undo.walkBackAndForth();
    phazite::test::checkSong (song);
}

TEST_CASE ("Song values' tracks' keeper: a folder holding a track that plays, moved into a bus turned down, is silent at the next "
           "plan, and its Undo and Redo are heard too")
{
    // A folder moved is told as one change of it (TracksArea.h: TrackChange::place), the tracks in it told nothing: what plays
    // (SongPlayer.h: tracksChanged()) marks the folder's channel and the folders it left and came to, and the next plan routes
    // the folder, with the track in it, into the bus, whose fader is down.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    s.setTempo (120.0);
    const auto played = s.firstTrack;
    s.setInstrument (played, phazite::devices::Synth::typeID);
    s.addNote (s.addMidiClip (played, 0.0, 64.0), 60, 0.0, 64.0);
    const auto folder = s.addBus ("Folder", { played });
    const auto quiet = s.addTrack ("Quiet");
    const auto bus = s.addBus ("Bus", { quiet });
    s.setVolume (bus, -100.0f);
    song.getUndoManager().clearUndoHistory();

    phazite::test::EnginePlayback playback;
    s.run ("transport.playStop");

    // The level once what changed has eased (a fader, a path that goes or comes).
    const auto level = [&playback]
    {
        playback.play (0.2);
        const auto audio = playback.play (0.2);
        return audio.getMagnitude (0, 0, audio.getNumSamples());
    };

    REQUIRE (level() > 0.01f);
    phazite::test::UndoCheck undo (s.context);
    undo.check ("Into Bus",
                [&]
                {
                    const phazite::UndoStep step (song, "Into Bus");
                    phazite::moveTrackIntoFolder (song, folder, bus);
                });
    REQUIRE (phazite::getParentFolder (song, folder) == bus);
    CHECK (phazite::getParentFolder (song, played) == folder);
    CHECK (level() < 1.0e-4f);

    phazite::undo (song);
    CHECK_FALSE (phazite::getParentFolder (song, folder).isValid());
    CHECK (level() > 0.01f);

    phazite::redo (song);
    CHECK (phazite::getParentFolder (song, folder) == bus);
    CHECK (level() < 1.0e-4f);

    s.run ("transport.playStop");
    phazite::test::checkSong (song);
}
