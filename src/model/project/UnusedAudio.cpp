// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UnusedAudio.h"
#include "internal/Project.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/WholeFile.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <set>

namespace phazite
{
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

    // What the song plays, and its versions, and its branches' songs as they were left, and its files on the disk.
    std::set<juce::File> findUsedAudio (const ProjectContext& context)
    {
        const auto* song = findSong (context);
        const auto folder = song->getFile().getParentDirectory();
        auto paths = findAudioFilePaths (song->getState());

        if (const auto* history = getVersionHistory (context))
            paths.addArray (history->findVersionsAudioPaths());

        for (const auto& file : findSavedSongFiles (song->getFile()))
            if (const auto xml = file.existsAsFile() ? juce::XmlDocument::parse (file) : nullptr)
                paths.addArray (findAudioFilePaths (juce::ValueTree::fromXml (*xml)));

        std::set<juce::File> used;

        for (const auto& path : paths)
            used.insert (resolve (folder, path));

        return used;
    }
} // namespace

bool canCleanUpAudio (const ProjectContext& context)
{
    const auto* song = findSong (context);
    const auto* history = getVersionHistory (context);
    return song != nullptr && song->getFile() != juce::File() && ! isRecording (*song) && (history == nullptr || ! history->isReading());
}

std::vector<UnusedAudioFile> findUnusedAudio (const ProjectContext& context)
{
    std::vector<UnusedAudioFile> unused;

    if (! canCleanUpAudio (context))
        return unused;

    const auto folder = findSong (context)->getFile().getParentDirectory();
    const auto used = findUsedAudio (context);

    for (const auto* subfolder : { "Audio", "Generated" })
        for (const auto& file : folder.getChildFile (subfolder).findChildFiles (juce::File::findFiles, true))
            if (getFileKind (file) == FileKind::audio && used.count (file) == 0)
                unused.push_back ({ file, file.getRelativePathFrom (folder).replaceCharacter ('\\', '/'), file.getSize() });

    std::sort (unused.begin(), unused.end(), [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });
    return unused;
}

int deleteUnusedAudio (ProjectContext& context, const juce::Array<juce::File>& files, Removal removal)
{
    // Looked for again: what came into use since the list was made stays.
    auto deleted = 0;

    for (const auto& unused : findUnusedAudio (context))
        if (files.contains (unused.file) && (removal == Removal::toRecycleBin ? unused.file.moveToTrash() : unused.file.deleteFile()))
            ++deleted;

    return deleted;
}
} // namespace phazite
