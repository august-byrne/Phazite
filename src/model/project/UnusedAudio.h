// SPDX-License-Identifier: AGPL-3.0-or-later
// Clean Up Unused Audio... (docs/design/versions.md: outside the steps): the audio files in the open project's Audio and Generated
// folders that neither its song plays, nor its files on the disk (the project file
// as saved, its autosave and its backups, which closing without saving, restoring or opening a backup brings back), nor any
// of its versions (every version the store still holds, on a branch or one Recover Lost Versions brings back; the songs
// left on branches, Local History as thinned, the songs left on branches deleted within 30 days, and the songs before the
// merges they hold: VersionHistory.h, makeAudioPathsFinder()), to delete those the user ticks. It goes by what the store
// still holds rather than by the versions' 30 days, so that it keeps more than Recover Lost Versions can bring back, never
// less: a version's audio is offered once the store's sweep let the version go (StoreSweep.h). Takes, imports and generated sounds pile up there, versions or not. Deleting moves them to the recycle bin, where
// they can still be found: no undo brings them back, and a clip that an undo brings back plays without them. Not an undo
// step. Where a version can't be read whole (another computer's song whose pieces haven't all arrived, a pack another
// program holds), what it plays is unknown, so nothing is offered: never counted as unused.
#pragma once

#include "model/base/BackgroundJobs.h"
#include "model/project/ProjectContext.h"

#include <juce_events/juce_events.h>

#include <functional>
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
// versions are read, no version or snapshot is being written (which names files the Versions folder doesn't hold yet), and
// no audio file's work is under way (an import's copy is in the Audio folder before a clip plays it: AudioWork.h).
juce::String getWhyNotCleanUpAudio (const ProjectContext&);
bool canCleanUpAudio (const ProjectContext&);

// What a look found (UnusedAudioFinder): the files nothing plays, by their names; or, where a version couldn't be read
// whole, why it can't say (unreadable: none offered then); and what it knew of what the versions play, which a deletion
// checks again (UnusedAudioFinder::deleteUnused()).
struct UnusedAudio
{
    std::vector<UnusedAudioFile> files;
    juce::StringArray unreadable;
    juce::StringArray versionPaths;
    juce::String versionsKey;
    int projectNumber = 0;
};

// Deleting moves the files to the recycle bin, or (the tests') deletes them outright.
enum class Removal
{
    toRecycleBin,
    outright
};

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

    // Whether the last find() was refused (getWhyNotCleanUpAudio()) and it could look now: what shows the unused files asks
    // this at its ticks (cheap: no disk), and finds again, so that a look refused while the versions were read, or written,
    // or an import ran, isn't left at what it knew before (another project's look, which a refusal forgets).
    bool shouldLookAgain() const { return refused && ! looking && canCleanUpAudio (context); }
    const std::optional<UnusedAudio>& getFound() const noexcept { return found; } // none before the first look
    std::function<void()> onFound;

    // Deletes those of the files given that the last look found unused (getFound()) and that are still unused, on the
    // finder's thread, one at a time, the window held meanwhile (ProjectContext::holdForWork(): a progress window after a
    // second, whose Cancel, and Escape, stop it between two files; a quit too). First, on the message thread, cheaply, what
    // must be true as it deletes: that it can clean up now (getWhyNotCleanUpAudio()), that the project is the look's, that
    // every version could be read, and that the versions name what they named at the look; else it is refused at once,
    // nothing deleted. A file stays that the song plays as Delete is pressed (and the window held from then on, it plays
    // nothing else until the end), that a version played at the look, or that the project file as saved, its autosave or a
    // backup plays, read on the finder's thread after the window was held (each again only where it changed since it was
    // last read). done is told on the message thread, once, with how many went and why it refused or stopped, if it did,
    // the window's hold let go of first. Not while one is under way, nor while a look is. Message thread: the context given
    // is the finder's.
    struct Deleted
    {
        int count = 0;
        juce::String refused; // why it deleted nothing, or why it stopped
    };

    void deleteUnused (ProjectContext&, const juce::Array<juce::File>&, Removal, std::function<void (Deleted)> done);
    bool isDeleting() const noexcept { return deleting != nullptr; }

    // How many deletions have ended, by any finder, since the program started: what shows the unused files looks again once
    // it changes (the Project view, after the Clean Up window's Delete). Message thread.
    static int countDeletions() noexcept { return deletionsEnded; }

    // Tests: what the finder's thread does before it deletes each file (held until let go: a deletion takes its own copy as
    // deleteUnused() is called), and how many saved files the looks and the deletions parsed, since the program started (on
    // the message thread: none).
    static inline std::function<void (const juce::File&)> beforeDeletingForTests;
    struct SavedFilesRead
    {
        int all = 0, onMessageThread = 0;
    };

    static SavedFilesRead countSavedFilesRead() noexcept;

private:
    const ProjectContext& context;
    BackgroundJobs jobs;
    std::shared_ptr<UnusedAudioLook> saved; // what was read of the saved files, which the looks keep
    std::optional<UnusedAudio> found;
    juce::StringArray lookedFor; // what the song played as the look under way began
    bool looking = false, lookAgain = false, refused = false;

    // A deletion under way: whether it was cancelled and how far it is (its thread's), and the window's hold.
    struct Deletion;
    std::shared_ptr<Deletion> deleting;
    static inline int deletionsEnded = 0;

    void looked (UnusedAudio);
    void deleted (const std::shared_ptr<Deletion>&, Deleted);

    JUCE_DECLARE_WEAK_REFERENCEABLE (UnusedAudioFinder)
    JUCE_DECLARE_NON_COPYABLE (UnusedAudioFinder)
};
} // namespace phazite
