// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VersionsModel.h"
#include "internal/ApplySong.h"
#include "internal/VersionHistory.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Freeze.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/Export.h"
#include "model/project/internal/Export.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

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

//==============================================================================
bool newBranch (ProjectContext& context, const juce::String& fromVersion, const OpenSong& openSong)
{
    auto* history = findHistory (context);

    if (history == nullptr)
        return false;

    // From Now, the song goes on there as it is; from a version, that version's song opens there.
    if (fromVersion.isEmpty())
        return history->makeBranch ({}, true).isNotEmpty();

    const auto branch = history->makeBranch (fromVersion, false);
    return branch.isNotEmpty() && switchBranch (context, branch, openSong);
}

bool switchBranch (ProjectContext& context, const juce::String& branchID, const OpenSong& openSong)
{
    auto* history = findHistory (context);

    if (history == nullptr || openSong == nullptr)
        return false;

    const auto song = history->leaveFor (branchID);

    if (! song.isValid())
        return false;

    openSong (song); // last: it closes this song, and its history with it
    return true;
}

bool renameBranch (ProjectContext& context, const juce::String& branchID, const juce::String& name)
{
    auto* history = findHistory (context);
    return history != nullptr && history->renameBranch (branchID, name);
}

bool canMakeBranch (const ProjectContext& context)
{
    const auto* history = findHistory (context);
    return history != nullptr && ! history->isReading() && ! history->isSaving()
           && (history->findVersion (history->selectedVersion) != nullptr || history->findLastVersion() != nullptr);
}

void selectBranch (ProjectContext& context, const juce::String& branchID)
{
    if (auto* history = findHistory (context))
        history->selectedBranch = branchID;
}

void newBranchFromSelection (ProjectContext& context, const OpenSong& openSong)
{
    newBranch (context, getSelectedVersion (context), openSong);
}

void switchToSelectedBranch (ProjectContext& context, const OpenSong& openSong)
{
    if (const auto* history = findHistory (context))
        switchBranch (context, history->selectedBranch, openSong);
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
    return "Merge into " + (branch.has_value() ? branch->name : juce::String ("the Current Branch")) + "...";
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
    preview.theirs = history->readSong (versionID);

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
    const auto* version = history->findVersion (merge.version);

    // A change of its own, one undo step: the song made what the merge of its lines makes, its IDs counted on past them.
    song->storeDeviceStates();
    auto result = mergeSongs (merge.base, song->getState(), merge.theirs, toMergeLines (lines), [song] { return song->makeID(); });
    const UndoStep step (*song, "Merge \"" + (version != nullptr ? getDisplayName (*version) : juce::String ("a version")) + "\"");
    makeSongLike (*song, result.song);
    countIDs (*song);

    merge.clashes = std::move (result.clashes);
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
            info.values[0] = clash.baseElement.isValid() ? "as before" : "none";
            info.values[1] = clash.oursElement.isValid() ? "this branch's" : "none";
            info.values[2] = clash.theirsElement.isValid() ? merging.otherBranch + "'s" : "none";
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
    const juce::String whose[] = { "the value before both", "this branch's", merging.otherBranch + "'s" };
    auto* undo = &song->getUndoManager();

    // A change of its own, one undo step: "Take Idea 2's Bass volume".
    const UndoStep step (*song, "Take " + whose[value] + " " + clash.line + " " + clash.what);

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

            ExportSettings settings;
            settings.format = ExportSettings::Format::flac;
            settings.bitDepth = 16;
            settings.sampleRate = getBounceRate (context);

            exporting = startExport (*song, file, settings,
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

bool canBounce (const ProjectContext& context, const juce::String& versionID)
{
    auto* history = findHistory (context);
    auto* song = findSong (context);
    const auto* last = history != nullptr ? history->findLastVersion() : nullptr;
    const auto* session = getSession (context);

    return song != nullptr && last != nullptr && last->id == versionID && ! history->isSaving() && ! history->hasBounce (versionID)
           && ! history->hasChanges() && canExport (*song) && (session == nullptr || ! session->isLentToRender());
}

std::unique_ptr<TrackRender> startBounce (ProjectContext& context, const juce::String& versionID,
                                          std::function<void (const juce::Result&)> onFinished)
{
    if (! canBounce (context, versionID))
        return {};

    auto bounce = std::make_unique<VersionBounce> (context, versionID, getDisplayName (*findHistory (context)->findVersion (versionID)));

    if (! bounce->start (std::move (onFinished)))
        return {};

    return std::make_unique<TrackRender> (std::move (bounce));
}

void bounceVersion (ProjectContext& context, const juce::String& versionID, TrackRenderHost& host)
{
    const auto* history = findHistory (context);
    const auto* version = history != nullptr ? history->findVersion (versionID) : nullptr;
    const auto title = "Bouncing \"" + (version != nullptr ? getDisplayName (*version) : juce::String ("the version")) + "\"";

    if (! canBounce (context, versionID))
    {
        host.renderFinished (title, juce::Result::fail ("Only the version the song is now can be bounced, once."));
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

    if (history == nullptr || song == nullptr || ! history->hasBounce (versionID))
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
    if (auto* history = findHistory (context))
        history->bounceWithVersion = bounce;
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
    const auto* history = findHistory (context);
    return history != nullptr && history->hasBounce (getSelectedVersion (context));
}

void addVersionCommands (CommandRegistry& registry, ProjectContext& context, SaveProjectThen saveProjectThen, OpenSong openSong,
                         TrackRenderHost& renders)
{
    registry.add ({ .id = "version.save",
                    .name = "Save Version",
                    .category = "Project",
                    .description = "Saves the project and keeps the song as it is now as a version, which you can go back to, hear and "
                                   "compare later.",
                    .synonyms = { "commit", "snapshot", "save new version", "history", "checkpoint" },
                    .perform = [&context, saveProjectThen, &renders] { saveProjectAndVersion (context, saveProjectThen, renders); },
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

    // Branches and merges: what opens another branch's song is the app's (openSong).
    registry.add ({ .id = "version.newBranch",
                    .name = "New Branch from Here",
                    .category = "Project",
                    .description = "Starts a branch, named and colored at once: from the selected version, which the song then becomes, or "
                                   "from now, the song going on there with its changes. The branch left keeps its own.",
                    .synonyms = { "branch", "fork", "idea", "alternative", "try something" },
                    .perform = [&context, openSong] { newBranchFromSelection (context, openSong); },
                    .isEnabled = [&context] { return canMakeBranch (context); } });

    registry.add ({ .id = "version.switchBranch",
                    .name = "Switch to the Branch",
                    .category = "Project",
                    .description = "Opens the branch picked in the Versions view as it was left, and keeps the song as it is now on "
                                   "the branch it leaves, its changes too.",
                    .synonyms = { "checkout", "branch", "go to", "open branch" },
                    .perform = [&context, openSong] { switchToSelectedBranch (context, openSong); } });

    registry.add ({ .id = "version.merge",
                    .name = "Merge into the Current Branch...",
                    .category = "Project",
                    .description = "Lists what the selected version changed since the newest version both lines have, by track, to take "
                                   "into the song as one undo step.",
                    .synonyms = { "merge", "combine", "join", "pull" },
                    .perform = [&context] { mergeSelectedVersion (context); },
                    .isEnabled = [&context] { return canMerge (context); },
                    .currentName = [&context] { return describeMergeInto (context); } });

    registry.add ({ .id = "version.finishMerge",
                    .name = "Merge",
                    .category = "Project",
                    .description = "Takes the lines ticked in the merge's list into the song, as one undo step; save a version to finish "
                                   "the merge.",
                    .synonyms = { "merge", "apply", "take" },
                    .perform = [&context] { finishTickedMerge (context); },
                    .isEnabled = [&context] { return ! getMergeLines (context).empty(); } });

    registry.add ({ .id = "version.cancelMerge",
                    .name = "Cancel the Merge",
                    .category = "Project",
                    .description = "Closes the merge's list without taking anything into the song.",
                    .synonyms = { "cancel", "close", "back" },
                    .perform = [&context] { cancelMerge (context); },
                    .isEnabled = [&context] { return ! getMergeLines (context).empty(); } });

    registry.add ({ .id = "version.showChanges",
                    .name = "Show the Changes Since the Last Version",
                    .category = "Project",
                    .description = "Lists the changes since the last version again, rather than what differs from another one.",
                    .synonyms = { "done", "changes", "back" },
                    .perform = [&context] { compareWithVersion (context, {}); },
                    .isEnabled = [&context] { return getComparedVersion (context).isNotEmpty(); } });

    // Bounces, and hearing them: not undo steps, as they change the history and what is heard, not the song.
    registry.add ({ .id = "version.hear",
                    .name = "Hear It",
                    .category = "Project",
                    .description = "Plays the selected version's bounce in place of the song, at the same beat and as loud, to compare "
                                   "them; on Now, the song itself again. An edit to the song switches back to it.",
                    .synonyms = { "listen", "compare", "a/b", "reference", "play version" },
                    .perform = [&context] { hearSelectedVersion (context); },
                    .isEnabled = [&context] { return canHearSelected (context); } });

    registry.add ({ .id = "version.bounce",
                    .name = "Bounce",
                    .category = "Project",
                    .description = "Renders the song into the version it is now, as Export Song renders it, to hear that version later "
                                   "against the song.",
                    .synonyms = { "render", "bounce", "mixdown", "audio" },
                    .perform = [&context, &renders] { bounceSelectedVersion (context, renders); },
                    .isEnabled = [&context] { return canBounceSelected (context); } });

    registry.add ({ .id = "version.deleteBounce",
                    .name = "Delete Bounce",
                    .category = "Project",
                    .description = "Deletes the selected version's bounce, which nearly all of the room the versions take is. The "
                                   "version stays.",
                    .synonyms = { "remove", "free space", "clean up", "delete audio" },
                    .perform = [&context] { deleteSelectedBounce (context); },
                    .isEnabled = [&context] { return canDeleteSelectedBounce (context); } });
}
} // namespace phazite
