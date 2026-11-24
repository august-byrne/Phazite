// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsModel.h"
#include "internal/ApplySong.h"
#include "internal/AreaClashes.h"
#include "internal/SongMerge.h"
#include "internal/VersionHistory.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/WholeState.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AppInfo.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Freeze.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/AutomationWriter.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/Export.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    VersionInfo toInfo (const VersionHistory& history, const VersionRecord& version)
    {
        return { version.id,      getDisplayName (version),  version.note,
                 version.summary, version.name.isNotEmpty(), juce::Time (version.saved),
                 version.branch,  version.parents,           history.hasBounce (version.id) };
    }

    BranchInfo toInfo (const BranchRecord& branch)
    {
        return { branch.id, branch.name, branch.colour, branch.head };
    }

    VersionHistory* findHistory (const ProjectContext& context)
    {
        auto* history = getVersionHistory (context);
        return history != nullptr && history->keepsVersions() ? history : nullptr;
    }

    // The song as it is, kept in Local History before what makes it another (below).
    void keepSongBefore (VersionHistory&, const juce::String& reason);
} // namespace

//==============================================================================
// A command that reads whole songs before it changes anything (VersionHistory.h: startCommandRead()).
bool startCommandRead (ProjectContext& context, VersionHistory& history, const juce::String& title,
                       std::function<VersionHistory::TakeIn()> read, std::function<void (bool)> done,
                       std::shared_ptr<std::atomic<float>> progress)
{
    if (history.isReadingForCommand() || history.getStore() == nullptr)
        return false;

    const auto cancel = [&context] (const juce::String& why)
    {
        if (auto* reading = findHistory (context))
            reading->cancelReading (why);
    };

    std::shared_ptr<ProjectContext::WorkHold> hold = context.holdForWork (
        title, cancel, progress != nullptr ? std::function<float()> ([progress] { return progress->load(); }) : nullptr);
    return history.startReading (title, std::move (read), std::move (hold), std::move (done));
}

namespace
{
    // A song of the store read on the history's thread, a commit's (without a merge's element) or one kept without a commit
    // (a snapshot's), with its plug-ins' values, and as much of it as can be read where asked (missing); then taken in.
    struct SongRead
    {
        juce::ValueTree song;
        PluginParameterValues parameters;
        std::vector<MissingPiece> missing;
        bool idsGivenAnew = false; // it held an ID twice, which the checks gave anew as it was read
    };

    enum class Kept
    {
        asCommit,
        asItIs
    };

    bool startSongRead (ProjectContext& context, VersionHistory& history, const juce::String& title, const juce::String& piece,
                        bool asMuchAsCan, std::function<VersionHistory::Taken (SongRead&)> takeIn, Kept kept = Kept::asCommit)
    {
        return startCommandRead (context, history, title,
                                 [store = history.getStore(), piece, asMuchAsCan, kept, takeIn]() -> VersionHistory::TakeIn
                                 {
                                     auto read = std::make_shared<SongRead>();
                                     auto* missing = asMuchAsCan ? &read->missing : nullptr;
                                     read->song = kept == Kept::asCommit
                                                      ? readCommitSong (*store, piece, &read->parameters, missing, &read->idsGivenAnew)
                                                      : store->readSong (piece, &read->parameters, missing, &read->idsGivenAnew);
                                     store->getPieces().closeFiles();
                                     return [read, takeIn] { return takeIn (*read); };
                                 });
    }

    // Whether the song can be made another now, as a command that read on the history's thread takes what it read in: not
    // while it records, nor while a render has its plug-ins.
    bool canChangeSongNow (const ProjectContext& context)
    {
        const auto* history = findHistory (context);
        const auto* song = findSong (context);
        const auto* session = getSession (context);
        return history != nullptr && song != nullptr && ! history->isReading() && ! history->isSaving() && ! isRecording (*song)
               && (session == nullptr || ! session->isLentToRender());
    }

    // Why a song can't be made another now, as what was read is taken in; none while it can. Never while a version is open
    // in its place (a command that makes the song another goes back to Now first), but for opening another (showing).
    juce::String whyNotChangeSongNow (const ProjectContext& context, const juce::String& what, bool showing = false)
    {
        const auto* song = findSong (context);
        const auto* session = getSession (context);

        if (song != nullptr && song->isShowingOther() && ! showing)
            return "A version was opened while the song was read: " + what + " Go Back to Now first.";

        if (song != nullptr && isRecording (*song))
            return "The song started recording while it was read: " + what + " Stop recording first.";

        if (session != nullptr && session->isLentToRender())
            return "A render started while the song was read: " + what + " Let it finish first.";

        return canChangeSongNow (context) ? juce::String() : "The project's versions are busy: " + what;
    }

    // Why a restore of a commit that held an ID twice takes nothing: given anew as it was read, an item would come in as new,
    // beside the song's own, unseen (as a merge refuses such a commit).
    juce::String idsTwiceRefusal()
    {
        return "Nothing was restored: that version holds an ID twice, or IDs too large to count on from, which no Phazite writes "
               "(another tool wrote it), so what it holds can't be told from what the song has.";
    }

    // Why a version's song numbered otherwise than the song (Schema.h: idNumbering, areNumberedAlike()) isn't matched with
    // it by ID, for a line's restore and a merge to say, true whichever of the two is the older: a version from before the
    // song's IDs were given anew as it opened, or the song itself (after Restore This Version of a version from before, or on
    // a second computer still on the song from before) against a version saved after. Matched by ID, an item of one would be
    // taken for another of the other's (known bug 186). What makes the song another whole (a version opened, Restore This
    // Version) matches nothing that way, and takes it.
    juce::String numberedOtherwise (const juce::String& version)
    {
        return version
               + " and the song number their tracks and clips differently (one of them is from before the song's items were "
                 "numbered anew as it opened), so Phazite can't tell which track is which.";
    }

    juce::String otherNumberingRefusal (const juce::String& version)
    {
        return "Nothing was restored: " + numberedOtherwise (version)
               + " Open the version to hear it, or restore the whole version (Restore This Version).";
    }

    // Why a line's restore or discard takes nothing where it would give a track an ID the song holds now for something else
    // (ApplySong.h: givesTrackIDsHeldElsewhere()), after what it is (what: "Couldn't restore ..."), with what does work.
    juce::String idsElsewhereRefusal (const juce::String& what, const juce::String& whatWorks)
    {
        return what
               + ": it would give a track a number the song now gives something else (a branch made on another computer can "
                 "number its items as this one does), so Phazite can't tell the two apart. "
               + whatWorks;
    }

    // A merge's refusal for the same reason, with what does work: on another branch, going on from it there; on the song's
    // own branch (Merge the Newer Versions), the newer versions taken whole once the song's changes are discarded (which
    // Local History keeps), as Take the Newer Versions takes them without a match by ID.
    juce::String mergeNumberingRefusal (const VersionHistory& history, const juce::String& versionID)
    {
        const auto* version = history.findVersion (versionID);
        const auto* branch = history.findCurrentBranch();
        const auto name = version != nullptr ? "\"" + getDisplayName (*version) + "\"" : juce::String ("that version");
        const auto ownBranch = version != nullptr && branch != nullptr && version->branch == branch->id;

        return "Nothing was merged: " + numberedOtherwise (name + " (or the version both lines have)")
               + (ownBranch
                      ? " To go on from the newer versions: Cancel Merge if its list is open, then Discard All Changes, then Take the "
                        "Newer Versions (an automatic save keeps the song as it is now)."
                      : " Switch to its branch to go on from it, or open it to hear it.");
    }

    // Restore from This Commit, once the commit's song is read.
    VersionHistory::Taken takeInBringBack (ProjectContext& context, const juce::String& versionID, const SongRead& read)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);
        const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

        if (song == nullptr || version == nullptr)
            return false;

        if (const auto why = whyNotChangeSongNow (context, "nothing was restored."); why.isNotEmpty())
            return VersionHistory::Taken::refused (why);

        if (read.idsGivenAnew)
            return VersionHistory::Taken::refused (idsTwiceRefusal());

        history->restoreMissing = read.missing;
        history->restoreRefused.clear();
        history->tellNow();

        if (! read.song.isValid())
            return false;

        // A change of its own, one undo step, as git restore --source makes the working tree a commit's, the song as it was
        // kept in Local History first. The plug-ins' own settings as they are now, to tell which differ from the commit's:
        // only those are set again.
        keepSongBefore (*history, "restore");
        const UndoStep step (*song, "Restore \"" + getDisplayName (*version) + "\"");
        song->storeDeviceStates();
        makeSongLike (*song, read.song);
        removeBrokenReferences (*song); // what the song kept where a damaged piece was can name what the commit hadn't
        return true;
    }
} // namespace

bool keepsVersions (const ProjectContext& context)
{
    return findHistory (context) != nullptr;
}

bool isReadingVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isReading();
}

Project::VersionsCopier makeVersionsCopier (ProjectContext& context)
{
    return {
        [&context] (std::function<juce::Result (const BackgroundJobs::ShouldStop&)> copy, std::function<void (const juce::Result&)> copied)
        {
            if (auto* history = findHistory (context))
            {
                history->copyFolder (std::move (copy), std::move (copied),
                                     [&context] (const juce::String& versionID) { startBackgroundBounce (context, versionID); });
                return;
            }

            const auto result = copy ([] { return false; });
            juce::MessageManager::callAsync ([copied = std::move (copied), result] { copied (result); });
        },
        [&context]
        {
            if (auto* history = findHistory (context))
                history->endFolderCopy();
        },
    };
}

void waitForVersionsWritten (const ProjectContext& context)
{
    if (const auto* history = findHistory (context))
        history->waitForThread();
}

std::vector<VersionInfo> getVersions (const ProjectContext& context)
{
    std::vector<VersionInfo> infos;

    if (const auto* history = findHistory (context))
        for (const auto* version : history->getVersions())
            infos.push_back (toInfo (*history, *version));

    return infos;
}

std::vector<BranchInfo> getBranches (const ProjectContext& context)
{
    std::vector<BranchInfo> infos;

    if (const auto* history = findHistory (context))
        for (const auto& branch : history->getBranches())
            infos.push_back (toInfo (branch));

    return infos;
}

std::optional<BranchInfo> getCurrentBranch (const ProjectContext& context)
{
    if (const auto* history = findHistory (context))
    {
        if (const auto* branch = history->findCurrentBranch())
            return toInfo (*branch);
    }

    return std::nullopt;
}

std::optional<VersionInfo> getLastVersion (const ProjectContext& context)
{
    if (const auto* history = findHistory (context))
        if (const auto* version = history->findLastVersion())
            return toInfo (*history, *version);

    return std::nullopt;
}

//==============================================================================
// The song tied to its version.
std::optional<VersionInfo> getBaseVersion (const ProjectContext& context)
{
    if (const auto* history = findHistory (context))
        if (const auto* version = history->findBaseVersion())
            return toInfo (*history, *version);

    return std::nullopt;
}

int countNewerVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && ! history->isReading() ? history->countNewerVersions() : 0;
}

juce::String describeNewerVersions (const ProjectContext& context)
{
    auto* history = findHistory (context);
    const auto newer = countNewerVersions (context);
    const auto* branch = history != nullptr ? history->findCurrentBranch() : nullptr;

    if (newer == 0 || branch == nullptr)
        return {};

    // Said plainly, with what to do: one press, which the button beside it is.
    const auto said = "'" + branch->name + "' has " + juce::String (newer) + (newer == 1 ? " newer version" : " newer versions")
                      + " than the song grew from (saved on another computer, or since this project file was saved). ";

    if (isMerging (context))
        return said + "Save a version once the merge waiting has taken them in, or Cancel Merge first.";

    return said
           + (history->mayHaveChanges() ? "Merge the Newer Versions merges them with the song's changes; save a version then to keep both."
                                        : "Take the Newer Versions makes the song the branch's last version.")
           + (history->canSaveOnBranch() ? " A version saved first goes its own way beside them, to merge later."
                                         : " No version can be saved over them until then; Create Branch goes on from the version "
                                           "the song grew from.");
}

bool canTakeNewerVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isBehind() && ! history->isReading() && ! history->isSaving() && ! history->isReadingForCommand()
           && ! history->getMerging().has_value() && ! history->preview.has_value() && canChangeSongNow (context);
}

juce::String describeTakeNewer (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && history->mayHaveChanges() ? "Merge the Newer Versions" : "Take the Newer Versions";
}

namespace
{
    // Take the Newer Versions, once the branch's last version's song is read: the song made that version's, which it grows
    // from then, as one undo step that moves the song's names too, so that its Undo takes both back.
    VersionHistory::Taken takeInNewer (ProjectContext& context, const juce::String& versionID, juce::int64 changesThen, SongRead& read)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);

        if (history == nullptr || song == nullptr)
            return false;

        if (! read.song.isValid())
            return VersionHistory::Taken::refused ("The branch's last version can't be read from the Versions folder: a piece of it is "
                                                   "damaged or missing. Nothing was taken. Verify History says what is damaged.");

        if (const auto why = whyNotChangeSongNow (context, "nothing was taken."); why.isNotEmpty())
            return VersionHistory::Taken::refused (why);

        // Changed meanwhile, the song has changes of its own: they would go. Merge the Newer Versions keeps them.
        if (history->countSongChanges() != changesThen || history->mayHaveChanges())
            return VersionHistory::Taken::refused ("The song changed while the newer versions were read: nothing was taken. Merge "
                                                   "the Newer Versions keeps the song's changes with them.");

        if (! history->takeBaseFrom (versionID, read.song, read.parameters))
            return VersionHistory::Taken::refused ("The branch moved on while its newer versions were read: nothing was taken. "
                                                   "Try once more.");

        // A change of its own, one undo step: the song made that version's, and its names moved with it, in the step.
        const UndoStep step (*song, "Take the Newer Versions");
        song->storeDeviceStates();
        makeSongLike (*song, read.song);
        removeBrokenReferences (*song);
        auto standing = getSongStanding (*song);
        standing.versionsBase = versionID;
        setSongStanding (*song, std::move (standing)); // in the step: its Undo puts the base before it back
        history->tellNow();
        return true;
    }
} // namespace

bool takeNewerVersions (ProjectContext& context)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    const auto* last = history != nullptr ? history->findLastVersion() : nullptr;

    if (! canTakeNewerVersions (context) || last == nullptr)
        return false;

    // With changes of its own (or the branch's line elsewhere), merged in, the song's changes kept.
    if (history->mayHaveChanges() || history->findCommonVersion (last->id, history->getBaseVersionID()) != history->getBaseVersionID())
        return startMerge (context, last->id);

    return startSongRead (context, *history, "Reading \"" + getDisplayName (*last) + "\"", last->song, false,
                          [&context, versionID = last->id, changesThen = history->countSongChanges()] (SongRead& read)
                          { return takeInNewer (context, versionID, changesThen, read); });
}

juce::String getBaseProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->getBaseProblem() : juce::String();
}

bool canSaveOnBranch (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history == nullptr || history->canSaveOnBranch();
}

juce::String getSaveProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->saveProblem : juce::String();
}

namespace
{
    VersionChange toLine (const SongChange& change)
    {
        VersionChange line;
        line.kind = change.kind == SongChange::Kind::added     ? VersionChange::Kind::added
                    : change.kind == SongChange::Kind::removed ? VersionChange::Kind::removed
                                                               : VersionChange::Kind::changed;
        line.isMaster = change.where == SongChange::Where::master;
        line.isSong = change.where == SongChange::Where::song;
        line.track = line.isSong ? TrackID() : TrackID::fromRaw (change.trackID);
        line.name = change.name;
        line.what = change.what.joinIntoString (", ");
        return line;
    }

    SongChange fromLine (const VersionChange& line)
    {
        SongChange change;
        change.kind = line.kind == VersionChange::Kind::added     ? SongChange::Kind::added
                      : line.kind == VersionChange::Kind::removed ? SongChange::Kind::removed
                                                                  : SongChange::Kind::changed;
        change.where = line.isSong ? SongChange::Where::song : line.isMaster ? SongChange::Where::master : SongChange::Where::track;
        change.trackID = line.track.getRaw();
        change.name = line.name;
        change.what = juce::StringArray::fromTokens (line.what, ",", {});
        change.what.trim();
        change.what.removeEmptyStrings();
        return change;
    }

    std::vector<VersionChange> toLines (const std::vector<SongChange>& changes)
    {
        std::vector<VersionChange> lines;

        for (const auto& change : changes)
            lines.push_back (toLine (change));

        return lines;
    }
} // namespace

std::vector<VersionChange> getChangesSinceLastVersion (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr ? toLines (history->getChanges()) : std::vector<VersionChange>();
}

bool hasChangesSinceLastVersion (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && history->hasChanges();
}

int countChanges (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr ? (int) history->getChanges().size() : 0;
}

bool areChangesWorkedOut (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history == nullptr || (! history->isReading() && history->areChangesWorkedOut());
}

bool mayHaveChangesSinceLastVersion (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && history->mayHaveChanges();
}

bool discardAllChanges (ProjectContext& context)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->getLastVersionSong().isValid() || ! history->mayHaveChanges())
        return false;

    // A change of its own, one undo step: the song made the last version's again; Local History keeps what goes.
    keepSongBefore (*history, "discard");
    const UndoStep step (*song, "Discard All Changes");
    song->storeDeviceStates();
    makeSongLike (*song, history->getLastVersionSong());
    removeBrokenReferences (*song);
    return true;
}

namespace
{
    juce::String& getComputerHolder()
    {
        static juce::String computer = juce::Uuid().toString();
        return computer;
    }
} // namespace

void setThisComputer (const juce::String& id)
{
    if (id.isNotEmpty())
        getComputerHolder() = id;
}

juce::String getThisComputer()
{
    return getComputerHolder();
}

juce::String toBranchName (const juce::String& typed)
{
    // As typed, spaces and all: a branch is stored by its id, its name only a value in its record, so a name needs nothing
    // more than what any text field's line holds. Each run of white space (a tab, a line break pasted) one space; no control
    // characters, nor the format characters that change how text around them shows without showing themselves (zero-width
    // ones, bidirectional marks and overrides, the byte order mark); and at most maxBranchName characters.
    const auto isFormat = [] (juce::juce_wchar c)
    {
        return c == 0xad || c == 0x61c || c == 0x180e || (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e)
               || (c >= 0x2060 && c <= 0x206f) || c == 0xfeff;
    };

    juce::String name;
    auto space = false;

    for (const auto c : typed.trim())
    {
        if (name.length() >= maxBranchName)
            break;

        if (juce::CharacterFunctions::isWhitespace (c))
        {
            space = true;
            continue;
        }

        if (c < 32 || (c >= 127 && c < 160) || isFormat (c))
            continue;

        if (space && name.isNotEmpty())
            name << ' ';

        space = false;
        name << juce::String::charToString (c);
    }

    return name.substring (0, maxBranchName).trimEnd();
}

void setVersionDraft (ProjectContext& context, const juce::String& name, const juce::String& note)
{
    if (auto* history = findHistory (context))
    {
        history->draftName = name;
        history->draftNote = note;
    }
}

juce::String getVersionDraftName (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->draftName : juce::String();
}

juce::String getVersionDraftNote (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->draftNote : juce::String();
}

bool saveVersion (ProjectContext& context, std::function<void (juce::Result)> done)
{
    // On Now, never on a version open in its place (docs/design/versions.md: A version open).
    if (! backToNow (context))
    {
        if (done != nullptr)
            done (juce::Result::fail ("Go Back to Now first: a render has the version open."));

        return false;
    }

    auto* history = findHistory (context);

    if (history == nullptr)
    {
        if (done != nullptr)
            done (juce::Result::fail ("This song isn't a project's, so it keeps no versions."));

        return false;
    }

    const auto name = history->draftName, note = history->draftNote;

    if (! history->saveVersion (name, note, std::move (done)))
        return false;

    history->draftName.clear();
    history->draftNote.clear();
    return true;
}

bool isSavingVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isSaving(); // a commit, or a first save's copy (isSavingProjectFolder())
}

bool isSavingProjectFolder (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isCopyingFolder();
}

juce::int64 getVersionsSize (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->getSize() : 0;
}

bool canSaveVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    // Not on top of a last commit that can't be read while one that reads is offered (Reset Branch to Last Readable Commit),
    // nor over versions of the branch the song didn't grow from (Take the Newer Versions first).
    return history != nullptr && ! history->isReading() && ! history->isSaving() && ! history->canResetToReadable()
           && history->canSaveOnBranch() && hasSomethingToCommit (context);
}

bool hasSomethingToCommit (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && ! history->isReading() && (history->getMerging().has_value() || history->mayHaveChanges());
}

juce::String describeNextCommit (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && hasSomethingToCommit (context) ? history->summariseNextCommit() : juce::String();
}

namespace
{
    // The song alone again, without a bounce beside it.
    void stopComparing (ProjectContext&, VersionHistory&);

    // After commits left the graph (a branch deleted, a commit undone): a bounce of one of them heard no more.
    void stopHearingWhatWent (ProjectContext& context, VersionHistory& history)
    {
        if (history.bounceCompared.isNotEmpty() && history.findVersion (history.bounceCompared) == nullptr)
            stopComparing (context, history);
    }
} // namespace

bool editVersionMessage (ProjectContext& context, const juce::String& versionID, const juce::String& name, const juce::String& note)
{
    auto* history = findHistory (context);
    return history != nullptr && history->editMessage (versionID, name, note);
}

bool canEditSelectedMessage (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && ! history->isReading() && history->findVersion (history->selectedVersion) != nullptr;
}

bool canUndoLastCommit (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && ! history->isReadingForCommand() && history->canUndoLastCommit();
}

bool undoLastCommit (ProjectContext& context)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    const auto* last = history != nullptr ? history->findLastVersion() : nullptr;
    const auto* before = last != nullptr && canUndoLastCommit (context) ? history->findVersion (last->parents[0]) : nullptr;

    if (before == nullptr)
        return false;

    // The commit before's song, which the changes are from then, read on the history's thread.
    return startSongRead (
        context, *history, "Reading \"" + getDisplayName (*before) + "\"", before->song, false,
        [&context, undone = last->id] (SongRead& read) -> VersionHistory::Taken
        {
            auto* reading = findHistory (context);

            if (! read.song.isValid())
                return VersionHistory::Taken::refused (
                    "The version before can't be read from the Versions folder: a piece of it is damaged or missing. Verify History says what is damaged.");

            const auto* lastNow = reading != nullptr ? reading->findLastVersion() : nullptr;

            if (lastNow == nullptr || lastNow->id != undone
                || ! reading->undoLastCommit (std::move (read.song), std::move (read.parameters)))
                return false;

            stopHearingWhatWent (context, *reading);
            return true;
        });
}

void saveProjectAndVersion (ProjectContext& context, const SaveProjectThen& saveProjectThen, TrackRenderHost& renders)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return;

    // The project first, which its first save puts where the user says (its versions go beside it there), then the version,
    // and then, with a bounce, the version's bounce, which the song is.
    saveProjectThen (
        [&context, &renders] (bool saved)
        {
            if (! saved)
                return;

            saveVersion (context,
                         [&context, &renders] (juce::Result result)
                         {
                             if (const auto last = getLastVersion (context);
                                 result.wasOk() && last.has_value() && getBounceWithVersion (context))
                                 bounceVersion (context, last->id, renders);
                         });
        });
}

bool bringBackVersion (ProjectContext& context, const juce::String& versionID)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

    if (findSong (context) == nullptr || version == nullptr)
        return false;

    // As much of it as can be read, on the history's thread: what a damaged piece held stays as it is now, and is told
    // (getRestoreProblem()).
    return startSongRead (context, *history, "Reading \"" + getDisplayName (*version) + "\"", version->song, true,
                          [&context, versionID] (SongRead& read) { return takeInBringBack (context, versionID, read); });
}

bool compareWithVersion (ProjectContext& context, const juce::String& versionID)
{
    if (versionID.isNotEmpty())
        backToNow (context); // compared with Now

    auto* history = findHistory (context);

    if (history == nullptr)
        return false;

    // The last version is the changes' own, compared with at once; another is read on the history's thread first.
    const auto* last = history->findBaseVersion();
    const auto* version = history->findVersion (versionID);

    if (version == nullptr || (last != nullptr && last->id == versionID))
    {
        history->compareWith ({});
        return true;
    }

    if (versionID == history->getComparedVersion())
        return true;

    return startSongRead (context, *history, "Reading \"" + getDisplayName (*version) + "\"", version->song, false,
                          [&context, versionID] (SongRead& read) -> VersionHistory::Taken
                          {
                              auto* reading = findHistory (context);

                              if (reading == nullptr)
                                  return false;

                              reading->compareWith (versionID, read.song, std::move (read.parameters));

                              if (! read.song.isValid())
                                  return VersionHistory::Taken::refused (
                                      "That version can't be read from the Versions folder: a piece of it is "
                                      "damaged or missing. Verify History says what is damaged.");

                              return true;
                          });
}

juce::String getComparedVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->getComparedVersion() : juce::String();
}

std::vector<VersionChange> getListedChanges (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr ? toLines (history->getListedChanges()) : std::vector<VersionChange>();
}

bool takeBack (ProjectContext& context, const VersionChange& line)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->getComparedSong().isValid())
        return false;

    const auto then = history->getComparedSong();
    const auto* compared = history->findVersion (history->getComparedVersion());

    // Not from a version numbered otherwise (numberedOtherwise()), as the last version itself is until one is saved after the
    // song's IDs were given anew as it opened.
    if (! areNumberedAlike (*song, then))
    {
        history->restoreRefused = compared != nullptr
                                      ? otherNumberingRefusal ("\"" + getDisplayName (*compared) + "\"")
                                      : "Nothing was discarded: " + numberedOtherwise ("the last version")
                                            + " Discard All Changes makes the whole song the last version again; or save a version "
                                              "first, and a line's changes can be discarded from then on.";
        history->tellNow();
        return false;
    }

    if (history->restoreRefused.isNotEmpty())
    {
        history->restoreRefused.clear();
        history->tellNow();
    }

    // Not where it would give a track an ID the song holds now for something else, which it can't take: nothing written.
    if (! line.isSong && givesTrackIDsHeldElsewhere (*song, line.isMaster ? getMasterID (*song) : line.track, then))
    {
        history->restoreRefused =
            compared != nullptr ? idsElsewhereRefusal ("Couldn't restore " + line.name + " from \"" + getDisplayName (*compared) + "\"",
                                                       "Restore This Version takes the whole version.")
                                : idsElsewhereRefusal ("Couldn't discard the changes in " + line.name,
                                                       "Discard All Changes makes the whole song the last version again.");
        history->tellNow();
        return false;
    }

    // A change of its own, one undo step: Discard Changes, from the last version; Restore from another version.
    const UndoStep step (*song, compared != nullptr ? "Restore " + line.name + " from \"" + getDisplayName (*compared) + "\""
                                                    : "Discard Changes in " + line.name);
    song->storeDeviceStates();
    auto done = true;

    if (line.isSong)
        makeSongOwnLike (*song, then);
    else
        done = makeTrackLike (*song, line.isMaster ? getMasterID (*song) : line.track, then); // the master's by its kind

    removeBrokenReferences (*song);
    history->selectedChange.reset();
    return done;
}

std::vector<VersionChange> getVersionChanges (const ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    return history != nullptr ? toLines (history->getVersionChanges (versionID)) : std::vector<VersionChange>();
}

bool isReadingVersionChanges (const ProjectContext& context, const juce::String& versionID)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isReadingVersionChanges (versionID);
}

namespace
{
    // Restore from This Commit of a line, once the commit's song is read.
    VersionHistory::Taken takeInRestoreLine (ProjectContext& context, const juce::String& versionID, const VersionChange& line,
                                             const juce::ValueTree& then, const std::vector<MissingPiece>& missing, bool idsGivenAnew)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);
        const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

        if (song == nullptr || version == nullptr)
            return false;

        if (const auto why = whyNotChangeSongNow (context, "nothing was restored."); why.isNotEmpty())
            return VersionHistory::Taken::refused (why);

        if (idsGivenAnew)
            return VersionHistory::Taken::refused (idsTwiceRefusal());

        if (then.isValid() && ! areNumberedAlike (*song, then))
            return VersionHistory::Taken::refused (otherNumberingRefusal ("\"" + getDisplayName (*version) + "\""));

        // As much of it as can be read, as the whole song's restore takes it: what a damaged piece held stays as it is now, and
        // what of the line's that is, is told (getRestoreProblem()).
        const auto onLine = [&line] (const MissingPiece& piece)
        {
            if (piece.path.size() < 2)
                return true; // the whole song, or its own element

            if (line.isSong)
            {
                for (const auto* type : { &schema::TEMPOMAP, &schema::SECTIONS, &schema::CHORDLANE, &schema::LASTKEY })
                    if (piece.path[1].startsWith (type->toString() + ":"))
                        return true;

                return false;
            }

            // The master's line the master's piece, by its kind, whatever ID the version gave it (a version may give the master
            // another).
            for (const auto& step : piece.path)
                if (line.isMaster ? step.startsWith (schema::MASTER.toString() + ":")
                                  : step == schema::TRACK.toString() + ":" + juce::String ((juce::uint64) line.track.getRaw()))
                    return true;

            return false;
        };

        history->restoreMissing.clear();
        history->restoreRefused.clear();
        std::copy_if (missing.begin(), missing.end(), std::back_inserter (history->restoreMissing), onLine);
        history->tellNow();

        if (! then.isValid())
            return false;

        // A track the commit holds nowhere it can read, where the piece of a track that could hold it can't be read: whether it
        // was there then can't be told, so nothing is taken, rather than the track taken away as added since. One could hold it
        // that is a folder it is in now (the nearest named), a folder now, or a track the song hasn't now, whose kind is unknown.
        if (! line.isSong)
        {
            juce::ValueTree there;
            std::vector<juce::ValueTree> unreadable;

            for (const auto& track : getTrackElements (then))
            {
                if (readID (track[schema::trackID]) == line.track.getRaw())
                    there = track;
                else if (track.hasProperty (schema::versions::missing))
                    unreadable.push_back (track);
            }

            std::vector<std::uint64_t> folders; // the line's track's, nearest first
            for (auto folder = getParentFolder (*song, line.track); folder.isValid(); folder = getParentFolder (*song, folder))
                folders.push_back (folder.getRaw());

            juce::ValueTree holder;

            for (const auto& track : unreadable)
            {
                const auto id = readID (track[schema::trackID]);
                const auto place = std::find (folders.begin(), folders.end(), id);
                const auto couldHold =
                    place != folders.end() || ! hasTrack (*song, TrackID::fromRaw (id)) || isFolder (*song, TrackID::fromRaw (id));

                if (couldHold
                    && (! holder.isValid() || place < std::find (folders.begin(), folders.end(), readID (holder[schema::trackID]))))
                    holder = track;
            }

            // The master's line by its kind, never by its ID, which the version may give it another of: the master is in no
            // track, so no unreadable piece can hold it.
            if (! there.isValid() && ! line.isMaster && holder.isValid())
            {
                const auto id = TrackID::fromRaw (readID (holder[schema::trackID]));
                const auto name = hasTrack (*song, id) ? "\"" + getTrackName (*song, id) + "\"" : juce::String ("a track gone since");
                history->restoreMissing.clear();
                history->restoreRefused = "Couldn't restore " + line.name + " from \"" + getDisplayName (*version)
                                          + "\": the version's piece of " + name + ", which may hold it, can't be read.";
                history->tellNow();
                return false;
            }
        }

        // Not where it would give a track an ID the song holds now for something else, which it can't take: nothing written.
        if (! line.isSong && givesTrackIDsHeldElsewhere (*song, line.isMaster ? getMasterID (*song) : line.track, then))
        {
            history->restoreMissing.clear();
            history->restoreRefused = idsElsewhereRefusal ("Couldn't restore " + line.name + " from \"" + getDisplayName (*version) + "\"",
                                                           "Restore This Version takes the whole version.");
            history->tellNow();
            return false;
        }

        // A change of its own, one undo step, as git restore --source <commit> <file> makes one file a commit's.
        const UndoStep step (*song, "Restore " + line.name + " from \"" + getDisplayName (*version) + "\"");
        song->storeDeviceStates();
        auto done = true;

        if (line.isSong)
            makeSongOwnLike (*song, then);
        else
            done = makeTrackLike (*song, line.isMaster ? getMasterID (*song) : line.track, then); // the master's by its kind

        removeBrokenReferences (*song); // with the curves of what the song hasn't, one it brought in from a damaged piece too
        return done;
    }
} // namespace

bool restoreFromVersion (ProjectContext& context, const juce::String& versionID, const VersionChange& line)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

    if (findSong (context) == nullptr || version == nullptr)
        return false;

    // Read on the history's thread first.
    return startSongRead (context, *history, "Reading \"" + getDisplayName (*version) + "\"", version->song, true,
                          [&context, versionID, line] (SongRead& read)
                          { return takeInRestoreLine (context, versionID, line, read.song, read.missing, read.idsGivenAnew); });
}

void selectVersion (ProjectContext& context, const juce::String& versionID)
{
    if (auto* history = findHistory (context))
    {
        history->selectedVersion = versionID;
        history->selectedVersionChange.reset();
        history->selectedSnapshot = 0; // one row of the history is selected: a version (or Now), or an automatic save
    }
}

void selectVersionChange (ProjectContext& context, const juce::String& versionID, std::optional<VersionChange> line)
{
    if (auto* history = findHistory (context))
    {
        history->selectedVersion = versionID;
        history->selectedVersionChange = line.has_value() ? std::optional<SongChange> (fromLine (*line)) : std::nullopt;
        history->selectedSnapshot = 0;
    }
}

std::optional<VersionChange> getSelectedVersionChange (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->selectedVersionChange.has_value()
               ? std::optional<VersionChange> (toLine (*history->selectedVersionChange))
               : std::nullopt;
}

juce::String describeRestoreChange (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (history->selectedVersion) : nullptr;
    const auto line = getSelectedVersionChange (context);
    return version != nullptr && line.has_value() ? "Restore " + line->name + " from \"" + getDisplayName (*version) + "\""
                                                  : juce::String();
}

bool canRestoreSelectedChange (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->findVersion (history->selectedVersion) != nullptr && history->selectedVersionChange.has_value();
}

void restoreSelectedChange (ProjectContext& context)
{
    if (const auto line = getSelectedVersionChange (context))
        restoreFromVersion (context, getSelectedVersion (context), *line);
}

juce::String getSelectedVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->selectedVersion : juce::String();
}

void selectChange (ProjectContext& context, std::optional<VersionChange> line)
{
    if (auto* history = findHistory (context))
    {
        history->selectedChange = line.has_value() ? std::optional<SongChange> (fromLine (*line)) : std::nullopt;
    }
}

std::optional<VersionChange> getSelectedChange (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->selectedChange.has_value() ? std::optional<VersionChange> (toLine (*history->selectedChange))
                                                                     : std::nullopt;
}

juce::String describeBringBack (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (history->selectedVersion) : nullptr;
    return version != nullptr ? "Restore \"" + getDisplayName (*version) + "\"" : juce::String();
}

juce::String describePutBack (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* compared = history != nullptr ? history->findVersion (history->getComparedVersion()) : nullptr;
    return compared != nullptr ? "Restore from \"" + getDisplayName (*compared) + "\"" : juce::String();
}

bool canBringBack (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->findVersion (history->selectedVersion) != nullptr;
}

bool canTakeFrom (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* last = history != nullptr ? history->findBaseVersion() : nullptr;
    return canBringBack (context) && (last == nullptr || last->id != history->selectedVersion);
}

bool canPutBack (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->selectedChange.has_value();
}

void takeFromSelectedVersion (ProjectContext& context)
{
    compareWithVersion (context, getSelectedVersion (context));
}

void bringBackSelectedVersion (ProjectContext& context)
{
    bringBackVersion (context, getSelectedVersion (context));
}

void putBackSelectedChange (ProjectContext& context)
{
    if (const auto line = getSelectedChange (context))
        takeBack (context, *line);
}

//==============================================================================
bool newBranch (ProjectContext& context, const juce::String& fromVersion, const SaveProjectThen& saveProjectThen)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);

    if (history == nullptr)
        return false;

    // From Now, the song goes on there as it is; from a version, the song becomes that version's there, a switch.
    if (fromVersion.isEmpty())
        return history->makeBranch ({}, true).isNotEmpty();

    if (! canSwitchBranch (context))
        return false;

    // The branch made first, and taken away again if the switch to it doesn't finish (cancelled, or the commit can't be read),
    // so that nothing is left of a Create Branch that did nothing.
    const auto branch = history->makeBranch (fromVersion, false);
    const auto forget = [&context, branch] (bool switched)
    {
        // Once a first save's copy of the folder is over, if one is under way: nothing is written into it meanwhile.
        if (auto* made = findHistory (context); made != nullptr && ! switched)
            made->whenNotCopying (
                [&context, branch]
                {
                    if (auto* found = findHistory (context))
                        found->deleteBranch (branch, true);
                });
    };

    if (branch.isEmpty())
        return false;

    if (switchBranch (context, branch, saveProjectThen, forget))
        return true;

    forget (false);
    return false;
}

bool canSwitchBranch (const ProjectContext& context)
{
    // Not while the song records (a take not finished would be lost), while a render has its plug-ins, while a version is
    // being saved or the versions read, nor while a command reads them to change the song. Nor while the song's branch isn't
    // in the folder: the song would be left on the branch the folder names, as its song, and come back as that branch's
    // (whyNotSwitchBranch()).
    const auto* history = findHistory (context);
    return canChangeSongNow (context) && ! history->isReadingForCommand() && ! history->isBranchMissing();
}

juce::String whyNotSwitchBranch (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isBranchMissing()
               ? juce::String ("Create Branch first: the song's branch isn't in this folder, so a switch, a merge, Undo Last Version "
                               "or a reset would leave the song on, or move, another branch.")
               : juce::String();
}

// What takes a switch's reads in: the song made the other's in place (switchSongInPlace()), as one; unless the song
// changed while the other was read, which sends the song left round once more on the history's thread from a new copy,
// twice at most (rounds), after which the hold keeps the user's edits out and what a plug-in or the automation changed
// since is split as the switch ends (VersionHistory::finishSwitch()).
VersionHistory::TakeIn takeInSwitch (ProjectContext& context, VersionHistory::Switch plan, std::shared_ptr<VersionHistory::SwitchRead> read,
                                     int rounds)
{
    return [&context, plan, read, rounds]() mutable -> VersionHistory::Taken
    {
        auto* reading = findHistory (context);
        const auto nothing = juce::String ("nothing was switched, the song and the branch as they were.");

        if (reading == nullptr)
            return false;

        if (const auto why = whyNotChangeSongNow (context, nothing); why.isNotEmpty())
            return VersionHistory::Taken::refused (why);

        if (rounds > 0 && reading->copyLeftAgain (plan))
            return VersionHistory::Taken::readAgain (
                [&context, plan, read, rounds, store = reading->getStore()]() -> VersionHistory::TakeIn
                {
                    VersionHistory::keepLeftIn (*store, plan, *read);
                    store->getPieces().closeFiles();
                    return takeInSwitch (context, plan, read, rounds - 1);
                });

        VersionHistory::Taken taken (false);
        switchSongInPlace (context,
                           [reading, &plan, &read, &taken]
                           {
                               taken = reading->finishSwitch (plan, std::move (*read));
                               return taken.did;
                           });
        return taken;
    };
}

namespace
{
    // A switch to another branch or a commit, read on the history's thread (VersionHistory.h: Switch), then made in place,
    // as one; the project saved after it, so that its file is the song the versions say it is. Then done.
    bool startSwitch (ProjectContext& context, VersionHistory& history, std::optional<VersionHistory::Switch> plan,
                      const juce::String& title, const SaveProjectThen& saveProjectThen, std::function<void (bool)> done = {})
    {
        if (! plan.has_value())
            return false;

        return startCommandRead (
            context, history, title,
            [&context, plan = *plan, store = history.getStore()]() -> VersionHistory::TakeIn
            {
                auto read = std::make_shared<VersionHistory::SwitchRead> (VersionHistory::readSwitch (*store, plan));
                return takeInSwitch (context, plan, read, 2);
            },
            [&context, saveProjectThen, done] (bool did)
            {
                // The song left on the branch entered let go of once the project file holds the song, not before: a save
                // that fails leaves both branches' songs as they were.
                if (did)
                    saveProjectThen (
                        [&context] (bool saved)
                        {
                            if (auto* now = findHistory (context); now != nullptr && saved)
                                now->switchSaved();
                        });

                if (done != nullptr)
                    done (did);
            });
    }

    juce::String describeBranchRead (const VersionHistory& history, const juce::String& branchID)
    {
        for (const auto& branch : history.getBranches())
            if (branch.id == branchID)
                return "Reading branch '" + branch.name + "'";

        return "Reading the branch";
    }
} // namespace

bool switchBranch (ProjectContext& context, const juce::String& branchID, const SaveProjectThen& saveProjectThen,
                   std::function<void (bool switched)> done)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    return history != nullptr && canSwitchBranch (context)
           && startSwitch (context, *history, history->prepareSwitch (branchID), describeBranchRead (*history, branchID), saveProjectThen,
                           std::move (done));
}

bool renameBranch (ProjectContext& context, const juce::String& branchID, const juce::String& name)
{
    auto* history = findHistory (context);
    return history != nullptr && history->renameBranch (branchID, name);
}

bool canRenameBranch (const ProjectContext& context)
{
    const auto current = getCurrentBranch (context);
    return current.has_value() && ! isSavingVersion (context); // a version writes its record meanwhile
}

bool canDeleteBranch (const ProjectContext& context, const juce::String& branchID)
{
    const auto* history = findHistory (context);

    if (history == nullptr || history->isReading() || history->isSaving())
        return false;

    const auto current = getCurrentBranch (context);
    const auto branches = history->getBranches();
    const auto found = std::find_if (branches.begin(), branches.end(), [&branchID] (const auto& branch) { return branch.id == branchID; });

    if (found == branches.end() || (current.has_value() && current->id == branchID))
        return false;

    // Not while a merge of its line shows, nor while a merge waiting (here, or on another branch) took in a commit that only
    // its line holds, which would leave the graph with it.
    // What Delete Branch's question will say of the changes left on it, found out on the history's thread now, as the picker
    // lists it, so that it is known by the time it is asked.
    findHistory (context)->hasChangesLeftOn (branchID);

    return ! (history->preview.has_value() && history->preview->version == found->head) && ! history->holdsCommitMergeWaits (*found);
}

bool canDeleteAnyBranch (const ProjectContext& context)
{
    const auto branches = getBranches (context);
    return std::any_of (branches.begin(), branches.end(),
                        [&context] (const BranchInfo& branch) { return canDeleteBranch (context, branch.id); });
}

juce::String describeBranchDeletion (const ProjectContext& context, const juce::String& branchID)
{
    auto* history = findHistory (context);

    if (history == nullptr)
        return {};

    // What goes with it: its own versions and the changes left on it, which Recover Lost Versions brings back for 30 days.
    juce::StringArray parts;
    const auto only = history->countVersionsOnlyOn (branchID);

    if (only > 0)
        parts.add (juce::String (only) + (only == 1 ? " version" : " versions") + " that no other branch has");

    // Whether it has any is found out on the history's thread the first time it is asked for (canDeleteBranch() asks, as the
    // picker lists it): until it is known, said as maybe.
    using LeftSong = VersionHistory::LeftSong;

    if (const auto left = history->hasChangesLeftOn (branchID); ! left.has_value())
        parts.add ("the changes left on it, if it has any");
    else if (*left == LeftSong::changed)
        parts.add ("the changes left on it");
    else if (*left == LeftSong::unreadable)
        parts.add ("what was left on it, which can't be read from the Versions folder to tell what it holds");

    if (parts.isEmpty())
        return {};

    return "This also deletes " + parts.joinIntoString (", and ") + ". Recover Lost Versions brings "
           + (parts.size() == 1 && only == 1 ? "it" : "them") + " back for 30 days.";
}

bool deleteBranch (ProjectContext& context, const juce::String& branchID)
{
    auto* history = findHistory (context);

    if (history == nullptr || ! canDeleteBranch (context, branchID) || ! history->deleteBranch (branchID))
        return false;

    stopHearingWhatWent (context, *history);
    return true;
}

bool canMakeBranch (const ProjectContext& context)
{
    // From Now, while there is a version; from another version, whenever the song can switch to it. While the song's branch
    // isn't in the folder, from Now whatever is selected (newBranchFromSelection()): the way out, as the picker's.
    const auto* history = findHistory (context);

    if (history == nullptr || history->isReading() || history->isSaving())
        return false;

    return history->findVersion (history->selectedVersion) != nullptr && ! history->isBranchMissing()
               ? canSwitchBranch (context)
               : history->findBaseVersion() != nullptr;
}

void selectBranch (ProjectContext& context, const juce::String& branchID)
{
    if (auto* history = findHistory (context))
        history->selectedBranch = branchID;
}

juce::String newBranchNamed (ProjectContext& context, const juce::String& name)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return {};

    auto* history = findHistory (context);

    // From Now, whatever version is selected: while there is a version the song grew from.
    if (history == nullptr || history->isReading() || history->isSaving() || history->findBaseVersion() == nullptr)
        return {};

    const auto made = history->makeBranch ({}, true);

    if (made.isNotEmpty() && name.trim().isNotEmpty())
        history->renameBranch (made, name);

    return made;
}

void newBranchFromSelection (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    // From Now while the song's branch isn't in the folder: the way out (canMakeBranch()).
    const auto* history = findHistory (context);
    newBranch (context, history != nullptr && history->isBranchMissing() ? juce::String() : getSelectedVersion (context), saveProjectThen);
}

void switchToSelectedBranch (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    if (const auto* history = findHistory (context))
        switchBranch (context, history->selectedBranch, saveProjectThen);
}

//==============================================================================
namespace
{

    bool isOnLine (const MergeClash& clash, const SongChange& line)
    {
        return line.where == SongChange::Where::song ? clash.trackID == 0 : clash.trackID == line.trackID;
    }

    // The song's element of a merge waiting set anew (Schema.h: MERGING), in the caller's undo step, so that Undo and Redo
    // take it out and back with what the step changed in the song, and every file the song is saved into holds it as the
    // song is: none for none.
    void setMergingElement (Song& song, const juce::ValueTree& element)
    {
        // The song's own value, its record read as the element holds it (SongOwnValue.h: setSongOwn()), in the step; whole
        // state writes the MERGING after the root's other children (SongOwnArea.h: writeSongOwnIn()).
        jassert (! song.isWritingOutsideHistory());
        auto own = getSongOwn (song);
        own.merging = element.isValid() ? std::optional (readMergingRecord (element)) : std::nullopt;
        setSongOwn (song, std::move (own));
    }
} // namespace

bool canMerge (const ProjectContext& context)
{
    const auto* history = findHistory (context);

    // Not while a merge waits for its version, as git refuses one: save its version or cancel it first. Nor while the song's
    // branch isn't in the folder, where its version couldn't be saved (whyNotSwitchBranch()).
    if (history == nullptr || history->isReading() || history->isSaving() || history->getMerging().has_value()
        || history->isBranchMissing())
        return false;

    const auto* last = history->findBaseVersion();
    const auto& selected = history->selectedVersion;

    // Not the current branch's last version, nor one it follows: what it has already.
    return last != nullptr && history->findVersion (selected) != nullptr && history->findCommonVersion (last->id, selected) != selected;
}

juce::String describeMergeInto (const ProjectContext& context)
{
    // Not into the branch the folder names while the song isn't on it (canMerge()).
    if (const auto* history = findHistory (context); history != nullptr && history->isBranchMissing())
        return "Merge into Current Branch (Create Branch First)...";

    const auto branch = getCurrentBranch (context);
    return branch.has_value() ? "Merge into '" + branch->name + "'..." : juce::String ("Merge into Current Branch...");
}

namespace
{
    // What the start of a merge takes on the message thread before it reads: the commits, what earlier merges of that line
    // gave anew, the song as it is (copied, its plug-ins' states in it as they gave them last: version.merge's gate asked
    // them, known bug 166) with its plug-ins' knobs, and how often the song had changed then.
    struct MergeStart
    {
        juce::String version, head, otherBranch, basePiece, theirsPiece;
        // This line's merges to translate by (those whose version the parting follows); every one of each line's; the other
        // line's whose version the parting follows.
        std::vector<RenumberedID> renumbering, ourMerges, theirMerges, theirMergesAtParting;
        SongState ours {}; // the song as it was taken for the merge: its tree written where the merge is worked out
        PluginParameterValues ourParameters;
        juce::int64 changesAtCopy = 0;
    };

    // The merge's list worked out from the two songs read (the newest both lines have, and the other's), as they were read,
    // against the song as it is (ours): numbered as this line, where earlier merges of that line gave what it added IDs anew,
    // its plug-ins' knobs too, an item that line made since with an ID such a merge gave to another of theirs given one past
    // the song's (next, which the song's count is moved past as it is taken in); what it changed, by line; and what of it
    // clashes with what the song changed: a merge of every line, tried on copies. Any thread.
    struct MergeWorked
    {
        VersionHistory::Merge preview;
        juce::ValueTree baseRead, theirsRead; // as read, to work it out again against the song if it changed meanwhile
        PluginParameterValues theirParametersRead;
        std::uint64_t next = 0;
        bool idsGivenAnew = false;      // as either song was read: then it can't be merged
        bool numberedOtherwise = false; // either song than the song (Schema.h: idNumbering): then it can't either
        bool heldTwice = false;         // an ID either song holds twice once translated, which would make two items one: refused
    };

    void workOutMerge (MergeWorked& worked, const MergeStart& start, const juce::ValueTree& ours,
                       const PluginParameterValues& ourParameters)
    {
        auto& preview = worked.preview;
        preview.base = worked.baseRead.createCopy();
        preview.theirs = worked.theirsRead.createCopy();
        preview.theirParameters = worked.theirParametersRead;
        const auto floor = juce::jmax (findNextID (preview.base), findNextID (preview.theirs));
        worked.next = juce::jmax (findNextID (ours), floor);
        const auto makeID = [&worked] { return worked.next++; };

        // The other line's own merges of what this line holds too, taken into the parting's numbering first (crossRenumbering()),
        // then both songs into this line's: one numbering a stage.
        const auto crossed = crossRenumbering (start.theirMerges, start.ourMerges, start.theirMergesAtParting, start.renumbering,
                                               preview.base, preview.theirs, makeID);
        translateIDs (preview.theirs, crossed);
        preview.theirParameters = translateKnobs (std::move (preview.theirParameters), crossed);

        const auto translation = findTranslation (preview.base, preview.theirs, start.renumbering, makeID, crossed);
        translateIDs (preview.base, translation.all);
        translateIDs (preview.theirs, translation.all);
        preview.theirParameters = translateKnobs (std::move (preview.theirParameters), translation.all);
        preview.translation = translation.all;
        preview.crossed = crossed;
        preview.renumbered = recordCrossings (crossed, translation.all); // by the IDs the merged song holds
        preview.renumbered.insert (preview.renumbered.end(), translation.givenAnew.begin(), translation.givenAnew.end());

        // Never two items under one ID, which the merge would make one (records another history wrote, or a history these
        // translations don't cover): nothing merged.
        worked.heldTwice = ! holdsEachIDOnce (preview.base) || ! holdsEachIDOnce (preview.theirs);

        if (worked.heldTwice)
            return;

        preview.lines = findChanges (preview.base, preview.theirs);

        auto next = juce::jmax (worked.next, findNextID (ours), findNextID (preview.theirs));
        preview.clashes = mergeSongs (preview.base, ours, preview.theirs, preview.lines, [&next] { return next++; }).clashes;
        nameKnobs (preview.clashes, ours, preview.theirs, ourParameters, preview.theirParameters);
    }

    // The song as it was copied for the merge's list, kept in the store as Merge will need it for Cancel Merge (VersionHistory.h:
    // keepMerge()), here on the history's thread (known bug 126: Merge split and wrote the whole song on the message thread):
    // its piece, which Merge takes while the song is still as it was copied; none if it couldn't be written.
    void keepSongBeforeMerge (VersionStore& store, MergeWorked& worked, const MergeStart& start, const juce::ValueTree& ours)
    {
        const auto pieces = splitSong (ours);
        const auto kept = store.writeSong (pieces);
        store.getPieces().closeFiles();
        worked.preview.before = ours;
        worked.preview.beforePiece = kept ? pieces.song : juce::String();
        worked.preview.beforeChanges = start.changesAtCopy;
    }

    MergeWorked readMerge (VersionStore& store, const MergeStart& start)
    {
        const auto ours = writeSongTree (start.ours); // the song whole, written here, on the history's thread, from its state
        MergeWorked worked;
        worked.preview.version = start.version;
        worked.preview.otherBranch = start.otherBranch;
        auto baseRenumbered = false, theirsRenumbered = false;
        worked.baseRead = start.basePiece.isNotEmpty() ? readCommitSong (store, start.basePiece, nullptr, nullptr, &baseRenumbered)
                                                       : juce::ValueTree (schema::PHAZITE);
        worked.theirsRead = readCommitSong (store, start.theirsPiece, &worked.theirParametersRead, nullptr, &theirsRenumbered);
        store.getPieces().closeFiles();

        // A song that held an ID twice (which no Phazite writes) was given IDs anew as it was read, each side on its own, so
        // that what the two lines share no longer has the same IDs: nothing a merge could make of them would be right.
        worked.idsGivenAnew = baseRenumbered || theirsRenumbered;

        // The other line's song or the parting's (none for two of no common line) numbered otherwise than the song (Schema.h:
        // idNumbering), one of them from before the song's IDs were given anew as it opened: its items would be matched with
        // others of the song's.
        worked.numberedOtherwise = worked.baseRead.isValid() && worked.theirsRead.isValid()
                                   && (! areNumberedAlike (ours, worked.theirsRead)
                                       || (start.basePiece.isNotEmpty() && ! areNumberedAlike (ours, worked.baseRead)));

        if (worked.baseRead.isValid() && worked.theirsRead.isValid() && ! worked.idsGivenAnew && ! worked.numberedOtherwise)
        {
            workOutMerge (worked, start, ours, start.ourParameters);

            if (! worked.heldTwice)
                keepSongBeforeMerge (store, worked, start, ours);
        }

        return worked;
    }

    // The merge's list shown, once what it read is in, with the song's count of IDs past those it gave anew; unless the song
    // changed meanwhile (a plug-in's own change, the automation written as it plays), which sends the list round once more on
    // the history's thread against a new copy, twice at most (rounds), after which the hold keeps the user's edits out and
    // what changed since is worked in here.
    VersionHistory::TakeIn takeInMerge (ProjectContext& context, MergeStart start, std::shared_ptr<MergeWorked> worked, int rounds)
    {
        return [&context, start, worked, rounds]() mutable -> VersionHistory::Taken
        {
            auto* history = findHistory (context);
            auto* song = findSong (context);
            const auto* last = history != nullptr ? history->findBaseVersion() : nullptr;
            const auto* version = history != nullptr ? history->findVersion (start.version) : nullptr;
            const auto name = version != nullptr ? "\"" + getDisplayName (*version) + "\"" : juce::String ("that version");

            if (song == nullptr || history == nullptr)
                return false;

            if (const auto why = whyNotChangeSongNow (context, "nothing was merged."); song->isShowingOther())
                return VersionHistory::Taken::refused (why);

            if (! worked->baseRead.isValid() || ! worked->theirsRead.isValid())
                return VersionHistory::Taken::refused ("Nothing was merged: " + name
                                                       + " or the version both lines have can't be read from the Versions folder. "
                                                         "Verify History says what is damaged.");

            if (worked->idsGivenAnew)
                return VersionHistory::Taken::refused ("Nothing was merged: " + name
                                                       + " or the version both lines have holds an ID twice, or IDs too large to count "
                                                         "on from, which no Phazite writes (another tool wrote it), so what the two "
                                                         "lines share can't be told apart.");

            // Numbered otherwise than the song as it was read against, or than the song now, made another whole meanwhile.
            if (worked->numberedOtherwise || getSongOwn (*song).idNumbering != start.ours.songOwn.idNumbering)
                return VersionHistory::Taken::refused (mergeNumberingRefusal (*history, start.version));

            if (last == nullptr || last->id != start.head || version == nullptr || history->getMerging().has_value()
                || history->preview.has_value())
                return VersionHistory::Taken::refused ("Nothing was merged: the branch moved on while " + name + " was read. Merge again.");

            if (history->countSongChanges() != start.changesAtCopy)
            {
                song->storeDeviceStates();
                start.ours = getSongState (*song);
                start.ourParameters = history->readPluginParameters();
                start.changesAtCopy = history->countSongChanges();

                if (rounds > 0)
                    return VersionHistory::Taken::readAgain (
                        [&context, start, worked, rounds, store = history->getStore()]() -> VersionHistory::TakeIn
                        {
                            const auto ours = writeSongTree (start.ours); // on the history's thread, from its state
                            workOutMerge (*worked, start, ours, start.ourParameters);

                            if (! worked->heldTwice)
                                keepSongBeforeMerge (*store, *worked, start, ours);
                            return takeInMerge (context, start, worked, rounds - 1);
                        });

                // Here, on the message thread, its tree written here: the hold keeps the user's edits out after two rounds
                // (rare).
                workOutMerge (*worked, start, writeSongTree (start.ours), start.ourParameters);
                worked->preview.beforePiece.clear(); // kept again as Merge is clicked
            }

            if (worked->heldTwice)
                return VersionHistory::Taken::refused (
                    "Nothing was merged: matched by the IDs earlier merges of these lines gave anew, " + name
                    + " or the version both lines have would hold one ID for two different things, which a merge would make one "
                      "(a history these lines' merges made that Phazite can't follow yet, or records another tool wrote). "
                      "To bring over what you need from it, open that version and use Restore on its tracks.");

            if (worked->next > findNextID (*song) && ! worked->preview.renumbered.empty())
            {
                countIDsFrom (*song, worked->next);
                // Its count of IDs, which isn't counted as a change (Cancel Merge leaves it as it is), no reason to keep the song
                // before it again as Merge is clicked.
                worked->preview.beforeChanges = history->countSongChanges();
            }

            worked->preview.numbering = start.ours.songOwn.idNumbering; // the song's, as the list was worked out
            history->preview = std::move (worked->preview);
            history->tellNow();
            return true;
        };
    }
} // namespace

bool startMerge (ProjectContext& context, const juce::String& versionID)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    auto* song = findSong (context);
    const auto* last = history != nullptr ? history->findBaseVersion() : nullptr;
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

    // Not while another waits for its commit, which it would take the place of, nor while the song's branch isn't in the folder
    // (canMerge()).
    if (song == nullptr || last == nullptr || version == nullptr || history->getMerging().has_value() || history->isReadingForCommand()
        || history->isBranchMissing())
        return false;

    // From the newest version both lines have, or from nothing for two of no common line; both songs read, and the list
    // worked out, on the history's thread (known bug 126).
    MergeStart start;
    start.version = versionID;
    start.head = last->id;
    const auto parted = history->findCommonVersion (last->id, versionID);
    const auto* partedVersion = history->findVersion (parted);

    if (parted.isNotEmpty() && partedVersion == nullptr)
        return false;

    start.basePiece = partedVersion != nullptr ? partedVersion->song : juce::String();
    start.theirsPiece = version->song;
    start.renumbering = history->findRenumbering (last->id, versionID, parted);
    start.ourMerges = history->findRenumbering (last->id, versionID, parted, false);
    start.theirMerges = history->findRenumbering (versionID, last->id, parted, false);
    start.theirMergesAtParting = history->findRenumbering (versionID, last->id, parted);

    for (const auto& branch : history->getBranches())
        if (branch.id == version->branch)
            start.otherBranch = branch.name;

    history->mergeFailure.clear();
    song->storeDeviceStates();
    start.ours = getSongState (*song); // its tree written on the history's thread (readMerge())
    start.ourParameters = history->readPluginParameters();
    start.changesAtCopy = history->countSongChanges();

    return startCommandRead (context, *history, "Reading \"" + getDisplayName (*version) + "\" to merge it",
                             [&context, start, store = history->getStore()]() -> VersionHistory::TakeIn
                             {
                                 auto worked = std::make_shared<MergeWorked> (readMerge (*store, start));
                                 return takeInMerge (context, start, worked, 2);
                             });
}

std::vector<MergeLine> getMergeLines (const ProjectContext& context)
{
    std::vector<MergeLine> lines;

    if (const auto* history = findHistory (context); history != nullptr && history->preview.has_value())
    {
        for (const auto& line : history->preview->lines)
        {
            const auto clashes = std::any_of (history->preview->clashes.begin(), history->preview->clashes.end(),
                                              [&line] (const MergeClash& clash) { return isOnLine (clash, line); });
            lines.push_back ({ toLine (line), clashes });
        }
    }

    return lines;
}

juce::String describeMerge (const ProjectContext& context)
{
    const auto* history = findHistory (context);

    if (history == nullptr)
        return {};

    const auto& merge = history->preview.has_value() ? history->preview : history->getMerging();
    const auto* version = merge.has_value() ? history->findVersion (merge->version) : nullptr;
    return version != nullptr ? getDisplayName (*version) : juce::String();
}

juce::StringArray getMergeNotes (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->getMerging().has_value() ? history->getMerging()->notes : juce::StringArray();
}

juce::String getMergeProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->getMergeProblem() : juce::String();
}

juce::String getHeadProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->getHeadProblem() : juce::String();
}

bool canResetToReadable (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->canResetToReadable();
}

bool resetToReadable (ProjectContext& context)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    const auto* readable =
        history != nullptr && history->canResetToReadable() ? history->findVersion (history->getReadableCommit()) : nullptr;

    if (readable == nullptr)
        return false;

    // That commit's song, which the changes are from then, read on the history's thread first.
    return startSongRead (context, *history, "Reading \"" + getDisplayName (*readable) + "\"", readable->song, false,
                          [&context] (SongRead& read) -> VersionHistory::Taken
                          {
                              if (! read.song.isValid())
                                  return VersionHistory::Taken::refused (
                                      "That version can't be read from the Versions folder any more: nothing was reset.");

                              auto* reading = findHistory (context);
                              return reading != nullptr && reading->resetToReadable (std::move (read.song), std::move (read.parameters));
                          });
}

void cancelMerge (ProjectContext& context)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return;

    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (history == nullptr || song == nullptr)
        return;

    // Its list closed, taking nothing.
    if (history->preview.has_value())
    {
        history->preview.reset();
        history->mergeFailure.clear();
        history->tellNow();
        return;
    }

    if (! canCancelMerge (context))
        return;

    // Taken, waiting for its commit: the song made what it was before it, a change of its own, one undo step (git merge
    // --abort), so that no commit says the other line is merged while the song doesn't have it; Undo makes it wait again.
    // Where the song before it can't be read, the branch's last commit's, as git's abort resets to HEAD. The song as it is
    // kept in Local History first, as before Discard All Changes.
    if (const auto& merging = history->getMerging(); merging.has_value())
    {
        const auto before = merging->before.isValid() ? merging->before : history->getLastVersionSong();
        keepSongBefore (*history, "abort");
        const UndoStep step (*song, "Cancel Merge");
        song->storeDeviceStates();

        if (before.isValid())
            makeSongLike (*song, before); // its MERGING too, which the song before it hadn't
        else
            setMergingElement (*song, {});

        removeBrokenReferences (*song);
        history->tellNow();
        return;
    }

    // An element that waits for nothing (one that can't be read, of a history the project's Versions folder isn't, or held
    // by the branch already): taken out alone, one undo step, the rest of the song as it is.
    const UndoStep step (*song, "Cancel Merge");
    setMergingElement (*song, {});
    history->tellNow();
}

bool canCancelMerge (const ProjectContext& context)
{
    // Not while the merge waiting is being read, whose song before it isn't known yet.
    const auto* history = findHistory (context);

    if (history == nullptr)
        return false;

    if (history->preview.has_value())
        return true;

    // An element that waits for nothing, while the view says so (one its own commit finished, silent, stays: harmless).
    const auto& merging = history->getMerging();
    return merging.has_value() ? ! merging->reading : history->holdsMergeElement() && history->getMergeProblem().isNotEmpty();
}

namespace
{
    // Merge refused where the song is numbered otherwise now than as the merge's list was worked out against it (Schema.h:
    // idNumbering; Merge::numbering): the list survives Restore This Version, Restore from Local History, Discard All Changes
    // and Undo of one, each of which can make the song another numbering's, whose items the list's IDs would be taken for
    // (known bug 186). Said where a merge says its problems, the list kept and nothing changed. A merge taken and waiting
    // (Schema.h: MERGING) needs no such check: its element is part of the song, so it goes and comes back with the song's
    // numbering, never one without the other.
    bool refuseOtherNumbering (VersionHistory& history, const Song& song, const VersionHistory::Merge& merge)
    {
        if (getSongOwn (song).idNumbering == merge.numbering)
            return false;

        history.mergeFailure = mergeNumberingRefusal (history, merge.version);
        history.tellNow();
        return true;
    }

    // Merge, once the song before it is kept in the store (merge.beforePiece): every line taken, as one undo step.
    bool takeMergeIn (ProjectContext& context, VersionHistory::Merge merge)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);
        const auto* last = history != nullptr ? history->findBaseVersion() : nullptr;

        if (song == nullptr || last == nullptr || ! history->preview.has_value() || history->getMerging().has_value())
            return false;

        if (refuseOtherNumbering (*history, *song, merge))
            return false;

        merge.head = last->id;
        auto result = mergeSongs (merge.base, merge.before, merge.theirs, merge.lines, [song] { return song->makeID(); });

        merge.clashes = std::move (result.clashes);
        merge.renumbered.insert (merge.renumbered.end(), result.renumbered.begin(),
                                 result.renumbered.end()); // after those of findTranslation()
        nameKnobs (merge.clashes, merge.before, merge.theirs, history->readPluginParameters(), merge.theirParameters);
        merge.notes = result.notes;
        const auto element = history->keepMerge (merge, std::vector<int> (merge.clashes.size(), 1));

        if (! element.isValid())
        {
            history->mergeFailure = "Nothing was merged: Phazite couldn't write the song as it is into the project's Versions folder, "
                                    "which Cancel Merge would need. Make room on the drive, or check that the folder can be written "
                                    "to, then Merge again.";
            history->tellNow();
            return false;
        }

        history->preview.reset();
        history->mergeFailure.clear();
        keepSongBefore (*history, "merge");

        // A change of its own, one undo step named for what it merges ("Merge idea-2"): the song made what the merge of every
        // line makes, its IDs counted on past them, with the merge waiting for its version in it (Schema.h: MERGING), so that
        // Undo takes the merge back whole, and every file the song is saved into holds it as the song does.
        const UndoStep step (*song, "Merge " + history->describeCommit (merge.version));
        result.song.removeChild (result.song.getChildWithName (schema::MERGING), nullptr); // one finished before, as the song had it
        makeSongLike (*song, result.song);
        removeBrokenReferences (*song); // what one side names that the other took away: a send to a return it deleted
        countIDs (*song);
        setMergingElement (*song, element);
        history->adoptLater(); // the line merged in, another computer's too, now that the song holds the merge
        history->tellNow();    // the view shows the merge's conflicts at once
        return true;
    }
} // namespace

bool finishMerge (ProjectContext& context)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    auto* song = findSong (context);

    const auto* last = history != nullptr ? history->findBaseVersion() : nullptr;

    if (song == nullptr || last == nullptr || ! history->preview.has_value() || history->getMerging().has_value())
        return false;

    // Not while a first save copies the Versions folder, into which the song before is kept: the list stays, and the view
    // says why, as a commit's refusal does.
    if (history->isCopyingFolder())
    {
        history->mergeFailure = "Nothing was merged: Phazite is saving the project into its folder. Merge again once it is saved.";
        history->tellNow();
        return false;
    }

    // Nor into a song numbered otherwise than as the list was worked out: before the song is kept in the Versions folder.
    if (refuseOtherNumbering (*history, *song, *history->preview))
        return false;

    // The song as it was, for Cancel Merge: kept in the store first (where the song's element of the merge names it), and in
    // Local History too. Nothing is taken if the store can't keep it: the merge's list stays, and the view says why. Kept
    // already as the list was worked out, on the history's thread, while the song is still as it was then (known bug 126);
    // changed since, kept there now, the window held meanwhile, then taken.
    auto merge = *history->preview;
    song->storeDeviceStates();

    if (merge.beforePiece.isNotEmpty() && history->countSongChanges() == merge.beforeChanges)
        return takeMergeIn (context, std::move (merge));

    // The song as it is, its state taken here and its tree written on the history's thread, which the merge then takes in (no
    // tree the message thread holds handed over).
    merge.before = {};
    merge.beforePiece.clear();
    return startCommandRead (
        context, *history, "Keeping the song as it is, for Cancel Merge",
        [&context, merge, taken = getSongState (*song), store = history->getStore()]() mutable -> VersionHistory::TakeIn
        {
            merge.before = writeSongTree (taken);
            const auto pieces = splitSong (merge.before);
            const auto kept = store->writeSong (pieces);
            store->getPieces().closeFiles();
            merge.beforePiece = kept ? pieces.song : juce::String();

            return [&context, merge]() -> VersionHistory::Taken
            {
                if (merge.beforePiece.isEmpty())
                {
                    const auto why =
                        juce::String ("Nothing was merged: Phazite couldn't write the song as it is into the project's Versions "
                                      "folder, which Cancel Merge would need. Make room on the drive, or check that the folder can be "
                                      "written to, then Merge again.");

                    if (auto* found = findHistory (context))
                        found->mergeFailure = why; // said where a merge says its problems, its list kept

                    return VersionHistory::Taken::refused (why);
                }

                return takeMergeIn (context, merge);
            };
        });
}

bool isMerging (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->getMerging().has_value();
}

std::vector<ClashInfo> getClashes (const ProjectContext& context)
{
    std::vector<ClashInfo> infos;
    const auto* history = findHistory (context);

    if (history == nullptr || ! history->getMerging().has_value())
        return infos;

    const auto& merging = *history->getMerging();

    for (size_t i = 0; i < merging.clashes.size(); ++i)
    {
        const auto& clash = merging.clashes[i];
        ClashInfo info;
        info.line = clash.line;
        info.what = clash.what;
        info.otherBranch = merging.otherBranch;
        info.chosen = merging.chosen[i];

        if (clash.property.isValid())
        {
            const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };

            for (int v = 0; v < 3; ++v)
                info.values[v] = values[v]->isVoid() ? juce::String ("none") : values[v]->toString();
        }
        else
        {
            info.values[0] = clash.baseElement.isValid() ? "as in the base" : "none";
            info.values[1] = clash.oursElement.isValid() ? "as on this branch" : "none";
            info.values[2] = clash.theirsElement.isValid() ? "as on " + merging.otherBranch : "none";
        }

        infos.push_back (std::move (info));
    }

    return infos;
}

namespace
{
    // A choice leaves nothing to the keeper of the song's values (SongValues.h: its safety net): each area's choice sets its
    // values (chooseInAreas()), a whole track's or the master's among them (chooseTrackClash(): its clips, modulators, macros,
    // sends and its devices' store entries through each area's code, its values through the tracks' setters), and no choice
    // in the tree adds an area's entries; checked in Debug builds as a choice ends, as the versions' passes check it (ApplySong.cpp:
    // NothingTakenIn). What the keeper took in counted for every area that counts it, a line each, in the order of the areas.
    struct NothingTakenIn
    {
        explicit NothingTakenIn (const Song& s)
            : song (s),
              before (count (s))
        {
        }

        ~NothingTakenIn() { jassert (count (song) == before); }

        static int count (const Song& s)
        {
            const auto* values = s.getValues();

            if (values == nullptr)
                return 0;

            // Not the devices': their choices set them through their setters (chooseDeviceClash()), and their part of the
            // keeper only tells what the tree holds, taking nothing in, until the devices' switch.
            return values->countTempoMapAdopted()      // the tempo map's (the clips' and the sends': as a track, below)
                   + values->countChordLaneAdopted()   // the chord lane's
                   + values->countMidiLinksAdopted()   // the MIDI links'
                   + values->countCurvesAdopted()      // the curves'
                   + values->countSectionsAdopted()    // the sections'
                   + values->countModulationAdopted()  // modulation's
                   + values->countDeviceStoreAdopted() // the store's: chooseDeviceStoreClash(), and a whole track's
                                                       // (chooseTrackClash()) through the store's functions
                   + values->countTrackLeavesAdopted() // the tracks' switches and view: a switch's clash and a whole
                                                       // track's through their setters (chooseTrackClash())
                   + values->countTracksAdopted()      // the tracks': a track's or the master's through their setters
                   + values->countSongOwnAdopted();    // the song's own: a clash of it through its setters
                                                       // (chooseSongOwnClash()), the merge's record through setSongOwn()
        }

        const Song& song;
        const int before;

        JUCE_DECLARE_NON_COPYABLE (NothingTakenIn)
    };

    // A clash in an area the song holds as a value beside its tree (AreaClashes.h): each area asked in turn, a line each, in
    // this order: the song's own (a property of the song's own, or its LASTKEY), the tempo map (a path into the song's
    // TEMPOMAP), the chord lane (a path into the song's CHORDLANE), the MIDI links, the sections, modulation (a path through a
    // modulator or a macro: a container's macro's before the container's), the sends (a path through a send), the devices (a
    // path through a device, a container or a chain: the store's first), a curve (a path to a CURVE of the song's AUTOMATION,
    // or through one, refused), a note (a note's property: its path goes through its clip, so before the clip's), a clip (a
    // clip's property, or any other path through a clip, refused), a whole track or the master, or a switch of one
    // (chooseTrackClash()). What the first whose path it is did with it; none of them (notTheArea) for a clash in the song's
    // tree.
    AreaChoice chooseInAreas (Song& song, const MergeClash& clash, int value, const MergeSides& sides)
    {
        if (const auto choice = chooseSongOwnClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a property of the song's own (PHAZITE's), or its LASTKEY whole

        if (const auto choice = chooseTempoMapClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice;

        if (const auto choice = chooseChordLaneClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice;

        if (const auto choice = chooseMidiLinkClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a path into the song's MIDILINKS

        if (const auto choice = chooseSectionClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a path into the song's SECTIONS

        if (const auto choice = chooseModulationClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a path through a MODULATOR or a MACRO

        if (const auto choice = chooseSendClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a path through a SEND

        if (const auto choice = chooseDeviceStoreClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a path to what the store holds of a DEVICE or a CHAIN

        if (const auto choice = chooseDeviceClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // any other path through a DEVICE, a CONTAINER or a CHAIN

        if (const auto choice = chooseCurveClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice;

        if (const auto choice = chooseNoteClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice;

        if (const auto choice = chooseClipClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice;

        if (const auto choice = chooseTrackClash (song, clash, value, sides); choice != AreaChoice::notTheArea)
            return choice; // a whole TRACK or the MASTER, or a switch of one

        return AreaChoice::notTheArea;
    }
} // namespace

bool chooseClash (ProjectContext& context, int index, int value)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->getMerging().has_value() || ! juce::isPositiveAndBelow (value, 3)
        || ! juce::isPositiveAndBelow (index, (int) history->getMerging()->clashes.size()))
        return false;

    const auto merging = *history->getMerging();
    const auto& clash = merging.clashes[(size_t) index];

    // A change of its own, one undo step, named for the choice: "Take the Other Branch's: Bass volume".
    const UndoStep step (*song, getClashChoiceName (value) + ": " + clash.line + " " + clash.what);
    const NothingTakenIn byTheValuesAlone (*song);

    // Each area the song holds as a value beside its tree asked. Nothing should reach a clash none of them has: only what
    // Phazite doesn't know at the song's top could, which the song drops as it opens (SongOwnArea.h) and a merge leaves out,
    // making no clash of it (SongMerge.cpp: mergeSongs()). Should one come all the same, it is chosen with nothing of the
    // song's to write but the choice itself, below.
    const auto choice = chooseInAreas (*song, clash, value, { merging.base, merging.before, merging.theirs });
    jassert (choice != AreaChoice::notTheArea); // a clash no area holds: the merge makes none (above)

    if (choice == AreaChoice::refused)
        return false;

    // What the value chosen names that the song took away since (a send to a return deleted here, a sidechain from a track
    // gone), as the merge itself did; and which value it has, which Undo puts back with the song's.
    removeBrokenReferences (*song);
    auto chosen = merging.chosen;
    chosen[(size_t) index] = value;
    juce::String digits;

    for (const auto each : chosen)
        digits << each;

    // The merge waiting's record, in the choice's step.
    auto own = getSongOwn (*song);

    if (own.merging.has_value())
        own.merging->chosen = digits;

    setSongOwn (*song, std::move (own));
    history->tellNow();
    return true;
}

juce::String getClashChoiceName (int value)
{
    return value == 0 ? "Take What Was Before" : value == 1 ? "Keep This Branch's" : "Take the Other Branch's";
}

void mergeSelectedVersion (ProjectContext& context)
{
    startMerge (context, getSelectedVersion (context));
}

namespace
{
    // A branch's last commit, or none.
    juce::String findBranchHead (const VersionHistory& history, const juce::String& branchID)
    {
        for (const auto& branch : history.getBranches())
            if (branch.id == branchID)
                return branch.head;

        return {};
    }
} // namespace

int countCommitsToMerge (const ProjectContext& context, const juce::String& branchID)
{
    // Its line's commits that the current branch's line doesn't hold.
    const auto* history = findHistory (context);
    const auto* last = history != nullptr ? history->findBaseVersion() : nullptr;
    const auto head = history != nullptr ? findBranchHead (*history, branchID) : juce::String();

    return last != nullptr && head.isNotEmpty() ? history->countVersionsNotIn (head, last->id) : 0;
}

bool canMergeBranch (const ProjectContext& context, const juce::String& branchID)
{
    const auto* history = findHistory (context);
    const auto current = getCurrentBranch (context);

    return history != nullptr && ! history->isReading() && ! history->isSaving() && ! history->isBranchMissing() && current.has_value()
           && current->id != branchID && ! history->preview.has_value() && ! history->getMerging().has_value()
           && countCommitsToMerge (context, branchID) > 0;
}

bool canMergeAnyBranch (const ProjectContext& context)
{
    const auto branches = getBranches (context);
    return std::any_of (branches.begin(), branches.end(),
                        [&context] (const BranchInfo& branch) { return canMergeBranch (context, branch.id); });
}

bool mergeBranch (ProjectContext& context, const juce::String& branchID)
{
    const auto* history = findHistory (context);
    return history != nullptr && canMergeBranch (context, branchID) && startMerge (context, findBranchHead (*history, branchID));
}

bool canFinishMerge (const ProjectContext& context)
{
    return ! getMergeLines (context).empty() && ! isSavingProjectFolder (context);
}

//==============================================================================
VersionsRoom getVersionsRoom (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? VersionsRoom { history->getSize(), history->getBouncesSize() } : VersionsRoom {};
}

namespace
{
    // The rate the song plays at, if an export offers it (the comparison then plays the bounce as it is), or else 48 kHz.
    double getBounceRate (const ProjectContext& context)
    {
        if (auto* session = getSession (context))
            for (const auto offered : exportSampleRates)
                if (juce::exactlyEqual (offered, session->getEngine().getSampleRate()))
                    return offered;

        return 48000.0;
    }

    // What a bounce is: FLAC of 16 bits, dithered, at that rate.
    ExportSettings getBounceSettings (const ProjectContext& context)
    {
        ExportSettings settings;
        settings.format = ExportSettings::Format::flac;
        settings.bitDepth = 16;
        settings.sampleRate = getBounceRate (context);
        return settings;
    }

    // A version's bounce: the song rendered as Export Song renders it (model/project/internal/Export.h), into the version's
    // file among the bounces, in FLAC of 16 bits, dithered. The file is the export's: written whole, or not at all.
    class VersionBounce final : public TrackRender::Impl
    {
    public:
        VersionBounce (ProjectContext& c, juce::String id, const juce::String& name)
            : context (c),
              versionID (std::move (id)),
              title ("Bouncing \"" + name + "\"")
        {
        }

        bool start (std::function<void (const juce::Result&)> onFinished)
        {
            auto* history = findHistory (context);
            auto* song = findSong (context);

            if (history == nullptr || song == nullptr)
                return false;

            const auto file = history->getBounceFile (versionID);

            if (! file.getParentDirectory().createDirectory())
                return false;

            exporting = startExport (*song, file, getBounceSettings (context),
                                     [this, done = std::move (onFinished)] (const ExportReport& report)
                                     {
                                         if (auto* h = findHistory (context))
                                             h->readBounces();

                                         if (done != nullptr)
                                             done (report.result); // last: it may delete this
                                     });
            return exporting != nullptr;
        }

        float getProgress() const override { return exporting != nullptr ? exporting->getProgress() : 0.0f; }
        juce::String getTitle() const override { return title; }

    private:
        ProjectContext& context;
        const juce::String versionID, title;
        std::unique_ptr<SongExport> exporting;
    };

    // Deletes a file the engine may still be reading (a bounce compared until now): at once if it can, or once the plans
    // that played it are let go of, each plan given letting go of those before it (engine/core/Handover.h).
    void deleteOnceFree (Song::Ptr song, juce::File file, int triesLeft)
    {
        if (! file.existsAsFile() || file.deleteFile() || triesLeft <= 0)
            return;

        if (auto* session = findSession (*song))
            session->update();

        juce::Timer::callAfterDelay (100, [song, file, triesLeft] { deleteOnceFree (song, file, triesLeft - 1); });
    }

    // The song alone again, without a bounce beside it.
    void stopComparing (ProjectContext& context, VersionHistory& history)
    {
        history.bounceCompared.clear();
        history.hearingBounce = false;

        if (auto* session = getSession (context))
        {
            session->getPlayer().setComparison (std::nullopt);
            session->update();
        }
    }
} // namespace

namespace
{
    // Whether the song is a version as it is now: the current branch's last, with no changes since.
    bool isSongNow (VersionHistory& history, const juce::String& versionID)
    {
        const auto* last = history.findBaseVersion();
        return last != nullptr && last->id == versionID && ! history.mayHaveChanges();
    }

    // An old version's bounce (step V5): its song alone, read from the versions and made a song of its own in the project's
    // folders, rendered in the background as Export Song renders a song (internal/Export.h: startBackgroundExport()), with
    // plug-in instances of its own, while the open song plays on. The history holds it while it renders.
    class OldVersionBounce final : public TrackRender::Impl
    {
    public:
        OldVersionBounce (ProjectContext& c, juce::String id)
            : context (c),
              versionID (std::move (id))
        {
        }

        juce::Result start()
        {
            auto* history = findHistory (context);
            auto* open = findSong (context);
            const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

            if (open == nullptr || version == nullptr)
                return juce::Result::fail ("That version isn't there any more.");

            title = "Bouncing \"" + getDisplayName (*version) + "\"";

            // Its song read on the history's thread (known bug 126), then rendered.
            history->readCommitInBackground (versionID,
                                             [this, living = std::weak_ptr<bool> (alive)] (const juce::ValueTree& state)
                                             {
                                                 if (! living.expired())
                                                     if (const auto result = render (state); result.failed())
                                                         finished (result); // which lets go of this
                                             });
            return juce::Result::ok();
        }

        juce::Result render (const juce::ValueTree& state)
        {
            auto* history = findHistory (context);
            auto* open = findSong (context);
            const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

            if (open == nullptr || version == nullptr)
                return juce::Result::fail ("That version isn't there any more.");

            song = state.isValid() ? makeSong (state, open->getFile(), open->getCacheFolder()) : nullptr;

            if (song == nullptr)
                return juce::Result::fail ("Phazite couldn't read \"" + getDisplayName (*version) + "\".");

            if (getSongEnd (*song) <= BeatPosition())
                return juce::Result::fail ("\"" + getDisplayName (*version) + "\" has no clips, so there is nothing to bounce.");

            const auto file = history->getBounceFile (versionID);

            if (! file.getParentDirectory().createDirectory())
                return juce::Result::fail ("Phazite couldn't write into the project's Versions folder.");

            exporting = startBackgroundExport (*song, getPluginHost (getEngineContext (context)), file, getBounceSettings (context),
                                               [this] (const ExportReport& report) { finished (report.result); });
            return juce::Result::ok();
        }

        float getProgress() const override { return exporting != nullptr ? exporting->getProgress() : 0.0f; }
        juce::String getTitle() const override { return title; }

    private:
        ProjectContext& context;
        const juce::String versionID;
        juce::String title;
        Song::Ptr song;                                              // the version's, alone
        std::unique_ptr<SongExport> exporting;                       // goes before the song it renders
        std::shared_ptr<bool> alive = std::make_shared<bool> (true); // what its read checks as it comes back

        void finished (const juce::Result& result)
        {
            auto* history = findHistory (context);

            if (history == nullptr)
                return;

            history->bounceProblem =
                result.wasOk() ? juce::String() : title.replace ("Bouncing", "Couldn't bounce") + ": " + result.getErrorMessage();
            history->bouncingVersion.clear();
            const auto self = std::move (history->backgroundBounce); // this: it goes as this returns
            history->readBounces();
        }
    };
} // namespace

bool canBounce (const ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);
    const auto* session = getSession (context);

    if (song == nullptr || history == nullptr || history->findVersion (versionID) == nullptr || history->isSaving()
        || history->hasBounce (versionID) || history->backgroundBounce != nullptr || isRecording (*song)
        || (session != nullptr && session->isLentToRender()))
        return false;

    // The song as it is now must have something to play; an old version's song is rendered on its own.
    return ! isSongNow (*history, versionID) || canExport (*song);
}

std::unique_ptr<TrackRender> startBounce (ProjectContext& context, const juce::String& versionID,
                                          std::function<void (const juce::Result&)> onFinished)
{
    if (! canBounce (context, versionID) || ! isSongNow (*findHistory (context), versionID))
        return {};

    auto bounce = std::make_unique<VersionBounce> (context, versionID, getDisplayName (*findHistory (context)->findVersion (versionID)));

    if (! bounce->start (std::move (onFinished)))
        return {};

    return std::make_unique<TrackRender> (std::move (bounce));
}

bool startBackgroundBounce (ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);

    if (! canBounce (context, versionID) || isSongNow (*history, versionID))
        return false;

    auto bounce = std::make_unique<OldVersionBounce> (context, versionID);

    if (const auto started = bounce->start(); started.failed())
    {
        history->bounceProblem = started.getErrorMessage();
        history->tellNow();
        return false;
    }

    history->bounceProblem.clear();
    history->bouncingVersion = versionID;
    history->backgroundBounce = std::make_unique<TrackRender> (std::move (bounce));
    history->tellNow();
    return true;
}

std::optional<float> getBounceProgress (const ProjectContext& context, const juce::String& versionID)
{
    const auto* history = findHistory (context);

    if (history == nullptr || history->backgroundBounce == nullptr || history->bouncingVersion != versionID)
        return std::nullopt;

    return history->backgroundBounce->getProgress();
}

juce::String getBounceProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->bounceProblem : juce::String();
}

//==============================================================================
namespace
{
    juce::String count (size_t number, const char* one, const char* many)
    {
        return juce::String ((juce::int64) number) + " " + (number == 1 ? one : many);
    }

    // A piece of a commit that couldn't be read, in words, by the track it was in as the song names it now: "Bass: a clip".
    juce::String describeMissing (const Song& song, const MissingPiece& missing)
    {
        if (missing.path.isEmpty())
            return "the whole song";

        juce::String track;

        for (const auto& step : missing.path)
        {
            const auto type = step.upToFirstOccurrenceOf (":", false, false);
            const auto key = step.fromFirstOccurrenceOf (":", false, false);

            if (type == schema::MASTER.toString())
                track = "Master";
            else if (type == schema::TRACK.toString())
                track = hasTrack (song, TrackID::fromRaw ((std::uint64_t) key.getLargeIntValue()))
                            ? getTrackName (song, TrackID::fromRaw ((std::uint64_t) key.getLargeIntValue()))
                            : juce::String ("a track gone since");
        }

        const auto last = missing.path[missing.path.size() - 1].upToFirstOccurrenceOf (":", false, false);
        const auto what = last == schema::CLIP.toString()              ? "a clip"
                          : last == schema::DEVICE.toString()          ? "a device"
                          : last == schema::CONTAINER.toString()       ? "a container"
                          : last == schema::CURVE.toString()           ? "automation"
                          : last == schema::TRACK.toString()           ? "the whole track"
                          : last == schema::MASTER.toString()          ? "the whole master"
                          : last == schema::versions::RUN.toString()   ? "notes"
                          : last == schema::versions::BYTES.toString() ? "a plug-in's settings"
                          : last == schema::PHAZITE.toString()         ? "the song's own"
                                                                       : "a part";

        return (track.isNotEmpty() ? track : juce::String ("Song")) + ": " + what;
    }

    juce::String describeVersion (const VersionHistory& history, const juce::String& id)
    {
        const auto* version = history.findVersion (id);
        return "\"" + (version != nullptr ? getDisplayName (*version) : id.substring (0, 7)) + "\"";
    }
} // namespace

namespace
{
    // What a snapshot's reason is called in the view.
    juce::String describeReason (const juce::String& reason)
    {
        return reason == "open"      ? "As opened"
               : reason == "auto"    ? "Auto-saved"
               : reason == "restore" ? "Before Restore"
               : reason == "discard" ? "Before Discard All Changes"
               : reason == "merge"   ? "Before Merge"
               : reason == "history" ? "Before Restoring an Automatic Save"
               : reason == "abort"   ? "Before Cancel Merge"
               : reason == "look"    ? "Before Opening a Version"
                                     : reason;
    }

    // The song as it is, kept in Local History before what makes it another, while it differs from its last commit.
    void keepSongBefore (VersionHistory& history, const juce::String& reason)
    {
        if (history.mayHaveChanges())
            history.takeSnapshot (reason);
    }

    // When a snapshot was taken, as the undo step names it: its time today, else its day and time.
    juce::String describeSnapshotTime (juce::int64 time)
    {
        const juce::Time when (time), now (juce::Time::getCurrentTime());
        const auto today = when.getYear() == now.getYear() && when.getDayOfYear() == now.getDayOfYear();
        return today ? when.formatted ("%H:%M") : when.formatted ("%d %b %H:%M");
    }
} // namespace

std::vector<SnapshotInfo> getSnapshots (const ProjectContext& context)
{
    std::vector<SnapshotInfo> infos;

    if (const auto* history = findHistory (context))
    {
        const auto branches = history->getBranches();

        for (const auto& snapshot : history->getSnapshots())
        {
            const auto branch =
                std::find_if (branches.begin(), branches.end(), [&snapshot] (const auto& b) { return b.id == snapshot.branch; });
            infos.push_back ({ snapshot.time, branch != branches.end() ? branch->name : juce::String(), describeReason (snapshot.reason),
                               branch != branches.end() ? branch->id : juce::String() });
        }
    }

    return infos;
}

namespace
{
    // Restore from Local History, once the snapshot's song is read.
    VersionHistory::Taken takeInSnapshot (ProjectContext&, juce::int64 time, const juce::ValueTree& then);
} // namespace

bool restoreSnapshot (ProjectContext& context, juce::int64 time)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return false;
    auto* history = findHistory (context);

    if (history == nullptr || findSong (context) == nullptr)
        return false;

    // Its song read on the history's thread first, as it was kept.
    const auto& all = history->getSnapshots();
    const auto snapshot = std::find_if (all.begin(), all.end(), [time] (const Snapshot& each) { return each.time == time; });

    if (snapshot == all.end())
        return false;

    return startSongRead (
        context, *history, "Reading the automatic save of " + describeSnapshotTime (time), snapshot->song, false,
        [&context, time] (SongRead& read) { return takeInSnapshot (context, time, read.song); }, Kept::asItIs);
}

namespace
{
    VersionHistory::Taken takeInSnapshot (ProjectContext& context, juce::int64 time, const juce::ValueTree& then)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);

        if (history == nullptr || song == nullptr)
            return false;

        if (! then.isValid())
            return VersionHistory::Taken::refused (
                "That automatic save can't be read from the Versions folder: a piece of it is damaged or missing. Nothing was "
                "restored.");

        if (const auto why = whyNotChangeSongNow (context, "nothing was restored."); why.isNotEmpty())
            return VersionHistory::Taken::refused (why);

        // The song as it is kept first; then a change of its own, one undo step, as VS Code's Local History restores a file's save.
        keepSongBefore (*history, "history");
        const UndoStep step (*song, "Restore the Automatic Save of " + describeSnapshotTime (time));
        song->storeDeviceStates();
        makeSongLike (*song, then);
        removeBrokenReferences (*song);
        return true;
    }
} // namespace

void selectSnapshot (ProjectContext& context, juce::int64 time)
{
    if (auto* history = findHistory (context))
        history->selectedSnapshot = time;
}

juce::int64 getSelectedSnapshot (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->selectedSnapshot : 0;
}

juce::String describeRestoreSnapshot (const ProjectContext& context)
{
    const auto time = getSelectedSnapshot (context);
    return canRestoreSelectedSnapshot (context) ? "Restore the Automatic Save of " + describeSnapshotTime (time) : juce::String();
}

bool canRestoreSelectedSnapshot (const ProjectContext& context)
{
    const auto* history = findHistory (context);

    if (history == nullptr || history->selectedSnapshot == 0)
        return false;

    const auto& all = history->getSnapshots();
    return std::any_of (all.begin(), all.end(),
                        [history] (const Snapshot& snapshot) { return snapshot.time == history->selectedSnapshot; });
}

void restoreSelectedSnapshot (ProjectContext& context)
{
    restoreSnapshot (context, getSelectedSnapshot (context));
}

bool verifyHistory (ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && ! history->isChecking() && history->checkHistory();
}

void stopVerifyingHistory (ProjectContext& context)
{
    if (auto* history = findHistory (context); history != nullptr && history->isChecking())
        history->cancelCheck();
}

void verifyOrStopVerifying (ProjectContext& context)
{
    if (isVerifyingHistory (context))
        stopVerifyingHistory (context);
    else
        verifyHistory (context);
}

juce::String describeVerify (const ProjectContext& context)
{
    return isVerifyingHistory (context) ? "Stop Verifying History" : juce::String();
}

bool canVerifyHistory (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->keepsVersions() && ! history->isReading() && (history->isChecking() || ! history->isSaving());
}

bool isVerifyingHistory (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->isChecking();
}

juce::String describeHistoryCheck (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* song = findSong (context);

    if (history == nullptr || song == nullptr)
        return {};

    if (history->isChecking())
        return "Verifying the history... " + juce::String (juce::roundToInt (history->getCheckProgress() * 100.0f)) + "%";

    // What the sweep as the history opened couldn't be sure of, first.
    juce::StringArray parts;

    if (const auto& unsure = history->getSweepProblem(); unsure.isNotEmpty())
        parts.add ("Phazite is keeping everything in the Versions folder, because " + unsure + ". Verify History says what is damaged.");

    if (const auto& uncopied = history->getAdoptionProblem(); uncopied.isNotEmpty())
        parts.add (uncopied + ".");

    const auto& check = history->getLastCheck();

    if (! check.has_value())
        return parts.joinIntoString (" ");

    if (check->stopped)
    {
        parts.add ("Verify History was stopped before it was done.");
        return parts.joinIntoString (" ");
    }

    if (check->damagedPieces.isEmpty() && check->damaged.empty() && check->audio.empty() && check->damagedSongs.empty()
        && check->unreadableParents.empty() && check->unreadPacks.isEmpty())
        parts.add ("Verified " + count ((size_t) check->numVersions, "version", "versions") + ", "
                   + count ((size_t) check->numSongs, "song kept without a version", "songs kept without a version") + ", "
                   + count ((size_t) check->numPieces, "piece", "pieces") + " and "
                   + count ((size_t) check->numAudioFiles, "audio file", "audio files") + ": all whole.");

    if (! check->unreadPacks.isEmpty())
    {
        juce::StringArray names;

        for (const auto& pack : check->unreadPacks)
            names.add (pack.substring (0, 7));

        parts.add (count ((size_t) check->unreadPacks.size(), "pack", "packs") + " of the Versions folder can't be read ("
                   + names.joinIntoString (", ") + "): damaged, held by another program, or written by another build of Phazite.");
    }

    if (! check->damagedPieces.isEmpty())
        parts.add (count ((size_t) check->damagedPieces.size(), "piece is", "pieces are") + " damaged.");

    if (! check->damaged.empty())
    {
        juce::StringArray names;

        for (const auto& [id, missing] : check->damaged)
            names.add (describeVersion (*history, id) + " (" + describeMissing (*song, missing.front())
                       + (missing.size() > 1 ? " and " + juce::String ((juce::int64) missing.size() - 1) + " more" : juce::String()) + ")");

        parts.add (count (check->damaged.size(), "version can't", "versions can't") + " be read whole: " + names.joinIntoString (", ")
                   + ". Restore This Version takes what can be.");
    }

    // The audio a version plays that isn't as it recorded it, by version: "Verse 2" (Audio/Kick.wav missing).
    if (! check->audio.empty())
    {
        juce::StringArray names;

        for (const auto& [id, problems] : check->audio)
        {
            juce::StringArray files;

            for (const auto& problem : problems)
                files.add (problem.path
                           + (problem.kind == VersionHistory::AudioProblem::Kind::missing      ? " missing"
                              : problem.kind == VersionHistory::AudioProblem::Kind::unreadable ? " can't be read"
                              : problem.kind == VersionHistory::AudioProblem::Kind::otherSize  ? " changed: another size"
                                                                                               : " changed since"));

            names.add (describeVersion (*history, id) + " (" + files.joinIntoString ("; ") + ")");
        }

        parts.add (count (check->audio.size(), "version can't play its audio as it was saved",
                          "versions can't play their audio as they were saved")
                   + ": " + names.joinIntoString (", ") + ".");
    }

    // Versions whose earlier versions can't be read, which the history and Restore need.
    for (const auto& [id, parents] : check->unreadableParents)
    {
        juce::StringArray earlier;

        for (const auto& parent : parents)
            earlier.add (parent.substring (0, 7));

        parts.add (describeVersion (*history, id) + " follows a version that can't be read from the Versions folder ("
                   + earlier.joinIntoString (", ") + "): damaged, or not arrived from another computer.");
    }

    // The songs kept without a version that can't be read whole: Local History's, those left on branches.
    for (const auto& damaged : check->damagedSongs)
    {
        juce::String branchName;

        for (const auto& branch : history->getBranches())
            if (branch.id == damaged.song.branch)
                branchName = branch.name;

        const auto mine = damaged.song.computer == makeUsableFileName (getThisComputer());
        const auto what = damaged.song.kind == KeptSong::Kind::snapshot
                              ? "The automatic save of " + describeSnapshotTime (damaged.song.time) + (mine ? "" : " on another computer")
                              : "The song " + juce::String (mine ? "" : "another computer ")
                                    + (damaged.song.kind == KeptSong::Kind::deleted
                                           ? "left on a deleted branch"
                                           : "left on " + (branchName.isNotEmpty() ? "'" + branchName + "'" : juce::String ("a branch")));
        parts.add (
            what + " can't be read whole (" + describeMissing (*song, damaged.missing.front())
            + (damaged.missing.size() > 1 ? " and " + juce::String ((juce::int64) damaged.missing.size() - 1) + " more" : juce::String())
            + ").");
    }

    if (! check->lost.empty())
        parts.add (count (check->lost.size(), "version", "versions") + " no branch holds: Recover Lost Versions lists them.");

    return parts.joinIntoString (" ");
}

std::vector<LostVersionsInfo> getLostVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    std::vector<LostVersionsInfo> lost;

    if (history == nullptr)
        return lost;

    for (const auto& line : history->getLostLines())
        lost.push_back (
            { line.key, line.branch, line.why, juce::Time (line.time), line.versions.size(), line.name, line.song.isNotEmpty() });

    return lost;
}

bool canRecoverLostVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && ! history->isReading() && ! history->isSaving() && ! history->getLostLines().empty();
}

juce::String recoverLostVersions (ProjectContext& context, const juce::String& key)
{
    auto* history = findHistory (context);
    return history != nullptr && canRecoverLostVersions (context) ? history->recoverLine (key) : juce::String();
}

juce::String getReadingProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->readingProblem : juce::String();
}

juce::String getRestoreProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* song = findSong (context);

    if (history == nullptr || song == nullptr)
        return {};

    if (history->restoreRefused.isNotEmpty())
        return history->restoreRefused;

    if (history->restoreMissing.empty())
        return {};

    const auto& missing = history->restoreMissing;

    if (missing.size() == 1 && missing.front().path.isEmpty())
        return "That version can't be read: Verify History says what is damaged.";

    juce::StringArray what;

    for (const auto& piece : missing)
        what.addIfNotAlreadyThere (describeMissing (*song, piece));

    return "Restored but for what the version's damaged pieces held, which stayed as it was: " + what.joinIntoString (", ") + ".";
}

juce::StringArray getDivergentVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* branch = history != nullptr ? history->findCurrentBranch() : nullptr;
    return branch != nullptr ? branch->divergent : juce::StringArray();
}

juce::String describeDivergence (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* branch = history != nullptr ? history->findCurrentBranch() : nullptr;

    if (branch == nullptr || branch->divergent.isEmpty())
        return {};

    juce::StringArray names;

    for (const auto& id : branch->divergent)
        names.add (describeVersion (*history, id));

    return branch->name + " went another way on another computer too, to " + names.joinIntoString (" and ") + ": Merge into '"
           + branch->name + "' in its menu joins the lines.";
}

void bounceVersion (ProjectContext& context, const juce::String& versionID, TrackRenderHost& host)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return;

    auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;
    const auto title = "Bouncing \"" + (version != nullptr ? getDisplayName (*version) : juce::String ("the version")) + "\"";

    if (! canBounce (context, versionID))
    {
        host.renderFinished (title, juce::Result::fail ("It can't be bounced now: it has a bounce already, or the song is being bounced, "
                                                        "exported or recorded."));
        return;
    }

    // An old version: in the background, beside the song playing.
    if (! isSongNow (*history, versionID))
    {
        if (! startBackgroundBounce (context, versionID))
            host.renderFinished (title, juce::Result::fail (getBounceProblem (context)));

        return;
    }

    if (auto render =
            startBounce (context, versionID, [&host, title] (const juce::Result& result) { host.renderFinished (title, result); }))
        host.runRender (std::move (render));
    else
        host.renderFinished (title, juce::Result::fail ("Phazite couldn't start rendering."));
}

bool deleteBounce (ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (history == nullptr || song == nullptr)
        return false;

    // Still rendering in the background: it stops, and leaves nothing.
    if (history->backgroundBounce != nullptr && history->bouncingVersion == versionID)
    {
        history->backgroundBounce = nullptr;
        history->bouncingVersion.clear();
        history->readBounces();
        return true;
    }

    if (! history->hasBounce (versionID))
        return false;

    // Compared with the song until now: the song alone again first, then the file, once the engine lets go of it.
    if (history->bounceCompared == versionID)
        stopComparing (context, *history);

    const auto file = history->getBounceFile (versionID);
    history->bouncesGoing.insert (versionID);
    deleteOnceFree (song, file, 50);
    history->readBounces();
    return true;
}

void setBounceWithVersion (ProjectContext& context, bool bounce)
{
    // Told, as what Save Version says changes with it.
    if (auto* history = findHistory (context); history != nullptr && history->bounceWithVersion != bounce)
    {
        history->bounceWithVersion = bounce;
        history->tellNow();
    }
}

bool getBounceWithVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->bounceWithVersion;
}

bool hearVersion (ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);
    auto* session = getSession (context);

    if (history == nullptr || song == nullptr || session == nullptr || (versionID.isNotEmpty() && ! history->hasBounce (versionID)))
        return false;

    // The version's bounce beside the song from the next plan on, where each beat is in it by the version's tempo map.
    auto& player = session->getPlayer();

    if (versionID.isNotEmpty() && (history->bounceCompared != versionID || player.getComparisonState() == nullptr))
    {
        // Its tempo map alone, from its song's own piece (known bug 126: the whole song was read for it).
        const auto then = history->readSongOwn (versionID);
        juce::ValueTree tempo (schema::PHAZITE);

        if (const auto map = then.getChildWithName (schema::TEMPOMAP); map.isValid())
            tempo.appendChild (map.createCopy(), nullptr);

        player.setComparison (SongPlayer::Comparison { history->getBounceFile (versionID), tempo });
        history->bounceCompared = versionID;
        session->update();
    }

    if (auto* comparison = player.getComparisonState())
        comparison->hear (versionID.isNotEmpty());

    history->hearingBounce = versionID.isNotEmpty() && player.getComparisonState() != nullptr;

    if (! isSongPlaying (*song))
        playOrStop (*song); // from where playback starts

    history->tellNow();
    return true;
}

juce::String getHeardVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->hearingBounce ? history->bounceCompared : juce::String();
}

juce::String describeHearing (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (getHeardVersion (context)) : nullptr;
    return version != nullptr ? "Hearing: " + getDisplayName (*version) : juce::String();
}

//==============================================================================
// A version open (docs/design/versions.md: A version open).
namespace
{
    // When a version was saved, as the bar says it: "just now", "5 minutes ago", "3 hours ago", "3 days ago", "on 12 Nov".
    juce::String describeAgo (juce::Time saved, juce::Time now)
    {
        const auto minutes = (now - saved).inMinutes();
        const auto count = [] (juce::int64 number, const char* unit)
        { return juce::String (number) + " " + unit + (number == 1 ? "" : "s") + " ago"; };

        if (minutes < 1.0)
            return "just now";

        if (minutes < 60.0)
            return count ((juce::int64) minutes, "minute");

        if (minutes < 24.0 * 60.0)
            return count ((juce::int64) (minutes / 60.0), "hour");

        if (minutes < 30.0 * 24.0 * 60.0)
            return count ((juce::int64) (minutes / (24.0 * 60.0)), "day");

        return "on " + saved.formatted ("%d %b %Y");
    }

    // Why a version open can't go back to Now yet, put back by anything (Back to Now, a save, a close): none when it can.
    // While a render has the song's plug-ins: what it renders is the version's, and lands in the song as it ends (Bounce in
    // Place, Consolidate). While an automation pass is under way: its points are the version's, and the undos would be
    // recorded into it (AutomationWriter::isPassUnderWay()). While the branch an edit makes waits for Now's piece: Back to Now
    // would take the edit back.
    juce::String whyStaysOpen (const ProjectContext& context, const VersionHistory& history)
    {
        const auto* session = getSession (context);

        if (session != nullptr && session->isLentToRender())
            return "Not while a render has the song: what it renders lands in the version open. Let it finish first.";

        if (history.shown.has_value() && history.shown->branchWaits)
            return "Not now: the branch your change makes is being written. A moment.";

        if (session != nullptr && session->getWriter().isPassUnderWay())
            return "Stop first: automation is being written.";

        return {};
    }

    // Now back, exactly: a trial on top put back, the trial put back (its step undone, the steps it stashed to redo back), what
    // changed meanwhile without an undo step made as Now had it (mute and solo, the loop, view state, a plug-in's settings
    // stored, the count of IDs), what was selected and the time range selected as Now had them, and the plug-ins changed in
    // their own windows meanwhile given Now's settings again. Nothing where it can't go yet (whyStaysOpen()): whatever would
    // write the song refuses then (it is still the version's).
    bool putBackShown (ProjectContext& context, VersionHistory& history)
    {
        auto* song = findSong (context);

        if (! history.shown.has_value() || song == nullptr)
            return true;

        if (whyStaysOpen (context, history).isNotEmpty())
            return false;

        // What isn't an undo step made as Now had it while the song is still shown, so that it is no change of the project's.
        auto shown = std::move (*history.shown);
        history.shown.reset();
        // Now's tree written here, from its state, where the song is made as it was.
        shown.trial->putBackHeld ([song, &shown] { makeSongAsCopied (*song, writeSongTree (shown.now)); });

        // What was selected, and the range: the redo steps undone with the version set the range from before they were done.
        getSelection (context).putBack (shown.selected);
        getRangeSelection (context).set (shown.range, shown.rangeTracks);

        if (auto* session = getSession (context))
        {
            auto given = false;

            for (const auto& [device, count] : song->getDeviceChangesOutsideHistory())
            {
                if (shown.outsideAtOpen[device] != count)
                {
                    session->getPlugins().giveSongState (DeviceID::fromRaw (device));
                    given = true;
                }
            }

            // Their knobs, which the changes read from them, once they have taken it: compared again then.
            if (given)
                session->getPlugins().whenStatesIn (true,
                                                    [&context, number = context.getProjectNumber()] (bool)
                                                    {
                                                        juce::MessageManager::callAsync (
                                                            [&context, number]
                                                            {
                                                                if (auto* h = findHistory (context);
                                                                    h != nullptr && context.getProjectNumber() == number)
                                                                    h->compareAllAgain();
                                                            });
                                                    });
        }

        history.tellNow();
        return true;
    }

    // "Undo twice", "Undo 3 times": how to take so many steps back, in the notice's words.
    juce::String describeUndos (int steps)
    {
        return steps == 1 ? juce::String ("Undo") : steps == 2 ? juce::String ("Undo twice") : "Undo " + juce::String (steps) + " times";
    }

    // Why an edit's branch wasn't made: the history busy, a record or Now's piece that couldn't be written, or its wait for
    // Now's piece stopped (the window's Cancel, or Phazite closing).
    enum class NoBranch
    {
        busy,
        write,
        stopped
    };

    // The branch an edit of a version open makes, now that the hold can end (endOpenVersionForEdit()): the hold ends first, the
    // version's step kept (the song shows it no more), then the branch is made of what it is, which the steps on top of it
    // (the edit) begin, the song growing from that version on it and Now kept on the branch it was on
    // (VersionHistory::branchFromShown()); or, where it can't be made (cannot, or the branch's records), the version is kept as
    // a restore on Now's branch, the edit after it, which Undo takes back, said. With mustBranch (recording, which doesn't
    // start then), the version is kept so too, and said. Either way what it showed is the song's own now, a change of the
    // project's. Whether the branch was made.
    bool makeEditBranch (ProjectContext& context, const SaveProjectThen& saveProjectThen, bool mustBranch, std::optional<NoBranch> cannot)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);

        if (history == nullptr || song == nullptr || ! history->shown.has_value())
            return false;

        const auto versionName = describeVersion (*history, history->shown->version);
        const auto* left = history->findCurrentBranch();
        const auto leftID = left != nullptr ? left->id : juce::String();
        const auto leftName = left != nullptr ? left->name : juce::String();

        auto& trial = *history->shown->trial;
        const auto onTop = trial.countStepsOnTop();
        const auto undosBack = trial.countUndosBack();
        trial.keep ({});

        // What it showed is the song's now: a change of the project's, which its changes meanwhile weren't (Project.h), so that
        // a save in the background under way since before it opened doesn't end with the project marked saved.
        song->markChangedOutsideHistory();

        const auto made = cannot.has_value() ? juce::String() : history->branchFromShown();
        const auto problem = cannot.value_or (history->branchProblem == "write" ? NoBranch::write : NoBranch::busy);
        history->shown.reset(); // its trial closed: nothing more to put back

        if (made.isEmpty())
        {
            // Lossless still: the version is a step on Now's history, the edit after it, which Undo takes back.
            const auto why = problem == NoBranch::write ? juce::String ("Phazite couldn't write into the project's Versions folder, so ")
                             : problem == NoBranch::stopped ? juce::String ("Making the branch was stopped, so ")
                                                            : juce::String ("The project's versions were busy, so ");
            history->noticeTitle = mustBranch ? "Recording didn't start" : "No branch could be made";
            history->noticeText =
                why + versionName + " is now the song on '" + leftName + "'" + (mustBranch ? "." : ", with your change.")
                + (undosBack > 0 ? " " + describeUndos (undosBack) + " takes it back to the song as it was." : juce::String());
            history->noticeBranch.clear();
            history->tellNow();
            return false;
        }

        // The branch's history starts with the edit: nothing under it, Now's steps and the version's, is reachable from it.
        startHistoryAgain (*song, onTop);
        history->shownLast.clear();

        const auto* branch = history->findCurrentBranch();
        history->noticeTitle = "A new branch";
        history->noticeText = "You changed " + versionName + ", so this is now the branch "
                              + (branch != nullptr ? branch->name : juce::String()) + ". '" + leftName + "' keeps the song as you left it.";
        history->noticeBranch = leftID;
        history->tellNow();

        // The project saved after it, as after a switch, so that its file is the branch's song: at the next turn of the message
        // loop, once the step has ended (a gesture's waiting changes run after it).
        if (saveProjectThen != nullptr)
            juce::MessageManager::callAsync (
                [&context, saveProjectThen, number = context.getProjectNumber()]
                {
                    if (auto* open = findSong (context); open != nullptr && context.getProjectNumber() == number)
                        Gesture::whenEnded (*open, &context, [saveProjectThen] { saveProjectThen ([] (bool) {}); });
                });

        return true;
    }

    // What an edit of a version open does as it ends the hold (the trial's edited: the steps on top of it ended with changes;
    // or, with mustBranch, an edit that can't wait, recording, before anything is recorded): the branch (makeEditBranch()).
    // Where Now's piece is still being written on the history's thread (keepShownNow(): seconds for a large song), the branch
    // waits for it: the version stays shown with the edit on top of it and the window held (as a switch holds it), so that
    // nothing else lands meanwhile but what joins the edit, and it is made as it would have been once the piece is in, or the
    // version kept as a restore if the piece couldn't be written or the wait is stopped. Not behind a first save's copy of
    // the folder (minutes): the version kept as a restore at once then, said. Recording waits for nothing: where the branch
    // can't be made at once, nothing records, and why is said, the version still open. Whether it took the edit (made the
    // branch, kept the version as a restore, or waits): false for recording unless the branch was made.
    bool endOpenVersionForEdit (ProjectContext& context, const SaveProjectThen& saveProjectThen, bool mustBranch)
    {
        auto* history = findHistory (context);
        auto* song = findSong (context);

        if (history == nullptr || song == nullptr || ! history->shown.has_value())
            return false;

        auto& shown = *history->shown;
        const auto versionName = describeVersion (*history, shown.version);
        const auto busy =
            history->isSaving() || history->isReadingForCommand() || history->isBranchMissing() || history->findCurrentBranch() == nullptr;
        const auto pieceIn = shown.nowPiece.isNotEmpty();

        if (mustBranch && (shown.branchWaits || busy || ! pieceIn))
        {
            history->noticeTitle = "Recording didn't start";
            history->noticeText = "No branch could be made from " + versionName
                                  + " yet: its versions are busy. Try again in a moment, or go Back to Now to record there.";
            history->noticeBranch.clear();
            history->tellNow();
            return false;
        }

        // Waiting already: what ended since joins the edit.
        if (shown.branchWaits)
            return true;

        if (! busy && ! pieceIn && shown.nowPieceComing && ! history->isCopyingFolder())
        {
            const auto finish = [&context, saveProjectThen, number = shown.number] (std::optional<NoBranch> cannot)
            {
                auto* waiting = findHistory (context);

                if (waiting == nullptr || ! waiting->shown.has_value() || waiting->shown->number != number || ! waiting->shown->branchWaits)
                    return;

                // The wait (and its hold on the window) let go of once the branch is made, as this returns.
                const auto waited = std::exchange (waiting->shown->whenNowIn, {});
                waiting->shown->branchWaits = false;
                makeEditBranch (context, saveProjectThen, false, cannot);
            };

            const std::shared_ptr<ProjectContext::WorkHold> hold =
                context.holdForWork ("Making a branch from " + versionName, [finish] (const juce::String&) { finish (NoBranch::stopped); });
            shown.branchWaits = true;
            shown.whenNowIn = [finish, hold] (bool written)
            { finish (written ? std::nullopt : std::optional<NoBranch> (NoBranch::write)); };
            history->tellNow();
            return true;
        }

        return makeEditBranch (context, saveProjectThen, mustBranch,
                               busy || ! pieceIn ? std::optional<NoBranch> (NoBranch::busy) : std::nullopt)
               || ! mustBranch;
    }

    // The version's song, read and kept (VersionHistory::keepShownSong()), made the song's in place: in a trial that holds the
    // history, Now copied and kept in Local History first; or, from another version open, from it.
    bool showVersion (ProjectContext& context, VersionHistory& history, const juce::String& versionID,
                      const SaveProjectThen& saveProjectThen)
    {
        auto* song = findSong (context);
        const auto* read = history.findShownSong (versionID);
        const auto* version = history.findVersion (versionID);

        if (song == nullptr || read == nullptr || version == nullptr)
            return false;

        // Its step is named for what it is if an edit keeps it where no branch can be made: a restore of the version.
        const auto stepName = "Restore \"" + getDisplayName (*version) + "\"";
        const auto then = read->song; // kept: makeSongLike() copies what it takes

        if (! history.shown.has_value())
        {
            keepSongBefore (history, "look");
            song->storeDeviceStates();
            VersionHistory::Shown shown;
            shown.version = versionID;
            shown.number = ++history.shownFromNow;
            shown.now = getSongState (*song);
            shown.nowParameters = history.readPluginParameters();
            shown.outsideAtOpen = song->getDeviceChangesOutsideHistory();
            shown.mayHaveChanges = history.mayHaveChanges(); // Now's, before the trial changes the song
            shown.selected = getSelection (context).keep();  // and what is selected, before the redo steps done again set any
            shown.range = getRangeSelection (context).getRange();
            shown.rangeTracks = getRangeSelection (context).getTracks();
            shown.trial = std::make_unique<Trial> (*song, stepName, nullptr);
            shown.trial->holdHistory ({ [&context]
                                        {
                                            if (auto* h = findHistory (context))
                                                putBackShown (context, *h);
                                        },
                                        [&context, saveProjectThen] (bool mustBranch)
                                        { return endOpenVersionForEdit (context, saveProjectThen, mustBranch); } });
            history.shown = std::move (shown);
            history.keepShownNow(); // Now's piece, on the history's thread, for a branch an edit may make
            history.shown->trial->apply ([song, &then] { makeSongLike (*song, then); });
        }
        else
        {
            history.shown->version = versionID;
            history.shown->trial->rename (stepName);
            history.shown->trial->apply ([song, &then] { makeSongLike (*song, then); }, true);
        }

        history.shownLast = versionID;
        history.compareProblem.clear();
        history.tellNow();
        return true;
    }
} // namespace

bool canOpenVersion (const ProjectContext& context, const juce::String& versionID)
{
    return whyNotOpenVersion (context, versionID).isEmpty();
}

juce::String whyNotOpenVersion (const ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    const auto* song = findSong (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

    if (history == nullptr || song == nullptr || version == nullptr)
        return "Select a version first.";

    if (history->shown.has_value() && history->shown->version == versionID)
        return "It is open now.";

    // The version Now is, while another is open: Back to Now.
    if (! history->shown.has_value() && isSongNow (*history, versionID))
        return "The song is " + describeVersion (*history, versionID) + " already, with no changes since.";

    if (isRecording (*song))
        return "Not while the song records.";

    // What keeps an open one where it is (a render, a branch being made, automation being written) keeps it from another;
    // and automation being written on Now is Now's: it waits for Stop too.
    if (const auto why = whyStaysOpen (context, *history); why.isNotEmpty())
        return why;

    if (Trial::isTrialOnHeld (*song))
        return "Finish swapping first.";

    if (Trial::isStepOpenOnHeld (*song))
        return "Not during a drag.";

    if (history->isReadingForCommand() || ! canChangeSongNow (context))
        return "Not now: the versions are busy, or a render has the song's plug-ins.";

    return {};
}

bool openVersion (ProjectContext& context, const juce::String& versionID, const SaveProjectThen& saveProjectThen)
{
    auto* history = findHistory (context);

    if (history == nullptr || ! canOpenVersion (context, versionID))
        return false;

    // The version Now is, from another open one: Now itself.
    if (isSongNow (*history, versionID))
    {
        backToNow (context);
        return true;
    }

    if (history->findShownSong (versionID) != nullptr)
        return showVersion (context, *history, versionID, saveProjectThen);

    // Read on the history's thread first, whole (a damaged version can't be opened: Restore This Version takes what it can),
    // then shown.
    const auto* version = history->findVersion (versionID);
    return startSongRead (context, *history, "Reading \"" + getDisplayName (*version) + "\"", version->song, false,
                          [&context, versionID, saveProjectThen] (SongRead& read) -> VersionHistory::Taken
                          {
                              auto* reading = findHistory (context);

                              if (reading == nullptr || reading->findVersion (versionID) == nullptr)
                                  return false;

                              if (! read.song.isValid())
                                  return VersionHistory::Taken::refused (
                                      describeVersion (*reading, versionID)
                                      + " can't be opened: a piece of it is damaged or missing in the Versions folder. Verify "
                                        "History says what; Restore This Version takes what can be read.");

                              if (read.idsGivenAnew)
                                  return VersionHistory::Taken::refused (
                                      idsTwiceRefusal().replace ("Nothing was restored", "It wasn't opened"));

                              if (const auto why = whyNotChangeSongNow (context, "it wasn't opened.", true); why.isNotEmpty())
                                  return VersionHistory::Taken::refused (why);

                              reading->keepShownSong ({ versionID, std::move (read.song), std::move (read.parameters) });
                              return showVersion (context, *reading, versionID, saveProjectThen);
                          });
}

bool backToNow (ProjectContext& context)
{
    auto* history = findHistory (context);
    return history == nullptr || putBackShown (context, *history);
}

bool canGoBackToNow (const ProjectContext& context)
{
    return whyNotBackToNow (context).isEmpty();
}

juce::String describeBackToNow (const ProjectContext& context)
{
    // Its name, or with why it can't while a version is open.
    const auto why = getOpenVersion (context).isNotEmpty() ? whyNotBackToNow (context) : juce::String();
    return why.isEmpty() ? juce::String ("Back to Now")
                         : "Back to Now (" + why.upToFirstOccurrenceOf (":", false, false).trimCharactersAtEnd (".") + ")";
}

juce::String whyNotBackToNow (const ProjectContext& context)
{
    const auto* song = findSong (context);
    const auto* history = findHistory (context);

    if (getOpenVersion (context).isEmpty() || song == nullptr || history == nullptr)
        return "No version is open.";

    if (const auto why = whyStaysOpen (context, *history); why.isNotEmpty())
        return why;

    // A save goes back all the same, putting back what is tried (Trial::putBackHeld()); the button says what to do first.
    if (Trial::isTrialOnHeld (*song))
        return "Finish swapping first.";

    if (Trial::isStepOpenOnHeld (*song))
        return "Not during a drag.";

    return {};
}

juce::String getOpenVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->shown.has_value() ? history->shown->version : juce::String();
}

juce::String describeOpenVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (getOpenVersion (context)) : nullptr;

    if (version == nullptr)
        return {};

    return "Looking at \"" + getDisplayName (*version) + "\" (" + describeAgo (juce::Time (version->saved), juce::Time::getCurrentTime())
           + ")";
}

juce::StringArray getOpenVersionMissingPlugins (const ProjectContext& context)
{
    const auto* song = findSong (context);
    return song != nullptr && getOpenVersion (context).isNotEmpty() ? getPluginsMissing (*song) : juce::StringArray();
}

void openSelectedVersion (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    openVersion (context, getSelectedVersion (context), saveProjectThen);
}

bool canOpenSelected (const ProjectContext& context)
{
    return canOpenVersion (context, getSelectedVersion (context));
}

juce::String describeOpenSelected (const ProjectContext& context)
{
    // Its name, or with why it can't open the version selected.
    const auto why = getSelectedVersion (context).isNotEmpty() ? whyNotOpenVersion (context, getSelectedVersion (context)) : juce::String();
    return why.isEmpty() ? juce::String ("Open Version") : "Open Version (" + why.trimCharactersAtEnd (".") + ")";
}

std::optional<VersionsNotice> takeVersionsNotice (ProjectContext& context)
{
    auto* history = findHistory (context);

    if (history == nullptr || history->noticeText.isEmpty())
        return std::nullopt;

    VersionsNotice notice { std::exchange (history->noticeTitle, {}),
                            std::exchange (history->noticeText, {}),
                            {},
                            std::exchange (history->noticeBranch, {}) };

    for (const auto& each : history->getBranches())
        if (each.id == notice.branch)
            notice.actionName = "Back to " + each.name;

    if (notice.actionName.isEmpty())
        notice.branch.clear();

    return notice;
}

juce::String getCompareVersion (const ProjectContext& context)
{
    // The version open, else the version opened last, else the version whose bounce was heard last, else the one the song
    // grew from.
    const auto* history = findHistory (context);

    if (history == nullptr)
        return {};

    if (history->shown.has_value())
        return history->shown->version;

    for (const auto* id : { &history->shownLast, &history->bounceCompared })
        if (history->findVersion (*id) != nullptr)
            return *id;

    const auto* last = history->findBaseVersion();
    return last != nullptr ? last->id : juce::String();
}

bool canCompare (const ProjectContext& context)
{
    auto* history = findHistory (context);
    const auto compared = getCompareVersion (context);

    if (history == nullptr || compared.isEmpty() || history->isReading())
        return false;

    // Hearing a bounce, or B open: a press goes back to the song, when it can (whyNotBackToNow()).
    if (history->hearingBounce)
        return true;

    if (history->shown.has_value())
        return whyNotBackToNow (context).isEmpty();

    return canOpenVersion (context, compared);
}

void toggleCompare (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    auto* history = findHistory (context);

    if (history == nullptr || ! canCompare (context))
        return;

    history->compareProblem.clear();

    if (history->hearingBounce)
    {
        hearVersion (context, {});
        return;
    }

    if (history->shown.has_value())
    {
        backToNow (context);
        return;
    }

    if (! openVersion (context, getCompareVersion (context), saveProjectThen))
    {
        history->compareProblem = whyNotOpenVersion (context, getCompareVersion (context));
        history->tellNow();
    }
}

juce::String getCompareProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->compareProblem : juce::String();
}

juce::String describeCompare (const ProjectContext& context)
{
    auto* history = findHistory (context);
    const auto compared = getCompareVersion (context);
    const auto* version = history != nullptr ? history->findVersion (compared) : nullptr;

    if (version == nullptr)
        return "A/B: there is no version yet to hear the song against";

    const auto name = "\"" + getDisplayName (*version) + "\"";

    if (history->hearingBounce)
        return "A/B: hearing " + name + "'s bounce (B) in place of the song. Press to hear the song (A) again";

    if (history->shown.has_value())
    {
        if (const auto why = whyNotBackToNow (context); why.isNotEmpty())
            return "A/B: " + name + " (B) is open in place of the song. " + why.trimCharactersAtEnd (".");

        return "A/B: " + name + " (B) is open in place of the song. Press to go back to Now (A), at the same beat";
    }

    if (! history->shown.has_value() && isSongNow (*history, compared))
        return "A/B: nothing to compare. The song is " + name + ", with no changes since";

    return "A/B: open " + name + " (B) in place of the song (A), live, at the same beat; press again to go back to Now";
}

void hearSelectedVersion (ProjectContext& context)
{
    if (canHearSelected (context))
        hearVersion (context, getSelectedVersion (context));
}

bool canHearSelected (const ProjectContext& context)
{
    auto* history = findHistory (context);
    const auto selected = getSelectedVersion (context);

    if (history == nullptr)
        return false;

    // On the changes' row, the song again; on a version, its bounce, which nothing makes by itself.
    return selected.isEmpty() ? history->hearingBounce : history->hasBounce (selected);
}

juce::String describeHearSelected (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto selected = getSelectedVersion (context);
    return history != nullptr && history->findVersion (selected) != nullptr && ! history->hasBounce (selected)
               ? juce::String ("Hear It (No bounce: Open Version plays it live)")
               : juce::String ("Hear It");
}

namespace
{
    // What the bounce commands act on: the version selected, or the current branch's last.
    juce::String getVersionToBounce (const ProjectContext& context)
    {
        const auto selected = getSelectedVersion (context);
        const auto last = getBaseVersion (context);
        return selected.isNotEmpty() ? selected : last.has_value() ? last->id : juce::String();
    }
} // namespace

void bounceSelectedVersion (ProjectContext& context, TrackRenderHost& host)
{
    bounceVersion (context, getVersionToBounce (context), host);
}

bool canBounceSelected (const ProjectContext& context)
{
    return canBounce (context, getVersionToBounce (context));
}

void deleteSelectedBounce (ProjectContext& context)
{
    deleteBounce (context, getSelectedVersion (context));
}

bool canDeleteSelectedBounce (const ProjectContext& context)
{
    const auto selected = getSelectedVersion (context);
    const auto* history = findHistory (context);
    return history != nullptr && (history->hasBounce (selected) || getBounceProgress (context, selected).has_value());
}

//==============================================================================
namespace
{
    // An old version's song exported as a DAWproject: its song read on the history's thread first (known bug 126), what it
    // is called meanwhile; then the song, made for it, kept while it renders; and what its read checks as it comes back.
    class VersionExport final : public TrackRender::Impl
    {
    public:
        explicit VersionExport (juce::String reading)
            : title (std::move (reading))
        {
        }

        void setSong (Song::Ptr s) { song = std::move (s); }
        Song& getSong() const noexcept { return *song; }
        void setRender (std::unique_ptr<TrackRender> r) { render = std::move (r); }
        std::weak_ptr<bool> getAlive() const { return alive; }
        float getProgress() const override { return render != nullptr ? render->getProgress() : 0.0f; }
        juce::String getTitle() const override { return render != nullptr ? render->getTitle() : title; }

    private:
        juce::String title;
        Song::Ptr song;
        std::unique_ptr<TrackRender> render; // goes before the song
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    };

    // The version selected, or none for Now (the song as it is).
    const VersionRecord* findVersionToExport (const ProjectContext& context)
    {
        const auto* history = findHistory (context);
        return history != nullptr ? history->findVersion (getSelectedVersion (context)) : nullptr;
    }
} // namespace

juce::String describeDawProjectExport (const ProjectContext& context)
{
    const auto* version = findVersionToExport (context);
    return version != nullptr ? "Export \"" + getDisplayName (*version) + "\" as DAWproject..." : juce::String ("Export as DAWproject...");
}

bool canExportDawProject (const ProjectContext& context)
{
    const auto* song = findSong (context);
    return song != nullptr && song->getFile() != juce::File() && ! isRecording (*song)
           && (findVersionToExport (context) != nullptr || getSongEnd (*song) > BeatPosition());
}

void exportDawProject (ProjectContext& context, TrackRenderHost& host)
{
    if (! backToNow (context)) // on Now, never on a version open in its place (docs/design/versions.md: A version open)
        return;

    auto* open = findSong (context);

    if (! canExportDawProject (context))
        return;

    const auto* version = findVersionToExport (context);
    const auto project = open->getFile().getFileNameWithoutExtension();
    const auto title = version != nullptr ? "\"" + getDisplayName (*version) + "\"" : project;
    const auto file = open->getFile().getSiblingFile ("Renders").getChildFile (
        makeUsableFileName (version != nullptr ? project + " - " + getDisplayName (*version) : project) + ".dawproject");

    // The version it came from, by which importing it again finds it (step D2).
    const auto comment = "Exported by " + juce::String (appName) + " " + getVersionString()
                         + (version != nullptr ? " from the version \"" + getDisplayName (*version) + "\" (" + version->id + ")."
                                               : juce::String (" from the song as it was."));
    const auto heading = "Exporting " + title + " as DAWproject";
    const auto finished = [&host, heading, file] (const juce::Result& result)
    {
        host.renderFinished (heading, result); // which may delete the render

        if (result.wasOk())
            host.renderWrote (heading, file);
    };

    if (version == nullptr)
    {
        host.runRender (startDawProjectExport (*open, nullptr, file, title, comment, finished));
        return;
    }

    // Its window shows at once, the commit's song read on the history's thread meanwhile; its Cancel stops waiting for it.
    auto exporting = std::make_unique<VersionExport> ("Reading " + title);
    auto* reading = exporting.get();
    findHistory (context)->readCommitInBackground (
        version->id,
        [&context, reading, living = reading->getAlive(), title, comment, file, finished] (const juce::ValueTree& state)
        {
            auto* openNow = findSong (context);

            if (living.expired() || openNow == nullptr)
                return;

            auto song = state.isValid() ? makeSong (state, openNow->getFile(), openNow->getCacheFolder()) : nullptr;

            if (song == nullptr)
            {
                finished (juce::Result::fail ("Phazite couldn't read " + title + ".")); // which deletes the render
                return;
            }

            reading->setSong (song);
            auto render =
                startDawProjectExport (reading->getSong(), &getPluginHost (getEngineContext (context)), file, title, comment, finished);

            if (! living.expired()) // one that failed at once was finished already
                reading->setRender (std::move (render));
        });
    host.runRender (std::make_unique<TrackRender> (std::move (exporting)));
}

void addVersionCommands (CommandRegistry& registry, ProjectContext& context, SaveProjectThen saveProjectThen, TrackRenderHost& renders)
{
    // In plain words (docs/design/versions.md: Decisions); git's words stay as search words, so that "commit" and "checkout" still
    // find them.
    const auto add = [&registry] (Command command)
    {
        command.category = "Versions";
        registry.add (std::move (command));
    };

    add ({ .id = "version.save",
           .name = "Save Version",
           .description = "Saves the project and a version of the song as it is on the current branch, named as typed, to go back "
                          "to, hear and compare later.",
           .synonyms = { "commit", "snapshot", "checkpoint", "history", "version", "git commit" },
           .perform = [&context, saveProjectThen, &renders] { saveProjectAndVersion (context, saveProjectThen, renders); },
           .isEnabled = [&context] { return canSaveVersion (context); },
           .needsPluginStates = true });

    add ({ .id = "version.undoLast",
           .name = "Undo Last Version",
           .description = "Takes the last version off the current branch and leaves the song as it is, so that what it recorded "
                          "shows in the changes again; its name comes back to the field.",
           .synonyms = { "undo commit", "uncommit", "reset", "amend", "git reset" },
           .perform = [&context] { undoLastCommit (context); },
           .isEnabled = [&context] { return canUndoLastCommit (context); } });

    add ({ .id = "version.takeNewer",
           .name = "Take the Newer Versions",
           .description = "Brings in the versions the current branch has that the song didn't grow from (saved on another computer, "
                          "or since the project file was saved), as one undo step: the song becomes the branch's last version, or, "
                          "with changes of its own, the newer versions are merged with them.",
           .synonyms = { "update", "pull", "sync", "merge newer", "newer versions", "git pull" },
           .perform = [&context] { takeNewerVersions (context); },
           .isEnabled = [&context] { return canTakeNewerVersions (context); },
           .currentName = [&context] { return describeTakeNewer (context); },
           .needsPluginStates = true });

    add ({ .id = "version.discardAll",
           .name = "Discard All Changes",
           .description = "Makes the song what the last version has again, as one undo step.",
           .synonyms = { "restore", "revert", "reset", "throw away", "git restore" },
           .perform = [&context] { discardAllChanges (context); },
           .isEnabled = [&context] { return mayHaveChangesSinceLastVersion (context); },
           .needsPluginStates = true });

    add ({ .id = "version.putBack",
           .name = "Discard Changes",
           .description = "Makes the selected line of the changes what the last version has again (while the changes are compared "
                          "with another version, what that version has), as one undo step.",
           .synonyms = { "revert", "restore", "undo change", "git restore" },
           .perform = [&context] { putBackSelectedChange (context); },
           .isEnabled = [&context] { return canPutBack (context); },
           .currentName = [&context] { return describePutBack (context); },
           .needsPluginStates = true });

    // On the version selected in the history; each a change of its own, which names its undo step after what it restores.
    add ({ .id = "version.bringBack",
           .name = "Restore This Version",
           .description = "Makes the whole song what it was in the selected version, as one undo step. The history keeps every "
                          "version, so the changes show what it took back.",
           .synonyms = { "revert", "go back", "roll back", "bring back", "reset", "checkout", "git restore" },
           .perform = [&context] { bringBackSelectedVersion (context); },
           .isEnabled = [&context] { return canBringBack (context); },
           .currentName = [&context] { return describeBringBack (context); },
           .needsPluginStates = true });

    add ({ .id = "version.takeFrom",
           .name = "Compare with the Song",
           .description = "Lists what differs between the selected version and the song now, by track, each to restore from the "
                          "version on its own.",
           .synonyms = { "compare", "diff", "differences", "take from", "working tree" },
           .perform = [&context] { takeFromSelectedVersion (context); },
           .isEnabled = [&context] { return canTakeFrom (context); } });

    add ({ .id = "version.restoreChange",
           .name = "Restore from This Version",
           .description = "Makes the line selected under a version in the history (a track, the master or the song's own) what it "
                          "was in that version, as one undo step.",
           .synonyms = { "restore track", "revert track", "bring back", "take", "cherry-pick", "git restore" },
           .perform = [&context] { restoreSelectedChange (context); },
           .isEnabled = [&context] { return canRestoreSelectedChange (context); },
           .currentName = [&context] { return describeRestoreChange (context); },
           .needsPluginStates = true });

    add ({ .id = "version.showChanges",
           .name = "Close the Comparison",
           .description = "Lists the changes since the last version again, rather than what differs from another version.",
           .synonyms = { "done", "changes", "back" },
           .perform = [&context] { compareWithVersion (context, {}); },
           .isEnabled = [&context] { return getComparedVersion (context).isNotEmpty(); } });

    // Branches and merges: a switch saves the project after it.
    add ({ .id = "version.newBranch",
           .name = "Create Branch",
           .description = "Creates a branch, named at once (idea-2 and on), from the selected version, which the song then becomes, "
                          "or from the current one, the song going on there with its changes. The branch left keeps its own.",
           .synonyms = { "branch", "fork", "idea", "alternative", "new branch" },
           .perform = [&context, saveProjectThen] { newBranchFromSelection (context, saveProjectThen); },
           .isEnabled = [&context] { return canMakeBranch (context); },
           .needsPluginStates = true });

    add ({ .id = "version.switchBranch",
           .name = "Switch to the Selected Branch",
           .description = "Makes the song the branch's selected in the branch picker (Switch Branch...), as it was left there; the "
                          "branch it leaves keeps the song as it is now, its changes too. It goes on playing.",
           .synonyms = { "switch", "branch", "go to", "checkout", "git checkout", "git switch" },
           .perform = [&context, saveProjectThen] { switchToSelectedBranch (context, saveProjectThen); },
           .isEnabled = [&context] { return canSwitchBranch (context); },
           .needsPluginStates = true });

    add ({ .id = "version.merge",
           .name = "Merge into Current Branch...",
           .description = "Lists what the selected version changed since the newest version both branches have, by track, to take "
                          "into the song as one undo step.",
           .synonyms = { "merge", "combine", "join", "pull", "git merge" },
           .perform = [&context] { mergeSelectedVersion (context); },
           .isEnabled = [&context] { return canMerge (context); },
           .currentName = [&context] { return describeMergeInto (context); },
           .needsPluginStates = true });

    add ({ .id = "version.finishMerge",
           .name = "Merge",
           .description = "Takes every line of the merge's list into the song, as one undo step; save a version to finish the merge.",
           .synonyms = { "merge", "apply", "take" },
           .perform = [&context] { finishMerge (context); },
           .isEnabled = [&context] { return canFinishMerge (context); },
           .needsPluginStates = true });

    add ({ .id = "version.cancelMerge",
           .name = "Cancel Merge",
           .description = "Closes the merge's list, taking nothing, or, once the merge was taken and waits for its version, makes "
                          "the song what it was before it, as one undo step.",
           .synonyms = { "abort", "abort merge", "close", "back", "undo merge", "git merge --abort" },
           .perform = [&context] { cancelMerge (context); },
           .isEnabled = [&context] { return canCancelMerge (context); },
           .needsPluginStates = true });

    // A version open in the song's place, live, and back: not undo steps (docs/design/versions.md: A version open). Opening one
    // stores the plug-ins' settings first, which Now keeps.
    add ({ .id = "version.open",
           .name = "Open Version",
           .description = "Opens the selected version in place of the song, live and without a bounce, to listen and look; Back to "
                          "Now brings the song back exactly as it was, and an edit there starts a branch.",
           .synonyms = { "checkout", "check out", "jump to", "view", "look at", "listen", "go to version" },
           .perform = [&context, saveProjectThen] { openSelectedVersion (context, saveProjectThen); },
           .isEnabled = [&context] { return canOpenSelected (context); },
           .currentName = [&context] { return describeOpenSelected (context); },
           .needsPluginStates = true });

    add ({ .id = "version.backToNow",
           .name = "Back to Now",
           .description = "Closes the version open and brings the song back as it was, its unsaved changes, undo steps and "
                          "selection included, at the same beat.",
           .synonyms = { "back", "now", "close version", "return", "a/b" },
           .perform = [&context] { backToNow (context); },
           .isEnabled = [&context] { return canGoBackToNow (context); },
           .currentName = [&context] { return describeBackToNow (context); },
           .overPlugins = true });

    // Bounces, and hearing them: not undo steps, as they change the history and what is heard, not the song.
    add ({ .id = "version.hear",
           .name = "Hear It",
           .description = "Plays the selected version's bounce in place of the song, at the same beat and as loud, to compare them; "
                          "on the changes' row, the song itself again. An edit to the song switches back to it.",
           .synonyms = { "listen", "compare", "a/b", "reference", "play bounce", "play version" },
           .perform = [&context] { hearSelectedVersion (context); },
           .isEnabled = [&context] { return canHearSelected (context); },
           .currentName = [&context] { return describeHearSelected (context); } });

    add ({ .id = "version.compare",
           .name = "A/B Compare",
           .description = "Opens the last version (or the version opened last) in place of the song, live and at the same beat, and "
                          "brings the song back at the next press: what changed since against what was.",
           .synonyms = { "a/b", "ab", "compare", "before and after", "reference", "listen", "stash", "toggle" },
           .perform = [&context, saveProjectThen] { toggleCompare (context, saveProjectThen); },
           .isEnabled = [&context] { return canCompare (context); },
           .isTicked = [&context] { return getHeardVersion (context).isNotEmpty() || getOpenVersion (context).isNotEmpty(); },
           .overPlugins = true,
           .readsPluginStates = [&context] { return getOpenVersion (context).isEmpty() && getHeardVersion (context).isEmpty(); } });

    add ({ .id = "version.bounce",
           .name = "Bounce",
           .description = "Renders the selected version, as Export Song renders the song, to hear it later against the song.",
           .synonyms = { "render", "bounce", "mixdown", "audio" },
           .perform = [&context, &renders] { bounceSelectedVersion (context, renders); },
           .isEnabled = [&context] { return canBounceSelected (context); } });

    add ({ .id = "version.deleteBounce",
           .name = "Delete Bounce",
           .description = "Deletes the selected version's bounce, which nearly all of the room the history takes is. The version "
                          "stays.",
           .synonyms = { "remove", "free space", "clean up", "delete audio" },
           .perform = [&context] { deleteSelectedBounce (context); },
           .isEnabled = [&context] { return canDeleteSelectedBounce (context); } });

    add ({ .id = "version.exportDawProject",
           .name = "Export as DAWproject...",
           .description = "Writes the selected version, or the song as it is, as a DAWproject file, which Bitwig Studio, Studio One "
                          "and Cubase open, into the project's Renders folder. What they can't play is bounced beside its notes.",
           .synonyms = { "dawproject", "bitwig", "studio one", "cubase", "share", "collaborate", "interchange" },
           .perform = [&context, &renders] { exportDawProject (context, renders); },
           .isEnabled = [&context] { return canExportDawProject (context); },
           .currentName = [&context] { return describeDawProjectExport (context); },
           .needsPluginStates = true });

    // The automatic saves (Local History).
    add ({ .id = "version.restoreSnapshot",
           .name = "Restore This Automatic Save",
           .description = "Makes the song what it was at the automatic save selected in the history, as one undo step; the song "
                          "as it is is kept as an automatic save first.",
           .synonyms = { "local history", "auto save", "autosave", "backup", "snapshot", "timeline", "go back", "recover" },
           .perform = [&context] { restoreSelectedSnapshot (context); },
           .isEnabled = [&context] { return canRestoreSelectedSnapshot (context); },
           .currentName = [&context] { return describeRestoreSnapshot (context); },
           .needsPluginStates = true });

    // Keeping the history whole.
    add ({ .id = "version.verify",
           .name = "Verify History",
           .description = "Reads every piece of the project's history and checks it against its name, reads every version's song "
                          "and the songs the automatic saves and the branches keep, checks that every version's audio files are as "
                          "it recorded them, and finds the versions no branch holds. The Versions view says what it found; "
                          "while it runs, the command stops it.",
           .synonyms = { "fsck", "check", "integrity", "damaged", "corrupt", "repair", "missing audio", "cancel" },
           .perform = [&context] { verifyOrStopVerifying (context); },
           .isEnabled = [&context] { return canVerifyHistory (context); },
           .currentName = [&context] { return describeVerify (context); } });

    add ({ .id = "version.resetToReadable",
           .name = "Reset Branch to Last Readable Version",
           .description = "Takes the current branch, whose last version can't be read from the Versions folder, back to the newest "
                          "version of its log that reads, keeping the song as it is.",
           .synonyms = { "damaged", "broken", "missing commit", "repair", "reset", "reflog" },
           .perform = [&context] { resetToReadable (context); },
           .isEnabled = [&context] { return canResetToReadable (context); } });
}
} // namespace phazite
