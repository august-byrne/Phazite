// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions' store (docs/versions.md): a song in pieces and back, the packs they go in, and the branches, the current
// one and the audio files' hashes beside them.
#include "TestClips.h"
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
#include "model/project/internal/Project.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

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
        REQUIRE (phazite::setInstrument (*song, tracks[0], phazite::makeOwnDeviceState (*song, phazite::devices::Synth::typeID)).isValid());
        eq = phazite::insertDeviceState (*song, tracks[0], phazite::makeOwnDeviceState (*song, phazite::devices::Equalizer::typeID));

        longClip = addMidiClip (*song, tracks[0], { at (0.0), at (64.0) }, "Long");
        shortClip = addMidiClip (*song, tracks[1], { at (0.0), at (4.0) }, "Short");

        for (int i = 0; i < 600; ++i)
            addMidiNote (*song, longClip, 36 + i % 24, at (i * 0.1), lasting (0.1), 90 + i % 30);

        addMidiNote (*song, shortClip, 60, at (0.0), lasting (1.0));

        // A plug-in's element, its state a block JUCE writes in base 64 (Mixer.h: a plug-in's DEVICE).
        juce::MemoryBlock state;

        for (int i = 0; i < 3000; ++i)
            state.append (&i, 1);

        juce::ValueTree device (schema::DEVICE);
        device.setProperty (schema::deviceID, (juce::int64) song->makeID(), nullptr);
        device.setProperty (schema::deviceType, schema::deviceIsPlugin, nullptr);
        device.setProperty (schema::pluginName, "Test Synth", nullptr);
        device.setProperty (schema::pluginState, phazite::writeBytes (state), nullptr);
        plugin = phazite::insertDeviceState (*song, tracks[2], device);
        REQUIRE (plugin.isValid());
    }

    juce::ValueTree findNotesClip() const { return phazite::findClipState (*song, longClip); }
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
    const auto state = made.song->getState();
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
    const auto state = made.song->getState();
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
    const auto device = phazite::findDeviceState (*made.song, made.plugin);
    const auto read = phazite::readBytes (device[schema::pluginState].toString());
    REQUIRE (read.has_value());
    const auto& state = *read;

    const auto pieces = phazite::splitSong (made.song->getState());
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
    const auto pieces = phazite::splitSong (made.song->getState(), values);

    phazite::PluginParameterValues read;
    const auto joined = phazite::joinSong (pieces.song, [&pieces] (const juce::String& name) { return readFrom (pieces, name); }, &read);

    REQUIRE (read.count (made.plugin.getRaw()) == 1);
    const auto& parameters = read.at (made.plugin.getRaw());
    REQUIRE (parameters.size() == 2);
    CHECK (parameters[0].name == "Cutoff");
    CHECK (juce::approximatelyEqual (parameters[0].value, 0.4f));
    CHECK (parameters[1].id == "2");

    // They aren't part of the song.
    CHECK (asPiece (joined) == asPiece (made.song->getState()));
}

TEST_CASE ("A version stores only what changed, and a note changed in a long clip stores its run alone")
{
    TemporaryFolder temporary;
    PiecesSong made;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);

    phazite::VersionRecord first;
    first.name = "First";
    first.saved = 1000;
    const auto firstPieces = phazite::splitSong (made.song->getState());
    const auto firstID = store.writeVersion (first, firstPieces);
    REQUIRE (firstID.isNotEmpty());
    const auto piecesAfterFirst = store.getPieces().getNumPieces();
    const auto sizeAfterFirst = store.getPieces().getSize();

    // One note in the middle of the long clip changes: its run, the clip, its track, the song and the version's record.
    auto notes = made.findNotesClip();
    auto note = notes.getChild (notes.getNumChildren() / 2);
    REQUIRE (note.hasType (schema::NOTE));
    note.setProperty (schema::noteVelocity, 7, nullptr);

    phazite::VersionRecord second;
    second.parents.add (firstID);
    second.saved = 2000;
    const auto secondID = store.writeVersion (second, phazite::splitSong (made.song->getState()));
    REQUIRE (secondID.isNotEmpty());
    const auto added = store.getPieces().getNumPieces() - piecesAfterFirst;
    CHECK (added >= 5); // its run (two, if the note now ends one), the clip, the track, the song and the record
    CHECK (added <= 6);
    CHECK (store.getPieces().getSize() - sizeAfterFirst < 8000);

    // A note added among them changes one run (or two, where it ends one), not the runs after it.
    const auto piecesAfterSecond = store.getPieces().getNumPieces();
    addMidiNote (*made.song, made.longClip, 70, at (30.05), lasting (0.05));
    auto clip = made.findNotesClip();
    clip.moveChild (clip.getNumChildren() - 1, clip.getNumChildren() / 2, nullptr);
    phazite::VersionRecord third;
    third.saved = 3000;
    REQUIRE (store.writeVersion (third, phazite::splitSong (made.song->getState())).isNotEmpty());
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
        firstID = store.writeVersion ({}, phazite::splitSong (made.song->getState()));
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
        secondID = store.writeVersion ({}, phazite::splitSong (made.song->getState()));
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

TEST_CASE ("A computer's record of a commit checked out on its own names the branch it came from, and other computers leave it out")
{
    std::map<juce::String, phazite::VersionRecord> versions;
    phazite::VersionRecord version;
    version.id = "a";
    versions.emplace (version.id, version);

    auto main = makeBranch ("main", juce::Colours::red, "a");
    auto checkout = makeBranch ({}, juce::Colours::red, "a");
    checkout.detachedFrom = main.id;

    CHECK (phazite::joinBranchRecords ({ main, checkout }, versions, thisComputer).size() == 2);
    const auto elsewhere = phazite::joinBranchRecords ({ main, checkout }, versions, "another computer");
    REQUIRE (elsewhere.size() == 1);
    CHECK (elsewhere[0].id == main.id);

    // Its file keeps it (Schema.h: branchDetachedFrom).
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    REQUIRE (store.writeBranch (checkout));
    const auto read = store.readBranches();
    REQUIRE (read.size() == 1);
    CHECK (read[0].detachedFrom == main.id);
    CHECK (read[0].others.isEmpty());
}

TEST_CASE ("A branch's log on this computer keeps each move of its last version, and leaves out a line a crash cut off")
{
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary), thisComputer);
    REQUIRE (store.logBranchMove ("branch", {}, "a", "branch"));
    REQUIRE (store.logBranchMove ("branch", "a", "b", "commit"));
    REQUIRE (store.logBranchMove ("branch", "b", "c", "merge"));

    // A line cut off midway by a crash.
    const auto log =
        getVersionsFolder (temporary).getChildFile ("local").getChildFile (thisComputer).getChildFile ("logs").getChildFile ("branch");
    REQUIRE (log.appendText ("123 c d com", false, false));

    const auto moves = store.readBranchLog ("branch");
    REQUIRE (moves.size() == 3);
    CHECK (moves[0].from.isEmpty());
    CHECK (moves[0].to == "a");
    CHECK (moves[0].why == "branch");
    CHECK (moves[1].from == "a");
    CHECK (moves[2].to == "c");
    CHECK (moves[2].why == "merge");
    CHECK (moves[2].time > 0);
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
// A pack's bytes as PieceStore.h lays them out: where its index starts, and how many entries it has, each of 48 bytes.
struct PackBytes
{
    juce::MemoryBlock bytes;
    size_t indexStart = 0;
    juce::uint32 count = 0;

    explicit PackBytes (const juce::File& file)
    {
        REQUIRE (file.loadFileAsData (bytes));
        const auto* end = static_cast<const char*> (bytes.getData()) + bytes.getSize() - 16;
        count = juce::ByteOrder::littleEndianInt (end);
        indexStart = (size_t) juce::ByteOrder::littleEndianInt64 (end + 4);
    }

    char* findEntry (const juce::String& name)
    {
        for (juce::uint32 i = 0; i < count; ++i)
            if (auto* entry = static_cast<char*> (bytes.getData()) + indexStart + (size_t) i * 48;
                juce::String::toHexString (entry, 32, 0) == name)
                return entry;

        return nullptr;
    }

    juce::String getIndexName() const
    {
        return juce::SHA256 (static_cast<const char*> (bytes.getData()) + indexStart, (size_t) count * 48).toHexString();
    }
};
} // namespace

TEST_CASE ("A pack whose index doesn't hash to its name is left out, and a size no piece of its length could have is refused")
{
    TemporaryFolder temporary;
    PiecesSong made;
    const auto pieces = phazite::splitSong (made.song->getState());
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
    const auto firstPieces = phazite::splitSong (song->getState());
    song->getState().setProperty ("madeOnLater", true, nullptr);
    const auto laterPieces = phazite::splitSong (song->getState());
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
    const auto pieces = phazite::splitSong (made.song->getState());
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
    auto* undo = &made.song->getUndoManager();
    phazite::findClipState (*made.song, made.longClip).setProperty (schema::clipName, "Changed", undo);
    phazite::findClipState (*made.song, made.shortClip).setProperty (schema::clipName, "Changed", undo);
    phazite::makeSongLike (*made.song, joined);

    CHECK (phazite::findClipState (*made.song, made.longClip)[schema::clipName].toString() == "Long");
    CHECK (phazite::findClipState (*made.song, made.shortClip)[schema::clipName].toString() == "Changed");
    CHECK_FALSE (phazite::writeElement (made.song->getState()).toString().contains (schema::versions::missing.toString()));
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
            ids.push_back (store.writeVersion ({}, phazite::splitSong (made.song->getState())));
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
