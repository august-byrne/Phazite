// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/UnsavedProjects.h"
#include "internal/Project.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AppInfo.h"
#include "model/versions/VersionsModel.h"

#include <algorithm>
#include <optional>
#include <set>

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

    // What marks a folder of these as discarded (Don't Save, or a project closed with nothing to keep) as its project goes:
    // if it can't be deleted yet (a read given up still writing there as Phazite quits), the next start deletes it rather
    // than offering its project back.
    constexpr const char* discardedMarker = "Discarded";

    // Whether a file in a project's Audio folder is no audio to recover: empty (a name kept for a copy that never came) or a
    // copy's temporary file (juce::TemporaryFile's "<name>_temp<hex>", which writeWholeFile() writes into and a quit left).
    bool isLeftOverOfACopy (const juce::File& file)
    {
        const auto name = file.getFileNameWithoutExtension();
        const auto at = name.lastIndexOf ("_temp");
        return file.getSize() == 0 || (at > 0 && at + 5 < name.length() && name.substring (at + 5).containsOnly ("0123456789abcdefABCDEF"));
    }

    // Deletes a folder of these, once no read given up (one stuck in the system) may still write into it (AudioWork.h). What
    // can't be deleted now (a file still open) goes at a later start (findProjectToRestore()).
    // The folders whose deletion is still to come (deleteFolder() puts it off), by their paths, on the message thread: no
    // new project is given one of these paths (UnsavedFolder::create()). A deletion put off deletes what is at its path
    // when its turn comes: with the folder gone meanwhile (deleted by hand, or the tests' folder emptied) and a project
    // made at the same path within the second its name is, it deleted that project's folder from under it (the tests, in
    // a release build, 2026-11-17).
    std::set<juce::String>& getPathsToDelete()
    {
        static std::set<juce::String> paths;
        return paths;
    }

    void deleteFolder (const juce::File& folder)
    {
        // Once nothing writes there: an audio file's read given up, nor a commit the project's history was writing as it closed.
        const auto path = folder.getFullPathName();
        getPathsToDelete().insert (path);
        AudioWork::whenNoneWritesInto (folder,
                                       [folder, path]
                                       {
                                           whenNoVersionsWriteInto (folder,
                                                                    [folder, path]
                                                                    {
                                                                        folder.deleteRecursively();
                                                                        getPathsToDelete().erase (path);
                                                                    });
                                       });
    }

    // When a project was last changed, as far as a crash left it: its autosave, or the newest of the files it made or brought
    // in (recordings, imported audio, renders), which are in its folder only. Nothing if it holds neither: nothing to
    // restore.
    std::optional<juce::Time> findLastChange (const juce::File& projectFile)
    {
        std::optional<juce::Time> last;

        if (Project::hasNewerAutosave (projectFile))
            last = Project::getAutosaveFile (projectFile).getLastModificationTime();

        for (const auto* name : { "Audio", "Generated", "Renders" })
            for (const auto& file : projectFile.getSiblingFile (name).findChildFiles (juce::File::findFiles, true))
                if (const auto time = file.getLastModificationTime(); ! last.has_value() || time > *last)
                    last = time;

        return last;
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

std::unique_ptr<UnsavedFolder> UnsavedFolder::create (const juce::File& parentFolder)
{
    // Named after when it was made, which is all that tells two apart, with a number after it for another made within the
    // second: the first name that no folder has, that no deletion still to come has (getPathsToDelete()), and that no
    // Phazite holds.
    const auto name = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    constexpr int mostTried = 1000;

    for (int number = 1; number <= mostTried; ++number)
    {
        const auto folder = parentFolder.getChildFile (number == 1 ? name : name + " (" + juce::String (number) + ")");

        if (folder.exists() || getPathsToDelete().count (folder.getFullPathName()) > 0)
            continue;

        auto lock = hold (folder);

        if (lock == nullptr)
            continue;

        if (folder.createDirectory().failed())
            return {};

        return std::unique_ptr<UnsavedFolder> (new UnsavedFolder (folder, std::move (lock)));
    }

    return {};
}

juce::File UnsavedFolder::findProjectToRestore (const juce::File& parentFolder)
{
    juce::File newest;
    juce::Time newestTime;

    for (const auto& folder : parentFolder.findChildFiles (juce::File::findDirectories, false))
    {
        const auto held = hold (folder);

        if (held == nullptr)
            continue; // another Phazite has its project open

        if (folder.getChildFile (discardedMarker).existsAsFile())
        {
            deleteFolder (folder); // discarded as it closed, its deletion put off then
            continue;
        }

        const auto file = findProjectFileIn (folder);
        const auto lastChange = file.existsAsFile() ? findLastChange (file) : std::nullopt;

        if (! lastChange.has_value())
        {
            deleteFolder (folder); // nothing to restore
            continue;
        }

        if (newest == juce::File() || *lastChange > newestTime)
        {
            newest = file;
            newestTime = *lastChange;
        }
    }

    return newest;
}

std::unique_ptr<UnsavedFolder> UnsavedFolder::takeOver (const juce::File& projectFile)
{
    const auto folder = projectFile.getParentDirectory().getParentDirectory();
    auto lock = hold (folder);

    if (lock == nullptr || ! folder.isDirectory())
        return {};

    return std::unique_ptr<UnsavedFolder> (new UnsavedFolder (folder, std::move (lock)));
}

UnsavedFolder::UnsavedFolder (const juce::File& f, std::unique_ptr<juce::InterProcessLock> l)
    : folder (f),
      lock (std::move (l))
{
}

UnsavedFolder::~UnsavedFolder()
{
    // What isn't deleted now (put off while a read given up still writes there, or a file held open) is marked, before the
    // lock goes: a later start that finds it deletes it.
    deleteFolder (folder);

    if (folder.isDirectory())
        folder.getChildFile (discardedMarker).create();
}

int UnsavedFolder::recoverUnusedAudio (Song& song)
{
    juce::Array<juce::File> played;

    for (const auto& clipState : getClipStates (song))
        if (isAudioState (clipState))
            played.add (getClipFile (song, clipState)); // the file, not a render of it

    // In the order they were made, as they were recorded.
    auto unused = song.getFile().getSiblingFile ("Audio").findChildFiles (juce::File::findFiles, true);
    unused.removeIf ([&] (const juce::File& file)
                     { return played.contains (file) || getFileKind (file) != FileKind::audio || isLeftOverOfACopy (file); });
    std::sort (unused.begin(), unused.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() < b.getLastModificationTime(); });

    if (unused.isEmpty())
        return 0;

    juce::StringArray paths;

    for (const auto& file : unused)
        paths.add (file.getFullPathName());

    const auto added = importFiles (song, paths, {}, BeatPosition());
    song.getUndoManager().clearUndoHistory(); // the project was just opened: restoring it is no undo step
    return (int) added.size();
}
} // namespace phazite
