// SPDX-License-Identifier: AGPL-3.0-or-later
// Projects not saved yet (docs/design.md, section 18). A new project is kept in a folder of its own until its first save
// asks what it is called and where it goes (Project::moveTo()), so that what it uses (recordings, imported audio) and its
// autosave have a place from the start. That folder goes when the project closes, saved or not.
#pragma once

#include "model/base/Tracktion.h"

namespace phazite
{
// Where projects not saved yet are kept: in the user's application data, not among their documents.
juce::File getUnsavedProjectsFolder();

// The folder of a project not saved yet. While it is held, other Phazites running at the same time leave the folder
// alone; when it goes, after its project has closed, the folder is deleted.
class UnsavedFolder
{
public:
    // A new folder in parentFolder (getUnsavedProjectsFolder() in the app) for Project::create() to make a project in, or
    // nullptr if it can't be made.
    static std::unique_ptr<UnsavedFolder> create (te::Engine&, const juce::File& parentFolder);

    // Clears up the folders in parentFolder that earlier Phazites left and no Phazite uses now. Those whose project closed
    // with nothing to restore are deleted. Of the others, whose changes were saved automatically after the project was
    // made (Project::hasNewerAutosave(): Phazite didn't close properly), returns the project of the newest, for the app
    // to offer to restore; the rest wait for a later start, as do those saved by a newer Phazite. Nothing if there is none.
    static juce::File findProjectToRestore (te::Engine&, const juce::File& parentFolder);

    // Holds the folder of a project that findProjectToRestore() found: for the project restored from it, or to delete
    // it. Nothing if another Phazite holds it now.
    static std::unique_ptr<UnsavedFolder> takeOver (te::Engine&, const juce::File& projectFile);

    ~UnsavedFolder();

    const juce::File& getFolder() const noexcept { return folder; }

private:
    UnsavedFolder (te::Engine&, const juce::File&, std::unique_ptr<juce::InterProcessLock>);

    te::Engine& engine;
    const juce::File folder;
    const std::unique_ptr<juce::InterProcessLock> lock; // held: other Phazites see that the folder is in use

    JUCE_DECLARE_NON_COPYABLE (UnsavedFolder)
};
} // namespace phazite
