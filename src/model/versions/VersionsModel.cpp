// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsModel.h"
#include "internal/ApplySong.h"
#include "internal/VersionHistory.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AppInfo.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/internal/Freeze.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/Export.h"
#include "model/project/internal/DawProject.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"

#include <cmath>
#include <map>
#include <set>

namespace phazite
{
namespace
{
    VersionInfo toInfo (const VersionHistory& history, const VersionRecord& version)
    {
        return { version.id,
                 getDisplayName (version),
                 version.note,
                 version.name.isNotEmpty(),
                 juce::Time (version.saved),
                 version.branch,
                 version.parents,
                 history.hasBounce (version.id) };
    }

    // A plug-in's clash, named by the knobs whose values differ between this side and the other ("Serum settings: Cutoff,
    // Mix"), and by the versions of the plug-in that made each side's state, where they differ.
    void nameKnobs (std::vector<MergeClash>& clashes, const juce::ValueTree& ours, const juce::ValueTree& theirs,
                    const PluginParameterValues& ourValues, const PluginParameterValues& theirValues)
    {
        for (auto& clash : clashes)
        {
            if (clash.property != schema::pluginState)
                continue;

            const auto ourDevice = findByPath (ours, clash.path), theirDevice = findByPath (theirs, clash.path);
            const auto id = readID ((ourDevice.isValid() ? ourDevice : theirDevice)[schema::deviceID]);
            juce::StringArray details;

            if (const auto mine = ourValues.find (id), other = theirValues.find (id); mine != ourValues.end() && other != theirValues.end())
            {
                std::map<juce::String, float> there;

                for (const auto& parameter : other->second)
                    there.emplace (parameter.id, parameter.value);

                juce::StringArray knobs;

                for (const auto& parameter : mine->second)
                    if (const auto found = there.find (parameter.id);
                        found != there.end() && std::abs (found->second - parameter.value) > 0.0005f)
                        knobs.add (parameter.name);

                if (knobs.size() > 3)
                    details.add (knobs[0] + ", " + knobs[1] + " and " + juce::String (knobs.size() - 2) + " more");
                else if (! knobs.isEmpty())
                    details.add (knobs.joinIntoString (", "));
            }

            const auto ourVersion = ourDevice[schema::pluginVersion].toString();
            const auto theirVersion = theirDevice[schema::pluginVersion].toString();

            if (ourVersion.isNotEmpty() && theirVersion.isNotEmpty() && ourVersion != theirVersion)
                details.add ("version " + ourVersion + " here, " + theirVersion + " there");

            if (! details.isEmpty())
                clash.what += ": " + details.joinIntoString ("; ");
        }
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
        if (const auto* branch = history->findCurrentBranch())
            return toInfo (*branch);

    return std::nullopt;
}

std::optional<VersionInfo> getLastVersion (const ProjectContext& context)
{
    if (const auto* history = findHistory (context))
        if (const auto* version = history->findLastVersion())
            return toInfo (*history, *version);

    return std::nullopt;
}

namespace
{
    VersionChange toLine (const SongChange& change, bool staged = false)
    {
        VersionChange line;
        line.staged = staged;
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

    std::vector<VersionChange> toLines (const std::vector<SongChange>& changes, bool staged = false)
    {
        std::vector<VersionChange> lines;

        for (const auto& change : changes)
            lines.push_back (toLine (change, staged));

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

std::vector<VersionChange> getStagedChanges (const ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr ? toLines (history->getStagedChanges(), true) : std::vector<VersionChange>();
}

int countChanges (const ProjectContext& context)
{
    // Each thing once, staged, changed since, or both.
    auto* history = findHistory (context);

    if (history == nullptr)
        return 0;

    std::set<std::pair<int, std::uint64_t>> things;

    for (const auto* list : { &history->getChanges(), &history->getStagedChanges() })
        for (const auto& change : *list)
            things.insert ({ (int) change.where, change.trackID });

    return (int) things.size();
}

bool stageChange (ProjectContext& context, const VersionChange& line)
{
    auto* history = findHistory (context);
    return history != nullptr && history->stage (fromLine (line));
}

bool unstageChange (ProjectContext& context, const VersionChange& line)
{
    auto* history = findHistory (context);
    return history != nullptr && history->unstage (fromLine (line));
}

void stageAllChanges (ProjectContext& context)
{
    if (auto* history = findHistory (context))
        history->stageAll();
}

void unstageAllChanges (ProjectContext& context)
{
    if (auto* history = findHistory (context))
        history->unstageAll();
}

bool discardAllChanges (ProjectContext& context)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->getStagedSong().isValid() || history->getChanges().empty())
        return false;

    // A change of its own, one undo step, as git restore makes the working tree the index again.
    const UndoStep step (*song, "Discard All Changes");
    song->storeDeviceStates();
    makeSongLike (*song, history->getStagedSong());
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
    // Spaces as dashes, and only what a ref can hold (git check-ref-format).
    juce::String name;

    for (const auto c : typed.trim())
    {
        if (juce::CharacterFunctions::isWhitespace (c))
            name << '-';
        else if (c >= 32 && c != 127 && juce::String ("~^:?*[\\").indexOfChar (c) < 0)
            name << juce::String::charToString (c);
    }

    while (name.contains ("--"))
        name = name.replace ("--", "-");

    while (name.contains (".."))
        name = name.replace ("..", ".");

    while (name.contains ("//"))
        name = name.replace ("//", "/");

    name = name.replace ("@{", "@").trimCharactersAtStart ("-./").trimCharactersAtEnd ("./");

    while (name.endsWithIgnoreCase (".lock"))
        name = name.dropLastCharacters (5).trimCharactersAtEnd ("./");

    return name;
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
    auto* history = findHistory (context);

    if (history == nullptr)
    {
        if (done != nullptr)
            done (juce::Result::fail ("This song isn't a project's, so it keeps no commits."));

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
    return history != nullptr && history->isSaving();
}

juce::int64 getVersionsSize (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr ? history->getSize() : 0;
}

bool canSaveVersion (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && ! history->isReading() && ! history->isSaving();
}

void saveProjectAndVersion (ProjectContext& context, const SaveProjectThen& saveProjectThen, TrackRenderHost& renders)
{
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

//==============================================================================
bool bringBackVersion (ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

    if (song == nullptr || version == nullptr)
        return false;

    // As much of it as can be read: what a damaged piece held stays as it is now, and is told (getRestoreProblem()).
    std::vector<MissingPiece> missing;
    const auto then = history->readSong (versionID, nullptr, &missing);
    history->restoreMissing = missing;
    history->tellNow();

    if (! then.isValid())
        return false;

    // A change of its own, one undo step, as git restore --source makes the working tree a commit's. The plug-ins' own
    // settings as they are now, to tell which differ from the commit's: only those are set again.
    const UndoStep step (*song, "Restore from \"" + getDisplayName (*version) + "\"");
    song->storeDeviceStates();
    makeSongLike (*song, then);
    return true;
}

void compareWithVersion (ProjectContext& context, const juce::String& versionID)
{
    if (auto* history = findHistory (context))
        history->compareWith (versionID);
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
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->getComparedSong().isValid())
        return false;

    const auto then = history->getComparedSong();
    const auto* compared = history->findVersion (history->getComparedVersion());

    // A change of its own, one undo step: Discard Changes, from the staged song (git restore); Restore from another commit.
    const UndoStep step (*song, compared != nullptr ? "Restore " + line.name + " from \"" + getDisplayName (*compared) + "\""
                                                    : "Discard Changes in " + line.name);
    song->storeDeviceStates();
    auto done = true;

    if (line.isSong)
        makeSongOwnLike (*song, then);
    else
        done = makeTrackLike (*song, line.track, then);

    removeBrokenReferences (*song);
    history->selectedChange.reset();
    return done;
}

void selectVersion (ProjectContext& context, const juce::String& versionID)
{
    if (auto* history = findHistory (context))
        history->selectedVersion = versionID;
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
        history->selectedChangeIsStaged = line.has_value() && line->staged;
    }
}

std::optional<VersionChange> getSelectedChange (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->selectedChange.has_value()
               ? std::optional<VersionChange> (toLine (*history->selectedChange, history->selectedChangeIsStaged))
               : std::nullopt;
}

juce::String describeBringBack (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (history->selectedVersion) : nullptr;
    return version != nullptr ? "Restore from \"" + getDisplayName (*version) + "\"" : juce::String();
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
    const auto* last = history != nullptr ? history->findLastVersion() : nullptr;
    return canBringBack (context) && (last == nullptr || last->id != history->selectedVersion);
}

bool canPutBack (const ProjectContext& context)
{
    // A line of the working tree's: a staged one is unstaged first, as in git.
    const auto* history = findHistory (context);
    return history != nullptr && history->selectedChange.has_value() && ! history->selectedChangeIsStaged;
}

bool canStageSelected (const ProjectContext& context)
{
    const auto line = getSelectedChange (context);
    return line.has_value() && ! line->staged && getComparedVersion (context).isEmpty();
}

bool canUnstageSelected (const ProjectContext& context)
{
    const auto line = getSelectedChange (context);
    return line.has_value() && line->staged;
}

void stageSelectedChange (ProjectContext& context)
{
    if (const auto line = getSelectedChange (context); line.has_value() && stageChange (context, *line))
        selectChange (context, std::nullopt);
}

void unstageSelectedChange (ProjectContext& context)
{
    if (const auto line = getSelectedChange (context); line.has_value() && unstageChange (context, *line))
        selectChange (context, std::nullopt);
}

bool canStageAll (const ProjectContext& context)
{
    return ! getChangesSinceLastVersion (context).empty() && getComparedVersion (context).isEmpty();
}

bool canUnstageAll (const ProjectContext& context)
{
    return ! getStagedChanges (context).empty();
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
    auto* history = findHistory (context);

    if (history == nullptr)
        return false;

    // From Now, the song goes on there as it is; from a version, the song becomes that version's there, a switch.
    if (fromVersion.isEmpty())
        return history->makeBranch ({}, true).isNotEmpty();

    if (! canSwitchBranch (context))
        return false;

    const auto branch = history->makeBranch (fromVersion, false);
    return branch.isNotEmpty() && switchBranch (context, branch, saveProjectThen);
}

bool canSwitchBranch (const ProjectContext& context)
{
    // Not while the song records (a take not finished would be lost), while a render has its plug-ins, while a version is
    // being saved or the versions read.
    const auto* history = findHistory (context);
    const auto* song = findSong (context);
    const auto* session = getSession (context);
    return history != nullptr && song != nullptr && ! history->isReading() && ! history->isSaving() && ! isRecording (*song)
           && (session == nullptr || ! session->isLentToRender());
}

bool switchBranch (ProjectContext& context, const juce::String& branchID, const SaveProjectThen& saveProjectThen)
{
    auto* history = findHistory (context);

    if (history == nullptr || ! canSwitchBranch (context)
        || ! switchSongInPlace (context, [history, &branchID] { return history->switchTo (branchID); }))
        return false;

    // The project's file the branch's song, as the versions now say it is.
    saveProjectThen ([] (bool) {});
    return true;
}

bool renameBranch (ProjectContext& context, const juce::String& branchID, const juce::String& name)
{
    auto* history = findHistory (context);
    return history != nullptr && history->renameBranch (branchID, name);
}

bool canMakeBranch (const ProjectContext& context)
{
    // From Now, while there is a version; from another version, whenever the song can switch to it.
    const auto* history = findHistory (context);

    if (history == nullptr || history->isReading() || history->isSaving())
        return false;

    return history->findVersion (history->selectedVersion) != nullptr ? canSwitchBranch (context) : history->findLastVersion() != nullptr;
}

void selectBranch (ProjectContext& context, const juce::String& branchID)
{
    if (auto* history = findHistory (context))
        history->selectedBranch = branchID;
}

void newBranchFromSelection (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    newBranch (context, getSelectedVersion (context), saveProjectThen);
}

void switchToSelectedBranch (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    if (const auto* history = findHistory (context))
        switchBranch (context, history->selectedBranch, saveProjectThen);
}

//==============================================================================
namespace
{
    MergeLines toMergeLines (const std::vector<VersionChange>& lines)
    {
        MergeLines merged;

        for (const auto& line : lines)
        {
            if (line.isSong)
                merged.songOwn = true;
            else
                merged.tracks.insert (line.track.getRaw());
        }

        return merged;
    }

    bool isOnLine (const MergeClash& clash, const SongChange& line)
    {
        return line.where == SongChange::Where::song ? clash.trackID == 0 : clash.trackID == line.trackID;
    }
} // namespace

bool canMerge (const ProjectContext& context)
{
    const auto* history = findHistory (context);

    if (history == nullptr || history->isReading() || history->isSaving())
        return false;

    const auto* last = history->findLastVersion();
    const auto& selected = history->selectedVersion;

    // Not the current branch's last version, nor one it follows: what it has already.
    return last != nullptr && history->findVersion (selected) != nullptr && history->findCommonVersion (last->id, selected) != selected;
}

juce::String describeMergeInto (const ProjectContext& context)
{
    const auto branch = getCurrentBranch (context);
    return branch.has_value() ? "Merge into '" + branch->name + "'..." : juce::String ("Merge into Current Branch...");
}

bool startMerge (ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);
    const auto* last = history != nullptr ? history->findLastVersion() : nullptr;
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;

    if (song == nullptr || last == nullptr || version == nullptr)
        return false;

    // From the newest version both lines have, or from nothing for two of no common line.
    VersionHistory::Merge preview;
    preview.version = versionID;
    const auto parted = history->findCommonVersion (last->id, versionID);
    preview.base = parted.isNotEmpty() ? history->readSong (parted) : juce::ValueTree (schema::PHAZITE);
    preview.theirs = history->readSong (versionID, &preview.theirParameters);

    // Numbered as this line, where earlier merges of that line gave what it added IDs anew, its plug-ins' knobs too.
    const auto renumbering = history->findRenumbering (last->id, versionID, parted);
    translateIDs (preview.base, renumbering);
    translateIDs (preview.theirs, renumbering);

    for (const auto& id : renumbering)
    {
        if (const auto knobs = preview.theirParameters.find (id.from); id.clip.isEmpty() && knobs != preview.theirParameters.end())
        {
            auto values = std::move (knobs->second);
            preview.theirParameters.erase (knobs);
            preview.theirParameters[id.to] = std::move (values);
        }
    }

    for (const auto& branch : history->getBranches())
        if (branch.id == version->branch)
            preview.otherBranch = branch.name;

    if (! preview.base.isValid() || ! preview.theirs.isValid())
        return false;

    // What it changed, by line, and what of it clashes with what the song changed: a merge of every line, tried on copies.
    preview.lines = findChanges (preview.base, preview.theirs);
    auto next = juce::jmax (findNextID (song->getState()), findNextID (preview.theirs));
    MergeLines everything;

    for (const auto& line : preview.lines)
    {
        if (line.where == SongChange::Where::song)
            everything.songOwn = true;
        else
            everything.tracks.insert (line.trackID);
    }

    preview.clashes = mergeSongs (preview.base, song->getState(), preview.theirs, everything, [&next] { return next++; }).clashes;
    nameKnobs (preview.clashes, song->getState(), preview.theirs, history->readPluginParameters(), preview.theirParameters);
    history->ticked.assign (preview.lines.size(), true);
    history->preview = std::move (preview);
    history->tellNow();
    return true;
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
            const auto index = lines.size();
            lines.push_back ({ toLine (line), clashes, index < history->ticked.size() && history->ticked[index] });
        }
    }

    return lines;
}

juce::String describeMerge (const ProjectContext& context)
{
    const auto* history = findHistory (context);

    if (history == nullptr)
        return {};

    const auto& merge = history->preview.has_value() ? history->preview : history->merging;
    const auto* version = merge.has_value() ? history->findVersion (merge->version) : nullptr;
    return version != nullptr ? getDisplayName (*version) : juce::String();
}

juce::StringArray getMergeNotes (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->merging.has_value() ? history->merging->notes : juce::StringArray();
}

void cancelMerge (ProjectContext& context)
{
    if (auto* history = findHistory (context); history != nullptr && history->preview.has_value())
    {
        history->preview.reset();
        history->tellNow();
    }
}

bool finishMerge (ProjectContext& context, const std::vector<VersionChange>& lines)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->preview.has_value())
        return false;

    auto merge = std::move (*history->preview);
    history->preview.reset();
    // A change of its own, one undo step named as git names a merge ("Merge branch 'idea-2'"): the song made what the merge
    // of its lines makes, its IDs counted on past them.
    song->storeDeviceStates();
    auto result = mergeSongs (merge.base, song->getState(), merge.theirs, toMergeLines (lines), [song] { return song->makeID(); });
    const UndoStep step (*song, "Merge " + history->describeCommit (merge.version));
    makeSongLike (*song, result.song);
    countIDs (*song);

    merge.clashes = std::move (result.clashes);
    merge.renumbered = std::move (result.renumbered);
    nameKnobs (merge.clashes, song->getState(), merge.theirs, history->readPluginParameters(), merge.theirParameters);
    merge.chosen.assign (merge.clashes.size(), 1);
    merge.notes = result.notes;
    history->merging = std::move (merge);
    history->tellNow();
    return true;
}

bool isMerging (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->merging.has_value();
}

std::vector<ClashInfo> getClashes (const ProjectContext& context)
{
    std::vector<ClashInfo> infos;
    const auto* history = findHistory (context);

    if (history == nullptr || ! history->merging.has_value())
        return infos;

    const auto& merging = *history->merging;

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

bool chooseClash (ProjectContext& context, int index, int value)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);

    if (song == nullptr || history == nullptr || ! history->merging.has_value() || ! juce::isPositiveAndBelow (value, 3)
        || ! juce::isPositiveAndBelow (index, (int) history->merging->clashes.size()))
        return false;

    auto& merging = *history->merging;
    const auto& clash = merging.clashes[(size_t) index];
    auto* undo = &song->getUndoManager();

    // A change of its own, one undo step, as a merge editor names its choices: "Accept Incoming Change: Bass volume".
    const UndoStep step (*song, getClashChoiceName (value) + ": " + clash.line + " " + clash.what);

    if (clash.property.isValid())
    {
        auto element = findByPath (song->getState(), clash.path);

        if (! element.isValid())
            return false;

        const juce::var* values[] = { &clash.base, &clash.ours, &clash.theirs };

        if (values[value]->isVoid())
            element.removeProperty (clash.property, undo);
        else
            element.setProperty (clash.property, *values[value], undo);
    }
    else
    {
        auto parentPath = clash.path;
        parentPath.remove (parentPath.size() - 1);
        auto parent = findByPath (song->getState(), parentPath);

        if (! parent.isValid())
            return false;

        const auto existing = findByPath (song->getState(), clash.path);
        const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
        const auto place = existing.isValid() ? parent.indexOf (existing) : -1;

        if (existing.isValid())
            parent.removeChild (existing, undo);

        if (elements[value]->isValid())
            parent.addChild (elements[value]->createCopy(), place, undo);
    }

    merging.chosen[(size_t) index] = value;
    history->tellNow();
    return true;
}

juce::String getClashChoiceName (int value)
{
    return value == 0 ? "Accept Base" : value == 1 ? "Accept Current Change" : "Accept Incoming Change";
}

void mergeSelectedVersion (ProjectContext& context)
{
    startMerge (context, getSelectedVersion (context));
}

void tickMergeLine (ProjectContext& context, int line, bool ticked)
{
    if (auto* history = findHistory (context); history != nullptr && juce::isPositiveAndBelow (line, (int) history->ticked.size()))
    {
        history->ticked[(size_t) line] = ticked;
        history->tellNow();
    }
}

void finishTickedMerge (ProjectContext& context)
{
    std::vector<VersionChange> ticked;

    for (const auto& line : getMergeLines (context))
        if (line.ticked)
            ticked.push_back (line.change);

    finishMerge (context, ticked);
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
        const auto* last = history.findLastVersion();
        return last != nullptr && last->id == versionID && ! history.hasChanges();
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
                return juce::Result::fail ("That commit isn't there any more.");

            title = "Bouncing \"" + getDisplayName (*version) + "\"";
            const auto state = history->readSong (versionID);
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
        Song::Ptr song;                        // the version's, alone
        std::unique_ptr<SongExport> exporting; // goes before the song it renders

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
                track = findTrackState (song, TrackID::fromRaw ((std::uint64_t) key.getLargeIntValue())).isValid()
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

bool verifyHistory (ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr && history->checkHistory();
}

bool canVerifyHistory (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->keepsVersions() && ! history->isReading() && ! history->isSaving() && ! history->isChecking();
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
        return "Verifying the history...";

    const auto& check = history->getLastCheck();

    if (! check.has_value())
        return {};

    juce::StringArray parts;

    if (check->damagedPieces.isEmpty() && check->damaged.empty())
        parts.add ("Verified " + count ((size_t) check->numVersions, "commit", "commits") + " and "
                   + count ((size_t) check->numPieces, "piece", "pieces") + ": all whole.");

    if (! check->damagedPieces.isEmpty())
        parts.add (count ((size_t) check->damagedPieces.size(), "piece is", "pieces are") + " damaged.");

    if (! check->damaged.empty())
    {
        juce::StringArray names;

        for (const auto& [id, missing] : check->damaged)
            names.add (describeVersion (*history, id) + " (" + describeMissing (*song, missing.front())
                       + (missing.size() > 1 ? " and " + juce::String ((juce::int64) missing.size() - 1) + " more" : juce::String()) + ")");

        parts.add (count (check->damaged.size(), "commit can't", "commits can't") + " be read whole: " + names.joinIntoString (", ")
                   + ". Restore from This Commit takes what can be.");
    }

    if (! check->lost.empty())
        parts.add (count (check->lost.size(), "commit", "commits") + " no branch holds: Recover Lost Commits makes a branch of each line.");

    return parts.joinIntoString (" ");
}

int countLostVersions (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && history->getLastCheck().has_value() ? (int) history->getLastCheck()->lost.size() : 0;
}

int recoverLostVersions (ProjectContext& context)
{
    auto* history = findHistory (context);
    return history != nullptr ? history->recoverLostVersions() : 0;
}

juce::String getRestoreProblem (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* song = findSong (context);

    if (history == nullptr || song == nullptr || history->restoreMissing.empty())
        return {};

    const auto& missing = history->restoreMissing;

    if (missing.size() == 1 && missing.front().path.isEmpty())
        return "That commit can't be read: Verify History says what is damaged.";

    juce::StringArray what;

    for (const auto& piece : missing)
        what.addIfNotAlreadyThere (describeMissing (*song, piece));

    return "Restored but for what the commit's damaged pieces held, which stayed as it was: " + what.joinIntoString (", ") + ".";
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
    auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;
    const auto title = "Bouncing \"" + (version != nullptr ? getDisplayName (*version) : juce::String ("the commit")) + "\"";

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
        const auto then = history->readSong (versionID);
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

void hearSelectedVersion (ProjectContext& context)
{
    hearVersion (context, getSelectedVersion (context));
}

bool canHearSelected (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto selected = getSelectedVersion (context);
    return history != nullptr && (selected.isEmpty() ? history->hearingBounce : history->hasBounce (selected));
}

namespace
{
    // What the bounce commands act on: the version selected, or the current branch's last.
    juce::String getVersionToBounce (const ProjectContext& context)
    {
        const auto selected = getSelectedVersion (context);
        const auto last = getLastVersion (context);
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
    // An old version's song exported as a DAWproject: the song, made for it, kept while it renders.
    class VersionExport final : public TrackRender::Impl
    {
    public:
        explicit VersionExport (Song::Ptr s)
            : song (std::move (s))
        {
        }

        Song& getSong() const noexcept { return *song; }
        void setRender (std::unique_ptr<TrackRender> r) { render = std::move (r); }
        float getProgress() const override { return render != nullptr ? render->getProgress() : 0.0f; }
        juce::String getTitle() const override { return render != nullptr ? render->getTitle() : juce::String(); }

    private:
        Song::Ptr song;
        std::unique_ptr<TrackRender> render; // goes before the song
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
    auto* open = findSong (context);

    if (! canExportDawProject (context))
        return;

    const auto* version = findVersionToExport (context);
    const auto project = open->getFile().getFileNameWithoutExtension();
    const auto title = version != nullptr ? "\"" + getDisplayName (*version) + "\"" : project;
    const auto file = open->getFile().getSiblingFile ("Renders").getChildFile (
        juce::File::createLegalFileName (version != nullptr ? project + " - " + getDisplayName (*version) : project) + ".dawproject");

    // The version it came from, by which importing it again finds it (step D2).
    const auto comment = "Exported by " + juce::String (appName) + " " + getVersionString()
                         + (version != nullptr ? " from the commit \"" + getDisplayName (*version) + "\" (" + version->id + ")."
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

    const auto state = findHistory (context)->readSong (version->id);
    auto song = state.isValid() ? makeSong (state, open->getFile(), open->getCacheFolder()) : nullptr;

    if (song == nullptr)
    {
        host.renderFinished (heading, juce::Result::fail ("Phazite couldn't read " + title + "."));
        return;
    }

    auto exporting = std::make_unique<VersionExport> (song);
    exporting->setRender (
        startDawProjectExport (exporting->getSong(), &getPluginHost (getEngineContext (context)), file, title, comment, finished));
    host.runRender (std::make_unique<TrackRender> (std::move (exporting)));
}

void addVersionCommands (CommandRegistry& registry, ProjectContext& context, SaveProjectThen saveProjectThen, TrackRenderHost& renders)
{
    // In git's words (docs/versions.md: Words), as VS Code's Source Control names them.
    const auto add = [&registry] (Command command)
    {
        command.category = "Source Control";
        registry.add (std::move (command));
    };

    add ({ .id = "version.save",
           .name = "Commit",
           .description = "Saves the project and commits the staged changes to the current branch, or every change while none is "
                          "staged, with the message typed, to go back to, hear and compare later.",
           .synonyms = { "save version", "snapshot", "checkpoint", "history", "version" },
           .perform = [&context, saveProjectThen, &renders] { saveProjectAndVersion (context, saveProjectThen, renders); },
           .isEnabled = [&context] { return canSaveVersion (context); } });

    // Staging, as git add and git restore --staged: on the line selected in the changes, or on every line.
    add ({ .id = "version.stage",
           .name = "Stage Changes",
           .description = "Stages the selected line of the changes, as it is now, for the next commit.",
           .synonyms = { "add", "git add", "index" },
           .perform = [&context] { stageSelectedChange (context); },
           .isEnabled = [&context] { return canStageSelected (context); } });

    add ({ .id = "version.unstage",
           .name = "Unstage Changes",
           .description = "Takes the selected line of the staged changes out of the next commit; the song keeps it as it is.",
           .synonyms = { "reset", "restore staged", "index" },
           .perform = [&context] { unstageSelectedChange (context); },
           .isEnabled = [&context] { return canUnstageSelected (context); } });

    add ({ .id = "version.stageAll",
           .name = "Stage All Changes",
           .description = "Stages every change, as the song is now, for the next commit.",
           .synonyms = { "add all", "git add", "index" },
           .perform = [&context] { stageAllChanges (context); },
           .isEnabled = [&context] { return canStageAll (context); } });

    add ({ .id = "version.unstageAll",
           .name = "Unstage All Changes",
           .description = "Takes every staged change out of the next commit; the song keeps them as they are.",
           .synonyms = { "reset", "restore staged", "index" },
           .perform = [&context] { unstageAllChanges (context); },
           .isEnabled = [&context] { return canUnstageAll (context); } });

    add ({ .id = "version.discardAll",
           .name = "Discard All Changes",
           .description = "Makes the song what is staged again, or the last commit while nothing is, as one undo step.",
           .synonyms = { "restore", "revert", "reset", "throw away", "git restore" },
           .perform = [&context] { discardAllChanges (context); },
           .isEnabled = [&context] { return ! getChangesSinceLastVersion (context).empty(); } });

    add ({ .id = "version.putBack",
           .name = "Discard Changes",
           .description = "Makes the selected line of the changes what is staged again, or the last commit while nothing is (while "
                          "the changes are compared with another commit, what that commit had), as one undo step.",
           .synonyms = { "revert", "restore", "undo change", "git restore" },
           .perform = [&context] { putBackSelectedChange (context); },
           .isEnabled = [&context] { return canPutBack (context); },
           .currentName = [&context] { return describePutBack (context); } });

    // On the commit selected in the graph; each a change of its own, which names its undo step after what it restores.
    add ({ .id = "version.bringBack",
           .name = "Restore from This Commit",
           .description = "Makes the song what it was in the selected commit, as one undo step, as git restore --source does. "
                          "The history keeps every commit, so the changes show what it took back.",
           .synonyms = { "revert", "go back", "checkout", "roll back", "bring back" },
           .perform = [&context] { bringBackSelectedVersion (context); },
           .isEnabled = [&context] { return canBringBack (context); },
           .currentName = [&context] { return describeBringBack (context); } });

    add ({ .id = "version.takeFrom",
           .name = "Compare with Working Tree",
           .description = "Lists what differs between the selected commit and the song now, by track, each to restore from the "
                          "commit on its own.",
           .synonyms = { "compare", "diff", "cherry-pick", "differences", "take from" },
           .perform = [&context] { takeFromSelectedVersion (context); },
           .isEnabled = [&context] { return canTakeFrom (context); } });

    add ({ .id = "version.showChanges",
           .name = "Close the Comparison",
           .description = "Lists the changes since the last commit again, rather than what differs from another commit.",
           .synonyms = { "done", "changes", "back" },
           .perform = [&context] { compareWithVersion (context, {}); },
           .isEnabled = [&context] { return getComparedVersion (context).isNotEmpty(); } });

    // Branches and merges: a checkout saves the project after it.
    add ({ .id = "version.newBranch",
           .name = "Create Branch",
           .description = "Creates a branch, named at once (idea-2 and on), from the selected commit, which the song then becomes, "
                          "or from the current one, the song going on there with its changes. The branch left keeps its own.",
           .synonyms = { "branch", "fork", "idea", "alternative", "new branch" },
           .perform = [&context, saveProjectThen] { newBranchFromSelection (context, saveProjectThen); },
           .isEnabled = [&context] { return canMakeBranch (context); } });

    add ({ .id = "version.switchBranch",
           .name = "Checkout Branch",
           .description = "Checks out the branch picked: the song becomes that branch's, as it was left there, and the branch it "
                          "leaves keeps it as it is now, its changes too. It goes on playing.",
           .synonyms = { "switch", "branch", "go to", "git checkout", "git switch" },
           .perform = [&context, saveProjectThen] { switchToSelectedBranch (context, saveProjectThen); },
           .isEnabled = [&context] { return canSwitchBranch (context); } });

    add ({ .id = "version.merge",
           .name = "Merge into Current Branch...",
           .description = "Lists what the selected commit changed since the newest commit both lines have, by track, to take into "
                          "the song as one undo step.",
           .synonyms = { "merge", "combine", "join", "pull", "git merge" },
           .perform = [&context] { mergeSelectedVersion (context); },
           .isEnabled = [&context] { return canMerge (context); },
           .currentName = [&context] { return describeMergeInto (context); } });

    add ({ .id = "version.finishMerge",
           .name = "Merge",
           .description = "Takes the lines ticked in the merge's list into the song, as one undo step; commit to finish the merge.",
           .synonyms = { "merge", "apply", "take" },
           .perform = [&context] { finishTickedMerge (context); },
           .isEnabled = [&context] { return ! getMergeLines (context).empty(); } });

    add ({ .id = "version.cancelMerge",
           .name = "Abort Merge",
           .description = "Closes the merge's list without taking anything into the song, as git merge --abort.",
           .synonyms = { "cancel", "close", "back", "abort" },
           .perform = [&context] { cancelMerge (context); },
           .isEnabled = [&context] { return ! getMergeLines (context).empty(); } });

    // Bounces, and hearing them: not undo steps, as they change the history and what is heard, not the song.
    add ({ .id = "version.hear",
           .name = "Hear It",
           .description = "Plays the selected commit's bounce in place of the song, at the same beat and as loud, to compare them; "
                          "on the uncommitted changes, the song itself again. An edit to the song switches back to it.",
           .synonyms = { "listen", "compare", "a/b", "reference", "play commit" },
           .perform = [&context] { hearSelectedVersion (context); },
           .isEnabled = [&context] { return canHearSelected (context); } });

    add ({ .id = "version.bounce",
           .name = "Bounce",
           .description = "Renders the selected commit, as Export Song renders the song, to hear it later against the song.",
           .synonyms = { "render", "bounce", "mixdown", "audio" },
           .perform = [&context, &renders] { bounceSelectedVersion (context, renders); },
           .isEnabled = [&context] { return canBounceSelected (context); } });

    add ({ .id = "version.deleteBounce",
           .name = "Delete Bounce",
           .description = "Deletes the selected commit's bounce, which nearly all of the room the history takes is. The commit "
                          "stays.",
           .synonyms = { "remove", "free space", "clean up", "delete audio" },
           .perform = [&context] { deleteSelectedBounce (context); },
           .isEnabled = [&context] { return canDeleteSelectedBounce (context); } });

    add ({ .id = "version.exportDawProject",
           .name = "Export as DAWproject...",
           .description = "Writes the selected commit, or the song as it is, as a DAWproject file, which Bitwig Studio, Studio One "
                          "and Cubase open, into the project's Renders folder. What they can't play is bounced beside its notes.",
           .synonyms = { "dawproject", "bitwig", "studio one", "cubase", "share", "collaborate", "interchange" },
           .perform = [&context, &renders] { exportDawProject (context, renders); },
           .isEnabled = [&context] { return canExportDawProject (context); },
           .currentName = [&context] { return describeDawProjectExport (context); } });

    // Keeping the history whole.
    add ({ .id = "version.verify",
           .name = "Verify History",
           .description = "Reads every piece of the project's history and checks it against its name, walks every commit's song, "
                          "and finds the commits no branch holds, as git fsck does. The Versions view says what it found.",
           .synonyms = { "fsck", "check", "integrity", "damaged", "corrupt", "repair" },
           .perform = [&context] { verifyHistory (context); },
           .isEnabled = [&context] { return canVerifyHistory (context); } });

    add ({ .id = "version.recover",
           .name = "Recover Lost Commits",
           .description = "Makes a branch of each line of the commits Verify History found that no branch holds, named after "
                          "the branch they were made on.",
           .synonyms = { "lost", "dangling", "orphaned", "reflog", "find commits" },
           .perform = [&context] { recoverLostVersions (context); },
           .isEnabled = [&context] { return countLostVersions (context) > 0; } });
}
} // namespace phazite
