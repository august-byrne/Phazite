// SPDX-License-Identifier: AGPL-3.0-or-later
// Every reader of what Phazite takes from files, and from its plug-ins' helpers, fed damaged inputs (Fuzzer.h): each must
// refuse an input or read it into something consistent (SongCheck.h for a song), and never crash, hang or assert. The
// seeds are real inputs: the golden projects and pack, the presets and keymaps that ship, and what Phazite writes itself.
// PHAZITE_FUZZ_ROUNDS sets the rounds of each case (a nightly run: 20000), PHAZITE_FUZZ_ROUND runs one round alone.
#include "Fuzzer.h"
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/PieceStore.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionStore.h"
#include "sandbox/core/PluginInfo.h"
#include "sandbox/core/Vst3State.h"
#include "services/Library.h"
#include "services/Settings.h"
#include "ui/input/Keymap.h"

#include <juce_cryptography/juce_cryptography.h>

#include <cstring>
#include <unordered_map>

namespace
{
using namespace phazite::test;
namespace fuzz = phazite::test::fuzz;

juce::MemoryBlock readBytes (const juce::File& file)
{
    juce::MemoryBlock bytes;
    file.loadFileAsData (bytes);
    return bytes;
}

juce::File getRepository()
{
    return getGoldenFolder().getParentDirectory().getParentDirectory();
}

// A project read as Project::open() reads it: refused with a reason, or a song that holds together.
std::string openProject (const juce::File& file, const juce::MemoryBlock& input)
{
    file.replaceWithData (input.getData(), input.getSize());
    juce::String error;
    const auto project = phazite::Project::open (file, false, error);

    if (project == nullptr)
        return error.isNotEmpty() ? std::string() : "refused without saying why";

    return fuzz::joinProblems (findSongProblems (project->getSong()));
}

// Every file in a folder and below is one whose name says it is audio.
std::string findNonAudio (const juce::File& folder)
{
    for (const auto& file : folder.findChildFiles (juce::File::findFiles, true))
        if (phazite::getFileKind (file) != phazite::FileKind::audio)
            return "a file that isn't audio copied in: " + file.getFileName().toStdString();

    return {};
}

// The seeds of the presets' cases: a preset of each kind of device Phazite has (the first that ships for each), and a track's
// and a container's, made here.
std::vector<juce::MemoryBlock> getPresetSeeds (TestSong& s, const juce::File& folder)
{
    std::vector<juce::MemoryBlock> seeds;

    // In the order of their names: a folder lists its files in an order of its own, which on Linux is another on each machine.
    auto devices = getFactoryPresetsFolder().findChildFiles (juce::File::findDirectories, false);
    devices.sort();

    for (const auto& device : devices)
    {
        auto presets = device.findChildFiles (juce::File::findFiles, true, juce::String ("*") + phazite::presetExtension);
        presets.sort();

        if (! presets.isEmpty())
            seeds.push_back (readBytes (presets[0]));
    }

    auto& song = *phazite::findSong (s.context);
    const auto track = s.addTrack ("Lead");
    s.setInstrument (track, phazite::devices::Synth::typeID);
    s.setVolume (track, -6.0f);
    const auto made = folder.getChildFile ("Track.phazitepreset");
    REQUIRE (phazite::writePreset (phazite::makeTrackPreset (song, track), made));
    seeds.push_back (readBytes (made));
    return seeds;
}
} // namespace

TEST_CASE ("Fuzz: project files open as a song that holds together, or not at all")
{
    TemporaryFolder temporary;
    const auto folder = temporary.folder.getChildFile ("Fuzzed");
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile ("Fuzzed.phazite");
    const auto golden = getGoldenFolder();
    const std::vector seeds { readBytes (golden.getChildFile ("projects/representative.xml")),
                              readBytes (golden.getChildFile ("projects/empty.xml")),
                              readBytes (golden.getChildFile ("commands/script.xml")) };

    fuzz::run ("project files", seeds, fuzz::Shape::xml, 300, [&] (const juce::MemoryBlock& input) { return openProject (file, input); });
}

TEST_CASE ("Fuzz: a version's pieces join into a song that holds together, or not at all")
{
    // The representative project's song in pieces, one of them damaged each round.
    TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Fuzzed.phazite");
    REQUIRE (getGoldenFolder().getChildFile ("projects/representative.xml").copyFileTo (file));
    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE (project != nullptr);
    const auto pieces = phazite::splitSong (project->getSong().getState());
    std::vector<juce::String> names;

    for (const auto& [name, bytes] : pieces.pieces)
        names.push_back (name);

    fuzz::runRounds ("version pieces", 200,
                     [&] (fuzz::Mutator& mutator, juce::MemoryBlock& fed) -> std::string
                     {
                         const auto& damagedName = names[(size_t) mutator.random.nextInt ((int) names.size())];
                         fed = mutator.mutate (pieces.pieces.at (damagedName), fuzz::Shape::xml);
                         const auto joined =
                             phazite::joinSong (pieces.song,
                                                [&] (const juce::String& name) -> std::optional<juce::MemoryBlock>
                                                {
                                                    if (name == damagedName)
                                                        return fed;

                                                    if (const auto found = pieces.pieces.find (name); found != pieces.pieces.end())
                                                        return found->second;

                                                    return std::nullopt;
                                                });

                         if (! joined.isValid())
                             return {};

                         const auto song = phazite::makeSong (joined, file);
                         return song != nullptr ? fuzz::joinProblems (findSongProblems (*song)) : std::string();
                     });
}

TEST_CASE ("Fuzz: a pack of a version's pieces is read whole or not at all")
{
    TemporaryFolder temporary;
    const auto pieces = temporary.folder.getChildFile ("Versions").getChildFile ("pieces");
    REQUIRE (pieces.createDirectory());
    const auto file = temporary.folder.getChildFile ("Fuzzed.phazite");
    const std::vector seeds { readBytes (getGoldenFolder().getChildFile ("format/song.pack")) };

    fuzz::run ("version packs", seeds, fuzz::Shape::bytes, 150,
               [&] (const juce::MemoryBlock& input) -> std::string
               {
                   // Named as a store finds a pack, by the SHA-256 of its index, where the end of it says the index is.
                   juce::String name ("0000000000000000000000000000000000000000000000000000000000000000");

                   if (input.getSize() > 16)
                   {
                       const auto* end = static_cast<const char*> (input.getData()) + input.getSize() - 16;
                       const auto count = (juce::uint64) juce::ByteOrder::littleEndianInt (end);
                       const auto start = (juce::uint64) juce::ByteOrder::littleEndianInt64 (end + 4);

                       if (start <= input.getSize() && count <= (input.getSize() - start) / 48)
                           name = juce::SHA256 (static_cast<const char*> (input.getData()) + start, (size_t) (count * 48)).toHexString();
                   }

                   for (const auto& old : pieces.findChildFiles (juce::File::findFiles, false))
                       old.deleteFile();

                   pieces.getChildFile (name + ".pack").replaceWithData (input.getData(), input.getSize());
                   const phazite::PieceStore store (pieces);

                   for (const auto& piece : store.getNames())
                   {
                       const auto joined = phazite::joinSong (piece, [&store] (const juce::String& wanted) { return store.read (wanted); });

                       if (joined.hasType (phazite::schema::PHAZITE))
                           if (const auto song = phazite::makeSong (joined, file))
                               if (const auto problems = findSongProblems (*song); ! problems.empty())
                                   return fuzz::joinProblems (problems);
                   }

                   store.closeFiles();
                   return {};
               });
}

TEST_CASE ("Fuzz: a project's versions' records, branches, logs, messages and local history are read or left out")
{
    TemporaryFolder temporary;
    const auto versions = temporary.folder.getChildFile ("Versions");
    juce::String branchID, versionID;

    {
        phazite::VersionStore store (versions, "fuzz-computer");
        auto song = phazite::createSong();
        phazite::VersionRecord record;
        record.name = "First";
        record.note = "A note";
        record.saved = 1793800000000;
        record.files.push_back ({ "Audio/Kick.wav", 1234, juce::String::repeatedString ("ab", 32) });
        versionID = store.writeVersion (record, phazite::splitSong (song->getState()));
        REQUIRE (versionID.isNotEmpty());

        phazite::BranchRecord branch;
        branch.id = "branch-1";
        branch.name = "main";
        branch.colour = juce::Colours::orange;
        branch.head = versionID;
        branch.changed = 1793800000000;
        REQUIRE (store.writeBranch (branch));
        REQUIRE (store.writeCurrentBranch (branch.id));
        REQUIRE (store.logBranchMove (branch.id, {}, versionID, "commit"));
        REQUIRE (store.writeMessage (versionID, { "First, again", "Edited", 1793800000001 }));
        REQUIRE (store.writeSnapshots ({ { 1793800000002, branch.id, record.song, "closed" } }));
        branchID = branch.id;
    }

    // Each file but the packs, damaged one at a time and put back after.
    juce::Array<juce::File> files;

    for (const auto& file : versions.findChildFiles (juce::File::findFiles, true))
        if (! file.hasFileExtension ("pack"))
            files.add (file);

    REQUIRE (files.size() >= 4);

    fuzz::runRounds ("version records", 150,
                     [&] (fuzz::Mutator& mutator, juce::MemoryBlock& fed) -> std::string
                     {
                         const auto file = files[mutator.random.nextInt (files.size())];
                         const auto original = readBytes (file);
                         fed = mutator.mutate (original, file.hasFileExtension ("xml") ? fuzz::Shape::xml : fuzz::Shape::bytes);
                         file.replaceWithData (fed.getData(), fed.getSize());

                         {
                             const phazite::VersionStore store (versions, "fuzz-computer");
                             const auto branches = store.readBranches();
                             const auto current = store.readCurrentBranch();
                             const auto log = store.readBranchLog (branchID);
                             const auto messages = store.readMessages();
                             const auto snapshots = store.readSnapshots();
                             const auto version = store.readVersion (versionID);
                             juce::ignoreUnused (branches, current, log, messages, snapshots, version);
                             store.getPieces().closeFiles();
                         }

                         file.replaceWithData (original.getData(), original.getSize());
                         return {};
                     });
}

TEST_CASE ("Fuzz: presets of every kind load into a song that holds together, or not at all")
{
    TestSong s;
    TemporaryFolder temporary;
    const auto seeds = getPresetSeeds (s, temporary.folder);
    const auto target = s.addTrack ("Target");
    const auto file = temporary.folder.getChildFile ("Fuzzed.phazitepreset");
    auto& song = *phazite::findSong (s.context);
    const auto audio = s.getProject().getAudioFolder();

    fuzz::run ("presets", seeds, fuzz::Shape::json, 150,
               [&] (const juce::MemoryBlock& input) -> std::string
               {
                   file.replaceWithData (input.getData(), input.getSize());

                   if (! phazite::readPreset (file).isObject())
                       return {};

                   const auto loaded = phazite::loadPreset (s.context, file, target);
                   auto problems = findSongProblems (song);

                   if (const auto copied = findNonAudio (audio); ! copied.empty())
                       problems.push_back (copied);

                   if (loaded)
                       phazite::undo (song);

                   return fuzz::joinProblems (problems);
               });
}

TEST_CASE ("Fuzz: a device's preset loads into it within its parameters' ranges, or not at all")
{
    TestSong s;
    TemporaryFolder temporary;
    const auto track = s.addTrack ("Drums");
    const auto drums = s.setInstrument (track, "phaziteDrums");
    const auto synthTrack = s.addTrack ("Synth");
    const auto synth = s.setInstrument (synthTrack, phazite::devices::Synth::typeID);
    auto& song = *phazite::findSong (s.context);
    const auto audio = s.getProject().getAudioFolder();
    std::vector<juce::MemoryBlock> seeds;

    for (const auto* device : { "Drum Sampler", "Synth" })
    {
        auto presets = getFactoryPresetsFolder().getChildFile (device).findChildFiles (juce::File::findFiles, true, "*.phazitepreset");
        presets.sort();

        for (int i = 0; i < juce::jmin (3, presets.size()); ++i)
            seeds.push_back (readBytes (presets[i]));
    }

    const auto file = temporary.folder.getChildFile ("Fuzzed.phazitepreset");

    fuzz::run ("presets into devices", seeds, fuzz::Shape::json, 100,
               [&] (const juce::MemoryBlock& input) -> std::string
               {
                   file.replaceWithData (input.getData(), input.getSize());
                   auto problems = std::vector<std::string>();

                   for (const auto device : { drums, synth })
                   {
                       const auto loaded = phazite::loadPresetInto (s.context, device, file);

                       for (const auto& problem : findSongProblems (song))
                           problems.push_back (problem);

                       if (loaded)
                           phazite::undo (song);
                   }

                   if (const auto copied = findNonAudio (audio); ! copied.empty())
                       problems.push_back (copied);

                   return fuzz::joinProblems (problems);
               });
}

TEST_CASE ("Fuzz: a DAWproject is taken into a song that holds together, or refused")
{
    TemporaryFolder temporary;
    const auto archive = temporary.folder.getChildFile ("Seed.dawproject");

    // A song with a MIDI clip and an audio clip, written as Phazite exports one.
    {
        TestSong s;
        const auto lead = s.addTrack ("Lead");
        s.setInstrument (lead, phazite::devices::Synth::typeID);
        const auto clip = s.addMidiClip (lead, 0.0, 4.0);
        s.addNote (clip, 60, 0.0, 1.0);
        const auto drums = s.addTrack ("Drums");
        s.addAudioClip (drums, writeSound (s.getSoundsFolder().getChildFile ("Loop.wav"), 0.25), 4.0);
        s.setVolume (drums, -3.0f);
        auto content = phazite::describeDawProject (*phazite::findSong (s.context), "Seed", "A comment");
        content.bounces.clear();
        REQUIRE (phazite::writeDawProject (content, {}, archive).wasOk());
    }

    const auto seedArchive = readBytes (archive);
    juce::String seedText;
    std::vector<std::pair<juce::String, juce::MemoryBlock>> otherEntries;

    {
        juce::ZipFile zip (archive);

        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const std::unique_ptr<juce::InputStream> stream (zip.createStreamForEntry (i));
            juce::MemoryBlock bytes;
            stream->readIntoMemoryBlock (bytes);

            if (zip.getEntry (i)->filename == "project.xml")
                seedText = bytes.toString();
            else
                otherEntries.push_back ({ zip.getEntry (i)->filename, bytes });
        }
    }

    REQUIRE (seedText.isNotEmpty());
    const auto project = temporary.folder.getChildFile ("Imported").getChildFile ("Imported.phazite");
    const auto fed = temporary.folder.getChildFile ("Fuzzed.dawproject");

    const auto take = [&] (const juce::File& file) -> std::string
    {
        // A damaged archive names dates that are none, which JUCE asserts against as its ZipFile reads them (in Debug
        // builds): Phazite never uses an entry's date, and the fuzzer's rounds expect that assertion (tests/Fuzzer.cpp).
        const auto read = phazite::readDawProject (file);

        if (read.project == nullptr)
            return read.error.isNotEmpty() ? std::string() : "refused without saying why";

        project.getParentDirectory().deleteRecursively();
        project.getParentDirectory().createDirectory();
        auto song = phazite::createSong (project);
        phazite::takeDawProject (*song, read);
        auto problems = findSongProblems (*song);

        if (const auto copied = findNonAudio (project.getSiblingFile ("Audio")); ! copied.empty())
            problems.push_back (copied);

        return fuzz::joinProblems (problems);
    };

    SUBCASE ("The archive's bytes")
    {
        fuzz::run ("DAWproject archives", { seedArchive }, fuzz::Shape::bytes, 100,
                   [&] (const juce::MemoryBlock& input)
                   {
                       fed.replaceWithData (input.getData(), input.getSize());
                       return take (fed);
                   });
    }

    SUBCASE ("Its project.xml, zipped again with its files")
    {
        fuzz::run ("DAWproject project.xml", { fuzz::toBlock (seedText) }, fuzz::Shape::xml, 150,
                   [&] (const juce::MemoryBlock& input)
                   {
                       fed.deleteFile();

                       {
                           juce::ZipFile::Builder builder;
                           builder.addEntry (std::make_unique<juce::MemoryInputStream> (input, true), 0, "project.xml",
                                             juce::Time (2026, 0, 1, 0, 0));

                           for (const auto& [name, bytes] : otherEntries)
                               builder.addEntry (std::make_unique<juce::MemoryInputStream> (bytes, true), 0, name,
                                                 juce::Time (2026, 0, 1, 0, 0));

                           juce::FileOutputStream out (fed);
                           REQUIRE (builder.writeToStream (out, nullptr));
                       }

                       return take (fed);
                   });
    }
}

TEST_CASE ("Fuzz: a VST3's state as its preset file, and as JUCE keeps it, is read within its bytes or refused")
{
    phazite::sandbox::Vst3State state;
    state.classID = "0123456789ABCDEF0123456789ABCDEF";
    state.component = fuzz::toBlock ("The processor's own state");
    state.controller = fuzz::toBlock ("The controller's");
    state.hasController = true;
    const std::vector seeds { phazite::sandbox::writeVst3Preset (state), phazite::sandbox::writeJuceVst3State (state) };

    fuzz::run ("vstpreset", seeds, fuzz::Shape::bytes, 2000,
               [] (const juce::MemoryBlock& input) -> std::string
               {
                   for (const auto& read : { phazite::sandbox::readVst3Preset (input),
                                             phazite::sandbox::readJuceVst3State (input, "0123456789ABCDEF0123456789ABCDEF") })
                       if (read.has_value() && read->component.getSize() + read->controller.getSize() > input.getSize() * 2)
                           return "a state larger than what held it";

                   juce::ignoreUnused (phazite::sandbox::readVst3ClassID (input));
                   return {};
               });
}

TEST_CASE ("Fuzz: what a helper tells of a plug-in is read within its message, whatever a crash left of it")
{
    using namespace phazite::sandbox;
    PluginInfo info;
    info.name = "Plug-in";
    info.latencySamples = 64;
    info.tailSeconds = 1.5;
    info.numInputChannels = info.numOutputChannels = 2;
    info.programs = { "Init", "Bright" };
    info.program = 1;

    for (int i = 0; i < 3; ++i)
        info.parameters.push_back ({ juce::String (i), "Parameter " + juce::String (i), "dB", 0.5f, 0.25f, 0, false, true });

    MessageWriter opened (MessageType::reply, 7);
    opened.add (true).add (juce::String());
    write (opened, info);
    MessageWriter changed (MessageType::parametersChanged, 0);
    changed.add ((std::int64_t) 3);
    write (changed, info.parameters);
    MessageWriter block (MessageType::getState, 9);
    block.add (fuzz::toBlock ("state bytes"));
    const std::vector seeds { opened.take(), changed.take(), block.take() };

    fuzz::run ("helper messages", seeds, fuzz::Shape::bytes, 3000,
               [] (const juce::MemoryBlock& input) -> std::string
               {
                   MessageReader reader (input);

                   if (reader.getType() == MessageType::parametersChanged)
                   {
                       juce::ignoreUnused (reader.readInt64());

                       if (readParameters (reader).size() > input.getSize())
                           return "more parameters than bytes";

                       return {};
                   }

                   if (reader.getType() == MessageType::getState)
                       return reader.readBlock().getSize() > input.getSize() ? "a block larger than its message" : "";

                   juce::ignoreUnused (reader.readBool(), reader.readString());
                   const auto read = readPluginInfo (reader);

                   if (read.parameters.size() + read.programs.size() > input.getSize())
                       return "more parameters and programs than bytes";

                   if (read.latencySamples < 0 || read.latencySamples > maxLatencySamples || ! std::isfinite (read.tailSeconds)
                       || read.tailSeconds < 0.0 || read.numInputChannels < 0 || read.numInputChannels > 64 || read.numOutputChannels < 0
                       || read.numOutputChannels > 64)
                       return "a number beyond what a plug-in can have";

                   return {};
               });
}

TEST_CASE ("Fuzz: the settings file, and the library's facts file, are read into values Phazite can use")
{
    TemporaryFolder temporary;
    const auto file = temporary.folder.getChildFile ("Phazite.settings");
    juce::PropertiesFile::Options options;
    options.applicationName = "Phazite";

    {
        juce::PropertiesFile properties (file, options);
        phazite::Settings settings (&properties);
        settings.set (phazite::Settings::interfaceScale, 1.25);
        settings.set (phazite::Settings::exportSampleRate, 48000.0);
        settings.set (phazite::Settings::browserFolders, juce::StringArray { "C:/Sounds", "D:/More" });
        settings.set (phazite::Settings::theme, juce::String ("light"));
        settings.set (phazite::Settings::playlistHeaderWidth, 240.0);
        settings.set (phazite::Settings::recordingCorrection, 3.5);
        settings.saveNow();
    }

    SUBCASE ("Settings")
    {
        fuzz::run ("settings", { readBytes (file) }, fuzz::Shape::xml, 150,
                   [&] (const juce::MemoryBlock& input) -> std::string
                   {
                       file.replaceWithData (input.getData(), input.getSize());
                       juce::PropertiesFile properties (file, options);
                       const phazite::Settings settings (&properties);

                       for (const auto* number : { &phazite::Settings::interfaceScale, &phazite::Settings::exportSampleRate,
                                                   &phazite::Settings::exportBitDepth, &phazite::Settings::exportLoudnessTarget,
                                                   &phazite::Settings::exportTruePeakCeiling, &phazite::Settings::recordingCorrection,
                                                   &phazite::Settings::playlistHeaderWidth, &phazite::Settings::pianoRollVelocityHeight })
                           if (! std::isfinite (settings.get (*number)))
                               return std::string ("not a number: ") + number->key;

                       if (! std::isfinite (settings.get (phazite::Settings::browserPreviewVolume, 0.5)))
                           return "not a number: the preview's volume";

                       juce::ignoreUnused (settings.get (phazite::Settings::browserFolders), settings.get (phazite::Settings::theme));
                       return {};
                   });
    }

    SUBCASE ("The library's facts file")
    {
        const auto sounds = temporary.folder.getChildFile ("Sounds");
        const auto facts = temporary.folder.getChildFile ("Library facts.txt");
        writeSound (sounds.getChildFile ("Loop 120 BPM.wav"), 0.1);
        phazite::Settings settings;
        settings.set (phazite::Settings::browserFolders, juce::StringArray { sounds.getFullPathName() });

        const auto index = [&]
        {
            phazite::Library library (getEngineContext(), settings, {}, {}, {}, facts);
            REQUIRE (runTimersUntil ([&library] { return ! library.isIndexing(); }, 60000));
            dispatchPendingMessages();
        };

        index();
        REQUIRE (facts.existsAsFile());

        fuzz::run ("library facts", { readBytes (facts) }, fuzz::Shape::bytes, 30,
                   [&] (const juce::MemoryBlock& input) -> std::string
                   {
                       facts.replaceWithData (input.getData(), input.getSize());
                       index();
                       return {};
                   });
    }
}

TEST_CASE ("Fuzz: a keymap is read or refused")
{
    std::vector<juce::MemoryBlock> seeds;

    // In the order of their names, so that a round feeds the same on every machine (CI's linux-debug met a round no other
    // run had, 2026-11-15: a folder's own order).
    auto keymaps = getRepository().getChildFile ("resources/keymaps").findChildFiles (juce::File::findFiles, false, "*.json");
    keymaps.sort();

    for (const auto& file : keymaps)
        seeds.push_back (readBytes (file));

    REQUIRE (seeds.size() >= 3);

    fuzz::run ("keymaps", seeds, fuzz::Shape::json, 500,
               [] (const juce::MemoryBlock& input) -> std::string
               {
                   // The keymaps are text: bytes that are no UTF-8 are no keymap, which only what reads text sees.
                   if (juce::CharPointer_UTF8::isValidString (static_cast<const char*> (input.getData()), (int) input.getSize()))
                       juce::ignoreUnused (phazite::ui::Keymap::fromJSON (input.toString()));

                   return {};
               });
}

TEST_CASE ("Fuzz: what Phazite reads of an audio file without playing it is a length, a rate and loop data it can use")
{
    TemporaryFolder temporary;
    const auto folder = temporary.folder;
    std::vector<juce::MemoryBlock> seeds;

    // A WAV file with an acid chunk (a loop of 8 beats in A), an AIFF file and a FLAC file, each short.
    const auto write = [&] (juce::AudioFormat& format, const juce::String& name, const std::unordered_map<juce::String, juce::String>& data)
    {
        const auto file = folder.getChildFile (name);
        juce::AudioBuffer<float> sound (2, 2205);

        for (int i = 0; i < sound.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c)
                sound.setSample (c, i, 0.25f * (float) std::sin (0.05 * i));

        {
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            const auto writer = format.createWriterFor (
                stream,
                juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16).withMetadataValues (
                    data));
            REQUIRE (writer != nullptr);
            writer->writeFromAudioSampleBuffer (sound, 0, sound.getNumSamples());
        }

        seeds.push_back (readBytes (file));
    };

    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;
    juce::FlacAudioFormat flac;
    write (wav, "Loop.wav",
           { { juce::WavAudioFormat::acidBeats, "8" },
             { juce::WavAudioFormat::acidOneShot, "0" },
             { juce::WavAudioFormat::acidRootSet, "1" },
             { juce::WavAudioFormat::acidRootNote, "57" } });
    write (aiff, "Sound.aif", {});
    write (flac, "Sound.flac", {});

    fuzz::run ("audio file headers", seeds, fuzz::Shape::bytes, 400,
               [&] (const juce::MemoryBlock& input) -> std::string
               {
                   const auto* data = static_cast<const char*> (input.getData());
                   const auto starts = [&] (const char* magic) { return input.getSize() >= 4 && std::memcmp (data, magic, 4) == 0; };
                   const auto file = folder.getChildFile (starts ("FORM") ? "Fuzzed.aif" : starts ("fLaC") ? "Fuzzed.flac" : "Fuzzed.wav");
                   file.replaceWithData (input.getData(), input.getSize());
                   const auto info = phazite::getAudioFileInfo (file);
                   file.deleteFile();

                   if (info.lengthInSamples < 0 || ! std::isfinite (info.sampleRate) || info.sampleRate < 0.0 || info.numChannels < 0
                       || ! std::isfinite (info.loopBeats) || info.loopBeats < 0.0 || ! std::isfinite (info.getSeconds()))
                       return "a length, a rate or loop data that can't be: " + std::to_string (info.lengthInSamples) + " samples at "
                              + std::to_string (info.sampleRate) + ", " + std::to_string (info.loopBeats) + " beats";

                   if (info.key.has_value() && ! juce::isPositiveAndBelow (info.key->root, 12))
                       return "a key that isn't one";

                   return {};
               });
}
