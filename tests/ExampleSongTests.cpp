// SPDX-License-Identifier: AGPL-3.0-or-later
// The example songs (src/model/project/ExampleSongs.h; the owner, 2026-11-01): each opens as a new project without undo
// steps or unsaved changes and plays without clipping, and House Groove and Lo-fi Beat show what their descriptions say.
// First Beat has a golden render (GoldenRenderTests.cpp) and a golden project (GoldenProjectTests.cpp).
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
#include "model/project/ExampleSongs.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

#include <algorithm>

namespace
{
// An example song as Help > Example Songs makes it: a new project, opened from its first save in a context of its own.
struct Example
{
    explicit Example (const char* id)
    {
        juce::String error;
        project = phazite::Project::create (
            phazite::test::getEngine(), temp.folder, id, error,
            phazite::getExampleSongContent (phazite::test::getEngineContext(), id, phazite::test::getFactoryPresetsFolder()));
        REQUIRE_MESSAGE (project != nullptr, error.toStdString());
        setEdit (context, &project->getEdit());
    }

    ~Example() { setEdit (context, nullptr); }

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
        CHECK_FALSE (phazite::getSongUndoManager (example.project->getEdit()).canUndo());
        CHECK (phazite::findExampleTrackToPlay (example.context, song).isValid());

        // Its clips lie end to end, each with its track's row to itself (none overlap, the samples' copies neither).
        for (const auto track : phazite::getAllPlaylistTracks (example.context))
            for (const auto& subRow : phazite::getClipSubRows (phazite::getClips (example.context, track)))
                CHECK (subRow == phazite::ClipSubRow());

        // Its first seconds: sound, never over full scale.
        const auto audio = phazite::test::makeReferenceEngine()->render (example.context, 6.0);
        CHECK (audio.getMagnitude (0, audio.getNumSamples()) < 1.0f);
        CHECK (audio.getRMSLevel (0, 0, audio.getNumSamples()) > juce::Decibels::decibelsToGain (-40.0f));
    }

    phazite::ProjectContext noProject { phazite::test::getEngineContext() };
    CHECK_FALSE (phazite::addExampleSong (noProject, "noSuchSong", {}));
}

TEST_CASE ("House Groove shows a bus, samples, a bass that ducks under the kick, automation, an LFO and returns")
{
    Example example ("houseGroove");
    auto& context = example.context;
    CHECK (example.getSectionNames().joinIntoString (", ") == "Intro, Build-up, Drop, Outro");
    CHECK (phazite::getLaneChords (context).size() == 40);

    // The drums mix through a bus with a compressor; its samples are audio clips, one of them reversed.
    CHECK (phazite::getTrackRow (context, example.track ("Drums"))->kind == phazite::TrackType::folder);
    CHECK (findDevice (phazite::getEffects (context, example.track ("Drums")), "Compressor").isValid());
    CHECK (example.clips ("Top Loop").size() == 16);
    const auto crashes = example.clips ("Crash");
    CHECK (crashes.size() == 3);
    CHECK (std::count_if (crashes.begin(), crashes.end(),
                          [&context] (const phazite::ClipInfo& clip) { return phazite::getAudioClip (context, clip.id)->reversed; })
           == 1);

    // The bass ducks under the beat; the chords' filter opens through the build-up and an LFO moves it; the riser's volume
    // rises.
    CHECK (example.listens ("Bass", "Beat"));
    const auto chords = phazite::getInstrument (context, example.track ("Chords"));
    REQUIRE (chords.has_value());
    const phazite::ParameterRef cutoff { chords->id, "cutoff" };
    CHECK (phazite::getAutomationPoints (context, cutoff).size() == 2);
    CHECK (phazite::getModulations (context, cutoff).size() == 1);
    CHECK (phazite::getAutomationPoints (context, phazite::getVolume (context, example.track ("Riser")).getRef()).size() == 2);

    // A reverb and a delay on returns, and the master's multiband.
    CHECK (phazite::getTrackRow (context, example.track ("Reverb"))->kind == phazite::TrackType::returnTrack);
    CHECK (example.sends ("Chords", "Reverb"));
    CHECK (example.sends ("Lead", "Reverb"));
    CHECK (example.sends ("Lead", "Delay"));
    CHECK (findDevice (phazite::getEffects (context, {}), "Multiband").isValid());
}

TEST_CASE ("Lo-fi Beat shows a container with macros, an LFO on the pan, samples, and the drums fading out")
{
    Example example ("loFiBeat");
    auto& context = example.context;
    CHECK (example.getSectionNames().size() == 7); // the lo-fi outline's
    CHECK (example.clips ("Vinyl").size() == 12);
    CHECK_FALSE (example.clips ("Shaker").empty());

    // The keys play through a container of tape-like effects, two of its macros named and moving them.
    const auto keys = example.track ("Keys");
    const auto container = findDevice (phazite::getEffects (context, keys), "Tape");
    REQUIRE (container.isValid());
    juce::StringArray macros;

    for (const auto& macro : phazite::getContainerMacros (context, container))
        if (! phazite::getModulations (context, macro.id).empty())
            macros.add (macro.name);

    CHECK (macros.joinIntoString (", ") == "Muffle, Drive");

    // An LFO pans the keys; the drums fade out at the end.
    const auto pan = phazite::getModulations (context, phazite::getPan (context, keys).getRef());
    REQUIRE (pan.size() == 1);
    const auto modulators = phazite::getModulators (context, keys);
    CHECK (std::any_of (modulators.begin(), modulators.end(), [&pan] (const phazite::ModulatorInfo& m)
                        { return m.id == pan.front().modulator && m.type == phazite::ModulatorType::lfo; }));
    CHECK (phazite::getAutomationPoints (context, phazite::getVolume (context, example.track ("Drums")).getRef()).size() == 2);
    CHECK (example.sends ("Melody", "Delay"));
}
