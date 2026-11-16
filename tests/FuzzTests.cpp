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
#include "UiTestHelpers.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/arrangement/internal/Transients.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
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
namespace schema = phazite::schema;

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
    auto project = phazite::Project::open (file, false, error);

    if (project == nullptr)
        return error.isNotEmpty() ? std::string() : "refused without saying why";

    if (const auto problems = fuzz::joinProblems (findSongProblems (project->getSong())); ! problems.empty())
        return problems;

    // What Phazite saved of it is Phazite's own: opened again and saved, the same bytes (makeConsistent() changes nothing in
    // a project Phazite can save; known bugs 179 and 25).
    if (project->save().failed())
        return "it opened, and couldn't be saved";

    const auto saved = file.loadFileAsString();
    project = nullptr;
    project = phazite::Project::open (file, false, error);

    if (project == nullptr || project->save().failed())
        return "what it saved doesn't open and save again: " + error.toStdString();

    if (const auto again = file.loadFileAsString(); again != saved)
    {
        auto before = juce::StringArray::fromLines (saved), after = juce::StringArray::fromLines (again);
        int line = 0;

        while (line < before.size() && line < after.size() && before[line] == after[line])
            ++line;

        return ("saved, opened again and saved, it changed at line " + juce::String (line + 1) + ": \"" + before[line].trim() + "\" to \""
                + after[line].trim() + "\"")
            .toStdString();
    }

    return {};
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

TEST_CASE (
    "Fuzz: a song another computer wrote into the versions goes into the song holding together, as it is checked out, merged, restored or brought back from Local History")
{
    // The representative project's song, damaged each round as anyone could write it, kept as another computer's commit on a
    // branch of its own and as a snapshot of this computer's Local History, with each piece's name what it holds (so that
    // the store reads it); the project opened again, the song made it in each way the versions make it, and checked.
    TestSong s;
    juce::String error;
    const auto golden = getGoldenFolder().getChildFile ("projects/representative.xml");
    const auto copy = s.getProject().getFolder().getChildFile ("Representative.phazite");
    REQUIRE (golden.copyFileTo (copy));
    const auto representative = phazite::Project::open (copy, false, error);
    REQUIRE (representative != nullptr);
    const auto seed = phazite::writeElement (representative->getSong().getState());
    const auto versions = s.getProject().getFolder().getChildFile ("Versions");
    auto round = 0;

    fuzz::runRounds ("versions from elsewhere", 20,
                     [&] (fuzz::Mutator& mutator, juce::MemoryBlock& fed) -> std::string
                     {
                         fed = mutator.mutate (seed, fuzz::Shape::xml);
                         const auto hostile = phazite::readElement (fed);

                         if (! hostile.hasType (schema::PHAZITE))
                             return {};

                         REQUIRE (waitForVersionHistory (s.context));
                         const auto head = phazite::getLastVersion (s.context)->id;
                         const auto main = phazite::getCurrentBranch (s.context)->id;
                         const auto time = juce::Time::currentTimeMillis() + ++round;
                         juce::String id;

                         {
                             phazite::VersionStore other (versions, "fuzz-computer");
                             phazite::VersionRecord record;
                             record.parents = { head };
                             record.branch = "fuzz-" + juce::String (round);
                             record.saved = time;
                             const auto pieces = phazite::splitSong (hostile);
                             id = other.writeVersion (record, pieces);
                             REQUIRE (id.isNotEmpty());
                             phazite::BranchRecord branch;
                             branch.id = record.branch;
                             branch.name = record.branch;
                             branch.head = id;
                             branch.computer = "fuzz-computer";
                             branch.changed = time;
                             REQUIRE (other.writeBranch (branch));
                             other.getPieces().closeFiles();

                             phazite::VersionStore mine (versions, phazite::getThisComputer());
                             auto snapshots = mine.readSnapshots();
                             snapshots.push_back ({ time, main, pieces.song, "auto" });
                             REQUIRE (mine.writeSnapshots (snapshots));
                         }

                         s.reopen (false);
                         REQUIRE (waitForVersionHistory (s.context));
                         std::vector<std::string> problems;
                         const auto check = [&] (const char* how)
                         {
                             for (const auto& problem : findSongProblems (*phazite::findSong (s.context)))
                                 problems.push_back (std::string (how) + ": " + problem);
                         };

                         if (finished (s.context, [&] { return phazite::checkoutVersion (s.context, id, s.save); }))
                         {
                             check ("checked out");
                             REQUIRE (finished (s.context, [&] { return phazite::backToBranch (s.context, s.save); }));
                         }

                         if (finished (s.context, [&] { return phazite::startMerge (s.context, id); }))
                         {
                             phazite::finishTickedMerge (s.context);
                             check ("merged");
                             phazite::cancelMerge (s.context);
                             check ("merge aborted");
                         }

                         if (finished (s.context, [&] { return phazite::bringBackVersion (s.context, id); }))
                         {
                             check ("restored");
                             phazite::undo (*phazite::findSong (s.context));
                         }

                         if (finished (s.context, [&] { return phazite::restoreSnapshot (s.context, time); }))
                         {
                             check ("Local History restored");
                             phazite::undo (*phazite::findSong (s.context));
                         }

                         return fuzz::joinProblems (problems);
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
    const auto songFile = temporary.folder.getChildFile ("Fuzzed.phazite");
    juce::String branchID, versionID;

    {
        phazite::VersionStore store (versions, "fuzz-computer");
        auto song = phazite::createSong();
        phazite::VersionRecord record;
        record.name = "First";
        record.note = "A note";
        record.saved = 1793800000000;
        record.files.push_back ({ "Audio/Kick.wav", 1234, juce::String::repeatedString ("ab", 32) });
        const auto pieces = phazite::splitSong (song->getState());
        versionID = store.writeVersion (record, pieces);
        record.song = pieces.song; // which the snapshot names below
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
                         std::string problems;

                         {
                             const phazite::VersionStore store (versions, "fuzz-computer");
                             const auto branches = store.readBranches();
                             const auto current = store.readCurrentBranch();
                             const auto log = store.readBranchLog (branchID);
                             const auto messages = store.readMessages();
                             const auto snapshots = store.readSnapshots();
                             const auto version = store.readVersion (versionID);
                             juce::ignoreUnused (branches, current, log, messages);

                             // The songs they name read from their pieces, as Local History and a commit are: a whole
                             // song that holds together, or none.
                             juce::StringArray songs;

                             for (const auto& snapshot : snapshots)
                                 songs.add (snapshot.song);

                             for (const auto& branch : branches)
                                 songs.add (branch.working);

                             if (version.has_value())
                                 songs.add (version->song);

                             for (const auto& piece : songs)
                             {
                                 if (const auto read = store.readSong (piece); read.hasType (phazite::schema::PHAZITE))
                                 {
                                     if (const auto song = phazite::makeSong (read, songFile))
                                     {
                                         if (const auto found = findSongProblems (*song); ! found.empty())
                                             problems = fuzz::joinProblems (found);
                                     }
                                 }
                             }

                             store.getPieces().closeFiles();
                         }

                         file.replaceWithData (original.getData(), original.getSize());
                         return problems;
                     });
}

TEST_CASE ("Fuzz: a song's merge waiting, damaged, waits whole or not at all, and the song holds together through Abort Merge")
{
    // A real merge waiting, its element (Schema.h: MERGING) as Phazite writes it, and one whose song before it names a
    // track's piece; each damaged, put into the song's file, and the project opened again, as one from anyone would be.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto commit = [&s]
    {
        REQUIRE (waitForVersionHistory (s.context));
        std::optional<juce::Result> result;
        REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
        REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
        REQUIRE (result->wasOk());
        REQUIRE (waitForVersionHistory (s.context));
        return phazite::getLastVersion (s.context)->id;
    };

    commit();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    s.setVolume (bass, -12.0f);
    s.addTrack ("Keys");
    const auto theirs = commit();
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));
    s.setVolume (bass, -6.0f);
    commit();
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
    phazite::finishTickedMerge (s.context);
    REQUIRE (phazite::isMerging (s.context));

    const auto state = phazite::findSong (s.context)->getState().createCopy();
    const auto element = state.getChildWithName (schema::MERGING);
    REQUIRE (element.isValid());
    auto pointingAtATrack = element.createCopy();

    for (const auto& [name, piece] : phazite::splitSong (state).pieces)
        if (piece.toString().startsWith ("<" + schema::TRACK.toString()))
            pointingAtATrack.setProperty (schema::mergingBefore, name, nullptr);

    std::vector<juce::MemoryBlock> seeds;

    for (const auto& seed : { element, pointingAtATrack })
    {
        const auto text = seed.toXmlString();
        seeds.emplace_back (text.toRawUTF8(), text.getNumBytesAsUTF8());
    }

    fuzz::run ("merges waiting", seeds, fuzz::Shape::xml, 40,
               [&] (const juce::MemoryBlock& input) -> std::string
               {
                   auto song = state.createCopy();
                   song.removeChild (song.getChildWithName (schema::MERGING), nullptr);

                   // Its attributes, as a file's reader takes them: the element's type and names checked, never fromXml().
                   if (const auto xml = juce::parseXML (input.toString());
                       xml != nullptr && xml->getTagName() == schema::MERGING.toString())
                   {
                       juce::ValueTree merging (schema::MERGING);

                       for (const auto& attribute : xml->getAttributeIterator())
                           merging.setProperty (attribute.name, attribute.value, nullptr);

                       song.appendChild (merging, nullptr);
                   }

                   s.openSong (song);

                   if (! waitForVersionHistory (s.context))
                       return "the history didn't open";

                   auto& opened = *phazite::findSong (s.context);
                   juce::ignoreUnused (phazite::getClashes (s.context), phazite::getMergeProblem (s.context),
                                       phazite::describeNextCommit (s.context));

                   for (int clash = 0; clash < (int) phazite::getClashes (s.context).size(); ++clash)
                       phazite::chooseClash (s.context, clash, 2);

                   phazite::cancelMerge (s.context);

                   if (phazite::getTrackIDs (opened).empty())
                       return "Abort Merge emptied the song";

                   return fuzz::joinProblems (findSongProblems (opened));
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
        // builds; in Release builds the C library makes a date of them as for any other, which Phazite never uses), as it
        // is read and as its files are copied: expected here, while the archive is read, and nowhere else. JUCE workaround
        // T8 (docs/juce-workarounds.md).
        const phazite::test::ExpectedAssertion ofZipDates ("juce_Time.cpp:170-175");
        const auto read = phazite::readDawProject (file);

        if (read.project == nullptr)
            return read.error.isNotEmpty() ? std::string() : "refused without saying why";

        project.getParentDirectory().deleteRecursively();
        project.getParentDirectory().createDirectory();

        // What the project's Audio folder held before: a file of its own, and one of an entry's name with other bytes.
        const auto audio = project.getSiblingFile ("Audio");
        audio.createDirectory();
        const std::vector<std::pair<juce::String, juce::String>> before { { "Kept.wav", "kept" }, { "Loop.wav", "not the loop" } };

        for (const auto& [name, bytes] : before)
            audio.getChildFile (name).replaceWithText (bytes);

        // As an import takes one in: its audio unpacked first, off the message thread in the app; what nothing plays then gone.
        auto song = phazite::createSong (project);
        const auto unpacked = phazite::unpackDawProjectAudio (read, audio);
        phazite::takeDawProject (*song, read, unpacked);
        phazite::forgetUnusedAudio (unpacked, song->getState(), project);
        auto problems = findSongProblems (*song);

        for (const auto& [name, bytes] : before)
            if (audio.getChildFile (name).loadFileAsString() != bytes)
                problems.push_back ("the Audio folder's " + name.toStdString() + " was changed or deleted");

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
            REQUIRE (runTimersUntil ([&library] { return ! library.isIndexing(); }));
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

    // What Normalize and Quantize Audio read of the whole of one, in the background (known bug 123): a file that says it is
    // longer, or has more channels or a higher rate, than it can hold is read as far as it holds, to a peak and hits they
    // can use.
    fuzz::run ("audio files read whole", seeds, fuzz::Shape::bytes, 200,
               [&] (const juce::MemoryBlock& input) -> std::string
               {
                   const auto* data = static_cast<const char*> (input.getData());
                   const auto starts = [&] (const char* magic) { return input.getSize() >= 4 && std::memcmp (data, magic, 4) == 0; };
                   const auto file = folder.getChildFile (starts ("FORM") ? "Whole.aif" : starts ("fLaC") ? "Whole.flac" : "Whole.wav");
                   file.replaceWithData (input.getData(), input.getSize());
                   const auto peak = phazite::readPeak (file, { 0.0, 30.0 }, nullptr, nullptr).peak;
                   const auto hits = phazite::Transients::analyse (file);
                   file.deleteFile();

                   if (peak.has_value() && (! std::isfinite (*peak) || *peak < 0.0f))
                       return "a peak that can't be: " + std::to_string (*peak);

                   if (! hits.has_value())
                       return "hits that stopped without being asked to";

                   for (size_t i = 0; i < hits->size(); ++i)
                       if (! std::isfinite ((*hits)[i]) || (*hits)[i] < 0.0 || (i > 0 && (*hits)[i] < (*hits)[i - 1]))
                           return "hits out of time or order";

                   return {};
               });
}
