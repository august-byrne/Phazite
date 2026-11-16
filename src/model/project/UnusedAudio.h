// SPDX-License-Identifier: AGPL-3.0-or-later
// Clean Up Unused Audio... (docs/design/versions.md: outside the steps): the audio files in the open project's Audio and Generated
// folders that neither its song plays (what is staged for the next commit too), nor its files on the disk (the project file
// as saved, its autosave and its backups, which closing without saving, restoring or opening a backup brings back), nor any
// of its versions (every commit, on a branch or one Recover Lost Commits brings back, the songs left on branches, Local
// History, and the songs before the merges they hold: VersionHistory.h, makeAudioPathsFinder()), to delete those the user
// ticks. Takes, imports and generated sounds pile up there, versions or not. Deleting moves them to the recycle bin, where
// they can still be found: no undo brings them back, and a clip that an undo brings back plays without them. Not an undo
// step. Where a version can't be read whole (another computer's song whose pieces haven't all arrived, a pack another
// program holds), what it plays is unknown, so nothing is offered: never counted as unused.
#pragma once

#include "model/base/BackgroundJobs.h"
#include "model/project/ProjectContext.h"

#include <juce_events/juce_events.h>

#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
struct UnusedAudioFile
{
    juce::File file;
    juce::String name; // its place in the project's folder: "Audio/Take 3.wav"
    juce::int64 size = 0;
};

// Why it can't look now, as a sentence, none while it can: a project is open, it doesn't record (a take being written), its
// versions are read, and no commit or snapshot is being written (which names files the Versions folder doesn't hold yet).
juce::String getWhyNotCleanUpAudio (const ProjectContext&);
bool canCleanUpAudio (const ProjectContext&);

// What a look found (UnusedAudioFinder): the files nothing plays, by their names; or, where a version couldn't be read
// whole, why it can't say (unreadable: none offered then); and what it knew of what the versions play, which a deletion
// checks again (deleteUnusedAudio()).
struct UnusedAudio
{
    std::vector<UnusedAudioFile> files;
    juce::StringArray unreadable;
    juce::StringArray versionPaths;
    juce::String versionsKey;
    int projectNumber = 0;
};

// Moves those of the files given that the look found unused and that are still unused to the recycle bin (or deletes
// them outright: the tests'), and says how many went. What must be fresh is looked at again, cheaply, on the message
// thread: the song and what is staged, the saved files, that nothing is being written, and that the versions name what they
// named at the look (else refused, saying so in refused, to look again).
enum class Removal
{
    toRecycleBin,
    outright
};

int deleteUnusedAudio (ProjectContext&, const UnusedAudio&, const juce::Array<juce::File>&, Removal = Removal::toRecycleBin,
                       juce::String* refused = nullptr);

// The look for the unused files, in the background (BackgroundJobs.h), for what shows them (the Project view's count, the
// Clean Up window): what the song plays taken on the message thread, the rest read on a thread of its own, each saved
// file read again only once it changed and each piece of the versions once (VersionHistory.h). find() looks again (once
// the look under way is done, if one is); onFound is told on the message thread once a look is done, or once it can't
// look (getFound() keeps the last look then); a look begun before the song changed what it plays (a file located, a clip
// added) is looked again rather than shown, as it would count a file now played. A look under way stops at its next step
// as the finder goes. Message thread.
struct UnusedAudioLook;

class UnusedAudioFinder
{
public:
    explicit UnusedAudioFinder (const ProjectContext&);
    ~UnusedAudioFinder();

    void find();
    bool isLooking() const noexcept { return looking; }
    const std::optional<UnusedAudio>& getFound() const noexcept { return found; } // none before the first look
    std::function<void()> onFound;

private:
    const ProjectContext& context;
    BackgroundJobs jobs;
    std::shared_ptr<UnusedAudioLook> saved; // what was read of the saved files, which the looks keep
    std::optional<UnusedAudio> found;
    juce::StringArray lookedFor; // what the song played as the look under way began
    bool looking = false, lookAgain = false;

    void looked (UnusedAudio);

    JUCE_DECLARE_WEAK_REFERENCEABLE (UnusedAudioFinder)
    JUCE_DECLARE_NON_COPYABLE (UnusedAudioFinder)
};
} // namespace phazite
