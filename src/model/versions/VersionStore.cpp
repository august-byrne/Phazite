// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/VersionStore.h"
#include "model/base/internal/Schema.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>

namespace phazite
{
namespace
{
    namespace versions = schema::versions;

    // A property a branch's file holds that BranchRecord has a field for.
    bool isBranchProperty (const juce::Identifier& name)
    {
        for (const auto* known :
             { &versions::branchID, &versions::branchName, &versions::branchColour, &versions::branchHead, &versions::branchChanged })
            if (name == *known)
                return true;

        return false;
    }

    // A small file replaced whole: written beside it first, then put in its place.
    bool replaceFile (const juce::File& file, const juce::String& text)
    {
        file.getParentDirectory().createDirectory();
        juce::TemporaryFile temporary (file);
        return temporary.getFile().replaceWithText (text, false, false, "\n") && temporary.overwriteTargetFileWithTemporary();
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
      pieces (folder.getChildFile ("pieces"))
{
}

juce::File VersionStore::getLocalFolder() const
{
    return folder.getChildFile ("local").getChildFile (juce::File::createLegalFileName (computer));
}

juce::String VersionStore::writeVersion (VersionRecord record, const SongPieces& song)
{
    record.song = song.song;
    auto block = writeElement (toElement (record));
    auto id = getPieceName (block);

    auto all = song.pieces;
    all.emplace (id, std::move (block));
    return pieces.write (all) ? id : juce::String();
}

std::optional<VersionRecord> VersionStore::readVersion (const juce::String& id) const
{
    if (const auto block = pieces.read (id))
        return toVersionRecord (readElement (*block), id);

    return std::nullopt;
}

bool VersionStore::writeSong (const SongPieces& song)
{
    return pieces.write (song.pieces);
}

juce::ValueTree VersionStore::readSong (const juce::String& songPiece, PluginParameterValues* parameters,
                                        std::vector<MissingPiece>* missing) const
{
    return joinSong (songPiece, [this] (const juce::String& name) { return pieces.read (name); }, parameters, missing);
}

//==============================================================================
juce::File VersionStore::getBranchFile (const juce::String& id, const juce::String& computerID) const
{
    return folder.getChildFile ("branches")
        .getChildFile (juce::File::createLegalFileName (id))
        .getChildFile (juce::File::createLegalFileName (computerID) + ".xml");
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

            if (xml == nullptr || ! xml->hasTagName (versions::BRANCH.toString()))
                continue;

            BranchRecord branch;
            branch.id = xml->getStringAttribute (versions::branchID);
            branch.name = xml->getStringAttribute (versions::branchName);
            branch.colour = juce::Colour::fromString (xml->getStringAttribute (versions::branchColour));
            branch.head = xml->getStringAttribute (versions::branchHead);
            branch.changed = xml->getStringAttribute (versions::branchChanged).getLargeIntValue();
            branch.computer = file.getFileNameWithoutExtension();

            for (const auto& attribute : xml->getAttributeIterator())
                if (! isBranchProperty (attribute.name))
                    branch.others.set (attribute.name, attribute.value);

            if (branch.id.isEmpty() || branch.id != branchFolder.getFileName())
                continue;

            if (branch.computer == juce::File::createLegalFileName (computer))
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
    std::map<juce::String, juce::String> working;

    if (const auto xml = juce::parseXML (getLocalFolder().getChildFile ("working.xml")))
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
bool VersionStore::logBranchMove (const juce::String& branchID, const juce::String& from, const juce::String& to, const juce::String& why)
{
    const auto file = getLocalFolder().getChildFile ("logs").getChildFile (juce::File::createLegalFileName (branchID));

    if (! file.getParentDirectory().createDirectory())
        return false;

    // A line a move: when, from, to, why; "-" for no version. Appended, and flushed to the disk as the stream closes.
    juce::FileOutputStream out (file);

    if (! out.openedOk())
        return false;

    out << juce::String (juce::Time::currentTimeMillis()) << ' ' << (from.isNotEmpty() ? from : "-") << ' ' << (to.isNotEmpty() ? to : "-")
        << ' ' << why << '\n';
    out.flush();
    return out.getStatus().wasOk();
}

std::vector<BranchMove> VersionStore::readBranchLog (const juce::String& branchID) const
{
    std::vector<BranchMove> moves;
    const auto file = getLocalFolder().getChildFile ("logs").getChildFile (juce::File::createLegalFileName (branchID));
    const auto text = file.loadFileAsString();

    // A last line without its end was cut off midway: left out.
    auto lines = juce::StringArray::fromLines (text);

    if (! text.endsWithChar ('\n') && ! lines.isEmpty())
        lines.remove (lines.size() - 1);

    for (const auto& line : lines)
    {
        const auto parts = juce::StringArray::fromTokens (line, " ", {});

        if (parts.size() < 4)
            continue;

        const auto version = [] (const juce::String& part) { return part == "-" ? juce::String() : part; };
        moves.push_back ({ parts[0].getLargeIntValue(), version (parts[1]), version (parts[2]), parts.joinIntoString (" ", 3) });
    }

    return moves;
}

//==============================================================================
juce::File VersionStore::getBounceFile (const juce::String& versionID) const
{
    return getBouncesFolder().getChildFile (juce::File::createLegalFileName (versionID) + ".flac");
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
        const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : projectFolder.getChildFile (path);

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
