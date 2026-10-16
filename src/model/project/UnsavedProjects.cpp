// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UnsavedProjects.h"
#include "Project.h"
#include "model/engine/AppInfo.h"

namespace phazite
{
namespace
{
    // The lock that Phazites check for a folder, held, or nullptr if another Phazite holds it. It is named after the
    // folder's path, hashed: a lock's name can't hold every character a path can.
    std::unique_ptr<juce::InterProcessLock> hold (const juce::File& folder)
    {
        auto lock = std::make_unique<juce::InterProcessLock> ("Phazite unsaved "
                                                              + juce::String::toHexString (folder.getFullPathName().hashCode64()));

        if (! lock->enter (0))
            return {};

        return lock;
    }

    // The project in a folder of these: Project::create() makes <folder>/<name>/<name>.phazite.
    juce::File findProjectFileIn (const juce::File& folder)
    {
        for (const auto& projectFolder : folder.findChildFiles (juce::File::findDirectories, false))
            if (const auto file = projectFolder.getChildFile (projectFolder.getFileName() + Project::fileExtension); file.existsAsFile())
                return file;

        return {};
    }

    // Deletes a folder of these, letting go first of the audio files in it that Tracktion keeps open for a moment after
    // reading them. What can't be deleted now (a file still open) goes at a later start (findProjectToRestore()).
    void deleteFolder (te::Engine& engine, const juce::File& folder)
    {
        for (const auto& file : folder.findChildFiles (juce::File::findFiles, true))
            engine.getAudioFileManager().releaseFile (te::AudioFile (engine, file));

        folder.deleteRecursively();
    }
} // namespace

juce::File getUnsavedProjectsFolder()
{
#if JUCE_WINDOWS
    // The local application data, not the roaming: recordings can be large.
    const auto appData = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
#else
    const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#endif
    return appData.getChildFile (appName).getChildFile ("Unsaved Projects");
}

std::unique_ptr<UnsavedFolder> UnsavedFolder::create (te::Engine& engine, const juce::File& parentFolder)
{
    // Named after when it was made, which is all that tells two apart.
    const auto folder = parentFolder.getNonexistentChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), {}, true);
    auto lock = hold (folder);

    if (lock == nullptr || folder.createDirectory().failed())
        return {};

    return std::unique_ptr<UnsavedFolder> (new UnsavedFolder (engine, folder, std::move (lock)));
}

juce::File UnsavedFolder::findProjectToRestore (te::Engine& engine, const juce::File& parentFolder)
{
    juce::File newest;
    juce::Time newestTime;

    for (const auto& folder : parentFolder.findChildFiles (juce::File::findDirectories, false))
    {
        const auto held = hold (folder);

        if (held == nullptr)
            continue; // another Phazite has its project open

        const auto file = findProjectFileIn (folder);

        if (! file.existsAsFile() || ! Project::hasNewerAutosave (file))
        {
            deleteFolder (engine, folder); // nothing to restore
            continue;
        }

        if (Project::isFromNewerPhazite (engine, file))
            continue; // for the version that opens it

        if (const auto time = Project::getAutosaveFile (file).getLastModificationTime(); newest == juce::File() || time > newestTime)
        {
            newest = file;
            newestTime = time;
        }
    }

    return newest;
}

std::unique_ptr<UnsavedFolder> UnsavedFolder::takeOver (te::Engine& engine, const juce::File& projectFile)
{
    const auto folder = projectFile.getParentDirectory().getParentDirectory();
    auto lock = hold (folder);

    if (lock == nullptr || ! folder.isDirectory())
        return {};

    return std::unique_ptr<UnsavedFolder> (new UnsavedFolder (engine, folder, std::move (lock)));
}

UnsavedFolder::UnsavedFolder (te::Engine& e, const juce::File& f, std::unique_ptr<juce::InterProcessLock> l)
    : engine (e),
      folder (f),
      lock (std::move (l))
{
}

UnsavedFolder::~UnsavedFolder()
{
    deleteFolder (engine, folder);
}
} // namespace phazite
