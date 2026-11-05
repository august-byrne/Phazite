// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsModel.h"
#include "internal/ApplySong.h"
#include "internal/VersionHistory.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
namespace
{
    VersionInfo toInfo (const VersionRecord& version)
    {
        return { version.id,     getDisplayName (version), version.note, version.name.isNotEmpty(), juce::Time (version.saved),
                 version.branch, version.parents };
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

std::vector<VersionInfo> getVersions (const ProjectContext& context)
{
    std::vector<VersionInfo> infos;

    if (const auto* history = findHistory (context))
        for (const auto* version : history->getVersions())
            infos.push_back (toInfo (*version));

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
            return toInfo (*version);

    return std::nullopt;
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

void saveProjectAndVersion (ProjectContext& context, const SaveProjectThen& saveProjectThen)
{
    // The project first, which its first save puts where the user says (its versions go beside it there), then the version.
    saveProjectThen (
        [&context] (bool saved)
        {
            if (saved)
                saveVersion (context);
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

    const auto then = history->readSong (versionID);

    if (! then.isValid())
        return false;

    // A change of its own, one undo step. The plug-ins' own settings as they are now, to tell which differ from the
    // version's: only those are set again.
    const UndoStep step (*song, "Bring Back \"" + getDisplayName (*version) + "\"");
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

    // A change of its own, one undo step: Put Back from the last version, Take from another.
    const UndoStep step (*song, compared != nullptr ? "Take " + line.name + " from \"" + getDisplayName (*compared) + "\""
                                                    : "Put Back " + line.name);
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
        history->selectedChange = line.has_value() ? std::optional<SongChange> (fromLine (*line)) : std::nullopt;
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
    return version != nullptr ? "Bring Back \"" + getDisplayName (*version) + "\"" : juce::String();
}

juce::String describePutBack (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    const auto* compared = history != nullptr ? history->findVersion (history->getComparedVersion()) : nullptr;
    return compared != nullptr ? "Take from \"" + getDisplayName (*compared) + "\"" : juce::String();
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

void addVersionCommands (CommandRegistry& registry, ProjectContext& context, SaveProjectThen saveProjectThen)
{
    registry.add ({ .id = "version.save",
                    .name = "Save Version",
                    .category = "Project",
                    .description = "Saves the project and keeps the song as it is now as a version, which you can go back to, hear and "
                                   "compare later.",
                    .synonyms = { "commit", "snapshot", "save new version", "history", "checkpoint" },
                    .perform = [&context, saveProjectThen] { saveProjectAndVersion (context, saveProjectThen); },
                    .isEnabled = [&context] { return canSaveVersion (context); } });

    // On the version selected in the Versions view's history, and the line of the changes selected there; each a change of
    // its own, which names its undo step after what it brings back.
    registry.add ({ .id = "version.bringBack",
                    .name = "Bring Back This Version",
                    .category = "Project",
                    .description = "Makes the song what it was in the selected version, as one undo step. The history keeps every "
                                   "version, so the changes since show what it took back.",
                    .synonyms = { "restore", "revert", "go back", "checkout", "roll back" },
                    .perform = [&context] { bringBackSelectedVersion (context); },
                    .isEnabled = [&context] { return canBringBack (context); },
                    .currentName = [&context] { return describeBringBack (context); } });

    registry.add ({ .id = "version.takeFrom",
                    .name = "Take from This Version...",
                    .category = "Project",
                    .description = "Lists what differs between the selected version and the song now, by track, each to take back "
                                   "from the version on its own.",
                    .synonyms = { "compare", "diff", "cherry-pick", "take", "differences" },
                    .perform = [&context] { takeFromSelectedVersion (context); },
                    .isEnabled = [&context] { return canTakeFrom (context); } });

    registry.add ({ .id = "version.putBack",
                    .name = "Put Back",
                    .category = "Project",
                    .description = "Takes the selected line of the changes back from the last version (or from the version the "
                                   "changes are listed against), as one undo step.",
                    .synonyms = { "revert", "undo change", "restore", "take" },
                    .perform = [&context] { putBackSelectedChange (context); },
                    .isEnabled = [&context] { return canPutBack (context); },
                    .currentName = [&context] { return describePutBack (context); } });

    registry.add ({ .id = "version.showChanges",
                    .name = "Show the Changes Since the Last Version",
                    .category = "Project",
                    .description = "Lists the changes since the last version again, rather than what differs from another one.",
                    .synonyms = { "done", "changes", "back" },
                    .perform = [&context] { compareWithVersion (context, {}); },
                    .isEnabled = [&context] { return getComparedVersion (context).isNotEmpty(); } });
}
} // namespace phazite
