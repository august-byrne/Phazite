// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/VersionStore.h"
#include "internal/StoreSweep.h"
#include "model/base/KeptByAge.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/versions/VersionsModel.h"

#include <juce_cryptography/juce_cryptography.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <set>

namespace phazite
{
namespace
{
    namespace versions = schema::versions;

    // A property a branch's file holds that BranchRecord has a field for.
    bool isBranchProperty (const juce::Identifier& name)
    {
        for (const auto* known : { &versions::branchID, &versions::branchName, &versions::branchColour, &versions::branchHead,
                                   &versions::branchChanged, &versions::branchDeleted })
            if (name == *known)
                return true;

        return false;
    }

    // A small file replaced whole: written beside it first and flushed to the disk, then put in its place
    // (model/base/WholeFile.h).
    bool replaceFile (const juce::File& file, const juce::String& text)
    {
        return file.getParentDirectory().createDirectory().wasOk()
               && writeWholeFile (file, [&text] (juce::OutputStream& out) { return out.writeText (text, false, false, "\n"); }).wasOk();
    }

    juce::String toText (const juce::ValueTree& element)
    {
        if (const auto xml = element.createXml())
            return xml->toString();

        return {};
    }

    void findAudioFilePaths (const juce::ValueTree& element, juce::StringArray& paths)
    {
        if (element.hasType (schema::CLIP))
            if (const auto path = element[schema::clipFile].toString(); path.isNotEmpty())
                paths.addIfNotAlreadyThere (path);

        if (element.hasType (schema::SOUNDS))
            for (int i = 0; i < element.getNumProperties(); ++i)
                if (const auto property = element.getPropertyName (i); schema::isSoundFileProperty (property))
                    if (const auto path = element[property].toString(); path.isNotEmpty())
                        paths.addIfNotAlreadyThere (path);

        for (const auto& child : element)
            findAudioFilePaths (child, paths);
    }
} // namespace

juce::ValueTree toElement (const VersionRecord& record)
{
    juce::ValueTree element (versions::VERSION);
    element.setProperty (versions::versionSong, record.song, nullptr);

    const auto setIfAny = [&element] (const juce::Identifier& property, const juce::String& value)
    {
        if (value.isNotEmpty())
            element.setProperty (property, value, nullptr);
    };

    setIfAny (versions::versionParents, record.parents.joinIntoString (" "));
    setIfAny (versions::versionName, record.name);
    setIfAny (versions::versionNote, record.note);
    setIfAny (versions::versionSummary, record.summary);
    setIfAny (versions::versionBranch, record.branch);
    setIfAny (versions::versionPhazite, record.phazite);
    element.setProperty (versions::versionSaved, record.saved, nullptr);

    auto files = record.files;
    std::sort (files.begin(), files.end(), [] (const auto& a, const auto& b) { return a.path.compare (b.path) < 0; });

    for (const auto& file : files)
        element.appendChild (
            juce::ValueTree (versions::FILE,
                             { { versions::filePath, file.path }, { versions::fileSize, file.size }, { versions::fileHash, file.sha256 } }),
            nullptr);

    auto renumbered = record.renumbered;
    std::sort (renumbered.begin(), renumbered.end(),
               [] (const auto& a, const auto& b) { return a.clip != b.clip ? a.clip.compare (b.clip) < 0 : a.from < b.from; });

    for (const auto& id : renumbered)
    {
        juce::ValueTree entry (versions::RENUMBERED,
                               { { versions::renumberedFrom, (juce::int64) id.from }, { versions::renumberedTo, (juce::int64) id.to } });

        if (id.clip.isNotEmpty())
            entry.setProperty (versions::renumberedClip, id.clip, nullptr);

        element.appendChild (entry, nullptr);
    }

    return element;
}

std::optional<VersionRecord> toVersionRecord (const juce::ValueTree& element, const juce::String& id)
{
    if (! element.hasType (versions::VERSION))
        return std::nullopt;

    VersionRecord record;
    record.id = id;
    record.song = element[versions::versionSong].toString();
    record.parents = juce::StringArray::fromTokens (element[versions::versionParents].toString(), " ", {});
    record.parents.removeEmptyStrings();
    record.name = element[versions::versionName].toString();
    record.note = element[versions::versionNote].toString();
    record.summary = element[versions::versionSummary].toString();
    record.branch = element[versions::versionBranch].toString();
    record.phazite = element[versions::versionPhazite].toString();
    record.saved = element[versions::versionSaved].toString().getLargeIntValue();

    for (const auto& child : element)
    {
        if (child.hasType (versions::FILE))
            record.files.push_back ({ child[versions::filePath].toString(), child[versions::fileSize].toString().getLargeIntValue(),
                                      child[versions::fileHash].toString() });
        else if (child.hasType (versions::RENUMBERED))
            record.renumbered.push_back ({ child[versions::renumberedClip].toString(),
                                           (std::uint64_t) child[versions::renumberedFrom].toString().getLargeIntValue(),
                                           (std::uint64_t) child[versions::renumberedTo].toString().getLargeIntValue() });
    }

    return record;
}

//==============================================================================
VersionStore::VersionStore (juce::File versionsFolder, juce::String computerID)
    : folder (std::move (versionsFolder)),
      computer (std::move (computerID)),
      pieces (folder.getChildFile ("pieces"), computer)
{
}

juce::File VersionStore::getLocalFolder() const
{
    return folder.getChildFile ("local").getChildFile (makeUsableFileName (computer));
}

juce::String VersionStore::writeVersion (VersionRecord record, const SongPieces& song, juce::String* problem)
{
    record.song = song.song;
    auto block = writeElement (toElement (record));
    auto id = getPieceName (block);

    auto all = song.pieces;
    all.emplace (id, std::move (block));
    return pieces.write (all, problem) ? id : juce::String();
}

std::optional<VersionRecord> VersionStore::readVersion (const juce::String& id) const
{
    if (const auto block = pieces.read (id))
        return toVersionRecord (readElement (*block), id);

    return std::nullopt;
}

namespace
{
    std::atomic<int> songsRead { 0 }, songsReadOnMessageThread { 0 }, songsWritten { 0 }, songsWrittenOnMessageThread { 0 };
}

bool VersionStore::writeSong (const SongPieces& song)
{
    ++songsWritten;

    if (juce::MessageManager::existsAndIsCurrentThread())
        ++songsWrittenOnMessageThread;

    return pieces.write (song.pieces);
}

VersionStore::SongsRead VersionStore::countSongsWritten()
{
    return { songsWritten.load(), songsWrittenOnMessageThread.load() };
}

VersionStore::SongsRead VersionStore::countSongsRead()
{
    return { songsRead.load(), songsReadOnMessageThread.load() };
}

juce::ValueTree VersionStore::readSong (const juce::String& songPiece, PluginParameterValues* parameters,
                                        std::vector<MissingPiece>* missing, bool* idsGivenAnew) const
{
    ++songsRead;

    if (juce::MessageManager::existsAndIsCurrentThread())
        ++songsReadOnMessageThread;

    return joinSong (
        songPiece, [this] (const juce::String& name) { return pieces.read (name); }, parameters, missing, folder.getParentDirectory(),
        idsGivenAnew);
}

//==============================================================================
juce::File VersionStore::getBranchFile (const juce::String& id, const juce::String& computerID) const
{
    return folder.getChildFile ("branches").getChildFile (makeUsableFileName (id)).getChildFile (makeUsableFileName (computerID) + ".xml");
}

std::vector<BranchRecord> VersionStore::readBranches() const
{
    std::vector<BranchRecord> branches;
    const auto working = readWorking();

    for (const auto& branchFolder : folder.getChildFile ("branches").findChildFiles (juce::File::findDirectories, false))
    {
        for (const auto& file : branchFolder.findChildFiles (juce::File::findFiles, false, "*.xml"))
        {
            const auto xml = juce::parseXML (file);

            if (xml == nullptr || xml->getTagName() != versions::BRANCH.toString())
                continue;

            BranchRecord branch;
            branch.id = xml->getStringAttribute (versions::branchID);
            branch.name = xml->getStringAttribute (versions::branchName);
            branch.colour = juce::Colour::fromString (xml->getStringAttribute (versions::branchColour));
            branch.head = xml->getStringAttribute (versions::branchHead);
            branch.changed = xml->getStringAttribute (versions::branchChanged).getLargeIntValue();
            branch.deleted = xml->getBoolAttribute (versions::branchDeleted);

            branch.computer = file.getFileNameWithoutExtension();

            for (const auto& attribute : xml->getAttributeIterator())
                if (! isBranchProperty (attribute.name))
                    branch.others.set (attribute.name, attribute.value);

            if (branch.id.isEmpty() || branch.id != branchFolder.getFileName())
                continue;

            if (branch.computer == makeUsableFileName (computer))
                if (const auto found = working.find (branch.id); found != working.end())
                    branch.working = found->second;

            branches.push_back (std::move (branch));
        }
    }

    return branches;
}

bool VersionStore::writeBranch (const BranchRecord& branch)
{
    juce::ValueTree element (versions::BRANCH);
    element.setProperty (versions::branchID, branch.id, nullptr);
    element.setProperty (versions::branchName, branch.name, nullptr);
    element.setProperty (versions::branchColour, branch.colour.toString(), nullptr);
    element.setProperty (versions::branchChanged, branch.changed, nullptr);

    if (branch.head.isNotEmpty())
        element.setProperty (versions::branchHead, branch.head, nullptr);

    if (branch.deleted)
        element.setProperty (versions::branchDeleted, true, nullptr);

    for (const auto& [name, value] : branch.others)
        if (! isBranchProperty (name))
            element.setProperty (name, value, nullptr);

    if (! replaceFile (getBranchFile (branch.id, computer), toText (element)))
        return false;

    // The song left on it, on this computer alone.
    auto working = readWorking();

    if (const auto found = working.find (branch.id); (found != working.end() ? found->second : juce::String()) == branch.working)
        return true;

    if (branch.working.isNotEmpty())
        working[branch.id] = branch.working;
    else
        working.erase (branch.id);

    return writeWorking (working);
}

std::map<juce::String, juce::String> VersionStore::readWorking() const
{
    return readWorkingIn (getLocalFolder());
}

std::map<juce::String, juce::String> VersionStore::readWorkingIn (const juce::File& localFolder)
{
    std::map<juce::String, juce::String> working;

    if (const auto xml = juce::parseXML (localFolder.getChildFile ("working.xml")))
        for (const auto* left : xml->getChildWithTagNameIterator (versions::LEFT.toString()))
            if (const auto branch = left->getStringAttribute (versions::leftBranch); branch.isNotEmpty())
                working[branch] = left->getStringAttribute (versions::leftSong);

    return working;
}

bool VersionStore::writeWorking (const std::map<juce::String, juce::String>& working)
{
    juce::ValueTree element (versions::WORKING);

    for (const auto& [branch, song] : working)
        element.appendChild (juce::ValueTree (versions::LEFT, { { versions::leftBranch, branch }, { versions::leftSong, song } }), nullptr);

    return replaceFile (getLocalFolder().getChildFile ("working.xml"), toText (element));
}

juce::String VersionStore::readCurrentBranch() const
{
    return folder.getChildFile ("current").loadFileAsString().trim();
}

bool VersionStore::writeCurrentBranch (const juce::String& id)
{
    return replaceFile (folder.getChildFile ("current"), id);
}

//==============================================================================
namespace
{
    // A file of lines (a log, a history), read as bytes: its whole lines, and whether it was damaged. A last line without
    // its end was cut off by a crash; zero bytes are what a power loss leaves where the end of a file was never written
    // (a line holding one is left out: reading as text would stop at the first, and leave every line after it unseen).
    struct LogText
    {
        juce::StringArray lines;
        bool zeroBytes = false, damaged = false;
    };

    LogText readLogText (const juce::File& file)
    {
        LogText text;
        juce::MemoryBlock bytes;

        if (! file.existsAsFile() || ! file.loadFileAsData (bytes) || bytes.isEmpty())
            return text;

        const auto* data = static_cast<const char*> (bytes.getData());
        const auto size = bytes.getSize();
        size_t start = 0;

        for (size_t i = 0; i <= size; ++i)
        {
            if (i < size && data[i] != '\n')
                continue;

            if (i == size && start == size)
                break; // the file ended with its last line's end

            const auto length = i - start;

            if (std::memchr (data + start, 0, length) != nullptr)
                text.zeroBytes = text.damaged = true;
            else if (i == size)
                text.damaged = true; // cut off before its end
            else
                text.lines.add (juce::String::fromUTF8 (data + start, (int) length).trimCharactersAtEnd ("\r"));

            start = i + 1;
        }

        return text;
    }

    // Whether a line reads as a log's: a time, two versions or "-", a reason, a song or "-", and the name after them.
    bool isLogLine (const juce::String& line)
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", {});
        return parts.size() >= 5 && parts[0].containsOnly ("0123456789") && parts[0].isNotEmpty() && parts[3].isNotEmpty();
    }
} // namespace

bool VersionStore::logBranchMove (const juce::String& branchID, const juce::String& from, const juce::String& to, const juce::String& why,
                                  const juce::String& name, const juce::String& song)
{
    const auto file = getLocalFolder().getChildFile ("logs").getChildFile (makeUsableFileName (branchID));
    jassert (! isInHeldFolder (file)); // a first save copies the folder (WholeFile.h: holdFolder())

    if (! file.getParentDirectory().createDirectory())
        return false;

    // A line a move: when, from, to, why, the song left on it, and the branch's name, which may hold spaces and comes last;
    // "-" for no version and no song. A name holds no control character (VersionsModel.h: toBranchName()), so no line end.
    // Appended and flushed to the disk as the stream closes; after damage (a line a crash cut off, zero bytes a power loss
    // left, a line of another shape), the log is written again whole, of its whole lines and this one, so that no fragment
    // stays in its middle.
    const auto orNone = [] (const juce::String& id) { return id.isNotEmpty() ? id : juce::String ("-"); };
    const auto line =
        juce::String (stamp()) + " " + orNone (from) + " " + orNone (to) + " " + why + " " + orNone (song) + " " + toBranchName (name);

    if (const auto text = readLogText (file); text.damaged)
    {
        juce::String whole;

        for (const auto& kept : text.lines)
            if (isLogLine (kept))
                whole << kept << '\n';

        return replaceFile (file, whole + line + "\n");
    }

    juce::FileOutputStream out (file);

    if (! out.openedOk())
        return false;

    out << line << '\n';
    out.flush();
    return out.getStatus().wasOk();
}
std::vector<BranchMove> VersionStore::readBranchLog (const juce::String& branchID) const
{
    return readLogFile (getLocalFolder().getChildFile ("logs").getChildFile (makeUsableFileName (branchID)));
}

std::vector<BranchMove> VersionStore::readEveryBranchLog (const juce::String& branchID) const
{
    std::vector<BranchMove> moves;

    for (const auto& local : folder.getChildFile ("local").findChildFiles (juce::File::findDirectories, false))
    {
        auto theirs = readLogFile (local.getChildFile ("logs").getChildFile (makeUsableFileName (branchID)));

        for (auto& move : theirs)
        {
            move.branch = makeUsableFileName (branchID);
            move.computer = local.getFileName();
        }

        moves.insert (moves.end(), theirs.begin(), theirs.end());
    }

    std::stable_sort (moves.begin(), moves.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
    return moves;
}

std::vector<BranchMove> VersionStore::readEveryLog (bool oldestFirst) const
{
    std::vector<BranchMove> moves;

    for (const auto& local : folder.getChildFile ("local").findChildFiles (juce::File::findDirectories, false))
    {
        for (const auto& log : local.getChildFile ("logs").findChildFiles (juce::File::findFiles, false))
        {
            auto theirs = readLogFile (log);

            for (auto& move : theirs)
            {
                move.branch = log.getFileName();
                move.computer = local.getFileName();
            }

            moves.insert (moves.end(), theirs.begin(), theirs.end());
        }
    }

    if (oldestFirst)
        std::stable_sort (moves.begin(), moves.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });

    return moves;
}

std::vector<BranchMove> VersionStore::readLogFile (const juce::File& file)
{
    std::vector<BranchMove> moves;

    for (const auto& line : readLogText (file).lines)
    {
        if (! isLogLine (line))
            continue;

        // The name is the rest of the line, its spaces as they were, made as a typed one is.
        const auto parts = juce::StringArray::fromTokens (line, " ", {});
        const auto none = [] (const juce::String& part) { return part == "-" ? juce::String() : part; };
        auto name = line;

        for (int i = 0; i < 5; ++i)
            name = name.fromFirstOccurrenceOf (" ", false, false);

        moves.push_back ({ parts[0].getLargeIntValue(),
                           none (parts[1]),
                           none (parts[2]),
                           parts[3],
                           parts.size() > 5 ? toBranchName (name) : juce::String(),
                           none (parts[4]),
                           {},
                           {} });
    }

    return moves;
}

//==============================================================================
namespace
{
    // A name Phazite gives a file or a folder of the store: what makeUsableFileName() leaves of an id. A sync program's copy
    // of a file two computers wrote ("working (conflicted copy).xml", "history.sync-conflict-...") has another.
    bool isPlainName (const juce::String& name)
    {
        return name.isNotEmpty() && name.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
               && ! name.containsIgnoreCase ("conflict");
    }

    // A file's XML, only where it parses without an error: JUCE's parser takes what it can of a file cut short (an
    // attribute left open), which would read as a record that names less.
    std::unique_ptr<juce::XmlElement> parseWhole (const juce::File& file)
    {
        if (! file.loadFileAsString().trimEnd().endsWithChar ('>'))
            return nullptr; // cut off before its element's end

        juce::XmlDocument document (file);
        auto xml = document.getDocumentElement();
        return document.getLastParseError().isEmpty() ? std::move (xml) : nullptr;
    }

    // A temporary file a write of a whole file leaves beside it as it goes (WholeFile.h, juce::TemporaryFile): none of a
    // record's. And a file the system or a file browser puts in any folder (desktop.ini, Thumbs.db, .DS_Store, .directory
    // and other dot files): no record either.
    bool isTemporary (const juce::File& file)
    {
        const auto name = file.getFileName();
        return name.contains ("_temp") || name.startsWithChar ('.') || name.equalsIgnoreCase ("desktop.ini")
               || name.equalsIgnoreCase ("Thumbs.db");
    }

    // Whether lines that don't read as they should (a log's or a history's) leave what the file names unknown: a line's
    // damage counts only where its time reads and is within the 30 days before asOf (or after it); an older line, or one
    // without a time, names nothing a sweep may now let go of, and is passed over. Zero bytes a power loss left count
    // while the newest whole line is that recent.
    bool isRecentDamage (const LogText& text, bool (*isWhole) (const juce::String&), juce::int64 asOf)
    {
        const auto isRecent = [asOf] (const juce::String& line)
        {
            const auto time = line.upToFirstOccurrenceOf (" ", false, false);
            return time.isNotEmpty() && time.containsOnly ("0123456789") && asOf - time.getLargeIntValue() <= keptByAge::dailyFor;
        };

        for (const auto& line : text.lines)
            if (! isWhole (line) && isRecent (line))
                return true;

        return text.zeroBytes && std::any_of (text.lines.begin(), text.lines.end(), isRecent);
    }

    bool isHistoryLine (const juce::String& line)
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", {});
        return parts.size() >= 4 && parts[0].containsOnly ("0123456789") && parts[0].isNotEmpty();
    }
} // namespace

bool VersionStore::areRecordsWhole() const
{
    const auto now = getVersionsTime();

    // Every computer's record of every branch: a BRANCH of its folder's id.
    for (const auto& branchFolder : folder.getChildFile ("branches").findChildFiles (juce::File::findDirectories, false))
    {
        if (! isPlainName (branchFolder.getFileName()))
            return false;

        for (const auto& file : branchFolder.findChildFiles (juce::File::findFiles, false))
        {
            if (isTemporary (file))
                continue;

            const auto xml = parseWhole (file);

            if (! isPlainName (file.getFileName()) || ! file.hasFileExtension ("xml") || xml == nullptr
                || xml->getTagName() != versions::BRANCH.toString()
                || xml->getStringAttribute (versions::branchID) != branchFolder.getFileName())
                return false;
        }
    }

    // Each computer's own folder: its songs left on branches, its Local History and its logs, and nothing else.
    for (const auto& local : folder.getChildFile ("local").findChildFiles (juce::File::findDirectories, false))
    {
        if (! isPlainName (local.getFileName()))
            return false;

        for (const auto& file : local.findChildFiles (juce::File::findFiles | juce::File::findDirectories, false))
        {
            const auto name = file.getFileName();

            if (isTemporary (file) || name == "files.xml" || name == "swept" || name == "stamped" || (name == "logs" && file.isDirectory()))
                continue;

            if (name == "working.xml")
            {
                const auto xml = parseWhole (file);

                if (xml == nullptr || xml->getTagName() != versions::WORKING.toString())
                    return false;

                continue;
            }

            if (name == "history")
            {
                if (isRecentDamage (readLogText (file), isHistoryLine, now))
                    return false;

                continue;
            }

            return false; // a file no Phazite writes there: a sync program's copy of one
        }

        for (const auto& log : local.getChildFile ("logs").findChildFiles (juce::File::findFiles, false))
        {
            if (isTemporary (log))
                continue;

            if (! isPlainName (log.getFileName()))
                return false;

            if (isRecentDamage (readLogText (log), isLogLine, now))
                return false;
        }
    }

    return true;
}

void VersionStore::settleStampFloor()
{
    // The newest time this computer ever stamped: the floor it kept (local/<computer>/stamped), its logs' and its Local
    // History's lines, and its own packs' pieces. Never lowered, and apart from the sweep's time: so a clock set back, at
    // every start (a dead battery), stamps from where the records were; one that was ahead keeps what it stamped longer.
    // A time later than ten years past the clock is no stamp of a computer's but a damaged file's, and is passed over, so
    // that the floor stays a time (and adding to it can't overflow).
    const auto local = getLocalFolder();
    const auto kept = local.getChildFile ("stamped");
    const auto stored = kept.loadFileAsString().trim();
    const auto latest = getVersionsTime() + latestStampAhead;
    juce::int64 newest = 0;

    const auto consider = [&newest, latest] (juce::int64 time)
    {
        if (time <= latest)
            newest = std::max (newest, time);
    };

    const auto readTime = [] (const juce::String& text)
    {
        // Digits alone, and few enough that they can't overflow.
        return text.isNotEmpty() && text.length() <= 15 && text.containsOnly ("0123456789") ? text.getLargeIntValue() : (juce::int64) 0;
    };

    consider (pieces.getNewestStamp());
    consider (pieces.findNewestOwnWrite());
    consider (readTime (stored));
    juce::Array<juce::File> files { local.getChildFile ("history") };
    files.addArray (local.getChildFile ("logs").findChildFiles (juce::File::findFiles, false));

    for (const auto& file : files)
        for (const auto& line : readLogText (file).lines)
            consider (readTime (line.upToFirstOccurrenceOf (" ", false, false)));

    pieces.raiseStampFloor (newest + 1); // after it: no two stamps the same

    if (newest > 0 && juce::String (newest) != stored && ! isInHeldFolder (folder))
        replaceFile (kept, juce::String (newest) + "\n");
}

juce::int64 VersionStore::getKeepingTime() const
{
    const auto now = getVersionsTime();
    const auto last = readSwept().time;
    return last <= 0 ? now : std::min (now, std::min (last, now) + sweepEvery);
}
VersionStore::Swept VersionStore::readSwept() const
{
    // Its first line when, then the versions swept whose bounces and edited messages are still to delete, a line each.
    auto lines = juce::StringArray::fromLines (getLocalFolder().getChildFile ("swept").loadFileAsString());
    lines.removeEmptyStrings();
    Swept swept;

    if (! lines.isEmpty())
    {
        swept.time = lines[0].getLargeIntValue();
        lines.remove (0);
        swept.bounces = lines;
    }

    return swept;
}
bool VersionStore::writeSwept (const Swept& swept)
{
    return replaceFile (getLocalFolder().getChildFile ("swept"),
                        juce::String (swept.time) + "\n" + swept.bounces.joinIntoString ("\n") + (swept.bounces.isEmpty() ? "" : "\n"));
}

//==============================================================================
namespace
{
    // One computer's edited messages, by commit.
    std::map<juce::String, MessageEdit> readMessagesFile (const juce::File& file)
    {
        std::map<juce::String, MessageEdit> edits;

        if (const auto xml = juce::parseXML (file); xml != nullptr && xml->getTagName() == versions::MESSAGES.toString())
        {
            for (const auto* message : xml->getChildWithTagNameIterator (versions::MESSAGE.toString()))
            {
                if (const auto commit = message->getStringAttribute (versions::messageCommit); commit.isNotEmpty())
                    edits[commit] = { message->getStringAttribute (versions::messageName),
                                      message->getStringAttribute (versions::messageNote),
                                      message->getStringAttribute (versions::messageChanged).getLargeIntValue() };
            }
        }

        return edits;
    }
} // namespace

std::map<juce::String, MessageEdit> VersionStore::readMessages() const
{
    // Every computer's, the newest for each commit (this computer's, of two set at once).
    std::map<juce::String, MessageEdit> edits;
    const auto mine = makeUsableFileName (computer) + ".xml";
    auto files = folder.getChildFile ("messages").findChildFiles (juce::File::findFiles, false, "*.xml");
    std::stable_partition (files.begin(), files.end(), [&mine] (const juce::File& file) { return file.getFileName() == mine; });

    for (const auto& file : files)
        for (const auto& [commit, edit] : readMessagesFile (file))
            if (const auto found = edits.find (commit); found == edits.end() || edit.changed > found->second.changed)
                edits[commit] = edit;

    return edits;
}

bool VersionStore::writeMessage (const juce::String& versionID, const MessageEdit& edit)
{
    // This computer's file, read and written whole with the commit's message as it is now.
    const auto file = folder.getChildFile ("messages").getChildFile (makeUsableFileName (computer) + ".xml");
    auto edits = readMessagesFile (file);
    edits[versionID] = edit;
    juce::ValueTree element (versions::MESSAGES);

    for (const auto& [commit, message] : edits)
        element.appendChild (juce::ValueTree (versions::MESSAGE, { { versions::messageCommit, commit },
                                                                   { versions::messageName, message.name },
                                                                   { versions::messageNote, message.note },
                                                                   { versions::messageChanged, message.changed } }),
                             nullptr);

    return replaceFile (file, toText (element));
}

bool VersionStore::forgetMessages (const juce::StringArray& versionIDs)
{
    const auto file = folder.getChildFile ("messages").getChildFile (makeUsableFileName (computer) + ".xml");
    auto edits = readMessagesFile (file);

    if (std::none_of (versionIDs.begin(), versionIDs.end(), [&edits] (const juce::String& id) { return edits.count (id) > 0; }))
        return true;

    juce::ValueTree element (versions::MESSAGES);

    for (const auto& [commit, message] : edits)
        if (! versionIDs.contains (commit))
            element.appendChild (juce::ValueTree (versions::MESSAGE, { { versions::messageCommit, commit },
                                                                       { versions::messageName, message.name },
                                                                       { versions::messageNote, message.note },
                                                                       { versions::messageChanged, message.changed } }),
                                 nullptr);

    return replaceFile (file, toText (element));
}

std::vector<Snapshot> VersionStore::readSnapshots (juce::int64 thinAt) const
{
    return readSnapshotsIn (getLocalFolder(), thinAt != 0 ? thinAt : getVersionsTime());
}

std::vector<KeptSong> VersionStore::findKeptSongs (juce::int64 asOf) const
{
    std::vector<KeptSong> songs;
    std::set<juce::String> seen; // each once, however long another computer's history is

    // As of no time: what the files list. Only while the sweep lets go (StoreSweep.h: doesSweepLetGo()), those whose pieces
    // the store still holds, so that a song the sweep let go doesn't leave Clean Up unsure for ever; with nothing let go, a
    // song whose piece isn't here (not arrived, or in a pack that can't be read) is listed, and reading it fails, unsure.
    const auto held = [this, asOf] (const juce::String& piece) { return asOf != 0 || ! doesSweepLetGo() || pieces.has (piece); };

    for (const auto& local : folder.getChildFile ("local").findChildFiles (juce::File::findDirectories, false))
    {
        const auto computerName = local.getFileName();

        for (const auto& [branch, song] : readWorkingIn (local))
            if (song.isNotEmpty() && seen.insert (song).second)
                songs.push_back ({ song, computerName, branch, KeptSong::Kind::left, 0 });

        for (const auto& snapshot : readSnapshotsIn (local, asOf))
            if (snapshot.song.isNotEmpty() && held (snapshot.song) && seen.insert (snapshot.song).second)
                songs.push_back ({ snapshot.song, computerName, snapshot.branch, KeptSong::Kind::snapshot, snapshot.time });
    }

    // The songs left on branches deleted within 30 days, which come back with them (Recover Lost Versions).
    for (const auto& move : readEveryLog())
        if (move.why == "delete" && move.song.isNotEmpty() && (asOf == 0 || asOf - move.time <= keptByAge::dailyFor) && held (move.song)
            && seen.insert (move.song).second)
            songs.push_back ({ move.song, move.computer, move.branch, KeptSong::Kind::deleted, move.time });

    return songs;
}

std::vector<Snapshot> VersionStore::readSnapshotsIn (const juce::File& localFolder, juce::int64 thinAt)
{
    std::vector<Snapshot> snapshots;

    for (const auto& line : readLogText (localFolder.getChildFile ("history")).lines)
        if (const auto parts = juce::StringArray::fromTokens (line, " ", {}); parts.size() >= 4)
            snapshots.push_back ({ parts[0].getLargeIntValue(), parts[1], parts[2], parts.joinIntoString (" ", 3) });

    std::stable_sort (snapshots.begin(), snapshots.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });

    if (thinAt == 0)
        return snapshots;

    // Thinned as of the time given, as the file is when a snapshot is written: one older than 30 days, or one an hour's or
    // a day's newer one stands for, is what the sweep lets go, so it is never offered meanwhile.
    std::vector<juce::int64> times;

    for (const auto& snapshot : snapshots)
        times.push_back (snapshot.time);

    const auto kept = keepByAge (times, thinAt);
    std::vector<Snapshot> thinned;

    for (size_t i = 0; i < snapshots.size(); ++i)
        if (kept[i])
            thinned.push_back (snapshots[i]);

    return thinned;
}
bool VersionStore::writeSnapshots (const std::vector<Snapshot>& snapshots)
{
    juce::String text;

    for (const auto& snapshot : snapshots)
        text << juce::String (snapshot.time) << ' ' << snapshot.branch << ' ' << snapshot.song << ' ' << snapshot.reason << '\n';

    return replaceFile (getLocalFolder().getChildFile ("history"), text);
}

//==============================================================================
juce::File VersionStore::getBounceFile (const juce::String& versionID) const
{
    return getBouncesFolder().getChildFile (makeUsableFileName (versionID) + ".flac");
}

std::vector<VersionRecord::File> VersionStore::hashFiles (const juce::StringArray& paths, const juce::File& projectFolder)
{
    const juce::ScopedLock sl (filesLock);
    const auto cacheFile = getLocalFolder().getChildFile ("files.xml");

    // What was read before, by path: its size, its time and its hash.
    struct Known
    {
        juce::int64 size = 0, modified = 0;
        juce::String sha256;
    };

    std::map<juce::String, Known> known;

    if (const auto xml = juce::parseXML (cacheFile))
        for (const auto* file : xml->getChildWithTagNameIterator (versions::FILE.toString()))
            known[file->getStringAttribute (versions::filePath)] = { file->getStringAttribute (versions::fileSize).getLargeIntValue(),
                                                                     file->getStringAttribute (versions::fileModified).getLargeIntValue(),
                                                                     file->getStringAttribute (versions::fileHash) };

    std::vector<VersionRecord::File> hashed;
    bool readAny = false;

    for (const auto& path : paths)
    {
        const auto file = juce::File::isAbsolutePath (path) ? makeFileOfFullPath (path) : projectFolder.getChildFile (path);

        if (! file.existsAsFile())
            continue;

        const auto size = file.getSize();
        const auto modified = file.getLastModificationTime().toMilliseconds();
        auto& entry = known[path];

        if (entry.sha256.isEmpty() || entry.size != size || entry.modified != modified)
        {
            juce::FileInputStream in (file);

            if (! in.openedOk())
                continue;

            entry = { size, modified, juce::SHA256 (in).toHexString() };
            readAny = true;
        }

        hashed.push_back ({ path, size, entry.sha256 });
    }

    if (readAny)
    {
        juce::ValueTree files (versions::FILES);

        for (const auto& [path, entry] : known)
            if (entry.sha256.isNotEmpty())
                files.appendChild (juce::ValueTree (versions::FILE, { { versions::filePath, path },
                                                                      { versions::fileSize, entry.size },
                                                                      { versions::fileModified, entry.modified },
                                                                      { versions::fileHash, entry.sha256 } }),
                                   nullptr);

        replaceFile (cacheFile, toText (files));
    }

    return hashed;
}

juce::StringArray findAudioFilePaths (const juce::ValueTree& song)
{
    juce::StringArray paths;
    findAudioFilePaths (song, paths);
    return paths;
}
} // namespace phazite
