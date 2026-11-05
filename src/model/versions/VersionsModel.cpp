// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsModel.h"
#include "internal/VersionHistory.h"
#include "model/commands/CommandRegistry.h"
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

std::vector<VersionChange> getChangesSinceLastVersion (const ProjectContext& context)
{
    std::vector<VersionChange> lines;

    if (auto* history = findHistory (context))
    {
        for (const auto& change : history->getChanges())
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
            lines.push_back (std::move (line));
        }
    }

    return lines;
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
}
} // namespace phazite
