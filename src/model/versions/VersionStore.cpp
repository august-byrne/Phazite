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
            element.appendChild (juce::ValueTree (versions::FILE, { { versions::filePath, file.path },
                                                                    { versions::fileSize, file.size },
                                                                    { versions::fileHash, file.sha256 } }),
                                 nullptr);

        return element;
    }

    std::optional<VersionRecord> fromElement (const juce::ValueTree& element, const juce::String& id)
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

        for (const auto& file : element)
            if (file.hasType (versions::FILE))
                record.files.push_back ({ file[versions::filePath].toString(), file[versions::fileSize].toString().getLargeIntValue(),
                                          file[versions::fileHash].toString() });

        return record;
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

    bool isSoundFileProperty (const juce::Identifier& property)
    {
        for (int pad = 0; pad < 16; ++pad)
            if (property == schema::getPadSoundFile (pad))
                return true;

        return property == schema::getOscillatorWavetableFile (0) || property == schema::getOscillatorWavetableFile (1);
    }

    void findAudioFilePaths (const juce::ValueTree& element, juce::StringArray& paths)
    {
        if (element.hasType (schema::CLIP))
            if (const auto path = element[schema::clipFile].toString(); path.isNotEmpty())
                paths.addIfNotAlreadyThere (path);

        if (element.hasType (schema::SOUNDS))
            for (int i = 0; i < element.getNumProperties(); ++i)
                if (const auto property = element.getPropertyName (i); isSoundFileProperty (property))
                    if (const auto path = element[property].toString(); path.isNotEmpty())
                        paths.addIfNotAlreadyThere (path);

        for (const auto& child : element)
            findAudioFilePaths (child, paths);
    }
} // namespace

VersionStore::VersionStore (juce::File versionsFolder)
    : folder (std::move (versionsFolder)),
      pieces (folder.getChildFile ("pieces"))
{
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
        return fromElement (readElement (*block), id);

    return std::nullopt;
}

bool VersionStore::writeSong (const SongPieces& song)
{
    return pieces.write (song.pieces);
}

juce::ValueTree VersionStore::readSong (const juce::String& songPiece, PluginParameterValues* parameters) const
{
    return joinSong (songPiece, [this] (const juce::String& name) { return pieces.read (name); }, parameters);
}

juce::File VersionStore::getBranchFile (const juce::String& id) const
{
    return folder.getChildFile ("branches").getChildFile (juce::File::createLegalFileName (id) + ".xml");
}

std::vector<BranchRecord> VersionStore::readBranches() const
{
    std::vector<BranchRecord> branches;

    for (const auto& file : folder.getChildFile ("branches").findChildFiles (juce::File::findFiles, false, "*.xml"))
    {
        const auto xml = juce::parseXML (file);

        if (xml == nullptr || ! xml->hasTagName (versions::BRANCH.toString()))
            continue;

        BranchRecord branch;
        branch.id = xml->getStringAttribute (versions::branchID);
        branch.name = xml->getStringAttribute (versions::branchName);
        branch.colour = juce::Colour::fromString (xml->getStringAttribute (versions::branchColour));
        branch.head = xml->getStringAttribute (versions::branchHead);
        branch.working = xml->getStringAttribute (versions::branchWorking);

        if (branch.id.isNotEmpty())
            branches.push_back (std::move (branch));
    }

    return branches;
}

bool VersionStore::writeBranch (const BranchRecord& branch)
{
    juce::ValueTree element (versions::BRANCH);
    element.setProperty (versions::branchID, branch.id, nullptr);
    element.setProperty (versions::branchName, branch.name, nullptr);
    element.setProperty (versions::branchColour, branch.colour.toString(), nullptr);

    if (branch.head.isNotEmpty())
        element.setProperty (versions::branchHead, branch.head, nullptr);

    if (branch.working.isNotEmpty())
        element.setProperty (versions::branchWorking, branch.working, nullptr);

    return replaceFile (getBranchFile (branch.id), toText (element));
}

juce::String VersionStore::readCurrentBranch() const
{
    return folder.getChildFile ("current").loadFileAsString().trim();
}

bool VersionStore::writeCurrentBranch (const juce::String& id)
{
    return replaceFile (folder.getChildFile ("current"), id);
}

juce::File VersionStore::getBounceFile (const juce::String& versionID) const
{
    return getBouncesFolder().getChildFile (juce::File::createLegalFileName (versionID) + ".flac");
}

std::vector<VersionRecord::File> VersionStore::hashFiles (const juce::StringArray& paths, const juce::File& projectFolder)
{
    const juce::ScopedLock sl (filesLock);
    const auto cacheFile = folder.getChildFile ("files.xml");

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
