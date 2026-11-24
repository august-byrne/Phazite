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
#include "GoldenFiles.h"
#include "PerformanceCounts.h"
#include "SongCheck.h"
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
#include "devices/reverb/Reverb.h"
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
#include "model/base/internal/DeviceStoreValue.h"
#include "model/base/internal/DevicesArea.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SavedValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SendsValue.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/TracksArea.h"
#include "model/base/internal/TracksValue.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/internal/EngineContext.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/ModulationValues.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/plugins/internal/DeviceStore.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ApplySong.h"
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
        { "a device's window moved", [&] { phazite::setWindowPlace (song, equalizer, { .x = 40, .y = 60 }); }, 0 },
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
          [&] { phazite::setWindowPlace (song, phazite::getEffects (s.context, track)[0].id, { .x = 10, .y = 20 }); } },
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

        // Every strip up to date first: one still due from the mixer's making (its ticker not yet run, as on a loaded
        // machine or after another test) would take the change's refresh into it.
        for (auto* strip : phazite::test::findAll<phazite::ui::ChannelStrip> (mixer))
            REQUIRE (phazite::test::runTimersUntil ([strip] { return ! strip->isRefreshDue(); }));

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
    // A reader of one field of one device reads it from the device's value in the devices' area (roadmap step S5.20, the
    // devices' switch: Mixer.h, findDevice(), a pointer into the area's value), never the device read whole into a value
    // anew: a drum sampler's whole value holds thousands of entries.
    // On songs of 4 tracks and of 40, each with an instrument and an effect, the last with a drum sampler, an equalizer, five
    // delays and a container: a tick of the equalizer's first knob, of the container's chain's volume and of a pad's field,
    // and the eight slots of its strip. Counted: the devices read into whole values (countDevicesReadAsValues()); the looks
    // in the devices' area by an ID (countDeviceLooks()), as many at a tick on 40 tracks as on 4; and what the value's
    // reading allocates at a tick, none (the parameter's name taken as its reference holds it, as before).
    using dev::DrumSampler;

    struct Tick
    {
        std::uint64_t whole = 0, looks = 0;
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
            const auto looks = phazite::countDeviceLooks();
            attachment.update();
            tick.whole = phazite::countDevicesReadAsValues() - whole;
            tick.looks = phazite::countDeviceLooks() - looks;

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
        MESSAGE (names[i] << ": devices read whole " << small[i].whole << " (4 tracks), " << big[i].whole << " (40 tracks); looks in "
                          << "the devices' area " << small[i].looks << ", " << big[i].looks << "; allocations of the value's reading "
                          << small[i].allocations.value_or (-1) << ", " << big[i].allocations.value_or (-1) << ", of the handle's "
                          << small[i].handleAllocations.value_or (-1) << ", " << big[i].handleAllocations.value_or (-1));

    CHECK (big == small);

    // Measured 2026-11-22: no device read whole, each field read from the element alone; no walk of the song at a tick, as
    // before the devices' values (the handle keeps what it found); no allocation for the value's reading, as before them
    // (the parameter's name taken as its reference holds it, never made a text); and one for the handle's reading around
    // it, the same for each kind of parameter, whose code the devices' values didn't change. The strip's slots read none
    // whole either. Measured 2026-11-23 after the devices' switch (the walks of the song gone, looks in the area counted
    // instead): a knob's tick 10 looks, a chain's volume's 5, a pad's field's 10, on 4 tracks and on 40; no device read
    // whole, and no allocation for the value's reading nor for the handle's.
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK (small[i].whole == 0);
        CHECK (small[i].looks <= 10);

        if (small[i].allocations.has_value())
            CHECK (*small[i].allocations == 0);

        if (small[i].handleAllocations.has_value())
            CHECK (*small[i].handleAllocations <= 1);
    }

    CHECK (small[3].whole == 0);
}

TEST_CASE ("Performance gate: a tick of a plug-in's control and a drag event look its device up as often however many tracks the "
           "song has")
{
    // The devices' store (model/plugins/internal/DeviceStore.h) reads and writes a plug-in's values set by hand by its
    // device's ID, a look in the store's area since its switch, and the device in the devices' area since theirs (Mixer.h:
    // findDevice(), counted: countDeviceLooks()), so that neither a tick nor a drag looks more often on a song of 40 tracks
    // than on one of 4. On songs of 4 tracks and of 40, the
    // stand-in plug-in (TestPlugin.h) on the last: a tick of its Gain's knob (its attachment's update(): enabled, the
    // modulation's range, its base value) and a handle's reading of its value, base value and text, before and after a value
    // set by hand; and the events of a drag of it through its handle inside a gesture: the first set (which makes the song's
    // value, holding what the plug-in held), another, and one to its default (which takes the song's value away).
    const phazite::test::TestPlugin testPlugin;

    struct Counts
    {
        std::uint64_t tickBefore = 0, tickAfter = 0;               // a tick, before and after a value set by hand
        std::uint64_t firstSet = 0, anotherSet = 0, atDefault = 0; // a drag's events

        bool operator== (const Counts&) const = default;
    };

    const auto countFor = [] (int numTracks)
    {
        phazite::test::TestSong s;
        auto& song = getSong (s);
        auto track = s.firstTrack;

        for (int i = 1; i < numTracks; ++i)
            track = s.addTrack ("Track " + juce::String (i + 1));

        phazite::DeviceID plugin;

        {
            const phazite::UndoStep step (song, "Add Device");
            plugin = phazite::test::TestPlugin::add (song, track);
        }

        REQUIRE (phazite::findPluginInstance (song, plugin) != nullptr);
        const phazite::ParameterRef gain { plugin, phazite::test::TestPlugin::gainID };

        const auto countTick = [&song, &gain]
        {
            phazite::ui::Knob knob ("Knob", 0.0);
            phazite::ui::ParameterAttachment attachment (knob);
            attachment.setParameter (phazite::makeParameterHandle (song, gain));
            attachment.update(); // its first tick finds the parameter
            const auto reader = phazite::makeParameterHandle (song, gain);
            auto sum = reader.getValue(); // finds the parameter
            const auto looks = phazite::countDeviceLooks();
            attachment.update();
            sum += reader.getValue() + reader.getBaseValue() + (float) reader.getText (reader.getBaseValue()).length();
            const auto looked = phazite::countDeviceLooks() - looks;
            CHECK (std::isfinite (sum));
            return looked;
        };

        Counts counts;
        counts.tickBefore = countTick();

        auto handle = phazite::makeParameterHandle (song, gain);
        REQUIRE (handle.exists());
        const auto atDefault = handle.getDefaultValue();
        REQUIRE (atDefault.has_value());
        handle.beginGesture();

        const auto countEvent = [&handle] (float value)
        {
            const auto looks = phazite::countDeviceLooks();
            handle.setValue (value);
            return phazite::countDeviceLooks() - looks;
        };

        counts.firstSet = countEvent (0.3f);
        counts.anotherSet = countEvent (0.4f);
        REQUIRE (phazite::findPluginValue (song, plugin, gain.parameter).has_value());
        counts.tickAfter = countTick();
        counts.atDefault = countEvent (*atDefault);
        CHECK_FALSE (phazite::findPluginValue (song, plugin, gain.parameter).has_value());
        handle.endGesture();
        return counts;
    };

    const auto small = countFor (4);
    const auto big = countFor (40);

    MESSAGE ("A plug-in's control's looks in the devices' area, on 4 tracks and on 40: a tick before a value set by hand "
             << small.tickBefore << ", " << big.tickBefore << "; after " << small.tickAfter << ", " << big.tickAfter
             << "; a drag's first set " << small.firstSet << ", " << big.firstSet << "; another " << small.anotherSet << ", "
             << big.anotherSet << "; one to its default " << small.atDefault << ", " << big.atDefault);

    CHECK (big == small);

    // Before the devices' store (by the committed code): model/devices/Parameters.cpp's plug-in cases read and wrote the
    // VALUE of the element the found parameter holds, ParameterHandle's find() walks nothing while that element is still
    // the song's (isStillThere()), and PluginInstances::takeValueFromSong() read the element its tree callback names: no
    // walk at a tick. At each event of the drag one walk, the automation's watcher's, which finds the parameter set by hand
    // by its reference (model/automation/internal/Automation.h: AutomationWatcher, locateParameter()), the store's
    // functions none. Measured 2026-11-23 after the devices' switch (looks in the area counted instead of walks): a tick 8
    // looks before a value set by hand and after it, each event of the drag 2, on 4 tracks and on 40.
    CHECK (small.tickBefore <= 8);
    CHECK (small.tickAfter <= 8);
    CHECK (small.firstSet <= 2);
    CHECK (small.anotherSet <= 2);
    CHECK (small.atDefault <= 2);
}

TEST_CASE ("Performance gate: a plug-in's report from its window and a drag event of its control compare as many of the devices' "
           "store's entries on a song of 40 plug-ins as on one of 4")
{
    // The store's setters (model/base/internal/SongValues.h: storeOutsideHistory(), setDeviceStore()) decide whether a write
    // changes the song's entries (a held value alone is no change), which every report from a plug-in's window and every
    // event of a drag of a plug-in's control asks. Counted: the entries the store's comparisons looked at
    // (DeviceStoreValue.h: countStoreEntriesCompared()). Songs of 4 and of 40 stand-in plug-ins (TestPlugin.h), each holding
    // 50 values set by hand (its gain and 49 parameters it hasn't, stored as its instance stores the values automation
    // moves); on the last of them: one report of its gain, as its instance hears it from its window (onParameterChanged),
    // and one event of a drag of its gain's control in a gesture, after the drag's first.
    const phazite::test::TestPlugin testPlugin;

    struct Counts
    {
        std::uint64_t report = 0, drag = 0;

        bool operator== (const Counts&) const = default;
    };

    const auto countFor = [] (int numPlugins)
    {
        phazite::test::TestSong s;
        auto& song = getSong (s);
        std::vector<phazite::DeviceID> plugins;

        {
            const phazite::UndoStep step (song, "Add Devices");

            for (int i = 0; i < numPlugins; ++i)
                plugins.push_back (phazite::test::TestPlugin::add (song, s.firstTrack));
        }

        s.waitForPlugins();

        for (const auto plugin : plugins)
        {
            phazite::StoredPlugin stored;
            stored.values.push_back ({ .parameter = phazite::test::TestPlugin::gainID, .value = 0.5f });

            for (int i = 0; i < 49; ++i)
                stored.values.push_back ({ .parameter = juce::String (100 + i), .value = 0.25f });

            phazite::storePluginOutsideHistory (song, plugin, stored);
        }

        const phazite::ParameterRef gain { plugins.back(), phazite::test::TestPlugin::gainID };
        auto* instance = phazite::findPluginInstance (song, gain.device);
        REQUIRE (instance != nullptr);
        REQUIRE (instance->onParameterChanged != nullptr);
        const auto index = instance->findParameter (gain.parameter);
        REQUIRE (index >= 0);

        Counts counts;
        auto compared = phazite::countStoreEntriesCompared();
        instance->onParameterChanged (index, 0.75f);
        counts.report = phazite::countStoreEntriesCompared() - compared;
        CHECK (phazite::findPluginValue (song, gain.device, gain.parameter) == std::optional (0.75f));

        auto handle = phazite::makeParameterHandle (song, gain);
        REQUIRE (handle.exists());
        handle.beginGesture();
        handle.setValue (0.3f);
        compared = phazite::countStoreEntriesCompared();
        handle.setValue (0.4f);
        counts.drag = phazite::countStoreEntriesCompared() - compared;
        handle.endGesture();
        CHECK (phazite::findPluginValue (song, gain.device, gain.parameter) == std::optional (0.4f));
        return counts;
    };

    const auto small = countFor (4);
    const auto big = countFor (40);

    MESSAGE ("The devices' store's entries compared, on 4 plug-ins of 50 values each and on 40: a report from a plug-in's window "
             << small.report << ", " << big.report << "; a drag event " << small.drag << ", " << big.drag);

    // Measured 2026-11-23: while the store's == walked every plug-in's values both ways, 24 on 4 plug-ins and 495 on 40, each;
    // compared by the entries that differ (immer's diff), the one that changed: 1 and 1.
    CHECK (big == small);
    CHECK (small.report <= 1);
    CHECK (small.drag <= 1);
}

TEST_CASE ("Performance gate: a container editor's refresh looks as often for each chain, however many chains it has")
{
    // The editor's chains (model/mixer/ContainerModel.h: getChains(), one ChainInfo for each) read each chain by its ID, a
    // look in the devices' area each time (countDeviceLooks(); a walk of the song's tree before the devices' switch), a fixed
    // number of them for each chain: whether it is heard (isChainHeard()) reads its container's other chains' solos in the
    // store by their IDs, never each chain by its ID again (which made the refresh look as the square of its chains).
    // Containers of 2, 4 and 8 chains, split into layers, with nothing in their chains, one of 8 with a chain soloed.
    phazite::test::TestSong s;
    auto& song = getSong (s);
    std::vector<phazite::DeviceID> containers;

    for (const auto numChains : { 2, 4, 8, 8 })
    {
        const auto track = s.addTrack ("Track " + juce::String ((int) containers.size() + 2));
        const auto container = phazite::addContainer (s.context, track);
        REQUIRE (container.isValid());

        while ((int) phazite::getChains (song, container).size() < numChains)
            REQUIRE (phazite::addChain (s.context, container).isValid());

        containers.push_back (container);
    }

    phazite::setChainSoloed (song, phazite::getChains (song, containers.back())[3], true);

    const auto countRefresh = [&s] (phazite::DeviceID container)
    {
        const auto looks = phazite::countDeviceLooks();
        const auto chains = phazite::getChains (s.context, container);
        const auto looked = phazite::countDeviceLooks() - looks;
        CHECK (! chains.empty());
        return looked;
    };

    const auto two = countRefresh (containers[0]);
    const auto four = countRefresh (containers[1]);
    const auto eight = countRefresh (containers[2]);
    const auto soloed = countRefresh (containers[3]);
    const auto perChain = (four - two) / 2;

    MESSAGE ("A container editor's refresh looks in the devices' area " << two << " times for 2 chains, " << four << " for 4, " << eight
                                                                        << " for 8, " << soloed << " for 8 with one soloed; " << perChain
                                                                        << " for each chain");

    // As many for each chain whatever their number, and a chain soloed looks no more. The committed code before the devices'
    // store walked 12 times for each chain (makeChainInfo(): its name, its container, its part (whether its container plays
    // an instrument, its chains, its split), its mute, its solo, whether it is heard, its keys, its velocities, its
    // instrument and its devices, each by its ID once) and once for the container's chains: 97 for 8 chains. Since the
    // store's switch its mute and its solo are looks in the store (model/plugins/internal/DeviceStore.h), no walk: 10 for
    // each chain, 81 for 8. Measured 2026-11-23 after the devices' switch, its walks looks in the devices' area: the same
    // counts, 10 for each chain, 81 for 8.
    CHECK (eight - four == 4 * perChain);
    CHECK (soloed == eight);
    CHECK (perChain <= 10);
    CHECK (eight <= 8 * 10 + 1);
}

TEST_CASE ("Performance gate: a store of a song's plug-ins, and opening it, look each plug-in up no more often than the song was "
           "walked before the devices' store")
{
    // What the plug-ins' instances write into the song (model/plugins/internal/PluginInstances.h: store(), its answers) and
    // read as they open (open(), setUp()), through the devices' store (model/plugins/internal/DeviceStore.h) on the element
    // they hold, which they found once: a look in the devices' area for each plug-in a store writes (countDeviceLooks(); a
    // walk of the song before the devices' switch), none for its answer, and none as one begins opening, as before the
    // store. The looks of a song with 3 stand-in plug-ins (TestPlugin.h) and of one with 1: a store of all of them, as a save
    // stores them (Song::storeDeviceStates()); and the song saved and opened again, each plug-in's looks the difference
    // between the two songs' over two.
    const phazite::test::TestPlugin testPlugin;

    struct Counts
    {
        std::uint64_t store = 0, reopen = 0, loading = 0;
    };

    const auto countFor = [] (int numPlugins)
    {
        phazite::test::TestSong s;
        auto& song = getSong (s);

        for (int i = 0; i < numPlugins; ++i)
        {
            const phazite::UndoStep step (song, "Add Device");
            phazite::test::TestPlugin::add (song, s.firstTrack);
        }

        // Every state asked for and stored once, as a save does, so that the store counted writes nothing that differs.
        const auto storeAll = [&song]
        {
            auto in = false;
            phazite::whenPluginStatesIn (song, true, [&in] (bool) { in = true; });
            REQUIRE (phazite::test::dispatchMessagesUntil ([&in] { return in; }));
        };

        storeAll();
        song.storeDeviceStates();
        storeAll();

        Counts counts;
        auto looks = phazite::countDeviceLooks();
        song.storeDeviceStates();
        counts.store = phazite::countDeviceLooks() - looks;
        storeAll();

        // Saved (its plug-ins' states asked for and stored), opened again and its plug-ins begun opening (sync()): all on
        // this thread, nothing else running meanwhile. Then their loading in the background, which the message loop brings
        // in, beside the versions' timer, which looks for each plug-in still loading at each of its ticks (VersionHistory:
        // takePluginsJoined()): a count that depends on how long they load, shown alone.
        looks = phazite::countDeviceLooks();
        s.reopen (false);
        phazite::getSession (s.context)->update(); // its plug-ins begin opening (PluginInstances::sync())
        counts.reopen = phazite::countDeviceLooks() - looks;
        looks = phazite::countDeviceLooks();
        REQUIRE (phazite::test::dispatchMessagesUntil ([&s] { return ! phazite::getSession (s.context)->getPlugins().isLoading(); }));
        counts.loading = phazite::countDeviceLooks() - looks;
        return counts;
    };

    const auto one = countFor (1);
    const auto three = countFor (3);
    const auto opening = (three.reopen - one.reopen) / 2;

    MESSAGE ("A store of 3 plug-ins looks in the devices' area " << three.store << " times (of 1, " << one.store << "); saving and opening "
                                                                 << "them " << three.reopen << " (1, " << one.reopen << "): " << opening
                                                                 << " for each plug-in; loading them " << three.loading << " (1, "
                                                                 << one.loading << ")");

    // Before the devices' store (the committed code): a store walked once for each plug-in (store() finds its element,
    // which write() wrote into), 3 for 3; saving and opening, twice for each: once as the save stored it (its answer written
    // into the element held for it), once as the versions, made with the song, asked which plug-ins load
    // (getPluginsLoading()), and none as sync() opened it (open() read the element sync() found). As each loads:
    // finishOpening() finds it, once, and setUp() reads that element. Measured 2026-11-23 after the devices' switch, the
    // walks looks in the devices' area: the same counts, 3 for 3, 1 for 1, 2 for each plug-in saved and opened.
    CHECK (three.store == 3);
    CHECK (one.store == 1);
    CHECK (opening == 2);
}

TEST_CASE ("Performance gate: the plan after a device's knob moved on a song of 40 tracks reads that channel's devices one field at a "
           "time, none whole")
{
    // The plan's compile reads what it plays of the song's devices from the devices' area (Mixer.h: getDevicesValue(),
    // findDevice(); roadmap step S5.20, the devices' switch), never a device read whole into a value anew: a drum sampler's
    // whole value holds thousands of entries.
    // On a song of 40 tracks, each with an instrument and an effect, the last with a drum sampler, an equalizer and a
    // container with a delay in its chain: the plan after the equalizer's knob moved, which reads the values of that
    // channel's three devices again (SongPlayer::countDevicesRead()) and no device whole (countDevicesReadAsValues()),
    // looks a device up in the area no more often than the song was walked before (countDeviceLooks(): none), and allocates no
    // more than before.
    using dev::DrumSampler;
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    }

    const auto last = tracks.back();
    phazite::test::setOwnInstrument (*song, last, DrumSampler::typeID);
    const auto equalizer = phazite::getEffects (*song, last).front();
    const auto container = phazite::addContainer (*song, last);
    REQUIRE (container.isValid());
    phazite::test::addOwnDeviceToChain (*song, phazite::getChains (*song, container).front(), dev::Delay::typeID);

    phazite::SongPlayer player (*song);
    REQUIRE (player.compile (44100.0, 512, std::nullopt) != nullptr);
    const phazite::ParameterRef knob { equalizer, juce::String (std::string (dev::Equalizer::info.parameters[0].id)) };

    const auto turn = [&song, &knob] (float value)
    {
        const phazite::UndoStep step (*song, "Turn Knob");
        phazite::setValueSetByHand (*song, knob, value);
    };

    // Its first value set by hand, then another, each followed by a plan.
    turn (0.25f);
    REQUIRE (player.compile (44100.0, 512, std::nullopt) != nullptr);
    turn (0.5f);
    const auto devicesRead = phazite::SongPlayer::countDevicesRead();
    const auto whole = phazite::countDevicesReadAsValues();
    const auto looks = phazite::countDeviceLooks();
    REQUIRE (player.compile (44100.0, 512, std::nullopt) != nullptr);
    const auto read = phazite::SongPlayer::countDevicesRead() - devicesRead;
    const auto readWhole = phazite::countDevicesReadAsValues() - whole;
    const auto looked = phazite::countDeviceLooks() - looks;

    turn (0.75f);
    std::unique_ptr<phazite::engine::Plan> plan;
    const auto allocations = phazite::test::countAllocations ([&player, &plan] { plan = player.compile (44100.0, 512, std::nullopt); });

    MESSAGE ("The plan after a knob moved on 40 tracks: devices' values read " << read << ", devices read whole " << readWhole << ", looks "
                                                                               << looked << ", allocations " << allocations.value_or (-1));
    CHECK (read == 3); // the drum sampler, the equalizer and the delay
    CHECK (readWhole == 0);
    CHECK (looked == 0);

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run

    REQUIRE (plan != nullptr);

    // Measured 2026-11-22 on Windows (MSVC's Debug build): 5,928 allocations before the compile read devices through the
    // conversion, 5,840 after, the same in every run; no device read whole and no walk, before and after. Measured 2026-11-23
    // after the devices' switch, reading the area: 5,718 (5,797 just before it), no device read whole and no look. Another
    // standard library counts otherwise.
#if JUCE_WINDOWS
    CHECK (*allocations <= 5718);
#endif
}

TEST_CASE ("Performance gate: the plan after a track's rename, colour, mute, height or volume on a song of 40 tracks reads its "
           "channels' fields one at a time, and no more than it did")
{
    // The plan's compile reads each channel's fields (its kind, fader, volume, pan, pad, input, mute and solo) from the element
    // the song's index gives it, through the tracks' readers of one field (model/base/internal/TracksValue.h), never a track
    // whole (countTracksReadAsValues()). A track's own property marks no channel (model/playback/SongPlayer.cpp: markChange()),
    // so the plan after a rename, a colour, a mute or a height reads no device's values again, and one after a volume set
    // reads none either: what the compile reads of the channels stays what it read before the tracks' readers outside their
    // area moved onto the tracks' readers (roadmap step S5.20, the tracks' second piece).
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    }

    const auto track = tracks[17];
    phazite::SongPlayer player (*song);
    REQUIRE (player.compile (44100.0, 512, std::nullopt) != nullptr);

    struct Counts
    {
        std::uint64_t devices = 0, whole = 0, visited = 0;
        std::optional<int> allocations;
    };

    const auto measure = [&player] (const std::function<void()>& edit)
    {
        edit();
        const auto devices = phazite::SongPlayer::countDevicesRead();
        const auto whole = phazite::countTracksReadAsValues();
        const auto visited = phazite::SongIndex::countElementsVisited();
        std::unique_ptr<phazite::engine::Plan> plan;
        Counts counts;
        counts.allocations = phazite::test::countAllocations ([&player, &plan] { plan = player.compile (44100.0, 512, std::nullopt); });
        REQUIRE (plan != nullptr);
        counts.devices = phazite::SongPlayer::countDevicesRead() - devices;
        counts.whole = phazite::countTracksReadAsValues() - whole;
        counts.visited = phazite::SongIndex::countElementsVisited() - visited;
        return counts;
    };

    struct Edit
    {
        const char* what;
        std::function<void()> make;
        int allocations; // at most, measured as the devices left the tree (Windows, MSVC's Debug build; 5,737 and 5,727 before)
    };

    const std::vector<Edit> edits {
        { "a track renamed", [&] { phazite::renameTrack (*song, track, "Renamed"); }, 5657 },
        { "a track's colour", [&] { phazite::setTrackColours (*song, { track }, juce::Colour (0xff336699)); }, 5657 },
        { "a track muted", [&] { phazite::toggleTracksMuted (*song, { track }); }, 5647 },
        { "a track's height", [&] { phazite::setSavedTrackHeight (*song, track, 120); }, 5647 },
        { "a track's volume set",
          [&]
          {
              const phazite::UndoStep step (*song, "Change Volume");
              phazite::setChannelVolume (*song, track, -6.0f);
          },
          5647 },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        const auto counts = measure (edit.make);
        MESSAGE (std::string (edit.what) << ": devices' values read " << counts.devices << ", tracks read whole " << counts.whole
                                         << ", elements visited " << counts.visited << ", allocations "
                                         << counts.allocations.value_or (-1));
        CHECK (counts.devices == 0);
        CHECK (counts.whole == 0);
        CHECK (counts.visited <= 162); // 40 tracks, measured as the allocations were

#if JUCE_WINDOWS
        if (counts.allocations.has_value())
            CHECK (*counts.allocations <= edit.allocations);
#endif
    }
}

TEST_CASE ("Performance gate: a version that differs from a song of 40 tracks by one knob, applied, writes nothing into the song's "
           "tree and reads each of the version's devices once and none of the song's")
{
    // The versions' devices pass (model/versions/ApplySong.cpp: makeDevicesLike()) takes the song's devices from the devices'
    // area (Mixer.h: getDevicesValue()), reading none, and reads the version's once each, as the song holds them
    // (DevicesValue.h: readHeldDevice()), compares them as values and writes through the devices' setters only what differs:
    // here one value set by hand, one entry of one device's values in the area (roadmap step S5.20, the devices' switch: the
    // song's tree holds no device), the setter finding the device by a look in the area (countDeviceLooks()). Nothing is
    // written into the song's tree, and the store's pass writes nothing (the song holds no plug-in or chain).
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    }

    const auto equalizer = phazite::getEffects (*song, tracks.back()).front();
    const phazite::ParameterRef knob { .device = equalizer, .parameter = "band4Gain" };
    const auto turn = [&song, &knob] (float value)
    {
        const phazite::UndoStep step (*song, "Turn Knob");
        phazite::setValueSetByHand (*song, knob, value);
    };

    turn (3.0f);
    const auto version = phazite::copyWholeSong (*song);
    turn (-6.0f);

    // What the pass writes into the song's tree, which holds no device.
    struct Writes : public juce::ValueTree::Listener
    {
        int properties = 0, children = 0;
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++properties; }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++children; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++children; }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { ++children; }
    };

    Writes writes;
    auto state = song->getState(); // a listener belongs to the tree object it was added to
    state.addListener (&writes);
    const auto whole = phazite::countDevicesReadAsValues();
    const auto looks = phazite::countDeviceLooks();

    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, version);
    }

    const auto readWhole = phazite::countDevicesReadAsValues() - whole;
    const auto looked = phazite::countDeviceLooks() - looks;
    state.removeListener (&writes);
    const auto devices = (std::uint64_t) phazite::getDevicesValue (*song).devices.size();
    MESSAGE ("A version a knob apart on 40 tracks: properties written " << writes.properties
                                                                        << ", elements added, taken out or "
                                                                           "moved "
                                                                        << writes.children << ", devices read whole " << readWhole
                                                                        << " (of " << devices << " on each side), looks " << looked);
    // Measured 2026-11-23 after the devices' switch: nothing written into the tree, the version's 80 devices read once each
    // and none of the song's, no look (the pass's setter finds the device in the value it holds); before it, one property
    // written, 81 read whole (the one set once more as the tree held it) and one walk.
    CHECK (writes.properties == 0);
    CHECK (writes.children == 0);
    CHECK (readWhole == devices); // the version's once each; the song's none
    CHECK (looked == 0);
    CHECK (phazite::getValueSetByHand (*song, knob) == doctest::Approx (3.0f));
}

TEST_CASE ("Performance gate: one track's line of a version a knob apart, restored on a song of 40 tracks and two full drum "
           "samplers, reads whole only that track's devices of the version's and none of the song's")
{
    // A line's devices pass (model/versions/ApplySong.cpp: makeDevicesLike() for one track) takes the song's devices from the
    // devices' area (Mixer.h: getDevicesValue()), reading none, and reads whole only the version's devices of the
    // line's channel; of the other channels, by their own fields alone, only what names an item the line's channel holds. On
    // a song of 40 tracks with an instrument and an effect each, and two drum samplers of every field and sound set (3,329
    // values and 1,024 sounds each) on tracks of their own: one track's line a knob apart restored, in a step.
    using dev::DrumSampler;
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    }

    phazite::test::OwnValues values;

    for (const auto& info : DrumSampler::info.parameters)
    {
        auto value =
            dev::constrain (info, info.defaultValue + (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

        if (phazite::isDefaultValue (info, value))
            value =
                dev::constrain (info, info.defaultValue - (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

        values.emplace_back (juce::String (info.id.data(), info.id.size()), value);
    }

    for (int i = 0; i < 2; ++i)
    {
        const auto drumsTrack = phazite::insertPlainTrack (*song, tracks.front());
        const auto drums = phazite::test::setOwnInstrument (*song, drumsTrack, DrumSampler::typeID, values);

        for (int pad = 0; pad < DrumSampler::numPads; ++pad)
            for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
                phazite::test::writeDeviceSoundByHand (*song, drums, phazite::schema::getPadSound (pad, layer), "808 Kick");
    }

    const auto line = tracks.back();
    const auto lineDevices = (std::uint64_t) phazite::getChannelDeviceValues (*song, line).size();
    const auto equalizer = phazite::getEffects (*song, line).front();
    const phazite::ParameterRef knob { .device = equalizer, .parameter = "band4Gain" };
    const auto turn = [&song, &knob] (float value)
    {
        const phazite::UndoStep step (*song, "Turn Knob");
        phazite::setValueSetByHand (*song, knob, value);
    };

    turn (3.0f);
    const auto version = phazite::copyWholeSong (*song);
    turn (-6.0f);

    // The line, and the whole song for the record (its gate is the one above), each in a step, each undone after.
    const auto restore = [&song, line, &version]
    {
        const phazite::UndoStep step (*song, "Restore");
        phazite::makeTrackLike (*song, line, version);
    };

    const auto bringBack = [&song, &version]
    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, version);
    };

    struct Taken
    {
        std::uint64_t readWhole = 0;
        double milliseconds = 0.0;
    };

    const auto measure = [&song, &knob] (const std::function<void()>& apply)
    {
        Taken taken;
        const auto whole = phazite::countDevicesReadAsValues();
        const auto at = juce::Time::getMillisecondCounterHiRes();
        apply();
        taken.milliseconds = juce::Time::getMillisecondCounterHiRes() - at;
        taken.readWhole = phazite::countDevicesReadAsValues() - whole;
        CHECK (phazite::getValueSetByHand (*song, knob) == doctest::Approx (3.0f));
        phazite::undo (*song);
        REQUIRE (phazite::getValueSetByHand (*song, knob) == doctest::Approx (-6.0f));
        return taken;
    };

    const auto lineTaken = measure (restore);
    const auto songTaken = measure (bringBack);
    const auto allocations = phazite::test::countAllocations (restore);
    phazite::undo (*song); // the work runs in every build (PerformanceCounts.h), counted or not

    const auto songAllocations = phazite::test::countAllocations (bringBack);
    MESSAGE ("One track's line a knob apart on 40 tracks and two full drum samplers: devices read whole "
             << lineTaken.readWhole << " (the line's " << lineDevices << "), allocations " << allocations.value_or (-1) << ", in "
             << lineTaken.milliseconds << " ms; the whole song's: devices read whole " << songTaken.readWhole << " (of "
             << phazite::getDevicesValue (*song).devices.size() << "), allocations " << songAllocations.value_or (-1) << ", in "
             << songTaken.milliseconds << " ms");
    CHECK (lineTaken.readWhole <= lineDevices + 1); // the version's of the line's channel, and the one set once more as it holds it

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run

    CHECK (phazite::getValueSetByHand (*song, knob) == doctest::Approx (3.0f));
#if JUCE_WINDOWS
    // Measured 2026-11-23 on Windows (MSVC's Debug build): 38,707 allocations and 171 ms before (both sides' devices read
    // whole, 165 of them); 2,361 and about 6 ms before the devices' versions pass; 1,439 and 4 ms with it (3 read whole).
    // The whole song's, 42,294 allocations and 180 ms with both sides read whole (165), 22,928 and 93 ms with the version's
    // alone (83: its 82, and the one set once more).
    CHECK (*allocations <= 1500);
#endif
}

TEST_CASE ("Performance gate: a version a track's name apart from a song of 40 tracks, applied, writes one property, reads each "
           "of the version's tracks once and none of the song's, nor what a track holds; a line's restore reads its own track")
{
    // The versions' tracks pass (model/versions/ApplySong.cpp: makeTracksLike()) takes the song's tracks from the tracks'
    // keeper (Tracks.h: getHeldTracks(), findHeldTrack(): none read), reads the version's once each, as the song holds one
    // (TracksValue.h: readHeldTrack()), and writes through the tracks' setters only what differs: here one track's name, one
    // property of its element, while the song's tree is the tracks' storage. It reads nothing a track holds (no device read
    // whole for it: the devices pass reads the version's once each, as its own gate above counts) and erases nothing. One
    // track's line reads the version's track alone, the song's from the keeper.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    }

    const auto line = tracks[20];
    const auto rename = [&song, line] (const juce::String& name)
    {
        const phazite::UndoStep step (*song, "Rename");
        phazite::setTrackName (*song, line, name);
    };

    rename ("Then");
    const auto version = phazite::copyWholeSong (*song);
    rename ("Now");

    struct Writes : public juce::ValueTree::Listener
    {
        int properties = 0, children = 0;
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { ++properties; }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override { ++children; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { ++children; }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { ++children; }
    };

    struct Taken
    {
        Writes writes {};
        std::uint64_t tracksRead = 0, devicesRead = 0;
    };

    const auto measure = [&song, line] (const std::function<void()>& apply)
    {
        Taken taken;
        auto state = song->getState(); // a listener belongs to the tree object it was added to
        state.addListener (&taken.writes);
        const auto tracksBefore = phazite::countTracksReadAsValues();
        const auto devicesBefore = phazite::countDevicesReadAsValues();

        {
            const phazite::UndoStep step (*song, "Apply");
            apply();
        }

        taken.tracksRead = phazite::countTracksReadAsValues() - tracksBefore;
        taken.devicesRead = phazite::countDevicesReadAsValues() - devicesBefore;
        state.removeListener (&taken.writes);
        CHECK (phazite::getTrackName (*song, line) == "Then");
        phazite::undo (*song);
        REQUIRE (phazite::getTrackName (*song, line) == "Now");
        return taken;
    };

    const auto songTaken = measure ([&song, &version] { phazite::makeSongLike (*song, version); });
    const auto lineTaken = measure ([&song, line, &version] { phazite::makeTrackLike (*song, line, version); });
    const auto devices = (std::uint64_t) phazite::getDevicesValue (*song).devices.size();
    MESSAGE ("A version a track's name apart on 40 tracks: properties written "
             << songTaken.writes.properties << ", elements added, taken out or moved " << songTaken.writes.children
             << ", tracks read whole " << songTaken.tracksRead << " (40 and the master on each side), devices read whole "
             << songTaken.devicesRead << " (of " << devices << " on each side); the line's: properties written "
             << lineTaken.writes.properties << ", tracks read whole " << lineTaken.tracksRead << ", devices read whole "
             << lineTaken.devicesRead);
    // Measured 2026-11-24 with the tracks' versions pass: one property written, nothing added, taken out or moved, the
    // version's 40 tracks and its master read once each and none of the song's, the version's 80 devices once each by the
    // devices pass (none for the tracks); the line's one property, its one track read, its two devices.
    CHECK (songTaken.writes.properties == 1);
    CHECK (songTaken.writes.children == 0);
    CHECK (songTaken.tracksRead == 41);
    CHECK (songTaken.devicesRead == devices);
    CHECK (lineTaken.writes.properties == 1);
    CHECK (lineTaken.writes.children == 0);
    CHECK (lineTaken.tracksRead == 1);
    CHECK (lineTaken.devicesRead <= 3); // the version's of the line's channel
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

TEST_CASE ("Performance gate: the loader's rule for the devices' store allocates nothing for the golden project's devices and "
           "chains, a file Phazite wrote")
{
    // The loader makes each DEVICE and CHAIN hold what the song holds of the devices' store and a window's place, as Phazite
    // writes it (model/base/internal/DeviceStoreValue.h: keepStoreAsHeld()): it looks through the element as it is
    // (isStoreWrittenAsHeld()) and reads it and writes it again only where it isn't so. The representative golden project
    // (a plug-in with a state, a program, a version and a value set by hand, another with a state, chains muted and
    // soloed), every DEVICE and CHAIN of it as it is read from its text.
    const auto xml = phazite::test::readXml (phazite::test::getGoldenFolder().getChildFile ("projects/representative.xml"));
    REQUIRE (xml != nullptr);
    const auto state = juce::ValueTree::fromXml (*xml);

    std::vector<juce::ValueTree> elements;
    const std::function<void (const juce::ValueTree&)> collect = [&collect, &elements] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (phazite::schema::CLIP) || child.hasType (phazite::schema::AUTOMATION))
                continue;

            if (child.hasType (phazite::schema::DEVICE) || child.hasType (phazite::schema::CHAIN))
                elements.push_back (child);

            collect (child);
        }
    };

    collect (state);
    REQUIRE (elements.size() >= 10);

    // Once on copies first, uncounted: what the program makes once, as the rule's readers first look up the schema's
    // descriptions (3 allocations, measured 2026-11-23), is no cost of a file's.
    for (const auto& element : elements)
        juce::ignoreUnused (phazite::keepStoreAsHeld (element.createCopy()));

    auto changed = false;
    const auto allocations = phazite::test::countAllocations (
        [&elements, &changed]
        {
            for (const auto& element : elements)
                changed = phazite::keepStoreAsHeld (element) || changed;
        });

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run

    CHECK_FALSE (changed);
    MESSAGE ("The store's loader's rule over " << elements.size() << " devices and chains: " << *allocations << " allocations");
    CHECK (*allocations == 0);
}

TEST_CASE ("Performance gate: a step of a knob's drag on a full drum sampler, among 40 tracks, reads no device whole and looks it "
           "up a few times, and the song's devices are read once as it is made")
{
    // The devices' setter of one value (Mixer.h: setDeviceValue(), on the area since the devices' switch) sets one entry of
    // the device's values in the area, the device read no more whole (countDevicesReadAsValues()) and looked up by its ID
    // a few times (countDeviceLooks()), what hears the devices told by immer's diff (model/base/internal/DevicesArea.h), the
    // setter's "no change?" comparing the entries where the device's values differ alone, a few however many it holds
    // (countDeviceEntriesCompared()); and as the song is made (makeSong()), each device, container and chain read whole once
    // at most, by the adoption: the loader's checks of the curves and the MIDI links read the devices' fields alone, once
    // (DevicesValue.h: readDevicesFields()). On a song of 40 tracks with an instrument and an effect each, an automated
    // parameter and a MIDI link each, and a drum sampler of every field and sound set (3,329 values, 1,024 sounds), in a
    // project context (what plays it, its views' watcher): a step of a drag of a pad's field through its handle, as a
    // control's drag makes one.
    using dev::DrumSampler;
    auto made = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*made, 40);

    // Each track's equalizer's first parameter automated and its synth's first linked to a controller, so that the loader's
    // checks of the curves and the MIDI links (Project.cpp: makeConsistent()) find their devices as the song is made.
    {
        const phazite::UndoStep step (*made, "Automate and Link");
        auto number = 20;

        for (const auto track : tracks)
        {
            const auto synth = phazite::test::setOwnInstrument (*made, track, dev::Synth::typeID);
            const auto equalizer = phazite::test::addOwnDevice (*made, track, dev::Equalizer::typeID);
            const auto& synthParameter = dev::Synth::info.parameters.front().id;
            const auto& equalizerParameter = dev::Equalizer::info.parameters.front().id;
            const phazite::ParameterRef automated { equalizer, juce::String (equalizerParameter.data(), equalizerParameter.size()) };
            REQUIRE (phazite::addPoint (*made, automated, { at (0.0), 0.25f }) >= 0);
            phazite::linkMidiController (*made, { .channel = 1, .number = number++ },
                                         { synth, juce::String (synthParameter.data(), synthParameter.size()) });
        }
    }

    REQUIRE (phazite::getMidiLinks (*made).size() == tracks.size());
    phazite::test::OwnValues values;

    for (const auto& info : DrumSampler::info.parameters)
    {
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
        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
            phazite::test::writeDeviceSoundByHand (*made, drums, phazite::schema::getPadSound (pad, layer), "808 Kick");

    // The song made from it, as a project opens one: each of its devices read whole once, as it takes its values.
    const auto text = phazite::test::copyWholeSong (*made);
    const auto readBefore = phazite::countDevicesReadAsValues();
    const auto madeAt = juce::Time::getMillisecondCounterHiRes();
    const auto song = phazite::makeSong (text);
    const auto madeIn = juce::Time::getMillisecondCounterHiRes() - madeAt;
    REQUIRE (song != nullptr);
    const auto readAsMade = phazite::countDevicesReadAsValues() - readBefore;
    const auto numDevices = phazite::getDevicesValue (*song).devices.size();

    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, song.get());
    const auto& padField = DrumSampler::info.parameters[(size_t) DrumSampler::getParameterIndex (40, (DrumSampler::Field) 1)];
    auto handle = phazite::makeDeviceHandle (*song, drums).getParameter (padField.id);
    REQUIRE (handle.isValid());
    const auto low = padField.minimum + 0.1f * (padField.maximum - padField.minimum);
    const auto high = padField.minimum + 0.2f * (padField.maximum - padField.minimum);

    handle.beginGesture();
    handle.setValue (low); // the gesture's first step finds the parameter
    phazite::test::dispatchPendingMessages();
    const auto whole = phazite::countDevicesReadAsValues();
    const auto looks = phazite::countDeviceLooks();
    const auto compares = phazite::countDeviceEntriesCompared();
    handle.setValue (high);
    const auto readWhole = phazite::countDevicesReadAsValues() - whole;
    const auto looked = phazite::countDeviceLooks() - looks;
    const auto compared = phazite::countDeviceEntriesCompared() - compares;
    auto toLow = true;
    const auto allocations = phazite::test::countAllocations (
        [&handle, &toLow, low, high]
        {
            handle.setValue (toLow ? low : high);
            toLow = ! toLow;
        });
    handle.endGesture();
    phazite::test::dispatchPendingMessages();
    setSong (context, nullptr);

    MESSAGE ("A step of a pad's drag on 40 tracks and a full drum sampler: devices read whole "
             << readWhole << ", looks " << looked << ", values compared " << compared << ", allocations " << allocations.value_or (-1)
             << "; the song made: " << numDevices << " devices read " << readAsMade << " times whole, in " << madeIn << " ms");
    // Measured 2026-11-23 on Windows (MSVC's Debug build), before the devices' listener and with it: no device read whole
    // at a step; one walk, the same before, which is the writer's (setValueSetByHand() finding the device's element), none
    // the listener's; 239 allocations before, 246 with it (the value kept, one path of its map copied, and the change told);
    // the song made in 114 ms before, reading no device whole, and in 171 ms with it, each of its 81 devices read once.
    // Measured 2026-11-23 after the devices' switch: no device read whole at a step, 4 looks in the area by an ID, 36
    // allocations (one entry of one map set); the song made in 184 ms, each of its 81 devices read once (2.4 s and 3,483
    // reads while the loader read the song's devices whole for each track). Measured 2026-11-23 with a curve and a MIDI link
    // on each track, three runs each: before the setter's comparison went by identity first, 734 values compared at a step
    // (every one up to the one that differs), after it 2 (the entries of the one map node where the two differ: at most a
    // node's 32, however many values the device holds); before the loader's checks of the curves and the MIDI links read
    // the devices' fields once, 201 devices read whole as the song was made (each EQ twice for its curve, each synth once
    // for its link) in 184, 180 and 182 ms, after it 81 in 200, 177 and 172 ms (a Debug build beside another's build).
    CHECK (readWhole == 0);
    CHECK (looked <= 4);
    CHECK (compared <= 32);
    CHECK (readAsMade <= (std::uint64_t) numDevices); // each device read whole once at most, by the adoption

    if (! allocations.has_value())
        return; // nothing counted on this build, and nothing run

#if JUCE_WINDOWS
    CHECK (*allocations <= 40); // 239 before the devices' listener, 246 with it, 36 since the devices' switch
#endif
}

TEST_CASE ("Performance gate: a frozen track holding a full drum sampler, compared after its fader moved, writes none of its "
           "devices into elements and compares them by identity")
{
    // What a frozen track's file holds (Freeze.h: FreezeKeeper::Holds) is compared once a change in the track is told (a
    // fader moved, at the next turn of the message loop or as Song::writePending() asks, as a save or a plan's compile does):
    // its frozen devices as the song's values hold them, by the identity of the song's devices first, none of their values
    // or sounds compared (countDeviceEntriesCompared()), none written into an element. Counted: what that comparison
    // allocates. On a track with a drum sampler of every field and sound set (3,329 values, 1,024 sounds), frozen.
    using dev::DrumSampler;
    phazite::test::TestSong s;
    phazite::test::TemporaryFolder temp;
    auto& song = getSong (s);
    const auto track = s.firstTrack;
    phazite::test::OwnValues values;

    for (const auto& info : DrumSampler::info.parameters)
    {
        auto value =
            dev::constrain (info, info.defaultValue + (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

        if (phazite::isDefaultValue (info, value))
            value =
                dev::constrain (info, info.defaultValue - (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

        values.emplace_back (juce::String (info.id.data(), info.id.size()), value);
    }

    const auto drums = phazite::test::setOwnInstrument (song, track, DrumSampler::typeID, values);

    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
            phazite::test::writeDeviceSoundByHand (song, drums, phazite::schema::getPadSound (pad, layer), "808 Kick");

    phazite::freezeFromFile (song, track, phazite::test::writeSound (temp.folder.getChildFile ("Frozen.wav"), 1.0));
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::isTrackFrozen (song, track));

    {
        const phazite::UndoStep step (song, "Change Volume");
        phazite::setChannelVolume (song, track, -4.0f);
    }

    const auto compares = phazite::countDeviceEntriesCompared();
    const auto allocations = phazite::test::countAllocations ([&song] { song.writePending(); });
    const auto compared = phazite::countDeviceEntriesCompared() - compares;
    CHECK (phazite::isTrackFrozen (song, track));

    MESSAGE ("A frozen track with a full drum sampler compared after its fader moved: values compared " << compared << ", allocations "
                                                                                                        << allocations.value_or (-1));
    CHECK (compared == 0);

    if (! allocations.has_value())
        return; // nothing counted on this build

    // Measured 2026-11-23 on Windows (MSVC's Debug build): 3,396 allocations while the comparison wrote the frozen devices'
    // elements from their values at each check, 35 since it compares the values by identity.
#if JUCE_WINDOWS
    CHECK (*allocations <= 40);
#endif
}

TEST_CASE ("Performance gate: an effect added into a chain, moved within it and removed looks up as many devices, and allocates no "
           "more, on 40 tracks than on 4")
{
    // The devices' setters (Mixer.h: setNewDevices(), moveInDeviceList(), eraseDevice(); roadmap step S5.20, the devices'
    // switch) find what they change by a look in the devices' area and set its next value once, what hears the devices told
    // by immer's diff (model/base/internal/DevicesArea.h), never a walk of the song: a drag of a device within its chain
    // makes one at each step. On songs of 4 tracks and of 40, each with an instrument and an effect,
    // the first with a container whose chain holds two delays: an equalizer added into the chain, moved to its start and
    // removed, each one step on a song alone (no project: what else listens costs the same on both). Counted: the looks in
    // the devices' area by an ID (countDeviceLooks()), and what the edit allocates, on the first track.
    struct Edit
    {
        std::uint64_t looks = 0;
        std::optional<int> allocations {};
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

        const auto container = phazite::addContainer (*song, tracks.front());
        REQUIRE (container.isValid());
        const auto chain = phazite::getChains (*song, container).front();
        phazite::test::addOwnDeviceToChain (*song, chain, dev::Delay::typeID);
        phazite::test::addOwnDeviceToChain (*song, chain, dev::Delay::typeID);

        // One edit as a step: the looks it made, and what it allocated where that is counted (else run alone).
        const auto measure = [&song] (const std::function<void()>& change)
        {
            const auto inStep = [&song, &change]
            {
                const phazite::UndoStep step (*song, "Edit");
                change();
            };

            Edit edit;
            const auto before = phazite::countDeviceLooks();
            edit.allocations = phazite::test::countAllocations (inStep); // run in every build, counted or not
            edit.looks = phazite::countDeviceLooks() - before;
            return edit;
        };

        phazite::DeviceID added;
        const auto adding = measure ([&] { added = phazite::test::addOwnDeviceToChain (*song, chain, dev::Equalizer::typeID); });
        REQUIRE (added.isValid());
        const auto moving = measure ([&] { phazite::moveInChain (*song, added, 0); });
        REQUIRE (phazite::getDevicesValue (*song).findList ({ .track = tracks.front(), .container = container, .chain = chain })->front()
                 == added);
        const auto removing = measure ([&] { phazite::removeDevice (*song, added); });
        REQUIRE_FALSE (phazite::hasDevice (*song, added));

        // What the removal's writer does beside it that grows with the song, alone (Mixer.h: forgetWhatNamesTheGone(), which
        // makes a set of every ID the song holds), with nothing gone: no look.
        const auto forgetting = measure ([&] { phazite::forgetWhatNamesTheGone (*song); });
        return std::array { adding, moving, removing, forgetting };
    };

    const auto small = countFor (4);
    const auto big = countFor (40);
    const std::array<std::string, 4> names { "an effect added into a chain", "moved within it", "removed",
                                             "what names the gone forgotten alone" };

    for (size_t i = 0; i < names.size(); ++i)
        MESSAGE (names[i] << ": looks " << small[i].looks << " (4 tracks), " << big[i].looks << " (40 tracks); allocations "
                          << small[i].allocations.value_or (-1) << ", " << big[i].allocations.value_or (-1));

    for (size_t i = 0; i < names.size(); ++i)
    {
        INFO (names[i]);
        CHECK (big[i].looks == small[i].looks);
        CHECK (small[i].looks <= 4); // measured 2026-11-23: added 1, moved 0, removed 4, forgetting alone 0
    }

    if (! small[0].allocations.has_value() || ! big[0].allocations.has_value())
        return; // nothing counted on this build

    for (size_t i = 0; i < names.size(); ++i)
    {
        INFO (names[i]);
        REQUIRE (big[i].allocations.has_value());
        REQUIRE (small[i].allocations.has_value());
    }

    // The removal's beside what its writer's forgetting of what names the gone makes, which grows with the song (not this
    // listener's; the writers' piece of S5.20). Measured 2026-11-23 (MSVC's Debug build), 4 tracks / 40: added 78 / 74 (74 /
    // 75 in a run after other tests), moved 61 / 61, removed 163 / 452 of which its writer's forgetting 63 / 351, so 100 / 101
    // beside it; with the listener's first form, which walked the song's devices for each, on the last track 158 / 952,
    // 133 / 745 and 292 / 1,913, and 13 / 85 device elements walked. Before the listener the tree's callbacks walked none.
    // Measured 2026-11-23 after the devices' switch (the setters on values, immer's diff telling the change): added 30 / 30,
    // moved 17 / 17, removed 62 / 279 of which the forgetting 52 / 268, so 10 / 11 beside it.
    // Within a few, which what ran before moves either way (a pool of names, the maps' layout): never one for each of the 36
    // tracks more.
    const auto listened = [] (const auto& counts) { return *counts[2].allocations - *counts[3].allocations; };
    constexpr auto few = 4;
    CHECK (*big[0].allocations <= *small[0].allocations + few);
    CHECK (*big[1].allocations <= *small[1].allocations + few);
    CHECK (listened (big) <= listened (small) + few);
}

TEST_CASE ("Performance gate: a chain duplicated reads its own items whole and none of its channel's others, and allocates as "
           "much beside a full drum sampler as beside a synth")
{
    // duplicateChain() (model/mixer/internal/Containers.h) reads the chain and the devices in it whole, to copy them, and none
    // of its channel's other items: a full drum sampler (3,329 values, 1,024 sounds) on the same channel, read whole, would
    // cost thousands of allocations for nothing. On a song alone (no project): a track with an instrument (a synth, or a full
    // drum sampler) and a container whose chain holds a delay and an equalizer, the chain duplicated as one step. Counted:
    // the devices read whole (countDevicesReadAsValues()), and what the step allocates.
    using dev::DrumSampler;

    struct Copy
    {
        std::uint64_t readWhole = 0;
        std::optional<int> allocations {};
    };

    const auto countFor = [] (bool withSampler)
    {
        auto song = phazite::createSong();
        const auto track = phazite::ensureTracks (*song, 1).front();

        if (withSampler)
        {
            phazite::test::OwnValues values;

            for (const auto& info : DrumSampler::info.parameters)
            {
                auto value = dev::constrain (info, info.defaultValue
                                                       + (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

                if (phazite::isDefaultValue (info, value))
                    value = dev::constrain (info, info.defaultValue
                                                      - (info.unit == dev::Unit::choice ? 1.0f : 0.25f * (info.maximum - info.minimum)));

                values.emplace_back (juce::String (info.id.data(), info.id.size()), value);
            }

            const auto drums = phazite::test::setOwnInstrument (*song, track, DrumSampler::typeID, values);

            for (int pad = 0; pad < DrumSampler::numPads; ++pad)
                for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
                    phazite::test::writeDeviceSoundByHand (*song, drums, phazite::schema::getPadSound (pad, layer), "808 Kick");
        }
        else
        {
            phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        }

        phazite::DeviceID container;

        {
            const phazite::UndoStep step (*song, "Add Container");
            container = phazite::addContainer (*song, track);
        }

        REQUIRE (container.isValid());
        const auto chain = phazite::getChains (*song, container).front();
        phazite::test::addOwnDeviceToChain (*song, chain, dev::Delay::typeID);
        phazite::test::addOwnDeviceToChain (*song, chain, dev::Equalizer::typeID);

        const auto duplicate = [&song, chain]
        {
            const phazite::UndoStep step (*song, "Duplicate Chain");
            phazite::duplicateChain (*song, chain);
        };

        Copy copy;
        const auto before = phazite::countDevicesReadAsValues();
        copy.allocations = phazite::test::countAllocations (duplicate); // run in every build, counted or not
        copy.readWhole = phazite::countDevicesReadAsValues() - before;
        REQUIRE (phazite::getChains (*song, container).size() == 2);
        return copy;
    };

    const auto beside = countFor (false);
    const auto besideSampler = countFor (true);
    MESSAGE ("A chain of two devices duplicated: devices read whole "
             << beside.readWhole << " beside a synth, " << besideSampler.readWhole << " beside a full drum sampler; allocations "
             << beside.allocations.value_or (-1) << ", " << besideSampler.allocations.value_or (-1));

    // Since the devices' switch none read whole, the chain and its two devices copied from the values the area holds; before
    // it, each read whole once to be copied and their copies once by what heard the devices change, 6, whatever else is on
    // the channel.
    CHECK (besideSampler.readWhole == beside.readWhole);
    CHECK (beside.readWhole == 0);

    if (! beside.allocations.has_value() || ! besideSampler.allocations.has_value())
        return; // nothing counted on this build

    // Measured 2026-11-23 (MSVC's Debug build): 6 read whole on both; 150 allocations beside a synth, 148 beside the drum
    // sampler (with the channel read whole, as the copy first did, 11 read whole and 156 beside the synth, 9,599 beside the
    // sampler, its values and sounds read for nothing). Measured 2026-11-23 after the devices' switch: 77 on both. Within a
    // few, which what ran before moves either way.
    constexpr auto few = 4;
    CHECK (*besideSampler.allocations <= *beside.allocations + few);
#if JUCE_WINDOWS
    CHECK (*beside.allocations <= 80); // 150 before the devices' switch, 77 since
#endif
}

TEST_CASE ("Performance gate: the unsaved check after devices' knobs turned and turned back compares the devices turned alone, "
           "each once, on 4 tracks and on 40")
{
    // The devices that changed are noted by their IDs (model/project/internal/Project.h: devicesToCompare) and compared with
    // the saved copy's when the check is asked, by value since the devices' switch (model/base/internal/WholeState.h:
    // holdSameDevice(), a look in each side's devices by the ID), never a walk of either. On songs of 4 tracks and of 40, each
    // with an instrument and an effect, saved: one effect's knob turned and turned back, then 30 (or each of the 4), the check
    // asked after each. Counted: what the comparisons with the saved copy compared (Project::countElementsCompared(), one
    // for each device compared).
    struct Counts
    {
        std::uint64_t one = 0, many = 0, items = 0, turned = 0;
    };

    const auto countFor = [] (int numTracks)
    {
        phazite::test::TestSong s;
        auto& song = getSong (s);
        auto& project = s.getProject();
        std::vector<phazite::DeviceID> effects;

        for (const auto track : phazite::ensureTracks (song, numTracks))
        {
            phazite::test::setOwnInstrument (song, track, dev::Synth::typeID);
            effects.push_back (phazite::test::addOwnDevice (song, track, dev::Equalizer::typeID));
        }

        REQUIRE (project.save().wasOk());
        phazite::test::dispatchPendingMessages();
        REQUIRE_FALSE (project.hasUnsavedChanges());

        const auto& gain = dev::Equalizer::info.parameters[(size_t) dev::Equalizer::getParameterIndex (2, dev::Equalizer::gain)];
        const auto gainID = juce::String (std::string (gain.id));

        // So many effects' knobs turned in one step and turned back in another, and the check asked: what it compared.
        const auto compare = [&] (size_t count)
        {
            for (const auto value : { 3.0f, gain.defaultValue })
            {
                const phazite::UndoStep step (song, "Turn");

                for (size_t i = 0; i < count && i < effects.size(); ++i)
                    phazite::setValueSetByHand (song, { effects[i], gainID }, value);
            }

            const auto before = phazite::Project::countElementsCompared();
            CHECK_FALSE (project.hasUnsavedChanges());
            return (std::uint64_t) (phazite::Project::countElementsCompared() - before);
        };

        Counts counts;
        counts.one = compare (1);
        counts.many = compare (30);
        counts.turned = (std::uint64_t) std::min (effects.size(), (size_t) 30);
        counts.items = (std::uint64_t) phazite::getDevicesValue (song).devices.size();
        return counts;
    };

    const auto small = countFor (4);
    const auto big = countFor (40);
    MESSAGE ("The unsaved check: compared for one knob " << small.one << " (4 tracks), " << big.one << " (40 tracks); for "
                                                         << "30 (or 4) " << small.many << ", " << big.many << "; devices in the song "
                                                         << small.items << ", " << big.items);

    // Each device turned compared once, and no other, however many the song holds. Measured 2026-11-23, 4 tracks / 40: one
    // knob 1 / 1, 30 (or 4) 4 / 30, compared as values; while the trees held the devices (counting the device elements
    // walked, each tree's indexed once by a walk), 16 / 160 for one knob as for many; with a walk of each tree for each
    // device compared and for each owner, 16 / 160 and 64 / 4,800.
    for (const auto& counts : { small, big })
    {
        CHECK (counts.one == 1); // the knob's device compared: none would be a check skipped
        CHECK (counts.many == counts.turned);
    }
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

TEST_CASE ("Performance gate: a track deleted erases what it holds reading as much on a song of 40 tracks as on one of 4, and "
           "allocating no more but for the depth of the areas' maps")
{
    // What a track's removal takes of the song's areas beside its tree, explicitly, before its element goes
    // (Tracks.h: eraseTrackHoldings()): its clips' list, its channel's and its containers' lists of modulators and its sends'
    // list each looked in alone, and each area set once. No modulator or send read from an element (ModulationValue.h:
    // countModulatorsRead(), SendsValue.h: countSendsRead()), no more looks at the sends (countSendLooks()) and no more of the
    // song's elements visited (SongIndex.h: countElementsVisited(), which counts the clips' lists walked too) for 40 tracks of
    // 3 clips, a modulator and a send each than for 4; and the allocations the same but for what immer's maps allocate for
    // the depth of their trees, what hears the areas change included (the project's). Each area's part alone is stated for a
    // song of no project, which nothing hears. The whole Delete Track allocates more for the larger song, for what names the
    // gone (Mixer.h: forgetWhatNamesTheGone(), which gathers the song's IDs): stated, not gated here.
    struct Counts
    {
        std::uint64_t modulatorsRead = 0, sendsRead = 0, sendLooks = 0, visited = 0;
        std::optional<int> erase {}, remove {}, bareClips {}, bareModulators {}, bareSends {};
    };

    const auto countsFor = [] (int numTracks)
    {
        phazite::test::TestSong s;
        auto& song = *phazite::findSong (s.context);
        std::vector<phazite::TrackID> tracks { s.firstTrack };

        while (tracks.size() < (size_t) numTracks)
            tracks.push_back (s.addTrack ("Track"));

        const auto reverb = s.addReturn (tracks[0], dev::Reverb::typeID, -6.0f);

        for (const auto track : tracks)
        {
            for (int i = 0; i < 3; ++i)
                s.addNote (s.addMidiClip (track, i * 4.0, 4.0), 60 + i, i * 4.0, 1.0);

            s.addModulator (track, phazite::ModulatorType::lfo);

            if (track != tracks[0])
                s.addSend (track, reverb, -6.0f);
        }

        phazite::test::dispatchPendingMessages();
        const auto victim = tracks[1];
        Counts counts;

        {
            const phazite::UndoStep step (song, "Erase");
            const auto modulatorsBefore = phazite::countModulatorsRead();
            const auto sendsBefore = phazite::countSendsRead();
            const auto looksBefore = phazite::countSendLooks();
            const auto visitedBefore = phazite::SongIndex::countElementsVisited();
            counts.erase = phazite::test::countAllocations ([&song, victim] { phazite::eraseTrackHoldings (song, { victim }); });
            counts.modulatorsRead = phazite::countModulatorsRead() - modulatorsBefore;
            counts.sendsRead = phazite::countSendsRead() - sendsBefore;
            counts.sendLooks = phazite::countSendLooks() - looksBefore;
            counts.visited = phazite::SongIndex::countElementsVisited() - visitedBefore;
        }

        CHECK (phazite::getClips (song, victim).empty());
        phazite::undo (song);
        REQUIRE (phazite::getClips (song, victim).size() == 3);
        phazite::test::dispatchPendingMessages();

        {
            const phazite::UndoStep step (song, "Delete Track");
            counts.remove = phazite::test::countAllocations ([&song, victim] { phazite::deleteTracks (song, { victim }, nullptr); });
        }

        // The same song's areas in a song of no project, which nothing listens to (what the views, the player and the versions
        // do as they are told is theirs, not the erase's): each area's part alone.
        auto bare = phazite::createSong();
        const auto bareTracks = phazite::ensureTracks (*bare, numTracks);
        const auto bareReturn = phazite::insertReturnTrack (*bare, "Reverb", nullptr);

        for (const auto track : bareTracks)
        {
            for (int i = 0; i < 3; ++i)
            {
                const auto clip =
                    phazite::insertMidiClip (*bare, track, { phazite::test::at (i * 4.0), phazite::test::at (i * 4.0 + 4.0) }, nullptr);
                phazite::addNote (*bare, clip, 60 + i, phazite::test::at (i * 4.0), phazite::test::lasting (1.0), 100);
            }

            phazite::addModulator (*bare, track, phazite::ModulatorType::lfo);
            phazite::addSend (*bare, track, bareReturn, false);
        }

        bare->getUndoManager().clearUndoHistory();
        const auto bareVictim = bareTracks[1];
        const phazite::UndoStep step (*bare, "Erase");
        counts.bareClips = phazite::test::countAllocations ([&bare, bareVictim] { phazite::eraseTrackClips (*bare, { bareVictim }); });
        counts.bareModulators = phazite::test::countAllocations (
            [&bare, bareVictim] { phazite::eraseModulatorsOf (*bare, { phazite::ModulatorPlace { .track = bareVictim } }); });
        counts.bareSends = phazite::test::countAllocations ([&bare, bareVictim] { phazite::eraseTrackSends (*bare, { bareVictim }); });
        return counts;
    };

    const auto small = countsFor (4), large = countsFor (40);
    INFO ("4 tracks: " << small.modulatorsRead << " modulators and " << small.sendsRead << " sends read, " << small.sendLooks
                       << " looks at the sends, " << small.visited << " elements visited, " << small.erase.value_or (-1)
                       << " allocations erasing, " << small.remove.value_or (-1) << " deleting; 40 tracks: " << large.modulatorsRead << ", "
                       << large.sendsRead << ", " << large.sendLooks << ", " << large.visited << ", " << large.erase.value_or (-1) << ", "
                       << large.remove.value_or (-1));
    INFO ("Alone, of no project: 4 tracks: " << small.bareClips.value_or (-1) << " clips, " << small.bareModulators.value_or (-1)
                                             << " modulation, " << small.bareSends.value_or (-1)
                                             << " sends; 40 tracks: " << large.bareClips.value_or (-1) << ", "
                                             << large.bareModulators.value_or (-1) << ", " << large.bareSends.value_or (-1));
    CHECK (small.modulatorsRead == 0);
    CHECK (large.modulatorsRead == 0);
    CHECK (small.sendsRead == 0);
    CHECK (large.sendsRead == 0);
    CHECK (large.sendLooks == small.sendLooks);
    CHECK (large.visited == small.visited);

    if (! small.erase.has_value() || ! large.erase.has_value())
        return; // nothing counted on this build (PerformanceCounts.h)

    // Measured 2026-11-23 (Windows Debug): 79 and 92, the clips' 29 and 38, modulation's 9 and 11, the sends' 9 and 11 of
    // them: each map a level deeper for ten times the entries. Through the whole-value setters, whose checks in Debug builds
    // walk the area (ModulationValue.h: holdsTogether(); SendsValue.h: holdsItsLists()), 92 and 285. Delete Track whole: 188
    // and 536 (what names the gone gathers the song's IDs).
    CHECK (*large.erase <= *small.erase + 16);
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

TEST_CASE ("Performance gate: a track's own edit has the versions copy and compare that track alone")
{
    // A rename, a colour, a mute, a volume and the master's volume, each copied and compared alone (ChangesWorker.cpp: take(),
    // a track found by its ID), however many tracks the song has.
    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 32, 16.0);
    auto& song = getSong (s);
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&result] { return result.has_value(); }, phazite::test::versionHistoryTimeout));
    REQUIRE (phazite::test::waitForVersionHistory (s.context));
    REQUIRE (phazite::countChanges (phazite::test::withChangesIn (s.context)) == 0);
    const auto track = built.tracks[5];

    struct Edit
    {
        const char* what;
        std::function<void()> make;
    };

    const std::vector<Edit> edits {
        { "a track renamed", [&] { phazite::renameTrack (song, track, "Renamed"); } },
        { "a track's colour", [&] { phazite::setTrackColours (song, { track }, juce::Colour (0xff336699)); } },
        { "a track muted", [&] { phazite::toggleMute (s.context, track); } },
        { "a track's volume", [&] { s.setVolume (track, -6.0f); } },
        { "the master's volume",
          [&]
          {
              const phazite::UndoStep step (song, "Change Volume");
              phazite::setChannelVolume (song, {}, -9.0f);
          } },
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        const auto copied = phazite::ChangesWorker::countTracksCopied();
        const auto compared = phazite::ChangesWorker::countTracksCompared();
        edit.make();
        phazite::test::dispatchPendingMessages();
        phazite::countChanges (phazite::test::withChangesIn (s.context));
        CHECK (phazite::ChangesWorker::countTracksCopied() - copied == 1);
        CHECK (phazite::ChangesWorker::countTracksCompared() - compared == 1);
    }
}

TEST_CASE ("Performance gate: a round of the versions' changes after a rename on a song of 40 tracks reads no track whole")
{
    // A rename marks its track and the tracks' names (VersionHistory.cpp: markTrack()), so the round copies that track and the
    // song's skeleton (ChangesWorker.cpp: makeSkeleton(): each track's ID, kind and name, in its folder, and the master's ID),
    // on the message thread. The skeleton reads those three fields one at a time from the elements the song's index gives,
    // through the tracks' readers of one field (model/base/internal/TracksValue.h), never a track whole
    // (countTracksReadAsValues()), and allocates no more than the walk of the elements it replaced did.
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 40);

    for (const auto track : tracks)
    {
        phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID);
        phazite::test::addOwnDevice (*song, track, dev::Equalizer::typeID);
    }

    phazite::ChangesWorker worker;
    worker.setReference (phazite::test::copyWholeSong (*song), {}, true, true);
    const phazite::ChangesWorker::KnobReader none = [] (const std::set<std::uint64_t>&) { return phazite::PluginParameterValues(); };

    const auto rename = [&] (const juce::String& name)
    {
        phazite::renameTrack (*song, tracks[17], name);
        worker.markTrack (tracks[17].getRaw());
        worker.markOrder();
    };

    // A round first, taken and worked out, so that what is made once (the kinds' words) is made before the round counted.
    rename ("First");
    const auto first = worker.take (*song, none, none, {});
    REQUIRE (first != nullptr);
    REQUIRE (worker.takeResult (phazite::ChangesWorker::work (*first)));

    rename ("Renamed");
    const auto whole = phazite::countTracksReadAsValues();
    std::shared_ptr<phazite::ChangesWorker::Request> request;
    const auto allocations = phazite::test::countAllocations ([&] { request = worker.take (*song, none, none, {}); });
    const auto readWhole = phazite::countTracksReadAsValues() - whole;
    REQUIRE (request != nullptr);
    CHECK (request->skeleton);
    CHECK (request->tracks.size() == 1);
    MESSAGE ("A round after a rename on 40 tracks: tracks read whole " << readWhole << ", allocations " << allocations.value_or (-1));
    CHECK (readWhole == 0);

    if (! allocations.has_value())
        return;

#if JUCE_WINDOWS
    // Measured 2026-11-23 on Windows (MSVC's Debug build), the whole round's copies (the renamed track's, with its devices,
    // and the skeleton), the same in every run: 126 with the walk of the track list's elements the skeleton made before the
    // tracks' readers moved, 405 with each track read whole (getTracksValue()) and its element written, 130 now (the list of
    // the index's elements, and the open folders').
    CHECK (*allocations <= 130);
#endif
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
        // A notification that changes nothing: the track renamed, the rename undone.
        {
            const phazite::UndoStep step (song, "Rename Track");
            phazite::setTrackName (song, built.tracks[5], "Renamed");
        }

        phazite::undo (song);
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

TEST_CASE ("Performance gate: a device removed on a song of 200 curves of other devices reads no curve and allocates no more than "
           "on one without curves")
{
    // What names the gone forgets the curves of what is gone with the rest, the automation of what is deleted going with it
    // (Mixer.h: forgetWhatNamesTheGone()): it walks the curves' order by their owners' IDs, each looked for among the items
    // still there, which it gathered for the modulations, the sends and the MIDI links already; it reads no curve's points,
    // and allocates nothing for the curves while none is gone. An EQ without curves removed from a song of 40 tracks, without
    // curves and with 200 of the tracks' synths' (5 each): no point read either way (CurveValues.h: countCurvePointsRead()),
    // the song's curves the same value after (none written), and the allocations the same.
    struct Counts
    {
        std::uint64_t pointsRead = 0;
        bool curvesKept = false;
        std::optional<int> allocations {};
    };

    const auto countsFor = [] (int numCurves)
    {
        auto song = phazite::createSong();
        const auto tracks = phazite::ensureTracks (*song, 40);
        std::vector<phazite::DeviceID> synths;
        phazite::DeviceID victim;

        {
            const phazite::UndoStep step (*song, "Devices");

            for (const auto track : tracks)
                synths.push_back (phazite::test::setOwnInstrument (*song, track, dev::Synth::typeID));

            victim = phazite::test::addOwnDevice (*song, tracks.front(), dev::Equalizer::typeID);
        }

        {
            const phazite::UndoStep step (*song, "Curves");

            for (int i = 0; i < numCurves; ++i)
            {
                const auto id = dev::Synth::info.parameters[(size_t) (i % 5)].id;
                phazite::addPoint (*song, { synths[(size_t) (i / 5)], juce::String (id.data(), id.size()) }, { at (0.0), 0.5f });
            }
        }

        REQUIRE (phazite::getCurveKeys (*song).size() == (size_t) numCurves);
        song->getUndoManager().clearUndoHistory();
        const auto before = phazite::getCurvesValue (*song);
        const auto pointsBefore = phazite::countCurvePointsRead();
        Counts counts;
        // The removal runs in every build, counted or not (PerformanceCounts.h).
        counts.allocations = phazite::test::countAllocations ([&song, victim] { phazite::removeDevice (*song, victim); });
        REQUIRE_FALSE (phazite::hasDevice (*song, victim));
        counts.pointsRead = phazite::countCurvePointsRead() - pointsBefore;
        counts.curvesKept = phazite::getCurvesValue (*song).isSameAs (before);
        return counts;
    };

    const auto none = countsFor (0), many = countsFor (200);
    INFO ("no curves: " << none.allocations.value_or (-1) << " allocations; 200 curves: " << many.allocations.value_or (-1));
    CHECK (none.pointsRead == 0);
    CHECK (many.pointsRead == 0);
    CHECK (many.curvesKept);

    if (! none.allocations.has_value() || ! many.allocations.has_value())
        return; // nothing counted on this build (PerformanceCounts.h)

    CHECK (*many.allocations == *none.allocations);
}

TEST_CASE ("Performance gate: the tracks' keeper reads no track whole and walks no element for a volume's drag event, a mute's "
           "click and a row's height drag, and as much for a track added or moved on 20 tracks as on 200")
{
    // While the song's tree holds the tracks, the tracks' part of the keeper tells the values' listeners what the tree tells
    // of them (model/base/internal/TracksArea.h): one property from that one property (no track read whole:
    // countTracksReadAsValues(); no element walked: countTrackElementsWalked()), a track added read alone with what is in it,
    // a track moved within its list by the places the tree names. Counted on songs of 20 and 200 tracks, the same.
    struct Counts
    {
        std::uint64_t whole = 0, walked = 0;
        bool operator== (const Counts&) const = default;
    };

    const auto measure = [] (const std::function<void()>& edit)
    {
        const auto whole = phazite::countTracksReadAsValues();
        const auto walked = phazite::countTrackElementsWalked();
        edit();
        return Counts { phazite::countTracksReadAsValues() - whole, phazite::countTrackElementsWalked() - walked };
    };

    const auto run = [&measure] (int numTracks)
    {
        auto song = phazite::createSong();
        const auto tracks = phazite::ensureTracks (*song, numTracks);
        const auto track = tracks[(size_t) numTracks / 2];
        std::map<std::string, Counts> counts;

        {
            phazite::Gesture gesture (*song, "Drag Volume");
            phazite::setChannelVolume (*song, track, -2.0f);
            counts["a volume's drag event"] = measure ([&] { phazite::setChannelVolume (*song, track, -3.0f); });
            gesture.commit();
        }

        counts["a mute's click"] = measure ([&] { phazite::toggleTracksMuted (*song, { track }); });
        counts["a row's height"] = measure ([&] { phazite::setSavedTrackHeight (*song, track, 100); });
        counts["a track added"] = measure (
            [&]
            {
                const phazite::UndoStep step (*song, "Add Track");
                phazite::insertTrack (*song, track, nullptr);
            });

        counts["a track moved"] = measure ([&] { phazite::moveTracksAfter (*song, { tracks.front() }, tracks.back()); });

        // A folder of 10 tracks moved in the list, and into another folder (JUCE takes it out and puts it in again: it and
        // its tracks read again).
        const auto folder =
            phazite::groupIntoFolder (*song, std::vector<phazite::TrackID> (tracks.begin() + 1, tracks.begin() + 11), nullptr);
        const auto other = phazite::groupIntoFolder (*song, { tracks.back() }, nullptr);
        counts["a folder of 10 moved"] = measure ([&] { phazite::moveTracksAfter (*song, { folder }, other); });
        counts["a folder of 10 into another"] = measure ([&] { phazite::moveTrackIntoFolder (*song, folder, other); });
        phazite::test::checkSong (*song);
        return counts;
    };

    const auto few = run (20);
    const auto many = run (200);

    for (const auto& [what, counted] : many)
    {
        INFO (what);
        MESSAGE (what << ": tracks read whole " << counted.whole << ", elements walked " << counted.walked);
        CHECK (counted == few.at (what));
    }

    for (const auto* what : { "a volume's drag event", "a mute's click", "a row's height" })
    {
        INFO (what);
        CHECK (many.at (what).whole == 0);
        CHECK (many.at (what).walked == 0);
    }

    CHECK (many.at ("a track added").whole == 1);
    CHECK (many.at ("a track moved").whole == 0);
    CHECK (many.at ("a folder of 10 moved").whole == 0);
    CHECK (many.at ("a folder of 10 into another").whole == 11);
}

TEST_CASE ("Performance gate: a mute's click, a row's height drag event and a lane's resize set one entry of their values, read no "
           "track whole and compare no list entry, and allocate as much on 20 tracks as on 200")
{
    // The tracks' switches and view are areas of the song's values (SongValues.h: trackSwitches, tracksOutside): a mute's
    // click sets one entry of the switches (TrackSwitchesValue.h: withMute()), a row's height and a lane's one entry of the
    // view (TracksOutside.h: withTrackView()), each a look in a map, no track read whole (countTracksReadAsValues()), no
    // element of the tracks walked (countTrackElementsWalked()), no entry of the tracks' lists compared
    // (countTrackListEntriesCompared()); and the playlist's rows, rebuilt, read each row's view and frozen switch by a look,
    // no track whole. Counted on songs of 20 and 200 tracks, the same; and their allocations, the same for both and within a
    // bound of each (as measured: a mute's click 15, a height 4, a lane's resize 18).
    struct Counts
    {
        std::uint64_t whole = 0;
        std::uint64_t walked = 0;
        std::uint64_t compared = 0;
        bool operator== (const Counts&) const = default;
    };

    const auto measure = [] (const std::function<void()>& edit)
    {
        const auto whole = phazite::countTracksReadAsValues();
        const auto walked = phazite::countTrackElementsWalked();
        const auto compared = phazite::countTrackListEntriesCompared();
        edit();
        return Counts { .whole = phazite::countTracksReadAsValues() - whole,
                        .walked = phazite::countTrackElementsWalked() - walked,
                        .compared = phazite::countTrackListEntriesCompared() - compared };
    };

    struct Measured
    {
        std::map<std::string, Counts> counts {};
        std::map<std::string, std::optional<int>> allocations {};
        std::uint64_t rowsReadWhole = 0;
    };

    const auto run = [&measure] (int numTracks)
    {
        phazite::test::TestSong s;
        auto& song = getSong (s);
        const auto tracks = phazite::ensureTracks (song, numTracks);
        const auto track = tracks[(size_t) numTracks / 2];
        const phazite::ParameterRef volume { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
        phazite::showLane (song, track, volume);
        Measured measured;

        measured.counts["a mute's click"] = measure ([&] { phazite::toggleTracksMuted (song, { track }); });
        measured.counts["a row's height drag event"] = measure ([&] { phazite::setSavedTrackHeight (song, track, 100); });
        measured.counts["a lane's resize"] = measure ([&] { phazite::setLaneHeight (song, track, volume, 100); });

        // Each counted once more, apart, on what the count runs (PerformanceCounts.h: it runs its work in every build).
        measured.allocations["a mute's click"] = phazite::test::countAllocations ([&] { phazite::toggleTracksMuted (song, { track }); });
        measured.allocations["a row's height drag event"] =
            phazite::test::countAllocations ([&] { phazite::setSavedTrackHeight (song, track, 120); });
        measured.allocations["a lane's resize"] =
            phazite::test::countAllocations ([&] { phazite::setLaneHeight (song, track, volume, 120); });

        CHECK (phazite::isTrackMuted (song, track, false) == false);
        CHECK (phazite::getSavedTrackHeight (song, track) == std::optional (120));
        CHECK (phazite::getLanes (song, track).front().height == 120);

        const auto whole = phazite::countTracksReadAsValues();
        CHECK (phazite::getPlaylistRows (s.context).size() == phazite::getTrackIDs (song).size());
        measured.rowsReadWhole = phazite::countTracksReadAsValues() - whole;
        phazite::test::checkSong (song);
        return measured;
    };

    const auto few = run (20);
    const auto many = run (200);

    for (const auto& [what, counted] : many.counts)
    {
        INFO (what);
        MESSAGE (what << ": tracks read whole " << counted.whole << ", elements walked " << counted.walked << ", list entries compared "
                      << counted.compared);
        CHECK (counted == Counts {});
        CHECK (few.counts.at (what) == Counts {});
    }

    CHECK (few.rowsReadWhole == 0);
    CHECK (many.rowsReadWhole == 0);

    const std::map<std::string, int> bounds { { "a mute's click", 15 }, { "a row's height drag event", 4 }, { "a lane's resize", 18 } };

    for (const auto& [what, allocations] : many.allocations)
    {
        INFO (what);

        if (! allocations.has_value() || ! few.allocations.at (what).has_value())
            continue; // nothing counted on this build (PerformanceCounts.h)

        MESSAGE (what << ": allocations on 20 tracks " << *few.allocations.at (what) << ", on 200 " << *allocations);
        CHECK (*allocations == *few.allocations.at (what));
        CHECK (*allocations <= bounds.at (what));
        CHECK (*few.allocations.at (what) <= bounds.at (what));
    }
}

TEST_CASE ("Performance gate: a song of 200 tracks opened reads each track and the master once as it takes its values")
{
    auto song = phazite::createSong();
    phazite::ensureTracks (*song, 200);
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, phazite::test::copyWholeSong (*song));
    const auto parsed = juce::parseXML (text.toString());
    REQUIRE (parsed != nullptr);
    const auto tree = juce::ValueTree::fromXml (*parsed);
    const auto before = phazite::countTracksReadAsValues();
    const auto opened = phazite::makeSong (tree);
    REQUIRE (opened != nullptr);
    CHECK (phazite::countTracksReadAsValues() - before == phazite::getTrackIDs (*opened).size() + 1);
}

TEST_CASE ("Performance gate: the tracks' keeper compares no entry of the tracks' lists for a volume's drag event, a mute's "
           "click and a rename, at most twice the list's for a track moved in it, and allocates as much for a drag event, on 20 "
           "tracks as on 200")
{
    // What the keeper tells works out a track's place in its list only for one added, taken out or come to another folder
    // (TrackChange::made, place), and a list's order before and after in one pass of each (model/base/TracksArea.cpp:
    // keepOrder()): its count of list entries compared (countTrackListEntriesCompared()). Found 2026-11-23: a volume's drag
    // event and a rename searched their track's list twice (202 entries on 200 tracks), and a track added or deleted compared
    // the list with itself entry by entry (40,601 on 200 tracks, its square); now none, and 503 and 602. And one drag event's
    // allocations (the tree's write, the change told, the tracks' value's path copied): no more on 200 tracks than on 20.
    struct Measured
    {
        std::map<std::string, std::uint64_t> compared {};
        std::optional<int> dragAllocations {};
        size_t listLength = 0;
    };

    const auto compare = [] (const std::function<void()>& edit)
    {
        const auto before = phazite::countTrackListEntriesCompared();
        edit();
        return phazite::countTrackListEntriesCompared() - before;
    };

    const auto run = [&compare] (int numTracks)
    {
        auto song = phazite::createSong();
        const auto tracks = phazite::ensureTracks (*song, numTracks);
        const auto track = tracks[(size_t) numTracks / 2];
        Measured measured;
        measured.listLength = phazite::getTrackIDs (*song).size();

        {
            phazite::Gesture gesture (*song, "Drag Volume");
            phazite::setChannelVolume (*song, track, -2.0f);
            measured.compared["a volume's drag event"] = compare ([&] { phazite::setChannelVolume (*song, track, -3.0f); });
            measured.dragAllocations = phazite::test::countAllocations ([&] { phazite::setChannelVolume (*song, track, -4.0f); });
            gesture.commit();
        }

        measured.compared["a mute's click"] = compare ([&] { phazite::toggleTracksMuted (*song, { track }); });
        measured.compared["a rename"] = compare (
            [&]
            {
                const phazite::UndoStep step (*song, "Rename Track");
                phazite::setTrackName (*song, track, "Renamed");
            });

        measured.compared["a track moved"] = compare ([&] { phazite::moveTracksAfter (*song, { tracks.front() }, tracks.back()); });
        measured.compared["a track moved to the top"] =
            compare ([&] { phazite::moveTracksAfter (*song, { tracks.front() }, phazite::TrackID()); });
        measured.compared["a track added"] = compare (
            [&]
            {
                const phazite::UndoStep step (*song, "Add Track");
                phazite::insertTrack (*song, track, nullptr);
            });

        measured.compared["a track deleted"] = compare ([&] { phazite::deleteTracks (*song, { tracks.back() }, nullptr); });
        phazite::test::checkSong (*song);
        return measured;
    };

    const auto few = run (20);
    const auto many = run (200);

    for (const auto& [what, compared] : many.compared)
        MESSAGE (what << ": list entries compared on 20 tracks " << few.compared.at (what) << ", on 200 " << compared);

    for (const auto* what : { "a volume's drag event", "a mute's click", "a rename" })
    {
        INFO (what);
        CHECK (few.compared.at (what) == 0);
        CHECK (many.compared.at (what) == 0);
    }

    // A track moved in the top list: the list before and after, each stepped through once. A track added or deleted: its
    // place in the list besides (TrackChange::made), the list once more at most.
    for (const auto* measured : { &few, &many })
    {
        INFO (measured->listLength << " tracks");
        CHECK (measured->compared.at ("a track moved") <= 2 * measured->listLength);
        CHECK (measured->compared.at ("a track moved to the top") <= 2 * measured->listLength);
        CHECK (measured->compared.at ("a track added") <= 3 * (measured->listLength + 1));
        CHECK (measured->compared.at ("a track deleted") <= 3 * (measured->listLength + 1));
    }

    if (! few.dragAllocations.has_value() || ! many.dragAllocations.has_value())
        return;

    MESSAGE ("A volume's drag event's allocations on 20 tracks: " << *few.dragAllocations << ", on 200: " << *many.dragAllocations);
    // 15 on 20 tracks and 16 on 200 in the windows-debug build, 2026-11-23 (the tracks' value one node deeper in its map on
    // 200): the same bound for both.
    constexpr auto mostAllocations = 20;
    CHECK (*few.dragAllocations <= mostAllocations);
    CHECK (*many.dragAllocations <= mostAllocations);
}

TEST_CASE ("Performance gate: the check for unsaved changes after a track's rename looks at as many tracks' elements on 20 "
           "tracks as on 200")
{
    // A track that changed is compared with the saved copy's of its ID (Project.h: areTracksAsSaved()): found in the song by
    // the song's index, and in the saved copy by an index made once as the copy is taken, so that one rename looks at one
    // element on each side (Project::countTrackElementsWalkedWithSaved()). Found 2026-11-23: both trees' tracks were indexed
    // by a walk at each comparison.
    const auto run = [] (int numTracks)
    {
        phazite::test::TestSong s;
        auto& song = getSong (s);
        auto& project = s.getProject();
        const auto tracks = phazite::ensureTracks (song, numTracks);
        REQUIRE (project.save().wasOk());
        phazite::test::dispatchPendingMessages();
        REQUIRE_FALSE (project.hasUnsavedChanges());

        const auto before = phazite::Project::countTrackElementsWalkedWithSaved();

        {
            const phazite::UndoStep step (song, "Rename Track");
            phazite::setTrackName (song, tracks[(size_t) numTracks / 2], "Renamed");
        }

        CHECK (project.hasUnsavedChanges());
        return phazite::Project::countTrackElementsWalkedWithSaved() - before;
    };

    const auto few = run (20);
    const auto many = run (200);
    MESSAGE ("Tracks' elements looked at after a rename on 20 tracks: " << few << ", on 200: " << many);
    CHECK (many == few);
    CHECK (many <= 2);
}
