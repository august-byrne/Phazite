// SPDX-License-Identifier: AGPL-3.0-or-later
// Consolidate (src/model/mixer/FreezeModel.h, src/model/arrangement/internal/Consolidate.h; docs/recording-audit.md), as
// Ableton Live's: the selected clips on each track, or what lies in a time range selected across tracks, joined into one
// clip, a MIDI clip of the notes as they play, an audio file rendered before the track's devices, as one undo step.
#include "TestClips.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/stereo/StereoExpander.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::test::at;
using phazite::test::lasting;

// A saved project at 120 BPM (a beat is half a second) with two tracks, Keys and Bass, open in a context of its own.
struct Song
{
    Song()
    {
        juce::String error;
        project = phazite::Project::create (temp.folder, "Song", error);
        REQUIRE (project != nullptr);
        auto& song = getSong();
        const auto tracks = phazite::ensureTracks (song, 2);
        keys = tracks[0];
        bass = tracks[1];
        phazite::setTrackName (song, keys, "Keys");
        phazite::setTrackName (song, bass, "Bass");
        REQUIRE (project->save().wasOk());
        song.getUndoManager().clearUndoHistory();
        setSong (context, &song);
    }

    ~Song()
    {
        host.render = nullptr;
        setSong (context, nullptr);
    }

    phazite::Song& getSong() const { return project->getSong(); }

    // The notes of a track's clips, each as a song beat, a length and a pitch, in time order.
    std::vector<phazite::NoteData> getNotes (phazite::TrackID track) const
    {
        std::vector<phazite::NoteData> notes;

        for (const auto clip : phazite::getClips (getSong(), track))
            for (const auto& note : phazite::getClipNotes (getSong(), clip))
                notes.push_back (note);

        std::sort (notes.begin(), notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
        return notes;
    }

    // Consolidates the selection with its command's function, as the app does, and waits for a render.
    void consolidate()
    {
        host.finished.clear();
        phazite::consolidateSelection (context, host);

        if (host.render != nullptr)
            REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! host.finished.isEmpty(); }, 60000));
    }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::test::RenderHost host;
    phazite::TrackID keys, bass;
};

// The RMS level of a stretch of a file's left channel, in seconds.
double getLevel (const juce::File& file, double start, double end)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    const auto first = juce::roundToInt (start * reader->sampleRate);
    return audio.getRMSLevel (0, first, juce::roundToInt (end * reader->sampleRate) - first);
}
} // namespace

TEST_CASE ("Consolidate joins a track's selected MIDI clips into one, their notes as they play, as one undo step")
{
    Song song;
    auto& model = song.getSong();

    // A clip from beat 0 to 4 whose second note rings past its end (the clip grows with it, and is trimmed back), and one
    // from 8 to 12 trimmed at its start, so that its first note starts before it and plays from its start.
    const auto first = phazite::test::addMidiClip (model, song.keys, { at (0.0), at (4.0) });
    phazite::test::addMidiNote (model, first, 60, at (1.0), lasting (1.0), 90);
    phazite::test::addMidiNote (model, first, 62, at (3.5), lasting (2.0));
    phazite::trimClipEnd (model, first, at (4.0));
    const auto second = phazite::test::addMidiClip (model, song.keys, { at (6.0), at (12.0) });
    phazite::test::addMidiNote (model, second, 64, at (1.5), lasting (1.0));
    phazite::test::addMidiNote (model, second, 65, at (3.0), lasting (1.0));
    phazite::trimClipStart (model, second, at (8.0)); // its note at beat 7.5 now plays from 8
    REQUIRE (phazite::getClipRange (model, first) == phazite::BeatRange (at (0.0), at (4.0)));
    REQUIRE (phazite::getClipRange (model, second) == phazite::BeatRange (at (8.0), at (12.0)));
    model.getUndoManager().clearUndoHistory();

    auto& selection = getSelection (song.context);
    selection.selectOnly (first);
    selection.addToSelection (second);
    REQUIRE (phazite::canConsolidateSelection (song.context));
    CHECK_FALSE (phazite::doesConsolidateRender (song.context)); // notes alone: no render
    song.consolidate();
    CHECK (song.host.render == nullptr);

    // One clip from the first's start to the last's end, selected, its notes where and as long as they played.
    const auto clips = phazite::getClips (model, song.keys);
    REQUIRE (clips.size() == 1);
    CHECK (phazite::getClipRange (model, clips[0]) == phazite::BeatRange (at (0.0), at (12.0)));
    CHECK (selection.getClips() == std::vector { clips[0] });

    const auto notes = song.getNotes (song.keys);
    REQUIRE (notes.size() == 4);
    CHECK (notes[0].start == at (1.0));
    CHECK (notes[0].length == lasting (1.0));
    CHECK (notes[0].velocity == 90);
    CHECK (notes[1].start == at (3.5));
    CHECK (notes[1].length == lasting (0.5)); // cut where its clip ended
    CHECK (notes[2].pitch == 64);
    CHECK (notes[2].start == at (8.0)); // from where its clip started
    CHECK (notes[2].length == lasting (0.5));
    CHECK (notes[3].start == at (9.0));

    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Consolidate"));
    phazite::undo (model);
    CHECK (phazite::getClips (model, song.keys).size() == 2);
}

TEST_CASE ("Consolidate renders a track's audio clips before its devices into one file in the Audio folder")
{
    Song song;
    auto& model = song.getSong();

    // A sine from the start for 1.5 s, and again from 2 s quieter, on Bass through an expander that turns it up 6 dB.
    const auto sound = phazite::test::writeSound (song.temp.folder.getChildFile ("Sine.wav"), 2.0);
    const auto loud = phazite::test::addAudioClip (model, song.bass, "Sine", sound, 0.0, 1.5);
    const auto quiet = phazite::test::addAudioClip (model, song.bass, "Sine", sound, 2.0, 1.5);
    phazite::setClipGain (model, quiet, -6.0f);
    REQUIRE (phazite::insertDeviceState (
                 model, song.bass, phazite::makeOwnDeviceState (model, phazite::devices::StereoExpander::typeID, { { "gain", 6.0f } }))
                 .isValid());
    model.getUndoManager().clearUndoHistory();

    auto& selection = getSelection (song.context);
    selection.selectOnly (loud);
    selection.addToSelection (quiet);
    CHECK (phazite::doesConsolidateRender (song.context));
    song.consolidate();
    REQUIRE (song.host.finished == juce::StringArray { "Consolidating: ok" });

    // One audio clip from the first's start to the last's end, named as they were, its file in the project's Audio folder.
    const auto clips = phazite::getClips (model, song.bass);
    REQUIRE (clips.size() == 1);
    const auto clipState = phazite::findClipState (model, clips[0]);
    REQUIRE (phazite::isAudioState (clipState));
    CHECK (phazite::getClipRange (model, clipState).getStart() == at (0.0));
    CHECK (phazite::getClipRange (model, clipState).getEnd().inBeats() == doctest::Approx (7.0).epsilon (0.001));
    CHECK (phazite::getClipName (model, clipState) == "Sine");
    const auto file = phazite::getClipFile (model, clipState);
    CHECK (file.getParentDirectory() == song.project->getAudioFolder());
    CHECK (file.getFileName() == "Bass (Consolidated).wav");

    // What the clips played, with the second's gain, without the expander, which still plays after it; silence between.
    const auto sine = juce::Decibels::gainToDecibels (0.25 / std::sqrt (2.0));
    CHECK (juce::Decibels::gainToDecibels (getLevel (file, 0.1, 1.4)) == doctest::Approx (sine).epsilon (0.05));
    CHECK (getLevel (file, 1.55, 1.95) < 1.0e-4);
    CHECK (juce::Decibels::gainToDecibels (getLevel (file, 2.1, 3.4)) == doctest::Approx (sine - 6.0).epsilon (0.05));
    CHECK (phazite::getDevices (model, song.bass).size() == 1);
    CHECK (selection.getClips() == std::vector { clips[0] });

    // One undo step, which brings both clips back.
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Consolidate"));
    phazite::undo (model);
    CHECK (phazite::getClips (model, song.bass).size() == 2);
}

TEST_CASE ("With a time range selected across tracks, Consolidate joins what lies in it on each, cut at its edges")
{
    Song song;
    auto& model = song.getSong();

    // On each track a clip from 0 to 8 with a note on every beat; the range from beat 2 to 6 over both.
    for (const auto track : { song.keys, song.bass })
    {
        const auto clip = phazite::test::addMidiClip (model, track, { at (0.0), at (8.0) });

        for (int beat = 0; beat < 8; ++beat)
            phazite::test::addMidiNote (model, clip, 60, at (beat), lasting (0.5));
    }

    model.getUndoManager().clearUndoHistory();
    auto& range = getRangeSelection (song.context);
    range.set ({ at (2.0), at (6.0) }, { song.keys, song.bass });
    song.consolidate();

    // Each track: the part before, the joined clip over the range with the four notes in it, and the part after; the range
    // stays.
    for (const auto track : { song.keys, song.bass })
    {
        CAPTURE (phazite::getTrackName (model, track));
        auto clips = phazite::getClips (model, track);
        REQUIRE (clips.size() == 3);
        std::sort (clips.begin(), clips.end(), [&model] (auto a, auto b)
                   { return phazite::getClipRange (model, a).getStart() < phazite::getClipRange (model, b).getStart(); });
        CHECK (phazite::getClipRange (model, clips[0]) == phazite::BeatRange (at (0.0), at (2.0)));
        CHECK (phazite::getClipRange (model, clips[1]) == phazite::BeatRange (at (2.0), at (6.0)));
        CHECK (phazite::getClipRange (model, clips[2]) == phazite::BeatRange (at (6.0), at (8.0)));

        auto joined = phazite::getClipNotes (model, clips[1]);
        std::sort (joined.begin(), joined.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
        REQUIRE (joined.size() == 4);

        for (size_t i = 0; i < joined.size(); ++i)
            CHECK (joined[i].start == at (2.0 + (double) i));
    }

    CHECK (range.getRange() == phazite::BeatRange (at (2.0), at (6.0)));
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Consolidate"));
}

TEST_CASE ("With nothing selected, Consolidate can't, and says why")
{
    Song song;
    phazite::test::addMidiClip (song.getSong(), song.keys, { at (0.0), at (4.0) });
    CHECK_FALSE (phazite::canConsolidateSelection (song.context));
    song.consolidate();
    CHECK (song.host.finished == juce::StringArray { "Consolidating: Select the clips to join, or a time range across tracks." });
}
