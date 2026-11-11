// SPDX-License-Identifier: AGPL-3.0-or-later
// The example songs (src/model/project/ExampleSongs.h; the owner, 2026-11-01 and 2026-11-04): each opens as a new project
// without undo steps or unsaved changes, plays without clipping and saves the same once it is opened again, and each shows
// what its description says. Exodus has a golden render (GoldenRenderTests.cpp). And the songs written out as WAV files,
// to listen to and to measure (the song-export suite, at the end).
#include "GoldenFiles.h"
#include "RenderHarness.h"
#include "TestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/chords/ChordModel.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/project/ExampleSongs.h"
#include "model/project/Export.h"
#include "model/project/ProjectContents.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TempoModel.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
// An example song as Help > Example Songs makes it: a new project, opened from its first save in a context of its own.
struct Example
{
    explicit Example (const char* id)
    {
        juce::String error;
        project = phazite::Project::create (
            temp.folder, id, error,
            phazite::getExampleSongContent (phazite::test::getEngineContext(), id, phazite::test::getFactoryPresetsFolder()));
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());
        setSong (context, &project->getSong());
    }

    ~Example() { setSong (context, nullptr); }

    // Any of its tracks by name: the returns too, which the playlist shows only while they have automation lanes.
    phazite::TrackID track (const juce::String& name) const
    {
        for (const auto id : phazite::getAllPlaylistTracks (context))
            if (const auto row = phazite::getTrackRow (context, id); row.has_value() && row->name == name)
                return id;

        FAIL ("no track called " << name);
        return {};
    }

    std::vector<phazite::ClipInfo> clips (const juce::String& trackName) const { return phazite::getClips (context, track (trackName)); }

    bool listens (const juce::String& listener, const juce::String& source) const
    {
        const auto connections = phazite::getConnections (context);
        return std::any_of (
            connections.begin(), connections.end(), [this, &listener, &source] (const phazite::Connection& c)
            { return c.kind == phazite::Connection::Kind::sidechain && c.from == track (source) && c.to == track (listener); });
    }

    bool sends (const juce::String& from, const juce::String& to) const
    {
        const auto connections = phazite::getConnections (context);
        return std::any_of (connections.begin(), connections.end(), [this, &from, &to] (const phazite::Connection& c)
                            { return c.kind == phazite::Connection::Kind::send && c.from == track (from) && c.to == track (to); });
    }

    juce::StringArray getSectionNames() const
    {
        juce::StringArray names;

        for (const auto& section : phazite::getSongSections (context))
            names.add (section.name);

        return names;
    }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
};

phazite::DeviceID findDevice (const std::vector<phazite::DeviceSlot>& devices, const juce::String& name)
{
    for (const auto& device : devices)
        if (device.name == name)
            return device.id;

    return {};
}
} // namespace

TEST_CASE ("Every example song opens as a new project without undo steps or unsaved changes, and plays without clipping")
{
    for (const auto& song : phazite::getExampleSongs())
    {
        CAPTURE (std::string (song.id));
        Example example (song.id);
        phazite::test::settle();
        CHECK_FALSE (example.project->hasUnsavedChanges());
        CHECK_FALSE (example.project->getSong().getUndoManager().canUndo());
        CHECK (phazite::findExampleTrackToPlay (example.context, song).isValid());

        // Its clips lie end to end, each with its track's row to itself (none overlap, the samples' copies neither).
        for (const auto track : phazite::getAllPlaylistTracks (example.context))
            for (const auto& subRow : phazite::getClipSubRows (phazite::getClips (example.context, track)))
                CHECK (subRow == phazite::ClipSubRow());

        // Its first seconds: sound, never over full scale.
        const auto audio = phazite::test::makeReferenceEngine()->render (example.context, 6.0);
        CHECK (audio.getMagnitude (0, audio.getNumSamples()) < 1.0f);
        CHECK (audio.getRMSLevel (0, 0, audio.getNumSamples()) > juce::Decibels::decibelsToGain (-40.0f));

        // Saved, opened again and saved again, it is the same.
        const auto file = example.project->getFile();
        REQUIRE (example.project->save().wasOk());
        const auto first = phazite::test::readNormalisedProject (file);
        REQUIRE (first != nullptr);
        setSong (example.context, nullptr);
        example.project = nullptr;
        juce::String error;
        auto reopened = phazite::Project::open (file, false, error);
        REQUIRE_MESSAGE (reopened != nullptr, error.toStdString());
        CHECK_FALSE (reopened->hasUnsavedChanges());
        REQUIRE (reopened->save().wasOk());
        const auto second = phazite::test::readNormalisedProject (file);
        REQUIRE (second != nullptr);
        phazite::test::checkSameProject ("saved again", *first, *second);
    }

    phazite::ProjectContext noProject { phazite::test::getEngineContext() };
    CHECK_FALSE (phazite::addExampleSong (noProject, "noSuchSong", {}));
}

TEST_CASE ("Exodus shows a song by someone else: its notes from a file, its parts in layers on two buses that pump, and filters")
{
    Example example ("exodus");
    auto& context = example.context;
    auto& song = example.project->getSong();
    CHECK (example.getSectionNames().joinIntoString (", ")
           == "Intro, Theme, Breakdown, Build-up, Theme 2, Breakdown 2, Build-up 2, Theme 3, Outro");

    // Its credit, which goes with what is made from it: in its description and in its project's notes.
    CHECK (juce::String (phazite::findExampleSong ("exodus")->description).contains ("XCALIBUR (CC BY 4.0)"));
    CHECK (phazite::getProjectNotes (context).contains ("by XCALIBUR"));
    CHECK (phazite::getProjectNotes (context).contains ("CC BY 4.0"));

    // Its recording's tempo in the tempo lane: up from 50 BPM to 114 over the intro, which the first two themes hold; from
    // 111 up to 260 over each breakdown before it drops to 50 and ramps back up over the build-up; the last theme at 111.
    const auto tempos = phazite::getTempoChanges (context);
    CHECK (tempos.size() == 15);
    REQUIRE_FALSE (tempos.empty());
    CHECK (tempos.front().bpm == doctest::Approx (50.0));
    CHECK (tempos.front().curve == doctest::Approx (phazite::tempoRamp));

    for (const auto& [beat, bpm] :
         { std::pair (16.0, 98.0), std::pair (32.0, 114.0), std::pair (100.0, 114.0), std::pair (128.0, 111.0), std::pair (158.9375, 260.0),
           std::pair (160.0, 50.0), std::pair (176.0, 98.0), std::pair (192.0, 114.0), std::pair (280.0, 114.0), std::pair (288.0, 111.0),
           std::pair (318.9375, 260.0), std::pair (320.0, 50.0), std::pair (352.0, 111.0), std::pair (440.0, 111.0) })
    {
        CAPTURE (beat);
        CHECK (phazite::getTempoAt (context, phazite::BeatPosition::fromBeats (beat)) == doctest::Approx (bpm).epsilon (0.01));
    }

    const auto notesOf = [&example, &context] (const juce::String& trackName)
    {
        std::vector<phazite::ClipID> clips;

        for (const auto& clip : example.clips (trackName))
            clips.push_back (clip.id);

        return phazite::getNotes (context, clips);
    };

    const auto rangeOf = [&notesOf] (const juce::String& trackName)
    {
        auto range = std::pair (127, 0);

        for (const auto& note : notesOf (trackName))
            range = { std::min (range.first, note.pitch), std::max (range.second, note.pitch) };

        return range;
    };

    // The notes of its file (resources/songs/Exodus.mid) that its recording plays, without the project's bars 24 to 32: the
    // drums, each on its own track, with the intro's beat, which the recording has and the project hasn't (31 kicks, 15
    // snares and an open hat); the rolling bass and the sub, both on each chord's root; and the arpeggio's one line on its
    // four layers, the supersaw an octave under the square and the wide saw, the sparkle two octaves over them.
    for (const auto& [name, count] :
         { std::pair ("Kick", 343 + 31), std::pair ("Snare", 179 + 15), std::pair ("Clap", 24), std::pair ("Open Hat", 9 + 1),
           std::pair ("Perc", 24), std::pair ("Rolling Bass", 1612), std::pair ("Sub", 37), std::pair ("Square", 1744),
           std::pair ("Wide Saw", 1744), std::pair ("Supersaw", 1744), std::pair ("Sparkle", 1744) })
    {
        CAPTURE (name);
        CHECK (notesOf (name).size() == (size_t) count);
    }

    // Each drum's track plays its own drum alone, on the kit as it comes, and nothing plays on a track it isn't named for:
    // no drum pad plays on a track of its own away from its notes, and the folder holds no clips.
    for (const auto& [name, pad] :
         { std::pair ("Kick", 36), std::pair ("Snare", 38), std::pair ("Clap", 39), std::pair ("Open Hat", 46), std::pair ("Perc", 37) })
    {
        CAPTURE (name);
        const auto kit = phazite::getInstrument (context, example.track (name));
        REQUIRE (kit.has_value());
        CHECK (kit->name == "Drum Sampler");
        CHECK_FALSE (phazite::findPadTrack (song, kit->id, pad).isValid());

        for (const auto& note : notesOf (name))
            CHECK (note.pitch == pad);
    }

    CHECK (example.clips ("Drums").empty());

    CHECK (rangeOf ("Sub") == std::pair (31, 36)); // G1 to C2
    CHECK (rangeOf ("Rolling Bass") == rangeOf ("Sub"));
    CHECK (rangeOf ("Square") == std::pair (69, 89));
    CHECK (rangeOf ("Wide Saw") == rangeOf ("Square"));
    CHECK (rangeOf ("Supersaw") == std::pair (57, 77));
    CHECK (rangeOf ("Sparkle") == std::pair (93, 113));

    // The drums: the kick on every beat and the snare on two and four, from the song's first bar; an eighth bar's fill has
    // three kicks and an open hat.
    const auto plays = [&notesOf] (const juce::String& track, double beat)
    {
        const auto notes = notesOf (track);
        return std::any_of (notes.begin(), notes.end(),
                            [beat] (const phazite::NoteData& note) { return std::abs (note.start.inBeats() - beat) < 1.0e-6; });
    };

    for (const auto bar : { 0.0, 8.0 })
    {
        CAPTURE (bar);

        for (const auto beat : { 0.0, 1.0, 2.0, 3.0 })
            CHECK (plays ("Kick", bar * 4.0 + beat));

        CHECK (plays ("Snare", bar * 4.0 + 1.0));
        CHECK (plays ("Snare", bar * 4.0 + 3.0));
    }

    CHECK (plays ("Kick", 7.0 * 4.0 + 2.0));
    CHECK_FALSE (plays ("Kick", 7.0 * 4.0 + 3.0));
    CHECK (plays ("Open Hat", 7.0 * 4.0 + 2.5));

    // Its parts in clips that play again as aliases, as patterns do.
    for (const auto* name : { "Kick", "Snare", "Clap", "Rolling Bass", "Square" })
    {
        CAPTURE (name);
        const auto clips = example.clips (name);
        CHECK (std::count_if (clips.begin(), clips.end(), [] (const phazite::ClipInfo& clip) { return clip.linked; }) > 0);
    }

    // The chord lane follows the sub, in F major, the key of its recording, which plays every pitched part a fourth higher
    // than its project: its roots, from B flat, IV, every one in the key.
    const auto chords = phazite::getLaneChords (context);
    CHECK (chords.size() == 37);
    REQUIRE_FALSE (chords.empty());
    CHECK (chords.front().name == "Bb");
    CHECK (phazite::getLaneKeyAt (context, {}) == phazite::Key { 5, phazite::Scale::major });
    CHECK (chords.front().numeral.degree == 4);

    for (const auto& chord : chords)
    {
        CAPTURE (chord.name.toStdString());
        CHECK (chord.numeral.accidental == 0);
    }

    // The mix. The drums in a folder; two buses, the bass's (with the sparkle) and the arpeggio's, each ducking under the
    // Kick track, which is heard. Nothing is muted, nothing else ducks, and nothing sends: every plate is on its track or
    // its bus.
    const auto connections = phazite::getConnections (context);
    const auto outputsTo = [&example, &connections] (const juce::String& from, const juce::String& to)
    {
        return std::any_of (
            connections.begin(), connections.end(), [&example, &from, &to] (const phazite::Connection& c)
            { return c.kind == phazite::Connection::Kind::output && c.from == example.track (from) && c.to == example.track (to); });
    };

    for (const auto* inBassBus : { "Rolling Bass", "Sub", "Sparkle" })
    {
        CAPTURE (inBassBus);
        CHECK (outputsTo (inBassBus, "Bass + Sparkle"));
    }

    for (const auto* inArpBus : { "Square", "Wide Saw", "Supersaw" })
    {
        CAPTURE (inArpBus);
        CHECK (outputsTo (inArpBus, "Arp"));
    }

    for (const auto* drum : { "Kick", "Snare", "Clap", "Open Hat", "Perc" })
    {
        CAPTURE (drum);
        CHECK (outputsTo (drum, "Drums"));
        CHECK_FALSE (phazite::isTrackMuted (song, example.track (drum), false));
    }

    CHECK (example.listens ("Bass + Sparkle", "Kick"));
    CHECK (example.listens ("Arp", "Kick"));
    CHECK (std::count_if (connections.begin(), connections.end(),
                          [] (const phazite::Connection& c) { return c.kind == phazite::Connection::Kind::sidechain; })
           == 2);
    CHECK (std::none_of (connections.begin(), connections.end(),
                         [] (const phazite::Connection& c) { return c.kind == phazite::Connection::Kind::send; }));

    // Each part's effects after its instrument, by their devices' names.
    const auto chainOf = [&example, &context] (const juce::String& trackName)
    {
        juce::StringArray names;

        for (const auto& effect : phazite::getEffects (context, example.track (trackName)))
            names.add (effect.name);

        return names.joinIntoString (", ");
    };

    CHECK (chainOf ("Rolling Bass") == "EQ");
    CHECK (chainOf ("Sub") == "Chorus, EQ");
    CHECK (chainOf ("Sparkle") == "EQ, Reverb");
    CHECK (chainOf ("Bass + Sparkle") == "Reverb, Compressor");
    CHECK (chainOf ("Square") == "Reverb");
    CHECK (chainOf ("Wide Saw") == "Reverb");
    CHECK (chainOf ("Supersaw") == "Limiter, Exciter, Limiter, Reverb");
    CHECK (chainOf ("Arp") == "Compressor, Vinyl");
    CHECK (chainOf ("Drums") == "");
    CHECK (chainOf ("Kick") == "");

    // An LFO pans the sparkle, slowly.
    const auto pan = phazite::getModulations (context, phazite::getPan (context, example.track ("Sparkle")).getRef());
    REQUIRE (pan.size() == 1);
    CHECK (pan.front().amount == doctest::Approx (0.07f));

    // The master: an EQ, a high-pass and a low-pass whose cutoffs sweep in automation lanes, the high-pass switched out
    // through the themes by its mix, and a limiter, each its preset as it comes.
    const auto master = phazite::getEffects (context, {});
    juce::StringArray masterNames;
    std::vector<phazite::DeviceID> filters;

    for (const auto& device : master)
    {
        masterNames.add (device.name);

        if (device.name == "Filter")
            filters.push_back (device.id);
    }

    CHECK (masterNames.joinIntoString (", ").startsWith ("EQ, Filter, Filter, Limiter"));
    REQUIRE (filters.size() == 2);
    // Each sweep a few points with bent lines between them, as the original draws them (three to six points each), not
    // many straight ones: the intro's, the breakdowns' and the build-ups' high-pass, and the low-pass's.
    CHECK (phazite::getAutomationPoints (context, { filters[0], "cutoff" }).size() == 7 + 2 * (5 + 7));
    CHECK (phazite::getAutomationPoints (context, { filters[0], "mix" }).size() == 6);
    CHECK (phazite::getAutomationPoints (context, { filters[1], "cutoff" }).size() == 3 + 2 * 3 + 3 + 3);
    CHECK (phazite::getParameter (context, { findDevice (master, "Limiter"), "gain" }).getValue() == doctest::Approx (8.0f));
}

TEST_CASE ("House Groove shows a bus, samples, a bass and chords that pump under the kick, a breakdown, automation and returns")
{
    Example example ("houseGroove");
    auto& context = example.context;
    CHECK (example.getSectionNames().joinIntoString (", ") == "Intro, Build-up, Drop, Breakdown, Drop 2, Outro");
    CHECK (phazite::getLaneChords (context).size() == 64);

    // The drums mix through a bus with a compressor; its samples are audio clips, the crashes rising into the drops
    // reversed.
    CHECK (phazite::getTrackRow (context, example.track ("Drums"))->kind == phazite::TrackType::folder);
    CHECK (findDevice (phazite::getEffects (context, example.track ("Drums")), "Compressor").isValid());
    CHECK (example.clips ("Top Loop").size() == 40);
    const auto crashes = example.clips ("Crash");
    CHECK (crashes.size() == 5);
    CHECK (std::count_if (crashes.begin(), crashes.end(),
                          [&context] (const phazite::ClipInfo& clip) { return phazite::getAudioClip (context, clip.id)->reversed; })
           == 2);

    // The bass and the chords duck under the beat; the chords' filter opens through the build-up, closes in the breakdown
    // and opens again, and an LFO moves it; the risers' volume rises; a pad plays the breakdown.
    CHECK (example.listens ("Bass", "Beat"));
    CHECK (example.listens ("Chords", "Beat"));
    const auto chords = phazite::getInstrument (context, example.track ("Chords"));
    REQUIRE (chords.has_value());
    const phazite::ParameterRef cutoff { chords->id, "cutoff" };
    CHECK (phazite::getAutomationPoints (context, cutoff).size() == 6);
    CHECK (phazite::getModulations (context, cutoff).size() == 1);
    CHECK (phazite::getAutomationPoints (context, phazite::getVolume (context, example.track ("Riser")).getRef()).size() == 4);
    CHECK_FALSE (example.clips ("Pad").empty());

    // A reverb and a delay on returns, and the master's multiband.
    CHECK (phazite::getTrackRow (context, example.track ("Reverb"))->kind == phazite::TrackType::returnTrack);
    CHECK (example.sends ("Chords", "Reverb"));
    CHECK (example.sends ("Pad", "Reverb"));
    CHECK (example.sends ("Lead", "Reverb"));
    CHECK (example.sends ("Lead", "Delay"));
    CHECK (findDevice (phazite::getEffects (context, {}), "Multiband").isValid());
}

TEST_CASE ("Lo-fi Beat shows a container with macros, a wobbling e-piano, samples, a walking bass and a filter on the master")
{
    Example example ("loFiBeat");
    auto& context = example.context;
    CHECK (example.getSectionNames().size() == 7); // the lo-fi outline's
    CHECK (example.clips ("Vinyl").size() == 12);
    CHECK (example.clips ("Rain").size() == 4);
    CHECK_FALSE (example.clips ("Shaker").empty());

    // Ninth chords on the lane.
    const auto chords = phazite::getLaneChords (context);
    CHECK (std::any_of (chords.begin(), chords.end(), [] (const phazite::LaneChord& chord) { return chord.name.contains ("9"); }));

    // The keys play through a container of tape-like effects, two of its macros named and moving them, and wobble as a tape
    // does: their own LFO on their pitch.
    const auto keys = example.track ("Keys");
    const auto container = findDevice (phazite::getEffects (context, keys), "Tape");
    REQUIRE (container.isValid());
    juce::StringArray macros;

    for (const auto& macro : phazite::getContainerMacros (context, container))
        if (! phazite::getModulations (context, macro.id).empty())
            macros.add (macro.name);

    CHECK (macros.joinIntoString (", ") == "Muffle, Drive");
    const auto piano = phazite::getInstrument (context, keys);
    REQUIRE (piano.has_value());
    CHECK (phazite::getParameter (context, { piano->id, "lfoDepth" }).getValue() > 0.0f);

    // An LFO pans the keys; the drums fade out at the end; the master's filter opens over the intro and closes at the end.
    const auto pan = phazite::getModulations (context, phazite::getPan (context, keys).getRef());
    REQUIRE (pan.size() == 1);
    const auto modulators = phazite::getModulators (context, keys);
    CHECK (std::any_of (modulators.begin(), modulators.end(), [&pan] (const phazite::ModulatorInfo& m)
                        { return m.id == pan.front().modulator && m.type == phazite::ModulatorType::lfo; }));
    CHECK (phazite::getAutomationPoints (context, phazite::getVolume (context, example.track ("Drums")).getRef()).size() == 2);
    const auto masterFilter = findDevice (phazite::getEffects (context, {}), "Filter");
    REQUIRE (masterFilter.isValid());
    CHECK (phazite::getAutomationPoints (context, { masterFilter, "cutoff" }).size() == 4);
    CHECK (example.sends ("Melody", "Delay"));

    // The bass walks: four notes a bar after the intro.
    std::vector<phazite::ClipID> bassClips;

    for (const auto& clip : example.clips ("Bass"))
        bassClips.push_back (clip.id);

    CHECK (phazite::getNotes (context, bassClips).size() == 44 * 4);
}

// Not a test: the example songs written out to listen to and to measure, each its mix and its stems (a file for each channel
// that reaches the master, each alone and lined up: model/project/Export.h), as the preset report writes the presets'
// sounds. ctest and CI skip it; run it from a build in the repository with
//   phazite_tests.exe --test-suite=song-export --no-skip
// with the environment variable PHAZITE_SONG_EXPORT naming the folder, and PHAZITE_SONG a song's id ("exodus") for that
// song alone. Each song gets "<name> - mix.wav" and a folder "<name>" of its stems, at 44.1 kHz in floating point, so that
// nothing in them is clipped or dithered. With PHAZITE_SONG_SOLOS set ("all", or tracks' names between commas), it also
// gets a folder "<name> solos": the mix again with each of those tracks soloed, which is that track as it sounds in the
// mix, through its buses and the master, ducked by what it ducks under.
TEST_CASE ("Song export: each example song's mix and stems as WAV files" * doctest::test_suite ("song-export") * doctest::skip())
{
    const juce::File folder (juce::SystemStats::getEnvironmentVariable ("PHAZITE_SONG_EXPORT", {}));
    REQUIRE_MESSAGE (folder.createDirectory().wasOk(), "set PHAZITE_SONG_EXPORT to the folder for the songs' files");
    const auto only = juce::SystemStats::getEnvironmentVariable ("PHAZITE_SONG", {});
    const auto solos = juce::SystemStats::getEnvironmentVariable ("PHAZITE_SONG_SOLOS", {});
    const auto soloNames = juce::StringArray::fromTokens (solos, ",", {});

    for (const auto& song : phazite::getExampleSongs())
    {
        if (only.isNotEmpty() && only != song.id)
            continue;

        CAPTURE (std::string (song.id));
        Example example (song.id);

        const auto exportAndWait = [&example] (const juce::File& file, const phazite::ExportSettings& settings)
        {
            std::optional<phazite::ExportReport> report;
            const auto running = phazite::startExport (example.project->getSong(), file, settings,
                                                       [&report] (const phazite::ExportReport& r) { report = r; });
            REQUIRE (phazite::test::dispatchMessagesUntil ([&report] { return report.has_value(); }, 1200000));
            return *report;
        };

        phazite::ExportSettings settings;
        settings.bitDepth = 32;
        const auto mix = exportAndWait (folder.getChildFile (juce::String (song.name) + " - mix.wav"), settings);
        CHECK_MESSAGE (mix.result.wasOk(), mix.result.getErrorMessage().toStdString());
        MESSAGE (song.name << ": " << mix.seconds << " s, " << mix.integratedLufs << " LUFS, true peak " << mix.truePeakDb << " dB");

        settings.stems = true;
        const auto stems = exportAndWait (folder.getChildFile (song.name), settings);
        CHECK_MESSAGE (stems.result.wasOk(), stems.result.getErrorMessage().toStdString());

        for (const auto& file : stems.stems)
            MESSAGE ("  " << file.getFileName());

        if (solos.isEmpty())
            continue;

        settings.stems = false;
        auto& made = example.project->getSong();
        const auto soloFolder = folder.getChildFile (juce::String (song.name) + " solos");
        soloFolder.createDirectory();
        auto number = 0;

        for (const auto track : phazite::getAllPlaylistTracks (example.context))
        {
            const auto row = phazite::getTrackRow (example.context, track);
            ++number;

            if (! row.has_value() || phazite::isTrackMuted (made, track, false)
                || (solos != "all" && ! soloNames.contains (row->name.trim())))
                continue;

            phazite::setTrackSoloed (made, track, true);
            const auto name = juce::String (number).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (row->name) + ".wav";
            const auto solo = exportAndWait (soloFolder.getChildFile (name), settings);
            phazite::setTrackSoloed (made, track, false);
            CHECK_MESSAGE (solo.result.wasOk(), solo.result.getErrorMessage().toStdString());
            MESSAGE ("  solo " << name << ": " << solo.integratedLufs << " LUFS");
        }
    }
}
