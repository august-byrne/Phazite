// SPDX-License-Identifier: AGPL-3.0-or-later
// A Phazite project: a folder that holds the project file and everything the project uses.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <functional>

namespace phazite
{
class SongFollower;

//==============================================================================
// The project folder:
//   <name>.phazite   the project file: Tracktion's Edit, with Phazite's own data inside
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

    // Opens a saved project, upgraded to the current project format (model/base/Migrations.h): the file itself changes
    // when the project is saved. With restoreAutosave it opens the autosaved changes instead (see hasNewerAutosave());
    // saving then still writes to the project file. A project saved by a newer Phazite isn't opened.
    static std::unique_ptr<Project> open (te::Engine&, const juce::File& projectFile, bool restoreAutosave, juce::String& error);

    // Whether the file was saved by a newer Phazite, which open() doesn't open (design question 17).
    static bool isFromNewerPhazite (te::Engine&, const juce::File& projectFile);

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

    // Phazite's own data in the project file, next to the engine's (phazite::getPhaziteState(), model/base/internal/Schema.h). Every
    // project has it: it holds the format version.
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

    // Saves the project as it is now, unsaved changes included, as a new project <parentFolder>/<name>
    // with copies of its Audio, Generated and Renders folders and an ID of its own. This project and
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
    bool changedSinceSave = false, changedSinceAutosave = false, isWriting = false;

    // The project as it was when it was opened, with Phazite's parts added, or last saved. Tracktion sends notifications
    // that change nothing: after a tempo update, which opening or making an Edit causes too, each MIDI clip notifies its
    // mute switch so that editors redraw, and aliases copy their notes to each other again; and JUCE notifies a value
    // written again as another type. So while the project has no unsaved changes, a notification only sets
    // mayHaveChanged, and hasUnsavedChanges() and autosave() compare the project with this copy, once. Never listen to it
    // or change it.
    juce::ValueTree savedState;
    bool mayHaveChanged = false;

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

    // Tracktion writes these while it plays, without anyone changing the song, so they don't count as changes: the
    // playhead position, and the Edit's input-device states (the inputs it knows, and which track live input goes to).
    // Nor do the loop (where it is, whether it is on, and whether it follows the song), the metronome's switch and where
    // playback starts, which are about playing the song rather than the song (design question 18); nor do view settings,
    // which are saved but aren't the song (decision 4: schema::isTrackViewProperty(), isGridProperty(), PIANOROLL, and
    // isDeviceViewProperty(), where a device's window was, since step 2.12). A property
    // (isUncountedProperty()), or an element with everything in it (isUncountedElement()): notifications and the
    // comparison with savedState leave out the same. Never leave out lastSignificantChange: Tracktion's time stamp is how
    // a change it notices outside the Edit's values (a plug-in's own settings) reaches autosave. Tracktion renews it when
    // the undo history changes, a track's mute or solo changes, or a plug-in reports a change; view settings are written
    // without the undo manager, so they leave it alone.
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
// Autosaves a project every few minutes while it has unsaved changes.
class AutosaveTimer : private juce::Timer
{
public:
    explicit AutosaveTimer (Project&, int intervalSeconds = 120);
    ~AutosaveTimer() override;

private:
    Project& project;
    void timerCallback() override;
};
} // namespace phazite
