// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions' store (docs/versions.md): a song in pieces and back, the packs they go in, and the branches, the current
// one and the audio files' hashes beside them.
#include "TestClips.h"
#include "TestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionStore.h"

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
        device.setProperty (schema::pluginState, state.toBase64Encoding(), nullptr);
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
        if (piece.toString().startsWith ("<RUN>"))
            ++runs;

    CHECK (runs >= 3);
    CHECK (runs <= 40);

    // A piece that can't be read: the song can't be.
    const auto missing = phazite::joinSong (pieces.song,
                                            [&pieces] (const juce::String& name) -> std::optional<juce::MemoryBlock>
                                            {
                                                if (pieces.pieces.at (name).toString().startsWith ("<RUN>"))
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
    juce::MemoryBlock state;
    REQUIRE (state.fromBase64Encoding (device[schema::pluginState].toString()));

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
    phazite::VersionStore store (getVersionsFolder (temporary));

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
    phazite::VersionStore reopened (getVersionsFolder (temporary));
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
        phazite::VersionStore store (getVersionsFolder (temporary));
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
        phazite::VersionStore store (getVersionsFolder (temporary));
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

    phazite::VersionStore store (getVersionsFolder (temporary));
    const auto first = store.readVersion (firstID);
    REQUIRE (first.has_value());
    CHECK (store.readSong (first->song).isValid());

    const auto second = store.readVersion (secondID);
    CHECK ((! second.has_value() || ! store.readSong (second->song).isValid()));
}

TEST_CASE ("The branches and the current one are kept, each branch in its file")
{
    TemporaryFolder temporary;
    phazite::VersionStore store (getVersionsFolder (temporary));
    CHECK (store.readBranches().empty());
    CHECK (store.readCurrentBranch().isEmpty());

    const phazite::BranchRecord main { juce::Uuid().toString(), "Main", juce::Colour (0xff56b4e9), "abc", {} };
    const phazite::BranchRecord idea { juce::Uuid().toString(), "Idea 2", juce::Colour (0xff009e73), "def", "123" };
    REQUIRE (store.writeBranch (main));
    REQUIRE (store.writeBranch (idea));
    REQUIRE (store.writeCurrentBranch (idea.id));

    phazite::VersionStore reopened (getVersionsFolder (temporary));
    const auto branches = reopened.readBranches();
    REQUIRE (branches.size() == 2);
    CHECK (std::count (branches.begin(), branches.end(), main) == 1);
    CHECK (std::count (branches.begin(), branches.end(), idea) == 1);
    CHECK (reopened.readCurrentBranch() == idea.id);

    // A branch written again replaces its file.
    auto moved = main;
    moved.head = "xyz";
    REQUIRE (reopened.writeBranch (moved));
    const auto again = reopened.readBranches();
    CHECK (again.size() == 2);
    CHECK (std::count (again.begin(), again.end(), moved) == 1);
}

TEST_CASE ("The audio files a song plays are hashed once, and again once they change")
{
    TemporaryFolder temporary;
    const auto project = temporary.folder.getChildFile ("Project");
    const auto file = project.getChildFile ("Audio").getChildFile ("Take 1.wav");
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.replaceWithText ("not really audio"));

    phazite::VersionStore store (project.getChildFile ("Versions"));
    const auto hashed = store.hashFiles ({ "Audio/Take 1.wav", "Audio/Missing.wav" }, project);
    REQUIRE (hashed.size() == 1);
    CHECK (hashed[0].path == "Audio/Take 1.wav");
    CHECK (hashed[0].size == file.getSize());
    CHECK (hashed[0].sha256 == juce::SHA256 (file).toHexString());
    CHECK (project.getChildFile ("Versions").getChildFile ("files.xml").existsAsFile());

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
