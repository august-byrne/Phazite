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
#include "TestDeviceValues.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/ProjectSession.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
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
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/ModulationValues.h"
#include "model/modulation/internal/Modulation.h"
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
#include "ui/components/Controls.h"
#include "ui/mixer/ChannelStrip.h"
#include "ui/mixer/MixerView.h"
#include "ui/playlist/TrackHeader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
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
          [&] { phazite::test::storePluginState (song, device, juce::String ("c3RhdGU=")); } },
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
            phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);

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
        onlyThese ([&] { phazite::test::addOwnDevice (*song, tracks[2], dev::Delay::typeID); }, { tracks[2] });
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

TEST_CASE ("Performance gate: a tick of a device's knob, a chain's volume or a drum pad's field reads no device whole, nor do a "
           "strip's slots, however many tracks the song has")
{
    // While the song's tree holds its devices (roadmap step S5.20, the devices' first piece), a reader of one field of one
    // device reads that field from the device's element alone, through the conversion's readers of one field
    // (model/base/internal/DevicesValue.h), never the device whole: a drum sampler's whole value holds thousands of entries.
    // On songs of 4 tracks and of 40, each with an instrument and an effect, the last with a drum sampler, an equalizer, five
    // delays and a container: a tick of the equalizer's first knob, of the container's chain's volume and of a pad's field,
    // and the eight slots of its strip. Counted: the devices read into whole values (countDevicesReadAsValues()); the walks
    // of the song that find a device's element (countDeviceWalks()), none at a tick, as a control's handle keeps the
    // parameter it found (model/devices/ParameterHandle.cpp, find()), as before the devices' values; and what the value's
    // reading allocates at a tick, none (the parameter's name taken as its reference holds it, as before).
    using dev::DrumSampler;

    struct Tick
    {
        std::uint64_t whole = 0, walks = 0;
        std::optional<int> allocations {};       // the value's reading at a tick (getValueSetByHand()), where counted
        std::optional<int> handleAllocations {}; // the handle's (ParameterHandle::getBaseValue()), the value's reading among them

        bool operator== (const Tick&) const = default;
    };

    const auto countFor = [] (int numTracks)
    {
        auto song = phazite::createSong();
        const auto tracks = phazite::ensureTracks (*song, numTracks);

        for (const auto track : tracks)
        {
            phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
            phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
        }

        const auto last = tracks.back();
        const auto drums = phazite::test::setOwnInstrument (*song, last, DrumSampler::typeID);
        const auto equalizer = phazite::getEffects (*song, last).front();

        for (int i = 0; i < 5; ++i)
            phazite::test::addOwnDevice (*song, last, dev::Delay::typeID);

        const auto container = phazite::addContainer (*song, last);
        REQUIRE (container.isValid());
        const auto chain = phazite::getChains (*song, container).front();
        REQUIRE (phazite::getDevices (*song, last).size() == 8);

        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setSong (context, song.get());

        // A control's tick (its attachment's update()), and the value's reading alone by a handle of its own to the same
        // parameter, which has found it once.
        const auto countTick = [&song] (const std::function<phazite::ParameterHandle()>& makeHandle)
        {
            auto handle = makeHandle();
            REQUIRE (handle.isValid());
            phazite::ui::Knob knob ("Knob", 0.0);
            phazite::ui::ParameterAttachment attachment (knob);
            attachment.setParameter (std::move (handle));
            attachment.update(); // its first tick finds the parameter
            Tick tick;
            const auto whole = phazite::countDevicesReadAsValues();
            const auto walks = phazite::countDeviceWalks();
            attachment.update();
            tick.whole = phazite::countDevicesReadAsValues() - whole;
            tick.walks = phazite::countDeviceWalks() - walks;

            const auto reader = makeHandle();
            auto sum = reader.getBaseValue(); // finds the parameter
            tick.handleAllocations = phazite::test::countAllocations ([&reader, &sum] { sum += reader.getBaseValue(); });
            const auto found = phazite::locateParameter (*song, reader.getRef());
            REQUIRE (found.has_value());
            tick.allocations =
                phazite::test::countAllocations ([&song, &found, &sum] { sum += phazite::getValueSetByHand (*song, *found); });
            CHECK (std::isfinite (sum));
            return tick;
        };

        const auto padField = DrumSampler::info.parameters[(size_t) DrumSampler::getParameterIndex (40, (DrumSampler::Field) 1)].id;
        const auto knob =
            countTick ([&] { return phazite::makeDeviceHandle (*song, equalizer).getParameter (dev::Equalizer::info.parameters[0].id); });
        const auto chainVolume = countTick ([&] { return phazite::getChainVolume (context, chain); });
        const auto pad = countTick ([&] { return phazite::makeDeviceHandle (*song, drums).getParameter (padField); });

        const auto before = phazite::countDevicesReadAsValues();
        const auto instrument = phazite::getInstrument (context, last);
        const auto effects = phazite::getEffects (context, last);
        const auto slots = phazite::countDevicesReadAsValues() - before;
        CHECK (instrument.has_value());
        CHECK (effects.size() == 7);
        setSong (context, nullptr);
        Tick strip;
        strip.whole = slots;
        return std::array { knob, chainVolume, pad, strip };
    };

    const auto small = countFor (4);
    const auto big = countFor (40);

    const std::array<std::string, 4> names { "a knob's tick", "a chain's volume's tick", "a pad's field's tick", "a strip's eight slots" };

    for (size_t i = 0; i < names.size(); ++i)
        MESSAGE (names[i] << ": devices read whole " << small[i].whole << " (4 tracks), " << big[i].whole << " (40 tracks); walks of "
                          << "the song " << small[i].walks << ", " << big[i].walks << "; allocations of the value's reading "
                          << small[i].allocations.value_or (-1) << ", " << big[i].allocations.value_or (-1) << ", of the handle's "
                          << small[i].handleAllocations.value_or (-1) << ", " << big[i].handleAllocations.value_or (-1));

    CHECK (big == small);

    // Measured 2026-11-22: no device read whole, each field read from the element alone; no walk of the song at a tick, as
    // before the devices' values (the handle keeps what it found); no allocation for the value's reading, as before them
    // (the parameter's name taken as its reference holds it, never made a text); and one for the handle's reading around
    // it, the same for each kind of parameter, whose code the devices' values didn't change. The strip's slots read none
    // whole either.
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK (small[i].whole == 0);
        CHECK (small[i].walks == 0);

        if (small[i].allocations.has_value())
            CHECK (*small[i].allocations == 0);

        if (small[i].handleAllocations.has_value())
            CHECK (*small[i].handleAllocations <= 1);
    }

    CHECK (small[3].whole == 0);
}

TEST_CASE ("Performance gate: the loader's rule for the devices allocates nothing for a song Phazite wrote, a full drum sampler's "
           "thousands of values among its devices")
{
    // The loader makes each device, container and chain hold what the song holds of it, as Phazite writes it
    // (model/base/internal/DevicesValue.h: keepDeviceAsHeld()): it looks through an element's properties, its VALUES and
    // its SOUNDS as they are, and reads it whole and writes it again only where one isn't as the writer writes it. A song
    // Phazite wrote, with a drum sampler of every field and sound set (3,329 values, 1,024 sounds), 40 tracks with an
    // instrument and an effect each, and a container: the rule over every element of it as it is read from its text.
    using dev::DrumSampler;
    auto made = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*made, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*made, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*made, track, dev::Equalizer::typeID);
    }

    const auto container = phazite::addContainer (*made, tracks.front());
    REQUIRE (container.isValid());
    phazite::test::OwnValues values;

    for (const auto& info : DrumSampler::info.parameters)
    {
        // A value off its default: a choice's next, a number a quarter of its range on (or back).
        auto value =
            dev::constrain (info, info.defaultValue + (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

        if (phazite::isDefaultValue (info, value))
            value =
                dev::constrain (info, info.defaultValue - (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

        values.emplace_back (juce::String (info.id.data(), info.id.size()), value);
    }

    const auto drumsTrack = phazite::insertPlainTrack (*made, tracks.back());
    const auto drums = phazite::test::setOwnInstrument (*made, drumsTrack, DrumSampler::typeID, values);

    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
    {
        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
        {
            phazite::test::writeDeviceSoundByHand (*made, drums, phazite::schema::getPadSound (pad, layer), "808 Kick");
            phazite::test::writeDeviceSoundByHand (*made, drums, phazite::schema::getPadSoundFile (pad, layer),
                                                   "Audio/Pad " + juce::String (pad) + " " + juce::String (layer) + ".wav");
        }
    }

    // Opened once and saved: a file Phazite wrote.
    const auto opened = phazite::makeSong (phazite::test::copyWholeSong (*made));
    REQUIRE (opened != nullptr);
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (*opened));
    const auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    const auto state = juce::ValueTree::fromXml (*xml);

    std::vector<juce::ValueTree> elements;
    const std::function<void (const juce::ValueTree&)> collect = [&collect, &elements] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (phazite::schema::CLIP) || child.hasType (phazite::schema::AUTOMATION))
                continue;

            if (child.hasType (phazite::schema::DEVICE) || child.hasType (phazite::schema::CONTAINER)
                || child.hasType (phazite::schema::CHAIN))
                elements.push_back (child);

            collect (child);
        }
    };

    collect (state);
    REQUIRE (elements.size() >= 83);
    auto changed = false;
    const auto allocations = phazite::test::countAllocations (
        [&elements, &changed]
        {
            for (const auto& element : elements)
                changed = phazite::keepDeviceAsHeld (element) || changed;
        });

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run

    CHECK_FALSE (changed);

    // What the rule cost before it looked through an element first: every one of Phazite's own devices read whole and
    // written again.
    const auto before = phazite::test::countAllocations (
        [&elements]
        {
            for (const auto& element : elements)
                if (const auto held = phazite::readHeldDevice (element, phazite::readDevicePlace (element));
                    held.has_value() && held->form == phazite::DeviceValue::Form::own)
                    phazite::writeDeviceElement (*held);
        });

    MESSAGE ("The loader's rule over " << elements.size() << " devices, containers and chains: " << *allocations
                                       << " allocations; read whole and written again, as before: " << before.value_or (-1));

    // Measured 2026-11-22: none.
    CHECK (*allocations == 0);
}

TEST_CASE ("Performance gate: the loader's rule allocates nothing for a send Phazite wrote")
{
    // The loader makes every send as the song holds it (SendsValue.h: keepSendAsHeld()), each of a song's: one it wrote needs
    // no change, which the rule finds by reading the send's numbers and comparing its properties' texts, nothing allocated.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 10);
    std::vector<phazite::TrackID> returns;

    for (const auto* name : { "A", "B", "C", "D" })
        returns.push_back (phazite::addReturnTrack (*song, name, nullptr));

    for (const auto track : tracks)
    {
        for (const auto returnTrack : returns)
        {
            const auto send = phazite::addSend (*song, track, returnTrack, returnTrack == returns.front());
            phazite::setSendLevel (*song, send, -12.3456f);
        }
    }

    // Saved and read as a file is.
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (*song));
    const auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    const auto state = juce::ValueTree::fromXml (*xml);
    std::vector<juce::ValueTree> elements;
    const std::function<void (const juce::ValueTree&)> collect = [&collect, &elements] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (phazite::schema::SEND))
                elements.push_back (child);

            collect (child);
        }
    };

    collect (state);
    REQUIRE (elements.size() == 40);
    auto changed = false;
    const auto allocations = phazite::test::countAllocations (
        [&elements, &changed]
        {
            for (const auto& element : elements)
                changed = phazite::keepSendAsHeld (element) || changed;
        });

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run

    CHECK_FALSE (changed);
    MESSAGE ("The loader's rule over " << elements.size() << " sends: " << *allocations << " allocations");

    // Measured 2026-11-22: none.
    CHECK (*allocations == 0);
}

TEST_CASE ("Performance gate: a tick of a send's level control reads no send from an element, and allocates as much however many "
           "tracks and sends the song has")
{
    // A send's knob shows its level at each of the display ticker's ticks. Its handle, having found the send's level once,
    // checks that the send is still there and reads its level, each a look in the sends' area by the send's ID
    // (model/devices/internal/Parameters.h: FoundParameter), never a walk of the song's tracks; a strip's knob, whose send may
    // not be there yet, also finds it in its track's list first (model/mixer/MixerModel.h: getSendLevelTo(); Mixer.h:
    // findSendFromTo()). Counted as sends read into values from an element (SendsValue.h: countSendsRead()), none, as the
    // looks in the sends' area (countSendLooks()), and as the allocations of the tick, on songs of 4 tracks and of 40, each
    // sending to 4 returns, for the last track's send to the third return: a strip's knob and a knob of the send by its ID.
    struct Tick
    {
        std::uint64_t read = 0, looks = 0;
        std::optional<int> allocations {};
    };

    const auto countFor = [] (int numTracks)
    {
        auto song = phazite::createSong();
        const auto tracks = phazite::ensureTracks (*song, numTracks);
        std::vector<phazite::TrackID> returns;

        for (const auto* name : { "A", "B", "C", "D" })
            returns.push_back (phazite::addReturnTrack (*song, name, nullptr));

        std::vector<phazite::DeviceID> lastSends;

        for (const auto track : tracks)
        {
            lastSends.clear();

            for (const auto returnTrack : returns)
                lastSends.push_back (phazite::addSend (*song, track, returnTrack, false));
        }

        REQUIRE (lastSends.size() == 4);
        REQUIRE (lastSends[2].isValid());
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setSong (context, song.get());

        const auto countTick = [] (phazite::ParameterHandle handle)
        {
            REQUIRE (handle.isValid());
            phazite::ui::Knob knob ("Send level", 0.0);
            phazite::ui::ParameterAttachment attachment (knob);
            attachment.setParameter (std::move (handle));
            attachment.update(); // its first tick finds the send's level
            Tick tick;
            const auto before = phazite::countSendsRead();
            const auto looksBefore = phazite::countSendLooks();
            attachment.update();
            tick.read = phazite::countSendsRead() - before;
            tick.looks = phazite::countSendLooks() - looksBefore;
            tick.allocations = phazite::test::countAllocations ([&attachment] { attachment.update(); });
            return tick;
        };

        const auto counts = std::pair (countTick (phazite::getSendLevelTo (context, tracks.back(), returns[2])),
                                       countTick (phazite::getSendLevel (context, lastSends[2])));
        setSong (context, nullptr);
        return counts;
    };

    const auto [smallStrip, smallByID] = countFor (4);
    const auto [bigStrip, bigByID] = countFor (40);
    MESSAGE ("Sends read by a tick: a strip's knob "
             << smallStrip.read << " (4 tracks), " << bigStrip.read << " (40 tracks); by its ID " << smallByID.read << ", " << bigByID.read
             << "; allocations: a strip's knob " << smallStrip.allocations.value_or (-1) << ", " << bigStrip.allocations.value_or (-1)
             << "; by its ID " << smallByID.allocations.value_or (-1) << ", " << bigByID.allocations.value_or (-1)
             << "; looks: a strip's knob " << smallStrip.looks << ", " << bigStrip.looks << "; by its ID " << smallByID.looks << ", "
             << bigByID.looks);

    // Measured 2026-11-22, the sends' switch: none read from an element (21 by a strip's knob, 5 by its ID, before, when the
    // tree held the sends); each look in the area.
    CHECK (smallStrip.read == 0);
    CHECK (bigStrip.read == 0);
    CHECK (smallByID.read == 0);
    CHECK (bigByID.read == 0);

    // And the looks in the sends' area (SendsValue.h: countSendLooks()), as many however many tracks and sends the song has,
    // which a walk of them would not be. Measured 2026-11-22: 5 by its ID, a look by the send's ID each time the attachment's
    // tick asks the handle for its parameter (its enabling, its modulation's range, whether it may set its value, its value:
    // ParameterHandle.cpp, find(), isStillThere()), and one for its level (Parameters.cpp: getValueSetByHand()); 9 by a strip's
    // knob, which also finds its send in its track's list at each of those four asks (Mixer.h: findSendFromTo()).
    CHECK (bigStrip.looks == smallStrip.looks);
    CHECK (bigByID.looks == smallByID.looks);
    CHECK (smallStrip.looks <= 9);
    CHECK (smallByID.looks <= 5);

    if (! smallStrip.allocations.has_value() || ! bigStrip.allocations.has_value() || ! smallByID.allocations.has_value()
        || ! bigByID.allocations.has_value())
        return;

    CHECK (*bigStrip.allocations == *smallStrip.allocations);
    CHECK (*bigByID.allocations == *smallByID.allocations);

    // Measured 2026-11-22: one each, none the sends' code's: the empty std::vector of modulations that the knob's ring asks for
    // (model/modulation/internal/Modulation.h: findModulationsOf(), from ModulationModel.h's getModulationRange()), whose
    // iterator-debugging proxy the Debug build of MSVC's standard library allocates (where a build doesn't, none).
    CHECK (*smallStrip.allocations <= 1);
    CHECK (*smallByID.allocations <= 1);
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

        // Three notes added one after the other, each counted, and the least of the three taken: what making the song left
        // for later (a timer that waits for quiet, the rest of the playlist's components) runs inside one of them at most, as
        // the machine has it, and isn't an edit's work (CI's Clang job once counted 5,134 for the big song where the others
        // count a few hundred). All three are in the message, so that work every edit does shows as three large counts.
        std::string counted;

        const auto countFor = [&] (int numTracks)
        {
            phazite::test::PlaylistScreen screen;
            const auto clips = playlistOf (screen, numTracks);
            int least = std::numeric_limits<int>::max();

            for (int i = 0; i < 3; ++i)
            {
                const auto count = phazite::test::countAllocations (
                                       [&]
                                       {
                                           phazite::addNote (screen.context, clips[1][1], 72 + i, at (2.5), lasting (0.25), 100);
                                           phazite::test::deliverChanges();
                                       })
                                       .value_or (0);
                counted += (counted.empty() ? "" : ", ") + std::to_string (numTracks) + " tracks: " + std::to_string (count);
                least = std::min (least, count);
            }

            return least;
        };

        const auto small = countFor (20), big = countFor (200);
        CAPTURE (counted);
        MESSAGE ("Allocations, a note added, 20 tracks / 200 tracks of 24 clips: " << small << " / " << big << " (" << counted << ")");

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

TEST_CASE ("Performance gate: selecting automation points and finding them by their keys compares keys as often as the points, not "
           "their square")
{
    // Each selected point was looked for through the whole curve, and selecting them compared each with every other: a
    // drawn lane of 10,000 points selected cost about 10^8 comparisons each time a lane painted. Counted as the selected
    // points' keys compared (Selection.h: getPointComparisons()), as they are selected and found again in their curve.
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

TEST_CASE ("Performance gate: bending several selected points of a curve reads the curve as often however many they are")
{
    // An edit of a curve takes its value from the curves' area (Automation.cpp: SongCurve): one for each point bent read the
    // whole curve for each, 1,000 points of a curve of 10,000 bent reading 10,000,000 points. Counted as the points read into
    // values or compared one by one (CurveValues.h: countCurvePointsRead()): the area's setter compares the curve once with
    // the one it held, to know whether it changed.
    constexpr auto numPoints = 1000;

    const auto pointsReadFor = [] (int numBent)
    {
        phazite::test::TestSong song;
        auto& document = *phazite::findSong (song.context);
        const auto volume = song.getVolumeParameter (song.firstTrack);
        std::vector<phazite::ParameterPoint> points;

        for (int i = 0; i < numPoints; ++i)
            points.push_back ({ volume, { phazite::BeatPosition::fromBeats (i * 0.25), (float) (i % 2) * 0.5f } });

        phazite::addAutomationPoints (document, points);
        std::vector<int> bent;

        for (int i = 0; i < numBent; ++i)
            bent.push_back (i * 10);

        const auto before = phazite::countCurvePointsRead();
        phazite::setAutomationCurves (song.context, volume, bent, 0.5f);
        const auto read = phazite::countCurvePointsRead() - before;
        const auto after = phazite::getPoints (document, volume);
        REQUIRE (after.size() == (size_t) numPoints);
        CHECK (after[(size_t) bent.back()].curve == doctest::Approx (0.5f));
        return read;
    };

    // The curve compared once by the edit's set, whether 4 points are bent or 40 (twice before the curves were values: read
    // by the edit, and read back by Debug's check of what it wrote).
    const auto small = pointsReadFor (4), large = pointsReadFor (40);
    INFO ("4 points bent: " << small << " points read; 40: " << large);
    CHECK (large == small);
    CHECK (small <= (std::uint64_t) numPoints);
}

TEST_CASE ("Performance gate: asking whether each selected section is selected reads no value of the song's sections for each")
{
    // A paint of the sections' strip and of the playlist asks it of each section in view (SectionsStrip.cpp, Playlist.cpp),
    // and the selection asks the song whether it holds that section (Selection.cpp: isThere()): a value made of every section
    // for each (SectionsValue.h: getSectionsValue()) read the square of the sections, twice a frame while the playlist follows
    // the playhead. Asked through SectionsValue.h's holdsSection(), it reads none into a value. Counted as the lists of sections
    // built from the song's value (Sections.h: countSectionListsBuilt(), none), as the sections read from the tree
    // (countSectionsRead(), which a live song never reads), and as allocations, which immer's lists of freed nodes hide from
    // the count, but which must not grow either.
    struct Counts
    {
        std::uint64_t built = 0, read = 0;
        std::optional<int> allocations;
    };

    const auto countsFor = [] (int numSections)
    {
        phazite::test::TestSong s;
        auto& song = *phazite::findSong (s.context);
        auto& selection = phazite::getSelection (s.context);
        std::vector<phazite::SectionID> ids;

        {
            const phazite::UndoStep step (song, "Sections");

            for (int i = 0; i < numSections; ++i)
                ids.push_back (phazite::insertSection (song, phazite::BeatPosition::fromBeats (i * 4.0),
                                                       { .name = "Part", .length = phazite::BeatDuration::fromBeats (4.0) }));
        }

        for (const auto& id : ids)
            selection.addToSelection (id);

        auto selected = 0;
        const auto askEach = [&ids, &selection, &selected]
        {
            for (const auto& id : ids)
                if (selection.isSelected (id))
                    ++selected;
        };

        Counts counts;
        const auto builtBefore = phazite::countSectionListsBuilt();
        const auto readBefore = phazite::countSectionsRead();
        askEach();
        counts.built = phazite::countSectionListsBuilt() - builtBefore;
        counts.read = phazite::countSectionsRead() - readBefore;
        CHECK (selected == numSections);
        counts.allocations = phazite::test::countAllocations (askEach);
        return counts;
    };

    // Ten times the sections, at most ten times the work (none, now); a hundred times, with a value read for each.
    const auto small = countsFor (4), large = countsFor (40);
    CHECK (small.built == 0);
    CHECK (large.built == 0);
    INFO ("4 sections: " << small.read << " read; 40: " << large.read);
    CHECK (large.read <= small.read * 10 + 8);

    if (small.allocations.has_value() && large.allocations.has_value()) // counted on this build (PerformanceCounts.h)
    {
        INFO ("4 sections: " << *small.allocations << " allocations; 40: " << *large.allocations);
        CHECK (*large.allocations <= *small.allocations * 10 + 8);
    }
}

TEST_CASE ("Performance gate: a modulated knob's ring and a modulator's slot read the modulators they show, however many the song "
           "holds")
{
    // A knob asks at each tick of the display ticker what modulates its parameter (ModulationModel.h: getModulationRange(), from
    // Controls.cpp's ParameterAttachment::update()), and a modulator's slot whether its modulator is on, its value and whether
    // it goes both ways (ModulatorSlots.cpp: updateValues()). Each looks in modulation's area (Modulation.h: findModulationsOf(),
    // getModulatorValue()), reading no modulator from an element: counted as the modulators read (ModulationValue.h:
    // countModulatorsRead()), none for 4 modulators on the track as for 40. And the channel's slots, each with its name
    // (ModulationModel.h: getModulators(); Modulation.h, getModulatorName(), which counts each slot's place among its kind's
    // by the area's list), none either.
    struct Counts
    {
        std::uint64_t knob = 0, slot = 0, still = 0, slots = 0;
    };

    const auto countsFor = [] (int numModulators)
    {
        phazite::test::TestSong s;
        const auto track = s.addTrack ("Keys");
        const auto synth = s.setInstrument (track, dev::Synth::typeID);
        const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
        s.modulate (lfo, { synth, "cutoff" }, 0.5f);

        for (int i = 1; i < numModulators; ++i)
            s.addModulator (track, i % 2 == 0 ? phazite::ModulatorType::random : phazite::ModulatorType::macro);

        const auto cutoff = phazite::getParameter (s.context, { synth, "cutoff" });
        const auto resonance = phazite::getParameter (s.context, { synth, "resonance" });
        Counts counts;

        auto before = phazite::countModulatorsRead();
        CHECK (phazite::getModulationRange (cutoff).has_value());
        counts.knob = phazite::countModulatorsRead() - before;

        before = phazite::countModulatorsRead();
        CHECK_FALSE (phazite::getModulationRange (resonance).has_value()); // a knob nothing modulates, as most are
        counts.still = phazite::countModulatorsRead() - before;

        before = phazite::countModulatorsRead();
        CHECK (phazite::isModulatorOn (s.context, lfo));
        CHECK (phazite::isBipolar (s.context, lfo));
        juce::ignoreUnused (phazite::getModulatorValue (s.context, lfo));
        counts.slot = phazite::countModulatorsRead() - before;

        before = phazite::countModulatorsRead();
        CHECK (phazite::getModulators (s.context, track).size() == (size_t) numModulators);
        counts.slots = phazite::countModulatorsRead() - before;
        return counts;
    };

    const auto small = countsFor (4), large = countsFor (40);
    INFO ("4 modulators: " << small.knob << " read for the knob, " << small.slot << " for the slot; 40: " << large.knob << ", "
                           << large.slot);
    CHECK (small.knob == 0); // looks in the area (1 while the tree held modulation: its one modulation's modulator)
    CHECK (large.knob == small.knob);
    CHECK (small.still == 0);
    CHECK (large.still == 0);
    CHECK (small.slot == 0); // looks in the area (3 while the tree held modulation: once for each of its questions)
    CHECK (large.slot == small.slot);
    // Measured 2026-11-22: none, the area looked in (6 for each slot while the tree held modulation: 24 for 4 and 240 for 40).
    INFO ("slots: 4 modulators: " << small.slots << " read; 40: " << large.slots);
    CHECK (small.slots == 0);
    CHECK (large.slots == 0);
}

TEST_CASE ("Performance gate: removing an effect allocates no more for a song of many clips than for one of few, and reads no "
           "modulator")
{
    // Every removal finds what still names something (Mixer.h: forgetWhatNamesTheGone()), the song's items' IDs among them:
    // its tree's, and those of its values that something can name (IDsHeld.h: forEachNameableIDHeld()), never a clip's (a
    // clip is no item that a curve, a MIDI link, a modulation or a send names), nor its modulation read whole while its tree
    // holds it (the tree's walk finds a modulator's and a macro's IDs). The same for 8 tracks of 10 clips as for 8 of 250.
    struct Counts
    {
        std::uint64_t modulatorsRead = 0;
        std::optional<int> allocations {};
    };

    const auto countsFor = [] (int clipsPerTrack)
    {
        phazite::test::TestSong s;
        std::vector<phazite::TrackID> tracks { s.firstTrack };

        while (tracks.size() < 8)
            tracks.push_back (s.addTrack ("Track"));

        for (const auto track : tracks)
            for (int i = 0; i < clipsPerTrack; ++i)
                s.addMidiClip (track, i * 4.0, 4.0);

        const auto synth = s.setInstrument (tracks[0], dev::Synth::typeID);
        const auto lfo = s.addModulator (tracks[0], phazite::ModulatorType::lfo);
        s.modulate (lfo, { synth, "cutoff" }, 0.5f);
        const auto macro = s.addModulator (tracks[0], phazite::ModulatorType::macro);
        s.modulate (macro, s.getVolumeParameter (tracks[0]), 0.25f);
        const auto first = s.addEffect (tracks[1], dev::Delay::typeID);
        const auto second = s.addEffect (tracks[1], dev::Delay::typeID);
        phazite::test::dispatchPendingMessages();

        Counts counts;
        const auto before = phazite::countModulatorsRead();
        phazite::removeDevice (s.context, first);
        counts.modulatorsRead = phazite::countModulatorsRead() - before;
        phazite::test::dispatchPendingMessages();
        counts.allocations = phazite::test::countAllocations ([&s, second] { phazite::removeDevice (s.context, second); });
        return counts;
    };

    const auto few = countsFor (10), many = countsFor (250);
    INFO ("8 tracks of 10 clips: " << few.allocations.value_or (-1) << " allocations; of 250: " << many.allocations.value_or (-1));
    CHECK (few.modulatorsRead == 0);
    CHECK (many.modulatorsRead == 0);

    if (! few.allocations.has_value() || ! many.allocations.has_value())
        return; // nothing counted on this build, where the counted removal didn't run (PerformanceCounts.h)

    CHECK (*many.allocations <= *few.allocations + 8); // 127 and 127, measured 2026-11-22
}

TEST_CASE ("Performance gate: a macro turned and a modulator's value set read none of it whole, and a track duplicated no more "
           "for a song of many modulators")
{
    // A control's drag sets a macro's value or a modulator's parameter at the mouse's rate, through its handle, which found the
    // parameter once (ParameterHandle.h): modulation's code looks the value up in its area and sets the one value changed
    // (Modulation.h: setMacroValue(), setModulatorValueSetByHand()), and the automation's watcher of the parameter touched last
    // finds the parameter there to leave a modulator's alone (Automation.h: AutomationWatcher). And a duplicated track's
    // modulators are given to its copy from the area's lists (Modulation.h: copyModulation()): counted as the modulators read
    // from elements (ModulationValue.h: countModulatorsRead()), none, on a song of 4 tracks of one modulator each and of 40.
    struct Counts
    {
        std::uint64_t macro = 0, modulator = 0, duplicate = 0;
    };

    const auto countsFor = [] (int numTracks)
    {
        phazite::test::TestSong s;
        std::vector<phazite::TrackID> tracks { s.firstTrack };

        while (tracks.size() < (size_t) numTracks)
            tracks.push_back (s.addTrack ("Track"));

        const auto macro = s.addModulator (tracks[0], phazite::ModulatorType::macro);
        const auto lfo = s.addModulator (tracks[1], phazite::ModulatorType::lfo);

        for (size_t i = 2; i < tracks.size(); ++i)
            s.addModulator (tracks[i], phazite::ModulatorType::random);

        auto turned = phazite::getModulatorParameter (s.context, macro, phazite::getModulatorParameters (s.context, macro).front().id);
        auto rate = phazite::getModulatorParameter (s.context, lfo, "rate");
        REQUIRE (turned.exists());
        REQUIRE (rate.exists());
        Counts counts;

        // A step of each one's drag, the gesture begun and its parameter found by its first move.
        for (const auto& [handle, first, next, count] :
             { std::tuple { &turned, 0.25f, 0.5f, &counts.macro }, std::tuple { &rate, 2.0f, 3.0f, &counts.modulator } })
        {
            handle->beginGesture();
            handle->setValue (first);
            const auto before = phazite::countModulatorsRead();
            handle->setValue (next);
            *count = phazite::countModulatorsRead() - before;
            handle->endGesture();
        }

        auto& song = *phazite::findSong (s.context);
        const auto beforeCopy = phazite::countModulatorsRead();

        {
            const phazite::UndoStep step (song, "Duplicate Track");
            REQUIRE (phazite::duplicateTracks (song, { tracks[1] }, nullptr).size() == 1);
        }

        counts.duplicate = phazite::countModulatorsRead() - beforeCopy;
        return counts;
    };

    // Measured 2026-11-22: none for both songs, modulation's area looked in. While the tree held modulation: 2, 2 and 1 (for a
    // value the keeper's reading and the watcher's, for the copy the keeper's); before its writers worked on values, 4, 4 and
    // 5 for 4 tracks, 41 for 40.
    const auto small = countsFor (4), large = countsFor (40);
    INFO ("4 tracks: " << small.macro << ", " << small.modulator << ", " << small.duplicate << "; 40: " << large.macro << ", "
                       << large.modulator << ", " << large.duplicate);
    CHECK (small.macro == 0);
    CHECK (large.macro == small.macro);
    CHECK (small.modulator == 0);
    CHECK (large.modulator == small.modulator);
    CHECK (small.duplicate == 0);
    CHECK (large.duplicate == small.duplicate);
}

TEST_CASE ("Performance gate: a macro turned allocates as much on a song of 200 tracks with a modulator each as on one of 4")
{
    // A step of a macro's drag sets its one value in modulation's area (Modulation.h: setMacroValue()): a look by its ID and a
    // new value of the area that shares all else, told once (ModulationArea.h: ModulationTeller, which compares the two by
    // identity, looking at the one entry that differs), whatever the song's count of modulators.
    const auto allocationsFor = [] (int numTracks) -> std::optional<int>
    {
        phazite::test::TestSong s;
        std::vector<phazite::TrackID> tracks { s.firstTrack };

        while (tracks.size() < (size_t) numTracks)
            tracks.push_back (s.addTrack ("Track"));

        const auto macro = s.addModulator (tracks[0], phazite::ModulatorType::macro);

        for (size_t i = 1; i < tracks.size(); ++i)
            s.addModulator (tracks[i], phazite::ModulatorType::lfo);

        auto turned = phazite::getModulatorParameter (s.context, macro, phazite::getModulatorParameters (s.context, macro).front().id);
        REQUIRE (turned.exists());
        turned.beginGesture();
        turned.setValue (0.25f);
        phazite::test::dispatchPendingMessages();
        const auto allocations = phazite::test::countAllocations ([&turned] { turned.setValue (0.5f); });
        turned.endGesture();
        return allocations;
    };

    const auto few = allocationsFor (4), many = allocationsFor (200);
    INFO ("4 tracks: " << few.value_or (-1) << " allocations; 200: " << many.value_or (-1));

    if (! few.has_value() || ! many.has_value())
        return; // nothing counted on this build, where the counted turn didn't run (PerformanceCounts.h)

    // Measured 2026-11-22: 9 and 10 (immer's map one level deeper for 200 modulators); 28 and 421 while the parameter's
    // lookup (Parameters.h: locateParameter(), which the automation's watcher asks at each step) walked the tree before
    // modulation's area.
    CHECK (*many <= *few + 2);
}

TEST_CASE ("Performance gate: an edit of a section and the playlist's paint that follows build one list of the sections for each "
           "part that draws them")
{
    // What draws the sections asks the song for them once a paint (PlaylistModel.h: getSongSections(), which builds a list of
    // every section, sorted: Sections.h, getSections()), never once for each section: the lanes behind the tracks
    // (Playlist.cpp: Playlist::Lanes::paint()) and the sections' strip in the time bar (SectionsStrip.cpp: paint()). Counted
    // for one rename of a section, its changes delivered to the views and the whole window painted once, with as many lists
    // for 40 sections as for 4: the bound is what this measured when the sections left the song's tree (roadmap step S5.20),
    // the edit's own work and the views' answers to it included.
    const auto builtFor = [] (int numSections)
    {
        phazite::test::PlaylistScreen screen;
        std::vector<phazite::SectionID> ids;

        {
            const phazite::UndoStep step (*screen.song, "Sections");

            for (int i = 0; i < numSections; ++i)
                ids.push_back (phazite::insertSection (*screen.song, at (i * 4.0), { .name = "Part", .length = lasting (4.0) }));
        }

        phazite::test::deliverChanges();
        screen.window.paintEverything (juce::SoftwareImageType());
        const auto before = phazite::countSectionListsBuilt();
        phazite::renameSection (screen.context, ids[1], "Renamed");
        phazite::test::deliverChanges();
        screen.window.paintEverything (juce::SoftwareImageType());
        return phazite::countSectionListsBuilt() - before;
    };

    const auto small = builtFor (4), large = builtFor (40);
    MESSAGE ("4 sections: " << small << " lists built; 40: " << large);
    CHECK (large == small);
    CHECK (small <= 2); // measured: the lanes' paint and the strip's, one list each; the edit and the views' answers build none
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
        const auto clipBefore = phazite::getClipValue (*song, clip);
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

        // The clip as it was (a controller's values are its notes' value's); and what was heard, each value of the lane's in the
        // beat taken out and each drawn put in (none of them the same), nothing else.
        CHECK (phazite::getClipValue (*song, clip) == clipBefore);
        CHECK (counts.changes == (int) (removed + inTheBeat()));
        phazite::setSong (context, nullptr);
        return counts;
    };

    const auto few = countFor (200), many = countFor (20000);
    INFO ("A step in a clip of 200 notes: " << few.changes << " changes, " << few.visited << " elements looked at; of 20,000 notes: "
                                            << many.changes << " changes, " << many.visited << " looked at");
    CHECK (few.notesRead == 0);
    CHECK (many.notesRead == 0);
    CHECK (few.changes == 7);            // five values of the pedal's in the beat, both its ends in it, and two drawn
    CHECK (many.changes == few.changes); // whatever else the clip holds
    CHECK (many.visited == few.visited); // no walk through the clip: its value is read, not its elements (0 and 0 looked at
                                         // once the clips were values, 3 and 3 before; one walk of its 20,000 elements before
                                         // the notes were values)
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
    const auto clip = countElements (phazite::copyWholeClip (song, built.clips[3]));
    const auto wholeSong = countElements (phazite::test::copyWholeSong (song));

    INFO ("Elements compared after a note added and taken back: " << noteAdded << ", of its clip's " << clip << " and the song's "
                                                                  << wholeSong);
    CHECK (noteAdded == 0); // the clip's elements compared before the notes were values beside the tree
    CHECK (noteAdded <= clip);
    CHECK (noteAdded * 8 < wholeSong);
}

TEST_CASE ("Performance gate: the check for unsaved changes after a clip's property was set compares that clip alone, as much "
           "for a clip of a track of 256 clips as of 2")
{
    // The clips are heard from the song's values (model/base/internal/SongValues.h: Listener::clipsChanged()), each compared
    // with the saved copy's values (WholeState.h: holdSameClip()), the clip found by its ID in each: no element of either
    // tree, and nothing of its track's other clips. Before the clips were values, finding the saved clip scanned the saved
    // track's children (2026-11-21: 257 elements looked at for a clip of the track of 256 clips here, at every edit).
    phazite::test::TestSong s;
    auto& song = getSong (s);
    auto& project = s.getProject();
    const auto few = s.firstTrack;
    const auto many = s.addTrack ("Many");
    const auto inFew = s.addMidiClip (few, 0.0, 4.0);
    s.addMidiClip (few, 4.0, 4.0);
    phazite::ClipID inMany;

    for (int i = 0; i < 256; ++i)
        if (const auto clip = s.addMidiClip (many, i * 4.0, 4.0); i == 128)
            inMany = clip;

    REQUIRE (inMany.isValid());
    REQUIRE (project.save().wasOk());
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (project.hasUnsavedChanges());

    // A clip muted and played again before anything asks: the check compares it with its saved copy, and finds it the same.
    const auto check = [&song, &project] (phazite::ClipID clip)
    {
        phazite::toggleClipsMuted (song, { clip });
        phazite::toggleClipsMuted (song, { clip });
        const auto compared = phazite::Project::countElementsCompared();
        CHECK_FALSE (project.hasUnsavedChanges());
        return phazite::Project::countElementsCompared() - compared;
    };

    check (inFew); // once before what is counted, which then counts the same work for each

    const auto inTheFew = check (inFew);
    const auto inTheMany = check (inMany);
    INFO ("Elements compared for a clip of 2: " << inTheFew << ", of 256: " << inTheMany);
    CHECK (inTheMany == inTheFew);
    CHECK (inTheMany >= 1); // the clip compared: none would be a check skipped
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

TEST_CASE ("Performance gate: a song of 40 modulators and 200 automated parameters opened reads a few of its modulators for each, "
           "not all of them for each curve")
{
    // The loader's checks look up each curve's parameter (Project.cpp, makeConsistent(): Parameters.h, locateParameter()),
    // which looks in modulation's area first; a song the loader makes consistent holds no values yet, and a look there reads
    // its tree (Modulation.h: getModulatorValue()): the one element of the ID alone, never every modulator of the song.
    // Counted as the modulators read from elements (ModulationValue.h: countModulatorsRead()), through the loader and the
    // adoption (makeSong()).
    constexpr auto numTracks = 40;
    phazite::test::TestSong s;
    std::vector<phazite::TrackID> tracks { s.firstTrack };

    while (tracks.size() < (size_t) numTracks)
        tracks.push_back (s.addTrack ("Track"));

    auto& made = getSong (s);
    auto curves = 0;

    for (const auto track : tracks)
    {
        const auto lfo = s.addModulator (track, phazite::ModulatorType::lfo);
        s.modulate (lfo, s.getPanParameter (track), 0.5f);
        std::vector<phazite::ParameterRef> automated { s.getVolumeParameter (track), s.getPanParameter (track) };

        for (const auto& parameter : phazite::getModulatorParameters (s.context, lfo))
            if (automated.size() < 5)
                automated.push_back ({ phazite::DeviceID::fromRaw (lfo.getRaw()), parameter.id });

        REQUIRE (automated.size() == 5);

        for (const auto& parameter : automated)
        {
            REQUIRE (phazite::addPoint (made, parameter, { at (0.0), 0.25f }) >= 0);
            REQUIRE (phazite::addPoint (made, parameter, { at (4.0), 0.75f }) >= 0);
            ++curves;
        }
    }

    REQUIRE (curves == 200);
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (made));
    const auto xml = juce::parseXML (text.toString());
    REQUIRE (xml != nullptr);
    const auto state = juce::ValueTree::fromXml (*xml);

    const auto before = phazite::countModulatorsRead();
    const auto opened = phazite::makeSong (state);
    const auto read = phazite::countModulatorsRead() - before;
    REQUIRE (opened != nullptr);
    CHECK (phazite::getModulationValue (*opened).modulators.size() == (size_t) numTracks);

    // Measured 2026-11-22: 240 in a Debug build, 6 for each modulator: one for each curve of its own 3 parameters, the
    // loader's keeping of it as held, the adoption's and the adoption's Debug assertion's. 8,120 while a look in a song
    // without values read its whole modulation, all 40 for each curve.
    INFO ("Modulators read: " << read << " for " << numTracks << " modulators and " << curves << " curves");
    CHECK (read <= 8 * (std::uint64_t) numTracks);
}

TEST_CASE ("Performance gate: a drawn lane of 10,000 points taken into the song's values as it opens makes far fewer allocations "
           "than points")
{
    // Roadmap step S5.20, automation's switch: the song takes its file's curves into the curves' area as it holds its values
    // (Song::holdValues(): CurvesArea.h, readHeldCurves()), each point given a key, the points put into the value's leaves
    // many at a time.
    constexpr auto numPoints = 10000;
    auto state = phazite::test::copyWholeSong (*phazite::createSong());
    const phazite::ParameterRef volume { phazite::DeviceID::fromRaw (2), "master volume" };
    juce::ValueTree curve (phazite::schema::CURVE,
                           { { phazite::schema::curveDevice, "2" }, { phazite::schema::curveParameter, volume.parameter } });

    for (int i = 0; i < numPoints; ++i)
        curve.appendChild (juce::ValueTree (phazite::schema::POINT, { { phazite::schema::pointBeat, i * 0.25 },
                                                                      { phazite::schema::pointValue, (float) (i % 2) * 0.5f } }),
                           nullptr);

    juce::ValueTree automation (phazite::schema::AUTOMATION);
    automation.appendChild (curve, nullptr);
    state.appendChild (automation, nullptr);
    phazite::Song song (state);
    const auto allocations = phazite::test::countAllocations ([&song] { song.holdValues(); });

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run: the song holds no values

    REQUIRE (phazite::getPoints (song, volume).size() == (size_t) numPoints);

    // Measured (2026-11-22, Windows, Debug): 1,338 allocations for 10,000 points (the value's leaves and nodes, the elements
    // taken out of the tree, the other areas' adoption): fewer than one for five points.
    MESSAGE ("Allocations: " << *allocations << " for " << numPoints << " points");
    CHECK (*allocations * 5 < numPoints);
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
