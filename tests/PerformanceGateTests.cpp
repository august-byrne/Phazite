// SPDX-License-Identifier: AGPL-3.0-or-later
// The speed gates that ctest runs every time: counts of work that don't depend on the machine, so that a change that
// makes Phazite do more (a plan compiled twice for one edit, a track's notes compiled again when another's change, a
// component for every clip of a big song, a save that writes the song twice, work that grows as the square of the song)
// fails here, on any machine, while the timings of the benchmarks (Benchmarks.cpp, BenchmarkGates.cpp) can't be compared
// with stored numbers on a shared machine.
//
// What they don't see: anything slower that does the same amount of work (a device three times as costly, a slower
// paint): the benchmark gates (BenchmarkGates.cpp) compare such costs with a reference workload run beside them. The
// allocations are counted where AudioThreadMonitor.h can count them (Windows Debug and Linux builds); elsewhere those
// cases say so and check nothing.
#include "AudioThreadMonitor.h"
#include "BenchmarkSong.h"
#include "EditorScreens.h"
#include "EnginePlayback.h"
#include "PerformanceCounts.h"
#include "TestClips.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/ProjectSession.h"
#include "devices/delay/Delay.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "engine/nodes/NoteSequence.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/GridSettings.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationValues.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ChangesWorker.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/MixerView.h"
#include "ui/playlist/TrackHeader.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

using phazite::test::at;
using phazite::test::lasting;

namespace
{
namespace dev = phazite::devices;

phazite::Song& getSong (phazite::test::TestSong& s)
{
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    return *song;
}

const phazite::engine::Plan& getPlan()
{
    const auto* plan = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getPlan();
    REQUIRE (plan != nullptr);
    return *plan;
}

// What a plan points at that sounds on from plan to plan, by each strip's ID: its notes and its devices.
struct Shared
{
    std::map<std::int64_t, std::shared_ptr<const phazite::engine::NoteSequence>> notes;
    std::map<std::int64_t, std::vector<std::shared_ptr<phazite::engine::DeviceNode>>> devices;
};

Shared getShared (const phazite::engine::Plan& plan)
{
    Shared shared;

    for (const auto& strip : plan.strips)
    {
        shared.notes[strip.id] = strip.notes;
        shared.devices[strip.id] = {};

        for (const auto& device : strip.devices)
            shared.devices[strip.id].push_back (device.node);
    }

    return shared;
}

std::int64_t idOf (phazite::TrackID track)
{
    return (std::int64_t) track.getRaw();
}

// An editor's snap switched, as its grid menu does (view state).
void toggleSnap (phazite::Song& song, phazite::GridOwner owner)
{
    auto grid = phazite::loadGridSettings (song, owner);
    grid.snap = ! grid.snap;
    phazite::saveGridSettings (song, owner, grid);
}
} // namespace

//==============================================================================
TEST_CASE ("Performance gate: each edit gives the engine one plan, however many changes it makes, and values set by hand none")
{
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 12, 16.0);
    auto& song = getSong (s);
    REQUIRE (s.getProject().save().wasOk());
    phazite::test::PlanCounter plans (s.context);

    const auto track = built.tracks[1];
    const auto clip = built.clips[1];
    const auto equalizer = phazite::getEffects (s.context, track)[0].id;
    const auto output = juce::String (std::string (dev::Equalizer::info.parameters[(size_t) dev::Equalizer::outputParameter].id));

    struct Edit
    {
        const char* what;
        std::function<void()> make;
        int plans;
    };

    // In the order they are made, each on the song as the ones before left it.
    const std::vector<Edit> edits {
        { "a note added", [&] { phazite::addNote (s.context, clip, 72, at (2.5), lasting (0.25), 100); }, 1 },
        { "eight notes added in one turn of the message loop",
          [&]
          {
              for (int i = 0; i < 8; ++i)
                  phazite::addNote (s.context, clip, 60 + i, at (4.0 + i * 0.25), lasting (0.25), 100);
          },
          1 },
        { "a clip moved", [&] { phazite::moveClip (s.context, clip, at (1.0), track); }, 1 },
        { "a track renamed", [&] { phazite::renameTrack (s.context, track, "Renamed"); }, 1 },
        { "an effect added", [&] { s.addEffect (track, dev::Delay::typeID); }, 1 },
        { "an effect removed", [&] { phazite::removeDevice (s.context, phazite::getEffects (s.context, track).back().id); }, 1 },
        { "Undo", [&] { phazite::undo (song); }, 1 },
        { "Redo", [&] { phazite::redo (song); }, 1 },
        { "the tempo changed", [&] { s.setTempo (128.0); }, 1 },
        { "the loop switched on", [&] { phazite::toggleLoop (song); }, 1 },
        { "a device's parameter set by hand, its first", [&] { s.setParameter (equalizer, output, -3.0f); },
          // The first value a device has set by hand adds its VALUES element (Schema.h), which the session takes as any
          // element added: a plan, once for each device.
          1 },
        { "the same parameter set by hand again", [&] { s.setParameter (equalizer, output, -4.0f); }, 0 },
        { "a track's volume set by hand", [&] { s.setVolume (track, -6.0f); }, 0 },
        { "a track's pan set by hand", [&] { s.setPan (track, 0.25f); }, 0 },
        { "a track muted", [&] { phazite::toggleMute (s.context, track); }, 0 },
        { "a clip selected", [&] { s.select (clip); }, 0 },
        { "the playhead moved", [&] { phazite::movePlayhead (song, at (8.0)); }, 0 },
        { "the project saved", [&] { REQUIRE (s.getProject().save().wasOk()); }, 0 },

        // View state, which no plan holds (known bug 124): each gave a plan before.
        { "the playlist's grid changed", [&] { toggleSnap (song, phazite::GridOwner::playlist); }, 0 },
        { "the piano roll's grid changed, its first", [&] { toggleSnap (song, phazite::GridOwner::pianoRoll); }, 0 },
        { "the piano roll's grid changed again", [&] { toggleSnap (song, phazite::GridOwner::pianoRoll); }, 0 },
        { "a device's window moved", [&] { phazite::rememberWindowPosition (song, equalizer, { 40, 60 }); }, 0 },
        { "an automation lane shown", [&] { phazite::showLane (song, track, { equalizer, output }); }, 0 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
    }

    CHECK (song.getIndex().isExact()); // the checks of every lookup are off in the gates: the whole index checked here
}

TEST_CASE ("Performance gate: a new plan keeps what didn't change, a track's notes and every device")
{
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 12, 16.0);
    phazite::test::PlanCounter plans (s.context);
    const auto before = getShared (getPlan());

    // A note added on one track: only its notes are compiled again.
    const auto edited = built.tracks[3];
    phazite::addNote (s.context, built.clips[3], 72, at (2.5), lasting (0.25), 100);
    REQUIRE (plans.take() == 1);
    const auto after = getShared (getPlan());
    REQUIRE (after.notes.size() == before.notes.size());

    for (const auto& [id, notes] : after.notes)
    {
        INFO ("the strip of " << id);
        REQUIRE (before.notes.count (id) == 1);

        if (id == idOf (edited))
            CHECK (notes != before.notes.at (id));
        else
            CHECK (notes == before.notes.at (id));

        // The devices sound on: the same as before, every one.
        CHECK (after.devices.at (id) == before.devices.at (id));
    }

    CHECK (getSong (s).getIndex().isExact());
}

TEST_CASE ("Performance gate: an edit's plan reads the values of the devices on the channel it changed, and the curves it changed")
{
    // Known bug 145: every plan read every device's values from the song again, most of a big song's compile (0.4 ms a track,
    // 40 ms an edit at 100 tracks); and known bug 124: every plan read all the automation again. Now a device's values are
    // read for the first plan and once its channel changed, and a curve once its points changed: an edit of clips, the
    // tempo or another channel reads none of them.
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 12, 16.0);
    auto& song = getSong (s);
    phazite::test::PlanCounter plans (s.context);
    const auto track = built.tracks[3];
    auto devicesRead = phazite::SongPlayer::countDevicesRead();
    auto curvesRead = phazite::SongPlayer::countCurvesRead();

    struct Read
    {
        size_t devices = 0, curves = 0;
    };

    const auto take = [&]
    {
        const auto devicesNow = phazite::SongPlayer::countDevicesRead(), curvesNow = phazite::SongPlayer::countCurvesRead();
        return Read { (size_t) (devicesNow - std::exchange (devicesRead, devicesNow)),
                      (size_t) (curvesNow - std::exchange (curvesRead, curvesNow)) };
    };

    const auto check = [&] (const char* what, size_t devices, size_t curves)
    {
        INFO (std::string (what));
        REQUIRE (plans.take() == 1);
        const auto read = take();
        CHECK (read.devices == devices);
        CHECK (read.curves == curves);
    };

    phazite::addNote (s.context, built.clips[3], 72, at (2.5), lasting (0.25), 100);
    check ("a note added", 0, 0);

    phazite::moveClip (s.context, built.clips[5], at (1.0), built.tracks[5]);
    check ("a clip moved", 0, 0);

    s.setTempo (128.0);
    check ("the tempo changed", 0, 0);

    // An automation point added to the first Synth's cutoff: that curve alone.
    phazite::addAutomationPoint (s.context, { built.instruments[0], "cutoff" }, { at (3.0), 0.6f });
    check ("an automation point added", 0, 1);

    // A device added to one track: that track's devices, each once.
    s.addEffect (track, dev::Delay::typeID);
    check ("an effect added", phazite::getEffects (s.context, track).size() + 1, 0); // and its instrument

    phazite::undo (song);
    check ("Undo", phazite::getEffects (s.context, track).size() + 1, 0);

    CHECK (song.getIndex().isExact());
}

TEST_CASE ("Performance gate: a change no device reads leaves the next edit's plan reading no device's values")
{
    // Known bug 145's review: every change under a track marked its channel, so that the next real edit read every device
    // on it again: a track's height, its mute, a plug-in's state stored by a save (Song::storeDeviceStates(), at every
    // save and auto-save). Now a channel is marked for a change at or under a device, a container, a chain or a modulator
    // (but where a device's window was, and a plug-in's own state), or an element of it added, removed or moved.
    const phazite::test::TestPlugin plugin;
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 12, 16.0);
    auto& song = getSong (s);
    const auto track = built.tracks[3];
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add Device");
        device = phazite::test::TestPlugin::add (song, track);
    }

    phazite::test::PlanCounter plans (s.context);
    auto read = phazite::SongPlayer::countDevicesRead();

    struct Change
    {
        const char* what;
        std::function<void()> make;
    };

    const std::vector<Change> changes {
        { "a track's height", [&] { phazite::setSavedTrackHeight (song, track, 120); } },
        { "a track muted", [&] { phazite::toggleMute (s.context, track); } },
        { "a device's window moved",
          [&] { phazite::rememberWindowPosition (song, phazite::getEffects (s.context, track)[0].id, { 10, 20 }); } },
        { "the plug-ins' states stored, as a save stores them", [&] { song.storeDeviceStates(); } },
        { "a plug-in's state written, as a store writes it",
          [&] { phazite::findDeviceState (song, device).setProperty (phazite::schema::pluginState, "c3RhdGU=", nullptr); } },
    };

    for (const auto& change : changes)
    {
        INFO (std::string (change.what));
        change.make();
        plans.take();
        read = phazite::SongPlayer::countDevicesRead();

        // The next note on that track: its notes compiled again, and no device's values read.
        phazite::addNote (s.context, built.clips[3], 72, at (2.5), lasting (0.25), 100);
        CHECK (plans.take() == 1);
        CHECK (phazite::SongPlayer::countDevicesRead() == read);
    }

    CHECK (song.getIndex().isExact());
}

TEST_CASE ("Performance gate: an edit's plan asks the disk about the files of the track it changed, each once")
{
    // Known bug 124: every plan asked the disk about each audio clip's file twice, and made the tempo map several times for
    // each clip. Now a track's audio is placed again only once its clips (or the tempo) changed, and the disk is asked
    // about a file again only for a track whose clips changed, once however many of its clips play it; and about every file
    // the song plays once a second has passed since it last did (known bug 198: a file written again in its place by another
    // program played as first read), once however many of its clips play it, by Phazite's clock, held here. Four tracks of
    // ten clips, each track's of a file of its own, beside the first track's MIDI clip.
    phazite::test::HeldClock clock;
    phazite::test::TestSong s;
    std::vector<phazite::TrackID> tracks;
    std::vector<phazite::ClipID> clips;

    for (int t = 0; t < 4; ++t)
    {
        tracks.push_back (s.addTrack ("Audio " + juce::String (t + 1)));
        const auto file = phazite::test::writeSound (s.getSoundsFolder().getChildFile ("Loop " + juce::String (t + 1) + ".wav"), 1.0);

        for (int c = 0; c < 10; ++c)
            clips.push_back (s.addAudioClip (tracks.back(), file, c * 4.0));
    }

    s.setInstrument (s.firstTrack, dev::Synth::typeID);
    const auto midi = s.addMidiClip (s.firstTrack, 0.0, 16.0);
    phazite::test::dispatchPendingMessages();
    phazite::test::PlanCounter plans (s.context);
    auto asked = phazite::SongPlayer::countFilesAsked();

    const auto take = [&asked]
    {
        const auto now = phazite::SongPlayer::countFilesAsked();
        return now - std::exchange (asked, now);
    };

    s.addNote (midi, 60, 0.0, 1.0);
    REQUIRE (plans.take() == 1);
    CHECK (take() == 0);

    phazite::moveClip (s.context, clips[3], at (13.0), tracks[0]);
    REQUIRE (plans.take() == 1);
    CHECK (take() == 1); // the first track's file, for its ten clips

    s.setTempo (128.0);
    REQUIRE (plans.take() == 1);
    CHECK (take() == 0); // every clip placed again, its file known

    phazite::deleteClips (s.context, { clips[12] }, false);
    REQUIRE (plans.take() == 1);
    CHECK (take() == 1); // the second track's file, for its nine clips left

    // A second on: the next edit's plan asks about each file once, and the one after it about none.
    clock.advance (phazite::trustFilesMilliseconds);
    s.addNote (midi, 62, 1.0, 1.0);
    REQUIRE (plans.take() == 1);
    CHECK (take() == 4);

    s.addNote (midi, 64, 2.0, 1.0);
    REQUIRE (plans.take() == 1);
    CHECK (take() == 0);
}

//==============================================================================
namespace
{
// The samples a plan's rings keep, on one channel: each ring its paths read (a strip's taps, and the delays' own), once.
std::int64_t countRingSamples (const phazite::engine::Plan& plan)
{
    std::set<const phazite::engine::DelayLine*> rings;
    const auto add = [&rings] (const std::shared_ptr<phazite::engine::Delay>& delay)
    {
        if (delay != nullptr && delay->getLine() != nullptr)
            rings.insert (delay->getLine().get());
    };

    for (const auto& strip : plan.strips)
    {
        for (const auto& tap : strip.taps)
            rings.insert (tap.ring.get());

        for (const auto& input : strip.inputs)
            add (input.delay);

        for (const auto& device : strip.devices)
        {
            add (device.sidechainDelay);
            add (device.inputDelay);
        }

        for (const auto& container : strip.containers)
            for (const auto& chain : container.chains)
                add (chain.delay);
    }

    std::int64_t samples = 0;

    for (const auto* ring : rings)
        samples += ring->isAllocated() ? ring->getCapacity() : 0;

    return samples;
}
} // namespace

TEST_CASE ("Performance gate: a render's rings grow with its delays, not with its paths, and a strip it leaves out has none")
{
    // The review of known bug 167, round 5: a render (an export, each stem, a bounce) gave every path a ring with a third of
    // a second of room, which a song of many tracks and sends multiplied past memory. Now a render's path with no delay has
    // none, one with a delay reads its source's tap, as long as the longest delay from there and a block; and nothing is
    // made for what the render leaves out. Eight tracks, each sending after its fader to one return or to four; one has 64
    // samples of latency: each other track's sound after its fader (to the master) and before its mute (to the returns) is
    // kept 64 samples and a block, however many returns there are; a muted track (whose sends after its fader are cut)
    // keeps none. Live, every path keeps a ring with room (as before), shared by the paths from one point of a strip.
    constexpr int tracks = 8, block = 256, latency = 64;

    for (const auto returns : { 1, 4 })
    {
        for (const auto muted : { false, true })
        {
            CAPTURE (returns);
            CAPTURE (muted);
            phazite::test::TestSong s;
            auto& song = getSong (s);
            std::vector<phazite::TrackID> ids { s.firstTrack };

            while ((int) ids.size() < tracks)
                ids.push_back (s.addTrack ("Track " + juce::String (ids.size() + 1)));

            for (int r = 0; r < returns; ++r)
            {
                const auto back = s.addReturn (ids.front(), dev::Equalizer::typeID, 0.0f);

                for (size_t t = 1; t < ids.size(); ++t)
                    s.addSend (ids[t], back, 0.0f);
            }

            if (muted)
                phazite::toggleMute (s.context, ids.back());

            // With no latency, nothing.
            const auto device = phazite::test::addTestDevice (song, ids.front(), phazite::test::TestLatency::typeID);
            phazite::test::dispatchPendingMessages();
            CHECK (countRingSamples (*phazite::SongPlayer (song).compile (48000.0, block, std::nullopt)) == 0);

            s.setParameter (device, "samples", (float) latency);
            phazite::test::dispatchPendingMessages();
            const auto kept = muted ? tracks - 2 : tracks - 1;
            CHECK (countRingSamples (*phazite::SongPlayer (song).compile (48000.0, block, std::nullopt)) == 2 * kept * (latency + block));
        }
    }
}

//==============================================================================
TEST_CASE ("Performance gate: the playlist makes components for what is in view, however big the song")
{
    // The same playlist, in the same window, of a song of 20 tracks and of 200, each with 24 clips of 4 bars.
    const auto countFor = [] (int numTracks)
    {
        phazite::test::PlaylistScreen screen;
        const auto tracks = phazite::ensureTracks (*screen.song, numTracks);

        for (const auto track : tracks)
            for (int c = 0; c < 24; ++c)
                REQUIRE (phazite::insertMidiClip (*screen.song, track, { at (c * 16.0), at (c * 16.0 + 16.0) }, nullptr).isValid());

        phazite::test::deliverChanges();
        CHECK (screen.song->getIndex().isExact());
        return std::tuple (phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist).size(),
                           phazite::test::findAll<phazite::ui::TrackHeader> (screen.playlist).size(),
                           phazite::test::findAll<juce::Component> (screen.playlist).size());
    };

    const auto [smallClips, smallHeaders, smallComponents] = countFor (20);
    const auto [bigClips, bigHeaders, bigComponents] = countFor (200);
    MESSAGE ("20 tracks: " << smallClips << " clip components, " << smallHeaders << " headers, " << smallComponents
                           << " components; 200 tracks: " << bigClips << " clip components, " << bigHeaders << " headers, " << bigComponents
                           << " components");
    CHECK (smallClips > 0);
    CHECK (bigClips == smallClips);

    // The headers of the tracks in view, and of one more at either end, which the Tab key goes on to: as many for 200 tracks
    // as for 20, which don't all fit either. (They were made for every track, seven components each, known bug 144 until
    // 2026-11-15.)
    CHECK (smallHeaders < 20);
    CHECK (bigHeaders == smallHeaders);
    CHECK (bigComponents == smallComponents);
}

TEST_CASE ("Performance gate: the playlist makes the automation lanes' components for what is in view, however many tracks")
{
    // Every track's volume lane shown, of 20 tracks and of 200: the lanes' headers and curves of those in view (they were made
    // for every lane, the reviewer's finding on known bug 144, 2026-11-15).
    const auto countFor = [] (int numTracks)
    {
        phazite::test::PlaylistScreen screen;

        for (const auto track : phazite::ensureTracks (*screen.song, numTracks))
        {
            const auto volume = phazite::getVolume (screen.context, track);
            phazite::showAutomationLane (volume);
            phazite::addAutomationPoint (screen.context, volume.getRef(), { at (2.0), 0.5f });
        }

        phazite::test::deliverChanges();
        return phazite::test::findAll<juce::Component> (screen.playlist).size();
    };

    const auto small = countFor (20), big = countFor (200);
    MESSAGE ("Components, every track's volume lane shown: 20 tracks " << small << ", 200 tracks " << big);
    CHECK (big == small);
}

TEST_CASE ("Performance gate: the mixer makes strips for what is in view, however many tracks")
{
    // The same mixer, in the same window, of 20 tracks and of 200, each with an EQ: the strips in view, and one more at
    // either end, which the Tab key goes on to. (Every channel had a strip, its timer ticking eight times a second, known
    // bug 127 until 2026-11-15.)
    const auto countFor = [] (int numTracks)
    {
        auto song = phazite::createSong();

        for (const auto track : phazite::ensureTracks (*song, numTracks))
            REQUIRE (phazite::insertDeviceState (*song, track, phazite::makeOwnDeviceState (*song, dev::Equalizer::typeID)).isValid());

        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setSong (context, song.get());
        phazite::CommandRegistry registry;
        phazite::ui::CommandBridge commands { registry };
        phazite::ui::MixerView mixer { commands, context };
        phazite::test::TestWindow window { mixer, 1000, 600 };
        phazite::test::deliverChanges();
        const auto counts = std::pair (phazite::test::findAll<phazite::ui::ChannelStrip> (mixer).size(),
                                       phazite::test::findAll<juce::Component> (mixer).size());
        setSong (context, nullptr);
        return counts;
    };

    const auto [smallStrips, smallComponents] = countFor (20);
    const auto [bigStrips, bigComponents] = countFor (200);
    MESSAGE ("20 tracks: " << smallStrips << " strips, " << smallComponents << " components; 200 tracks: " << bigStrips << " strips, "
                           << bigComponents << " components");
    CHECK (smallStrips > 0);
    CHECK (smallStrips < 20);
    CHECK (bigStrips == smallStrips);
    CHECK (bigComponents == smallComponents);
}

TEST_CASE ("Performance gate: a device changed on one channel among many brings only its strip up to date")
{
    // Every strip brought itself up to date at a change of any channel's devices (known bug 127 until 2026-11-15): now only
    // the changed channel's, and the return a changed send goes to. Each strip shows what is due at its next tick.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 6);
    const auto reverb = phazite::addReturnTrack (*song, "Reverb", nullptr);
    REQUIRE (reverb.isValid());
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, song.get());
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::MixerView mixer { commands, context };
    phazite::test::TestWindow window { mixer, 1400, 600 };
    phazite::test::deliverChanges();

    const auto stripOf = [&mixer] (phazite::TrackID track) -> phazite::ui::ChannelStrip&
    {
        for (auto* strip : phazite::test::findAll<phazite::ui::ChannelStrip> (mixer))
            if (strip->getTrack() == track)
                return *strip;

        FAIL ("no strip shows the track");
        throw std::logic_error ("unreachable");
    };

    REQUIRE (phazite::test::findAll<phazite::ui::ChannelStrip> (mixer).size() == 7); // every channel in view

    // What each strip has done since: brought itself up to date, or has that due.
    const auto refreshes = [&]
    {
        std::map<std::uint64_t, int> counts;

        for (auto* strip : phazite::test::findAll<phazite::ui::ChannelStrip> (mixer))
            counts[strip->getTrack().getRaw()] = strip->getRefreshCount() + (strip->isRefreshDue() ? 1 : 0);

        return counts;
    };

    const auto onlyThese = [&] (const std::function<void()>& change, std::vector<phazite::TrackID> expected)
    {
        phazite::test::deliverChanges();
        const auto before = refreshes();
        change();
        phazite::test::deliverChanges();

        // Wait for the changed strips to show it.
        for (const auto track : expected)
            CHECK (phazite::test::runTimersUntil ([&] { return ! stripOf (track).isRefreshDue(); }));

        const auto after = refreshes();

        for (const auto& [raw, count] : after)
        {
            const auto track = phazite::TrackID::fromRaw (raw);
            const auto changed = std::find (expected.begin(), expected.end(), track) != expected.end();
            INFO ("the strip of track " << (juce::int64) raw);
            CHECK (count == before.at (raw) + (changed ? 1 : 0));
        }
    };

    SUBCASE ("an effect added")
    {
        onlyThese (
            [&]
            { REQUIRE (phazite::insertDeviceState (*song, tracks[2], phazite::makeOwnDeviceState (*song, dev::Delay::typeID)).isValid()); },
            { tracks[2] });
    }

    std::vector<phazite::TrackID> everyChannel (tracks.begin(), tracks.end());
    everyChannel.push_back (reverb);

    SUBCASE ("a send added: where sound can go changed, which every strip's send knobs follow")
    {
        onlyThese ([&] { REQUIRE (phazite::addSend (*song, tracks[3], reverb, false).isValid()); }, everyChannel);
    }

    SUBCASE ("a send switched before the fader, which its return lists")
    {
        const auto send = phazite::addSend (*song, tracks[3], reverb, false);
        REQUIRE (send.isValid());
        phazite::test::deliverChanges();

        for (const auto track : everyChannel)
            REQUIRE (phazite::test::runTimersUntil ([&] { return ! stripOf (track).isRefreshDue(); }));

        onlyThese ([&] { phazite::setPreFader (*song, send, true); }, { tracks[3], reverb });
    }

    setSong (context, nullptr);
}

TEST_CASE ("Performance gate: what an edit repaints in the playlist")
{
    phazite::test::PlaylistScreen screen;
    const auto tracks = phazite::ensureTracks (*screen.song, 20);
    std::vector<phazite::ClipID> clips;

    for (const auto track : tracks)
        clips.push_back (phazite::insertMidiClip (*screen.song, track, { at (0.0), at (16.0) }, nullptr));

    phazite::test::deliverChanges();
    const auto window = screen.window.getLocalBounds();

    struct Edit
    {
        const char* what;
        std::function<void()> make;
        std::function<juce::Rectangle<int>()> mayRepaint; // the most it may repaint, in the window
        const char* knownException;                       // why it repaints more for now, or nullptr
    };

    const auto nothing = [] { return juce::Rectangle<int>(); };
    const std::vector<Edit> edits {
        { "a track's volume set by hand", [&] { phazite::getVolume (screen.context, tracks[1]).setValue (0.5f); }, nothing, nullptr },
        { "a track's pan set by hand", [&] { phazite::getPan (screen.context, tracks[1]).setValue (0.5f); }, nothing, nullptr },
        // Its clip alone (every lane repainted, known bug 144 until 2026-11-15).
        { "a note added", [&] { phazite::addNote (screen.context, clips[1], 60, at (2.0), lasting (1.0), 100); },
          [&] { return screen.window.getLocalArea (screen.lanes, screen.boundsOf (clips[1])); }, nullptr },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        screen.window.takeRepaintedArea();
        edit.make();
        phazite::test::deliverChanges();
        const auto repainted = screen.window.takeRepaintedArea();
        const auto allowed = edit.mayRepaint();
        const auto within = allowed.isEmpty() ? repainted.isEmpty() : allowed.expanded (2).contains (repainted.getBounds());
        MESSAGE (std::string (edit.what) << ": repainted " << repainted.getBounds().toString() << " (allowed " << allowed.toString()
                                         << ") in a window of " << window.toString());

        if (edit.knownException != nullptr)
        {
            // Once it repaints no more than it may, the exception goes.
            INFO ("known exception: " << edit.knownException);
            CHECK_FALSE (within);
        }
        else
        {
            CHECK (within);
        }
    }

    CHECK (screen.song->getIndex().isExact());
}

TEST_CASE ("Performance gate: a note added to one clip among thousands shows that clip again, and no other")
{
    // A playlist of 200 tracks, each with 24 clips of 4 bars: a note added, or a clip moved, read every clip of the song again
    // and repainted every clip in view and all the lanes (known bug 144 until 2026-11-15). Its plans aren't compiled (the
    // session lent, as to a render), so that what is counted is the playlist's.
    const auto playlistOf = [] (phazite::test::PlaylistScreen& screen, int numTracks)
    {
        auto* session = phazite::getSession (screen.context);
        REQUIRE (session != nullptr);
        session->lendToRender (true);
        std::vector<std::vector<phazite::ClipID>> clips;

        for (const auto track : phazite::ensureTracks (*screen.song, numTracks))
        {
            auto& onTrack = clips.emplace_back();

            for (int c = 0; c < 24; ++c)
                onTrack.push_back (phazite::insertMidiClip (*screen.song, track, { at (c * 16.0), at (c * 16.0 + 16.0) }, nullptr));
        }

        phazite::test::deliverChanges();
        return clips;
    };

    SUBCASE ("what it repaints")
    {
        phazite::test::PlaylistScreen screen;
        const auto clips = playlistOf (screen, 200);
        auto* overview = phazite::test::findComponentWithTitle (screen.playlist, "Overview");
        REQUIRE (overview != nullptr);
        const auto inWindow = [&screen] (juce::Rectangle<int> inLanes) { return screen.window.getLocalArea (screen.lanes, inLanes); };

        const auto repaintedBy = [&screen] (const std::function<void()>& edit)
        {
            screen.window.takeRepaintedArea();
            edit();
            phazite::test::deliverChanges();
            return screen.window.takeRepaintedArea().getBounds();
        };

        // In view: its clip alone.
        const auto clip = clips[1][1];
        const auto bounds = inWindow (screen.boundsOf (clip));
        REQUIRE_FALSE (bounds.isEmpty());
        CHECK (bounds.expanded (2).contains (
            repaintedBy ([&] { phazite::addNote (screen.context, clip, 60, at (2.0), lasting (1.0), 100); })));

        // Out of view: nothing.
        REQUIRE (screen.boundsOf (clips[150][20]).isEmpty());
        CHECK (repaintedBy ([&] { phazite::addNote (screen.context, clips[150][20], 60, at (2.0), lasting (1.0), 100); }).isEmpty());

        // A clip moved a beat: where it was, where it is, and the overview of the song, which shows it.
        const auto moved = clips[2][0];
        const auto before = inWindow (screen.boundsOf (moved));
        const auto track = screen.get (moved).track;
        const auto repainted = repaintedBy ([&] { phazite::moveClip (screen.context, moved, at (1.0), track); });
        const auto allowed = before.getUnion (inWindow (screen.boundsOf (moved))).expanded (2);
        const auto overviewArea = screen.window.getLocalArea (overview, overview->getLocalBounds());
        CHECK (allowed.getUnion (overviewArea).contains (repainted));
        CHECK (repainted.intersects (allowed));
    }

    SUBCASE ("what it allocates, however big the song")
    {
        if (! phazite::test::AudioThreadMonitor::isAvailable())
        {
            MESSAGE ("Allocations aren't counted in this build (AudioThreadMonitor.h): nothing checked here.");
            return;
        }

        const auto countFor = [&] (int numTracks)
        {
            phazite::test::PlaylistScreen screen;
            const auto clips = playlistOf (screen, numTracks);
            return phazite::test::countAllocations (
                       [&]
                       {
                           phazite::addNote (screen.context, clips[1][1], 72, at (2.5), lasting (0.25), 100);
                           phazite::test::deliverChanges();
                       })
                .value_or (0);
        };

        const auto small = countFor (20), big = countFor (200);
        MESSAGE ("Allocations, a note added, 20 tracks / 200 tracks of 24 clips: " << small << " / " << big);

        // Ten times the clips: about the same work.
        CHECK (big < small * 2);
    }
}

//==============================================================================
TEST_CASE ("Performance gate: what an edit and a save allocate grows as the song does, and a save writes the song once")
{
    if (! phazite::test::AudioThreadMonitor::isAvailable())
    {
        MESSAGE ("Allocations aren't counted in this build (AudioThreadMonitor.h): nothing checked here.");
        return;
    }

    struct Counts
    {
        int edit = 0, save = 0, text = 0;
        int plans = 0; // made within the edit's count
    };

    const auto countsFor = [] (int numTracks)
    {
        phazite::test::TestSong s;
        const auto built = phazite::test::buildBenchmarkSong (s, numTracks, 16.0);
        auto& song = getSong (s);
        REQUIRE (s.getProject().save().wasOk());
        phazite::test::dispatchPendingMessages();

        phazite::test::PlanCounter plans (s.context);
        plans.take();
        Counts counts;

        // The plan is made within the count and without the message loop: a wait for it there took in whatever else the loop
        // delivered meanwhile (the same song counted 13,244, 15,380 or 20,407 from run to run, and Linux CI's small song 869
        // against the big one's 10,059), which says nothing of how the work grows.
        counts.edit = phazite::test::countAllocations (
                          [&]
                          {
                              phazite::addNote (s.context, built.clips[0], 72, at (2.5), lasting (0.25), 100);
                              plans.makePlanNow();
                          })
                          .value_or (0);
        counts.plans += plans.take();
        REQUIRE (counts.plans == 1);
        counts.save = phazite::test::countAllocations ([&] { REQUIRE (s.getProject().save().wasOk()); }).value_or (0);
        counts.text = phazite::test::countAllocations (
                          [&]
                          {
                              juce::MemoryOutputStream text;
                              phazite::writeProjectText (text, phazite::test::copyWholeSong (song));
                          })
                          .value_or (0);
        CHECK (song.getIndex().isExact());
        return counts;
    };

    // Measured (2026-11-19, Windows), 16 / 64 tracks: a note added with its new plan 3,779 / 13,229; a save 8,721 / 34,309;
    // the song's text alone 6,431 / 25,534.
    const auto small = countsFor (16), big = countsFor (64); // four times the tracks
    MESSAGE ("Allocations, 16 tracks / 64 tracks: a note added with its new plan "
             << small.edit << " / " << big.edit << "; a save " << small.save << " / " << big.save << "; the song's text alone "
             << small.text << " / " << big.text);

    // Four times the song: about four times the work (3.5 and 3.9 measured), and well short of the 16 times that work which
    // grows as the square of the song makes.
    CAPTURE (small.plans);
    CAPTURE (big.plans);
    CHECK (big.edit < small.edit * 6);
    CHECK (big.save < small.save * 6);

    // A save writes the song's text once, with what it keeps of the song beside it (1.34 times the text's allocations
    // measured): the text written twice would make it 2.3.
    CHECK (big.save * 4 < big.text * 7);
}

TEST_CASE ("Performance gate: selected automation points are found by their beats, the work growing with the points, not with their square")
{
    // Each selected point was looked for through the whole curve, and selecting them compared each with every other: a
    // drawn lane of 10,000 points selected cost about 10^8 comparisons each time a lane painted.
    const auto comparisonsFor = [] (int numPoints)
    {
        phazite::test::TestSong song;
        auto& document = *phazite::findSong (song.context);
        const auto volume = song.getVolumeParameter (song.firstTrack);
        std::vector<phazite::ParameterPoint> points;
        std::vector<int> all;

        for (int i = 0; i < numPoints; ++i)
        {
            points.push_back ({ volume, { phazite::BeatPosition::fromBeats (i * 0.25), (float) (i % 2) * 0.5f } });
            all.push_back (i);
        }

        // Two at the last beat, a jump.
        points.push_back ({ volume, { phazite::BeatPosition::fromBeats ((numPoints - 1) * 0.25), 0.75f } });
        all.push_back (numPoints);
        phazite::addAutomationPoints (document, points);

        auto& selection = phazite::getSelection (song.context);
        const auto before = phazite::Selection::getPointComparisons();
        phazite::selectPoints (selection, volume, all);
        const auto selected = phazite::getSelectedPoints (selection);
        const auto used = phazite::Selection::getPointComparisons() - before;
        REQUIRE (selected.has_value());
        CHECK (selected->indices.size() == (size_t) numPoints + 1);
        CHECK (selection.getNumObjectsSelected() == numPoints + 1);
        CHECK (document.getIndex().isExact());
        return used;
    };

    // Ten times the points, about ten times the work (a little more for the sorting); never a hundred.
    const auto small = comparisonsFor (1000), large = comparisonsFor (10000);
    INFO ("1,000 points: " << small << " comparisons; 10,000: " << large);
    CHECK (large < small * 20);
}

//==============================================================================
TEST_CASE ("Performance gate: a step of a drag of notes looks at as many elements as it moves notes, whatever the song and the clip")
{
    // Known bug 122: each note was looked for through every track's clips, several times for each note moved, and through
    // its clip's notes, so that a step of a drag of 500 notes in a clip of 5,000 looked at millions of elements.
    const auto visitedFor = [] (int numTracks, int numNotes)
    {
        auto song = phazite::createSong();
        phazite::ProjectContext context (phazite::test::getEngineContext());
        const auto tracks = phazite::ensureTracks (*song, numTracks);

        for (const auto track : tracks)
            for (int c = 1; c < 24; ++c)
                REQUIRE (phazite::insertMidiClip (*song, track, { at (2000.0 + c * 16.0), at (2016.0 + c * 16.0) }, nullptr).isValid());

        // A clip of notes four a beat, at the start of the last track (which a walk through the song finds last), a tenth of
        // them selected.
        const auto clip = phazite::insertMidiClip (*song, tracks.back(), { at (0.0), at (numNotes * 0.25) }, nullptr);
        std::vector<phazite::NoteData> notes;

        for (int i = 0; i < numNotes; ++i)
            notes.push_back ({ .clip = clip, .pitch = 48 + i % 24, .start = at (i * 0.25), .length = lasting (0.2) });

        phazite::setClipNotes (*song, clip, notes);
        phazite::setSong (context, song.get());
        notes = phazite::getNotes (context, { clip });
        notes.resize ((size_t) numNotes / 10);
        const auto ends = phazite::getClipEnds (context, { clip });

        // As the piano roll's drag makes each step (PianoRollNoteTools.cpp: dragNotes()).
        const auto step = [&] (double by)
        {
            phazite::restoreClipEnds (context, ends);
            auto moved = notes;

            for (auto& note : moved)
                note.start = note.start + lasting (by);

            phazite::setNotes (context, moved);
            REQUIRE (phazite::getNote (context, notes.front().getRef()).has_value());
        };

        step (0.25); // the first, as the song is first asked for its notes
        const auto before = phazite::SongIndex::countElementsVisited();
        step (0.5);
        const auto visited = phazite::SongIndex::countElementsVisited() - before;
        CHECK (song->getIndex().isExact());
        phazite::setSong (context, nullptr);
        return visited;
    };

    const auto small = visitedFor (10, 200), moreNotes = visitedFor (10, 2000), moreTracks = visitedFor (100, 200);
    INFO ("Elements looked at by a step of a drag of 20 notes: " << small << "; of 200 notes in a clip of 2,000: " << moreNotes
                                                                 << "; of 20 notes in a song of ten times the tracks: " << moreTracks);
    CHECK (small > 0);
    CHECK (moreNotes < small * 20); // ten times the notes moved: about ten times the work, never a hundred
    CHECK (moreTracks < small * 2); // ten times the song: the same work
}

TEST_CASE ("Performance gate: a step of a drag in a controller lane reads no note, and changes as much whatever the notes in its clip")
{
    // As the piano roll's lane draws a step (PianoRollControlLane.cpp: putCells()): the lane's values in a beat replaced by
    // two. It takes the lane's values there out of the clip's value and puts the new ones in, one change of the value
    // (model/base/internal/SongValues.h), walking no element of the clip's and reading none of its notes.
    struct Counts
    {
        std::uint64_t notesRead = 0, visited = 0;
        int changes = 0;
    };

    // Each change of the tree, and each controller's value a change of the clips' values took away or brought.
    struct Heard : juce::ValueTree::Listener,
                   phazite::ValueArea<phazite::NotesByClip>::Listener
    {
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++changes; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++changes; }
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++changes; }

        void valueAreaChanged (const phazite::NotesByClip& before, const phazite::NotesByClip& after) override
        {
            for (const auto& [id, value] : after.clips)
            {
                const auto* old = before.find (phazite::ClipID::fromRaw (id));

                if (old != nullptr && old->isSameAs (value))
                    continue;

                // In the order each holds them: what isn't in the other, one by one (a lane's values are few and ordered).
                std::vector<phazite::ControlValues> was, now;

                if (old != nullptr)
                    was.assign (old->controls.begin(), old->controls.end());

                now.assign (value.controls.begin(), value.controls.end());

                for (const auto& control : was)
                    if (const auto found = std::find (now.begin(), now.end(), control); found != now.end())
                        now.erase (found);
                    else
                        ++changes;

                changes += (int) now.size();
            }
        }

        int changes = 0;
    };

    const auto countFor = [] (int numNotes)
    {
        auto song = phazite::createSong();
        phazite::ProjectContext context (phazite::test::getEngineContext());
        const auto track = phazite::ensureTracks (*song, 1).front();
        const auto clip = phazite::insertMidiClip (*song, track, { at (0.0), at (std::max (64.0, numNotes * 0.25)) }, nullptr);
        std::vector<phazite::NoteData> notes;

        for (int i = 0; i < numNotes; ++i)
            notes.push_back ({ .clip = clip, .pitch = 48 + i % 24, .start = at (i * 0.25), .length = lasting (0.2) });

        phazite::setClipNotes (*song, clip, notes);

        for (int i = 0; i < 256; ++i)
            phazite::addControl (*song, clip, at (i * 0.25), phazite::ControlKind::controller, 64, i % 2 == 0 ? 127 : 0);

        phazite::setSong (context, song.get());
        const phazite::ControlLane pedal { phazite::ControlKind::controller, 64, 1 };
        const auto step = [&] (double from)
        {
            phazite::setControlValues (context, clip, pedal, { at (from), at (from + 1.0) },
                                       { { clip, at (from), 64, 0 }, { clip, at (from + 0.5), 100, 0 } });
        };

        step (4.0);
        Heard heard;
        auto root = song->getState();
        root.addListener (&heard);
        song->getValues()->notes.addListener (&heard);
        const auto element = phazite::findClipState (*song, clip);
        const auto inTheBeat = [&]
        {
            const auto values = phazite::getControlValues (*song, { clip }, pedal);
            return std::count_if (values.begin(), values.end(),
                                  [] (const phazite::ControlPoint& point) { return point.beat >= at (8.0) && point.beat <= at (9.0); });
        };

        const auto removed = inTheBeat();
        const auto notesBefore = phazite::countNotesRead();
        const auto visitedBefore = phazite::SongIndex::countElementsVisited();
        step (8.0);
        const Counts counts { phazite::countNotesRead() - notesBefore, phazite::SongIndex::countElementsVisited() - visitedBefore,
                              heard.changes };
        root.removeListener (&heard);
        song->getValues()->notes.removeListener (&heard);

        // The clip's own element, not a copy of it put in its place; and what was heard, each value of the lane's in the beat
        // taken out and each drawn put in (none of them the same), nothing else.
        CHECK (phazite::findClipState (*song, clip) == element);
        CHECK (counts.changes == (int) (removed + inTheBeat()));
        phazite::setSong (context, nullptr);
        return counts;
    };

    const auto few = countFor (200), many = countFor (20000);
    INFO ("A step in a clip of 200 notes: " << few.changes << " changes, " << few.visited << " elements looked at; of 20,000 notes: "
                                            << many.changes << " changes, " << many.visited << " looked at");
    CHECK (few.notesRead == 0);
    CHECK (many.notesRead == 0);
    CHECK (few.changes == 7);                  // five values of the pedal's in the beat, both its ends in it, and two drawn
    CHECK (many.changes == few.changes);       // whatever else the clip holds
    CHECK (many.visited - few.visited <= 100); // no walk through the clip: its value is read, not its elements (3 and 3 looked
                                               // at, 2026-11-21; one walk of its 20,000 elements before the notes were values)
}

TEST_CASE ("Performance gate: a step of a note's drag in the piano roll doesn't walk the controller values its clip holds")
{
    // The lane's chooser marks the lanes the clips hold that it doesn't show, which a walk through the clips' elements finds:
    // read at each change of the song, a note dragged in a clip of a long take paid that walk at each step. It waits for the
    // drag's end.
    const auto visitedFor = [] (int numValues)
    {
        phazite::test::PianoRollScreen screen;
        std::vector<phazite::ControlValue> values;

        for (int i = 0; i < numValues; ++i)
            values.push_back ({ at (i * 8.0 / numValues), phazite::ControlKind::controller, 1, i % 128, 1 });

        phazite::addControls (screen.getSong(), screen.clip, values);
        screen.addNote (1.0);
        phazite::test::Mouse mouse (*screen.notes);
        mouse.press (screen.at (1.5));
        mouse.dragTo (screen.at (2.5));
        phazite::test::dispatchPendingMessages();
        const auto before = phazite::SongIndex::countElementsVisited();
        mouse.dragTo (screen.at (3.5));
        phazite::test::dispatchPendingMessages();
        const auto visited = phazite::SongIndex::countElementsVisited() - before;
        mouse.release();
        phazite::test::dispatchPendingMessages();

        // Once the drag is over, the chooser says what the clip holds.
        auto* chooser = dynamic_cast<juce::TextButton*> (phazite::test::findComponentWithTitle (screen.pianoRoll, "Lane: Velocity"));
        REQUIRE (chooser != nullptr);
        CHECK (chooser->getTooltip().contains ("Mod Wheel (CC 1)") == (numValues > 0));
        return visited;
    };

    // No walk through the clip's elements: its notes and values are its value, read whole (188 elements looked at with and
    // without the values, 2026-11-21; one walk, about 20,200, before the notes were values beside the tree, and the
    // chooser's read at each step made it two, 40,193).
    const auto none = visitedFor (0), many = visitedFor (20000);
    INFO ("Elements looked at by a step of a note's drag in a clip of no controller values: " << none << "; of 20,000: " << many);

    CHECK (many < none + 100); // not their difference: the counts are unsigned, and the one with values can come out the smaller
}

TEST_CASE ("Performance gate: a drag or a paste of many notes looks through its clip's track once, not once for each note")
{
    // Where a clip can grow to (the next clip on its track) was found by a walk through the track's clips for each note a
    // drag's step placed, and for each pasted note past the clip's end: 500 notes in a clip on a track of 200 clips looked at
    // 100,000 clips for each step.
    struct Visited
    {
        std::uint64_t drag = 0, paste = 0;
    };

    const auto visitedFor = [] (int clipsOnTrack)
    {
        constexpr int numNotes = 5000, numMoved = 500, numPasted = 500;
        auto song = phazite::createSong();
        phazite::ProjectContext context (phazite::test::getEngineContext());
        const auto track = phazite::ensureTracks (*song, 1).front();

        // The clip first, then the others after it, a bar apart: the next one leaves its clip room to grow into.
        const auto clipEnd = numNotes * 0.25;
        const auto clip = phazite::insertMidiClip (*song, track, { at (0.0), at (clipEnd) }, nullptr);

        for (int c = 0; c < clipsOnTrack - 1; ++c)
            REQUIRE (phazite::insertMidiClip (*song, track, { at (clipEnd + 1000.0 + c * 8.0), at (clipEnd + 1004.0 + c * 8.0) }, nullptr)
                         .isValid());

        std::vector<phazite::NoteData> notes;

        for (int i = 0; i < numNotes; ++i)
            notes.push_back ({ .clip = clip, .pitch = 48 + i % 24, .start = at (i * 0.25), .length = lasting (0.2) });

        phazite::setClipNotes (*song, clip, notes);
        phazite::setSong (context, song.get());
        notes = phazite::getNotes (context, { clip });
        notes.resize ((size_t) numMoved);
        const auto ends = phazite::getClipEnds (context, { clip });

        // As the piano roll's drag makes each step (PianoRollNoteTools.cpp: dragNotes()).
        const auto step = [&] (double by)
        {
            phazite::restoreClipEnds (context, ends);
            auto moved = notes;

            for (auto& note : moved)
                note.start = note.start + lasting (by);

            phazite::setNotes (context, moved);
        };

        Visited visited;
        step (0.25); // the first, as the song is first asked for its notes
        auto before = phazite::SongIndex::countElementsVisited();
        step (0.5);
        visited.drag = phazite::SongIndex::countElementsVisited() - before;

        // A paste past the clip's end, which grows the clip to hold the notes, as the paste makes them (Clipboard.cpp:
        // pasteNotes()).
        std::vector<phazite::NoteData> pasted;

        for (int i = 0; i < numPasted; ++i)
            pasted.push_back ({ .clip = clip, .pitch = 60, .start = at (clipEnd + i * 0.5), .length = lasting (0.25) });

        before = phazite::SongIndex::countElementsVisited();
        const auto made = phazite::replaceNotes (*song, {}, pasted);
        visited.paste = phazite::SongIndex::countElementsVisited() - before;
        CHECK (made.size() == (size_t) numPasted);
        CHECK (song->getIndex().isExact());
        phazite::setSong (context, nullptr);
        return visited;
    };

    const auto few = visitedFor (20), many = visitedFor (200);
    INFO ("Elements looked at by a step of a drag of 500 notes, with 20 clips on the track: "
          << few.drag << "; with 200: " << many.drag << ". By a paste of 500 notes past the clip's end: " << few.paste << " and "
          << many.paste);
    CHECK (few.drag > 0);
    CHECK (few.paste > 0);
    CHECK (many.drag < few.drag + 1000); // ten times the clips on the track: the same work, but a walk of them once
    CHECK (many.paste < few.paste + 1000);
}

TEST_CASE ("Performance gate: the piano roll's hover and selection rectangle find notes among those it paints, not in the song")
{
    // Known bug 122: the mouse's every move over the notes read every note of the clips shown, and the other tracks' notes,
    // from the song; and each step of a rectangle read them all again, then compared each with every note chosen.
    phazite::test::PianoRollScreen screen;
    auto& song = screen.getSong();
    constexpr int count = 500;
    const auto middle = screen.middlePitch();
    phazite::trimClipEnd (song, screen.clip, at (count * 0.25));
    const auto ghostClip = phazite::insertMidiClip (song, screen.tracks[1], { at (0.0), at (count * 0.25) }, nullptr);

    for (const auto clip : { screen.clip, ghostClip })
    {
        std::vector<phazite::NoteData> notes;

        for (int i = 0; i < count; ++i)
            notes.push_back ({ .clip = clip, .pitch = middle - 6 + i % 12, .start = at (i * 0.25), .length = lasting (0.2) });

        phazite::setClipNotes (song, clip, notes);
    }

    phazite::test::deliverChanges();
    phazite::test::Mouse mouse (*screen.notes);
    const auto y = screen.yOf (middle + 10); // above the notes: no note there, so ghosts are looked for too
    mouse.moveTo ({ 10, y });                // the first: what it reads is kept, as a paint keeps it

    auto before = phazite::countNotesRead();

    for (int i = 1; i <= 10; ++i)
        mouse.moveTo ({ 10 + i * 30, y + (i % 2) * screen.rowHeight() * 12 });

    const auto hover = phazite::countNotesRead() - before;

    // A rectangle across them, a step at a time: each selects more of them.
    before = phazite::countNotesRead();
    const auto comparedBefore = phazite::Selection::getNoteComparisons();
    mouse.press ({ 2, y }, juce::ModifierKeys::commandModifier);

    for (int i = 1; i <= 10; ++i)
    {
        mouse.dragTo ({ 2 + i * (screen.notes->getWidth() - 4) / 10, screen.yOf (middle - 8) });
        phazite::test::deliverChanges();
    }

    mouse.release();
    const auto band = phazite::countNotesRead() - before;
    const auto compared = phazite::Selection::getNoteComparisons() - comparedBefore;
    const auto selected = (std::uint64_t) screen.getSelected().size();

    INFO ("Notes read by 10 moves of the mouse: " << hover << "; by 10 steps of a rectangle: " << band << ", which selected " << selected
                                                  << " with " << compared << " comparisons");
    CHECK (selected > 10);
    CHECK (hover == 0);

    // What the rectangle's steps read are the notes selected (which what asks whether there are any finds there still), not
    // the clip's: all ten of them less than the clip's notes once (measured 2026-11-15: 362 of 500, and 9,181 before).
    CHECK (band < (std::uint64_t) count);
    CHECK (compared < selected * 20); // each note chosen once, not compared with every other
    CHECK (song.getIndex().isExact());
}

//==============================================================================
TEST_CASE (
    "Performance gate: the count of changes since the last commit compares nothing while nothing changed, and one track for an edit of it")
{
    // Known bug 126: the activity bar's badge asks for the changes each time the versions say anything (four times a
    // second while a bounce renders); a plug-in's own change compared every track of the song again.
    const phazite::test::TestPlugin plugin;
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 32, 16.0);
    auto& song = getSong (s);
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add Device");
        device = phazite::test::TestPlugin::add (song, built.tracks[3]);
    }

    REQUIRE (phazite::test::waitForVersionHistory (s.context));

    // Committed as Source Control's Commit is (version.save's gate): once the plug-ins that said they changed, the one just
    // added among them, have given their states.
    std::optional<juce::Result> result;
    s.context.withPluginStates ([&s, &result] (bool)
                                { REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; })); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    REQUIRE (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 0);
    auto* history = phazite::getVersionHistory (s.context);

    // Told again and again, nothing changed: nothing compared.
    auto before = phazite::countElementsCompared();

    for (int i = 0; i < 20; ++i)
    {
        history->tellNow();
        CHECK (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 0);
    }

    CHECK (phazite::countElementsCompared() == before);

    // What the whole song's comparison looks at, against which the others are measured.
    before = phazite::countElementsCompared();
    REQUIRE (phazite::findChanges (history->getLastVersionSong(), phazite::test::copyWholeSong (song)).empty());
    const auto wholeSong = phazite::countElementsCompared() - before;

    // The count is the whole program's: what this history and those that went still compare on their threads is in first (a
    // whole song's comparison landed in the note's count once, in a whole run on a busy machine).
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    phazite::test::waitForVersionsWrites();

    // A note added: its track compared again, not the song.
    before = phazite::countElementsCompared();
    phazite::addNote (s.context, built.clips[0], 72, at (2.5), lasting (0.25), 100);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 1);
    const auto noteAdded = phazite::countElementsCompared() - before;

    // A plug-in's own change, in its window: its track compared again, not the song.
    before = phazite::countElementsCompared();
    song.markChangedOutsideHistory (device.getRaw());
    phazite::test::dispatchPendingMessages();
    phazite::countChanges (phazite::test::withChangesIn (s.context));
    const auto pluginChanged = phazite::countElementsCompared() - before;

    INFO ("Elements compared: the whole song " << wholeSong << ", a note added " << noteAdded << ", a plug-in's own change "
                                               << pluginChanged);
    CHECK (noteAdded > 0);
    CHECK (noteAdded * 8 < wholeSong);
    CHECK (pluginChanged > 0);
    CHECK (pluginChanged * 8 < wholeSong);
    CHECK (song.getIndex().isExact());
}

TEST_CASE ("Performance gate: an edit has the versions copy and compare the one track it changed, on the history's thread, never "
           "on the message thread")
{
    // S4.5 (docs/design/versions.md: Off the message thread): the changes since the last version are worked out on the
    // history's thread from copies of what the song's notifications said changed. An edit copies its track alone, compares
    // it alone, and the message thread compares nothing, however many tracks the song has.
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 32, 16.0);
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    REQUIRE (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 0);

    struct Counts
    {
        juce::int64 copied, compared, onMessageThread;
    };

    const auto count = []
    {
        return Counts { phazite::ChangesWorker::countTracksCopied(), phazite::ChangesWorker::countTracksCompared(),
                        phazite::countElementsComparedOnMessageThread() };
    };

    // A version just saved of the song as it is: no changes, known without a round.
    auto before = count();
    phazite::countChanges (phazite::test::withChangesIn (s.context));
    auto after = count();
    CHECK (after.copied == before.copied);
    CHECK (after.compared == before.compared);

    // A note added to one clip, then to another track's: each its track, once.
    for (const auto edit : { 0, 1 })
    {
        CAPTURE (edit);
        before = count();
        phazite::addNote (s.context, built.clips[edit == 0 ? 0 : 3], 72, at (2.5), lasting (0.25), 100);
        phazite::test::dispatchPendingMessages();
        CHECK (phazite::countChanges (phazite::test::withChangesIn (s.context)) == edit + 1);
        after = count();
        CHECK (after.copied - before.copied == 1);
        CHECK (after.compared - before.compared == 1);
        CHECK (after.onMessageThread == before.onMessageThread);
    }

    // Told again and again, nothing changed: nothing copied or compared.
    before = count();

    for (int i = 0; i < 10; ++i)
    {
        phazite::getVersionHistory (s.context)->tellNow();
        phazite::countChanges (phazite::test::withChangesIn (s.context));
    }

    after = count();
    CHECK (after.copied == before.copied);
    CHECK (after.compared == before.compared);
    CHECK (after.onMessageThread == before.onMessageThread);
}

TEST_CASE ("Performance gate: a drag in a clip of 50,000 notes copies its track for the versions once, as it is committed")
{
    // The review of S4.5: rounds of the changes ran back to back during a drag, each copying the dragged clip's whole track on
    // the message thread. None runs while a gesture is open; one, after it.
    phazite::test::TestSong s;
    const auto clip = s.addMidiClip (s.firstTrack, 0.0, 64.0);

    {
        // Its notes written into the song as a file holds them, opened as a project is.
        auto state = phazite::test::copyWholeSong (*phazite::findSong (s.context));
        juce::ValueTree clipState;

        for (const auto& track : phazite::getTrackElements (state))
            for (const auto& child : track)
                if (child.hasType (phazite::schema::CLIP) && phazite::readID (child[phazite::schema::clipID]) == clip.getRaw())
                    clipState = child;

        REQUIRE (clipState.isValid());

        for (int i = 0; i < 50000; ++i)
            clipState.appendChild (juce::ValueTree (phazite::schema::NOTE, { { phazite::schema::noteID, (juce::int64) i + 1 },
                                                                             { phazite::schema::notePitch, 36 + i % 48 },
                                                                             { phazite::schema::noteStart, i * 0.00125 },
                                                                             { phazite::schema::noteLength, 0.001 },
                                                                             { phazite::schema::noteVelocity, 100 } }),
                                   nullptr);

        s.openSong (state);
    }

    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 0);

    // A note of it dragged through many moves, the badge and the view asking all along.
    auto& song = *phazite::findSong (s.context);
    const auto copied = phazite::ChangesWorker::countTracksCopied();

    {
        phazite::Gesture drag (song, "Move Notes");
        const auto note = phazite::NoteRef { clip, phazite::NoteID::fromRaw (1) };
        REQUIRE (phazite::getNoteData (song, note).has_value());

        for (int i = 0; i < 60; ++i)
        {
            phazite::moveNote (song, note, at (0.01 * (i + 1)), 36);
            phazite::test::dispatchPendingMessages();
            phazite::countChanges (s.context);
        }

        CHECK (phazite::ChangesWorker::countTracksCopied() == copied);
        drag.commit();
    }

    CHECK (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 1);
    CHECK (phazite::ChangesWorker::countTracksCopied() - copied == 1);
}

TEST_CASE ("Performance gate: the check for unsaved changes compares no element after a notification that changes nothing, and a "
           "note's clip after a note added and taken back")
{
    // The title's star and Auto Save ask after every notification: a track's notification compared every track of the song,
    // each value written as text (22 ms on 16,384 notes, 2026-11-16; Benchmarks.cpp, "the UI's work on a big song").
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 16, 64.0);
    auto& song = getSong (s);
    auto& project = s.getProject();
    REQUIRE (project.save().wasOk());
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (project.hasUnsavedChanges());

    const std::function<int (const juce::ValueTree&)> countElements = [&countElements] (const juce::ValueTree& element)
    {
        auto count = 1;

        for (const auto& child : element)
            count += countElements (child);

        return count;
    };

    // Told again and again, nothing changed: no element compared.
    auto before = phazite::Project::countElementsCompared();

    for (int i = 0; i < 20; ++i)
    {
        phazite::findTrackState (song, built.tracks[5]).sendPropertyChangeMessage (phazite::schema::trackName);
        CHECK_FALSE (project.hasUnsavedChanges());
    }

    CHECK (phazite::Project::countElementsCompared() == before);

    // A note added and taken back (Undo): its clip's notes compared as values (WholeState.h: holdSameNotes()), and no element
    // of the tree, nor the song.
    {
        const phazite::UndoStep step (song, "Add Note");
        REQUIRE (phazite::test::addMidiNote (song, built.clips[3], 72, at (2.5), lasting (0.25), 100).isValid());
    }

    phazite::undo (song);
    phazite::test::dispatchPendingMessages();
    before = phazite::Project::countElementsCompared();
    CHECK_FALSE (project.hasUnsavedChanges());
    const auto noteAdded = phazite::Project::countElementsCompared() - before;
    const auto clip = countElements (phazite::copyWhole (song, phazite::findClipState (song, built.clips[3])));
    const auto wholeSong = countElements (phazite::test::copyWholeSong (song));

    INFO ("Elements compared after a note added and taken back: " << noteAdded << ", of its clip's " << clip << " and the song's "
                                                                  << wholeSong);
    CHECK (noteAdded == 0); // the clip's elements compared before the notes were values beside the tree
    CHECK (noteAdded <= clip);
    CHECK (noteAdded * 8 < wholeSong);
}

TEST_CASE ("Performance gate: the loader's checks of a song read from its text make no allocation for each note")
{
    // makeConsistent() goes through every element and number of a song as it opens (a project, a version's pieces joined):
    // its checks made a list of each element's properties, a set's node for each note's ID and a text for each number it
    // compared, several allocations a note (2026-11-16: a project of 40 tracks opened in 62 ms, 28 before). What is left is
    // for each track, clip and device, which a song has far fewer of than notes. With what a song holds many of besides:
    // controllers, automation points, warp markers, and aliases of clips.
    auto made = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*made, 8);
    auto notes = 0;

    for (const auto track : tracks)
    {
        const auto clip = phazite::test::addMidiClip (*made, track, { at (0.0), at (128.0) }, "Notes");

        for (int step = 0; step < 500; ++step, ++notes)
            phazite::test::addMidiNote (*made, clip, 48 + step % 24, at (step * 0.25), lasting (0.2), 100);

        for (int step = 0; step < 100; ++step)
            phazite::addControl (*made, clip, at (step * 0.5), phazite::ControlKind::controller, 1, step);

        const auto aliases = phazite::copyClips (*made, { clip }, true);
        REQUIRE (aliases.size() == 1);
        phazite::moveClip (*made, aliases.front(), at (128.0), track);

        const phazite::ParameterRef volume { phazite::getFader (*made, track), phazite::getVolumeParameterID (*made, track) };

        for (int point = 0; point < 100; ++point)
            REQUIRE (phazite::addPoint (*made, volume, { at (point * 2.0), (point % 10) / 10.0f }) >= 0);
    }

    phazite::test::TemporaryFolder temporary;
    const auto sound = phazite::test::writeSound (temporary.folder.getChildFile ("Loop.wav"), 4.0);
    const auto audio = phazite::test::addAudioClip (*made, tracks.front(), "Loop", sound, 300.0, 4.0);
    REQUIRE (audio.isValid());

    for (int marker = 1; marker < 20; ++marker)
        REQUIRE (phazite::addWarpMarker (*made, audio, marker * 0.2));

    // As a project file holds it: every value text.
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (*made));
    const auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    phazite::Song song (juce::ValueTree::fromXml (*xml));
    const auto allocations = phazite::test::countAllocations ([&song] { phazite::makeConsistent (song); });

    if (! allocations.has_value())
        return; // nothing counted on this build

    // Fewer than one for every ten of the elements a song holds many of: its notes, its controllers, its automation's
    // points and its warp markers, an alias's included.
    const std::function<int (const juce::ValueTree&)> countMany = [&countMany] (const juce::ValueTree& element)
    {
        auto count = 0;

        for (const auto& child : element)
            count += (child.hasType (phazite::schema::NOTE) || child.hasType (phazite::schema::CONTROL)
                              || child.hasType (phazite::schema::POINT) || child.hasType (phazite::schema::WARP)
                          ? 1
                          : 0)
                     + countMany (child);

        return count;
    };

    const auto many = countMany (phazite::test::copyWholeSong (song));
    INFO ("Allocations: " << *allocations << " for " << many << " notes, controllers, points and warp markers (" << notes << " notes)");
    CHECK (*allocations * 10 < many);

    // And Phazite's own song is left as it was written.
    juce::MemoryOutputStream after;
    phazite::writeProjectText (after, phazite::test::copyWholeSong (song));
    CHECK (after.toString() == text.toString());
}

TEST_CASE ("Performance gate: an import, Normalize and Quantize Audio read none of the audio on the message thread")
{
    phazite::test::TestSong s;
    auto& song = getSong (s);

    // Ten seconds of clicks, and a file of the same name in the project already, the same: the import compares the two.
    const auto clicks = phazite::test::writeClicks (s.getSoundsFolder(), 20, 120.0);
    const auto audioFolder = s.getProject().getAudioFolder();
    REQUIRE (audioFolder.createDirectory());
    REQUIRE (clicks.copyFileTo (audioFolder.getChildFile (clicks.getFileName())));

    const auto before = phazite::AudioWork::getMessageThreadReads();
    REQUIRE (phazite::importFilesInBackground (s.context, { clicks.getFullPathName() }, s.firstTrack, at (0.0)));
    phazite::test::waitForAudioWork (s.context);
    const auto clips = phazite::getClips (s.context, s.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto clip = clips[0].id;

    phazite::normalizeClip (s.context, clip);
    phazite::test::waitForAudioWork (s.context);
    phazite::quantizeAudio (s.context, { clip }, lasting (1.0));
    phazite::test::waitForAudioWork (s.context);

    // Each made its change, and the message thread read none of the audio for them (known bug 123).
    CHECK (phazite::getAudioClip (s.context, clip)->gain > 0.0f);
    CHECK_FALSE (phazite::getWarpMarkers (s.context, clip).empty());
    CHECK (phazite::AudioWork::getMessageThreadReads() == before);

    // What the gate counts: a peak read at once, on the message thread, counts its bytes.
    REQUIRE (phazite::getClipPeak (song, clip).has_value());
    CHECK (phazite::AudioWork::getMessageThreadReads() > before);
}

TEST_CASE ("Performance gate: a track's controllers' index holds a place for each change, however many controllers change")
{
    // Known bug 85's review: the index of the controllers' values that the chase reads kept every controller's value at a
    // mark every 64 changes, so it grew with the changes times the controllers (90 MB for a long take with dense pressure
    // on one track), made again at each note edited on it. Now each controller's changes are a list of places among the
    // events: 120,000 changes cost as much, spread over one controller or sixty.
    using Type = phazite::devices::NoteEvent::Type;
    const auto bytesFor = [] (int controllers)
    {
        std::vector<phazite::engine::NoteSequence::Control> controls;

        for (int i = 0; i < 120000; ++i)
            controls.push_back ({ i * 0.01, Type::controller, i % controllers, (float) (i % 128) / 127.0f });

        return phazite::engine::NoteSequence ({ { 0.0, 1200.0, 60, 0.8f } }, std::move (controls)).getBytes();
    };

    const auto one = bytesFor (1), sixty = bytesFor (60);
    CAPTURE (one);
    CAPTURE (sixty);
    CHECK (sixty <= one + 60 * 64);                                              // a few bytes for each controller more
    CHECK (one <= 120000 * (sizeof (phazite::engine::NoteSequence::Event) + 8)); // the events, and a place for each
}

TEST_CASE (
    "Performance gate: a branch switch, a whole version's restore, the start of a merge and an open that offers the last version read "
    "no song on the message thread, nor split or write one")
{
    // Known bug 126: a switch read two songs on the message thread (the branch's and its last commit's), and the start of a
    // merge two (the commit both lines have, and the other's), keeping the window for a second and more in a large song.
    // Now they are read on the history's thread, and only what they read is taken in on the message thread.
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 4, 8.0);
    auto& song = getSong (s);
    const auto commit = [&s]
    {
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        std::optional<juce::Result> result;
        REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
        REQUIRE (phazite::test::waitForVersionHistory (s.context));
        return phazite::getLastVersion (s.context)->id;
    };

    commit();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    REQUIRE (phazite::newBranch (s.context, {}, s.save));
    const auto idea = phazite::getCurrentBranch (s.context)->id;
    phazite::setChannelVolume (song, built.tracks[1], -9.0f);
    const auto theirs = commit();
    const auto before = phazite::VersionStore::countSongsRead();
    const auto splitsBefore = phazite::countSongsSplit();
    const auto writesBefore = phazite::VersionStore::countSongsWritten();

    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));
    phazite::setChannelVolume (song, built.tracks[2], -3.0f);
    commit();
    phazite::setChannelVolume (song, built.tracks[3], -2.0f); // a change left on main as it switches
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, idea, s.save); }));
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));
    CHECK (phazite::getChannelVolume (song, built.tracks[3]) == doctest::Approx (-2.0f)); // the change left there, back
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::bringBackVersion (s.context, theirs); }));
    phazite::undo (song);
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
    CHECK_FALSE (phazite::getMergeLines (s.context).empty());

    // And Merge, which keeps the song before it in the store for Cancel Merge: kept as its list was worked out, on the
    // history's thread (the second review of known bug 126).
    phazite::finishMerge (s.context);
    CHECK (phazite::isMerging (s.context));

    const auto after = phazite::VersionStore::countSongsRead();
    INFO ("Songs read: " << after.all - before.all << ", on the message thread " << after.onMessageThread - before.onMessageThread);
    CHECK (after.all > before.all);
    CHECK (after.onMessageThread == before.onMessageThread);

    // Nor do they split or write a song there (the song left on the branch, kept as it switches).
    CHECK (phazite::countSongsSplit().onMessageThread == splitsBefore.onMessageThread);
    CHECK (phazite::VersionStore::countSongsWritten().onMessageThread == writesBefore.onMessageThread);
    CHECK (phazite::VersionStore::countSongsWritten().all > writesBefore.all);

    // Nor does an open whose project file can't be read, which offers the branch's last commit (known bug 184): the project
    // copied with its history as it is now, its file damaged, opened in a session of its own.
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    phazite::test::waitForVersionsWrites();
    phazite::test::TemporaryFolder elsewhere;
    const auto folder = song.getFile().getParentDirectory();
    REQUIRE (folder.copyDirectoryTo (elsewhere.folder.getChildFile (folder.getFileName())));
    const auto damaged = elsewhere.folder.getChildFile (folder.getFileName()).getChildFile (song.getFile().getFileName());
    REQUIRE (damaged.replaceWithText ("<PHAZITE damaged"));

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    phazite::ProjectSession session (phazite::test::getEngineContext(), recent, elsewhere.folder,
                                     elsewhere.folder.getChildFile ("Unsaved"));
    juce::String offered;
    session.askWhichCopy =
        [&offered] (const juce::String&, const juce::String&, const juce::String& commitOffered, std::function<void (int)> answer)
    {
        offered = commitOffered;
        answer (0);
    };
    const auto openBefore = phazite::VersionStore::countSongsRead();
    session.openProject (damaged);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&offered] { return offered.isNotEmpty(); }));
    CHECK (phazite::VersionStore::countSongsRead().onMessageThread == openBefore.onMessageThread);
    CHECK (phazite::VersionStore::countSongsRead().all > openBefore.all);
}

//==============================================================================
namespace
{
// Each device the engine plays now (the plan's strips' own, no container here), by its node: its name and how many blocks it
// has worked, those it slept through not counted (engine::DeviceRunner::getBlocksWorked()).
struct Worked
{
    juce::String name;
    std::uint64_t blocks = 0;
};

std::map<const phazite::engine::DeviceNode*, Worked> countBlocksWorked()
{
    std::map<const phazite::engine::DeviceNode*, Worked> worked;

    for (const auto& strip : getPlan().strips)
        for (const auto& device : strip.devices)
            worked[device.node.get()] = { juce::String (std::string (device.node->getDeviceInfo().name)),
                                          device.node->getRunner().getBlocksWorked() };

    return worked;
}
} // namespace

TEST_CASE ("Performance gate: a stopped song's devices work no block once their sound has ended, each of Phazite's own")
{
    // A stopped song of 20 tracks of a synth, an EQ, a compressor and a reverb cost 7.5 % of a core in its first seconds
    // (docs/performance.md): each reverb, which nothing had come into, played its two seconds of tail from silence as the
    // project opened (stopped, a device that has heard nothing since it was reset has nothing to ring with). Counted here,
    // not timed: the blocks
    // each device works, which once it rests (engine/nodes/DeviceRunner.h) are none. Every one of Phazite's own devices
    // rests; Age, whose noise plays with the song, rests while it is stopped (devices::Processor::playsWithTheSong()).
    // A plug-in never does (it may play with nothing coming in), nor is it counted here.
    phazite::test::TestSong s;

    // A track for each of Phazite's own devices: an instrument alone, an effect after a synth, each with a chord in its first
    // beat; and a reverb whose mix an LFO moves, which rests all the same (its knobs show the modulation: known bug 171).
    const auto addChord = [&s] (phazite::TrackID track) { s.addChord (s.addMidiClip (track, 0.0, 4.0), { 48, 55, 64 }, 0.0, 1.0); };

    for (const auto* info : phazite::getOwnDevices())
    {
        const auto track = s.addTrack (juce::String (std::string (info->name)));
        const std::string type (info->typeID);

        if (info->kind == dev::DeviceKind::effect)
        {
            s.setInstrument (track, dev::Synth::typeID);
            s.addEffect (track, type.c_str());
        }
        else
        {
            s.setInstrument (track, type.c_str());
        }

        addChord (track);
    }

    const auto modulated = s.addTrack ("Modulated reverb");
    s.setInstrument (modulated, dev::Synth::typeID);
    const auto reverb = s.addEffect (modulated, "phaziteReverb");
    s.modulate (s.addModulator (modulated, phazite::ModulatorType::lfo), { reverb, "mix" }, -0.5f);
    addChord (modulated);

    phazite::test::EnginePlayback playback;
    const auto blockSeconds = playback.getBlockSize() / playback.getSampleRate();

    // Whether each device worked no block between two counts.
    const auto checkRested = [] (const std::map<const phazite::engine::DeviceNode*, Worked>& before, const juce::String& when)
    {
        for (const auto& [node, now] : countBlocksWorked())
        {
            const auto found = before.find (node);
            REQUIRE (found != before.end());
            const auto blocks = now.blocks - found->second.blocks;
            INFO (now.name << " " << when << ": " << blocks << " blocks worked in a second");
            CHECK (blocks == 0);
        }
    };

    // As the song opens, nothing played: every device rests within a quarter of a second (DeviceRunner::sleepAfterSeconds),
    // a reverb's and a delay's long tails too, as nothing came into them to ring with.
    playback.play (0.5);
    const auto opened = countBlocksWorked();
    playback.play (1.0);
    checkRested (opened, "as the song opens");

    // Played, then stopped as the chord ends (its beat, at 120 beats a minute): each rests once its sound has ended, a reverb's
    // tail and a delay's echoes heard out. A quarter of a second at a time, the last time each worked noted, until none has
    // worked for two seconds (or a minute has gone).
    s.run ("transport.playStop");
    playback.play (0.5);
    s.run ("transport.playStop");
    REQUIRE_FALSE (phazite::isPlaying (s.context));

    auto last = countBlocksWorked();
    std::map<const phazite::engine::DeviceNode*, double> workedUntil;
    auto seconds = 0.0, quietFor = 0.0;

    while (quietFor < 2.0 && seconds < 60.0)
    {
        playback.play (0.25);
        seconds += 0.25;
        quietFor += 0.25;
        const auto now = countBlocksWorked();

        for (const auto& [node, worked] : now)
        {
            if (worked.blocks != last[node].blocks)
            {
                workedUntil[node] = seconds;
                quietFor = 0.0;
            }
        }

        last = now;
    }

    // Each device's type, and after how long it rested: the longest of those of its type, to a block and a quarter of a second.
    juce::String report ("\nAfter Stop, as its chord ended, each device rested after (seconds, the longest of its kind):");
    std::map<juce::String, double> restedAfter;

    for (const auto& [node, worked] : last)
        restedAfter[worked.name] = std::max (restedAfter[worked.name], workedUntil[node] + blockSeconds);

    for (const auto& [name, after] : restedAfter)
        report << "\n" << name << ": " << juce::String (after, 2);

    MESSAGE (report);
    const auto ended = countBlocksWorked();
    playback.play (1.0);
    checkRested (ended, "after Stop, its sound ended");
}
