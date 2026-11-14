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
#include "PerformanceCounts.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <map>

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
    };

    for (const auto& edit : edits)
    {
        INFO (std::string (edit.what));
        edit.make();
        CHECK (plans.take() == edit.plans);
    }
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
        return std::pair (phazite::test::findAll<phazite::ui::ClipComponent> (screen.playlist).size(),
                          phazite::test::findAll<juce::Component> (screen.playlist).size());
    };

    const auto [smallClips, smallComponents] = countFor (20);
    const auto [bigClips, bigComponents] = countFor (200);
    MESSAGE ("20 tracks: " << smallClips << " clip components, " << smallComponents << " components; 200 tracks: " << bigClips
                           << " clip components, " << bigComponents << " components");
    CHECK (smallClips > 0);
    CHECK (bigClips == smallClips);

    // Each track's header is a few components (its name, its switches, its peak bar), made for every track whether in view
    // or not (measured: 7 a track, 2026-11-14): no more than that.
    CHECK (bigComponents - smallComponents <= 7 * 180);
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
        { "a note added", [&] { phazite::addNote (screen.context, clips[1], 60, at (2.0), lasting (1.0), 100); },
          [&] { return screen.boundsOf (clips[1]); },
          // Playlist::songChanged() lays out every clip again and repaints all the lanes for any change in the clips' group of
          // changes, a note's included.
          "every change of a clip repaints all the lanes" },
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
    };

    const auto countsFor = [] (int numTracks)
    {
        phazite::test::TestSong s;
        const auto built = phazite::test::buildBenchmarkSong (s, numTracks, 16.0);
        auto& song = getSong (s);
        REQUIRE (s.getProject().save().wasOk());
        phazite::test::dispatchPendingMessages();

        Counts counts;
        counts.edit = phazite::test::countAllocations (
                          [&]
                          {
                              phazite::addNote (s.context, built.clips[0], 72, at (2.5), lasting (0.25), 100);
                              phazite::test::dispatchPendingMessages();
                          })
                          .value_or (0);
        counts.save = phazite::test::countAllocations ([&] { REQUIRE (s.getProject().save().wasOk()); }).value_or (0);
        counts.text = phazite::test::countAllocations (
                          [&]
                          {
                              juce::MemoryOutputStream text;
                              phazite::writeProjectText (text, song.getState());
                          })
                          .value_or (0);
        return counts;
    };

    // Measured (2026-11-14), 8 / 16 / 32 / 64 tracks: a note added with its new plan 11,305 / 28,562 / 56,232 / 118,593; a
    // save 4,600 / 9,164 / 18,111 / 35,867; the song's text alone 3,355 / 6,768 / 13,463 / 26,736.
    const auto small = countsFor (16), big = countsFor (64); // four times the tracks
    MESSAGE ("Allocations, 16 tracks / 64 tracks: a note added with its new plan "
             << small.edit << " / " << big.edit << "; a save " << small.save << " / " << big.save << "; the song's text alone "
             << small.text << " / " << big.text);

    // Four times the song: about four times the work (4.15 and 3.9 measured), and well short of the 16 times that work which
    // grows as the square of the song makes.
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
        return used;
    };

    // Ten times the points, about ten times the work (a little more for the sorting); never a hundred.
    const auto small = comparisonsFor (1000), large = comparisonsFor (10000);
    INFO ("1,000 points: " << small << " comparisons; 10,000: " << large);
    CHECK (large < small * 20);
}
