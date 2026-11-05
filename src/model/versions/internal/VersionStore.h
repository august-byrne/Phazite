// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's Versions folder (model/base/internal/Schema.h: schema::versions; docs/versions.md): its pieces
// (PieceStore.h), its versions, each a piece of its own that names its song's piece and the versions it follows, its
// branches, each in a file of its own, the current branch, and what each audio file the songs play hashes to. Plain
// records and files, for any thread; its PieceStore takes care of its own threads, and the rest is read and written whole.
#pragma once

#include "PieceStore.h"
#include "SongPieces.h"

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace phazite
{
// A version, as the store keeps it (Schema.h: VERSION): its id is its piece's name.
struct VersionRecord
{
    // An audio file its song plays: its path as the song holds it, its size, and the SHA-256 of what it holds.
    struct File
    {
        juce::String path;
        juce::int64 size = 0;
        juce::String sha256;

        bool operator== (const File&) const = default;
    };

    juce::String id, song;     // its own piece's name, and its song's piece's
    juce::StringArray parents; // the versions it follows: none for the first, two for a merge
    juce::String name, note, summary, branch, phazite;
    juce::int64 saved = 0; // milliseconds since 1970
    std::vector<File> files;
};

// A branch, as its file holds it (Schema.h: BRANCH).
struct BranchRecord
{
    juce::String id, name;
    juce::Colour colour;
    juce::String head;    // its last version, or none before its first
    juce::String working; // the song as it was left there (its piece), while another branch is the current one

    bool operator== (const BranchRecord&) const = default;
};

class VersionStore
{
public:
    explicit VersionStore (juce::File versionsFolder); // which needn't exist yet

    const juce::File& getFolder() const noexcept { return folder; }
    PieceStore& getPieces() noexcept { return pieces; }
    const PieceStore& getPieces() const noexcept { return pieces; }

    // Writes a version: the pieces of its song the store doesn't have yet, and its own, in one pack; the record's song
    // is the pieces' song. Returns its id, or nothing if it couldn't be written.
    juce::String writeVersion (VersionRecord, const SongPieces&);
    std::optional<VersionRecord> readVersion (const juce::String& id) const;

    // A song's pieces kept without a version (a branch's song as it was left), and a song read from its piece, with its
    // plug-ins' parameters' values if asked: invalid if one of its pieces is missing or damaged.
    bool writeSong (const SongPieces&);
    juce::ValueTree readSong (const juce::String& songPiece, PluginParameterValues* parameters = nullptr) const;

    // The branches, each in its file (in no particular order), and which one the project file is.
    std::vector<BranchRecord> readBranches() const;
    bool writeBranch (const BranchRecord&);
    juce::String readCurrentBranch() const;
    bool writeCurrentBranch (const juce::String& id);

    // What the audio files at these paths (as a song holds them: within the project's folder, or full ones) hash to, each
    // read once while its size and its time stay the same (files.xml). A file that is missing is left out.
    std::vector<VersionRecord::File> hashFiles (const juce::StringArray& paths, const juce::File& projectFolder);

    // The versions' bounces, each the song as it was then rendered as Export Song renders it (docs/versions.md: Bounces and
    // hearing them), in their folder by their versions' ids; and a version's, whether it has one or not.
    juce::File getBouncesFolder() const { return folder.getChildFile ("bounces"); }
    juce::File getBounceFile (const juce::String& versionID) const;

private:
    const juce::File folder;
    PieceStore pieces;
    juce::CriticalSection filesLock; // files.xml, one reader or writer at a time

    juce::File getBranchFile (const juce::String& id) const;

    JUCE_DECLARE_NON_COPYABLE (VersionStore)
};

// The paths of the audio files a song plays (its audio clips', and its devices' sound files: Schema.h, getPadSoundFile()
// and getOscillatorWavetableFile()), each once, in order.
juce::StringArray findAudioFilePaths (const juce::ValueTree& song);
} // namespace phazite
