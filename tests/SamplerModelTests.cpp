// SPDX-License-Identifier: AGPL-3.0-or-later
// The sampler in a project (src/model/devices/internal/SamplerSound.h): a sound file put on it as one undo step, copied
// into the project, its root key found from the file's own note, a note in its name in the octave of its pitch, or its
// pitch, its fine taking back how far off the note it is, its loop from its smpl chunk; one of its factory sounds; the
// engine playing what the song names; slices moved, added and taken away by hand, which stay whatever slices the sound;
// its slices written as notes into a clip that plays the loop in time; and presets that carry its sound.
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/Sampler.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"

#include <unordered_map>

namespace
{
using phazite::devices::Sampler;

// A sampler on a project's first track, without an undo history.
struct Screen
{
    Screen()
    {
        sampler = song.setInstrument (song.firstTrack, Sampler::typeID);
        REQUIRE (sampler.isValid());
        getSong().getUndoManager().clearUndoHistory();
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }
    float getValue (const char* id) const { return phazite::getValueSetByHand (getSong(), phazite::ParameterRef { sampler, id }); }
    juce::String describeUndo() const { return phazite::describeUndo (&getSong()); }

    juce::var getSound (const juce::Identifier& property) const
    {
        return phazite::findDeviceState (getSong(), sampler).getChildWithName (phazite::schema::SOUNDS)[property];
    }

    // What Phazite's engine plays.
    const Sampler& getPlayed() const
    {
        phazite::getSession (song.context)->update();
        const auto* played = phazite::makeDeviceHandle (getSong(), sampler).getProcessor<Sampler>();
        REQUIRE (played != nullptr);
        return *played;
    }

    phazite::test::TemporaryFolder temp; // for sound files outside the project
    phazite::test::TestSong song;
    phazite::DeviceID sampler;
};

// A WAV file of a sine at a frequency, with its smpl chunk's note and loop where given.
juce::File writeTone (const juce::File& file, double frequency, double seconds, std::optional<int> unityNote = {},
                      std::optional<std::pair<int, int>> loop = {})
{
    constexpr double rate = 44100.0;
    juce::AudioBuffer<float> buffer (1, (int) (seconds * rate));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate));

    std::unordered_map<juce::String, juce::String> metadata;

    if (unityNote.has_value() || loop.has_value())
    {
        metadata["MidiUnityNote"] = juce::String (unityNote.value_or (60));
        metadata["NumSampleLoops"] = loop.has_value() ? "1" : "0";

        if (loop.has_value())
        {
            metadata["Loop0Type"] = "0";
            metadata["Loop0Start"] = juce::String (loop->first);
            metadata["Loop0End"] = juce::String (loop->second);
        }
    }

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream,
        juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24).withMetadataValues (metadata));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return file;
}
} // namespace

TEST_CASE ("A new sampler plays the 808 that comes with it, at its root key, C2")
{
    Screen screen;
    CHECK (phazite::isSampler (screen.getSong(), screen.sampler));
    CHECK (phazite::getSampleName (screen.getSong(), screen.sampler) == juce::String ("808 Bass"));
    CHECK_FALSE (phazite::hasSampleFile (screen.getSong(), screen.sampler));
    CHECK (phazite::getFactorySampleIndex (screen.getSong(), screen.sampler) == 0);
    CHECK (screen.getValue ("root") == 36.0f);
    CHECK (screen.getPlayed().getSound() == phazite::devices::getFactorySample (0).get());
}

TEST_CASE ("A sound file put on the sampler is one undo step, copied into the project, at the root key of its pitch")
{
    Screen screen;
    auto& song = screen.getSong();
    const auto tone = writeTone (screen.temp.folder.getChildFile ("Lead Tone.wav"), 220.0, 1.0); // A3, note 57

    REQUIRE (phazite::setSampleFile (song, screen.sampler, tone));
    CHECK (screen.describeUndo() == juce::String ("Undo Put Lead Tone on the Sampler"));
    CHECK (phazite::getSampleName (song, screen.sampler) == juce::String ("Lead Tone"));
    CHECK (screen.getValue ("root") == 57.0f);
    CHECK (std::abs (screen.getValue ("fine")) < 3.0f);
    CHECK (screen.getSound (phazite::schema::samplerSampleFile).toString().startsWith ("Audio/"));
    CHECK (phazite::findProjectFile (song, screen.getSound (phazite::schema::samplerSampleFile).toString()).existsAsFile());

    // The engine plays the file.
    const auto* played = screen.getPlayed().getSound();
    REQUIRE (played != nullptr);
    CHECK (played->sampleRate == 44100.0);
    CHECK (played->audio.getNumSamples() == 44100);

    // Undo puts the 808 back, at its root.
    phazite::undo (song);
    CHECK (phazite::getSampleName (song, screen.sampler) == juce::String ("808 Bass"));
    CHECK (screen.getValue ("root") == 36.0f);
    CHECK (screen.getPlayed().getSound() == phazite::devices::getFactorySample (0).get());

    // What isn't a sound changes nothing.
    const auto text = screen.temp.folder.getChildFile ("Notes.txt");
    text.replaceWithText ("not a sound");
    CHECK_FALSE (phazite::setSampleFile (song, screen.sampler, text));
}

TEST_CASE ("A sound's root key comes from its file's own note, else a note in its name in its pitch's octave, else its pitch")
{
    phazite::test::TemporaryFolder temp;

    // Its smpl chunk's note and loop.
    const auto withChunk =
        phazite::readSampleSettings (writeTone (temp.folder.getChildFile ("A.wav"), 220.0, 1.0, 62, std::pair { 4410, 39689 }));
    REQUIRE (withChunk.has_value());
    CHECK (withChunk->root == 62);
    CHECK (withChunk->rootFrom == phazite::SampleSettings::RootFrom::file);
    REQUIRE (withChunk->loop.has_value());
    CHECK (withChunk->loop->first == doctest::Approx (0.1f).epsilon (0.001));
    CHECK (withChunk->loop->second == doctest::Approx (0.9f).epsilon (0.001));

    // A note in its name, in the octave its pitch is in: an 808 at F#1 named "F#".
    const auto named = phazite::readSampleSettings (writeTone (temp.folder.getChildFile ("808 Long F#.wav"), 46.25, 1.0));
    REQUIRE (named.has_value());
    CHECK (named->root == 30);
    CHECK (named->rootFrom == phazite::SampleSettings::RootFrom::name);

    // Its pitch alone, with Fine taking back how far off the note it is: 30 cents sharp of A3.
    const auto sharp =
        phazite::readSampleSettings (writeTone (temp.folder.getChildFile ("Tone.wav"), 220.0 * std::pow (2.0, 0.3 / 12.0), 1.0));
    REQUIRE (sharp.has_value());
    CHECK (sharp->root == 57);
    CHECK (sharp->rootFrom == phazite::SampleSettings::RootFrom::pitch);
    CHECK (sharp->fine == doctest::Approx (-30.0f).epsilon (0.1));

    // Nothing to go by: middle C.
    juce::AudioBuffer<float> noise (1, 44100);
    juce::Random random (5);

    for (int i = 0; i < noise.getNumSamples(); ++i)
        noise.setSample (0, i, random.nextFloat() - 0.5f);

    const auto noiseFile = temp.folder.getChildFile ("Noise.wav");
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (noiseFile);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        writer->writeFromAudioSampleBuffer (noise, 0, noise.getNumSamples());
    }
    const auto plain = phazite::readSampleSettings (noiseFile);
    REQUIRE (plain.has_value());
    CHECK (plain->root == 60);
    CHECK (plain->rootFrom == phazite::SampleSettings::RootFrom::none);
}

TEST_CASE ("A sound with a loop of its own puts the loop on the sampler; one of its factory sounds is one undo step")
{
    Screen screen;
    auto& song = screen.getSong();
    REQUIRE (phazite::setSampleFile (song, screen.sampler,
                                     writeTone (screen.temp.folder.getChildFile ("Pad.wav"), 220.0, 1.0, 57, std::pair { 22050, 44099 })));
    CHECK (screen.getValue ("loopOn") == 1.0f);
    CHECK (screen.getValue ("loopStart") == doctest::Approx (0.5f).epsilon (0.001));
    CHECK (screen.getValue ("loopEnd") == doctest::Approx (1.0f).epsilon (0.001));

    phazite::setFactorySample (song, screen.sampler, phazite::devices::findFactorySample ("808 Kick"));
    CHECK (screen.describeUndo() == juce::String ("Undo Put 808 Kick on the Sampler"));
    CHECK (phazite::getSampleName (song, screen.sampler) == juce::String ("808 Kick"));
    CHECK_FALSE (phazite::hasSampleFile (song, screen.sampler));
    CHECK (screen.getValue ("loopOn") == 0.0f);
    CHECK (screen.getValue ("root") == (float) phazite::devices::getFactorySampleRoot (phazite::devices::findFactorySample ("808 Kick")));
}

TEST_CASE ("Slices moved, added and taken away by hand stay whatever slices the sound, each one undo step")
{
    Screen screen;
    auto& song = screen.getSong();
    const auto loop = phazite::test::writeClicks (screen.temp.folder, 8, 120.0); // two bars, a click a beat
    REQUIRE (phazite::setSampleFile (song, screen.sampler, loop));
    screen.song.setParameter (screen.sampler, "mode", (float) Sampler::Mode::slice);
    screen.song.setParameter (screen.sampler, "sensitivity", 1.0f);

    auto positions = phazite::getSlicePositions (song, screen.sampler);
    REQUIRE (positions.size() == 7); // a slice at each click after the first

    // Added by hand: there, and still there by beats.
    phazite::addSlice (song, screen.sampler, 0.06f);
    CHECK (screen.describeUndo() == juce::String ("Undo Add Slice"));
    positions = phazite::getSlicePositions (song, screen.sampler);
    REQUIRE (positions.size() == 8);
    CHECK (positions[0] == doctest::Approx (0.06f).epsilon (0.01));
    screen.song.setParameter (screen.sampler, "sliceBy", (float) Sampler::SliceBy::equal);
    screen.song.setParameter (screen.sampler, "sliceCount", 2.0f);
    CHECK (phazite::getSlicePositions (song, screen.sampler).size() == 2); // the half, and the one by hand

    // Taken away: a slice it found stays away at any sensitivity.
    screen.song.setParameter (screen.sampler, "sliceBy", (float) Sampler::SliceBy::transients);
    phazite::removeSlice (song, screen.sampler, 3); // the second click's
    CHECK (screen.describeUndo() == juce::String ("Undo Remove Slice"));
    CHECK (phazite::getSlicePositions (song, screen.sampler).size() == 7);
    screen.song.setParameter (screen.sampler, "sensitivity", 0.9f);
    screen.song.setParameter (screen.sampler, "sensitivity", 1.0f);
    CHECK (phazite::getSlicePositions (song, screen.sampler).size() == 7);

    // Moved: one found goes where it is put, by hand.
    const auto before = phazite::getSlicePositions (song, screen.sampler);
    phazite::moveSlice (song, screen.sampler, 4, before[3] + 0.02f);
    CHECK (screen.describeUndo() == juce::String ("Undo Move Slice"));
    const auto after = phazite::getSlicePositions (song, screen.sampler);
    REQUIRE (after.size() == 7);
    CHECK (after[3] == doctest::Approx (before[3] + 0.02f).epsilon (0.01));

    // The engine plays them; undo takes a change back.
    REQUIRE (screen.getPlayed().getManualSlices() != nullptr);
    CHECK (screen.getPlayed().getManualSlices()->added.size() == 2);
    phazite::undo (song);
    CHECK (phazite::getSlicePositions (song, screen.sampler) == before);
    CHECK (screen.getPlayed().getManualSlices()->added.size() == 1);

    // Another sound starts with no slices changed by hand.
    REQUIRE (phazite::setSampleFile (song, screen.sampler, writeTone (screen.temp.folder.getChildFile ("Other.wav"), 220.0, 1.0)));
    CHECK (screen.getSound (phazite::schema::samplerSlices).toString().isEmpty());
    CHECK (screen.getSound (phazite::schema::samplerSlicesRemoved).toString().isEmpty());
}

TEST_CASE ("Slices to Clip writes a slice's note at each slice's beat, so that the clip plays the loop in time with the song")
{
    Screen screen;
    auto& song = screen.getSong();
    const auto loop = phazite::test::writeClicks (screen.temp.folder, 4, 120.0); // a bar, its four beats
    REQUIRE (phazite::setSampleFile (song, screen.sampler, loop));
    screen.song.setParameter (screen.sampler, "mode", (float) Sampler::Mode::slice);
    screen.song.setParameter (screen.sampler, "sensitivity", 1.0f);
    screen.song.setTempo (90.0); // the loop's beats follow the song's

    const auto clip = phazite::writeSliceNotes (song, screen.sampler, nullptr);
    REQUIRE (clip.isValid());
    CHECK (screen.describeUndo() == juce::String ("Undo Slices to Clip"));
    CHECK (phazite::getClipTrack (song, clip) == screen.song.firstTrack);

    const auto notes = phazite::getClipNotes (song, clip);
    REQUIRE (notes.size() == 4);

    for (size_t i = 0; i < notes.size(); ++i)
    {
        CAPTURE (i);
        CHECK (notes[i].pitch == Sampler::firstSliceNote + (int) i);
        CHECK (notes[i].start.inBeats() == doctest::Approx ((double) i).epsilon (0.02));
    }

    // Another in the way: after it, on a bar.
    const auto second = phazite::writeSliceNotes (song, screen.sampler, nullptr);
    CHECK (phazite::getClipRange (song, second).getStart().inBeats() >= phazite::getClipRange (song, clip).getEnd().inBeats());
}

TEST_CASE ("A sampler's preset carries its sound file to another project, and its factory sound by name")
{
    Screen screen;
    auto& song = screen.getSong();
    REQUIRE (phazite::setSampleFile (song, screen.sampler, writeTone (screen.temp.folder.getChildFile ("Vox.wav"), 330.0, 0.5)));

    auto state = phazite::findDeviceState (song, screen.sampler).createCopy();
    phazite::writeDeviceFilePaths (song, state);
    const auto path = state.getChildWithName (phazite::schema::SOUNDS)[phazite::schema::samplerSampleFile].toString();
    CHECK (juce::File::isAbsolutePath (path));
    CHECK (juce::File (path).existsAsFile());

    const auto properties = phazite::getSoundProperties (Sampler::info);
    CHECK (std::find (properties.begin(), properties.end(), phazite::schema::samplerSample) != properties.end());
    CHECK (std::find (properties.begin(), properties.end(), phazite::schema::samplerSlices) != properties.end());
    CHECK (phazite::schema::isSoundFileProperty (phazite::schema::samplerSampleFile));
}
