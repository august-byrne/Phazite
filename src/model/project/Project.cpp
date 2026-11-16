// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Project.h"
#include "devices/modulation/Modulators.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/Json.h"
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

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
    bool tryAgainBriefly (const std::function<bool()>& attempt, const std::function<bool()>& shouldStop)
    {
        const auto mayWait = ! juce::MessageManager::existsAndIsCurrentThread();

        for (int tries = 0;; ++tries)
        {
            if (attempt())
                return true;

            if (! mayWait || tries >= 50 || shouldStop())
                return false;

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
        // Where each element goes, by its parent's type (Schema.h: "Child of").
        const std::map<juce::String, juce::StringArray>& getPlaces()
        {
            static const std::map<juce::String, juce::StringArray> places {
                { "PHAZITE", { "LASTKEY", "TEMPOMAP", "PIANOROLL", "TRACKS", "MASTER", "AUTOMATION", "SECTIONS", "CHORDLANE", "MERGING" } },
                { "TEMPOMAP", { "TEMPO", "TIMESIGNATURE" } },
                { "TRACKS", { "TRACK" } },
                { "TRACK", { "TRACK", "LANES", "DEVICE", "CONTAINER", "SEND", "MODULATOR", "MACRO", "CLIP" } },
                { "MASTER", { "LANES", "DEVICE", "CONTAINER", "MODULATOR" } },
                { "LANES", { "LANE" } },
                { "DEVICE", { "VALUES", "SOUNDS", "VALUE" } },
                { "CONTAINER", { "CHAIN", "MACRO" } },
                { "CHAIN", { "DEVICE" } },
                { "MODULATOR", { "MODULATES", "VALUES" } },
                { "MACRO", { "MODULATES" } },
                { "AUTOMATION", { "CURVE" } },
                { "CURVE", { "POINT" } },
                { "SECTIONS", { "SECTION" } },
                { "CHORDLANE", { "CHORD", "CHORDKEY" } },
                { "CLIP", { "NOTE", "CONTROL", "WARP" } },
            };
            return places;
        }

        // Whether Phazite knows an element's type. One it doesn't (a later build's, another tool's) stays where it is, as it is, with
        // everything in it, and is written back (docs/project-format.md): nothing reads it.
        bool isKnown (const juce::ValueTree& element)
        {
            static const auto known = []
            {
                std::set<juce::String> types { "PHAZITE" };

                for (const auto& [parent, children] : getPlaces())
                {
                    types.insert (parent);

                    for (const auto& child : children)
                        types.insert (child);
                }

                return types;
            }();

            return known.count (element.getType().toString()) > 0;
        }

        // The furthest place in beats a song holds anything at, and in seconds of a file.
        constexpr double maxBeat = maxSongBeats, maxSeconds = 1.0e7, some = 1.0e-9;

        // The IDs a song gives stay well below where counting them on would overflow.
        constexpr auto maxID = schema::maxID;

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

        // A value as a number: a number's own, or text that is a number and nothing else; none for anything else, and
        // infinity for text too large for one.
        std::optional<double> readNumber (const juce::var& value)
        {
            if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
                return (double) value;

            if (! value.isString())
                return std::nullopt;

            const auto text = value.toString().trim().toStdString();
            double number = 0.0;
            const auto read = std::from_chars (text.data(), text.data() + text.size(), number);

            if (read.ec == std::errc::result_out_of_range)
                return std::numeric_limits<double>::infinity();

            if (text.empty() || read.ec != std::errc() || read.ptr != text.data() + text.size())
                return std::nullopt;

            return number;
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
                ! juce::exactlyEqual (kept, *number) || ! isSameValue (element[property], value))
                element.setProperty (property, value, nullptr);
        }

        // Whether a property holds a number Schema.h writes down, which a reader takes as a double: one the loader's table
        // gives a range (getRanges()), a device's or a modulator's value set by hand (VALUES), a point's value; or numbers
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

        // Whether Phazite knows an element's type, by its name's pooled text (juce::Identifier keeps one of each): what isKnown()
        // says, without looking a text up, for the walks that go through every element of a song as it opens.
        bool isKnownType (const juce::Identifier& type)
        {
            static const auto known = []
            {
                std::unordered_set<const void*> types { schema::PHAZITE.getCharPointer().getAddress() };

                for (const auto& [parent, children] : getPlaces())
                {
                    types.insert (juce::Identifier (parent).getCharPointer().getAddress());

                    for (const auto& child : children)
                        types.insert (juce::Identifier (child).getCharPointer().getAddress());
                }

                return types;
            }();

            return known.count (type.getCharPointer().getAddress()) > 0;
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

                // An exponent needs its e: the cheap test first.
                if (text.containsAnyOf ("eE") && holdsNumbers (element, property, inList))
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
                for (const auto& each : getSwitchProperties())
                {
                    if (type != *each.type || ! element.hasProperty (*each.property))
                        continue;

                    const auto& value = element[*each.property];

                    if (value.isBool() || (value.isString() && (value.toString() == "1" || value.toString() == "0")))
                        continue;

                    if ((bool) value)
                        element.setProperty (*each.property, true, nullptr);
                    else if (each.offIsNone)
                        element.removeProperty (*each.property, nullptr);
                    else
                        element.setProperty (*each.property, false, nullptr);
                }

                if (type == schema::MODULATOR)
                    keepListInRange (element, schema::modulatorSteps, -1.0, 1.0, 0.0);

                if (type == schema::SOUNDS)
                    for (const auto* slices : { &schema::samplerSlices, &schema::samplerSlicesRemoved })
                        keepListInRange (element, *slices, 0.0, 1.0, std::nullopt);
            }

            for (auto child : element)
                if (isKnownType (child.getType()))
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
                const auto& places = getPlaces();
                const auto found = places.find (parent.getType().toString());

                for (auto i = parent.getNumChildren(); --i >= 0;)
                {
                    auto child = parent.getChild (i);
                    const auto type = child.getType().toString();

                    if (! isKnown (child))
                        continue;

                    if (child.hasType (schema::TRACK)
                        && ! juce::StringArray { schema::trackIsTrack, schema::trackIsFolder, schema::trackIsReturn }.contains (
                            child[schema::trackKind].toString()))
                        child.setProperty (schema::trackKind,
                                           child.getChildWithName (schema::TRACK).isValid() ? schema::trackIsFolder : schema::trackIsTrack,
                                           nullptr);

                    const auto belongs = found != places.end() && found->second.contains (type)
                                         && ! (child.hasType (schema::TRACK) && parent.hasType (schema::TRACK)
                                               && parent[schema::trackKind].toString() != schema::trackIsFolder)
                                         && ! (child.hasType (schema::CLIP) && parent[schema::trackKind].toString() != schema::trackIsTrack)
                                         && ! (child.hasType (schema::CLIP)
                                               && ! juce::StringArray { schema::clipIsMidi, schema::clipIsAudio }.contains (
                                                   child[schema::clipKind].toString()))
                                         && ! ((child.hasType (schema::NOTE) || child.hasType (schema::CONTROL))
                                               && parent[schema::clipKind].toString() != schema::clipIsMidi)
                                         && ! (child.hasType (schema::WARP) && parent[schema::clipKind].toString() != schema::clipIsAudio)
                                         && ! (child.hasType (schema::CONTROL)
                                               && ! juce::StringArray { schema::controlIsController, schema::controlIsPitchWheel,
                                                                        schema::controlIsChannelPressure, schema::controlIsAftertouch,
                                                                        schema::controlIsProgramChange }
                                                        .contains (child[schema::controlKind].toString()));

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
            // Every number Schema.h gives a range within it, and every word one of its words.
            void keepNumbers (juce::ValueTree element)
            {
                for (const auto& range : getRanges())
                    if (element.hasType (*range.type) && element.hasProperty (*range.property))
                        keepInRange (element, *range.property, range.low, range.high, range.whole, range.fallback);

                for (const auto& words : getWords())
                    if (element.hasType (*words.type) && element.hasProperty (*words.property)
                        && ! words.words.contains (element[*words.property].toString()))
                        element.removeProperty (*words.property, nullptr);

                for (auto child : element)
                    if (isKnown (child))
                        keepNumbers (child);
            }

            //==============================================================================
            // The largest ID a song holds that it can count on from.
            static std::uint64_t findLargestID (const juce::ValueTree& tree)
            {
                std::uint64_t largest = 0;

                for (int i = 0; i < tree.getNumProperties(); ++i)
                    if (const auto property = tree.getPropertyName (i);
                        schema::holdsID (tree, property) || (tree.hasType (schema::NOTE) && property == schema::noteID))
                        if (const auto id = readID (tree[property]); id < maxID)
                            largest = std::max (largest, id);

                for (const auto& child : tree)
                    largest = std::max (largest, findLargestID (child));

                return largest;
            }

            // Every item's ID its own, a note's in its clip; one that isn't, or none, given a new one. An alias group's too
            // large to count on from goes, its clips each alone.
            void keepIDs (juce::ValueTree tree, std::set<std::uint64_t>& seen)
            {
                std::set<std::uint64_t> notes;

                for (auto child : tree)
                {
                    if (! isKnown (child))
                        continue;

                    if (child.hasType (schema::CLIP) && child.hasProperty (schema::clipLink))
                        if (const auto link = readID (child[schema::clipLink]); link == 0 || link >= maxID)
                            child.removeProperty (schema::clipLink, nullptr);

                    if (child.hasType (schema::NOTE))
                    {
                        if (const auto id = readID (child[schema::noteID]); id == 0 || id >= maxID || ! notes.insert (id).second)
                        {
                            const auto made = song.makeID();
                            notes.insert (made);
                            child.setProperty (schema::noteID, (juce::int64) made, nullptr);
                        }

                        continue;
                    }

                    for (const auto* property : { &schema::trackID, &schema::channelFader, &schema::channelVCA })
                    {
                        if (! schema::holdsID (child, *property) || (property != &schema::trackID && ! child.hasProperty (*property)))
                            continue;

                        if (const auto id = readID (child[*property]); id == 0 || id >= maxID || ! seen.insert (id).second)
                        {
                            const auto made = song.makeID();
                            seen.insert (made);
                            child.setProperty (*property, (juce::int64) made, nullptr);
                        }
                    }

                    keepIDs (child, seen);
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

                    std::vector<juce::Identifier> properties;

                    for (int p = 0; p < child.getNumProperties(); ++p)
                        properties.push_back (child.getPropertyName (p));

                    for (const auto& property : properties)
                    {
                        if (! schema::namesID (child, property) || items.count (readID (child[property])) > 0 || readInPart)
                            continue;

                        if (child.hasType (schema::LANE) || child.hasType (schema::CURVE) || child.hasType (schema::MODULATES))
                            gone = true;
                        else if (property == schema::trackPadDevice)
                            child.removeProperty (schema::trackPad, nullptr);

                        child.removeProperty (property, nullptr);
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
            // Each curve's points within its parameter's range, and none of a parameter its device hasn't. A plug-in tells
            // its parameters only once it plays (from 0 to 1); a chain's crossover is kept while its container isn't split
            // into bands, or while it is the first chain, which have none for now (describeParameter() finds none).
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
                    auto low = 0.0, high = 1.0, fallback = 0.0;

                    if (const auto device = findDeviceState (song, parameter.device);
                        device.isValid() && device[schema::deviceType].toString() == schema::deviceIsPlugin)
                    {
                        // A plug-in's from 0 to 1, as its element says it is one, never asked of its instance: a song that belongs
                        // to no session (one read from the versions, on the history's thread) has none.
                    }
                    else if (const auto description = describeParameter (song, parameter))
                    {
                        low = fallback = (double) description->range.start;
                        high = (double) description->range.end;
                    }
                    else if (findContainerChain (song, parameter.device).isValid() && parameter.parameter == schema::crossoverParameter)
                    {
                        low = fallback = (double) lowestCrossover;
                        high = (double) highestCrossover;
                    }
                    else if (readInPart)
                    {
                        // Of a device in what wasn't read: kept, its points finite (the song that takes it in knows the device).
                        low = -std::numeric_limits<double>::max();
                        high = std::numeric_limits<double>::max();
                    }
                    else if (findDeviceState (song, parameter.device)[schema::deviceType].toString() != schema::deviceIsPlugin)
                    {
                        automation.removeChild (i, nullptr);
                        continue;
                    }

                    for (auto point : curve)
                        if (point.hasType (schema::POINT))
                            keepInRange (point, schema::pointValue, low, high, false, fallback);
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

                // The song's next ID past what it holds (not past an ID too large to count on from), then each its own.
                const auto saved = readID (state[schema::nextID]);
                state.setProperty (schema::nextID, (juce::int64) std::max (findLargestID (state) + 1, saved < maxID ? saved : 0), nullptr);
                std::set<std::uint64_t> items;
                keepIDs (state, items);
                countIDs (song);
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
    auto backups = projectFile.getSiblingFile ("Backups").findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);

    std::sort (backups.begin(), backups.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   const auto ta = a.getLastModificationTime(), tb = b.getLastModificationTime();
                   return ta != tb ? ta > tb : a.getFileName() > b.getFileName();
               });

    for (const auto& backup : backups)
        if (auto song = readSong (backup); song.isValid())
            return { backup, song };

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

Project::Project (Song::Ptr s, const juce::File& f)
    : song (std::move (s)),
      songState (song->getState()),
      file (f),
      autosaveWriter (std::make_unique<BackgroundWriter>())
{
    // The song's cache folder, where its frozen tracks' files are, is the project's Cache folder. Closing the project empties
    // it of what nothing uses; opening it doesn't, as clips find the renders they use only when they play.
    song->setCacheFolder (getCacheFolder());
    rememberSavedState(); // what opening made of it isn't a change
    changesOutsideHistory = song->getChangesOutsideHistory();
    songState.addListener (this);
}

Project::~Project()
{
    JUCE_ASSERT_MESSAGE_THREAD

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

    if (! hasUnsavedChanges())
        return false;

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true); // what saving stores isn't a change by the user
        storeForSaving (*song);
    }

    if (! holdTheSameProject (song->getState(), savedState))
        return true;

    // The song is as saved after all.
    changedSinceSave = changedSinceAutosave = false;
    changesOutsideHistory = song->getChangesOutsideHistory();
    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();
    return false;
}

juce::Result Project::save()
{
    JUCE_ASSERT_MESSAGE_THREAD

    // What is being tried in place (Swap) is kept first, as its own step: the file holds what is heard.
    Trial::keepOpenTrial (song->getUndoManager());

    autosaveWriter->wait(); // an autosave being written, which this makes old
    backgroundSave = {};    // and a save in the background, which this overtakes
    backUpProjectFile();
    backedUpAt = juce::Time::currentTimeMillis();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true); // what saving stores isn't a change by the user
        storeForSaving (*song);

        if (const auto written = writeProjectFile (song->getState(), file); written.failed())
            return juce::Result::fail ("Phazite couldn't save the project to " + file.getFullPathName() + ": " + written.getErrorMessage());
    }

    changedSinceSave = false;
    changedSinceAutosave = false;
    changesOutsideHistory = song->getChangesOutsideHistory(); // stored in the song as it saved
    rememberSavedState();                                     // what the file holds now
    getAutosaveFile (file).deleteFile();
    autosavedAt = 0;
    autosaveWriter->forgetWritten();
    return juce::Result::ok();
}

juce::Result Project::autosave()
{
    JUCE_ASSERT_MESSAGE_THREAD
    autosaveWriter->wait();
    compareWithSavedState();

    if (! changedSinceAutosave)
        return juce::Result::ok();

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);

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
    compareWithSavedState();

    if (! changedSinceAutosave || autosaveWriter->isBusy())
        return;

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);
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
    compareWithSavedState();

    if (! changedSinceSave || autosaveWriter->isBusy() || backgroundSave.isValid())
        return;

    // The previous file into the backups every few minutes, not at every save.
    if (const auto now = juce::Time::currentTimeMillis(); now - backedUpAt >= (juce::int64) backupMinutes * 60 * 1000)
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

    changesAtBackgroundSave = changeCount;
    outsideAtBackgroundSave = song->getChangesOutsideHistory();
}

bool Project::finishBackgroundSave()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! backgroundSave.isValid() || autosaveWriter->isBusy())
        return true;

    const auto written = std::exchange (backgroundSave, {});

    if (autosaveWriter->takeFailure())
        return false; // its changes are still to save

    // What the file holds now: saved, unless the song changed since it was copied.
    savedState = written;
    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();
    changesOutsideHistory = outsideAtBackgroundSave;

    if (changeCount == changesAtBackgroundSave && song->getChangesOutsideHistory() == outsideAtBackgroundSave)
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

    const auto newFile = makeFolderWithFiles (parentFolder, name, error);

    if (newFile == juce::File())
        return {};

    const juce::ScopedValueSetter<bool> writing (isWriting, true);
    storeForSaving (*song);

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
    autosaveWriter->wait(); // its autosave goes with it

    juce::String error;
    const auto folder = findNewProjectFolder (parentFolder, name, error);
    const auto copy = folder != juce::File() ? makeCopyFolder (folder, leftoverRemovals, error) : juce::File();

    if (copy == juce::File())
    {
        onFinished (juce::Result::fail (error));
        return {};
    }

    // The versions last (copyVersions()), counted with the rest.
    auto list = listFoldersToCopy (getFolder(), FoldersListed::allButVersions);
    const auto versionsBytes = listFoldersToCopy (getFolder(), FoldersListed::versions).bytes;

    if (list.bytes + versionsBytes > copyAtOnceBytes)
        return std::make_unique<TrackRender> (
            std::make_unique<FolderCopy> (*this, copy, folder, std::move (list), versionsBytes, std::move (onFinished)));

    // Small: at once, on the message thread, which starts no write meanwhile; the versions once what writes them is done.
    std::atomic<juce::int64> copied { 0 };
    auto result = copyFolders (getFolder(), copy, list, [] { return false; }, copied);

    if (result.wasOk())
    {
        if (versionsCopier.wait != nullptr)
            versionsCopier.wait();

        result = copyVersions (getFolder(), copy, [] { return false; }, copied);
    }

    if (result.wasOk())
        result = finishMoveTo (copy, folder);

    if (result.failed())
        copy.deleteRecursively();

    onFinished (result);
    return {};
}
juce::Result Project::finishMoveTo (const juce::File& copy, const juce::File& folder)
{
    JUCE_ASSERT_MESSAGE_THREAD
    autosaveWriter->wait();

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

    // One that can't be made is told, once until one is made again; the save goes on without it.
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");

    // A file that can't be read is no copy either, said before JUCE tries: on Linux its copy reads from a stream it hasn't
    // checked, and asserts. JUCE workaround 36 (docs/juce-workarounds.md).
    if (! file.hasReadAccess()
        || ! file.copyFileTo (backups.getNonexistentChildFile (file.getFileNameWithoutExtension() + " " + stamp, fileExtension, false)))
    {
        if (! std::exchange (backupFailed, true) && onBackupFailed != nullptr)
            onBackupFailed ("Phazite couldn't copy the project's last saved version into " + backups.getFullPathName()
                            + ". It saved the project without a backup; check that that folder can be written to.");

        return;
    }

    backupFailed = false;

    // Keep the newest numBackupsToKeep. A copy keeps the original's modification time, which is
    // when that version was saved.
    auto versions = backups.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    versions.removeIf ([autosaveFile = getAutosaveFile (file)] (const juce::File& f) { return f == autosaveFile; });

    std::sort (versions.begin(), versions.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   const auto ta = a.getLastModificationTime(), tb = b.getLastModificationTime();
                   return ta != tb ? ta > tb : a.getFileName() > b.getFileName();
               });

    for (int i = numBackupsToKeep; i < versions.size(); ++i)
        versions.getReference (i).deleteFile();
}

void Project::rememberSavedState()
{
    savedState = song->getState().createCopy();
    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();
}

void Project::forgetSavedState()
{
    // No song to compare with, so none is the same (holdTheSameProject()): unsaved, whatever closing compares, until a save
    // remembers what it wrote.
    savedState = {};
    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();
    changedSinceSave = changedSinceAutosave = true;
}

void Project::compareWithSavedState()
{
    // A plug-in's own change, which a save stores in the song.
    if (const auto outside = song->getChangesOutsideHistory(); outside != changesOutsideHistory)
    {
        changesOutsideHistory = outside;
        changedSinceSave = changedSinceAutosave = true;
    }

    if (! mayHaveChanged)
        return;

    // The elements that notified, each with its copy at the same place in savedState: the song's own elements keep their
    // places unless one was added or removed, which notifies the song itself.
    const auto state = song->getState();
    const auto isSame = [this, &state] (const juce::ValueTree& element)
    {
        const auto index = state.indexOf (element);
        return index >= 0 && index < savedState.getNumChildren() && holdTheSameProject (element, savedState.getChild (index));
    };

    const auto same =
        compareAll ? holdTheSameProject (state, savedState) : std::all_of (elementsToCompare.begin(), elementsToCompare.end(), isSame);

    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();

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

    if (! (tree == state || tree.isAChildOf (state)) || ! schema::isCounted (tree, property))
        return;

    ++changeCount;

    if (changedSinceSave)
    {
        changedSinceAutosave = true;
        return;
    }

    // Perhaps nothing changed: compared with savedState when asked, the element of the song it is in.
    mayHaveChanged = true;
    auto element = tree;

    while (element.getParent().isValid() && element.getParent() != state)
        element = element.getParent();

    if (element == state || element.getParent() != state)
        compareAll = true;
    else
        elementsToCompare.addIfNotAlreadyThere (element);
}

void Project::markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! schema::isUncountedElement (parent, child))
        markChanged (parent);
}

bool Project::holdTheSameProject (const juce::ValueTree& a, const juce::ValueTree& b)
{
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
        const auto now = juce::Time::getMillisecondCounter();

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

    const auto now = juce::Time::getMillisecondCounter();

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
