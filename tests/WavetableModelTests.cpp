// SPDX-License-Identifier: AGPL-3.0-or-later
// The wavetable synth in a project (src/model/devices/internal/WavetablePlugin.h and Wavetables.h): one of Phazite's
// instruments; what each oscillator plays, kept in the project and changed as undo steps; sound files copied into the
// project and read as Serum writes them; and presets that carry them to another project.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/WavetablePlugin.h"
#include "model/devices/internal/Wavetables.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/presets/PresetModel.h"
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
        project = phazite::Project::create (phazite::test::getEngine(), parent, name, error);
        REQUIRE (project != nullptr);
        edit().ensureNumberOfAudioTracks (1);
        setEdit (context, &edit());
        synth =
            phazite::setInstrument (*te::getAudioTracks (edit())[0], edit().getPluginCache().createNewPlugin (WavetableSynth::typeID, {}));
        REQUIRE (synth != nullptr);
        REQUIRE (phazite::isWavetableSynth (*synth));
        phazite::test::settle();
        edit().getUndoManager().clearUndoHistory();
    }

    ~Song() { setEdit (context, nullptr); }

    te::Edit& edit() const { return project->getEdit(); }
    juce::String describeUndo() const { return phazite::describeUndo (&edit()); }

    juce::String getPlayedTable (int osc) const
    {
        const auto table = phazite::getWavetable (*synth, osc);
        return table != nullptr ? table->getName() : juce::String();
    }

    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    te::Plugin* synth = nullptr;
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
    CHECK (phazite::getWavetableName (*song.synth, 0) == juce::String ("Basic Shapes"));
    CHECK (song.getPlayedTable (1) == juce::String ("Basic Shapes"));
}

TEST_CASE ("What a wavetable oscillator plays is kept in the project, and choosing it is an undo step")
{
    phazite::test::TemporaryFolder temp;
    juce::File file;

    {
        Song song (temp.folder, "Tables");
        phazite::setFactoryWavetable (*song.synth, 0, factory ("Vowels"));
        CHECK (phazite::getWavetableName (*song.synth, 0) == juce::String ("Vowels"));
        CHECK (song.getPlayedTable (0) == juce::String ("Vowels"));
        CHECK (song.describeUndo() == juce::String ("Undo Put Vowels on Osc 1"));

        // The one it plays already: no step.
        song.edit().getUndoManager().clearUndoHistory();
        phazite::setFactoryWavetable (*song.synth, 0, factory ("Vowels"));
        CHECK_FALSE (song.edit().getUndoManager().canUndo());

        // Undo and redo, as the oscillator plays them too.
        phazite::setFactoryWavetable (*song.synth, 1, factory ("Growl"));
        phazite::undo (song.edit());
        CHECK (song.getPlayedTable (1) == juce::String ("Basic Shapes"));
        phazite::redo (song.edit());
        CHECK (song.getPlayedTable (1) == juce::String ("Growl"));

        REQUIRE (song.project->save().wasOk());
        file = song.project->getFile();
    }

    // Opened again, it plays them.
    juce::String error;
    auto project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
    REQUIRE (project != nullptr);
    te::Plugin* synth = nullptr;

    for (auto* plugin : te::getAllPlugins (project->getEdit(), false))
        if (phazite::isWavetableSynth (*plugin))
            synth = plugin;

    REQUIRE (synth != nullptr);
    CHECK (phazite::getWavetableName (*synth, 0) == juce::String ("Vowels"));
    CHECK (phazite::getWavetable (*synth, 1)->getName() == juce::String ("Growl"));
}

TEST_CASE ("A sound file becomes a wavetable oscillator's table, the project playing its own copy")
{
    phazite::test::TemporaryFolder temp;
    Song song (temp.folder, "Files");
    const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Elsewhere/Buzz.wav"), 0.2);

    REQUIRE (phazite::setWavetableFile (*song.synth, 1, sound));
    CHECK (song.describeUndo() == juce::String ("Undo Put Buzz on Osc 2"));
    CHECK (phazite::hasWavetableFile (*song.synth, 1));
    CHECK (phazite::getWavetableName (*song.synth, 1) == juce::String ("Buzz"));
    CHECK (song.synth->state["osc2WavetableFile"].toString() == juce::String ("Audio/Buzz.wav"));
    CHECK (song.project->getAudioFolder().getChildFile ("Buzz.wav").existsAsFile());

    // 0.2 s at 44.1 kHz is 8820 samples: its first four whole frames of 2048.
    const auto table = phazite::getWavetable (*song.synth, 1);
    REQUIRE (table != nullptr);
    CHECK (table->getName() == juce::String ("Buzz"));
    CHECK (table->getNumFrames() == 4);

    // A factory table in its place drops the file; undo puts it back.
    phazite::setFactoryWavetable (*song.synth, 1, factory ("Fold"));
    CHECK_FALSE (phazite::hasWavetableFile (*song.synth, 1));
    CHECK (song.getPlayedTable (1) == juce::String ("Fold"));
    phazite::undo (song.edit());
    CHECK (song.getPlayedTable (1) == juce::String ("Buzz"));

    // Not a sound: nothing changes.
    song.edit().getUndoManager().clearUndoHistory();
    const auto text = temp.folder.getChildFile ("notes.txt");
    REQUIRE (text.replaceWithText ("not a sound"));
    CHECK_FALSE (phazite::setWavetableFile (*song.synth, 0, text));
    CHECK_FALSE (song.edit().getUndoManager().canUndo());

    // A file that isn't there any more: the oscillator plays its factory table.
    song.synth->state.setProperty ("osc1WavetableFile", "Audio/Gone.wav", nullptr);
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
    const auto table = phazite::readWavetable (phazite::test::getEngine(), file);
    REQUIRE (table != nullptr);
    CHECK (table->getNumFrames() == 8);
    CHECK (table->getName() == juce::String ("Serum"));

    // Without the chunk's help, a sound becomes whole frames of 2048.
    const auto plain =
        phazite::readWavetable (phazite::test::getEngine(), phazite::test::writeSound (temp.folder.getChildFile ("Plain.wav"), 0.1));
    REQUIRE (plain != nullptr);
    CHECK (plain->getNumFrames() == 2);

    CHECK (phazite::readWavetable (phazite::test::getEngine(), temp.folder.getChildFile ("Nothing.wav")) == nullptr);
}

TEST_CASE ("A preset of the wavetable synth brings its sound file into the project that loads it")
{
    phazite::test::TemporaryFolder temp;
    const auto presetFile = temp.folder.getChildFile ("Buzzy Lead" + juce::String (phazite::presetExtension));

    {
        Song first (temp.folder, "First");
        REQUIRE (phazite::setWavetableFile (*first.synth, 0, phazite::test::writeSound (temp.folder.getChildFile ("Buzz.wav"), 0.2)));
        phazite::setFactoryWavetable (*first.synth, 1, factory ("Comb"));
        REQUIRE (phazite::saveDevicePreset (first.context, phazite::getDeviceID (*first.synth), presetFile));
    }

    // The preset holds the file by its full path, to copy in.
    const auto preset = juce::parseXML (presetFile);
    REQUIRE (preset != nullptr);
    CHECK (juce::File::isAbsolutePath (preset->getChildByName ("PLUGIN")->getStringAttribute ("osc1WavetableFile")));

    Song second (temp.folder, "Second");
    const auto track = phazite::getTrackID (*te::getAudioTracks (second.edit())[0]);
    REQUIRE (phazite::loadPreset (second.context, presetFile, track));

    // It takes the track's instrument's place, playing its own copy of the file.
    auto* loaded = phazite::getInstrument (*te::getAudioTracks (second.edit())[0]);
    REQUIRE (loaded != nullptr);
    REQUIRE (phazite::isWavetableSynth (*loaded));
    CHECK (loaded->state["osc1WavetableFile"].toString() == juce::String ("Audio/Buzz.wav"));
    CHECK (second.project->getAudioFolder().getChildFile ("Buzz.wav").existsAsFile());
    CHECK (phazite::getWavetable (*loaded, 0)->getName() == juce::String ("Buzz"));
    CHECK (phazite::getWavetable (*loaded, 1)->getName() == juce::String ("Comb"));
}
