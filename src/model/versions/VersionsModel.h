// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project's versions, as the Versions view, the activity bar, the status bar and the commands reach them
// (docs/versions.md): by plain values, versions and branches by their ids. What changes is told by the project context's
// listeners (ProjectContext::Listener::versionsChanged()).
#pragma once

#include "model/base/IDs.h"
#include "model/project/ProjectContext.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;

struct VersionInfo
{
    juce::String id;
    juce::String name; // the one it was given, or the summary of its changes
    juce::String note;
    bool hasOwnName = false;
    juce::Time saved;
    juce::String branch;       // its branch's id
    juce::StringArray parents; // the versions it follows: none for the first, two for a merge
};

struct BranchInfo
{
    juce::String id, name;
    juce::Colour colour;
    juce::String head; // its last version's id; none before its first
};

// A line of the changes since the last version: a track (a folder or a return too), the master, or the song's own.
struct VersionChange
{
    enum class Kind
    {
        added,
        removed,
        changed
    };

    Kind kind = Kind::changed;
    TrackID track; // the track's, or the master's; none for the song's own
    bool isMaster = false, isSong = false;
    juce::String name; // "Bass", "Master", "Song"
    juce::String what; // "volume, 12 notes"; empty for a track added or taken away
};

// Whether the open project keeps versions (it has a project file), and whether they are still being read as it opens.
bool keepsVersions (const ProjectContext&);
bool isReadingVersions (const ProjectContext&);

// Every branch's versions, newest first; the branches, in the order they were made; the current one, which the project
// file is (none before the first version); and its last version.
std::vector<VersionInfo> getVersions (const ProjectContext&);
std::vector<BranchInfo> getBranches (const ProjectContext&);
std::optional<BranchInfo> getCurrentBranch (const ProjectContext&);
std::optional<VersionInfo> getLastVersion (const ProjectContext&);

// What the song holds now that the current branch's last version didn't, by track: none before the first version.
std::vector<VersionChange> getChangesSinceLastVersion (const ProjectContext&);
bool hasChangesSinceLastVersion (const ProjectContext&);

// The name and the note for the next version, as the Versions view holds them while they are typed.
void setVersionDraft (ProjectContext&, const juce::String& name, const juce::String& note);
juce::String getVersionDraftName (const ProjectContext&);
juce::String getVersionDraftNote (const ProjectContext&);

// Records a version of the song as it is now, with the draft's name and note (which it then clears), written in the
// background, and calls back once it is written, or not (why, then); false if it can't start (while one is being saved,
// while the versions are being read, or for a song that keeps none). Not an undo step: it changes the history, not the
// song.
bool saveVersion (ProjectContext&, std::function<void (juce::Result)> done = {});
bool isSavingVersion (const ProjectContext&);

// How many bytes the project's versions take on disk.
juce::int64 getVersionsSize (const ProjectContext&);

//==============================================================================
// Going back (docs/versions.md: Going back): each a named undo step, which what plays follows as it follows an undo. What
// isn't counted (view state, how the song plays) stays as it is now, and the history keeps every version.

// Bring Back This Version: the song made what it was then ("Bring Back "Verse 2""). False for a version it can't read.
bool bringBackVersion (ProjectContext&, const juce::String& versionID);

// What the Versions view lists against the song: the changes since the last version, or, while Take from This Version...
// compares the song with another (compareWithVersion(); none for the last version again), what differs from that one. And
// one of its lines taken back from that version: Put Back from the last version ("Put Back Bass"), Take from another ("Take
// Bass from "Verse 2""). What it named that isn't there then is taken away: a sidechain or a modulation of what is gone.
void compareWithVersion (ProjectContext&, const juce::String& versionID);
juce::String getComparedVersion (const ProjectContext&);
std::vector<VersionChange> getListedChanges (const ProjectContext&);
bool takeBack (ProjectContext&, const VersionChange&);

// The Versions view's choices, which the commands act on: the version selected in its history, and the line of the changes
// selected (none for neither).
void selectVersion (ProjectContext&, const juce::String& versionID);
juce::String getSelectedVersion (const ProjectContext&);
void selectChange (ProjectContext&, std::optional<VersionChange>);
std::optional<VersionChange> getSelectedChange (const ProjectContext&);

// The commands on the selected version and the selected line (version.bringBack, version.takeFrom, version.putBack,
// version.showChanges), what they are called now, and whether they can run.
juce::String describeBringBack (const ProjectContext&);
juce::String describePutBack (const ProjectContext&);
bool canBringBack (const ProjectContext&);
bool canTakeFrom (const ProjectContext&);
bool canPutBack (const ProjectContext&);
void takeFromSelectedVersion (ProjectContext&);
void bringBackSelectedVersion (ProjectContext&);
void putBackSelectedChange (ProjectContext&);

//==============================================================================
// Branches (docs/versions.md: Branches). Switching opens the other branch's song as a project opens, which the app does
// (OpenSong: ProjectSession, tests' TestSong::openSong()): the song as it is now, unsaved changes included, is kept on the
// branch it leaves, and the undo history starts again. Not undo steps.
using OpenSong = std::function<void (const juce::ValueTree& song)>;

// New Branch from Here: a branch named "Idea 2" (the next number free), in the next color, from a version, or from Now (none):
// from the current branch's last version, the song with its unsaved changes going on there. From a version, the song becomes
// that version's. False if it can't be made (before the first version, while one is being saved).
bool newBranch (ProjectContext&, const juce::String& fromVersion, const OpenSong&);
bool switchBranch (ProjectContext&, const juce::String& branchID, const OpenSong&);
bool renameBranch (ProjectContext&, const juce::String& branchID, const juce::String& name);
bool canMakeBranch (const ProjectContext&);

// The branch the Versions view's branch menu picked, which version.switchBranch switches to.
void selectBranch (ProjectContext&, const juce::String& branchID);

//==============================================================================
// Merging (docs/versions.md: Merging). A version of another line (another branch's, or one the current branch's past doesn't
// hold) into the song: its window lists what it changed since the newest version both have, by line, each with whether it
// clashes with what the song changed; Merge takes the lines ticked as one undo step ("Merge "Half-time""). Then, until the
// next version is saved (which follows both lines), each clash switches between its three values, each switch an undo step.
struct MergeLine
{
    VersionChange change;
    bool clashes = false; // with what this side changed
    bool ticked = true;   // to be taken: each to start with
};

bool canMerge (const ProjectContext&);                  // the version selected can be: not the current branch's last, nor in its past
juce::String describeMergeInto (const ProjectContext&); // version.merge's name: "Merge into Main..."
bool startMerge (ProjectContext&, const juce::String& versionID);
std::vector<MergeLine> getMergeLines (const ProjectContext&); // the merge window's, while it shows
juce::String describeMerge (const ProjectContext&);           // "Half-time" (the version's name), while it shows or is merging
juce::StringArray getMergeNotes (const ProjectContext&);      // what the merge couldn't decide, once it is made
void cancelMerge (ProjectContext&);
bool finishMerge (ProjectContext&, const std::vector<VersionChange>& lines);
bool isMerging (const ProjectContext&); // made, until its version is saved

// The merge window's ticks, and its Merge with the lines ticked (version.finishMerge).
void tickMergeLine (ProjectContext&, int line, bool ticked);
void finishTickedMerge (ProjectContext&);

struct ClashInfo
{
    juce::String line, what;
    juce::String values[3]; // before both changed it, this branch's, the other's: as text
    juce::String otherBranch;
    int chosen = 1;
};

std::vector<ClashInfo> getClashes (const ProjectContext&);
bool chooseClash (ProjectContext&, int clash, int value); // 0 before both, 1 this branch's, 2 the other's

// Save Version (version.save): the project saved first, then a version recorded, as saveProjectThen() does it: the app's
// save, which asks where a project goes the first time, and calls back with whether it saved.
using SaveProjectThen = std::function<void (std::function<void (bool saved)>)>;
void saveProjectAndVersion (ProjectContext&, const SaveProjectThen&);
bool canSaveVersion (const ProjectContext&);

// The versions' commands: version.save, the commands on the version and the line the Versions view selects, and those of
// branches and merges, which open another branch's song through openSong.
void addVersionCommands (CommandRegistry&, ProjectContext&, SaveProjectThen, OpenSong);
void newBranchFromSelection (ProjectContext&, const OpenSong&);
void switchToSelectedBranch (ProjectContext&, const OpenSong&);
void mergeSelectedVersion (ProjectContext&);
} // namespace phazite
