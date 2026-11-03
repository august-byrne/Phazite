// SPDX-License-Identifier: AGPL-3.0-or-later
// A Phazite project: a folder that holds the project file and everything the project uses.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <functional>

namespace phazite
{
class SongFollower;

// A new song's Edit, as a new project's starts (Project::create() makes one in a project folder): Phazite's parts (its
// tempo map, its track list, the master's loudness meter) and one empty track, with no undo steps. Its file, if given, is
// where its audio files' paths start from. For a song outside a project: tests' songs, and what is rendered alone.
std::unique_ptr<te::Edit> createSongEdit (te::Engine&, const juce::File& file = {});

// Where new projects go unless the user picks another folder: Documents/Phazite/Projects.
juce::File getDefaultProjectsFolder();

// An Edit made from a song alone (Phazite's state, Schema.h, which it takes), as the mirror builds Tracktion's side of it
// (docs/tracktion-migration.md, T1): what the song doesn't hold isn't in it. Its file, if given, is where its audio files'
// paths start from, and its cache folder where its frozen tracks' files are. No undo steps, no unsaved changes.
std::unique_ptr<te::Edit> createEditFromSong (te::Engine&, const juce::ValueTree& song, const juce::File& file = {},
                                              const juce::File& cacheFolder = {});

//==============================================================================
// The project folder:
//   <name>.phazite   the project file: the song (Schema.h), from which an Edit is built as the project opens
//   Audio/           recordings and imported audio
//   Generated/       sounds made by Phazite's generators
//   Renders/         exports: the song, and its stems
//   Backups/         earlier versions of the project file (newest numBackupsToKeep), and the autosave
//   Cache/           what Phazite can make again: frozen tracks' files, and Tracktion's renders of stretched audio clips
//                    (the Edit's temporary folder; model/mixer/internal/Freeze.h). Closing the project deletes what
//                    nothing in it uses any more.
//
// Create, open and destroy Projects on the message thread.
class Project : private juce::ValueTree::Listener
{
public:
    static constexpr const char* fileExtension = ".phazite";
    static constexpr int numBackupsToKeep = 10;

    // Creates the folder <parentFolder>/<name> with a new project in it, and saves it. addContent, if given, adds what the
    // project starts with (the starter song, an outline's sections): it goes into that first save, and the project is
    // then opened from it, so it starts without undo steps or unsaved changes (Tracktion puts clips added in code in
    // order only after this returns, as an undo step, but does so while it loads a project). Returns nullptr and sets
    // error if that folder already exists or can't be written, or the saved project can't be opened.
    static std::unique_ptr<Project> create (te::Engine&, const juce::File& parentFolder, const juce::String& name, juce::String& error,
                                            const std::function<void (te::Edit&)>& addContent = {});

    // Opens a saved project. With restoreAutosave it opens the autosaved changes instead (see hasNewerAutosave()); saving
    // then still writes to the project file.
    static std::unique_ptr<Project> open (te::Engine&, const juce::File& projectFile, bool restoreAutosave, juce::String& error);

    // True if the project has an autosave from a session that ended without saving or closing the
    // project, such as a crash: saving and closing both delete the autosave.
    static bool hasNewerAutosave (const juce::File& projectFile);

    static juce::File getAutosaveFile (const juce::File& projectFile);
    static juce::File getCacheFolder (const juce::File& projectFile) { return projectFile.getSiblingFile ("Cache"); }

    // Closing a project discards its autosave: the user has saved, or chosen not to.
    ~Project() override;

    te::Edit& getEdit() const noexcept { return *edit; }
    const juce::File& getFile() const noexcept { return file; }
    juce::File getFolder() const { return file.getParentDirectory(); }
    juce::File getAudioFolder() const { return getFolder().getChildFile ("Audio"); }
    juce::File getGeneratedFolder() const { return getFolder().getChildFile ("Generated"); }
    juce::File getRendersFolder() const { return getFolder().getChildFile ("Renders"); }
    juce::File getBackupsFolder() const { return getFolder().getChildFile ("Backups"); }
    juce::File getCacheFolder() const { return getCacheFolder (file); }

    // The song, which the project file holds (phazite::getSong(), model/base/internal/Schema.h).
    juce::ValueTree getPhaziteState();

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
    Project (std::unique_ptr<te::Edit>, const juce::File&);

    std::unique_ptr<te::Edit> edit;
    juce::File file;
    std::unique_ptr<SongFollower> songFollower; // keeps the loop around the song, and stops at its end (Playback.h)

    class BackgroundWriter; // one file at a time; its destructor waits for the one under way
    std::unique_ptr<BackgroundWriter> autosaveWriter;
    bool changedSinceSave = false, changedSinceAutosave = false, isWriting = false;

    // The song as it was when the project was opened, with Phazite's parts added, or last saved. JUCE notifies a value
    // written again as another type, which changes nothing. So while the project has no unsaved changes, a notification
    // only sets mayHaveChanged, and hasUnsavedChanges() and autosave() compare the song with this copy, once. Never listen
    // to it or change it. Only the elements of the song that notified are compared (the tracks, the tempo map...), unless
    // the song itself did (docs/performance-audit.md, U3). What Tracktion writes into the rest of the Edit's state (the
    // mirror, the playhead, its inputs) isn't saved, so it is no change.
    juce::ValueTree savedState;
    bool mayHaveChanged = false;
    juce::Array<juce::ValueTree> elementsToCompare;
    bool compareAll = false;

    // Makes the folder <parentFolder>/<name> with copies of the project's Audio, Generated, Renders and Cache folders, and
    // returns the project file that goes in it. If the name is empty, the folder exists or something can't be written, sets
    // error and returns nothing, leaving nothing behind.
    juce::File makeFolderWithFiles (const juce::File& parentFolder, const juce::String& name, juce::String& error) const;
    void setFile (const juce::File&); // the project file from now on, the Edit's too

    void backUpProjectFile();
    void rememberSavedState();
    void compareWithSavedState();
    void markChanged (const juce::ValueTree&, const juce::Identifier& property = {});
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);

    // These don't count as changes: the loop (where it is, whether it is on, and whether it follows the song), the
    // metronome's switch and where playback starts, which are about playing the song rather than the song (design question
    // 18); nor does the song's next ID (Schema.h: nextID), which the item it numbers comes with; nor do view settings,
    // which are saved but aren't the song (decision 4: schema::isTrackViewProperty(), isGridProperty(), PIANOROLL, and
    // isDeviceViewProperty(), where a device's window was, since step 2.12). A property (isUncountedProperty()), or an
    // element with everything in it (isUncountedElement()): notifications and the comparison with savedState leave out the
    // same. A change of Tracktion's time stamp (lastSignificantChange) is one:
    // it is how a change Tracktion notices outside the song (a plug-in's own settings, which a save stores in the song)
    // reaches autosave. Tracktion renews it when its undo history changes, a track's mute or solo changes, or a plug-in
    // reports a change; view settings are written without the undo manager, so they leave it alone.
    static bool isUncountedProperty (const juce::ValueTree&, const juce::Identifier& property);
    static bool isUncountedElement (const juce::ValueTree& parent, const juce::ValueTree& element);
    static bool isCounted (const juce::ValueTree&, const juce::Identifier& property);
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
