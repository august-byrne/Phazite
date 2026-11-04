// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth in a project (src/model/devices/internal/Wavetables.h): one of Phazite's instruments; what each
// oscillator plays, kept in the project and changed as undo steps; sound files copied into the project and read as Serum
// writes them; and presets that carry them to another project.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Wavetables.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
using phazite::devices::WavetableSynth;

// A project in a folder of its own, open in a context, with the wavetable synth on its first track and no undo history.
struct Song
{
    Song (const juce::File& parent, const juce::String& name)
    {
        juce::String error;
        project = phazite::Project::create (parent, name, error);
        REQUIRE (project != nullptr);
        auto& song = getSong();
        track = phazite::ensureTracks (song, 1)[0];
        setSong (context, &song);
        synth = phazite::setInstrument (song, track, phazite::makeOwnDeviceState (song, WavetableSynth::typeID));
        REQUIRE (synth.isValid());
        REQUIRE (phazite::isWavetableSynth (song, synth));
        phazite::test::settle();
        song.getUndoManager().clearUndoHistory();
    }

    ~Song() { setSong (context, nullptr); }

    phazite::Song& getSong() const { return project->getSong(); }
    juce::String describeUndo() const { return phazite::describeUndo (&getSong()); }

    // The table an oscillator plays as Phazite's engine plays the song.
    juce::String getPlayedTable (int osc) const
    {
        phazite::getSession (context)->update();
        const auto* played = phazite::makeDeviceHandle (getSong(), synth).getProcessor<WavetableSynth>();
        REQUIRE (played != nullptr);
        const auto table = played->getTable (osc);
        return table != nullptr ? table->getName() : juce::String();
    }

    // What an oscillator's file is, as the synth's SOUNDS in the song keep it.
    juce::var getSound (const juce::Identifier& property) const
    {
        return phazite::findDeviceState (getSong(), synth).getChildWithName (phazite::schema::SOUNDS)[property];
    }

    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::TrackID track;
    phazite::DeviceID synth;
};

int factory (const char* name)
{
    return phazite::devices::findFactoryWavetable (name);
}

// A WAV file of 32-bit float samples, with Serum's 'clm ' chunk giving its frames' length.
juce::File writeSerumWavetable (const juce::File& file, const std::vector<float>& samples, int frameLength)
{
    juce::MemoryOutputStream data;
    const auto writeChunk = [&data] (const char* id, const juce::MemoryBlock& body)
    {
        data.write (id, 4);
        data.writeInt ((int) body.getSize());
        data.write (body.getData(), body.getSize());

        if (body.getSize() % 2 == 1)
            data.writeByte (0);
    };

    juce::MemoryOutputStream format;
    format.writeShort (3); // IEEE float
    format.writeShort (1); // mono
    format.writeInt (44100);
    format.writeInt (44100 * 4);
    format.writeShort (4);
    format.writeShort (32);

    const juce::String clm ("<!>" + juce::String (frameLength) + " 10000000 wavetable (www.xferrecords.com)");
    juce::MemoryBlock audio (samples.data(), samples.size() * sizeof (float));

    data.write ("WAVE", 4);
    writeChunk ("fmt ", format.getMemoryBlock());
    writeChunk ("clm ", juce::MemoryBlock (clm.toRawUTF8(), clm.getNumBytesAsUTF8()));
    writeChunk ("data", audio);

    juce::FileOutputStream out (file);
    REQUIRE (out.openedOk());
    out.setPosition (0);
    out.truncate();
    out.write ("RIFF", 4);
    out.writeInt ((int) data.getDataSize());
    out.write (data.getData(), data.getDataSize());
    return file;
}
} // namespace

TEST_CASE ("The wavetable synth is one of Phazite's instruments, after the synth")
{
    const auto own = phazite::getOwnDevices();
    REQUIRE (own.size() > 2);
    CHECK (own[1] == &WavetableSynth::info);

    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Instruments");
    auto found = false;

    for (const auto& type : phazite::getDeviceTypes (song.context))
        found = found || (type.name == "Wavetable" && type.isInstrument && type.isBuiltIn);

    CHECK (found);
    CHECK (phazite::getWavetableName (song.getSong(), song.synth, 0) == juce::String ("Basic Shapes"));
    CHECK (song.getPlayedTable (1) == juce::String ("Basic Shapes"));
}

TEST_CASE ("What a wavetable oscillator plays is kept in the project, and choosing it is an undo step")
{
    phazite::test::TemporaryFolder temp;
    juce::File file;
    phazite::TrackID track;

    {
        Song song (temp.folder, "Tables");
        auto& s = song.getSong();
        track = song.track;
        phazite::setFactoryWavetable (s, song.synth, 0, factory ("Vowels"));
        CHECK (phazite::getWavetableName (s, song.synth, 0) == juce::String ("Vowels"));
        CHECK (song.getPlayedTable (0) == juce::String ("Vowels"));
        CHECK (song.describeUndo() == juce::String ("Undo Put Vowels on Osc 1"));

        // The one it plays already: no step.
        s.getUndoManager().clearUndoHistory();
        phazite::setFactoryWavetable (s, song.synth, 0, factory ("Vowels"));
        CHECK_FALSE (s.getUndoManager().canUndo());

        // Undo and redo, as the oscillator plays them too.
        phazite::setFactoryWavetable (s, song.synth, 1, factory ("Growl"));
        phazite::undo (s);
        CHECK (song.getPlayedTable (1) == juce::String ("Basic Shapes"));
        phazite::redo (s);
        CHECK (song.getPlayedTable (1) == juce::String ("Growl"));

        REQUIRE (song.project->save().wasOk());
        file = song.project->getFile();
    }

    // Opened again, it plays them.
    juce::String error;
    auto project = phazite::Project::open (file, false, error);
    REQUIRE (project != nullptr);
    auto& reopened = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &reopened);
    phazite::getSession (context)->update();

    const auto synth = phazite::getInstrument (reopened, track);
    REQUIRE (phazite::isWavetableSynth (reopened, synth));
    CHECK (phazite::getWavetableName (reopened, synth, 0) == juce::String ("Vowels"));
    const auto* played = phazite::makeDeviceHandle (reopened, synth).getProcessor<WavetableSynth>();
    REQUIRE (played != nullptr);
    CHECK (played->getTable (1)->getName() == juce::String ("Growl"));
    setSong (context, nullptr);
}

TEST_CASE ("A sound file becomes a wavetable oscillator's table, the project playing its own copy")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Files");
    auto& s = song.getSong();
    const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Elsewhere/Buzz.wav"), 0.2);

    REQUIRE (phazite::setWavetableFile (s, song.synth, 1, sound));
    CHECK (song.describeUndo() == juce::String ("Undo Put Buzz on Osc 2"));
    CHECK (phazite::hasWavetableFile (s, song.synth, 1));
    CHECK (phazite::getWavetableName (s, song.synth, 1) == juce::String ("Buzz"));
    CHECK (song.getSound (phazite::schema::getOscillatorWavetableFile (1)).toString() == juce::String ("Audio/Buzz.wav"));
    CHECK (song.project->getAudioFolder().getChildFile ("Buzz.wav").existsAsFile());

    // 0.2 s at 44.1 kHz is 8820 samples: its first four whole frames of 2048.
    const auto table = phazite::getWavetable (s, song.synth, 1);
    REQUIRE (table != nullptr);
    CHECK (table->getName() == juce::String ("Buzz"));
    CHECK (table->getNumFrames() == 4);

    // A factory table in its place drops the file; undo puts it back.
    phazite::setFactoryWavetable (s, song.synth, 1, factory ("Fold"));
    CHECK_FALSE (phazite::hasWavetableFile (s, song.synth, 1));
    CHECK (song.getPlayedTable (1) == juce::String ("Fold"));
    phazite::undo (s);
    CHECK (song.getPlayedTable (1) == juce::String ("Buzz"));

    // Not a sound: nothing changes.
    s.getUndoManager().clearUndoHistory();
    const auto text = temp.folder.getChildFile ("notes.txt");
    REQUIRE (text.replaceWithText ("not a sound"));
    CHECK_FALSE (phazite::setWavetableFile (s, song.synth, 0, text));
    CHECK_FALSE (s.getUndoManager().canUndo());

    // A file that isn't there any more: the oscillator plays its factory table.
    phazite::findDeviceState (s, song.synth)
        .getOrCreateChildWithName (phazite::schema::SOUNDS, nullptr)
        .setProperty (phazite::schema::getOscillatorWavetableFile (0), "Audio/Gone.wav", nullptr);
    CHECK (song.getPlayedTable (0) == juce::String ("Basic Shapes"));
}

TEST_CASE ("A WAV file's 'clm ' chunk, as Serum writes it, says how long its frames are")
{
    phazite::test::TemporaryFolder temp;

    // Eight frames of 512 samples, each a sine of a harmonic more: as plain frames of 2048 they would be two.
    std::vector<float> samples;

    for (int frame = 0; frame < 8; ++frame)
        for (int n = 0; n < 512; ++n)
            samples.push_back (0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * (frame + 1) * n / 512));

    const auto file = writeSerumWavetable (temp.folder.getChildFile ("Serum.wav"), samples, 512);
    const auto table = phazite::readWavetable (file);
    REQUIRE (table != nullptr);
    CHECK (table->getNumFrames() == 8);
    CHECK (table->getName() == juce::String ("Serum"));

    // Without the chunk's help, a sound becomes whole frames of 2048.
    const auto plain = phazite::readWavetable (phazite::test::writeSound (temp.folder.getChildFile ("Plain.wav"), 0.1));
    REQUIRE (plain != nullptr);
    CHECK (plain->getNumFrames() == 2);

    CHECK (phazite::readWavetable (temp.folder.getChildFile ("Nothing.wav")) == nullptr);
}

TEST_CASE ("A preset of the wavetable synth brings its sound file into the project that loads it")
{
    phazite::test::TemporaryFolder temp;
    const auto presetFile = temp.folder.getChildFile ("Buzzy Lead" + juce::String (phazite::presetExtension));

    {
        Song first (temp.folder, "First");
        auto& s = first.getSong();
        REQUIRE (phazite::setWavetableFile (s, first.synth, 0, phazite::test::writeSound (temp.folder.getChildFile ("Buzz.wav"), 0.2)));
        phazite::setFactoryWavetable (s, first.synth, 1, factory ("Comb"));
        REQUIRE (phazite::saveDevicePreset (first.context, first.synth, presetFile));
    }

    // The preset holds the file by its full path, to copy in, and the factory table by its name.
    const auto preset = phazite::readPreset (presetFile);
    REQUIRE (preset.isObject());
    const auto sounds = preset[phazite::schema::preset::device][phazite::schema::preset::sounds];
    CHECK (juce::File::isAbsolutePath (sounds["osc1WavetableFile"].toString()));
    CHECK (sounds["osc2Wavetable"].toString() == juce::String ("Comb"));

    Song second (temp.folder, "Second");
    auto& s = second.getSong();
    REQUIRE (phazite::loadPreset (second.context, presetFile, second.track));

    // It takes the track's instrument's place, playing its own copy of the file.
    const auto loaded = phazite::getInstrument (s, second.track);
    REQUIRE (phazite::isWavetableSynth (s, loaded));
    const auto loadedSounds = phazite::findDeviceState (s, loaded).getChildWithName (phazite::schema::SOUNDS);
    CHECK (loadedSounds[phazite::schema::getOscillatorWavetableFile (0)].toString() == juce::String ("Audio/Buzz.wav"));
    CHECK (second.project->getAudioFolder().getChildFile ("Buzz.wav").existsAsFile());
    phazite::getSession (second.context)->update();
    CHECK (phazite::getWavetable (s, loaded, 0)->getName() == juce::String ("Buzz"));
    CHECK (phazite::getWavetable (s, loaded, 1)->getName() == juce::String ("Comb"));
}
