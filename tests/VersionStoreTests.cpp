// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions' store (docs/design/versions.md): a song in pieces and back, the packs they go in, and the branches, the current
// one and the audio files' hashes beside them.
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

#include <algorithm>
#include <cstring>

#include <juce_cryptography/juce_cryptography.h>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

// A song with what a piece holds: tracks with clips, one with many notes (in runs), Phazite's own devices, a plug-in's
// element with a state, a curve, a section and a tempo.
struct PiecesSong
{
    phazite::Song::Ptr song = phazite::createSong();
    std::vector<phazite::TrackID> tracks = phazite::ensureTracks (*song, 3);
    phazite::ClipID longClip, shortClip;
    phazite::DeviceID eq, plugin;

    PiecesSong()
    {
        phazite::setTrackName (*song, tracks[0], "Bass");
        phazite::setChannelVolume (*song, tracks[0], -6.0f);
        phazite::test::setOwnInstrument (*song, tracks[0], phazite::devices::Synth::typeID);
        eq = phazite::test::addOwnDevice (*song, tracks[0], phazite::devices::Equalizer::typeID);

        longClip = addMidiClip (*song, tracks[0], { at (0.0), at (64.0) }, "Long");
        shortClip = addMidiClip (*song, tracks[1], { at (0.0), at (4.0) }, "Short");

        for (int i = 0; i < 600; ++i)
            addMidiNote (*song, longClip, 36 + i % 24, at (i * 0.1), lasting (0.1), 90 + i % 30);

        addMidiNote (*song, shortClip, 60, at (0.0), lasting (1.0));

        // A plug-in's element, its state a block JUCE writes in base 64 (Mixer.h: a plug-in's DEVICE).
        juce::MemoryBlock state;

        for (int i = 0; i < 3000; ++i)
            state.append (&i, 1);

        juce::PluginDescription testSynth;
        testSynth.name = "Test Synth";
        plugin = phazite::test::addPluginDevice (*song, tracks[2], testSynth, state);
    }

    // The long clip in a copy of the song whole.
    juce::ValueTree findNotesClip (const juce::ValueTree& whole) const
    {
        for (const auto& track : phazite::getTrackElements (whole))
            for (const auto& clip : track)
                if (clip.hasType (schema::CLIP) && phazite::readID (clip[schema::clipID]) == longClip.getRaw())
                    return clip;

        return {};
    }
};

std::optional<juce::MemoryBlock> readFrom (const phazite::SongPieces& pieces, const juce::String& name)
{
    if (const auto found = pieces.pieces.find (name); found != pieces.pieces.end())
        return found->second;

    return std::nullopt;
}

juce::String asPiece (const juce::ValueTree& tree)
{
    return phazite::writeElement (tree).toString();
}

juce::File getVersionsFolder (const TemporaryFolder& temporary)
{
    return temporary.folder.getChildFile ("Versions");
}

// The computer the tests' stores write as: the one the versions' history of a song uses (VersionsModel.h).
const juce::String thisComputer = phazite::getThisComputer();

// A branch as this computer's record of it.
phazite::BranchRecord makeBranch (const juce::String& name, juce::Colour colour, const juce::String& head)
{
    phazite::BranchRecord branch;
    branch.id = juce::Uuid().toString();
    branch.name = name;
    branch.colour = colour;
    branch.head = head;
    branch.computer = thisComputer;
    branch.changed = 1000;
    return branch;
}
} // namespace

TEST_CASE ("A song in pieces joins again as it was, its children in their order")
{
    PiecesSong made;
    const auto state = phazite::test::copyWholeSong (*made.song);
    const auto pieces = phazite::splitSong (state);

    const auto joined = phazite::joinSong (pieces.song, [&pieces] (const juce::String& name) { return readFrom (pieces, name); });
    REQUIRE (joined.isValid());
    CHECK (asPiece (joined) == asPiece (state));

    // A clip's many notes are runs of their own; a short clip's notes stay in its piece.
    int runs = 0;

    for (const auto& [name, piece] : pieces.pieces)
        if (piece.toString().startsWith ("<_run>"))
            ++runs;

    CHECK (runs >= 3);
    CHECK (runs <= 40);

    // A piece that can't be read: the song can't be.
    const auto missing = phazite::joinSong (pieces.song,
                                            [&pieces] (const juce::String& name) -> std::optional<juce::MemoryBlock>
                                            {
                                                if (pieces.pieces.at (name).toString().startsWith ("<_run>"))
                                                    return std::nullopt;

                                                return readFrom (pieces, name);
                                            });
    CHECK_FALSE (missing.isValid());
}

TEST_CASE ("The same song always makes the same pieces, whatever order its properties were set in")
{
    PiecesSong made;
    const auto state = phazite::test::copyWholeSong (*made.song);
    const auto first = phazite::splitSong (state);
    CHECK (phazite::splitSong (state).song == first.song);

    // A copy with each element's properties set again in the opposite order.
    auto reordered = state.createCopy();
    const std::function<void (juce::ValueTree)> reverse = [&reverse] (juce::ValueTree tree)
    {
        juce::NamedValueSet properties;

        for (int i = tree.getNumProperties(); --i >= 0;)
            properties.set (tree.getPropertyName (i), tree[tree.getPropertyName (i)]);

        tree.removeAllProperties (nullptr);

        for (const auto& property : properties)
            tree.setProperty (property.name, property.value, nullptr);

        for (const auto& child : tree)
            reverse (child);
    };

    reverse (reordered);
    CHECK (phazite::splitSong (reordered).song == first.song);

    // Each piece is named by what it holds.
    for (const auto& [name, piece] : first.pieces)
        CHECK (phazite::getPieceName (piece) == name);
}

TEST_CASE ("A plug-in's state is a piece of its own, in its bytes")
{
    PiecesSong made;
    const auto device = phazite::test::copyDevice (*made.song, made.plugin);
    const auto read = phazite::readBytes (device[schema::pluginState].toString());
    REQUIRE (read.has_value());
    const auto& state = *read;

    const auto pieces = phazite::splitSong (phazite::test::copyWholeSong (*made.song));
    const auto found = pieces.pieces.find (phazite::getPieceName (state));
    REQUIRE (found != pieces.pieces.end());
    CHECK (found->second == state);

    // And it comes back as the plug-in's state, as it was written.
    const auto joined = phazite::joinSong (pieces.song, [&pieces] (const juce::String& name) { return readFrom (pieces, name); });
    const std::function<juce::ValueTree (const juce::ValueTree&)> findPlugin = [&findPlugin] (const juce::ValueTree& tree)
    {
        for (const auto& child : tree)
        {
            if (child.hasType (schema::DEVICE) && child[schema::deviceType].toString() == schema::deviceIsPlugin)
                return child;

            if (const auto inside = findPlugin (child); inside.isValid())
                return inside;
        }

        return juce::ValueTree();
    };

    CHECK (findPlugin (joined)[schema::pluginState].toString() == device[schema::pluginState].toString());
}

TEST_CASE ("A plug-in's parameters' values go in a piece beside its state, and come back apart from the song")
{
    PiecesSong made;
    const phazite::PluginParameterValues values { { made.plugin.getRaw(), { { "1", "Cutoff", 0.4f }, { "2", "Resonance", 0.25f } } } };
    const auto pieces = phazite::splitSong (phazite::test::copyWholeSong (*made.song), values);

    phazite::PluginParameterValues read;
    const auto joined = phazite::joinSong (pieces.song, [&pieces] (const juce::String& name) { return readFrom (pieces, name); }, &read);

    REQUIRE (read.count (made.plugin.getRaw()) == 1);
    const auto& parameters = read.at (made.plugin.getRaw());
    REQUIRE (parameters.size() == 2);
    CHECK (parameters[0].name == "Cutoff");
    CHECK (juce::approximatelyEqual (parameters[0].value, 0.4f));
    CHECK (parameters[1].id == "2");

    // They aren't part of the song.
    CHECK (asPiece (joined) == asPiece (phazite::test::copyWholeSong (*made.song)));
}

TEST_CASE ("A version stores only what changed, and a note changed in a long clip stores its run alone")
{
    TemporaryFolder temporary;
    PiecesSong made;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);

    phazite::VersionRecord first;
    first.name = "First";
    first.saved = 1000;
    const auto firstPieces = phazite::splitSong (phazite::test::copyWholeSong (*made.song));
    const auto firstID = store.writeVersion (first, firstPieces);
    REQUIRE (firstID.isNotEmpty());
    const auto piecesAfterFirst = store.getPieces().getNumPieces();
    const auto sizeAfterFirst = store.getPieces().getSize();

    // One note in the middle of the long clip changes: its run, the clip, its track, the song and the version's record.
    const auto longNotes = phazite::getClipNotes (*made.song, made.longClip);
    REQUIRE (longNotes.size() > 2);
    phazite::setNoteVelocities (*made.song, { longNotes[longNotes.size() / 2].getRef() }, 7);

    phazite::VersionRecord second;
    second.parents.add (firstID);
    second.saved = 2000;
    const auto secondID = store.writeVersion (second, phazite::splitSong (phazite::test::copyWholeSong (*made.song)));
    REQUIRE (secondID.isNotEmpty());
    const auto added = store.getPieces().getNumPieces() - piecesAfterFirst;
    CHECK (added >= 5); // its run (two, if the note now ends one), the clip, the track, the song and the record
    CHECK (added <= 6);
    CHECK (store.getPieces().getSize() - sizeAfterFirst < 8000);

    // A note added among them changes one run (or two, where it ends one), not the runs after it.
    const auto piecesAfterSecond = store.getPieces().getNumPieces();
    addMidiNote (*made.song, made.longClip, 70, at (30.05), lasting (0.05));
    // And moved to the middle of the clip's notes, as a version from elsewhere can hold it: in the song whole.
    auto whole = phazite::test::copyWholeSong (*made.song);
    auto clip = made.findNotesClip (whole);
    REQUIRE (clip.isValid());
    clip.moveChild (clip.getNumChildren() - 1, clip.getNumChildren() / 2, nullptr);
    phazite::VersionRecord third;
    third.saved = 3000;
    REQUIRE (store.writeVersion (third, phazite::splitSong (whole)).isNotEmpty());
    CHECK (store.getPieces().getNumPieces() - piecesAfterSecond <= 6);

    // Both read back as they were saved, from a store opened again.
    phazite::VersionStore reopened (getVersionsFolder (temporary), thisComputer);
    const auto readFirst = reopened.readVersion (firstID);
    REQUIRE (readFirst.has_value());
    CHECK (readFirst->name == "First");
    CHECK (readFirst->song == firstPieces.song);
    CHECK (asPiece (reopened.readSong (readFirst->song))
           == asPiece (
               phazite::joinSong (firstPieces.song, [&firstPieces] (const juce::String& name) { return readFrom (firstPieces, name); })));

    const auto readSecond = reopened.readVersion (secondID);
    REQUIRE (readSecond.has_value());
    CHECK (readSecond->parents == juce::StringArray (firstID));
}

TEST_CASE ("A pack cut off midway leaves the store readable, and a damaged piece affects only its versions")
{
    TemporaryFolder temporary;
    PiecesSong made;
    const auto packs = getVersionsFolder (temporary).getChildFile ("pieces");
    juce::String firstID, secondID;

    {
        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        firstID = store.writeVersion ({}, phazite::splitSong (phazite::test::copyWholeSong (*made.song)));
        REQUIRE (firstID.isNotEmpty());
    }

    const auto firstPacks = packs.findChildFiles (juce::File::findFiles, false, "*.pack");
    REQUIRE (firstPacks.size() == 1);

    // A pack whose write was cut off: never read.
    {
        juce::MemoryBlock whole;
        REQUIRE (firstPacks[0].loadFileAsData (whole));
        REQUIRE (packs.getChildFile ("cut.pack").replaceWithData (whole.getData(), whole.getSize() / 2));
    }

    phazite::setChannelVolume (*made.song, made.tracks[1], -12.0f);

    {
        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        CHECK (store.readVersion (firstID).has_value());
        secondID = store.writeVersion ({}, phazite::splitSong (phazite::test::copyWholeSong (*made.song)));
        REQUIRE (secondID.isNotEmpty());
    }

    // The second version's own pieces are in a pack of their own: a byte of its pieces damaged, it can't be read, and the
    // first still can.
    juce::File secondPack;

    for (const auto& file : packs.findChildFiles (juce::File::findFiles, false, "*.pack"))
        if (file != firstPacks[0] && file.getFileName() != "cut.pack")
            secondPack = file;

    REQUIRE (secondPack.existsAsFile());
    juce::MemoryBlock bytes;
    REQUIRE (secondPack.loadFileAsData (bytes));
    static_cast<char*> (bytes.getData())[10] ^= 0x5a;
    REQUIRE (secondPack.replaceWithData (bytes.getData(), bytes.getSize()));

    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    const auto first = store.readVersion (firstID);
    REQUIRE (first.has_value());
    CHECK (store.readSong (first->song).isValid());

    const auto second = store.readVersion (secondID);
    CHECK ((! second.has_value() || ! store.readSong (second->song).isValid()));
}

TEST_CASE ("A version is written only if its pack reads back: a pack cut short under its own name is written again")
{
    TemporaryFolder temporary;
    const VersionsTime clock (juce::Time::currentTimeMillis()); // the same pieces at the same time: the same pack
    PiecesSong made;
    const auto packs = getVersionsFolder (temporary).getChildFile ("pieces");
    const auto pieces = phazite::splitSong (phazite::test::copyWholeSong (*made.song));
    juce::String id;

    {
        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        id = store.writeVersion ({}, pieces);
        REQUIRE (id.isNotEmpty());
    }

    // The pack as a write that failed midway once left it, in place under its name (a full disk, before writes were
    // checked): no store reads it.
    const auto found = packs.findChildFiles (juce::File::findFiles, false, "*.pack");
    REQUIRE (found.size() == 1);
    juce::MemoryBlock whole;
    REQUIRE (found[0].loadFileAsData (whole));
    REQUIRE (found[0].replaceWithData (whole.getData(), whole.getSize() / 2));

    {
        // The same version saved again: its pack is written again, whole, and the version reads.
        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        CHECK_FALSE (store.readVersion (id).has_value());
        CHECK (store.writeVersion ({}, pieces) == id);
        const auto version = store.readVersion (id);
        REQUIRE (version.has_value());
        CHECK (store.readSong (version->song).isValid());
    }

    CHECK (found[0].getSize() == (juce::int64) whole.getSize());
    CHECK (packs.findChildFiles (juce::File::findFiles, false).size() == 1); // and nothing left beside it

    // Where the pack can't be put in place (a folder of its name is in the way), the write says so: no version, never one
    // that can't be read.
    REQUIRE (found[0].deleteFile());
    REQUIRE (found[0].createDirectory().wasOk());
    REQUIRE (found[0].getChildFile ("In the way").replaceWithText ("in the way"));

    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    const auto written = store.writeVersion ({}, pieces);
    CHECK (written.isEmpty());
    CHECK_FALSE (store.readVersion (id).has_value());
    CHECK (packs.findChildFiles (juce::File::findFiles, false).isEmpty()); // nothing half written left beside it
}

TEST_CASE ("The branches and the current one are kept, each computer's record of a branch in its file")
{
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    CHECK (store.readBranches().empty());
    CHECK (store.readCurrentBranch().isEmpty());

    const auto main = makeBranch ("Main", juce::Colour (0xff56b4e9), "abc");
    auto idea = makeBranch ("Idea 2", juce::Colour (0xff009e73), "def");
    idea.working = "123";
    REQUIRE (store.writeBranch (main));
    REQUIRE (store.writeBranch (idea));
    REQUIRE (store.writeCurrentBranch (idea.id));

    // Each in a file of this computer's, the song left on one in this computer's own folder.
    CHECK (
        getVersionsFolder (temporary).getChildFile ("branches").getChildFile (main.id).getChildFile (thisComputer + ".xml").existsAsFile());
    CHECK (getVersionsFolder (temporary).getChildFile ("local").getChildFile (thisComputer).getChildFile ("working.xml").existsAsFile());

    phazite::VersionStore reopened (getVersionsFolder (temporary), thisComputer);
    const auto branches = reopened.readBranches();
    REQUIRE (branches.size() == 2);
    CHECK (std::count (branches.begin(), branches.end(), main) == 1);
    CHECK (std::count (branches.begin(), branches.end(), idea) == 1);
    CHECK (reopened.readCurrentBranch() == idea.id);

    // A branch written again replaces its file, and the song left on it goes once it is the current branch again.
    auto moved = main;
    moved.head = "xyz";
    auto entered = idea;
    entered.working = {};
    REQUIRE (reopened.writeBranch (moved));
    REQUIRE (reopened.writeBranch (entered));
    const auto again = reopened.readBranches();
    CHECK (again.size() == 2);
    CHECK (std::count (again.begin(), again.end(), moved) == 1);
    CHECK (std::count (again.begin(), again.end(), entered) == 1);

    // Another computer's record of the same branch is a file of its own, which this one never writes: its song left there
    // isn't this computer's to see.
    phazite::VersionStore elsewhere (getVersionsFolder (temporary), "another-computer");
    auto theirs = main;
    theirs.computer = "another-computer";
    theirs.head = "uvw";
    theirs.working = "456";
    REQUIRE (elsewhere.writeBranch (theirs));

    const auto all = reopened.readBranches();
    CHECK (all.size() == 3);
    CHECK (std::count (all.begin(), all.end(), moved) == 1);
    CHECK (std::any_of (all.begin(), all.end(), [] (const phazite::BranchRecord& record)
                        { return record.computer == "another-computer" && record.head == "uvw" && record.working.isEmpty(); }));
}

TEST_CASE (
    "Each computer's records of a branch make one branch: the last version that follows the others', or this computer's with the others' as lines of their own")
{
    std::map<juce::String, phazite::VersionRecord> versions;

    const auto add = [&versions] (const char* id, juce::StringArray parents)
    {
        phazite::VersionRecord record;
        record.id = id;
        record.parents = std::move (parents);
        versions.emplace (record.id, record);
    };

    add ("a", {});
    add ("b", { "a" });
    add ("c", { "b" });
    add ("d", { "b" });

    const auto record = [] (const char* computer, const char* head, juce::int64 changed, const char* name)
    {
        auto branch = makeBranch (name, juce::Colours::red, head);
        branch.id = "branch";
        branch.computer = computer;
        branch.changed = changed;
        return branch;
    };

    SUBCASE ("One computer's last version follows the other's: that one, with the newest name")
    {
        const auto joined =
            phazite::joinBranchRecords ({ record ("me", "b", 10, "main"), record ("them", "c", 20, "trunk") }, versions, "me");
        REQUIRE (joined.size() == 1);
        CHECK (joined[0].head == "c");
        CHECK (joined[0].name == "trunk");
        CHECK (joined[0].divergent.isEmpty());
        CHECK (joined[0].computer == "me");
    }

    SUBCASE ("The lines went their own ways: this computer's, with the other's as divergent")
    {
        const auto joined =
            phazite::joinBranchRecords ({ record ("me", "c", 10, "main"), record ("them", "d", 5, "main") }, versions, "me");
        REQUIRE (joined.size() == 1);
        CHECK (joined[0].head == "c");
        CHECK (joined[0].divergent == juce::StringArray ("d"));
    }

    SUBCASE ("A computer with no record of it: the newest record's line, with the other's as divergent")
    {
        const auto joined =
            phazite::joinBranchRecords ({ record ("one", "c", 10, "main"), record ("two", "d", 30, "main") }, versions, "me");
        REQUIRE (joined.size() == 1);
        CHECK (joined[0].head == "d");
        CHECK (joined[0].divergent == juce::StringArray ("c"));
    }
}

TEST_CASE ("A branch's name is kept as typed, spaces and all, in its record's file, which is named by its id")
{
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    auto branch = makeBranch ("Half time  feel / take 2", juce::Colours::red, "a");
    REQUIRE (store.writeBranch (branch));

    const auto read = store.readBranches();
    REQUIRE (read.size() == 1);
    CHECK (read[0].name == "Half time  feel / take 2");
    CHECK (read[0].id == branch.id);
    CHECK (read[0].others.isEmpty());
    CHECK (getVersionsFolder (temporary).getChildFile ("branches").getChildFile (branch.id).isDirectory());
}

TEST_CASE ("A branch's name read from a record anyone may have written is made as a typed one is, and is never empty")
{
    std::map<juce::String, phazite::VersionRecord> versions;
    phazite::VersionRecord version;
    version.id = "a";
    versions.emplace (version.id, version);

    auto broken = makeBranch ("Verse\nidea\t2" + juce::String::repeatedString ("x", 200), juce::Colours::red, "a");
    auto empty = makeBranch (juce::String::charToString (0x200b) + "  ", juce::Colours::blue, "a");
    const auto joined = phazite::joinBranchRecords ({ broken, empty }, versions, thisComputer);
    REQUIRE (joined.size() == 2);

    for (const auto& branch : joined)
    {
        if (branch.id == broken.id)
        {
            CHECK (branch.name.startsWith ("Verse idea 2xxx"));
            CHECK (branch.name.length() == phazite::maxBranchName);
        }
        else
        {
            CHECK (branch.name == "branch");
        }
    }
}

TEST_CASE ("A branch's log on this computer keeps each move of its last version, and leaves out a line a crash cut off")
{
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    REQUIRE (store.logBranchMove ("branch", {}, "a", "branch", "Half time"));
    REQUIRE (store.logBranchMove ("branch", "a", "b", "commit", "Half time"));
    REQUIRE (store.logBranchMove ("branch", "b", "c", "merge", "Half time  idea"));
    REQUIRE (store.logBranchMove ("branch", "c", {}, "delete", "Half time idea", "song"));

    // A line cut off midway by a crash.
    const auto log =
        getVersionsFolder (temporary).getChildFile ("local").getChildFile (thisComputer).getChildFile ("logs").getChildFile ("branch");
    REQUIRE (log.appendText ("123 c d com", false, false));

    const auto moves = store.readBranchLog ("branch");
    REQUIRE (moves.size() == 4);
    CHECK (moves[0].from.isEmpty());
    CHECK (moves[0].to == "a");
    CHECK (moves[0].why == "branch");
    CHECK (moves[0].name == "Half time");
    CHECK (moves[0].song.isEmpty());
    CHECK (moves[1].from == "a");
    CHECK (moves[2].to == "c");
    CHECK (moves[2].why == "merge");
    CHECK (moves[2].name == "Half time idea"); // made as a typed name is
    CHECK (moves[2].time > 0);
    CHECK (moves[3].to.isEmpty());
    CHECK (moves[3].why == "delete");
    CHECK (moves[3].song == "song");

    // A move logged after the line a crash cut off is a line of its own, read whole, not run into it.
    REQUIRE (store.logBranchMove ("branch", "d", "e", "commit", "After the crash"));
    const auto after = store.readBranchLog ("branch");
    REQUIRE (after.size() == 5);
    CHECK (after[4].from == "d");
    CHECK (after[4].to == "e");
    CHECK (after[4].name == "After the crash");

    // A name anyone wrote into a log is made as a typed one is.
    REQUIRE (log.appendText ("124 e f commit - Verse\tidea  2\n", false, false));
    CHECK (store.readBranchLog ("branch").back().name == "Verse idea 2");

    // Every computer's logs, read together, say whose and of which branch.
    phazite::VersionStore elsewhere (getVersionsFolder (temporary), "elsewhere");
    REQUIRE (elsewhere.logBranchMove ("other", {}, "x", "branch", "Theirs"));
    const auto every = store.readEveryLog();
    REQUIRE (every.size() == 7);
    CHECK (std::count_if (every.begin(), every.end(), [] (const phazite::BranchMove& move)
                          { return move.branch == "other" && move.computer == "elsewhere" && move.name == "Theirs"; })
           == 1);
}

TEST_CASE ("The audio files a song plays are hashed once, and again once they change")
{
    TemporaryFolder temporary;
    const auto project = temporary.folder.getChildFile ("Project");
    const auto file = project.getChildFile ("Audio").getChildFile ("Take 1.wav");
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.replaceWithText ("not really audio"));

    phazite::VersionStore store (project.getChildFile ("Versions"), thisComputer);
    const auto hashed = store.hashFiles ({ "Audio/Take 1.wav", "Audio/Missing.wav" }, project);
    REQUIRE (hashed.size() == 1);
    CHECK (hashed[0].path == "Audio/Take 1.wav");
    CHECK (hashed[0].size == file.getSize());
    CHECK (hashed[0].sha256 == juce::SHA256 (file).toHexString());
    CHECK (project.getChildFile ("Versions").getChildFile ("local").getChildFile (thisComputer).getChildFile ("files.xml").existsAsFile());

    REQUIRE (file.replaceWithText ("now longer than it was"));
    file.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (10.0));
    const auto again = store.hashFiles ({ "Audio/Take 1.wav" }, project);
    REQUIRE (again.size() == 1);
    CHECK (again[0].sha256 == juce::SHA256 (file).toHexString());
    CHECK (again[0].sha256 != hashed[0].sha256);
}

TEST_CASE ("The audio files a song plays are found in its clips and its devices' sounds")
{
    // As a song's elements hold them (Schema.h): an audio clip's file, and a drum sampler's pad's.
    const juce::ValueTree song (
        schema::PHAZITE, {},
        { juce::ValueTree (
            schema::TRACKS, {},
            { juce::ValueTree (
                schema::TRACK, { { schema::trackID, 2 } },
                { juce::ValueTree (
                      schema::CLIP,
                      { { schema::clipID, 3 }, { schema::clipKind, schema::clipIsAudio }, { schema::clipFile, "Audio/Vocal.wav" } }),
                  juce::ValueTree (schema::DEVICE, { { schema::deviceID, 4 } },
                                   { juce::ValueTree (schema::SOUNDS, { { schema::getPadSoundFile (2), "Audio/Snare.wav" },
                                                                        { schema::getPadSound (3), "808 Kick" } }) }) }) }) });

    const auto paths = phazite::findAudioFilePaths (song);
    CHECK (paths.contains ("Audio/Vocal.wav"));
    CHECK (paths.contains ("Audio/Snare.wav"));
    CHECK (paths.size() == 2);
}

namespace
{
// A pack's bytes as PieceStore.h lays them out: where its index starts, and how many entries it has, each of 56 bytes.
struct PackBytes
{
    juce::MemoryBlock bytes;
    size_t indexStart = 0;
    juce::uint32 count = 0;

    explicit PackBytes (const juce::File& file)
    {
        REQUIRE (file.loadFileAsData (bytes));
        const auto* end = static_cast<const char*> (bytes.getData()) + bytes.getSize() - 32;
        count = juce::ByteOrder::littleEndianInt (end);
        indexStart = (size_t) juce::ByteOrder::littleEndianInt64 (end + 4);
    }

    char* findEntry (const juce::String& name)
    {
        for (juce::uint32 i = 0; i < count; ++i)
            if (auto* entry = static_cast<char*> (bytes.getData()) + indexStart + (size_t) i * 56;
                juce::String::toHexString (entry, 32, 0) == name)
                return entry;

        return nullptr;
    }

    // Its name: the SHA-256 of its index and its writer (the 16 bytes before the closing mark).
    juce::String getIndexName() const
    {
        juce::MemoryBlock named (static_cast<const char*> (bytes.getData()) + indexStart, (size_t) count * 56);
        named.append (static_cast<const char*> (bytes.getData()) + bytes.getSize() - 20, 16);
        return juce::SHA256 (named.getData(), named.getSize()).toHexString();
    }
};
} // namespace

TEST_CASE ("A pack whose index doesn't hash to its name is left out, and a size no piece of its length could have is refused")
{
    TemporaryFolder temporary;
    PiecesSong made;
    const auto pieces = phazite::splitSong (phazite::test::copyWholeSong (*made.song));
    juce::String id;

    {
        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        id = store.writeVersion ({}, pieces);
        REQUIRE (id.isNotEmpty());
    }

    const auto folder = getVersionsFolder (temporary).getChildFile ("pieces");
    const auto pack = folder.findChildFiles (juce::File::findFiles, false, "*.pack")[0];
    PackBytes changed (pack);
    auto* entry = changed.findEntry (id);
    REQUIRE (entry != nullptr);

    SUBCASE ("A damaged index: none of the pack is read")
    {
        entry[33] ^= 0x01; // where the version's piece starts, one bit off
        REQUIRE (pack.replaceWithData (changed.bytes.getData(), changed.bytes.getSize()));

        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        CHECK (store.getPieces().getNumPieces() == 0);
        CHECK (! store.readVersion (id).has_value());
    }

    SUBCASE ("An index that says a piece holds 4 GB: the piece isn't read, nor anything allocated for it, and the rest is")
    {
        const juce::uint32 huge = 0xffffffffu;
        std::memcpy (entry + 44, &huge, sizeof (huge));
        REQUIRE (pack.deleteFile());
        REQUIRE (folder.getChildFile (changed.getIndexName() + ".pack").replaceWithData (changed.bytes.getData(), changed.bytes.getSize()));

        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        CHECK (store.getPieces().has (id));
        CHECK (! store.getPieces().read (id).has_value());
        CHECK (! store.readVersion (id).has_value());
        CHECK (store.readSong (pieces.song).isValid());
    }

    SUBCASE ("An index that says a piece is empty, or starts where no file reaches: that piece is left out, and the rest is read")
    {
        SUBCASE ("Empty")
        {
            const juce::uint32 none = 0;
            std::memcpy (entry + 40, &none, sizeof (none));
        }

        SUBCASE ("Starting too far to count from")
        {
            const auto tooFar = std::numeric_limits<juce::int64>::max();
            std::memcpy (entry + 32, &tooFar, sizeof (tooFar));
        }

        REQUIRE (pack.deleteFile());
        REQUIRE (folder.getChildFile (changed.getIndexName() + ".pack").replaceWithData (changed.bytes.getData(), changed.bytes.getSize()));

        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
        CHECK (! store.getPieces().has (id));
        CHECK (! store.readVersion (id).has_value());
        CHECK (store.readSong (pieces.song).isValid());
    }
}

TEST_CASE ("A branch's file keeps what this build doesn't know when the branch is written again")
{
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    const auto main = makeBranch ("main", juce::Colour (0xff56b4e9), "abc");
    REQUIRE (store.writeBranch (main));

    // A field a later Phazite added to the file.
    const auto file = getVersionsFolder (temporary).getChildFile ("branches").getChildFile (main.id).getChildFile (thisComputer + ".xml");
    auto xml = juce::parseXML (file);
    REQUIRE (xml != nullptr);
    xml->setAttribute ("laterField", "kept");
    REQUIRE (xml->writeTo (file));

    const auto read = store.readBranches();
    REQUIRE (read.size() == 1);
    CHECK (read[0].others["laterField"] == juce::var ("kept"));

    auto renamed = read[0];
    renamed.name = "trunk";
    REQUIRE (store.writeBranch (renamed));

    const auto again = juce::parseXML (file);
    REQUIRE (again != nullptr);
    CHECK (again->getStringAttribute ("name") == "trunk");
    CHECK (again->getStringAttribute ("laterField") == "kept");
}

TEST_CASE ("Where two lines parted is found by the graph, whatever the computers' clocks said")
{
    std::map<juce::String, phazite::VersionRecord> versions;

    const auto add = [&versions] (const char* id, juce::int64 saved, juce::StringArray parents)
    {
        phazite::VersionRecord record;
        record.id = id;
        record.saved = saved;
        record.parents = std::move (parents);
        versions.emplace (record.id, record);
    };

    SUBCASE ("A version saved on a computer whose clock was behind")
    {
        add ("x", 2000, {});
        add ("y", 1000, { "x" }); // after x, though its clock says before
        add ("a", 3000, { "y" });
        add ("b", 3100, { "y" });
        CHECK (phazite::findMergeBase (versions, "a", "b") == "y");
        CHECK (phazite::findMergeBase (versions, "b", "a") == "y");
        CHECK (phazite::findMergeBase (versions, "a", "y") == "y");
    }

    SUBCASE ("Merges that crossed: of the two best, the one saved last, the same both ways")
    {
        add ("x", 10, {});
        add ("a1", 30, { "x" });
        add ("b1", 20, { "x" });
        add ("a2", 50, { "a1", "b1" });
        add ("b2", 60, { "b1", "a1" });
        CHECK (phazite::findMergeBase (versions, "a2", "b2") == "a1");
        CHECK (phazite::findMergeBase (versions, "b2", "a2") == "a1");
    }

    SUBCASE ("No common line")
    {
        add ("x", 10, {});
        add ("z", 20, {});
        CHECK (phazite::findMergeBase (versions, "x", "z").isEmpty());
    }
}

TEST_CASE ("A history whose current branch is gone opens on the branch made first")
{
    TemporaryFolder temporary;
    auto song = phazite::createSong (temporary.folder.getChildFile ("Song.phazite"));
    const auto firstPieces = phazite::splitSong (phazite::test::copyWholeSong (*song));
    song->getState().setProperty ("madeOnLater", true, nullptr);
    const auto laterPieces = phazite::splitSong (phazite::test::copyWholeSong (*song));
    phazite::VersionStore store (temporary.folder.getChildFile ("Versions"), thisComputer);

    phazite::VersionRecord first, later;
    first.saved = 1000;
    first.branch = "first";
    later.saved = 2000;
    later.branch = "later";
    const auto firstID = store.writeVersion (first, firstPieces), laterID = store.writeVersion (later, laterPieces);

    // Written in the other order, and named so that the folder lists the later one first.
    auto laterBranch = makeBranch ("a-later", juce::Colours::red, laterID),
         firstBranch = makeBranch ("b-first", juce::Colours::blue, firstID);
    laterBranch.id = "later";
    firstBranch.id = "first";
    REQUIRE (store.writeBranch (laterBranch));
    REQUIRE (store.writeBranch (firstBranch));
    REQUIRE (store.writeCurrentBranch ("gone"));

    phazite::VersionHistory history (*song, {});
    REQUIRE (dispatchMessagesUntil ([&history] { return ! history.isReading() && ! history.isBusy(); }, versionHistoryTimeout));
    REQUIRE (history.findCurrentBranch() != nullptr);
    CHECK (history.findCurrentBranch()->id == "first");

    // And the changes are listed against that branch's last version.
    REQUIRE (history.getLastVersionSong().isValid());
    CHECK_FALSE (history.getLastVersionSong().hasProperty ("madeOnLater"));
}

TEST_CASE ("A piece names what it holds, so a song with one that can't be read joins as far as it can, around a stand-in")
{
    PiecesSong made;
    const auto pieces = phazite::splitSong (phazite::test::copyWholeSong (*made.song));
    const auto shortClip = "CLIP:" + juce::String ((juce::int64) made.shortClip.getRaw());

    // The short clip's piece, found by what its reference says of it, as a tool would without reading it.
    juce::String shortClipPiece;

    for (const auto& [name, piece] : pieces.pieces)
        for (const auto& reference : phazite::findReferences (piece))
            if (reference.what == shortClip)
                shortClipPiece = reference.piece;

    REQUIRE (shortClipPiece.isNotEmpty());

    const auto withoutIt = [&] (const juce::String& name) -> std::optional<juce::MemoryBlock>
    { return name == shortClipPiece ? std::nullopt : readFrom (pieces, name); };

    // Whole or not at all; or, asked for as much as can be read, with a stand-in of its type and key in its place.
    CHECK_FALSE (phazite::joinSong (pieces.song, withoutIt).isValid());

    std::vector<phazite::MissingPiece> missing;
    const auto joined = phazite::joinSong (pieces.song, withoutIt, nullptr, &missing);
    REQUIRE (joined.isValid());
    REQUIRE (missing.size() == 1);
    CHECK (missing[0].piece == shortClipPiece);
    CHECK (missing[0].path[missing[0].path.size() - 1] == shortClip);
    CHECK (missing[0].path.contains ("TRACK:" + juce::String ((juce::int64) made.tracks[1].getRaw())));

    // The song changed since, both clips renamed: made like what was read, the long clip goes back, and the one that
    // couldn't be read stays as it is now. No stand-in is left in the song.
    for (const auto clip : { made.longClip, made.shortClip })
    {
        auto renamed = *phazite::getClipValue (*made.song, clip);
        renamed.name = "Changed";
        phazite::setClip (*made.song, renamed);
    }

    phazite::makeSongLike (*made.song, joined);

    CHECK (phazite::getClipValue (*made.song, made.longClip)->name == "Long");
    CHECK (phazite::getClipValue (*made.song, made.shortClip)->name == "Changed");
    CHECK_FALSE (
        phazite::writeElement (phazite::test::copyWholeSong (*made.song)).toString().contains (schema::versions::missing.toString()));
}

TEST_CASE ("Many packs are joined into one, every piece still read")
{
    TemporaryFolder temporary;
    PiecesSong made;
    std::vector<juce::String> ids;

    {
        phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);

        for (int i = 0; i < 5; ++i)
        {
            phazite::setChannelVolume (*made.song, made.tracks[1], -1.0f - (float) i);
            ids.push_back (store.writeVersion ({}, phazite::splitSong (phazite::test::copyWholeSong (*made.song))));
        }

        CHECK (store.getPieces().getNumPacks() == 5);
        const auto numPieces = store.getPieces().getNumPieces();
        REQUIRE (store.getPieces().joinPacks());
        CHECK (store.getPieces().getNumPacks() == 1);
        CHECK (store.getPieces().getNumPieces() == numPieces);

        for (const auto& id : ids)
        {
            const auto version = store.readVersion (id);
            REQUIRE (version.has_value());
            CHECK (store.readSong (version->song).isValid());
        }
    }

    const auto packs = getVersionsFolder (temporary).getChildFile ("pieces").findChildFiles (juce::File::findFiles, false);
    CHECK (packs.size() == 1);

    phazite::VersionStore reopened (getVersionsFolder (temporary), thisComputer);
    CHECK (reopened.getPieces().getNumPacks() == 1);
    CHECK (reopened.readVersion (ids.back()).has_value());
}

TEST_CASE ("Commits' messages edited on each computer are kept apart and read together, the newest for each commit")
{
    TemporaryFolder temporary;
    phazite::VersionStore here (getVersionsFolder (temporary), thisComputer), there (getVersionsFolder (temporary), "elsewhere");

    REQUIRE (here.writeMessage ("one", { "Mine", "A note", 2000 }));
    REQUIRE (here.writeMessage ("two", { "Mine too", {}, 1000 }));
    REQUIRE (there.writeMessage ("two", { "Theirs", "Later", 3000 }));
    REQUIRE (there.writeMessage ("one", { "Older", {}, 500 }));
    REQUIRE (here.writeMessage ("one", { "Mine again", {}, 2500 })); // replaces this computer's own

    const auto messages = here.readMessages();
    REQUIRE (messages.size() == 2);
    CHECK (messages.at ("one") == phazite::MessageEdit { "Mine again", {}, 2500 });
    CHECK (messages.at ("two") == phazite::MessageEdit { "Theirs", "Later", 3000 });
    CHECK (getVersionsFolder (temporary).getChildFile ("messages").findChildFiles (juce::File::findFiles, false).size() == 2);
}

TEST_CASE ("Local History is this computer's list of snapshots, oldest first, a line cut off midway left out")
{
    TemporaryFolder temporary;
    const VersionsTime clock (3000);
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    CHECK (store.readSnapshots().empty());

    const std::vector<phazite::Snapshot> snapshots { { 1000, "main", "aaaa", "auto" }, { 2000, "idea", "bbbb", "restore" } };
    REQUIRE (store.writeSnapshots (snapshots));
    CHECK (store.readSnapshots() == snapshots);

    // A crash mid-line: the last line, without its end, is left out.
    const auto file = store.getLocalFolder().getChildFile ("history");
    REQUIRE (file.appendText ("3000 main cc", false, false, nullptr));
    CHECK (store.readSnapshots() == snapshots);
}

TEST_CASE ("A join copies every piece of the packs it rewrites from a copy it read, never trusting one elsewhere it didn't")
{
    // A join whose delete another program stopped leaves its old pack A = {x} beside the joined J = {x, z}; x is damaged in
    // J. The next join rewrites A (all of it in J too): x comes from A's good copy, not left to J's.
    TemporaryFolder temporary;
    const auto folder = temporary.folder.getChildFile ("pieces");
    const auto makePiece = [] (const juce::String& text)
    {
        const juce::MemoryBlock bytes (text.toRawUTF8(), text.getNumBytesAsUTF8());
        return std::pair { phazite::getPieceName (bytes), bytes };
    };
    const auto [x, xBytes] = makePiece ("<CLIP name=\"x\" text=\"" + juce::String::repeatedString ("x", 400) + "\"/>");
    const auto [z, zBytes] = makePiece ("<CLIP name=\"z\" text=\"" + juce::String::repeatedString ("z", 400) + "\"/>");

    {
        phazite::PieceStore store (folder, phazite::getThisComputer());
        REQUIRE (store.write ({ { x, xBytes } }));
        store.closeFiles();
    }

    const auto packA = folder.findChildFiles (juce::File::findFiles, false, "*.pack")[0];

    {
        // J written by the same computer apart (as a join did), then x's bytes in it damaged.
        const auto apart = temporary.folder.getChildFile ("apart");
        phazite::PieceStore store (apart, phazite::getThisComputer());
        phazite::setVersionsTimeForTests (juce::Time::currentTimeMillis() + 1000);
        REQUIRE (store.write ({ { x, xBytes }, { z, zBytes } }));
        phazite::setVersionsTimeForTests (0);
        store.closeFiles();
        const auto packJ = apart.findChildFiles (juce::File::findFiles, false, "*.pack")[0];
        juce::MemoryBlock j, a;
        REQUIRE (packJ.loadFileAsData (j));
        REQUIRE (packA.loadFileAsData (a));

        // x compressed, as A holds it between its mark and its index: found in J, a byte of it changed.
        const auto* aBytes = static_cast<const char*> (a.getData());
        const auto aIndexStart = (size_t) juce::ByteOrder::littleEndianInt64 (aBytes + a.getSize() - 28);
        auto* jBytes = static_cast<char*> (j.getData());
        const auto found = std::search (jBytes, jBytes + j.getSize(), aBytes + 4, aBytes + aIndexStart);
        REQUIRE (found != jBytes + j.getSize());
        found[(aIndexStart - 4) / 2] ^= 0x5a;
        REQUIRE (folder.getChildFile (packJ.getFileName()).replaceWithData (j.getData(), j.getSize()));
    }

    phazite::PieceStore::smallPackForTests = 16; // neither small: A is rewritten as a pack J holds the same pieces of

    {
        phazite::PieceStore store (folder, phazite::getThisComputer());
        REQUIRE (store.getNumOwnPacks() == 2);
        CHECK (store.joinPacks());
        store.closeFiles();
    }

    phazite::PieceStore::smallPackForTests = 0;
    const phazite::PieceStore store (folder, phazite::getThisComputer());
    CHECK (store.read (x).has_value());
    CHECK (store.read (z).has_value());
    store.closeFiles();
}
