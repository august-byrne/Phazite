// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Project.h"
#include "devices/core/Clock.h"
#include "devices/modulation/Modulators.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/Json.h"
#include "model/base/KeptByAge.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/engine/AppInfo.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace phazite
{
bool isLinkElsewhere (const juce::File& entry, bool isSymbolicLink, const juce::File& linkedTarget)
{
    // A reparse point that leads to itself (a folder or a file the cloud keeps, on Windows) isn't a link. JUCE workaround
    // W35 (docs/juce-workarounds.md).
    return isSymbolicLink && linkedTarget != entry;
}

namespace
{
    // Text between an element's tags, which a song never holds (its values are attributes), and which a value tree has
    // nothing for: left out of a file from elsewhere before it becomes one.
    void removeText (juce::XmlElement& element)
    {
        element.deleteAllTextElements();

        for (auto* child : element.getChildIterator())
            removeText (*child);
    }

    // The song a project file holds (Schema.h: PHAZITE, the file's one element), or an invalid tree for a file that holds
    // none.
    juce::ValueTree readSong (const juce::File& file)
    {
        if (const auto xml = juce::XmlDocument::parse (file); xml != nullptr && xml->getTagName() == schema::PHAZITE.toString())
        {
            removeText (*xml);
            return juce::ValueTree::fromXml (*xml);
        }

        return {};
    }

    // What a save writes that the song doesn't hold yet: its devices' states as they are now, outside the undo history
    // (Song::storeDeviceStates()), and its next ID past every ID it holds.
    void storeForSaving (Song& song)
    {
        song.storeDeviceStates();
        countIDs (song);
    }

    // Writes a song into a project file (model/base/internal/SongText.h: writeProjectText()), through a temporary file that
    // takes its place once it is whole: each property on a line of its own, in the order of their names, each value in the
    // one text the song's versions write it in too, so that a tool that compares text (git, say) shows a change as the lines
    // it changed. A write that fails leaves the file as it was, and says why (model/base/WholeFile.h).
    juce::Result writeProjectFile (const juce::ValueTree& song, const juce::File& file)
    {
        if (! song.isValid())
            return juce::Result::fail ("There is no song to write.");

        juce::MemoryOutputStream text;
        writeProjectText (text, song);
        return writeWholeFile (file, text.getData(), text.getDataSize());
    }

    // The folder <parentFolder>/<name>, named as files can be, for a project of that name, or nothing, with error set, if
    // the name is empty or the folder exists.
    juce::File findNewProjectFolder (const juce::File& parentFolder, const juce::String& name, juce::String& error)
    {
        const auto safeName = makeUsableFileName (name.trim());

        if (safeName.isEmpty())
        {
            error = "The project needs a name.";
            return {};
        }

        const auto folder = parentFolder.getChildFile (safeName);

        if (folder.exists())
        {
            error = "There is already a folder called \"" + safeName + "\" in " + parentFolder.getFullPathName() + ".";
            return {};
        }

        return folder;
    }

    // That folder made, or nothing, with error set, if it can't be.
    juce::File makeProjectFolder (const juce::File& parentFolder, const juce::String& name, juce::String& error)
    {
        const auto folder = findNewProjectFolder (parentFolder, name, error);

        if (folder == juce::File())
            return {};

        if (const auto result = folder.createDirectory(); result.failed())
        {
            error = "Phazite couldn't create the project folder: " + result.getErrorMessage();
            return {};
        }

        return folder;
    }

    //==============================================================================
    // What a copy of a project takes along: the files it plays and made, its frozen tracks' and renders' (Cache), and its
    // versions (model/versions/), if it keeps any: a copy of a project is a copy with its past. Its backups don't come.
    constexpr const char* foldersCopied[] = { "Audio", "Generated", "Renders", "Cache", "Versions" };

    // Those folders' folders and files, each file with its size and time of change as listed, and how many bytes the files
    // hold, listed before any is copied.
    struct ListedFile
    {
        juce::File file;
        juce::int64 size = 0;
        juce::Time modified;
    };

    struct FoldersToCopy
    {
        std::vector<juce::File> folders;
        std::vector<ListedFile> files;
        juce::int64 bytes = 0;
    };

    // A folder's folders and files, and theirs, walked here rather than by JUCE's recursive listing, which on Windows
    // doesn't go into a folder the cloud keeps (JUCE workaround W35, docs/juce-workarounds.md). Never through a link: a
    // project from someone else may hold one to anywhere (its home folder, or the project itself, a loop); a link to a folder
    // is left out with what it leads to, and a link to a file too.
    void listFolder (const juce::File& folder, FoldersToCopy& list)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto file = entry.getFile();

            if (file.isSymbolicLink() && isLinkElsewhere (file, true, file.getLinkedTarget()))
                continue;

            if (entry.isDirectory())
            {
                list.folders.push_back (file);
                listFolder (file, list);
            }
            else
            {
                list.files.push_back ({ file, entry.getFileSize(), entry.getModificationTime() });
                list.bytes += entry.getFileSize();
            }
        }
    }

    // Which of those a list holds: all of them; or, for a first save, which copies the versions last (copyVersions()), what
    // its history writes, the versions but their bounces; or all else, the bounces with it, which a render writes, each on a
    // thread of its own (mayLeaveBehind()), and which can be as large as the audio.
    enum class FoldersListed
    {
        all,
        versions,
        allButVersions
    };

    FoldersToCopy listFoldersToCopy (const juce::File& projectFolder, FoldersListed listed = FoldersListed::all)
    {
        FoldersToCopy list;
        const auto versions = projectFolder.getChildFile ("Versions");
        const auto bounces = versions.getChildFile ("bounces");

        const auto add = [&list] (const juce::File& folder)
        {
            if (folder.isDirectory() && ! (folder.isSymbolicLink() && isLinkElsewhere (folder, true, folder.getLinkedTarget())))
            {
                list.folders.push_back (folder);
                listFolder (folder, list);
            }
        };

        for (const auto* name : foldersCopied)
        {
            const auto folder = projectFolder.getChildFile (name);

            if (listed == FoldersListed::all || (listed == FoldersListed::allButVersions) == (folder != versions))
                add (folder);
        }

        if (listed == FoldersListed::allButVersions)
            add (bounces);

        if (listed == FoldersListed::versions)
        {
            std::erase_if (list.folders, [&bounces] (const juce::File& each) { return each == bounces || each.isAChildOf (bounces); });
            std::erase_if (list.files,
                           [&] (const ListedFile& each)
                           {
                               const auto isBounce = each.file.isAChildOf (bounces);
                               list.bytes -= isBounce ? each.size : 0;
                               return isBounce;
                           });
        }

        return list;
    }

    // Whether a file the copy couldn't copy may be left behind: a render of an audio clip in the Cache folder, which Phazite
    // renders again from the clip as it plays, or a commit's bounce in the versions' bounces folder, which Phazite bounces
    // again as it is asked for, that went or changed since it was listed (closing the project tidied it up, a clip rendered it
    // again, or a bounce was being written on a thread of its own). Nothing else: a frozen track's file there may be all that
    // is left of a plug-in this computer hasn't, so it is never left behind, and any other failure (a full disk, a file
    // another program holds longer than copyFile() tries) fails the copy.
    bool mayLeaveBehind (const ListedFile& listed, const juce::File& projectFolder)
    {
        const auto isRender =
            listed.file.isAChildOf (projectFolder.getChildFile ("Cache")) && listed.file.getFileName().startsWith (renderFilePrefix);
        const auto isBounce = listed.file.getParentDirectory() == projectFolder.getChildFile ("Versions").getChildFile ("bounces")
                              && listed.file.hasFileExtension (".flac");

        if (! isRender && ! isBounce)
            return false;

        return ! listed.file.existsAsFile() || listed.file.getSize() != listed.size
               || listed.file.getLastModificationTime() != listed.modified;
    }

    // Tries something again while it fails, for a second or so, as another program (an antivirus, the indexer, a backup
    // tool) may hold a file a moment just as it is made or changed: on Windows such a hold refuses what asks for more
    // sharing than it gives. Whether it worked in the end; it stops trying once shouldStop says so. Never on the message
    // thread, which no sleep may hold up: there, it tries once.
    std::mutex retryHookLock;
    std::shared_ptr<const std::function<void (int)>> retryHook; // Project::setCopyRetryHook()'s

    juce::int64 backupTime = 0; // Project::setBackupTime()'s; 0 for the computer's clock

    juce::int64 getBackupTime()
    {
        return backupTime != 0 ? backupTime : juce::Time::currentTimeMillis();
    }

    // One of a project's backups (Project::backUpProjectFile()): when its version was saved, its modification time; and its
    // number among those of one second, 1 for the first.
    struct Backup
    {
        juce::File file;
        juce::int64 time = 0;
        int number = 1;
    };

    juce::String getBackupStem (const juce::File& projectFile, juce::Time saved)
    {
        return projectFile.getFileNameWithoutExtension() + " " + saved.formatted ("%Y-%m-%d %H-%M-%S");
    }

    // The number of a file named as the project's backups are ("<project> YYYY-MM-DD HH-MM-SS", then "_<n>" from 2), or 0
    // for one that isn't (another name's autosave, a temporary file a crash left, a file the user put there).
    int readBackupNumber (const juce::File& projectFile, const juce::File& candidate)
    {
        const auto prefix = projectFile.getFileNameWithoutExtension() + " ";
        const auto name = candidate.getFileNameWithoutExtension();

        if (! candidate.hasFileExtension (Project::fileExtension) || ! name.startsWith (prefix))
            return 0;

        const auto rest = name.substring (prefix.length());
        const juce::String stamp ("0000-00-00 00-00-00"); // a digit for each 0

        if (rest.length() < stamp.length())
            return 0;

        for (int i = 0; i < stamp.length(); ++i)
            if (stamp[i] == '0' ? ! juce::CharacterFunctions::isDigit (rest[i]) : rest[i] != stamp[i])
                return 0;

        const auto suffix = rest.substring (stamp.length());

        if (suffix.isEmpty())
            return 1;

        const auto digits = suffix.substring (1);

        if (! suffix.startsWithChar ('_') || digits.isEmpty() || digits.length() > 9 || ! digits.containsOnly ("0123456789")
            || digits.startsWithChar ('0'))
            return 0;

        return digits.getIntValue() >= 2 ? digits.getIntValue() : 0;
    }

    bool isOlder (const Backup& a, const Backup& b)
    {
        return a.time != b.time ? a.time < b.time : a.number < b.number;
    }

    // The project's backups, oldest first: by when their versions were saved, and of one second by their numbers.
    std::vector<Backup> findBackups (const juce::File& projectFile)
    {
        std::vector<Backup> found;

        for (const auto& candidate : projectFile.getSiblingFile ("Backups").findChildFiles (juce::File::findFiles, false,
                                                                                            juce::String ("*") + Project::fileExtension))
            if (const auto number = readBackupNumber (projectFile, candidate); number > 0)
                found.push_back ({ candidate, candidate.getLastModificationTime().toMilliseconds(), number });

        std::sort (found.begin(), found.end(), isOlder);
        return found;
    }

    bool tryAgainBriefly (const std::function<bool()>& attempt, const std::function<bool()>& shouldStop)
    {
        const auto mayWait = ! juce::MessageManager::existsAndIsCurrentThread();

        for (int tries = 1;; ++tries)
        {
            if (attempt())
                return true;

            if (! mayWait || tries > 50 || shouldStop())
                return false;

            std::shared_ptr<const std::function<void (int)>> hook;

            {
                const std::scoped_lock held (retryHookLock);
                hook = retryHook;
            }

            if (hook != nullptr)
                (*hook) (tries);

            juce::Thread::sleep (20);
        }
    }

    // A file copied into a new one, a piece at a time, so that a copy can stop between pieces, with its time of change kept
    // (a frozen track's file is known by it); counts what it copied. What kept it from being copied, if it stopped or
    // anything couldn't be read or written; a file another program holds a moment is tried again (tryAgainBriefly()).
    juce::Result copyFile (const juce::File& from, const juce::File& to, const std::function<bool()>& shouldStop,
                           std::atomic<juce::int64>& copied)
    {
        std::unique_ptr<juce::FileInputStream> in;

        // One that went is no hold to wait out.
        tryAgainBriefly ([&] { return (in = std::make_unique<juce::FileInputStream> (from))->openedOk() || ! from.existsAsFile(); },
                         shouldStop);

        if (! in->openedOk())
            return juce::Result::fail ("it couldn't be read (" + in->getStatus().getErrorMessage().trim() + ")");

        juce::int64 total = 0;

        {
            std::unique_ptr<juce::FileOutputStream> stream;

            if (! tryAgainBriefly ([&] { return (stream = std::make_unique<juce::FileOutputStream> (to))->openedOk(); }, shouldStop))
                return juce::Result::fail ("its copy couldn't be made (" + stream->getStatus().getErrorMessage().trim() + ")");

            auto& out = *stream;

            constexpr int pieceBytes = 1 << 20;
            juce::HeapBlock<char> piece (pieceBytes);

            for (;;)
            {
                if (shouldStop())
                    return juce::Result::fail ("the copy was cancelled");

                const auto read = in->read (piece, pieceBytes);

                if (read < 0)
                    return juce::Result::fail ("it couldn't be read to its end (" + in->getStatus().getErrorMessage().trim() + ")");

                if (read == 0)
                    break;

                if (! out.write (piece, (size_t) read))
                    return juce::Result::fail ("its copy couldn't be written (" + out.getStatus().getErrorMessage().trim() + ")");

                copied += read;
                total += read;
            }

            out.flush();

            if (out.getStatus().failed())
                return juce::Result::fail ("its copy couldn't be written (" + out.getStatus().getErrorMessage().trim() + ")");
        }

        // What was read, whole; and the file as long as what was read still: one written again meanwhile is no copy of it.
        if (to.getSize() != total)
            return juce::Result::fail ("its copy holds " + juce::String (to.getSize()) + " bytes of " + juce::String (total));

        if (const auto now = from.getSize(); now != total)
            return juce::Result::fail ("it changed while it was copied (" + juce::String (total) + " bytes read, " + juce::String (now)
                                       + " there now)");

        if (const auto changed = from.getLastModificationTime();
            changed != juce::Time() && ! tryAgainBriefly ([&] { return to.setLastModificationTime (changed); }, shouldStop))
            return juce::Result::fail ("its copy's time of change couldn't be set");

        return juce::Result::ok();
    }

    // The folders listed copied into another project's folder, which holds every folder a project has once it is done
    // (those it hadn't made empty). Any thread; whatever it leaves is the caller's to delete.
    juce::Result copyFolders (const juce::File& from, const juce::File& to, const FoldersToCopy& list,
                              const std::function<bool()>& shouldStop, std::atomic<juce::int64>& copied)
    {
        for (const auto& folder : list.folders)
            if (! to.getChildFile (folder.getRelativePathFrom (from)).createDirectory())
                return juce::Result::fail ("Phazite couldn't make the folder " + folder.getRelativePathFrom (from) + " of the copy.");

        for (const auto& listed : list.files)
        {
            if (shouldStop())
                return juce::Result::fail ("The copy was cancelled.");

            const auto& file = listed.file;
            const auto target = to.getChildFile (file.getRelativePathFrom (from));

            const auto copiedFile = copyFile (file, target, shouldStop, copied);

            if (copiedFile.wasOk())
                continue;

            if (shouldStop())
                return juce::Result::fail ("The copy was cancelled.");

            // A clip's render or a commit's bounce that went or changed while the copy ran is left behind, as if it hadn't been
            // made yet.
            if (mayLeaveBehind (listed, from))
            {
                target.deleteFile();
                continue;
            }

            return juce::Result::fail ("Phazite couldn't copy " + file.getRelativePathFrom (from).replaceCharacter ('\\', '/')
                                       + " into the project's new folder: " + copiedFile.getErrorMessage() + ".");
        }

        for (const auto* name : { "Audio", "Generated", "Renders", "Cache", "Backups" })
            if (! to.getChildFile (name).createDirectory())
                return juce::Result::fail ("Phazite couldn't make the project's " + juce::String (name) + " folder.");

        return juce::Result::ok();
    }

    // A first save's copy of the project's versions, last, listed as it starts: what their history writes is all written by
    // then (Project::VersionsCopier; known bug 169).
    juce::Result copyVersions (const juce::File& from, const juce::File& to, const std::function<bool()>& shouldStop,
                               std::atomic<juce::int64>& copied)
    {
        return copyFolders (from, to, listFoldersToCopy (from, FoldersListed::versions), shouldStop, copied);
    }

    // When this Phazite started: a copy marked before then was left by another run, which can't be writing it any more (one
    // Phazite runs at a time: PhaziteRunning, src/app/Main.cpp).
    const juce::Time processStart = juce::Time::getCurrentTime();

    // Whether a folder is a copy that a save that never finished left (a crash in the middle of one): named "(saving)", with
    // Phazite's marker in it, made before this run started, and holding no project file, so never a project the user named
    // so (whose marker a program held for a moment as it was made: putInPlace()). Nothing else is one: a folder without the
    // marker may be the user's.
    bool isLeftover (const juce::File& folder)
    {
        const auto marker = folder.getChildFile (Project::copyMarkerName);
        return folder.getFileName().contains (" (saving)") && folder.isDirectory() && ! folder.isSymbolicLink() && marker.existsAsFile()
               && marker.getLastModificationTime() < processStart
               && folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + Project::fileExtension).isEmpty();
    }

    // How a leftover copy is removed: null, by moving it to the system's trash (setLeftoverCopyRemoval()).
    std::function<bool (const juce::File&)> leftoverCopyRemoval;

    // The leftover copies in a folder moved to the system's trash, where the user can still get one back, on a background
    // thread: one can hold gigabytes of audio, on a network drive. But for the copy being made now, which it doesn't so
    // much as look at: on Windows a folder looked into can't be given its name meanwhile.
    void removeLeftoverCopies (const juce::File& parentFolder, const juce::File& beingMade, BackgroundJobs& jobs)
    {
        jobs.add (
            [parentFolder, beingMade, remove = leftoverCopyRemoval] (const BackgroundJobs::ShouldStop& shouldStop)
            {
                for (const auto& folder : parentFolder.findChildFiles (juce::File::findDirectories, false, "* (saving)*"))
                {
                    if (shouldStop() || folder == beingMade || ! isLeftover (folder))
                        continue;

                    if (remove != nullptr)
                        remove (folder);
                    else
                        folder.moveToTrash();
                }
            });
    }

    // Where the copy goes until it is whole, beside the folder it becomes: "<name> (saving)", with Phazite's marker in it, which
    // says that it is Phazite's to remove if it is ever left behind (removeLeftoverCopies()).
    juce::File makeCopyFolder (const juce::File& folder, BackgroundJobs& removals, juce::String& error)
    {
        const auto copy = folder.getParentDirectory().getNonexistentChildFile (folder.getFileName() + " (saving)", {}, true);
        removeLeftoverCopies (folder.getParentDirectory(), copy, removals);

        if (const auto result = copy.createDirectory(); result.failed())
        {
            error = "Phazite couldn't create the project folder: " + result.getErrorMessage();
            return {};
        }

        if (! copy.getChildFile (Project::copyMarkerName).create())
        {
            copy.deleteRecursively();
            error = "Phazite couldn't write into the project folder's parent, " + folder.getParentDirectory().getFullPathName() + ".";
            return {};
        }

        return copy;
    }

    // The whole copy put in the folder's place; false if the folder was made meanwhile, or the copy couldn't move.
    juce::Result putInPlace (const juce::File& copy, const juce::File& folder)
    {
        if (folder.exists())
            return juce::Result::fail ("There is already a folder called \"" + folder.getFileName() + "\" in "
                                       + folder.getParentDirectory().getFullPathName() + ".");

        if (! copy.moveFileTo (folder))
            return juce::Result::fail ("Phazite couldn't name the project's folder \"" + folder.getFileName() + "\".");

        // A project's folder now, the user's: its marker goes, tried again while another program (an antivirus, the indexer)
        // holds it a moment. One that stays does no harm: a folder with a project file in it is never taken for a leftover.
        const auto marker = folder.getChildFile (Project::copyMarkerName);

        for (int attempt = 0; attempt < 5 && ! marker.deleteFile(); ++attempt)
            juce::Thread::sleep (20);

        return juce::Result::ok();
    }

    //==============================================================================
    // A song as a file holds it made one the model can work with, whoever wrote the file: Phazite takes in projects and
    // presets from others, so whatever a file says is checked as it loads, and what doesn't hold together is left out or
    // brought back within what Schema.h allows. Each step keeps one of the song's rules (tests/SongCheck.h states them
    // again, on their own). A song Phazite saved itself is consistent already, and none of this changes it.
    namespace consistent
    {
        using schema::getPlaces;

        // Whether Phazite knows an element's type (Schema.h: schema::isKnownType()). One it doesn't (a later build's, another
        // tool's) stays where it is, as it is, with everything in it, and is written back (docs/project-format.md): nothing
        // reads it. What every walk of the loader asks of every element.
        bool isKnown (const juce::ValueTree& element)
        {
            return schema::isKnownType (element.getType());
        }

        // Where each element goes, as getPlaces() has it, by the pooled texts of the types' names.
        using TypeKey = const void*;

        TypeKey keyOf (const juce::Identifier& type)
        {
            return type.getCharPointer().getAddress();
        }

        bool belongsIn (const juce::Identifier& parent, const juce::Identifier& child)
        {
            static const auto places = []
            {
                std::unordered_map<TypeKey, std::vector<TypeKey>> table;

                for (const auto& [parentName, children] : getPlaces())
                    for (const auto& childName : children)
                        table[keyOf (juce::Identifier (parentName))].push_back (keyOf (juce::Identifier (childName)));

                return table;
            }();

            const auto found = places.find (keyOf (parent));
            return found != places.end() && std::find (found->second.begin(), found->second.end(), keyOf (child)) != found->second.end();
        }

        // The furthest place in beats a song holds anything at, and in seconds of a file.
        constexpr double maxBeat = maxSongBeats, maxSeconds = 1.0e7, some = 1.0e-9;

        // The IDs a song gives stay well below where counting them on would overflow (Schema.h): every ID below maxID (2^62);
        // and where an ID or the next ID reaches renumberMark (3 * 2^60), every ID at or past maxCountedID (2^61) given anew,
        // what names it following, counted on from below maxCountedID. Any other song keeps its IDs and its next ID.
        constexpr auto maxID = schema::maxID;
        constexpr auto maxCountedID = schema::maxCountedID;

        // A number Schema.h gives a range, and what it becomes when it is no number at all: none (its default), or a value.
        struct Range
        {
            const juce::Identifier* type;
            const juce::Identifier* property;
            double low, high;
            bool whole = false;
            std::optional<double> fallback {};
        };

        const std::vector<Range>& getRanges()
        {
            using namespace schema;
            static const std::vector<Range> ranges {
                { &PHAZITE, &loopStart, 0.0, maxBeat },
                { &PHAZITE, &loopEnd, 0.0, maxBeat },
                { &PHAZITE, &gridOffset, -3.0, 3.0, true },
                { &PIANOROLL, &gridOffset, -3.0, 3.0, true },
                { &TEMPO, &mapBeat, 0.0, maxBeat, false, 0.0 },
                { &TEMPO, &bpm, 20.0, 300.0, false, 120.0 },
                { &TEMPO, &tempoCurve, -1.0, 1.0 },
                { &TIMESIGNATURE, &mapBeat, 0.0, maxBeat, false, 0.0 },
                { &TIMESIGNATURE, &numerator, 1.0, 32.0, true, 4.0 },
                { &TIMESIGNATURE, &denominator, 2.0, 16.0, true, 4.0 },
                { &TRACK, &channelVolume, -100.0, 6.0 },
                { &TRACK, &channelPan, -1.0, 1.0 },
                { &TRACK, &keyRoot, 0.0, 11.0, true },
                { &TRACK, &trackPad, 0.0, 127.0, true },
                { &TRACK, &trackHeight, 32.0, 320.0, true },
                { &MASTER, &channelVolume, -100.0, 6.0 },
                { &MASTER, &channelPan, -1.0, 1.0 },
                { &MASTER, &trackHeight, 32.0, 320.0, true },
                { &LASTKEY, &keyRoot, 0.0, 11.0, true },
                { &LANE, &laneHeight, 32.0, 320.0, true },
                { &DEVICE, &deviceMix, 0.0, 1.0 },
                { &VALUE, &valueSetByHand, 0.0, 1.0, false, 0.0 },
                { &CONTAINER, &deviceMix, 0.0, 1.0 },
                { &CHAIN, &channelVolume, -100.0, 6.0 },
                { &CHAIN, &channelPan, -1.0, 1.0 },
                { &CHAIN, &chainCrossover, 20.0, 20000.0 },
                { &CHAIN, &chainLowKey, 0.0, 127.0, true },
                { &CHAIN, &chainHighKey, 0.0, 127.0, true },
                { &CHAIN, &chainLowVelocity, 1.0, 127.0, true },
                { &CHAIN, &chainHighVelocity, 1.0, 127.0, true },
                { &SEND, &sendLevel, -100.0, 6.0 },
                { &MACRO, &macroValue, 0.0, 1.0 },
                { &MODULATES, &modulatesAmount, -1.0, 1.0, false, 0.0 },
                { &POINT, &pointBeat, 0.0, maxBeat, false, 0.0 },
                { &POINT, &pointCurve, -1.0, 1.0 },
                { &SECTION, &sectionStart, 0.0, maxBeat, false, 0.0 },
                { &SECTION, &sectionLength, some, maxBeat, false, 4.0 },
                { &SECTION, &sectionEnergy, 0.0, 1.0 },
                { &CHORD, &chordStart, 0.0, maxBeat, false, 0.0 },
                { &CHORD, &chordLength, some, maxBeat, false, 4.0 },
                { &CHORD, &chordDegree, 1.0, 7.0, true, 1.0 },
                { &CHORD, &chordAccidental, -1.0, 1.0, true },
                { &CHORD, &chordInversion, 0.0, 16.0, true },
                { &CHORDKEY, &chordStart, 0.0, maxBeat, false, 0.0 },
                { &CHORDKEY, &keyRoot, 0.0, 11.0, true },
                { &CLIP, &clipStart, 0.0, maxBeat, false, 0.0 },
                { &CLIP, &clipLength, some, maxBeat, false, 4.0 },
                { &CLIP, &clipOffset, 0.0, maxBeat },
                { &CLIP, &clipFileStart, 0.0, maxSeconds, false, 0.0 },
                { &CLIP, &clipFileEnd, 0.0, maxSeconds, false, 0.0 },
                { &CLIP, &clipTempo, 20.0, 300.0 },
                { &CLIP, &clipPitch, -48.0, 48.0 },
                { &CLIP, &clipGain, -60.0, 24.0 },
                { &CLIP, &clipFadeIn, 0.0, maxSeconds },
                { &CLIP, &clipFadeOut, 0.0, maxSeconds },
                { &CLIP, &clipFadeInBend, -1.0, 1.0 },
                { &CLIP, &clipFadeOutBend, -1.0, 1.0 },
                { &CLIP, &clipTakes, 2.0, 1.0e6, true },
                { &CLIP, &clipTake, 1.0, 1.0e6, true },
                { &CLIP, &clipTakeLength, some, maxSeconds },
                { &NOTE, &notePitch, 0.0, 127.0, true, 60.0 },
                { &NOTE, &noteStart, -maxBeat, maxBeat, false, 0.0 },
                { &NOTE, &noteLength, some, maxBeat, false, 1.0 }, // more than 0: a tempo change scales notes past getMinimumNoteLength()
                { &NOTE, &noteVelocity, 1.0, 127.0, true, 100.0 },
                { &CONTROL, &controlNumber, 0.0, 127.0, true, 0.0 },
                { &CONTROL, &controlBeat, -maxBeat, maxBeat, false, 0.0 },
                { &CONTROL, &controlValue, 0.0, 16383.0, true, 0.0 },
                { &WARP, &warpTime, 0.0, maxSeconds, false, 0.0 },
                { &WARP, &warpPlaysAt, -maxSeconds, maxSeconds, false, 0.0 },
            };
            return ranges;
        }

        // Words a property is one of, by its element's type; anything else goes, as if it had none.
        struct Words
        {
            const juce::Identifier* type;
            const juce::Identifier* property;
            juce::StringArray words;
        };

        const std::vector<Words>& getWords()
        {
            using namespace schema;
            static const std::vector<Words> words {
                { &TRACK, &trackAutomationMode, { automationTouch, automationLatch, automationWrite } },
                { &MASTER, &trackAutomationMode, { automationTouch, automationLatch, automationWrite } },
                { &TRACK, &trackMonitoring, { monitorAlways, monitorNever } },
                { &CONTAINER, &containerSplit, { splitBands, splitMidSide, splitLeftRight } },
                { &CLIP, &clipFadeInShape, { fadeIsCurve, fadeIsSCurve } },
                { &CLIP, &clipFadeOutShape, { fadeIsCurve, fadeIsSCurve } },
            };
            return words;
        }

        // Ids written as text, a property's lowercase hexadecimal digits of a length, by its element's type; text of any other
        // form names nothing, and goes, as if it had none.
        struct HexID
        {
            const juce::Identifier* type;
            const juce::Identifier* property;
            int digits;
        };

        const std::vector<HexID>& getHexIDs()
        {
            using namespace schema;
            static const std::vector<HexID> ids { { &PHAZITE, &versionsBranch, versionsBranchDigits },
                                                  { &PHAZITE, &versionsBase, versionsBaseDigits },
                                                  { &PHAZITE, &idNumbering, idNumberingDigits } };
            return ids;
        }

        bool isHexID (const juce::var& value, int digits)
        {
            return value.isString() && schema::isVersionsID (value.toString(), digits);
        }

        // A value as a number: a number's own, or text that is a number and nothing else; none for anything else, and
        // infinity for text too large for one.
        std::optional<double> readNumber (const juce::var& value)
        {
            if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
                return (double) value;

            if (! value.isString())
                return std::nullopt;

            // Its own UTF-8 where trimming takes nothing off (it starts and ends with a visible ASCII character), as nearly
            // every number does: no copy made for each of a song's numbers.
            const auto read = [] (std::string_view text) -> std::optional<double>
            {
                double number = 0.0;
                const auto result = std::from_chars (text.data(), text.data() + text.size(), number);

                if (result.ec == std::errc::result_out_of_range)
                    return std::numeric_limits<double>::infinity();

                if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size())
                    return std::nullopt;

                return number;
            };

            const auto string = value.toString();
            const auto isVisible = [] (char c) { return c > ' ' && c < 127; };

            // Its bytes as they are, up to their end: a text from a file anyone wrote needn't be UTF-8, and the count JUCE works
            // out from its characters (getNumBytesAsUTF8()) is then not the count it holds.
            if (const std::string_view text (string.toRawUTF8()); ! text.empty() && isVisible (text.front()) && isVisible (text.back()))
                return read (text);

            return read (string.trim().toStdString());
        }

        // Whether a number's text is the one Phazite writes for the number it reads as (SongText.h: writeValue()), told from
        // the text alone, without writing the number, for the forms nearly every number of a song has: a whole number's
        // digits (up to 15, no leading zero, not "-0"); or a decimal from 0.001 up, of up to six significant digits, no
        // exponent, no leading or trailing zero, which Phazite writes as it is, as a float's fewest digits or in 15 (a float
        // holds about seven, so no other text of six or fewer reads as the same float, and none shorter). Anything else is
        // left to isSameValue().
        bool isPlainlyAsWritten (const juce::var& value)
        {
            if (! value.isString())
                return false;

            const auto string = value.toString();
            const std::string_view text (string.toRawUTF8()); // its bytes as they are (readNumber())
            const auto isDigit = [] (char c) { return c >= '0' && c <= '9'; };
            size_t i = text.starts_with ('-') ? 1 : 0;
            const auto wholeStart = i;

            while (i < text.size() && isDigit (text[i]))
                ++i;

            const auto wholeDigits = i - wholeStart;
            const auto belowOne = wholeDigits == 1 && text[wholeStart] == '0';

            if (wholeDigits == 0 || wholeDigits > 15 || (wholeDigits > 1 && text[wholeStart] == '0'))
                return false;

            if (i == text.size())
                return ! (belowOne && wholeStart == 1); // "-0" is written "0"

            if (text[i] != '.' || text.back() == '0')
                return false;

            const auto fractionStart = ++i;

            while (i < text.size() && isDigit (text[i]))
                ++i;

            const auto fractionDigits = i - fractionStart;

            if (i != text.size() || fractionDigits == 0)
                return false;

            if (! belowOne)
                return wholeDigits + fractionDigits <= 6;

            const auto zeros = text.find_first_not_of ('0', fractionStart) - fractionStart;
            return zeros <= 2 && fractionDigits - zeros <= 6;
        }

        // A number within its range: brought in, made whole, or for what is no finite number, its fallback (or none); and
        // held as Phazite writes it (SongText.h: writeValue()), so that a number another tool wrote otherwise ("120.0",
        // "0.7943282127380371") is the same value as the song's own, which the versions' changes and the project's
        // comparison with its saved copy compare as text.
        void keepInRange (juce::ValueTree element, const juce::Identifier& property, double low, double high, bool whole,
                          std::optional<double> fallback)
        {
            const auto number = readNumber (element[property]);

            if (! number.has_value() || ! std::isfinite (*number))
            {
                if (fallback.has_value())
                    element.setProperty (property, whole ? juce::var ((juce::int64) *fallback) : juce::var (*fallback), nullptr);
                else
                    element.removeProperty (property, nullptr);

                return;
            }

            auto kept = juce::jlimit (low, high, whole ? std::round (*number) : *number);

            // A time signature's note value is a power of two.
            if (element.hasType (schema::TIMESIGNATURE) && property == schema::denominator)
                kept = std::exp2 (std::round (std::log2 (kept)));

            if (const auto value = whole ? juce::var ((juce::int64) kept) : juce::var (kept);
                ! juce::exactlyEqual (kept, *number)
                || ! (isPlainlyAsWritten (element[property]) || isSameValue (element[property], value)))
                element.setProperty (property, value, nullptr);
        }

        // Whether a property holds a number Schema.h writes down, which a reader takes as a double: one the loader's table
        // gives a range (getRanges()), a device's or a modulator's value set by hand (VALUES), a point's position; or numbers
        // separated by spaces (inList): a step sequencer's steps, the sampler's slices.
        bool holdsNumbers (const juce::ValueTree& element, const juce::Identifier& property, bool& inList)
        {
            inList =
                (element.hasType (schema::MODULATOR) && property == schema::modulatorSteps)
                || (element.hasType (schema::SOUNDS) && (property == schema::samplerSlices || property == schema::samplerSlicesRemoved));

            if (inList || element.hasType (schema::VALUES) || (element.hasType (schema::POINT) && property == schema::pointValue))
                return true;

            const auto& ranges = getRanges();
            return std::any_of (ranges.begin(), ranges.end(),
                                [&] (const Range& range) { return element.hasType (*range.type) && property == *range.property; });
        }

        // The names of the properties that hold or name an ID of the song's (schema::holdsID(), namesID()), which their
        // elements' types then tell apart.
        bool mayHoldID (const juce::Identifier& property)
        {
            for (const auto* name :
                 { &schema::trackID, &schema::channelFader, &schema::channelVCA, &schema::clipLink, &schema::sendTo,
                   &schema::deviceSidechain, &schema::laneDevice, &schema::curveDevice, &schema::modulatesDevice, &schema::trackPadDevice })
                if (property == *name)
                    return true;

            return false;
        }

        // An ID as Phazite writes one: its whole number, its digits alone, the first not 0.
        bool isWrittenAsPhazitesID (const juce::String& text)
        {
            return text.isNotEmpty() && text.length() <= 19 && text[0] != '0' && text.containsOnly ("0123456789");
        }

        // A number of a list within its range (Schema.h): a step sequencer's steps from -1 to 1, 0 for what is no finite
        // number; the sampler's slices from 0 to 1, without what is none. One within it stays as it is written.
        void keepListInRange (juce::ValueTree element, const juce::Identifier& property, double low, double high,
                              std::optional<double> none)
        {
            if (! element.hasProperty (property))
                return;

            auto numbers = juce::StringArray::fromTokens (element[property].toString(), " ", {});
            auto changed = false;

            for (auto i = numbers.size(); --i >= 0;)
            {
                if (numbers[i].isEmpty())
                    continue;

                const auto number = readNumber (numbers[i]);
                const auto finite = number.has_value() && std::isfinite (*number);

                if (finite && *number >= low && *number <= high)
                    continue;

                changed = true;

                if (! finite && ! none.has_value())
                    numbers.remove (i);
                else
                    numbers.set (i, juce::String (finite ? juce::jlimit (low, high, *number) : *none));
            }

            if (changed)
                element.setProperty (property, numbers.joinIntoString (" "), nullptr);
        }

        // One walk through the song's elements, before anything reads a number of it, with what other tools write in forms of
        // their own: each number whose exponent JUCE's reader would overflow (Json.h: hasExponentTooLongToRead(); known bug
        // 170) the same number in a text it reads, past every double's or 0, which the ranges then take as no finite number
        // (a name that reads as such a number stays); and, but for a version's pieces (exponentsOnly), each switch and ID as
        // Phazite writes it, where another tool wrote it otherwise ("true", "false", "12.0"), as the song plays it (a switch as
        // juce::var reads it, an ID as readID() does), so that the versions, which compare text, see no change where there is
        // none (known bug 179); and each list's numbers within their range. What Phazite wrote stays as it is.
        void keepTexts (juce::ValueTree element, bool exponentsOnly)
        {
            const auto type = element.getType();

            for (auto i = element.getNumProperties(); --i >= 0;)
            {
                const auto property = element.getPropertyName (i);
                const auto& value = element[property];

                if (! value.isString())
                    continue;

                const auto text = value.toString();
                auto inList = false;

                // An exponent needs its e: the cheap test first, on its UTF-8 (no other character's bytes are an e's).
                const auto hasE = [&text]
                {
                    for (auto* c = text.toRawUTF8(); *c != 0; ++c)
                        if (*c == 'e' || *c == 'E')
                            return true;

                    return false;
                };

                if (hasE() && holdsNumbers (element, property, inList))
                {
                    auto kept = text;

                    if (inList)
                    {
                        auto numbers = juce::StringArray::fromTokens (text, " ", {});
                        auto changed = false;

                        for (auto& number : numbers)
                            if (const auto readable = makeExponentReadable (number); readable != number)
                            {
                                number = readable;
                                changed = true;
                            }

                        if (changed)
                            kept = numbers.joinIntoString (" ");
                    }
                    else
                    {
                        kept = makeExponentReadable (text);
                    }

                    if (kept != text)
                        element.setProperty (property, kept, nullptr);
                }
                else if (! exponentsOnly && mayHoldID (property) && ! isWrittenAsPhazitesID (text)
                         && (schema::holdsID (element, property) || schema::namesID (element, property)
                             || (type == schema::NOTE && property == schema::noteID)))
                {
                    if (const auto id = readID (value); id > 0 && id < maxID)
                        element.setProperty (property, juce::String ((juce::int64) id), nullptr);
                }
            }

            if (! exponentsOnly)
            {
                static const auto switchesByType = []
                {
                    std::unordered_map<TypeKey, std::vector<const SwitchProperty*>> table;

                    for (const auto& each : getSwitchProperties())
                        table[keyOf (*each.type)].push_back (&each);

                    return table;
                }();

                static const std::vector<const SwitchProperty*> none;
                const auto found = switchesByType.find (keyOf (type));

                for (const auto* each : found != switchesByType.end() ? found->second : none)
                {
                    if (! element.hasProperty (*each->property))
                        continue;

                    const auto& value = element[*each->property];

                    if (value.isBool() || (value.isString() && (value.toString() == "1" || value.toString() == "0")))
                        continue;

                    if ((bool) value)
                        element.setProperty (*each->property, true, nullptr);
                    else if (each->offIsNone)
                        element.removeProperty (*each->property, nullptr);
                    else
                        element.setProperty (*each->property, false, nullptr);
                }

                if (type == schema::MODULATOR)
                    keepListInRange (element, schema::modulatorSteps, -1.0, 1.0, 0.0);

                if (type == schema::SOUNDS)
                    for (const auto* slices : { &schema::samplerSlices, &schema::samplerSlicesRemoved })
                        keepListInRange (element, *slices, 0.0, 1.0, std::nullopt);
            }

            for (auto child : element)
                if (isKnown (child))
                    keepTexts (child, exponentsOnly);
        }
        // A full path on any system: this one's, one from the root ("/home/...", "\..."), or a Windows drive's ("C:..."). A
        // ':' anywhere else is part of a file's name, which Linux allows ("Audio/Loop 1:2.wav").
        bool isFullPath (const juce::String& path)
        {
            return juce::File::isAbsolutePath (path) || path.startsWithChar ('\\') || path.startsWithChar ('/')
                   || (path.length() >= 2 && juce::CharacterFunctions::isLetter (path[0]) && path[1] == ':');
        }

        // A path on a network share ("\\server\share", "//server/share", "\\?\UNC\..."), which Windows connects to as it is
        // looked at.
        bool isNetworkPath (const juce::String& path)
        {
            return path.length() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/');
        }

        struct Keeper
        {
            Song& song;
            juce::File folder;       // the project's, or none
            bool readInPart = false; // makeConsistent()'s ReadAs::inPart: what names an ID the song hasn't may lead into what wasn't read

            //==============================================================================
            // Each element of a type Phazite knows where Schema.h puts it; one somewhere else goes, with everything in it. One
            // of a type it doesn't know stays as it is (isKnown()).
            void keepPlaces (juce::ValueTree parent)
            {
                // The words of the kinds, made once: not a list of texts for each controller.
                static const juce::StringArray trackKinds { schema::trackIsTrack, schema::trackIsFolder, schema::trackIsReturn };
                static const juce::StringArray clipKinds { schema::clipIsMidi, schema::clipIsAudio };
                static const juce::StringArray controlKinds { schema::controlIsController, schema::controlIsPitchWheel,
                                                              schema::controlIsChannelPressure, schema::controlIsAftertouch,
                                                              schema::controlIsProgramChange };

                for (auto i = parent.getNumChildren(); --i >= 0;)
                {
                    auto child = parent.getChild (i);

                    if (! isKnown (child))
                        continue;

                    if (child.hasType (schema::TRACK) && ! trackKinds.contains (child[schema::trackKind].toString()))
                        child.setProperty (schema::trackKind,
                                           child.getChildWithName (schema::TRACK).isValid() ? schema::trackIsFolder : schema::trackIsTrack,
                                           nullptr);

                    const auto belongs =
                        belongsIn (parent.getType(), child.getType())
                        && ! (child.hasType (schema::TRACK) && parent.hasType (schema::TRACK)
                              && parent[schema::trackKind].toString() != schema::trackIsFolder)
                        && ! (child.hasType (schema::CLIP) && parent[schema::trackKind].toString() != schema::trackIsTrack)
                        && ! (child.hasType (schema::CLIP) && ! clipKinds.contains (child[schema::clipKind].toString()))
                        && ! ((child.hasType (schema::NOTE) || child.hasType (schema::CONTROL))
                              && parent[schema::clipKind].toString() != schema::clipIsMidi)
                        && ! (child.hasType (schema::WARP) && parent[schema::clipKind].toString() != schema::clipIsAudio)
                        && ! (child.hasType (schema::CONTROL) && ! controlKinds.contains (child[schema::controlKind].toString()));

                    if (! belongs)
                        parent.removeChild (i, nullptr);
                    else
                        keepPlaces (child); // in an element nothing goes in, only what Phazite doesn't know stays
                }
            }

            //==============================================================================
            // Every device of a type Phazite has, and its values set by hand, as every modulator's, of parameters it has,
            // within their ranges.
            // A parameter by its ID, as a device's or a modulator's description finds it (by an index of its own for one of
            // many: the drum sampler's 3,201), or none.
            using FindParameter = std::function<const devices::ParameterInfo*(std::string_view id)>;

            void keepValuesSetByHand (juce::ValueTree element, const FindParameter& find)
            {
                auto values = element.getChildWithName (schema::VALUES);

                for (auto i = values.getNumProperties(); --i >= 0;)
                {
                    const auto name = values.getPropertyName (i);
                    const auto saved = values[name];
                    const auto* parameter = find (std::string_view (name.getCharPointer().getAddress()));

                    if (parameter == nullptr)
                    {
                        values.removeProperty (name, nullptr);
                        continue;
                    }

                    if (parameter->unit == devices::Unit::choice && saved.isString())
                    {
                        if (! devices::findChoice (*parameter, saved.toString().toStdString()).has_value())
                            values.removeProperty (name, nullptr);

                        continue;
                    }

                    // Held as Phazite writes it, as keepInRange() holds a number: "1.0" or "5e-1" from another tool is the
                    // value Phazite's own file would hold, which the versions compare as text.
                    if (const auto number = readNumber (saved); ! number.has_value() || ! std::isfinite (*number))
                        values.removeProperty (name, nullptr);
                    else if (const auto kept = devices::constrain (*parameter, (float) *number);
                             ! juce::exactlyEqual ((double) kept, *number) || ! isSameValue (saved, toSavedValue (*parameter, kept)))
                        values.setProperty (name, toSavedValue (*parameter, kept), nullptr);
                }
            }

            void keepDevices (juce::ValueTree parent)
            {
                for (auto i = parent.getNumChildren(); --i >= 0;)
                {
                    auto child = parent.getChild (i);

                    if (child.hasType (schema::DEVICE))
                    {
                        const auto type = child[schema::deviceType].toString();

                        if (const auto* info = findOwnDevice (type.toStdString()))
                            keepValuesSetByHand (child,
                                                 [info] (std::string_view id) -> const devices::ParameterInfo*
                                                 {
                                                     const auto index = devices::findParameter (*info, id);
                                                     return index >= 0 ? &info->parameters[(size_t) index] : nullptr;
                                                 });
                        else if (type == schema::deviceIsPlugin)
                            child.removeChild (child.getChildWithName (schema::VALUES), nullptr); // a plug-in's are its VALUEs
                        else
                            parent.removeChild (i, nullptr);

                        continue;
                    }

                    if (child.hasType (schema::MODULATOR))
                    {
                        if (const auto kind = getModulatorKind (child))
                            keepValuesSetByHand (child,
                                                 [modulator = *kind] (std::string_view id) -> const devices::ParameterInfo*
                                                 {
                                                     const auto index = devices::findModulatorParameter (modulator, id);
                                                     return index >= 0 ? &devices::getModulatorParameters (modulator)[(size_t) index]
                                                                       : nullptr;
                                                 });
                        else
                            parent.removeChild (i, nullptr);

                        continue;
                    }

                    if (isKnown (child))
                        keepDevices (child);
                }
            }

            //==============================================================================
            // Every number Schema.h gives a range within it, every word one of its words, and every id written as text one.
            void keepNumbers (juce::ValueTree element)
            {
                for (const auto& id : getHexIDs())
                    if (element.hasType (*id.type) && element.hasProperty (*id.property) && ! isHexID (element[*id.property], id.digits))
                        element.removeProperty (*id.property, nullptr);

                // The tables' rows by their elements' types, looked up once for each element.
                static const auto byType = []
                {
                    std::unordered_map<TypeKey, std::pair<std::vector<const Range*>, std::vector<const Words*>>> table;

                    for (const auto& range : getRanges())
                        table[keyOf (*range.type)].first.push_back (&range);

                    for (const auto& words : getWords())
                        table[keyOf (*words.type)].second.push_back (&words);

                    return table;
                }();

                if (const auto rows = byType.find (keyOf (element.getType())); rows != byType.end())
                {
                    for (const auto* range : rows->second.first)
                        if (element.hasProperty (*range->property))
                            keepInRange (element, *range->property, range->low, range->high, range->whole, range->fallback);

                    for (const auto* words : rows->second.second)
                        if (element.hasProperty (*words->property) && ! words->words.contains (element[*words->property].toString()))
                            element.removeProperty (*words->property, nullptr);
                }

                for (auto child : element)
                    if (isKnown (child))
                        keepNumbers (child);
            }

            // Every item's ID its own, a note's in its clip; one that isn't, or none, given a new one; and a track whose ID is
            // given anew unfrozen, as a frozen track's file is found by its track's ID (model/mixer/internal/Freeze.h), which
            // may be another's. An alias group's past maxID goes, its clips each alone.
            // Where the song is given its IDs anew (renumbering, Schema.h: mustGiveIDsAnew()), so is every ID at or past
            // maxCountedID below maxID: an item's, a fader's or a VCA's, which what named it then names (renumbered,
            // keepRenumberedNames()); an alias group's, all its clips in the new group (relinked); and a note's, the same new
            // one in each clip of its alias group (linkedNotes), so that its clips keep following each other.
            bool renumbering {};
            std::map<std::uint64_t, std::uint64_t> renumbered {}, relinked {};
            std::map<std::uint64_t, std::map<std::uint64_t, std::uint64_t>> linkedNotes {};

            // The largest ID below a bound that a name names (schema::namesID()), anywhere in the song.
            static std::uint64_t findLargestName (const juce::ValueTree& tree, std::uint64_t below)
            {
                std::uint64_t largest = 0;

                for (int p = 0; p < tree.getNumProperties(); ++p)
                    if (const auto property = tree.getPropertyName (p); mayHoldID (property) && schema::namesID (tree, property))
                        if (const auto id = readID (tree[property]); id < below)
                            largest = std::max (largest, id);

                for (const auto& child : tree)
                    largest = std::max (largest, findLargestName (child, below));

                return largest;
            }

            // The song stamped with the numbering its IDs are given anew in (Schema.h: idNumbering), made from the song as it is
            // before they are: what they are given depends on that alone, so the same song gets the same IDs and the same
            // stamp, at every opening of its file and every reading of a version of it, and a song given them otherwise
            // another stamp.
            static void stampNumbering (juce::ValueTree state)
            {
                juce::MemoryOutputStream text;
                writeProjectText (text, state);
                const auto hash = juce::SHA256 (text.getData(), text.getDataSize()).toHexString();
                state.setProperty (schema::idNumbering, hash.substring (0, schema::idNumberingDigits), nullptr);
            }

            bool isToGiveAnew (std::uint64_t id) const { return id == 0 || id >= maxID || (renumbering && id >= maxCountedID); }
            bool isRenumbered (std::uint64_t id) const { return renumbering && id >= maxCountedID && id < maxID; }

            void keepIDs (juce::ValueTree tree, std::set<std::uint64_t>& seen)
            {
                std::optional<std::set<std::uint64_t>> notes; // the clip's notes' IDs, made as a note is first checked one by one

                // A clip's notes' new IDs by their old, where the song is given its IDs anew: its alias group's, or its own.
                // Made only then, and only for a clip with a note to give anew.
                std::optional<std::map<std::uint64_t, std::uint64_t>> ownNotes;

                const auto getNotesGiven = [this, &tree, &ownNotes]() -> std::map<std::uint64_t, std::uint64_t>&
                {
                    if (tree.hasType (schema::CLIP) && tree.hasProperty (schema::clipLink))
                        return linkedNotes[readID (tree[schema::clipLink])];

                    if (! ownNotes.has_value())
                        ownNotes.emplace();

                    return *ownNotes;
                };

                // Whether its notes' IDs are each their own already, as nearly every clip's are: found by sorting them, without
                // a set's node for each note.
                std::optional<bool> notesAreTheirOwn;

                const auto areTheirOwn = [this, &tree]
                {
                    std::vector<std::uint64_t> ids;
                    ids.reserve ((size_t) tree.getNumChildren());

                    for (const auto& child : tree)
                    {
                        if (! child.hasType (schema::NOTE))
                            continue;

                        const auto id = readID (child[schema::noteID]);

                        if (isToGiveAnew (id))
                            return false;

                        ids.push_back (id);
                    }

                    std::sort (ids.begin(), ids.end());
                    return std::adjacent_find (ids.begin(), ids.end()) == ids.end();
                };

                for (auto child : tree)
                {
                    if (! isKnown (child))
                        continue;

                    if (child.hasType (schema::CLIP) && child.hasProperty (schema::clipLink))
                    {
                        if (const auto link = readID (child[schema::clipLink]); link == 0 || link >= maxID)
                        {
                            child.removeProperty (schema::clipLink, nullptr);
                        }
                        else if (isRenumbered (link))
                        {
                            auto found = relinked.find (link);

                            if (found == relinked.end())
                                found = relinked.emplace (link, song.makeID()).first;

                            child.setProperty (schema::clipLink, (juce::int64) found->second, nullptr);
                        }
                    }

                    if (child.hasType (schema::NOTE))
                    {
                        if (! notesAreTheirOwn.has_value())
                            notesAreTheirOwn = areTheirOwn();

                        if (*notesAreTheirOwn)
                            continue;

                        const auto old = readID (child[schema::noteID]);
                        auto id = old;

                        if (isRenumbered (old))
                        {
                            auto& notesGiven = getNotesGiven();
                            auto found = notesGiven.find (old);

                            if (found == notesGiven.end())
                                found = notesGiven.emplace (old, song.makeID()).first;

                            id = found->second;
                        }
                        else if (isToGiveAnew (old))
                        {
                            id = 0;
                        }

                        auto& noteIDs = notes.has_value() ? *notes : notes.emplace();

                        if (id == 0 || ! noteIDs.insert (id).second)
                        {
                            id = song.makeID();
                            noteIDs.insert (id);
                        }

                        if (id != old)
                            child.setProperty (schema::noteID, (juce::int64) id, nullptr);

                        continue;
                    }

                    for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                    {
                        if (! schema::holdsID (child, *property) || (property != &schema::trackID && ! child.hasProperty (*property)))
                            continue;

                        if (const auto id = readID (child[*property]); isToGiveAnew (id) || ! seen.insert (id).second)
                        {
                            const auto made = song.makeID();
                            seen.insert (made);
                            child.setProperty (*property, (juce::int64) made, nullptr);

                            if (isRenumbered (id))
                                renumbered.try_emplace (id, made); // the first of its ID's, as for an ID held twice

                            if (property == &schema::trackID && child.hasType (schema::TRACK))
                                child.removeProperty (schema::trackFrozen, nullptr);
                        }
                    }

                    if (child.getNumChildren() > 0) // no walk, nor the walk's sets, for each controller, point or marker
                        keepIDs (child, seen);
                }
            }

            // Where the song was given its IDs anew: what named one at or past maxCountedID names its new one, or nothing if
            // none had it (then left out as any name of nothing is: keepSends(), keepNames()). A macro's curve, which would name the
            // macro by its ID as its parameter too (model/devices/internal/Parameters.h), isn't followed: no modulator's
            // parameter is automated, and such a curve goes as every song opens (Automation.h: forgetCurvesOfGoneDevices()).
            void keepRenumberedNames (juce::ValueTree tree)
            {
                for (auto child : tree)
                {
                    if (! isKnown (child))
                        continue;

                    for (int p = 0; p < child.getNumProperties(); ++p)
                    {
                        const auto property = child.getPropertyName (p);

                        if (! mayHoldID (property) || ! schema::namesID (child, property))
                            continue;

                        const auto old = readID (child[property]);

                        if (! isRenumbered (old))
                            continue;

                        const auto found = renumbered.find (old);
                        const auto now = found != renumbered.end() ? found->second : (std::uint64_t) 0;

                        child.setProperty (property, writeIDLike (child[property], now), nullptr);
                    }

                    keepRenumberedNames (child);
                }
            }

            //==============================================================================
            // A send to a return the song hasn't goes, as deleting the return takes its sends (model/mixer/internal/Mixer.h:
            // forgetWhatNamesTheGone()), and so does its ID, so that what names the send goes too (keepNames()).
            void keepSends (juce::ValueTree tree, std::set<std::uint64_t>& items)
            {
                for (auto i = tree.getNumChildren(); --i >= 0;)
                {
                    auto child = tree.getChild (i);

                    if (! isKnown (child))
                        continue;

                    if (child.hasType (schema::SEND) && items.count (readID (child[schema::sendTo])) == 0 && ! readInPart)
                    {
                        items.erase (readID (child[schema::sendID]));
                        tree.removeChild (i, nullptr);
                        continue;
                    }

                    keepSends (child, items);
                }
            }

            // What names an item naming one the song has: a lane, a curve and a modulation of what isn't there go, and so does
            // a sidechain or a pad's track's sampler.
            void keepNames (juce::ValueTree tree, const std::set<std::uint64_t>& items)
            {
                for (auto i = tree.getNumChildren(); --i >= 0;)
                {
                    auto child = tree.getChild (i);

                    if (! isKnown (child))
                        continue;

                    auto gone = false;

                    // Those that name an ID, listed first, as taking one away moves the others; no list for an element that
                    // names none, as nearly all (a note) don't.
                    const auto namesOne = [&child] (int p)
                    {
                        const auto property = child.getPropertyName (p);
                        return mayHoldID (property) && schema::namesID (child, property);
                    };

                    auto anyNamed = false;

                    for (int p = 0; p < child.getNumProperties() && ! anyNamed; ++p)
                        anyNamed = namesOne (p);

                    if (anyNamed)
                    {
                        std::vector<juce::Identifier> properties;

                        for (int p = 0; p < child.getNumProperties(); ++p)
                            if (namesOne (p))
                                properties.push_back (child.getPropertyName (p));

                        for (const auto& property : properties)
                        {
                            if (items.count (readID (child[property])) > 0 || readInPart)
                                continue;

                            if (child.hasType (schema::LANE) || child.hasType (schema::CURVE) || child.hasType (schema::MODULATES))
                                gone = true;
                            else if (property == schema::trackPadDevice)
                                child.removeProperty (schema::trackPad, nullptr);

                            child.removeProperty (property, nullptr);
                        }
                    }

                    if (gone)
                        tree.removeChild (i, nullptr);
                    else
                        keepNames (child, items);
                }
            }

            //==============================================================================
            // No two of a parent's elements with one key (items aside, whose IDs keepIDs() made their own): the first stays.
            void keepKeys (juce::ValueTree tree)
            {
                if (tree.getNumChildren() == 0)
                    return; // no set made for each note

                std::set<juce::String> keys;

                for (auto i = 0; i < tree.getNumChildren();)
                {
                    auto child = tree.getChild (i);

                    if (! isKnown (child))
                    {
                        ++i;
                        continue;
                    }

                    if (! schema::holdsID (child, schema::trackID))
                        if (const auto key = schema::getElementKey (child);
                            key.isNotEmpty() && ! keys.insert (child.getType().toString() + "/" + key).second)
                        {
                            tree.removeChild (i, nullptr);
                            continue;
                        }

                    keepKeys (child);
                    ++i;
                }
            }

            //==============================================================================
            // Each curve's points at positions along the control's travel, 0 to 1, one without a position, or with one that is no
            // number, left out (0 would be silence on a volume's lane), and a curve that had points and is left without gone (one
            // holding only what Phazite doesn't know stays); and no curve of a parameter its device hasn't. A plug-in tells its
            // parameters only once it plays: its curves are kept, as its element says it is one, never asked of its instance (a
            // song that belongs to no session, one read from the versions on the history's thread, has none). A chain's crossover is
            // kept while its container isn't split into bands, or while it is the first chain, which have none for now
            // (describeParameter() finds none); and a device's in what wasn't read, which the song that takes it in knows.
            void keepCurves()
            {
                auto automation = song.getState().getChildWithName (schema::AUTOMATION);

                for (auto i = automation.getNumChildren(); --i >= 0;)
                {
                    auto curve = automation.getChild (i);

                    if (! curve.hasType (schema::CURVE))
                        continue; // one of a type Phazite doesn't know

                    const ParameterRef parameter { DeviceID::fromRaw (readID (curve[schema::curveDevice])),
                                                   curve[schema::curveParameter].toString() };

                    if (findDeviceState (song, parameter.device)[schema::deviceType].toString() != schema::deviceIsPlugin
                        && ! describeParameter (song, parameter).has_value()
                        && ! (findContainerChain (song, parameter.device).isValid() && parameter.parameter == schema::crossoverParameter)
                        && ! readInPart)
                    {
                        automation.removeChild (i, nullptr);
                        continue;
                    }

                    auto hadPoints = false;
                    auto numPoints = 0;

                    for (auto j = curve.getNumChildren(); --j >= 0;)
                    {
                        auto point = curve.getChild (j);

                        if (! point.hasType (schema::POINT))
                            continue; // one of a type Phazite doesn't know, which stays

                        hadPoints = true;

                        if (const auto number = readNumber (point[schema::pointValue]); ! number.has_value() || ! std::isfinite (*number))
                        {
                            curve.removeChild (j, nullptr);
                            continue;
                        }

                        keepInRange (point, schema::pointValue, 0.0, 1.0, false, 0.0);
                        ++numPoints;
                    }

                    if (hadPoints && numPoints == 0)
                        automation.removeChild (i, nullptr);
                }
            }

            //==============================================================================
            // Every sound file where this computer can look for it without going elsewhere: a path within the project's
            // folder, or a full path of this computer's or another's, as Phazite writes one where a file couldn't be copied
            // in (Import.h: copyIntoProject()), stays as it is. One that no Phazite writes is looked for in the project's
            // Audio folder by its name, as it is, which the Project view lists as missing until it is found: a network
            // share's, which Windows would connect to as it looks, and one that leaves the project's folder with "..". And an
            // audio clip without a file goes.
            void keepPath (juce::ValueTree element, const juce::Identifier& property)
            {
                const auto path = element[property].toString();

                if (path.isEmpty() || folder == juce::File())
                    return;

                if (isNetworkPath (path) ? false : isFullPath (path) || folder.getChildFile (path).isAChildOf (folder))
                    return;

                const auto name = path.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
                element.setProperty (property, "Audio/" + (name.isNotEmpty() && name != ".." ? name : juce::String ("missing")), nullptr);
            }

            void keepPaths (juce::ValueTree tree)
            {
                for (auto i = tree.getNumChildren(); --i >= 0;)
                {
                    auto child = tree.getChild (i);

                    if (! isKnown (child))
                        continue;

                    if (child.hasType (schema::CLIP))
                    {
                        if (child[schema::clipKind].toString() == schema::clipIsAudio && child[schema::clipFile].toString().isEmpty())
                        {
                            tree.removeChild (i, nullptr);
                            continue;
                        }

                        keepPath (child, schema::clipFile);
                    }

                    if (child.hasType (schema::SOUNDS))
                        for (auto p = child.getNumProperties(); --p >= 0;)
                            if (const auto property = child.getPropertyName (p); schema::isSoundFileProperty (property))
                                keepPath (child, property);

                    keepPaths (child);
                }
            }

            void keep()
            {
                auto state = song.getState();
                keepTexts (state, false); // first: before anything reads a number
                keepPlaces (state);
                keepDevices (state);
                keepNumbers (state);

                // The song's next ID past what it holds (not past an ID too large to count on from), counted as the song
                // counts them (findLargestID(), findNextID()), then each its own. A song whose IDs reach the mark (Schema.h:
                // renumberMark) is stamped with its new numbering first (idNumbering, stampNumbering()), then counts on from
                // below maxCountedID, its IDs past it given anew, and past every ID a name
                // below it names (a curve of a device deleted, which goes as the song opens: no new ID may become what it
                // names); every merge waiting in it (MERGING) goes, as its record names IDs inside its text, which nothing
                // translates: a merge under way in a file with IDs past the mark is nobody's work.
                const auto saved = readID (state[schema::nextID]);
                const auto largest = findLargestID (state);
                renumbering = mustGiveIDsAnew (largest, saved);

                if (renumbering)
                {
                    stampNumbering (state);
                    const auto counted = std::max (findLargestID (state, maxCountedID), findLargestName (state, maxCountedID));
                    state.setProperty (schema::nextID, (juce::int64) std::max (counted + 1, saved < maxCountedID ? saved : 0), nullptr);

                    for (auto merging = state.getChildWithName (schema::MERGING); merging.isValid();
                         merging = state.getChildWithName (schema::MERGING))
                        state.removeChild (merging, nullptr);
                }
                else
                {
                    state.setProperty (schema::nextID, (juce::int64) std::max (largest + 1, saved < maxID ? saved : 0), nullptr);
                }

                std::set<std::uint64_t> items;
                keepIDs (state, items); // each new one from the song's next ID, which stays past every ID it holds

                if (renumbering)
                    keepRenumberedNames (state);
                keepSends (state, items);
                keepNames (state, items);
                forgetSidechainLoops (song); // what the model refuses: a device hearing its own track's sound round a loop
                keepKeys (state);
                keepCurves();
                keepPaths (state);
            }
        };
    } // namespace consistent
} // namespace

bool isLeftoverCopy (const juce::File& folder)
{
    return isLeftover (folder);
}

void setLeftoverCopyRemoval (std::function<bool (const juce::File&)> removal)
{
    leftoverCopyRemoval = std::move (removal);
}

void makeExponentsReadable (juce::ValueTree song)
{
    consistent::keepTexts (std::move (song), true);
}

std::optional<double> readLoadedNumber (const juce::var& value)
{
    return consistent::readNumber (value);
}

bool isNumberWrittenAsPhazite (const juce::var& value)
{
    return consistent::isPlainlyAsWritten (value);
}

const std::vector<SwitchProperty>& getSwitchProperties()
{
    using namespace schema;

    // clang-format off
    static const std::vector<SwitchProperty> switches {
        { &PHAZITE,   &loopFollowsSong,        false, true },
        { &PHAZITE,   &loop,                   true,  false },
        { &PHAZITE,   &metronome,              true,  false },
        { &PHAZITE,   &patternsKeepTheirSpeed, true,  false },
        { &PHAZITE,   &punch,                  false, false },
        { &PHAZITE,   &gridSnap,               false, true },
        { &PHAZITE,   &gridAdaptive,           false, true },
        { &PHAZITE,   &gridTriplets,           false, false },
        { &PIANOROLL, &gridSnap,               false, true },
        { &PIANOROLL, &gridAdaptive,           false, true },
        { &PIANOROLL, &gridTriplets,           false, false },
        { &TRACK,     &trackMute,              true,  false },
        { &TRACK,     &trackSolo,              true,  false },
        { &TRACK,     &folderBus,              true,  false },
        { &TRACK,     &trackFrozen,            true,  false },
        { &TRACK,     &trackCollapsed,         false, false },
        { &TRACK,     &trackPinned,            false, false },
        { &TRACK,     &drumMode,               false, false },
        { &MASTER,    &trackCollapsed,         false, false },
        { &LANE,      &laneCollapsed,          true,  false },
        { &DEVICE,    &deviceOff,              true,  false },
        { &DEVICE,    &pluginInstrument,       true,  false },
        { &CONTAINER, &deviceOff,              true,  false },
        { &CONTAINER, &containerInstrument,    true,  false },
        { &CHAIN,     &chainMute,              true,  false },
        { &CHAIN,     &chainSolo,              true,  false },
        { &SEND,      &sendPreFader,           true,  false },
        { &MODULATOR, &deviceOff,              true,  false },
        { &CLIP,      &clipMute,               true,  false },
        { &CLIP,      &clipReversed,           true,  false },
        { &NOTE,      &noteMute,               true,  false },
        { &NOTE,      &noteSlide,              true,  false },
        { &NOTE,      &notePortamento,         true,  false },
    };
    // clang-format on

    return switches;
}

void makeConsistent (Song& song, ReadAs read)
{
    consistent::Keeper { song, song.getFile() != juce::File() ? song.getFile().getParentDirectory() : juce::File(), read == ReadAs::inPart }
        .keep();
}

//==============================================================================
juce::ValueTree makeNewSong()
{
    // A song of its own, numbering its items itself: its tempo map, its master and one empty track.
    Song song { juce::ValueTree (schema::PHAZITE) };
    ensureTempoMap (song);
    getTrackList (song); // with the master's element first
    ensureTracks (song, 1);
    return song.getState();
}

Song::Ptr createSong (const juce::File& file)
{
    return makeSong (makeNewSong(), file);
}

juce::File getDefaultProjectsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Projects");
}

Song::Ptr makeSong (const juce::ValueTree& state, const juce::File& file, const juce::File& cacheFolder)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! state.hasType (schema::PHAZITE))
        return {};

    // What every song has: its tempo map, its track list with the master, and its next ID past every ID it holds; without
    // the frozen files that are gone and the curves of devices gone, which no undo can bring back now; and playing from its
    // start. None of it is the user's change, so it is neither an undo step nor an unsaved change.
    Song::Ptr song (new Song (state));
    song->setFile (file);
    song->setCacheFolder (cacheFolder);
    makeConsistent (*song);
    ensureTempoMap (*song);
    getTrackList (*song);
    countIDs (*song);
    forgetMissingFrozenFiles (*song);
    forgetCurvesOfGoneDevices (*song);
    song->getState().removeProperty (schema::playStart, nullptr);
    song->getUndoManager().clearUndoHistory();
    return song;
}

std::unique_ptr<Project> Project::create (const juce::File& parentFolder, const juce::String& name, juce::String& error,
                                          const std::function<void (Song&)>& addContent)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto folder = makeProjectFolder (parentFolder, name, error);

    if (folder == juce::File())
        return {};

    const auto file = folder.getChildFile (folder.getFileName() + fileExtension);
    auto song = makeSong (makeNewSong(), file, getCacheFolder (file));

    // What it starts with goes into its first save: no undo step, and no unsaved change.
    if (addContent != nullptr)
    {
        addContent (*song);
        song->getUndoManager().clearUndoHistory();
    }

    std::unique_ptr<Project> project (new Project (std::move (song), file));

    for (const auto& subfolder :
         { project->getAudioFolder(), project->getGeneratedFolder(), project->getRendersFolder(), project->getBackupsFolder() })
        subfolder.createDirectory();

    // A first save that fails takes the folder made for it along, the project first, so that the name can be tried again:
    // nothing else was there, as makeProjectFolder() makes only a folder that wasn't.
    if (const auto result = project->save(); result.failed())
    {
        error = result.getErrorMessage();
        project.reset();
        folder.deleteRecursively();
        return {};
    }

    return project;
}

std::unique_ptr<Project> Project::open (const juce::File& projectFile, bool restoreAutosave, juce::String& error, juce::String* notRestored)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! projectFile.existsAsFile())
    {
        error = "The project file " + projectFile.getFullPathName() + " doesn't exist.";
        return {};
    }

    // An autosave that doesn't read (cut short by a crash as it was written, say) gives way to the project file.
    const auto autosaveFile = getAutosaveFile (projectFile);
    auto state = restoreAutosave && autosaveFile.existsAsFile() ? readSong (autosaveFile) : juce::ValueTree();
    const auto useAutosave = state.isValid();

    if (restoreAutosave && autosaveFile.existsAsFile() && ! useAutosave && notRestored != nullptr)
        *notRestored = "The changes Phazite saved automatically couldn't be read, so \"" + projectFile.getFileNameWithoutExtension()
                       + "\" opened as it was last saved.";

    auto song = makeSong (useAutosave ? state : readSong (projectFile), projectFile, getCacheFolder (projectFile));

    if (song == nullptr)
    {
        error = "Phazite couldn't read the project file " + projectFile.getFullPathName() + ".";
        return {};
    }

    std::unique_ptr<Project> project (new Project (std::move (song), projectFile));

    if (useAutosave)
        project->forgetSavedState(); // the restored changes aren't in the project file yet

    return project;
}

std::unique_ptr<Project> Project::openWithSong (const juce::File& projectFile, const juce::ValueTree& state, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto song = makeSong (state.createCopy(), projectFile, getCacheFolder (projectFile));

    if (song == nullptr)
    {
        error = "Phazite couldn't read that copy of the project " + projectFile.getFileNameWithoutExtension() + ".";
        return {};
    }

    std::unique_ptr<Project> project (new Project (std::move (song), projectFile));
    project->forgetSavedState(); // not in the project file until it is saved
    return project;
}

std::pair<juce::File, juce::ValueTree> Project::readNewestBackup (const juce::File& projectFile)
{
    // Its backups, oldest first, and its autosave among them by its time (before a backup of the same time).
    auto copies = findBackups (projectFile);

    if (const auto autosave = getAutosaveFile (projectFile); autosave.existsAsFile())
    {
        const auto time = autosave.getLastModificationTime().toMilliseconds();
        copies.insert (std::find_if (copies.begin(), copies.end(), [time] (const Backup& b) { return b.time >= time; }),
                       Backup { autosave, time, 0 });
    }

    for (auto copy = copies.rbegin(); copy != copies.rend(); ++copy)
        if (auto song = readSong (copy->file); song.isValid())
            return { copy->file, song };

    return {};
}

bool Project::hasNewerAutosave (const juce::File& projectFile)
{
    const auto autosaveFile = getAutosaveFile (projectFile);
    return autosaveFile.existsAsFile() && autosaveFile.getLastModificationTime() >= projectFile.getLastModificationTime();
}

juce::File Project::getAutosaveFile (const juce::File& projectFile)
{
    return projectFile.getSiblingFile ("Backups").getChildFile (projectFile.getFileNameWithoutExtension() + " (autosave)" + fileExtension);
}

//==============================================================================
class Project::BackgroundWriter : private juce::Thread
{
public:
    BackgroundWriter()
        : juce::Thread ("Phazite autosave")
    {
    }

    // A write under way is never cut short (run() doesn't check), however long the drive takes, as the autosave a recording's
    // end starts may meet a drive that stalls: a time limit here would end the thread with Windows' TerminateThread, which can
    // leave the file broken and a lock of the heap's or the C runtime's held.
    ~BackgroundWriter() override { stopThread (-1); }

    bool isBusy() const noexcept { return busy.load(); }
    bool takeFailure() noexcept { return failed.exchange (false); }
    juce::int64 getWrittenAt() const noexcept { return writtenAt.load(); } // the last whole write, or 0
    void forgetWritten() noexcept { writtenAt = 0; }
    void wait() { waitForThreadToExit (-1); } // the write under way, which a save of the same file mustn't overlap

    // Writes a state that nothing else changes (a copy) into a file.
    void write (juce::ValueTree copy, const juce::File& target)
    {
        jassert (! isBusy());
        wait(); // the last write's thread, which has finished its work, is gone
        state = std::move (copy);
        file = target;
        busy = true;
        startThread();
    }

private:
    juce::ValueTree state;
    juce::File file;
    std::atomic<bool> busy { false }, failed { false };
    std::atomic<juce::int64> writtenAt { 0 }; // in milliseconds since 1970

    void run() override
    {
        if (writeProjectFile (state, file).wasOk())
            writtenAt = juce::Time::currentTimeMillis();
        else
            failed = true;

        state = {};
        busy = false;
    }
};

namespace
{
    // Why nothing writes the song while a version is open in its place that can't go yet (VersionsModel.h: backToNow()).
    const char* const notWhileShowing = "A version is open in the song's place and can't go back to Now yet (a render has it, "
                                        "automation is being written, or the branch a change makes is being written): let that "
                                        "finish, then save.";
} // namespace

Project::Project (Song::Ptr s, const juce::File& f)
    : song (std::move (s)),
      songState (song->getState()),
      file (f),
      autosaveWriter (std::make_unique<BackgroundWriter>()),
      namesWriter (std::make_unique<BackgroundWriter>())
{
    // The song's cache folder, where its frozen tracks' files are, is the project's Cache folder. Closing the project empties
    // it of what nothing uses; opening it doesn't, as clips find the renders they use only when they play.
    song->setCacheFolder (getCacheFolder());
    rememberSavedState(); // what opening made of it isn't a change
    changesOutsideHistory = getOwnChangesOutsideHistory();
    songState.addListener (this);

    // While the song shows another (a version open: Song::setShowingOther()), its unsaved mark is held as it was as that
    // began, compared now: what the other changes is noted, and compared once it went (compareWithSavedState()), and neither
    // its changes nor its plug-ins' own changes meanwhile count as the project's (a save in the background finishes as saved).
    song->setShowingHeard (
        [this] (bool showing)
        {
            if (showing)
            {
                compareWithSavedState();
                outsideAtShowing = song->getChangesOutsideHistory();
            }
            else
            {
                outsideWhileShowing += song->getChangesOutsideHistory() - outsideAtShowing;
            }
        });
}

int Project::getOwnChangesOutsideHistory() const
{
    const auto all = song->getChangesOutsideHistory();
    return all - outsideWhileShowing - (song->isShowingOther() ? all - outsideAtShowing : 0);
}

Project::~Project()
{
    JUCE_ASSERT_MESSAGE_THREAD
    song->setShowingHeard (nullptr);

    // The versions' names, if they still wait to be written (versionsMoved()): at once, as the project closes.
    stopTimer();
    cancelPendingUpdate();
    const auto namesFailedNow = takeNamesWritten(); // a write of them under way finished first, pending again if it failed
    namesWriter = nullptr;
    autosaveWriter->wait();
    finishBackgroundSave(); // a save in the background under way, which took them along: pending again if it failed
    compareWithSavedState();

    if (namesPending && ! changedSinceSave && savedState.isValid())
    {
        const auto state = song->getState();

        for (const auto* name : { &schema::versionsBranch, &schema::versionsBase })
            if (const auto* now = state.getPropertyPointer (*name))
                savedState.setProperty (*name, *now, nullptr);
            else
                savedState.removeProperty (*name, nullptr);

        if (writeProjectFile (savedState, file).wasOk())
            namesPending = false;
    }

    // A write of them that failed, and wasn't made good as it closed: told, once, as the timer tells one.
    if (namesFailedNow && namesPending)
        tellNamesFailed();

    autosaveWriter = nullptr; // before the autosave goes
    songState.removeListener (this);
    getAutosaveFile (file).deleteFile();
    purgeCache (*song);
}

bool Project::hasUnsavedChanges()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();
    return changedSinceSave;
}

bool Project::checkUnsavedChanges()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! hasUnsavedChanges() || song->isShowingOther()) // its mark held (compareWithSavedState())
        return changedSinceSave;

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true); // what saving stores isn't a change by the user
        storeForSaving (*song);
    }

    if (! holdTheSameProject (song->getState(), savedState))
        return true; // changedSinceSave stays as it was, true

    // The song is as saved after all.
    changedSinceSave = changedSinceAutosave = false;
    changesOutsideHistory = getOwnChangesOutsideHistory();
    clearToCompare();
    return false;
}

juce::Result Project::save()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // What is being tried in place (Swap) is kept first, as its own step: the file holds what is heard. Another song shown for
    // a while (a version open) goes first: the file holds the song's own, never the other (which can't go while a render has
    // it, whose step lands in it as it ends).
    if (! Trial::putBackShowing (song->getUndoManager()))
        return juce::Result::fail (notWhileShowing);

    Trial::keepOpenTrial (song->getUndoManager());

    autosaveWriter->wait(); // an autosave being written, which this makes old
    takeNamesWritten();     // and the versions' names being written into the file, which this writes too
    backgroundSave = {};    // and a save in the background, which this overtakes
    backUpProjectFile();
    backedUpAt = getBackupTime();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true); // what saving stores isn't a change by the user
        storeForSaving (*song);
        compareStoredStates(); // should the write fail, which leaves them out of the file (one that works forgets this)

        if (const auto written = writeProjectFile (song->getState(), file); written.failed())
            return juce::Result::fail ("Phazite couldn't save the project to " + file.getFullPathName() + ": " + written.getErrorMessage());
    }

    changedSinceSave = false;
    changedSinceAutosave = false;
    namesPending = namesFailed = false;                    // the versions' names went with the song
    changesOutsideHistory = getOwnChangesOutsideHistory(); // stored in the song as it saved
    rememberSavedState();                                  // what the file holds now
    getAutosaveFile (file).deleteFile();
    autosavedAt = 0;
    autosaveWriter->forgetWritten();
    return juce::Result::ok();
}

juce::Result Project::autosave()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! Trial::putBackShowing (song->getUndoManager())) // as save() does; nothing while it can't
        return juce::Result::ok();

    autosaveWriter->wait();
    compareWithSavedState();

    if (! changedSinceAutosave)
        return juce::Result::ok();

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);
        compareStoredStates(); // which the project file doesn't hold

        if (const auto written = writeProjectFile (song->getState(), target); written.failed())
            return juce::Result::fail ("Phazite couldn't write the autosave " + target.getFullPathName() + ": "
                                       + written.getErrorMessage());
    }

    changedSinceAutosave = false;
    autosavedAt = juce::Time::currentTimeMillis();
    return juce::Result::ok();
}

bool Project::hasChangesToAutosave()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();
    return changedSinceAutosave;
}

void Project::autosaveInBackground()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! Trial::putBackShowing (song->getUndoManager())) // as save() does; nothing while it can't
        return;

    compareWithSavedState();

    if (! changedSinceAutosave || autosaveWriter->isBusy())
        return;

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);
        compareStoredStates(); // which the project file doesn't hold
        autosaveWriter->write (song->getState().createCopy(), target);
    }

    changedSinceAutosave = false;
}

bool Project::isAutosaving() const
{
    return autosaveWriter->isBusy();
}

void Project::saveInBackground()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! Trial::putBackShowing (song->getUndoManager())) // as save() does; nothing while it can't
        return;

    compareWithSavedState();

    // Also for the versions' names alone, where a write of them failed (namesPending).
    if ((! changedSinceSave && ! (namesPending && namesFailed)) || autosaveWriter->isBusy() || backgroundSave.isValid()
        || namesWriter->isBusy())
        return;

    // The previous file into the backups every few minutes, not at every save.
    if (const auto now = getBackupTime(); now - backedUpAt >= (juce::int64) backupMinutes * 60 * 1000)
    {
        backUpProjectFile();
        backedUpAt = now;
    }

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);
        backgroundSave = song->getState().createCopy();
        autosaveWriter->write (backgroundSave.createCopy(), file);
    }

    namesPending = false; // in what it writes; pending again if the write fails (finishBackgroundSave())

    changesAtBackgroundSave = changeCount;
    outsideAtBackgroundSave = getOwnChangesOutsideHistory();
}

bool Project::finishBackgroundSave()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! backgroundSave.isValid() || autosaveWriter->isBusy())
        return true;

    const auto written = std::exchange (backgroundSave, {});

    if (autosaveWriter->takeFailure())
    {
        namesPending = namesPending || written.hasProperty (schema::versionsBase) || written.hasProperty (schema::versionsBranch);
        return false; // its changes are still to save
    }

    // What the file holds now: saved, unless the song changed since it was copied. What is noted to compare stays, compared
    // with this copy at the next check: what a store wrote since without a notification (compareStoredStates()).
    savedState = written;
    changesOutsideHistory = outsideAtBackgroundSave;

    if (changeCount == changesAtBackgroundSave && getOwnChangesOutsideHistory() == outsideAtBackgroundSave)
    {
        changedSinceSave = changedSinceAutosave = false;
        getAutosaveFile (file).deleteFile();
        autosavedAt = 0;
        autosaveWriter->forgetWritten();
    }

    return true;
}

std::optional<juce::Time> Project::getLastAutosave() const
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (const auto latest = std::max (autosavedAt, autosaveWriter->getWrittenAt()); latest > 0)
        return juce::Time (latest);

    return std::nullopt;
}

bool Project::takeAutosaveFailure()
{
    if (! autosaveWriter->takeFailure())
        return false;

    changedSinceAutosave = true; // written again at the next
    return true;
}

juce::File Project::saveCopyAs (const juce::File& parentFolder, const juce::String& name, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! Trial::putBackShowing (song->getUndoManager())) // as save() does
    {
        error = notWhileShowing;
        return {};
    }

    if (takeNamesWritten()) // the versions' names being written into the file, which the copy is made from, written first
        tellNamesFailed();
    const auto newFile = makeFolderWithFiles (parentFolder, name, error);

    if (newFile == juce::File())
        return {};

    const juce::ScopedValueSetter<bool> writing (isWriting, true);
    storeForSaving (*song);
    compareStoredStates(); // which this project's file doesn't hold

    if (const auto written = writeProjectFile (song->getState(), newFile); written.failed())
    {
        newFile.getParentDirectory().deleteRecursively(); // so that the name can be tried again
        error = "Phazite couldn't write the project file " + newFile.getFullPathName() + ": " + written.getErrorMessage();
        return {};
    }

    return newFile;
}

//==============================================================================
// A first save's copy of the project's folders on a background thread, as its progress window shows it: all but the
// versions on a thread of its own, then the versions by what writes them (VersionsCopier: the history's thread), each piece
// counted and Cancel stopping either; then, on the message thread, the project moved into it (finishMoveTo()). Deleted
// before then, it stops the copy and deletes what was copied.
class Project::FolderCopy final : public TrackRender::Impl,
                                  private juce::Timer
{
public:
    FolderCopy (Project& p, juce::File into, juce::File becomes, FoldersToCopy what, juce::int64 versionsBytes,
                std::function<void (const juce::Result&)> callback)
        : project (p),
          copy (std::move (into)),
          folder (std::move (becomes)),
          list (std::move (what)),
          bytes (list.bytes + versionsBytes),
          onFinished (std::move (callback))
    {
        const auto from = project.getFolder();
        const auto versionsWithTheRest = project.versionsCopier.copy == nullptr;
        jobs.add (
            [this, from, versionsWithTheRest] (const BackgroundJobs::ShouldStop& shouldStop)
            {
                result = copyFolders (from, copy, list, shouldStop, state->copied);

                if (result.wasOk() && versionsWithTheRest)
                    result = copyVersions (from, copy, shouldStop, state->copied);

                done = true;
            });

        startTimer (50);
    }

    ~FolderCopy() override
    {
        stopTimer();
        *alive = false;
        jobs.removeAll (true); // asked to stop between pieces, and waited for

        // The versions' copy on the history's thread, asked to stop and waited for while it runs (no more than a piece).
        state->stop = true;

        if (state->running.load())
            state->finished.wait (-1);

        if (! finished)
            copy.deleteRecursively();

        endVersionsCopy();
    }

    float getProgress() const override
    {
        return bytes > 0 ? juce::jlimit (0.0f, 1.0f, (float) ((double) state->copied.load() / (double) bytes)) : 1.0f;
    }

    juce::String getTitle() const override { return "Saving " + folder.getFileName(); }

private:
    // What the versions' copy on another thread shares with this, which may go first.
    struct State
    {
        std::atomic<juce::int64> copied { 0 };
        std::atomic<bool> stop { false }, running { false };
        juce::WaitableEvent finished { true };
    };

    Project& project;
    const juce::File copy, folder;
    const FoldersToCopy list;
    const juce::int64 bytes; // the versions' too
    const std::function<void (const juce::Result&)> onFinished;
    const std::shared_ptr<State> state = std::make_shared<State>();
    const std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    std::atomic<bool> done { false };
    juce::Result result = juce::Result::ok(); // the job's, read once done is set
    bool finished = false, versionsCopying = false;
    BackgroundJobs jobs { "Phazite first save", 1, juce::Thread::Priority::normal };

    void timerCallback() override
    {
        if (! done)
            return;

        stopTimer();

        if (result.failed() || project.versionsCopier.copy == nullptr)
        {
            finish (result);
            return;
        }

        // The versions, by the history's thread, after every write it was given; what it is asked for from now waits.
        versionsCopying = true;
        project.versionsCopier.copy (
            [state = state, from = project.getFolder(), to = copy] (const BackgroundJobs::ShouldStop& shouldStop)
            {
                // Running, then the stop looked at: this sees a stop asked for first, or ~FolderCopy() sees it running.
                state->running = true;
                const auto stopped = [&state, &shouldStop] { return state->stop.load() || shouldStop(); };
                const auto copied =
                    stopped() ? juce::Result::fail ("The copy was cancelled.") : copyVersions (from, to, stopped, state->copied);
                state->running = false;
                state->finished.signal();
                return copied;
            },
            [this, alive = alive] (const juce::Result& copied)
            {
                if (*alive)
                    finish (copied);
            });
    }

    void finish (const juce::Result& copied)
    {
        finished = true;
        auto moved = copied.wasOk() ? project.finishMoveTo (copy, folder) : copied;

        if (moved.failed())
            copy.deleteRecursively();

        endVersionsCopy(); // the history follows the project, wherever it is now, and what waited goes on

        // Last, through a copy: the progress window may delete this, and the function with it, as it is told.
        if (const auto callback = onFinished; callback != nullptr)
            callback (moved);
    }

    void endVersionsCopy()
    {
        if (std::exchange (versionsCopying, false) && project.versionsCopier.end != nullptr)
            project.versionsCopier.end();
    }
};

std::unique_ptr<TrackRender> Project::moveTo (const juce::File& parentFolder, const juce::String& name,
                                              std::function<void (const juce::Result&)> onFinished)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! Trial::putBackShowing (song->getUndoManager())) // as save() does
    {
        onFinished (juce::Result::fail (notWhileShowing));
        return {};
    }

    autosaveWriter->wait(); // its autosave goes with it
    if (takeNamesWritten()) // and its file whole, the versions' names in it, before the copy reads it
        tellNamesFailed();

    juce::String error;
    const auto folder = findNewProjectFolder (parentFolder, name, error);
    const auto copy = folder != juce::File() ? makeCopyFolder (folder, leftoverRemovals, error) : juce::File();

    if (copy == juce::File())
    {
        onFinished (juce::Result::fail (error));
        return {};
    }

    // The versions last (copyVersions()), counted with the rest. Always in the background, however little there is: the
    // message thread waits neither for the copy nor for the history's writes the versions' copy comes after (a commit being
    // written), and a file another program holds a moment is tried again there, which it can't be on the message thread.
    auto list = listFoldersToCopy (getFolder(), FoldersListed::allButVersions);
    const auto versionsBytes = listFoldersToCopy (getFolder(), FoldersListed::versions).bytes;
    return std::make_unique<TrackRender> (
        std::make_unique<FolderCopy> (*this, copy, folder, std::move (list), versionsBytes, std::move (onFinished)));
}

void Project::setCopyRetryHook (std::function<void (int tries)> hook)
{
    const std::scoped_lock held (retryHookLock);
    retryHook = hook != nullptr ? std::make_shared<const std::function<void (int)>> (std::move (hook)) : nullptr;
}

void Project::setBackupTime (juce::int64 now)
{
    JUCE_ASSERT_MESSAGE_THREAD
    backupTime = now;
}

juce::Result Project::finishMoveTo (const juce::File& copy, const juce::File& folder)
{
    JUCE_ASSERT_MESSAGE_THREAD
    autosaveWriter->wait();
    if (takeNamesWritten()) // none into the file left behind once the project is in the copy
        tellNamesFailed();

    if (const auto placed = putInPlace (copy, folder); placed.failed())
        return placed;

    const auto oldFile = file;
    setFile (folder.getChildFile (folder.getFileName() + fileExtension));

    if (const auto result = save(); result.failed())
    {
        setFile (oldFile);
        folder.deleteRecursively(); // so that the name can be tried again
        return result;
    }

    getAutosaveFile (oldFile).deleteFile(); // what it held is saved now
    return juce::Result::ok();
}

juce::File Project::makeFolderWithFiles (const juce::File& parentFolder, const juce::String& name, juce::String& error) const
{
    const auto folder = findNewProjectFolder (parentFolder, name, error);
    const auto copy = folder != juce::File() ? makeCopyFolder (folder, leftoverRemovals, error) : juce::File();

    if (copy == juce::File())
        return {};

    // A failure removes the half-made copy, so that the name can be tried again.
    std::atomic<juce::int64> copied { 0 };
    auto result = copyFolders (getFolder(), copy, listFoldersToCopy (getFolder()), [] { return false; }, copied);

    if (result.wasOk())
        result = putInPlace (copy, folder);

    if (result.failed())
    {
        copy.deleteRecursively();
        error = result.getErrorMessage();
        return {};
    }

    return folder.getChildFile (folder.getFileName() + fileExtension);
}

void Project::setFile (const juce::File& newFile)
{
    // The song finds the files it plays by their paths relative to it; what plays it finds them again.
    file = newFile;
    song->setFile (newFile);
    song->setCacheFolder (getCacheFolder()); // the copy of the old one

    if (auto* session = findSession (*song))
        session->update();
}

void Project::backUpProjectFile()
{
    if (! file.existsAsFile())
        return;

    const auto backups = getBackupsFolder();
    backups.createDirectory();

    // The project's backups, oldest first (Project.h names them); nothing else there is one.
    auto versions = findBackups (file);

    // A file the newest backup holds already is no new backup (ten saves without a change make one), and its versions
    // are thinned as they are. One that can't be made is told, once until one is made again; the save goes on without it.
    if (versions.empty() || ! file.hasIdenticalContentTo (versions.back().file))
    {
        // Named, and dated, by when this version was saved, as a copy doesn't keep that everywhere (on Linux JUCE's copy is
        // a new file). A file that can't be read is no copy either, said before JUCE tries: on Linux its copy reads from a
        // stream it hasn't checked, and asserts. JUCE workaround 36 (docs/juce-workarounds.md).
        const auto saved = file.getLastModificationTime();
        const auto stem = getBackupStem (file, saved);
        auto copy = backups.getChildFile (stem + fileExtension);
        int number = 1;

        while (copy.exists())
            copy = backups.getChildFile (stem + "_" + juce::String (++number) + fileExtension);

        if (! file.hasReadAccess() || ! file.copyFileTo (copy))
        {
            if (! std::exchange (backupFailed, true) && onBackupFailed != nullptr)
                onBackupFailed ("Phazite couldn't copy the project's last saved version into " + backups.getFullPathName()
                                + ". It saved the project without a backup; check that that folder can be written to.");

            return;
        }

        copy.setLastModificationTime (saved);
        versions.push_back ({ copy, saved.toMilliseconds(), number });
        std::sort (versions.begin(), versions.end(), isOlder); // newest, unless the computer's clock went back
    }

    backupFailed = false;

    // Kept by age, as Local History is, and the newest of those older than that too (Project.h). Oldest first, so that of
    // two of one time keepByAge() keeps the later one.
    std::vector<juce::int64> times;

    for (const auto& version : versions)
        times.push_back (version.time);

    const auto now = getBackupTime();
    auto keep = keepByAge (times, now);

    for (size_t i = versions.size(); i-- > 0;)
    {
        if (now - versions[i].time > keptByAge::dailyFor)
        {
            keep[i] = true; // newest first: the newest of them
            break;
        }
    }

    for (size_t i = 0; i < versions.size(); ++i)
        if (! keep[i])
            versions[i].file.deleteFile();
}

void Project::clearToCompare()
{
    mayHaveChanged = comparingWhole = false;
    toCompare.clear();
}

void Project::rememberSavedState()
{
    savedState = song->getState().createCopy();
    clearToCompare();
}

void Project::forgetSavedState()
{
    // No song to compare with, so none is the same (holdTheSameProject()): unsaved, whatever closing compares, until a save
    // remembers what it wrote.
    savedState = {};
    clearToCompare();
    changedSinceSave = changedSinceAutosave = true;
}

namespace
{
    std::atomic<juce::int64> elementsComparedWithSaved { 0 };

    // An element's place among those of its parent's that count (what holdTheSameProject() compares), or -1.
    int indexAmongCounted (const juce::ValueTree& parent, const juce::ValueTree& element)
    {
        auto index = 0;

        for (const auto& child : parent)
        {
            if (child == element)
                return index;

            if (! schema::isUncountedElement (parent, child))
                ++index;
        }

        return -1;
    }

    juce::ValueTree getCountedChild (const juce::ValueTree& parent, int index)
    {
        for (const auto& child : parent)
            if (! schema::isUncountedElement (parent, child) && index-- == 0)
                return child;

        return {};
    }
} // namespace

void Project::compareStoredStates()
{
    // Noted while the project has unsaved changes too: a save in the background under way (finishBackgroundSave()) may yet
    // make the copy it took the saved one, which this store came after.
    mayHaveChanged = true;
    const auto state = song->getState();

    for (const auto* type : { &schema::TRACKS, &schema::MASTER })
        if (const auto element = state.getChildWithName (*type);
            element.isValid()
            && std::none_of (toCompare.begin(), toCompare.end(),
                             [&element] (const ToCompare& each) { return each.element == element && each.property.isNull(); }))
            toCompare.push_back ({ element, {} });
}

juce::int64 Project::countElementsCompared()
{
    return elementsComparedWithSaved.load (std::memory_order_relaxed);
}

juce::ValueTree Project::findSavedCopy (const juce::ValueTree& element) const
{
    // Its places from the song down, among the elements that count: what a view state element added (a track's LANES) moves
    // in the song moves nothing in the copy.
    const auto state = song->getState();
    std::vector<int> places;

    for (auto e = element; e != state; e = e.getParent())
        places.push_back (indexAmongCounted (e.getParent(), e));

    auto copy = savedState;

    for (auto place = places.rbegin(); place != places.rend() && copy.isValid(); ++place)
        copy = getCountedChild (copy, *place);

    return copy;
}

bool Project::isAsSaved (const ToCompare& entry) const
{
    // One no longer in the song: what it was taken out of notified, and is compared whole.
    const auto state = song->getState();

    if (! (entry.element == state || entry.element.isAChildOf (state)))
        return true;

    // At the same place in the copy: where an element was added, removed or moved, its parent notified, and is compared
    // whole, which finds the difference; or there is none, and the places below are the same in both.
    const auto copy = findSavedCopy (entry.element);

    if (! copy.isValid() || copy.getType() != entry.element.getType())
        return false;

    if (entry.property.isNull())
        return holdTheSameProject (entry.element, copy);

    const auto* now = entry.element.getPropertyPointer (entry.property);
    const auto* saved = copy.getPropertyPointer (entry.property);
    return now == nullptr || saved == nullptr ? now == saved : isSameValue (*now, *saved);
}

void Project::compareWithSavedState()
{
    // Nothing while the song shows another for a while (Song::isShowingOther(): a version open), whose changes are no unsaved
    // ones: the mark stays what it was as that began (compared then), and what changed meanwhile is compared once it went.
    if (song->isShowingOther())
        return;

    // A plug-in's own change, which a save stores in the song.
    if (const auto outside = getOwnChangesOutsideHistory(); outside != changesOutsideHistory)
    {
        changesOutsideHistory = outside;
        changedSinceSave = changedSinceAutosave = true;
    }

    // Nothing to compare while the project has unsaved changes: what was noted waits, for a save in the background that may
    // make what it wrote the saved copy (finishBackgroundSave()); a save clears it.
    if (! mayHaveChanged || changedSinceSave)
        return;

    const auto same = std::all_of (toCompare.begin(), toCompare.end(), [this] (const ToCompare& entry) { return isAsSaved (entry); });
    clearToCompare();

    if (! same)
    {
        changedSinceSave = true;
        changedSinceAutosave = true;
    }
}

void Project::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (isWriting)
        return;

    const auto state = song->getState();

    if (tree == state && schema::isVersionsProperty (tree, property))
    {
        versionsMoved();
        return;
    }

    if (! (tree == state || tree.isAChildOf (state)) || ! schema::isCounted (tree, property))
        return;

    // Another song shown in place (a version open) changes nothing of the project's: noted below, to compare once it went.
    if (! song->isShowingOther())
        ++changeCount;

    if (changedSinceSave)
    {
        changedSinceAutosave = true;
        return;
    }

    // Perhaps nothing changed: compared with savedState when asked, what notified (a property, or an element whose elements
    // changed) or, past mostToCompare of them, the track (or the song's element) each is in, whole.
    mayHaveChanged = true;

    const auto wholeAround = [&state] (const juce::ValueTree& changed)
    {
        auto element = changed;

        // Up to its track, or the song's element it is in.
        for (auto parent = element.getParent(); parent.isValid() && parent != state && ! parent.hasType (schema::TRACKS);
             parent = element.getParent())
            element = parent;

        return element;
    };

    // Looked for from the last: one edit's notifications come one after another, mostly from one element.
    const auto add = [this] (const ToCompare& entry)
    {
        if (std::none_of (toCompare.rbegin(), toCompare.rend(),
                          [&entry] (const ToCompare& each) { return each.element == entry.element && each.property == entry.property; }))
            toCompare.push_back (entry);
    };

    if (! comparingWhole && toCompare.size() >= mostToCompare)
    {
        comparingWhole = true;

        // Those taken out of the song since they notified are compared no more (isAsSaved()): let go of here.
        for (const auto& each : std::exchange (toCompare, {}))
            if (each.element == state || each.element.isAChildOf (state))
                add ({ wholeAround (each.element), {} });
    }

    add (comparingWhole ? ToCompare { wholeAround (tree), {} } : ToCompare { tree, property });
}

void Project::versionsMoved()
{
    // With unsaved changes, the file holds an older song, which the names it holds go with: these go with the song at its
    // next save, and into the autosave at the next. Without, into the file in the background, once nothing else writes it.
    namesPending = true;
    compareWithSavedState();

    if (changedSinceSave || ! savedState.isValid())
        changedSinceAutosave = true;
    else
        triggerAsyncUpdate();
}

void Project::handleAsyncUpdate()
{
    writeVersionsNames();
}

void Project::writeVersionsNames()
{
    if (! namesPending)
        return;

    compareWithSavedState();

    if (changedSinceSave || ! savedState.isValid())
    {
        changedSinceAutosave = true; // the next save writes them, and the next autosave holds them
        return;
    }

    // Not over another write of the file (a save in the background, or the names before): looked at again in a moment.
    if (autosaveWriter->isBusy() || backgroundSave.isValid() || namesWriter->isBusy())
    {
        startTimer (20);
        return;
    }

    // The file as it was saved, the song's names in it as they are now: no backup, as the song in it is the one backed up.
    const auto state = song->getState();

    for (const auto* name : { &schema::versionsBranch, &schema::versionsBase })
    {
        if (const auto* now = state.getPropertyPointer (*name))
            savedState.setProperty (*name, *now, nullptr);
        else
            savedState.removeProperty (*name, nullptr);
    }

    namesWriter->write (savedState.createCopy(), file);
    namesPending = false;
    namesWriting = true;
    startTimer (20);
}

bool Project::takeNamesWritten()
{
    // Its failure taken here, so that the timer tells none after a save that wrote them (the review of S4.5); said by that
    // save if it fails too, as pending, and by the others that take it (tellNamesFailed()). Whether it failed.
    namesWriter->wait();

    if (! std::exchange (namesWriting, false) || ! namesWriter->takeFailure())
        return false;

    namesPending = true;
    changedSinceAutosave = true;
    return true;
}

void Project::tellNamesFailed()
{
    if (! std::exchange (namesFailed, true) && onVersionsNamesFailed != nullptr)
        onVersionsNamesFailed ("Phazite couldn't write which version the song grew from into " + file.getFullPathName()
                               + ". The next save writes it; until then, the project file may open against an older version.");
}

void Project::timerCallback()
{
    // A names write over: one that failed leaves them pending, for the next save to write, and is told once.
    if (namesWriting && ! namesWriter->isBusy())
    {
        if (takeNamesWritten())
            tellNamesFailed();
        else
            namesFailed = false;
    }

    if (namesWriting)
        return;

    stopTimer();

    if (namesPending && ! namesFailed)
        writeVersionsNames(); // what waited for another write of the file
}

bool Project::hasVersionsNamesWritten() const
{
    return ! namesPending && ! namesWriting;
}

void Project::markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! schema::isUncountedElement (parent, child))
        markChanged (parent);
}

bool Project::holdTheSameProject (const juce::ValueTree& a, const juce::ValueTree& b)
{
    elementsComparedWithSaved.fetch_add (1, std::memory_order_relaxed);

    if (a.getType() != b.getType())
        return false;

    // Each of a's counted properties is in b with the same value, and b has no others.
    int numCounted = 0;

    for (int i = 0; i < a.getNumProperties(); ++i)
    {
        const auto name = a.getPropertyName (i);

        if (schema::isUncountedProperty (a, name))
            continue;

        const auto* other = b.getPropertyPointer (name);

        if (other == nullptr || ! isSameValue (a[name], *other))
            return false;

        ++numCounted;
    }

    for (int i = 0; i < b.getNumProperties(); ++i)
        if (! schema::isUncountedProperty (b, b.getPropertyName (i)))
            --numCounted;

    if (numCounted != 0)
        return false;

    // The counted elements, in the same order, holding the same.
    for (int i = 0, j = 0;; ++i, ++j)
    {
        while (i < a.getNumChildren() && schema::isUncountedElement (a, a.getChild (i)))
            ++i;

        while (j < b.getNumChildren() && schema::isUncountedElement (b, b.getChild (j)))
            ++j;

        if (i == a.getNumChildren() || j == b.getNumChildren())
            return i == a.getNumChildren() && j == b.getNumChildren();

        if (! holdTheSameProject (a.getChild (i), b.getChild (j)))
            return false;
    }
}

//==============================================================================
AutosaveTimer::AutosaveTimer (Project& p, int interval, int firstDelay)
    : project (p),
      intervalSeconds (interval),
      firstDelaySeconds (firstDelay)
{
    startTimer (250);
}

void AutosaveTimer::setAutoSave (bool shouldAutoSave)
{
    autoSave = shouldAutoSave;
    changedSince = 0;
}

AutosaveTimer::~AutosaveTimer()
{
    stopTimer();
}

void AutosaveTimer::timerCallback()
{
    const auto recording = isRecording (project.getSong());
    const auto recordingEnded = wasRecording && ! recording;
    wasRecording = recording;

    // A write that finished, of the project file (Auto Save) or of the autosave: one that failed counts its changes as not
    // written, so they are written again, and after a few in a row the app says so.
    if (const auto autosaving = project.isAutosaving(); wasAutosaving && ! autosaving)
    {
        const auto wasSave = project.hasBackgroundSave();

        if (wasSave ? project.finishBackgroundSave() : ! project.takeAutosaveFailure())
        {
            failures = 0;
        }
        else
        {
            juce::Logger::writeToLog ((wasSave ? "Auto save failed: " + project.getFile().getFullPathName()
                                               : "Autosave failed: " + Project::getAutosaveFile (project.getFile()).getFullPathName()));

            if (++failures == failuresToReport && onRepeatedFailures != nullptr)
                onRepeatedFailures();
        }
    }

    wasAutosaving = project.isAutosaving();

    // Never halfway through a drag, nor while something is tried in place (Swap), which may yet be put back.
    if (recording || wasAutosaving || Gesture::isInProgress (project.getSong()) || Trial::isInProgress (project.getSong()))
        return;

    // Auto Save: a moment after the last change, as VS Code's afterDelay; while the song plays, at most every few seconds.
    if (autoSave)
    {
        const auto now = devices::Clock::getMillisecondCounter();

        if (const auto changes = project.getChangeCount() + project.getSong().getChangesOutsideHistory(); changes != lastChangeCount)
        {
            lastChangeCount = changes;
            lastChangeAt = now;
        }

        if (! project.hasUnsavedChanges())
            return;

        const auto quiet = now - lastChangeAt >= (juce::uint32) autoSaveDelayMs;
        const auto playingTooSoon = isSongPlaying (project.getSong()) && now - savedAt < (juce::uint32) playingSaveSeconds * 1000;

        if (recordingEnded || (quiet && ! playingTooSoon))
            saveDue = true;

        if (saveDue && havePluginStates())
        {
            saveDue = false;
            project.saveInBackground();
            wasAutosaving = project.isAutosaving();
            savedAt = now;
            tellOlderPluginStates();
        }

        return;
    }

    if (! project.hasChangesToAutosave())
    {
        changedSince = 0;
        return;
    }

    const auto now = devices::Clock::getMillisecondCounter();

    if (changedSince == 0)
        changedSince = now;

    // Due once changes have waited; while the song plays, once it stops, unless it plays on for too long.
    const auto wait = (juce::uint32) (autosavedOnce ? intervalSeconds : firstDelaySeconds) * 1000;
    const auto waited = now - changedSince;
    const auto playing = isSongPlaying (project.getSong());

    if (recordingEnded || (waited >= wait && (! playing || waited >= wait + (juce::uint32) maxWaitWhilePlayingSeconds * 1000)))
    {
        if (! havePluginStates())
            return;

        project.autosaveInBackground();
        wasAutosaving = project.isAutosaving();
        autosavedOnce = true;
        changedSince = 0;
        tellOlderPluginStates();
    }
}

bool AutosaveTimer::havePluginStates()
{
    // Asked at once, those that may have changed, again at each tick until every answer is in or given up on: the write then
    // asks none (Project::saveInBackground()).
    return askPluginStates (project.getSong(), false);
}

void AutosaveTimer::tellOlderPluginStates()
{
    if (auto older = getPluginsWithOlderStates (project.getSong()); older != toldOlder)
    {
        toldOlder = std::move (older);

        if (! toldOlder.isEmpty() && onOlderPluginStates != nullptr)
            onOlderPluginStates (toldOlder);
    }
}
} // namespace phazite
