// SPDX-License-Identifier: AGPL-3.0-or-later
// The example songs (src/model/project/ExampleSongs.h; the owner, 2026-11-01 and 2026-11-04): each opens as a new project
// without undo steps or unsaved changes, plays without clipping and saves the same once it is opened again, and each shows
// what its description says. Trap Beat has a golden render (GoldenRenderTests.cpp).
#include "GoldenFiles.h"
#include "RenderHarness.h"
#include "TestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/chords/ChordModel.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/project/ExampleSongs.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>
#include <cmath>

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

TEST_CASE ("Trap Beat shows half-time drums with hi-hat rolls, an 808 that glides on slide notes, bells and returns")
{
    Example example ("trapBeat");
    auto& context = example.context;
    CHECK (example.getSectionNames().joinIntoString (", ") == "Intro, Hook, Verse, Hook 2, Outro");
    CHECK (phazite::getLaneChords (context).size() == 56);

    const auto notesOf = [&example, &context] (const juce::String& trackName)
    {
        std::vector<phazite::ClipID> clips;

        for (const auto& clip : example.clips (trackName))
            clips.push_back (clip.id);

        return phazite::getNotes (context, clips);
    };

    // The snare on each bar's third beat (half time), and hats in rolls: some a sixth of a beat apart (triplet sixteenths),
    // some an eighth (32nds).
    constexpr int snare = 38, closedHat = 42;
    const auto drums = notesOf ("Drums");
    CHECK (std::all_of (drums.begin(), drums.end(), [] (const phazite::NoteData& note)
                        { return note.pitch != snare || std::abs (std::fmod (note.start.inBeats(), 4.0) - 2.0) < 1.0e-6; }));

    const auto hasHatsApart = [&drums] (double apart)
    {
        for (const auto& a : drums)
            for (const auto& b : drums)
                if (a.pitch == closedHat && b.pitch == closedHat && std::abs ((b.start - a.start).inBeats() - apart) < 1.0e-6)
                    return true;

        return false;
    };

    CHECK (hasHatsApart (1.0 / 6.0));
    CHECK (hasHatsApart (1.0 / 8.0));

    // The 808 glides on slide notes, which play nothing themselves, between the notes it plays.
    const auto bass = notesOf ("808");
    CHECK (std::any_of (bass.begin(), bass.end(), [] (const phazite::NoteData& note) { return note.slide; }));
    CHECK (std::any_of (bass.begin(), bass.end(), [] (const phazite::NoteData& note) { return ! note.slide; }));

    // The bells through a plate and a delay, the pad through the plate; a crash sample at each part after the intro, a
    // pluck in the verse, and the master's limiter.
    CHECK (example.sends ("Bells", "Reverb"));
    CHECK (example.sends ("Bells", "Delay"));
    CHECK (example.sends ("Pad", "Reverb"));
    CHECK (example.clips ("Crash").size() == 3);
    CHECK_FALSE (example.clips ("Pluck").empty());
    CHECK (findDevice (phazite::getEffects (context, {}), "Limiter").isValid());
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
