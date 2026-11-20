// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UnusedAudio.h"
#include "internal/Project.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/WholeFile.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <map>
#include <set>

namespace phazite
{
// What a look for the unused audio needs, taken on the message thread, and what it reads of the project's saved files, by
// their paths, while they stay as they were: so that a look can run on any thread.
struct UnusedAudioLook
{
    juce::File projectFile;
    juce::StringArray songPaths;                   // what the song plays now
    VersionHistory::AudioPathsFinder versionPaths; // what its versions play (VersionHistory.h), none for none
    int projectNumber = 0;

    struct Saved
    {
        juce::int64 size = 0, modified = 0;
        juce::StringArray paths;
    };

    std::map<juce::File, Saved> saved;
};

namespace
{
    // A path as a song holds it: within the project's folder, or a full one.
    juce::File resolve (const juce::File& projectFolder, const juce::String& path)
    {
        return juce::File::isAbsolutePath (path) ? makeFileOfFullPath (path) : projectFolder.getChildFile (path);
    }

    // The songs the project's files on the disk hold: the project file as saved, its autosave and its backups, which closing
    // without saving, restoring the autosave or opening a backup brings back. Each a small file, read whole.
    juce::Array<juce::File> findSavedSongFiles (const juce::File& projectFile)
    {
        juce::Array<juce::File> files { projectFile, Project::getAutosaveFile (projectFile) };
        files.addArray (projectFile.getSiblingFile ("Backups").findChildFiles (juce::File::findFiles, false,
                                                                               juce::String ("*") + Project::fileExtension));
        return files;
    }

    // What the song plays.
    juce::StringArray findSongPaths (const ProjectContext& context)
    {
        return findAudioFilePaths (findSong (context)->getState());
    }

    // What the saved files play, each read again only once it changed since the last look; what is kept of one gone since
    // (a backup thinned away) forgotten. False where it was told to stop first, between two files: a look under way as its
    // project closes read every backup that had changed first, the window waiting for it.
    bool addSavedPaths (UnusedAudioLook& look, juce::StringArray& paths, const std::function<bool()>& shouldStop = nullptr)
    {
        const auto files = findSavedSongFiles (look.projectFile);
        std::erase_if (look.saved, [&files] (const auto& entry) { return ! files.contains (entry.first); });

        for (const auto& file : files)
        {
            if (shouldStop != nullptr && shouldStop())
                return false;

            if (! file.existsAsFile())
                continue;

            const auto size = file.getSize();
            const auto modified = file.getLastModificationTime().toMilliseconds();
            auto& saved = look.saved[file];

            if (saved.size != size || saved.modified != modified)
            {
                const auto xml = juce::XmlDocument::parse (file);
                saved = { size, modified, xml != nullptr ? findAudioFilePaths (juce::ValueTree::fromXml (*xml)) : juce::StringArray() };
            }

            paths.addArray (saved.paths);
        }

        return true;
    }

    std::set<juce::File> resolveAll (const juce::File& projectFile, const juce::StringArray& paths)
    {
        std::set<juce::File> used;

        for (const auto& path : paths)
            used.insert (resolve (projectFile.getParentDirectory(), path));

        return used;
    }

    // A look, on any thread: none where it was told to stop first.
    std::optional<UnusedAudio> look (UnusedAudioLook& look, const std::function<bool()>& shouldStop)
    {
        UnusedAudio found;
        found.projectNumber = look.projectNumber;
        auto paths = look.songPaths;

        if (look.versionPaths != nullptr)
        {
            auto versions = look.versionPaths (shouldStop);

            if (versions.stopped)
                return std::nullopt;

            found.unreadable = versions.unreadable;
            found.versionPaths = versions.paths;
            found.versionsKey = versions.key;
            paths.addArray (versions.paths);
        }

        // What a version that can't be read whole plays is unknown: nothing is offered.
        if (! found.unreadable.isEmpty())
            return found;

        if (! addSavedPaths (look, paths, shouldStop))
            return std::nullopt;

        const auto used = resolveAll (look.projectFile, paths);
        const auto folder = look.projectFile.getParentDirectory();
        juce::Array<juce::File> files;

        // The folders' files one at a time, so that it stops between two of them.
        for (const auto* subfolder : { "Audio", "Generated" })
        {
            for (const auto& entry : juce::RangedDirectoryIterator (folder.getChildFile (subfolder), true, "*", juce::File::findFiles))
            {
                if (shouldStop != nullptr && shouldStop())
                    return std::nullopt;

                files.add (entry.getFile());
            }
        }

        for (const auto& file : files)
            if (getFileKind (file) == FileKind::audio && used.count (file) == 0)
                found.files.push_back ({ file, file.getRelativePathFrom (folder).replaceCharacter ('\\', '/'), file.getSize() });

        std::sort (found.files.begin(), found.files.end(),
                   [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });
        return found;
    }
} // namespace

juce::String getWhyNotCleanUpAudio (const ProjectContext& context)
{
    const auto* song = findSong (context);
    const auto* history = getVersionHistory (context);

    if (song == nullptr || song->getFile() == juce::File())
        return "Clean Up needs a project with a folder of its own.";

    if (isRecording (*song))
        return "Not while recording: the take being written is in the Audio folder.";

    // An old version open in its place: what the song plays is the version's, not its own (Clean Up's command goes back to
    // Now first).
    if (song->isShowingOther())
        return "Not while a version is open: go Back to Now first.";

    if (history != nullptr && history->isReading())
        return "Phazite is still reading the project's versions.";

    if (history != nullptr && history->isWriting())
        return "Not while a version or an automatic save is being written: it may hold files the Versions folder "
               "doesn't name yet.";

    return {};
}

bool canCleanUpAudio (const ProjectContext& context)
{
    return getWhyNotCleanUpAudio (context).isEmpty();
}

int deleteUnusedAudio (ProjectContext& context, const UnusedAudio& found, const juce::Array<juce::File>& files, Removal removal,
                       juce::String* refused)
{
    const auto refuse = [refused] (const juce::String& why)
    {
        if (refused != nullptr)
            *refused = why;

        return 0;
    };

    if (const auto why = getWhyNotCleanUpAudio (context); why.isNotEmpty())
        return refuse (why);

    if (found.projectNumber != context.getProjectNumber())
        return refuse ("Another project is open since Clean Up looked.");

    if (! found.unreadable.isEmpty())
        return refuse ("What some versions play can't be read yet: " + found.unreadable.joinIntoString ("; ") + ".");

    // The versions as they were at the look: anything that came since (a commit, a song left on a branch, a snapshot that
    // arrived from another computer) is looked at again first.
    const auto* history = getVersionHistory (context);

    if (history != nullptr && history->findVersionsAudioKey() != found.versionsKey)
        return refuse ("The versions changed since Clean Up looked: it looks again.");

    // What must be fresh, looked at again now: the song and the saved files.
    UnusedAudioLook again;
    again.projectFile = findSong (context)->getFile();
    auto paths = findSongPaths (context);
    paths.addArray (found.versionPaths);
    addSavedPaths (again, paths);
    const auto used = resolveAll (again.projectFile, paths);
    auto deleted = 0;

    for (const auto& unused : found.files)
        if (files.contains (unused.file) && used.count (unused.file) == 0 && unused.file.existsAsFile()
            && (removal == Removal::toRecycleBin ? unused.file.moveToTrash() : unused.file.deleteFile()))
            ++deleted;

    return deleted;
}

//==============================================================================
UnusedAudioFinder::UnusedAudioFinder (const ProjectContext& c)
    : context (c),
      jobs ("Phazite unused audio", 1, juce::Thread::Priority::low),
      saved (std::make_shared<UnusedAudioLook>())
{
}

UnusedAudioFinder::~UnusedAudioFinder()
{
    jobs.removeAll (true); // a look under way stops at its next step
}

void UnusedAudioFinder::find()
{
    if (! canCleanUpAudio (context))
    {
        lookAgain = false;

        if (onFound != nullptr)
            onFound();

        return;
    }

    // One look at a time, the next once it is done.
    if (looking)
    {
        lookAgain = true;
        return;
    }

    looking = true;
    UnusedAudioLook prepared;
    prepared.projectFile = findSong (context)->getFile();
    prepared.songPaths = findSongPaths (context);
    lookedFor = prepared.songPaths;
    prepared.projectNumber = context.getProjectNumber();

    if (const auto* history = getVersionHistory (context))
        prepared.versionPaths = history->makeAudioPathsFinder();

    jobs.add (
        [prepared = std::move (prepared), saved = saved,
         weak = juce::WeakReference<UnusedAudioFinder> (this)] (const BackgroundJobs::ShouldStop& shouldStop) mutable
        {
            // What was read of the saved files before, kept for the next look (one at a time, on this thread).
            prepared.saved = std::move (saved->saved);
            auto result = look (prepared, shouldStop);
            saved->saved = std::move (prepared.saved);

            if (! result.has_value())
                return; // told to stop: the finder goes

            juce::MessageManager::callAsync (
                [weak, result = std::move (*result)]() mutable
                {
                    if (auto* self = weak.get())
                        self->looked (std::move (result));
                });
        });
}

void UnusedAudioFinder::looked (UnusedAudio look)
{
    looking = false;

    // Not another project's, which opened meanwhile, nor one begun before the song changed what it plays (it would count a
    // file played now, located or imported meanwhile): looked for again.
    if (look.projectNumber == context.getProjectNumber() && findSong (context) != nullptr && findSongPaths (context) == lookedFor)
        found = std::move (look);
    else
        lookAgain = true;

    if (std::exchange (lookAgain, false))
        find();

    if (onFound != nullptr)
        onFound();
}
} // namespace phazite
