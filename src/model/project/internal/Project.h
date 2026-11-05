// SPDX-License-Identifier: AGPL-3.0-or-later
// A Phazite project: a folder that holds the project file and everything the project uses.
#pragma once

#include "model/base/Song.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <optional>

namespace phazite
{
// A new song, as a new project's starts: its tempo map, its master and one empty track, numbered by the song itself
// (Schema.h).
juce::ValueTree makeNewSong();

// A new song, as a new project's starts (Project::create() makes one in a project folder): its tempo map, its track list
// and one empty track, with no undo steps. Its file, if given, is where its audio files' paths start from. For a song
// outside a project: tests' songs, and what is rendered alone.
Song::Ptr createSong (const juce::File& file = {});

// Where new projects go unless the user picks another folder: Documents/Phazite/Projects.
juce::File getDefaultProjectsFolder();

// A song made of a state (Schema.h: PHAZITE, which it takes), with what every song has added (its tempo map, its track list
// with the master, its next ID), as a project opens. Its file, if given, is where its audio files' paths start from, and
// its cache folder where its frozen tracks' files are. No undo steps. Nothing for a state that isn't a song.
Song::Ptr makeSong (const juce::ValueTree& state, const juce::File& file = {}, const juce::File& cacheFolder = {});

//==============================================================================
// The project folder:
//   <name>.phazite   the project file: the song (Schema.h)
//   Audio/           recordings and imported audio
//   Generated/       sounds made by Phazite's generators
//   Renders/         exports: the song, and its stems
//   Backups/         earlier versions of the project file (newest numBackupsToKeep), and the autosave
//   Cache/           what Phazite can make again: frozen tracks' files (model/mixer/internal/Freeze.h) and renders of
//                    stretched audio clips (the song's cache folder). Closing the project deletes what nothing in it uses
//                    any more.
//   Versions/        the versions of the song the user saved, and their branches (model/versions/; Schema.h:
//                    schema::versions), once there is one
//
// Create, open and destroy Projects on the message thread.
class Project : private juce::ValueTree::Listener
{
public:
    static constexpr const char* fileExtension = ".phazite";
    static constexpr int numBackupsToKeep = 10;

    // Creates the folder <parentFolder>/<name> with a new project in it, and saves it. addContent, if given, adds what the
    // project starts with (the starter song, an outline's sections): it goes into that first save, so the project starts
    // without undo steps or unsaved changes. Returns nullptr and sets error if that folder already exists or can't be
    // written.
    static std::unique_ptr<Project> create (const juce::File& parentFolder, const juce::String& name, juce::String& error,
                                            const std::function<void (Song&)>& addContent = {});

    // Opens a saved project. With restoreAutosave it opens the autosaved changes instead (see hasNewerAutosave()); saving
    // then still writes to the project file.
    static std::unique_ptr<Project> open (const juce::File& projectFile, bool restoreAutosave, juce::String& error);

    // True if the project has an autosave from a session that ended without saving or closing the
    // project, such as a crash: saving and closing both delete the autosave.
    static bool hasNewerAutosave (const juce::File& projectFile);

    static juce::File getAutosaveFile (const juce::File& projectFile);
    static juce::File getCacheFolder (const juce::File& projectFile) { return projectFile.getSiblingFile ("Cache"); }

    // Closing a project discards its autosave: the user has saved, or chosen not to.
    ~Project() override;

    Song& getSong() const noexcept { return *song; }
    const juce::File& getFile() const noexcept { return file; }
    juce::File getFolder() const { return file.getParentDirectory(); }
    juce::File getAudioFolder() const { return getFolder().getChildFile ("Audio"); }
    juce::File getGeneratedFolder() const { return getFolder().getChildFile ("Generated"); }
    juce::File getRendersFolder() const { return getFolder().getChildFile ("Renders"); }
    juce::File getBackupsFolder() const { return getFolder().getChildFile ("Backups"); }
    juce::File getCacheFolder() const { return getCacheFolder (file); }

    // Whether the project holds anything it didn't hold when it was opened (with Phazite's parts added) or last saved,
    // leaving out what isn't a change (isUncountedProperty()). Compares the project with a copy at most once after each
    // change. Message thread only.
    bool hasUnsavedChanges();

    // Saves to the project file. A copy of the previous version goes into Backups/ first, and the
    // new file only replaces the old one once it has been written completely.
    juce::Result save();

    // Writes the autosave if anything changed since the last save or autosave.
    juce::Result autosave();

    // Whether anything changed since the last save or autosave: what autosave() would write. Message thread only.
    bool hasChangesToAutosave();

    // As autosave(), with the state copied here and its XML made and written on a background thread, so that the window
    // and what is played don't wait for it (docs/performance-audit.md, U2). Nothing while one is being written. Whether the
    // last one failed is told once, by takeAutosaveFailure(), and its changes are written again at the next.
    void autosaveInBackground();
    bool isAutosaving() const;
    bool takeAutosaveFailure();

    // When the autosave was last written, whole, for the window to say so; none since the project opened or last saved
    // (which deletes it). Message thread only.
    std::optional<juce::Time> getLastAutosave() const;

    // Saves the project as it is now, unsaved changes included, as a new project <parentFolder>/<name>
    // with copies of its Audio, Generated and Renders folders. This project and
    // its file stay as they are. Returns the new project file, or sets error and returns an empty
    // File (if that folder exists, for example).
    juce::File saveCopyAs (const juce::File& parentFolder, const juce::String& name, juce::String& error);

    // Saves the project, unsaved changes included, as <parentFolder>/<name> with copies of its Audio, Generated and
    // Renders folders, and goes on there with its undo history: the first save of a project kept with the unsaved ones
    // until then (model/project/internal/UnsavedProjects.h), which asks where it goes. The folder it was in stays as it is, for
    // its owner to delete once the project has closed (files in it may be open until then). Fails, changing nothing, if
    // that folder exists or can't be written.
    juce::Result moveTo (const juce::File& parentFolder, const juce::String& name);

private:
    Project (Song::Ptr, const juce::File&);

    Song::Ptr song;
    juce::ValueTree songState; // which this listens to (a listener belongs to the tree object it was added to)
    juce::File file;
    int changesOutsideHistory = 0; // the song's count of them as it was last compared (a plug-in's own changes)

    class BackgroundWriter; // one file at a time; its destructor waits for the one under way
    std::unique_ptr<BackgroundWriter> autosaveWriter;
    bool changedSinceSave = false, changedSinceAutosave = false, isWriting = false;
    juce::int64 autosavedAt = 0; // when autosave() last wrote it, in milliseconds since 1970; 0 for none

    // The song as it was when the project was opened, with Phazite's parts added, or last saved. JUCE notifies a value
    // written again as another type, which changes nothing. So while the project has no unsaved changes, a notification
    // only sets mayHaveChanged, and hasUnsavedChanges() and autosave() compare the song with this copy, once. Never listen
    // to it or change it. Only the elements of the song that notified are compared (the tracks, the tempo map...), unless
    // the song itself did (docs/performance-audit.md, U3).
    juce::ValueTree savedState;
    bool mayHaveChanged = false;
    juce::Array<juce::ValueTree> elementsToCompare;
    bool compareAll = false;

    // Makes the folder <parentFolder>/<name> with copies of the project's Audio, Generated, Renders and Cache folders, and
    // of its Versions if it keeps any, and returns the project file that goes in it. If the name is empty, the folder exists or something can't be written, sets
    // error and returns nothing, leaving nothing behind.
    juce::File makeFolderWithFiles (const juce::File& parentFolder, const juce::String& name, juce::String& error) const;
    void setFile (const juce::File&); // the project file from now on, the song's too

    void backUpProjectFile();
    void rememberSavedState();
    void compareWithSavedState();
    void markChanged (const juce::ValueTree&, const juce::Identifier& property = {});
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);

    // What isn't a change (Schema.h: schema::isUncountedProperty() and isUncountedElement(): view state, how the song plays,
    // its next ID) is left out of notifications and of the comparison with savedState alike. A plug-in's own change (its
    // settings, which a save stores in the song) counts, as the song counts it (Song::markChangedOutsideHistory()).
    static bool holdTheSameProject (const juce::ValueTree&, const juce::ValueTree&);

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override { markChanged (tree, property); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { markChildChanged (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { markChildChanged (parent, child); }
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override { markChanged (parent); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Project)
};

//==============================================================================
// Autosaves a project while it has unsaved changes, on a background thread (Project::autosaveInBackground()): the first
// time soon after the first change, then once changes have waited a few minutes, and at once when a recording ends (what
// a crash would lose most of). Never while recording: the recording isn't in the song until it ends. While the song
// plays, it waits for it to stop, for at most maxWaitWhilePlayingSeconds.
class AutosaveTimer : private juce::Timer
{
public:
    explicit AutosaveTimer (Project&, int intervalSeconds = 120, int firstDelaySeconds = 15);
    ~AutosaveTimer() override;

    // Called once when failuresToReport autosaves in a row have failed (docs/performance-audit.md, R7).
    std::function<void()> onRepeatedFailures;
    static constexpr int failuresToReport = 3;
    static constexpr int maxWaitWhilePlayingSeconds = 600;

private:
    Project& project;
    const int intervalSeconds, firstDelaySeconds;
    juce::uint32 changedSince = 0; // when the timer first saw a change not yet autosaved (the millisecond counter), or 0
    bool autosavedOnce = false;
    bool wasRecording = false, wasAutosaving = false;
    int failures = 0; // in a row

    void timerCallback() override;
};
} // namespace phazite
