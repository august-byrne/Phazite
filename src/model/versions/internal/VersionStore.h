// SPDX-License-Identifier: AGPL-3.0-or-later
// A project's Versions folder (docs/versions-format.md; model/base/internal/Schema.h: schema::versions): its pieces
// (PieceStore.h); its versions, each a piece of its own that names its song's piece and the versions it follows; its
// branches, each computer's record of each in a file of its own; the current branch; and what is this computer's alone (the
// audio files' hashes, the songs left on branches, each branch's log). Each computer writes only its own files and the pieces,
// which never change, so that a folder synced between computers never has two writers on one file but the current branch's,
// which goes with the project file. Plain records and files, for any thread; its PieceStore takes care of its own threads,
// and the rest is read and written whole (a log is appended to).
//
//   Versions/
//     pieces/<name>.pack                pieces (PieceStore.h)
//     branches/<branch>/<computer>.xml  a branch as that computer last had it (BRANCH)
//     current                           the current branch's id
//     local/<computer>/files.xml        what each audio file this computer read hashes to (FILES)
//     local/<computer>/working.xml      the song left on each branch this computer switched from (WORKING)
//     local/<computer>/logs/<branch>    the branch's log on this computer: each move of its last version, a line each
//     local/<computer>/history          this computer's local history: the song as it was at times, a line each
//     local/<computer>/swept            when this computer last swept the store, and what of it is still to delete
//     messages/<computer>.xml           the commits' messages as that computer edited them (MESSAGES)
//     bounces/<version>.flac            a version's bounce
#pragma once

#include "PieceStore.h"
#include "SongMerge.h"
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
    juce::StringArray parents; // the versions it follows: none for the first, two for a merge (the second the one merged in)
    juce::String name, note, summary, branch, phazite;
    juce::int64 saved = 0; // milliseconds since 1970
    std::vector<File> files;

    // A merge's: the IDs it gave anew to what the other line added (SongMerge.h), so that a later merge of that line knows
    // them again.
    std::vector<RenumberedID> renumbered;
};

// A branch, as a computer's record of it holds it (Schema.h: BRANCH).
struct BranchRecord
{
    juce::String id, name;
    juce::Colour colour;
    juce::String head;       // its last version on that computer, or none before its first
    juce::String computer;   // whose record it is
    juce::int64 changed = 0; // when its name or color were last set, or it was deleted (milliseconds since 1970): the newest computer's win
    bool deleted = false;    // Delete Branch, on that computer (Schema.h: branchDeleted)

    // What its file holds that this build doesn't know (a later Phazite's), written back as it was whenever the branch is
    // written again (a rename), so that nothing a later build keeps there is lost.
    juce::NamedValueSet others;

    // This computer's: the song as it was left there (its piece), while another branch is the current one.
    juce::String working;

    // Once every computer's record is taken together (VersionHistory.h): the last versions of lines of it that went their
    // own way on other computers, which its head doesn't follow; none while the computers agree.
    juce::StringArray divergent;

    bool operator== (const BranchRecord&) const = default;
};

// A commit's message as it was edited after the commit (Edit Commit Message...), which names it in place of the one its
// record holds: its name and note (either empty), and when they were set (milliseconds since 1970).
struct MessageEdit
{
    juce::String name, note;
    juce::int64 changed = 0;

    bool operator== (const MessageEdit&) const = default;
};

// The song as it was at a time, kept without a commit in this computer's local history (as VS Code's and JetBrains' Local
// History keep a file's saves): when (milliseconds since 1970), the branch it was on, its song's piece, and why it was taken
// ("open", as the project opened; "auto", while the song changed; "restore", "discard", "merge" and "history", before what
// made the song another).
struct Snapshot
{
    juce::int64 time = 0;
    juce::String branch, song, reason;

    bool operator== (const Snapshot&) const = default;
};

// A song a computer keeps without a version (VersionStore::findKeptSongs()): its piece, the computer's folder name, its
// branch's id, and what it is: a song left on a branch; a snapshot of Local History, with when it was taken; or the song
// left on a branch that was deleted, kept as its versions are for 30 days, with when it was deleted.
struct KeptSong
{
    enum class Kind
    {
        left,
        snapshot,
        deleted
    };

    juce::String piece, computer, branch;
    Kind kind = Kind::left;
    juce::int64 time = 0;
};

// A move of a branch's last version on a computer, as its log keeps it (git's reflog): when, from where (none for none), to
// where (none for a branch deleted), why ("commit", "merge", "branch", "recover", "reset", "undo" for Undo Last Version,
// "delete", "cancel" for a branch a command made and took away again as it didn't finish), the branch's name then, and the
// song left on it (a deleted branch's, kept with its versions; a recovered branch's, which it came back with). And, as
// every computer's logs are read together, the branch's id and the computer's folder name.
struct BranchMove
{
    juce::int64 time = 0;
    juce::String from, to, why, name, song;
    juce::String branch, computer;
};

class VersionStore
{
public:
    // A store in a Versions folder, which needn't exist yet, as one computer writes it (VersionsModel.h: getThisComputer()).
    VersionStore (juce::File versionsFolder, juce::String computer);

    const juce::File& getFolder() const noexcept { return folder; }
    const juce::String& getComputer() const noexcept { return computer; }
    PieceStore& getPieces() noexcept { return pieces; }
    const PieceStore& getPieces() const noexcept { return pieces; }

    // Writes a version: the pieces of its song the store doesn't have yet, and its own, in one pack; the record's song
    // is the pieces' song. Returns its id, or nothing if it couldn't be written.
    juce::String writeVersion (VersionRecord, const SongPieces&, juce::String* problem = nullptr); // why not, if given (PieceStore.h)
    std::optional<VersionRecord> readVersion (const juce::String& id) const;

    // A song's pieces kept without a version (a branch's song as it was left), and a song read from its piece, with its
    // plug-ins' parameters' values if asked: invalid if one of its pieces is missing or damaged, or, given somewhere to list
    // what is missing, as much of it as can be read (SongPieces.h: joinSong()).
    bool writeSong (const SongPieces&);
    juce::ValueTree readSong (const juce::String& songPiece, PluginParameterValues* parameters = nullptr,
                              std::vector<MissingPiece>* missing = nullptr, bool* idsGivenAnew = nullptr) const;

    // How many songs every store read since the program started, and how many of them on the message thread: what the
    // performance tests count (tests/PerformanceGateTests.cpp), as a song read whole is the costly part of a store.
    struct SongsRead
    {
        int all = 0, onMessageThread = 0;
    };

    static SongsRead countSongsRead();
    static SongsRead countSongsWritten(); // writeSong()'s, likewise

    // Every computer's record of every branch (in no particular order), this computer's with the songs left on them. A
    // branch written as this computer's record, the song left on it kept on this computer alone. And which one the project
    // file is.
    std::vector<BranchRecord> readBranches() const;
    bool writeBranch (const BranchRecord&);
    juce::String readCurrentBranch() const;
    bool writeCurrentBranch (const juce::String& id);

    // A branch's log on this computer: a move appended (at getVersionsTime()), on a line of its own even after one a crash
    // cut off; and the moves, oldest first (a line cut off by a crash left out). Names are made as typed ones are
    // (VersionsModel.h: toBranchName()) as they are read, whoever wrote them.
    bool logBranchMove (const juce::String& branchID, const juce::String& from, const juce::String& to, const juce::String& why,
                        const juce::String& name = {}, const juce::String& song = {});
    std::vector<BranchMove> readBranchLog (const juce::String& branchID) const;

    // Every computer's log of a branch taken together (theirs too, whose folders sync with this one's), oldest first; and
    // every computer's log of every branch, deleted ones too, oldest first or, unsorted, each file's lines in their order
    // (a move's branch is its log file's name: makeUsableFileName() of the branch's id).
    std::vector<BranchMove> readEveryBranchLog (const juce::String& branchID) const;
    std::vector<BranchMove> readEveryLog (bool oldestFirst = true) const;

    // Whether every file that names what the store keeps reads whole, as written: every computer's records of branches,
    // its songs left on branches, its Local History and its logs (a log's or a history's last line cut off by a crash
    // apart); and holds no file a sync program made of two computers' writes of one file (a "conflicted copy", or another
    // name no Phazite writes), as one computer id used on two machines makes. Where not, a sweep can't know what they name,
    // and leaves nothing out (StoreSweep.h).
    bool areRecordsWhole() const;

    // When this computer last swept the store (0 for never; a time after now taken as now), and the versions swept away
    // whose bounces and edited messages are still to delete (written before the sweep's join, finished at the next open
    // once their records are gone); and both written.
    struct Swept
    {
        juce::int64 time = 0;
        juce::StringArray bounces;
    };

    Swept readSwept() const;
    bool writeSwept (const Swept&);

    // The edited messages of versions swept away, out of this computer's own file (each computer takes out its own).
    bool forgetMessages (const juce::StringArray& versionIDs);

    // The commits' messages as they were edited, every computer's taken together (the newest for each commit), by the
    // commits' ids; and one edited on this computer, written into its own file (messages/<computer>.xml).
    std::map<juce::String, MessageEdit> readMessages() const;
    bool writeMessage (const juce::String& versionID, const MessageEdit&);

    // This computer's local history (local/<computer>/history: a line a snapshot, "time branch song reason"), oldest first,
    // its songs' pieces kept as a branch's song left on it is (writeSong()), thinned as it is read (VersionHistory.h:
    // thinSnapshots()) as of a time (none: now), so that what the sweep lets go is never offered; and the list written whole.
    std::vector<Snapshot> readSnapshots (juce::int64 thinAt = 0) const;
    bool writeSnapshots (const std::vector<Snapshot>&);

    // The time the store's keeping goes by: now, but no more than a week past this computer's last sweep
    // (min (now, last sweep + 7 days); a last sweep after now taken as now; now if it never swept), so that a clock set
    // ahead, or a project opened after a long break, lets go of no more at one open than a sweep a week later would. What
    // the sweep keeps, and what Local History is thinned by as it is written, go by it.
    juce::int64 getKeepingTime() const;

    // The time what this computer writes now is stamped with (its log's lines, its snapshots, its versions, its packs'
    // pieces): the clock, but never before the floor settled as the store opens, and always later than the stamp before
    // (PieceStore::stamp()). The floor is the newest time this computer ever stamped, kept in local/<computer>/stamped and
    // taken from its logs, its Local History and its packs, never lowered and apart from the sweep's time: so a clock set
    // back (a dead battery, at every start) never makes what is done now look old; one that was ahead keeps what it stamped
    // longer, the safe way. Settled on the history's thread as it opens (StoreSweep.h), never written into a held folder.
    juce::int64 stamp() const { return pieces.stamp(); }
    void settleStampFloor();
    static constexpr juce::int64 sweepEvery = 7LL * 24 * 60 * 60 * 1000;
    static constexpr juce::int64 latestStampAhead = 10LL * 366 * 24 * 60 * 60 * 1000; // a floor source later is passed over

    // The songs every computer keeps without a version (theirs too, whose folders sync with this one's): those left on
    // branches, those of Local History, and those left on branches deleted, each piece once. As of a time: Local History
    // thinned and the deleted branches' songs of the last 30 days (keptByAge::dailyFor, by every computer's logs) as of it.
    // Or, as of none (0), every one the files list (while the sweep lets go, those whose piece the store still holds): what
    // anything could still bring back, whatever the clock says (Clean Up Unused Audio's, unsure where one can't be read).
    std::vector<KeptSong> findKeptSongs (juce::int64 asOf) const;

    // What the audio files at these paths (as a song holds them: within the project's folder, or full ones) hash to, each
    // read once while its size and its time stay the same (files.xml). A file that is missing is left out.
    std::vector<VersionRecord::File> hashFiles (const juce::StringArray& paths, const juce::File& projectFolder);

    // The versions' bounces, each the song as it was then rendered as Export Song renders it (docs/design/versions.md: Bounces and
    // hearing them), in their folder by their versions' ids; and a version's, whether it has one or not.
    juce::File getBouncesFolder() const { return folder.getChildFile ("bounces"); }
    juce::File getBounceFile (const juce::String& versionID) const;

    // This computer's own folder in it.
    juce::File getLocalFolder() const;

private:
    const juce::File folder;
    const juce::String computer;
    PieceStore pieces;
    juce::CriticalSection filesLock; // files.xml, one reader or writer at a time

    juce::File getBranchFile (const juce::String& id, const juce::String& computerID) const;
    std::map<juce::String, juce::String> readWorking() const; // by branch
    static std::map<juce::String, juce::String> readWorkingIn (const juce::File& localFolder);
    static std::vector<Snapshot> readSnapshotsIn (const juce::File& localFolder, juce::int64 thinAt); // 0: not thinned
    static std::vector<BranchMove> readLogFile (const juce::File&);
    bool writeWorking (const std::map<juce::String, juce::String>&);

    JUCE_DECLARE_NON_COPYABLE (VersionStore)
};

// A version's record as its piece holds it (Schema.h: VERSION), and back: nothing for what isn't one.
juce::ValueTree toElement (const VersionRecord&);
std::optional<VersionRecord> toVersionRecord (const juce::ValueTree&, const juce::String& id);

// The paths of the audio files a song plays (its audio clips', and its devices' sound files: Schema.h, getPadSoundFile()
// and getOscillatorWavetableFile()), each once, in order.
juce::StringArray findAudioFilePaths (const juce::ValueTree& song);
} // namespace phazite
