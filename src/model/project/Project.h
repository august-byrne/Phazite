// SPDX-License-Identifier: AGPL-3.0-or-later
// A Phazite project: a folder that holds the project file and everything the project uses.
#pragma once

#include "model/base/Tracktion.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace phazite
{
class SongFollower;

//==============================================================================
// The project folder:
//   <name>.phazite   the project file: Tracktion's Edit, with Phazite's own data inside
//   Audio/           recordings and imported audio
//   Generated/       sounds made by Phazite's generators
//   Renders/         bounces and exports
//   Backups/         earlier versions of the project file (newest numBackupsToKeep), and the autosave
//
// Create, open and destroy Projects on the message thread.
class Project : private juce::ValueTree::Listener
{
public:
    static constexpr const char* fileExtension = ".phazite";
    static constexpr int numBackupsToKeep = 10;

    // Creates the folder <parentFolder>/<name> with a new project in it, and saves it. Returns nullptr
    // and sets error if that folder already exists or can't be written.
    static std::unique_ptr<Project> create (te::Engine&, const juce::File& parentFolder, const juce::String& name, juce::String& error);

    // Opens a saved project. With restoreAutosave it opens the autosaved changes instead (see
    // hasNewerAutosave()); saving then still writes to the project file.
    static std::unique_ptr<Project> open (te::Engine&, const juce::File& projectFile, bool restoreAutosave, juce::String& error);

    // True if the project has an autosave from a session that ended without saving or closing the
    // project, such as a crash: saving and closing both delete the autosave.
    static bool hasNewerAutosave (const juce::File& projectFile);

    static juce::File getAutosaveFile (const juce::File& projectFile);

    // Closing a project discards its autosave: the user has saved, or chosen not to.
    ~Project() override;

    te::Edit& getEdit() const noexcept { return *edit; }
    const juce::File& getFile() const noexcept { return file; }
    juce::File getFolder() const { return file.getParentDirectory(); }
    juce::File getAudioFolder() const { return getFolder().getChildFile ("Audio"); }
    juce::File getGeneratedFolder() const { return getFolder().getChildFile ("Generated"); }
    juce::File getRendersFolder() const { return getFolder().getChildFile ("Renders"); }
    juce::File getBackupsFolder() const { return getFolder().getChildFile ("Backups"); }

    // Phazite's own data in the project file, next to the engine's (phazite::getPhaziteState(), model/base/Schema.h).
    juce::ValueTree getPhaziteState();

    bool hasUnsavedChanges() const;

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

private:
    Project (std::unique_ptr<te::Edit>, const juce::File&);

    std::unique_ptr<te::Edit> edit;
    juce::File file;
    std::unique_ptr<SongFollower> songFollower; // keeps the loop around the song, and stops at its end (model/transport/Playback.h)
    bool changedSinceSave = false, changedSinceAutosave = false, isWriting = false;

    void backUpProjectFile();
    void markChanged (const juce::ValueTree&, const juce::Identifier& property = {});

    // Tracktion writes these while it plays, without anyone changing the song, so they don't count
    // as changes: the playhead position, and the Edit's input-device states (the inputs it knows,
    // and which track live input goes to). Nor does where the loop is, which follows the song, or
    // where playback starts.
    static bool isPlaybackState (const juce::ValueTree&, const juce::Identifier& property);

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override { markChanged (tree, property); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override { markChanged (parent); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override { markChanged (parent); }
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

//==============================================================================
// The projects opened most recently, newest first, kept in Phazite's settings.
class RecentProjects
{
public:
    static constexpr int maxNumProjects = 10;

    explicit RecentProjects (te::Engine&);

    void add (const juce::File& projectFile);
    void remove (const juce::File& projectFile);
    juce::Array<juce::File> getProjects() const; // only those that still exist

private:
    te::Engine& engine;
    juce::RecentlyOpenedFilesList load() const;
    void store (const juce::RecentlyOpenedFilesList&);
};
} // namespace phazite
