// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"

#include <algorithm>

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// A folder in the temp directory that is deleted again at the end of the test.
struct TemporaryFolder
{
    TemporaryFolder() { folder.createDirectory(); }
    ~TemporaryFolder() { folder.deleteRecursively(); }

    const juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteImportTests", {}, false);
};

// A short WAV file of silence, some samples long.
void writeWav (const juce::File& file, int numSamples)
{
    juce::AudioBuffer<float> buffer (1, numSamples);
    buffer.clear();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16)))
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

// A MIDI file with one middle C, a beat long.
void writeMidi (const juce::File& file)
{
    juce::MidiMessageSequence sequence;
    sequence.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0.0);
    sequence.addEvent (juce::MidiMessage::noteOff (1, 60), 960.0);

    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (960);
    midi.addTrack (sequence);

    file.deleteFile();
    juce::FileOutputStream stream (file);
    REQUIRE (midi.writeTo (stream));
}

std::unique_ptr<phazite::Project> createProject (const juce::File& folder)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), folder, "Import", error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    phazite::ensureTracks (project->getEdit(), 1);
    return project;
}
} // namespace

TEST_CASE ("Files are imported by kind: audio, MIDI, or nothing")
{
    CHECK (phazite::getFileKind (juce::File::getCurrentWorkingDirectory().getChildFile ("Kick.wav")) == phazite::FileKind::audio);
    CHECK (phazite::getFileKind (juce::File::getCurrentWorkingDirectory().getChildFile ("Song.mid")) == phazite::FileKind::midi);
    CHECK (phazite::getFileKind (juce::File::getCurrentWorkingDirectory().getChildFile ("Notes.txt")) == phazite::FileKind::other);
}

TEST_CASE ("Imported audio is copied into the project once, and keeps working after reopening")
{
    TemporaryFolder temp;
    const auto outside = temp.folder.getChildFile ("Samples");
    outside.createDirectory();
    const auto kick = outside.getChildFile ("Kick.wav");
    writeWav (kick, 22050); // half a second

    auto project = createProject (temp.folder);
    auto& song = phazite::getSong (project->getEdit());
    const auto track = phazite::getTrackID (*te::getAudioTracks (project->getEdit())[0]);

    const auto clip = phazite::importFile (song, track, kick, beats (4.0));
    REQUIRE (clip.isValid());
    const auto clipState = phazite::findClipState (song, clip);
    REQUIRE (phazite::isAudioState (clipState));
    CHECK (clipState[phazite::schema::clipName].toString() == juce::String ("Kick"));
    CHECK (phazite::getClipRange (song, clip).getStart().inBeats() == doctest::Approx (4.0));
    const auto played = phazite::getPlayedPosition (song, clipState);
    CHECK (played.end - played.start == doctest::Approx (0.5));
    CHECK (phazite::getClipFile (song, clipState) == project->getAudioFolder().getChildFile ("Kick.wav"));

    // The project stores where the audio is relative to itself, so it can move.
    CHECK_FALSE (juce::File::isAbsolutePath (clipState[phazite::schema::clipFile].toString()));

    // The same file again uses the same copy; another file of that name gets a name of its own.
    CHECK (phazite::copyIntoProject (song, kick) == project->getAudioFolder().getChildFile ("Kick.wav"));
    const auto otherKick = temp.folder.getChildFile ("Kick.wav");
    writeWav (otherKick, 1000);
    const auto otherCopy = phazite::copyIntoProject (song, otherKick);
    CHECK (otherCopy.isAChildOf (project->getAudioFolder()));
    CHECK (otherCopy.getFileName() != juce::String ("Kick.wav"));

    // A file in the project already isn't copied again.
    CHECK (phazite::copyIntoProject (song, otherCopy) == otherCopy);

    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    juce::String error;
    auto reopened = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (reopened != nullptr);
    const auto& reopenedSong = phazite::getSong (reopened->getEdit());
    const auto clips = phazite::getClips (reopenedSong, phazite::getTrackID (*te::getAudioTracks (reopened->getEdit())[0]));
    REQUIRE (clips.size() == 1);
    const auto reopenedClip = phazite::findClipState (reopenedSong, clips.front());
    REQUIRE (phazite::isAudioState (reopenedClip));
    CHECK (phazite::getClipFile (reopenedSong, reopenedClip).existsAsFile());
}

TEST_CASE ("A MIDI file becomes a MIDI clip where it is dropped")
{
    TemporaryFolder temp;
    const auto midiFile = temp.folder.getChildFile ("Chords.mid");
    writeMidi (midiFile);

    auto project = createProject (temp.folder.getChildFile ("Projects"));
    auto& song = phazite::getSong (project->getEdit());
    const auto track = phazite::getTrackID (*te::getAudioTracks (project->getEdit())[0]);

    const auto clip = phazite::importFile (song, track, midiFile, beats (8.0));
    REQUIRE (clip.isValid());
    REQUIRE (phazite::isMidiState (phazite::findClipState (song, clip)));
    CHECK (phazite::getClipRange (song, clip).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getClipNotes (song, clip).size() == 1);

    CHECK_FALSE (phazite::importFile (song, track, temp.folder.getChildFile ("Notes.txt"), beats (0.0)).isValid());

    // The first track with notes, named as the file names it, with its controllers, to the end of the bar it ends in.
    {
        juce::MidiMessageSequence empty, notes;
        empty.addEvent (juce::MidiMessage::textMetaEvent (3, "Tempo track"), 0.0);
        notes.addEvent (juce::MidiMessage::textMetaEvent (3, "Bassline"), 0.0);
        notes.addEvent (juce::MidiMessage::controllerEvent (1, 74, 100), 480.0);
        notes.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 90), 960.0);
        notes.addEvent (juce::MidiMessage::noteOff (1, 40), 4800.0);
        juce::MidiFile midi;
        midi.setTicksPerQuarterNote (960);
        midi.addTrack (empty);
        midi.addTrack (notes);
        const auto file = temp.folder.getChildFile ("Bass.mid");
        juce::FileOutputStream stream (file);
        REQUIRE (midi.writeTo (stream));
    }

    const auto bass = phazite::importFile (song, track, temp.folder.getChildFile ("Bass.mid"), beats (16.0));
    REQUIRE (bass.isValid());
    const auto clipState = phazite::findClipState (song, bass);
    CHECK (clipState[phazite::schema::clipName].toString() == juce::String ("Bassline"));
    CHECK ((double) clipState[phazite::schema::clipStart] == doctest::Approx (16.0));
    CHECK ((double) clipState[phazite::schema::clipLength] == doctest::Approx (8.0)); // the note ends in the second bar
    const auto note = clipState.getChildWithName (phazite::schema::NOTE);
    CHECK ((double) note[phazite::schema::noteStart] == doctest::Approx (1.0));
    CHECK ((double) note[phazite::schema::noteLength] == doctest::Approx (4.0));
    CHECK ((int) note[phazite::schema::noteVelocity] == 90);
    CHECK (note.hasProperty (phazite::schema::noteID));
    const auto control = clipState.getChildWithName (phazite::schema::CONTROL);
    CHECK ((int) control[phazite::schema::controlNumber] == 74);
    CHECK ((int) control[phazite::schema::controlValue] == 100);
    CHECK ((double) control[phazite::schema::controlBeat] == doctest::Approx (0.5));
}

TEST_CASE ("Track colors follow the words in a file's name")
{
    const auto colourNamed = [] (const char* name)
    {
        for (const auto& colour : phazite::getTrackColours())
            if (juce::String (colour.name) == name)
                return colour.colour;

        return juce::Colour();
    };

    CHECK (phazite::getColourForName ("Kick 01") == colourNamed ("Vermilion"));
    CHECK (phazite::getColourForName ("OpenHat_02") == colourNamed ("Vermilion"));
    CHECK (phazite::getColourForName ("808 Bass") == colourNamed ("Blue")); // bass before drums
    CHECK (phazite::getColourForName ("Lead Synth") == colourNamed ("Sky blue"));
    CHECK (phazite::getColourForName ("Vox Chop") == colourNamed ("Reddish purple"));
    CHECK_FALSE (phazite::getColourForName ("Recording 3").has_value());
}

TEST_CASE ("Dropped files go on the track that suits them, or on new tracks below it, as one undo step")
{
    TemporaryFolder temp;
    const auto kick = temp.folder.getChildFile ("Kick.wav");
    const auto chords = temp.folder.getChildFile ("Chords.mid");
    writeWav (kick, 22050);
    writeMidi (chords);

    auto project = createProject (temp.folder.getChildFile ("Project"));
    auto& edit = project->getEdit();
    auto& song = phazite::getSong (edit);
    const auto target = phazite::getTrackID (*te::getAudioTracks (edit)[0]);
    song.getUndoManager().clearUndoHistory();

    // The audio suits the empty track; the MIDI file gets a new track below it, named after the file. A file that is
    // neither is left out.
    const auto added = phazite::importFiles (
        song, { kick.getFullPathName(), chords.getFullPathName(), temp.folder.getChildFile ("Notes.txt").getFullPathName() }, target,
        beats (4.0));
    REQUIRE (added.size() == 2);
    CHECK (phazite::getClipTrack (song, added[0]) == target);
    CHECK (phazite::isMidiState (phazite::findClipState (song, added[1])));
    CHECK (phazite::getClipRange (song, added[1]).getStart().inBeats() == doctest::Approx (4.0));

    const auto midiTrack = phazite::getClipTrack (song, added[1]);
    const auto tracks = phazite::getTrackIDs (song);
    const auto indexOf = [&tracks] (phazite::TrackID track) { return std::find (tracks.begin(), tracks.end(), track) - tracks.begin(); };
    CHECK (phazite::getTrackName (song, midiTrack) == juce::String ("Chords"));
    CHECK (indexOf (midiTrack) == indexOf (target) + 1);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Add Files"));

    // MIDI doesn't go on a track with audio on it.
    const auto more = phazite::importFiles (song, { chords.getFullPathName() }, target, beats (0.0));
    REQUIRE (more.size() == 1);
    CHECK (phazite::getClipTrack (song, more[0]) != target);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Add File"));

    phazite::undo (edit);
    CHECK (phazite::getPlaylistTracks (edit).size() == 2);
}
