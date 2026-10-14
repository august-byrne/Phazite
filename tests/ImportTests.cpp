// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Tracks.h"
#include "model/project/Project.h"

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
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
    project->getEdit().ensureNumberOfAudioTracks (1);
    return project;
}
} // namespace

TEST_CASE ("Files are imported by kind: audio, MIDI, or nothing")
{
    auto& engine = phazite::test::getEngine();
    CHECK (phazite::getFileKind (engine, juce::File::getCurrentWorkingDirectory().getChildFile ("Kick.wav")) == phazite::FileKind::audio);
    CHECK (phazite::getFileKind (engine, juce::File::getCurrentWorkingDirectory().getChildFile ("Song.mid")) == phazite::FileKind::midi);
    CHECK (phazite::getFileKind (engine, juce::File::getCurrentWorkingDirectory().getChildFile ("Notes.txt")) == phazite::FileKind::other);
}

TEST_CASE ("Imported audio is copied into the project once, and keeps working after reopening")
{
    TemporaryFolder temp;
    const auto outside = temp.folder.getChildFile ("Samples");
    outside.createDirectory();
    const auto kick = outside.getChildFile ("Kick.wav");
    writeWav (kick, 22050); // half a second

    auto project = createProject (temp.folder);
    auto* track = te::getAudioTracks (project->getEdit())[0];

    auto* clip = dynamic_cast<te::WaveAudioClip*> (phazite::importFile (*track, kick, beats (4.0)));
    REQUIRE (clip != nullptr);
    CHECK (clip->getName() == juce::String ("Kick"));
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (clip->getPosition().getLength().inSeconds() == doctest::Approx (0.5));
    CHECK (clip->getAudioFile().getFile() == project->getAudioFolder().getChildFile ("Kick.wav"));

    // The project stores where the audio is relative to itself, so it can move.
    CHECK_FALSE (juce::File::isAbsolutePath (clip->state[te::IDs::source].toString()));

    // The same file again uses the same copy; another file of that name gets a name of its own.
    CHECK (phazite::copyIntoProject (project->getEdit(), kick) == project->getAudioFolder().getChildFile ("Kick.wav"));
    const auto otherKick = temp.folder.getChildFile ("Kick.wav");
    writeWav (otherKick, 1000);
    const auto otherCopy = phazite::copyIntoProject (project->getEdit(), otherKick);
    CHECK (otherCopy.isAChildOf (project->getAudioFolder()));
    CHECK (otherCopy.getFileName() != juce::String ("Kick.wav"));

    // A file in the project already isn't copied again.
    CHECK (phazite::copyIntoProject (project->getEdit(), otherCopy) == otherCopy);

    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    juce::String error;
    auto reopened = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (reopened != nullptr);
    const auto clips = te::getAudioTracks (reopened->getEdit())[0]->getClips();
    REQUIRE (clips.size() == 1);
    auto* reopenedClip = dynamic_cast<te::WaveAudioClip*> (clips.getFirst());
    REQUIRE (reopenedClip != nullptr);
    CHECK (reopenedClip->getAudioFile().getFile().existsAsFile());
}

TEST_CASE ("A MIDI file becomes a MIDI clip where it is dropped")
{
    TemporaryFolder temp;
    const auto midiFile = temp.folder.getChildFile ("Chords.mid");
    writeMidi (midiFile);

    auto project = createProject (temp.folder.getChildFile ("Projects"));
    auto* track = te::getAudioTracks (project->getEdit())[0];

    auto* clip = dynamic_cast<te::MidiClip*> (phazite::importFile (*track, midiFile, beats (8.0)));
    REQUIRE (clip != nullptr);
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (clip->getSequence().getNumNotes() == 1);

    CHECK (phazite::importFile (*track, temp.folder.getChildFile ("Notes.txt"), beats (0.0)) == nullptr);
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
