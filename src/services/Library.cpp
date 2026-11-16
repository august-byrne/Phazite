// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Library.h"
#include "model/base/Json.h"
#include "model/base/WholeFile.h"
#include "model/engine/AppInfo.h"
#include "model/engine/EngineContext.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    constexpr const char* devicePrefix = "device:";
    constexpr int startBytes = 64 * 1024; // what a set knows a file by, besides its size
    constexpr int mostCandidates = 32;    // files of a member's size looked into in its folder, and in the index, to find it again

    // The words in a name, lowercase: split at anything but letters and digits, between letters and digits
    // ("Kick01", "808Bass") and before a capital that follows a small letter ("DeepKick").
    juce::StringArray getTokens (const juce::String& name)
    {
        juce::StringArray tokens;
        juce::String token;
        juce::juce_wchar previous = 0;

        const auto flush = [&tokens, &token]
        {
            if (token.isNotEmpty())
                tokens.add (token.toLowerCase());

            token.clear();
        };

        for (const auto c : name)
        {
            if (! juce::CharacterFunctions::isLetterOrDigit (c))
            {
                flush();
            }
            else
            {
                const auto boundary = (juce::CharacterFunctions::isDigit (c) != juce::CharacterFunctions::isDigit (previous))
                                      || (juce::CharacterFunctions::isUpperCase (c) && juce::CharacterFunctions::isLowerCase (previous));

                if (boundary && previous != 0)
                    flush();

                token += c;
            }

            previous = juce::CharacterFunctions::isLetterOrDigit (c) ? c : 0;
        }

        flush();
        return tokens;
    }

    struct TagWords
    {
        const char* tag;
        juce::StringArray words;
    };

    // What a sound is, by the words in its name and folders. A word matches a word of the name that is the same, or
    // its plural, or (for words of four letters or more) starts with it, unless that is a tag word itself.
    const std::vector<TagWords>& getTagWords()
    {
        static const std::vector<TagWords> tagWords {
            { "Kick", { "kick", "kik", "bd", "bassdrum" } },
            { "Snare", { "snare", "snr", "sd" } },
            { "Clap", { "clap", "handclap" } },
            { "Hat", { "hat", "hihat", "hh", "hho", "hhc" } },
            { "Cymbal", { "cymbal", "crash", "ride", "china", "splash" } },
            { "Tom", { "tom", "floortom" } },
            { "Percussion",
              { "perc",    "percussion", "shaker", "shake",     "tamb",     "tambourine", "conga",  "bongo",   "cowbell", "rim",
                "rimshot", "snap",       "clave",  "woodblock", "triangle", "guiro",      "cabasa", "timbale", "agogo",   "tabla" } },
            { "Bass", { "bass", "sub", "reese" } }, // and "808" alone (getSoundTags())
            { "FX", { "fx", "sfx", "riser", "uplifter", "downlifter", "impact", "sweep", "whoosh", "swoosh", "noise", "transition" } },
            { "Foley", { "foley", "footstep", "paper", "cloth", "glass", "metal", "wood", "door" } },
            { "Vocal", { "vocal", "vox", "voice", "choir", "acapella", "adlib", "chant" } },
            { "Synth", { "synth", "lead", "pad", "chord", "keys", "piano", "pluck", "arp", "bell", "organ", "stab" } },
        };

        return tagWords;
    }

    bool isPluralOf (const juce::String& token, const juce::String& word)
    {
        return token.length() == word.length() + 1 && token.endsWithChar ('s') && token.startsWith (word);
    }

    // Whether a word is one of the tag words, or the plural of one.
    bool isTagWord (const juce::String& token)
    {
        static const auto all = []
        {
            std::set<juce::String> words;

            for (const auto& family : getTagWords())
            {
                for (const auto& word : family.words)
                {
                    words.insert (word);
                    words.insert (word + "s");
                }
            }

            return words;
        }();

        return all.count (token) > 0;
    }

    bool matchesWord (const juce::String& token, const juce::String& word)
    {
        if (token == word || isPluralOf (token, word))
            return true;

        // A longer word that starts with it, unless that one is a tag word too: "bassdrum" is a kick, not a bass.
        return word.length() >= 4 && token.startsWith (word) && ! isTagWord (token);
    }

    // Two words that make a tag word together, joined: "Bass Drum" and "BassDrum" are a kick, "Hi Hat" a hat.
    juce::StringArray joinTagWords (const juce::StringArray& tokens)
    {
        juce::StringArray joined;

        for (int i = 0; i < tokens.size(); ++i)
        {
            if (i + 1 < tokens.size() && isTagWord (tokens[i] + tokens[i + 1]))
            {
                joined.add (tokens[i] + tokens[i + 1]);
                ++i;
            }
            else
            {
                joined.add (tokens[i]);
            }
        }

        return joined;
    }

    bool hasWord (const juce::StringArray& tokens, const juce::StringArray& words)
    {
        return std::any_of (
            tokens.begin(), tokens.end(), [&words] (const juce::String& token)
            { return std::any_of (words.begin(), words.end(), [&token] (const juce::String& w) { return matchesWord (token, w); }); });
    }

    juce::StringArray getQueryWords (const juce::String& query)
    {
        auto words = juce::StringArray::fromTokens (query.toLowerCase(), " \t", "");
        words.removeEmptyStrings();
        return words;
    }

    std::string toLowerText (const juce::String& text)
    {
        return text.toLowerCase().toStdString();
    }

    // A search, and how well an item matches it, by its search key: 0 if the item's name starts with the query, 1 if a word
    // of its name starts with the query's first word, 2 if its name and tags merely hold every word, 3 if some of the words
    // are only in the names of its folders; -1 if any word is in none of them.
    struct Query
    {
        explicit Query (const juce::String& text)
        {
            const auto typed = getQueryWords (text);

            for (const auto& word : typed)
                words.push_back (word.toStdString());

            phrase = typed.joinIntoString (" ").toStdString();

            if (! words.empty())
                firstWord = " " + words.front();
        }

        bool isEmpty() const noexcept { return words.empty(); }

        int getScore (const SearchKey& key) const
        {
            auto onlyInFolders = false;

            for (const auto& word : words)
            {
                if (key.text.find (word) != std::string::npos)
                    continue;

                if (key.folders.find (word) == std::string::npos)
                    return -1;

                onlyInFolders = true;
            }

            if (onlyInFolders)
                return 3;

            if (key.name.compare (0, phrase.size(), phrase) == 0)
                return 0;

            return key.words.find (firstWord) != std::string::npos ? 1 : 2;
        }

        std::vector<std::string> words;
        std::string phrase, firstWord;
    };

    // Whether a search should stop, asked so often as it goes through the items.
    constexpr size_t itemsBetweenStops = 4096;
} // namespace

//==============================================================================
SearchKey makeSearchKey (const LibraryItem& item, const juce::File& root)
{
    SearchKey key;
    key.name = toLowerText (item.name);

    for (const auto& token : getTokens (item.name))
        key.words += " " + token.toStdString(); // lowercase already

    key.text = key.name + " " + toLowerText (item.tags.joinIntoString (" "));

    // The folders between the root and it, by their names: "Pack 1/Drums/Kicks" as "pack 1 drums kicks".
    if (root != juce::File() && item.file != juce::File())
        if (const auto parent = item.file.getParentDirectory(); parent.isAChildOf (root))
            key.folders = toLowerText (parent.getRelativePathFrom (root).replaceCharacters ("\\/", "  "));

    return key;
}

void LibraryIndex::countTags (const LibraryItem& item)
{
    if (item.kind == LibraryItem::Kind::sound || item.kind == LibraryItem::Kind::midi)
        for (const auto& tag : item.tags)
            ++soundTags[tag];
}

void LibraryIndex::addFile (LibraryItem item, const juce::File& root)
{
    countTags (item);
    fileKeys.push_back (makeSearchKey (item, root));
    files.push_back (std::move (item));
}

void LibraryIndex::addFolder (LibraryItem item, const juce::File& root)
{
    folderKeys.push_back (makeSearchKey (item, root));
    folders.push_back (std::move (item));
}

namespace
{

    //==============================================================================
    // A sound's tags and key: what it is, from the words of its name and folders; a loop or a one-shot, its tempo and its
    // key, from them or from its loop data.
    struct SoundDescription
    {
        juce::StringArray tags;
        std::optional<SoundKey> key;
    };

    SoundDescription describeSound (const juce::File& file, const juce::File& root, const SoundFacts* facts)
    {
        // The words of the name, and of the folders' names between the root and the file.
        const auto name = file.getFileNameWithoutExtension();
        auto tokens = joinTagWords (getTokens (name));
        juce::Array<juce::File> folders; // the nearest first

        for (auto folder = file.getParentDirectory(); folder.isAChildOf (root); folder = folder.getParentDirectory())
        {
            tokens.addArray (joinTagWords (getTokens (folder.getFileName())));
            folders.add (folder);
        }

        SoundDescription description;
        auto& tags = description.tags;

        for (const auto& tagWords : getTagWords())
        {
            // "808" alone is the 808's bass drum, played as a bass; "808 Clap" is a clap.
            const auto is808Bass = juce::String (tagWords.tag) == "Bass" && tags.isEmpty() && tokens.contains ("808");

            if (is808Bass || hasWord (tokens, tagWords.words))
                tags.add (tagWords.tag);
        }

        // A riser made for 4 bars at 128 BPM is still a one-shot. Loop data say what the sound is, and its tempo where its
        // name doesn't.
        auto tempo = getTempoFromName (name);
        auto isLoop = hasWord (tokens, { "loop", "break", "breakbeat", "groove" }) || (tempo.has_value() && ! tags.contains ("FX"));

        if (facts != nullptr && facts->loopData == AudioFileInfo::LoopData::loop)
        {
            isLoop = true;

            if (const auto beatsPerMinute = facts->seconds > 0.0 ? facts->loopBeats * 60.0 / facts->seconds : 0.0;
                ! tempo.has_value() && beatsPerMinute >= 40.0 && beatsPerMinute <= 300.0)
                tempo = std::round (beatsPerMinute);
        }
        else if (facts != nullptr && facts->loopData == AudioFileInfo::LoopData::oneShot)
        {
            isLoop = false;
        }

        tags.add (isLoop ? "Loop" : "One-shot");

        if (tempo.has_value())
            tags.add (juce::String (juce::roundToInt (*tempo)) + " BPM");

        // Its key: its name's, a folder's with minor or major, or its loop data's.
        description.key = getKeyFromName (name);

        for (int i = 0; i < folders.size() && ! description.key.has_value(); ++i)
            description.key = getKeyFromName (folders[i].getFileName(), false);

        if (! description.key.has_value() && facts != nullptr)
            description.key = facts->key;

        if (description.key.has_value())
            tags.add (description.key->describe());

        return description;
    }

    //==============================================================================
    // The files whose loop data the index reads: WAV and AIFF, which keep them in a chunk of their own.
    bool mayHaveLoopData (const juce::File& file)
    {
        return file.hasFileExtension ("wav;wave;aif;aiff;aifc");
    }

    SoundFacts readFacts (const juce::File& file, juce::int64 size, juce::int64 modified)
    {
        SoundFacts facts { size, modified };

        if (const std::unique_ptr<juce::AudioFormatReader> reader (getAudioFormats().createReaderFor (file)); reader != nullptr)
        {
            const auto info = readAudioFileInfo (*reader, {}); // its loop data alone, not its name's tempo
            facts.loopData = info.loopData;
            facts.loopBeats = info.loopData == AudioFileInfo::LoopData::loop ? info.loopBeats : 0.0;
            facts.seconds = info.getSeconds();
            facts.key = info.key;
        }

        return facts;
    }

    // The facts file: a line for each file, its size, when it was changed, its loop data, beats, seconds, key and path.
    juce::String toLine (const juce::String& path, const SoundFacts& facts)
    {
        juce::StringArray fields { juce::String (facts.size),
                                   juce::String (facts.modified),
                                   juce::String ((int) facts.loopData),
                                   juce::String (facts.loopBeats),
                                   juce::String (facts.seconds),
                                   juce::String (facts.key.has_value() ? facts.key->root : -1),
                                   juce::String (facts.key.has_value() ? (int) facts.key->mode : 0),
                                   path };
        return fields.joinIntoString ("\t");
    }

    std::map<juce::String, SoundFacts> readFactsFile (const juce::File& file)
    {
        std::map<juce::String, SoundFacts> read;
        juce::StringArray lines;
        file.readLines (lines);

        for (const auto& line : lines)
        {
            const auto fields = juce::StringArray::fromTokens (line, "\t", "");

            if (fields.size() != 8 || ! juce::File::isAbsolutePath (fields[7]))
                continue;

            SoundFacts facts { fields[0].getLargeIntValue(), fields[1].getLargeIntValue() };
            facts.loopData = (AudioFileInfo::LoopData) juce::jlimit (0, 2, fields[2].getIntValue());
            facts.loopBeats = readDouble (fields[3]);
            facts.seconds = readDouble (fields[4]);

            if (const auto root = fields[5].getIntValue(); juce::isPositiveAndBelow (root, 12))
                facts.key = SoundKey { root, (SoundKey::Mode) juce::jlimit (0, 2, fields[6].getIntValue()) };

            read[fields[7]] = facts;
        }

        return read;
    }

    void writeFactsFile (const juce::File& file, const std::map<juce::String, SoundFacts>& facts)
    {
        juce::MemoryOutputStream text;

        for (const auto& [path, each] : facts)
            text << toLine (path, each) << "\n";

        // Whole or not at all: one that couldn't be written is read again from the sounds at the next start.
        file.getParentDirectory().createDirectory();

        if (const auto written = writeWholeFile (file, text.getData(), text.getDataSize()); written.failed())
            juce::Logger::writeToLog ("Phazite couldn't keep what the library read of its sounds: " + written.getErrorMessage());
    }

    //==============================================================================
    // The SHA-256 of a file's first 64 KiB: with its size, what a set knows it by.
    juce::String readStart (const juce::File& file)
    {
        juce::FileInputStream stream (file);

        if (! stream.openedOk())
            return {};

        juce::MemoryBlock block;
        stream.readIntoMemoryBlock (block, startBytes);
        return juce::SHA256 (block).toHexString();
    }
} // namespace

//==============================================================================
juce::StringArray getSoundTags (const juce::File& file, const juce::File& root, const SoundFacts* facts)
{
    return describeSound (file, root, facts).tags;
}

std::optional<SoundKey> getSoundKey (const juce::File& file, const juce::File& root, const SoundFacts* facts)
{
    return describeSound (file, root, facts).key;
}

juce::StringArray getSoundTypesIn (const juce::String& name)
{
    const auto tokens = joinTagWords (getTokens (name));
    juce::StringArray types;

    for (const auto& tagWords : getTagWords())
        if ((juce::String (tagWords.tag) == "Bass" && types.isEmpty() && tokens.contains ("808")) || hasWord (tokens, tagWords.words))
            types.add (tagWords.tag);

    return types;
}

juce::String LibraryItem::getKey() const
{
    if (device.has_value())
        return devicePrefix + device->description.createIdentifierString();

    return file.getFullPathName();
}

//==============================================================================
struct Library::Indexer : public juce::Thread
{
    Indexer (Library& o, juce::Array<juce::File> rootsToScan, std::vector<Member> membersToFind)
        : juce::Thread ("Phazite library index"),
          owner (o),
          roots (std::move (rootsToScan)),
          members (std::move (membersToFind))
    {
    }

    // It looks at threadShouldExit() between files: never ended by force, which can leave a lock it held held for ever.
    ~Indexer() override { stopThread (-1); }

    // A file found, with its root, its size and when it was last changed (in milliseconds).
    struct Found
    {
        juce::File file, root;
        juce::int64 size = 0, modified = 0;
    };

    // A folder's files, and those of its folders where the listing meets them; a folder that links to one
    // the walk is already inside (a loop, which would be followed to the path's limit) is left out. One that links elsewhere,
    // as a folder of samples on another drive, is followed. Folders are told apart by what the file system knows them by
    // (File::getFileIdentifier(), which follows links), so a loop is found however its links are written. That identifier is
    // the inode on Linux and macOS and the file index on Windows, neither with the volume it is on, so two folders on
    // different volumes can share one (every NTFS volume's root has the same index), and a folder linked from another volume
    // could be taken for one the walk is inside, and skipped (docs/backlog.md: "The browser library's loop check keyed by
    // volume and file identifier").
    void walk (const juce::File& folder, const juce::File& root, std::vector<juce::uint64>& inside, std::vector<Found>& files)
    {
        const auto id = folder.getFileIdentifier();

        if (id != 0 && std::find (inside.begin(), inside.end(), id) != inside.end())
            return;

        inside.push_back (id);

        for (const auto& entry :
             juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
        {
            if (threadShouldExit() || (int) files.size() >= maxIndexed)
                break;

            if (const auto& file = entry.getFile(); entry.isDirectory())
            {
                // Search finds a folder by its name too, as Ableton Live's and FL Studio's browsers do.
                if ((int) folders.size() < maxIndexed)
                    folders.push_back ({ file, root });

                walk (file, root, inside, files);
            }
            else if (owner.isSoundFile (file) || owner.isMidiFile (file) || isPreset (file))
                files.push_back ({ file, root, entry.getFileSize(), entry.getModificationTime().toMilliseconds() });
        }

        inside.pop_back();
    }

    void run() override
    {
        std::vector<Found> files;

        for (const auto& root : roots)
        {
            std::vector<juce::uint64> inside;
            walk (root, root, inside, files);
        }

        if (threadShouldExit())
            return;

        // What was read of the files before, by the last index or, at first, from one start to the next.
        std::map<juce::String, SoundFacts> known;

        {
            const juce::ScopedLock lock (owner.indexLock);
            known = owner.facts;
        }

        if (known.empty() && owner.factsFile.existsAsFile())
            known = readFactsFile (owner.factsFile);

        // First from their names, with what was read of them before, as it was read.
        std::map<juce::String, SoundFacts> read;
        std::vector<size_t> unread;
        std::vector<LibraryItem> items;
        items.reserve (files.size());

        for (size_t i = 0; i < files.size(); ++i)
        {
            const auto& found = files[i];
            const SoundFacts* fact = nullptr;

            if (mayHaveLoopData (found.file))
            {
                const auto path = found.file.getFullPathName();

                if (const auto k = known.find (path);
                    k != known.end() && k->second.size == found.size && k->second.modified == found.modified)
                {
                    auto& kept = read[path];
                    kept = k->second;
                    fact = &kept;
                }
                else
                {
                    unread.push_back (i);
                }
            }

            items.push_back (owner.makeFileItem (found.file, found.root, fact, true));
        }

        // The folders, and the search keys of both, made here once rather than at each key typed (known bug 128).
        for (const auto& found : folders)
        {
            LibraryItem folder;
            folder.kind = LibraryItem::Kind::folder;
            folder.name = found.file.getFileName();
            folder.file = found.file;
            folderItems.push_back (std::move (folder));
            folderKeys.push_back (makeSearchKey (folderItems.back(), found.root));
        }

        full = (int) files.size() >= maxIndexed;
        publish (files, items, read);

        // The members of the sets that went since, looked for among the files found.
        findMovedMembers (files);

        // Then again with the loop data of the WAV and AIFF files not read before.
        for (const auto i : unread)
        {
            if (threadShouldExit())
                return;

            const auto& found = files[i];
            auto& fact = read[found.file.getFullPathName()];
            fact = readFacts (found.file, found.size, found.modified);
            items[i] = owner.makeFileItem (found.file, found.root, &fact, true);
        }

        if (! unread.empty())
            publish (files, items, read);

        // Kept for the next start, when anything changed.
        if (owner.factsFile != juce::File() && (! unread.empty() || read.size() != known.size()))
            writeFactsFile (owner.factsFile, read);
    }

    void publish (const std::vector<Found>& files, const std::vector<LibraryItem>& items, const std::map<juce::String, SoundFacts>& read)
    {
        auto made = std::make_shared<LibraryIndex>();
        made->files = items;
        made->fileKeys.reserve (items.size());

        for (size_t i = 0; i < items.size(); ++i)
        {
            made->fileKeys.push_back (makeSearchKey (items[i], files[i].root));
            made->countTags (items[i]);
        }

        made->folders = folderItems;
        made->folderKeys = folderKeys;
        made->full = full;
        std::shared_ptr<const LibraryIndex> built = std::move (made);

        {
            const juce::ScopedLock lock (owner.indexLock);
            owner.index = std::move (built);
            owner.facts = read;
        }

        owner.triggerAsyncUpdate();
    }

    // Each member of the sets that isn't where it was found last, looked for where it moved: among the files of its size in
    // the folder it was in (renamed there), then among those of the index, anywhere in the library; the one that starts as
    // it does. Here, once for each index, rather than as the sets are listed on the message thread (known bug 114).
    void findMovedMembers (const std::vector<Found>& files)
    {
        std::multimap<juce::int64, juce::File> bySize;

        for (const auto& found : files)
            bySize.emplace (found.size, found.file);

        std::set<juce::String> looked;
        std::map<juce::String, juce::String> moved;

        for (const auto& member : members)
        {
            if (threadShouldExit())
                return;

            if (member.size < 0 || member.start.isEmpty() || isStillThere (member) || ! looked.insert (member.key).second)
                continue;

            std::vector<juce::File> candidates;

            for (const auto& entry :
                 juce::RangedDirectoryIterator (juce::File (member.key).getParentDirectory(), false, "*", juce::File::findFiles))
                if (entry.getFileSize() == member.size && (int) candidates.size() < mostCandidates)
                    candidates.push_back (entry.getFile());

            const auto [from, to] = bySize.equal_range (member.size);

            for (auto i = from; i != to && (int) candidates.size() < 2 * mostCandidates; ++i)
                candidates.push_back (i->second);

            for (const auto& file : candidates)
            {
                if (file.getFullPathName() != member.key && file.existsAsFile() && readStart (file) == member.start)
                {
                    moved[member.key] = file.getFullPathName();
                    break;
                }
            }
        }

        {
            const juce::ScopedLock lock (owner.indexLock);
            owner.lookedFor = std::move (looked);
            owner.movedTo = std::move (moved);
        }

        owner.triggerAsyncUpdate();
    }

    Library& owner;
    const juce::Array<juce::File> roots;
    const std::vector<Member> members; // of the sets, as they were when the index started
    std::vector<Found> folders;        // as the walk found them
    std::vector<LibraryItem> folderItems;
    std::vector<SearchKey> folderKeys;
    bool full = false;
};

//==============================================================================
Library::Library (EngineContext& e, Settings& s, juce::File factory, juce::File presets, juce::File factoryPresets, juce::File factsToKeep)
    : engine (e),
      settings (s),
      factorySounds (std::move (factory)),
      presetsFolder (std::move (presets)),
      factoryPresetsFolder (std::move (factoryPresets)),
      factsFile (std::move (factsToKeep)),
      audioExtensions (engine.getAudioFileExtensions()), // which the index can check on its own thread
      index (std::make_shared<const LibraryIndex>())
{
    readSets();
    reindex();
}

Library::~Library()
{
    cancelSearch();
    cancelCount();
    searches.removeAll (true); // a search looks at whether it should stop between thousands of items
    searchDelivery.cancelPendingUpdate();
    countDelivery.cancelPendingUpdate();
    indexer = nullptr;
    cancelPendingUpdate();
}

bool Library::isSoundFile (const juce::File& file) const
{
    return audioExtensions.contains (file.getFileExtension().toLowerCase());
}

bool Library::isMidiFile (const juce::File& file) const
{
    return file.hasFileExtension (".mid;.midi");
}

bool Library::isPreset (const juce::File& file)
{
    return isPresetFile (file) && readPresetInfo (file).has_value();
}

juce::File Library::getDefaultPresetsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Presets");
}

juce::File Library::getDefaultFactoryPresetsFolder()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("presets");
}

juce::Array<juce::File> Library::getUserFolders() const
{
    juce::Array<juce::File> folders;

    for (const auto& path : settings.get (Settings::browserFolders))
        if (juce::File::isAbsolutePath (path))
            folders.add (juce::File (path));

    return folders;
}

void Library::addUserFolder (const juce::File& folder)
{
    auto list = settings.get (Settings::browserFolders);

    if (list.contains (folder.getFullPathName()))
        return;

    list.add (folder.getFullPathName());
    settings.set (Settings::browserFolders, list);
    reindex();
}

void Library::removeUserFolder (const juce::File& folder)
{
    auto list = settings.get (Settings::browserFolders);
    list.removeString (folder.getFullPathName());
    settings.set (Settings::browserFolders, list);
    reindex();
}

juce::Array<juce::File> Library::getRoots() const
{
    juce::Array<juce::File> roots;

    if (factorySounds.isDirectory())
        roots.add (factorySounds);

    for (const auto& folder : getUserFolders())
        if (folder.isDirectory())
            roots.add (folder);

    if (presetsFolder.isDirectory())
        roots.add (presetsFolder);

    if (factoryPresetsFolder.isDirectory())
        roots.add (factoryPresetsFolder);

    return roots;
}

juce::File Library::findRoot (const juce::File& file) const
{
    for (const auto& root : getRoots())
        if (file == root || file.isAChildOf (root))
            return root;

    return file.getParentDirectory();
}

LibraryItem Library::makeFileItem (const juce::File& file, const juce::File& root) const
{
    std::optional<SoundFacts> known;

    if (mayHaveLoopData (file))
    {
        const juce::ScopedLock lock (indexLock);

        if (const auto found = facts.find (file.getFullPathName()); found != facts.end())
            known = found->second;
    }

    return makeFileItem (file, root, known.has_value() ? &*known : nullptr, false);
}

LibraryItem Library::makeFileItem (const juce::File& file, const juce::File& root, const SoundFacts* known, bool knownFile) const
{
    LibraryItem item;
    item.file = file;

    if (! knownFile && file.isDirectory())
    {
        item.kind = LibraryItem::Kind::folder;
        item.name = file.getFileName();
        return item;
    }

    // A preset, tagged with what it holds: "Track", "Container", or its device's name.
    if (isPresetFile (file))
    {
        item.kind = LibraryItem::Kind::preset;
        item.name = file.getFileNameWithoutExtension();
        item.preset = readPresetInfo (file);
        item.tags.add ("Preset");

        if (item.preset.has_value())
            item.tags.add (item.preset->kind == PresetKind::track       ? juce::String ("Track")
                           : item.preset->kind == PresetKind::container ? juce::String ("Container")
                                                                        : item.preset->device);

        return item;
    }

    item.kind = isMidiFile (file) ? LibraryItem::Kind::midi : LibraryItem::Kind::sound;
    item.name = file.getFileNameWithoutExtension();
    auto description = describeSound (file, root, known);
    item.tags = std::move (description.tags);
    item.key = description.key;
    return item;
}

std::vector<LibraryItem> Library::listFolder (const juce::File& folder) const
{
    std::vector<LibraryItem> folders, files;
    const auto root = findRoot (folder);

    for (const auto& entry :
         juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
    {
        const auto& file = entry.getFile();

        if (entry.isDirectory())
            folders.push_back (makeFileItem (file, root));
        else if (isSoundFile (file) || isMidiFile (file) || isPreset (file))
            files.push_back (makeFileItem (file, root));
    }

    const auto byName = [] (const LibraryItem& a, const LibraryItem& b) { return a.name.compareNatural (b.name) < 0; };
    std::sort (folders.begin(), folders.end(), byName);
    std::sort (files.begin(), files.end(), byName);
    folders.insert (folders.end(), files.begin(), files.end());
    return folders;
}

std::vector<LibraryItem> Library::listDevices (LibraryItem::Kind kind) const
{
    std::vector<LibraryItem> items;

    for (const auto& type : engine.getDeviceTypes())
    {
        if (type.isInstrument != (kind == LibraryItem::Kind::instrument))
            continue;

        LibraryItem item;
        item.kind = kind;
        item.name = type.name;
        item.device = type;
        item.tags = { type.isInstrument ? "Instrument" : "Effect" };

        if (type.description.manufacturerName.isNotEmpty())
            item.tags.add (type.description.manufacturerName);

        items.push_back (std::move (item));
    }

    return items;
}

std::vector<LibraryItem> Library::getAllDevices() const
{
    auto devices = listDevices (LibraryItem::Kind::instrument);
    auto effects = listDevices (LibraryItem::Kind::effect);
    devices.insert (devices.end(), std::make_move_iterator (effects.begin()), std::make_move_iterator (effects.end()));
    return devices;
}

std::vector<LibraryItem> Library::search (const juce::String& query, int maximum, const Accepts& accepts) const
{
    const auto indexed = getWholeIndex(); // held while it is gone through: the index thread may replace it meanwhile
    return searchIndex (*indexed, getAllDevices(), query, maximum, accepts).items;
}

SearchResults Library::searchIndex (const LibraryIndex& index, const std::vector<LibraryItem>& devices, const juce::String& query,
                                    int maximum, const Accepts& accepts, const std::function<bool()>& shouldStop)
{
    SearchResults results;
    const Query wanted (query);

    if (wanted.isEmpty())
        return results;

    // The matches by their scores and where they are; the items themselves copied only for the best.
    struct Match
    {
        int score;
        const LibraryItem* item;
        size_t place; // in the index: the last of the ties, so that equals keep their order from key to key
    };

    std::vector<Match> matches;
    size_t looked = 0;

    const auto consider = [&] (const LibraryItem& item, const SearchKey& key)
    {
        if (const auto score = wanted.getScore (key); score >= 0 && (accepts == nullptr || accepts (item)))
            matches.push_back ({ score, &item, looked });
    };

    const auto through = [&] (const std::vector<LibraryItem>& items, const std::vector<SearchKey>& keys)
    {
        for (size_t i = 0; i < items.size() && i < keys.size(); ++i, ++looked)
        {
            if (looked % itemsBetweenStops == 0 && shouldStop != nullptr && shouldStop())
                return false;

            consider (items[i], keys[i]);
        }

        return true;
    };

    std::vector<SearchKey> deviceKeys;

    for (const auto& device : devices)
        deviceKeys.push_back (makeSearchKey (device, {}));

    const auto finished =
        through (index.files, index.fileKeys) && through (index.folders, index.folderKeys) && through (devices, deviceKeys);

    if (juce::MessageManager::existsAndIsCurrentThread())
        searchedOnMessageThread += (juce::int64) looked;

    if (! finished)
    {
        results.stopped = true;
        return results;
    }

    // The best first: sounds, presets and devices before folders, which are a way to them; then by score, then shorter
    // names, then by name, then by their places in the index (std::partial_sort keeps no order of its own among equals).
    const auto better = [] (const Match& a, const Match& b)
    {
        if (const auto aFolder = a.item->kind == LibraryItem::Kind::folder; aFolder != (b.item->kind == LibraryItem::Kind::folder))
            return ! aFolder;

        if (a.score != b.score)
            return a.score < b.score;

        if (a.item->name.length() != b.item->name.length())
            return a.item->name.length() < b.item->name.length();

        if (const auto byName = a.item->name.compareNatural (b.item->name); byName != 0)
            return byName < 0;

        return a.place < b.place;
    };

    const auto kept = std::min (matches.size(), (size_t) std::max (0, maximum));
    std::partial_sort (matches.begin(), matches.begin() + (std::ptrdiff_t) kept, matches.end(), better);
    results.total = (int) matches.size();
    results.items.reserve (kept);

    for (size_t i = 0; i < kept; ++i)
        results.items.push_back (*matches[i].item);

    return results;
}

void Library::startSearch (const juce::String& query, int maximum, Accepts accepts, std::function<void (SearchResults)> found) const
{
    JUCE_ASSERT_MESSAGE_THREAD

    // What it looks through, taken here: the index as it is now (shared, never changed) and the devices.
    const auto number = ++searchNumber;
    searchFound = std::move (found);
    searches.add (
        [this, number, indexed = getWholeIndex(), devices = getAllDevices(), query, maximum, accepts = std::move (accepts),
         before = beforeSearch] (const BackgroundJobs::ShouldStop& shouldStop)
        {
            if (before != nullptr)
                before();

            const auto isOld = [this, number, &shouldStop] { return shouldStop() || searchNumber.load() != number; };

            if (isOld())
                return;

            auto results = searchIndex (*indexed, devices, query, maximum, accepts, isOld);

            if (results.stopped || isOld())
                return;

            {
                const std::scoped_lock held (deliveredLock);
                delivered.emplace (number, std::move (results));
            }

            searchDelivery.triggerAsyncUpdate();
        });
}

void Library::cancelSearch() const
{
    ++searchNumber;
    searchFound = nullptr;

    const std::scoped_lock held (deliveredLock);
    delivered.reset();
}

void Library::deliverSearch() const
{
    std::optional<std::pair<int, SearchResults>> taken;

    {
        const std::scoped_lock held (deliveredLock);
        taken.swap (delivered);
    }

    // The newest search's alone: one started since, or cancelled, has its own (or none).
    if (! taken.has_value() || taken->first != searchNumber.load() || searchFound == nullptr)
        return;

    const auto found = std::exchange (searchFound, nullptr);
    found (std::move (taken->second));
}

bool Library::forEachFileMatch (const LibraryIndex& index, const juce::String& query,
                                const std::function<void (const LibraryItem&)>& matched, const std::function<bool()>& shouldStop)
{
    // No text: every one. Text without words (punctuation alone): none, as search finds none.
    const Query wanted (query);
    const auto all = query.trim().isEmpty();

    if (! all && wanted.isEmpty())
        return true;

    size_t looked = 0;
    auto finished = true;

    for (; looked < index.files.size() && looked < index.fileKeys.size(); ++looked)
    {
        if (looked % itemsBetweenStops == 0 && shouldStop != nullptr && shouldStop())
        {
            finished = false;
            break;
        }

        if (all || wanted.getScore (index.fileKeys[looked]) >= 0)
            matched (index.files[looked]);
    }

    if (juce::MessageManager::existsAndIsCurrentThread())
        searchedOnMessageThread += (juce::int64) looked;

    return finished;
}

void Library::startCount (Count count, std::function<void()> done) const
{
    JUCE_ASSERT_MESSAGE_THREAD

    // What it goes through, taken here: the index as it is now (shared, never changed).
    const auto number = ++countNumber;
    countDone = std::move (done);
    searches.add (
        [this, number, indexed = getWholeIndex(), count = std::move (count),
         before = beforeSearch] (const BackgroundJobs::ShouldStop& shouldStop)
        {
            if (before != nullptr)
                before();

            const auto isOld = [this, number, &shouldStop] { return shouldStop() || countNumber.load() != number; };

            if (isOld() || ! count (*indexed, isOld) || isOld())
                return;

            {
                const std::scoped_lock held (deliveredLock);
                counted = number;
            }

            countDelivery.triggerAsyncUpdate();
        });
}

void Library::cancelCount() const
{
    ++countNumber;
    countDone = nullptr;

    const std::scoped_lock held (deliveredLock);
    counted.reset();
}

void Library::deliverCount() const
{
    std::optional<int> taken;

    {
        const std::scoped_lock held (deliveredLock);
        taken.swap (counted);
    }

    // The newest count's alone: one started since, or cancelled, has its own (or none).
    if (! taken.has_value() || *taken != countNumber.load() || countDone == nullptr)
        return;

    const auto done = std::exchange (countDone, nullptr);
    done();
}

std::vector<LibraryItem> Library::searchDevices (const juce::String& query) const
{
    // Instruments, then effects, each with the built-in ones first, then by name.
    const auto byName = [] (const LibraryItem& a, const LibraryItem& b)
    {
        const auto isBuiltIn = [] (const LibraryItem& i) { return i.device.has_value() && i.device->isBuiltIn; };

        if (isBuiltIn (a) != isBuiltIn (b))
            return isBuiltIn (a);

        return a.name.compareNatural (b.name) < 0;
    };
    auto devices = listDevices (LibraryItem::Kind::instrument);
    auto effects = listDevices (LibraryItem::Kind::effect);
    std::stable_sort (devices.begin(), devices.end(), byName);
    std::stable_sort (effects.begin(), effects.end(), byName);
    devices.insert (devices.end(), effects.begin(), effects.end());

    if (Query (query).isEmpty())
        return devices;

    return searchIndex ({}, devices, query, (int) devices.size(), nullptr).items;
}

bool Library::isIndexing() const
{
    return indexer != nullptr && indexer->isThreadRunning();
}

std::shared_ptr<const LibraryIndex> Library::getWholeIndex() const
{
    const juce::ScopedLock lock (indexLock);
    return index;
}

std::shared_ptr<const std::vector<LibraryItem>> Library::getIndex() const
{
    const auto whole = getWholeIndex();
    return { whole, &whole->files }; // which keeps the whole index while it lives
}

bool Library::isIndexFull() const
{
    return getWholeIndex()->full;
}

juce::StringArray Library::getSoundTypes()
{
    juce::StringArray types;

    for (const auto& family : getTagWords())
        types.add (family.tag);

    types.add ("Loop");
    return types;
}

void Library::reindex()
{
    indexer = nullptr; // stops the one before
    indexer = std::make_unique<Indexer> (*this, getRoots(), getAllMembers());
    indexer->startThread (juce::Thread::Priority::low);
}

void Library::handleAsyncUpdate()
{
    // Members of the sets renamed or moved since, found again, before the browser lists them.
    takeMovedMembers();

    if (onIndexChanged != nullptr)
        onIndexChanged();
}

//==============================================================================
void Library::readSets()
{
    // A member's line: its size, its start and its path, for a file known by them; else its key alone.
    const auto read = [] (const juce::String& line)
    {
        const auto fields = juce::StringArray::fromTokens (line, "\t", "");
        return fields.size() == 3 ? Member { fields[2], fields[0].getLargeIntValue(), fields[1] } : Member { line };
    };

    favorites.clear();

    for (const auto& line : settings.get (Settings::browserFavorites))
        favorites.push_back (read (line));

    collections.clear();

    const auto parsed = parseJson (settings.get (Settings::browserCollections)); // which the list points into

    if (const auto* list = parsed.getArray())
    {
        for (const auto& saved : *list)
        {
            Collection collection { saved["name"].toString().trim() };

            if (collection.name.isEmpty() || findCollection (collection.name) != nullptr)
                continue;

            if (const auto* members = saved["items"].getArray())
                for (const auto& line : *members)
                    collection.members.push_back (read (line.toString()));

            collections.push_back (std::move (collection));
        }
    }

    favoriteKeys.clear();

    for (const auto& member : favorites)
        favoriteKeys.insert (member.key);
}

namespace
{
    template <typename MemberType>
    juce::String toSavedLine (const MemberType& member)
    {
        return member.size >= 0 && member.start.isNotEmpty() ? juce::String (member.size) + "\t" + member.start + "\t" + member.key
                                                             : member.key;
    }
} // namespace

void Library::writeFavorites() const
{
    juce::StringArray lines;
    favoriteKeys.clear();

    for (const auto& member : favorites)
    {
        lines.add (toSavedLine (member));
        favoriteKeys.insert (member.key);
    }

    settings.set (Settings::browserFavorites, lines);
}

void Library::writeCollections() const
{
    juce::Array<juce::var> list;

    for (const auto& collection : collections)
    {
        juce::Array<juce::var> lines;

        for (const auto& member : collection.members)
            lines.add (toSavedLine (member));

        auto saved = juce::DynamicObject::Ptr (new juce::DynamicObject());
        saved->setProperty ("name", collection.name);
        saved->setProperty ("items", lines);
        list.add (juce::var (saved.get()));
    }

    settings.set (Settings::browserCollections, juce::JSON::toString (list, true));
}

Library::Member Library::makeMember (const LibraryItem& item) const
{
    Member member { item.getKey() };

    // A sound, MIDI file or preset is known by its size and its start too.
    if (item.isFile() && item.kind != LibraryItem::Kind::folder && item.file.existsAsFile())
    {
        member.size = item.file.getSize();
        member.start = readStart (item.file);
    }

    return member;
}

bool Library::isStillThere (const Member& member)
{
    if (member.key.startsWith (devicePrefix) || ! juce::File::isAbsolutePath (member.key))
        return true; // a device, found by findItem()

    const juce::File file (member.key);
    return member.size < 0 ? file.exists() : file.existsAsFile() && file.getSize() == member.size;
}

std::vector<Library::Member> Library::getAllMembers() const
{
    auto all = favorites;

    for (const auto& collection : collections)
        all.insert (all.end(), collection.members.begin(), collection.members.end());

    return all;
}

std::vector<LibraryItem> Library::getItems (const std::vector<Member>& members) const
{
    std::vector<LibraryItem> items;
    auto lookAgain = false;

    for (const auto& member : members)
    {
        if (auto item = findItem (member.key))
            items.push_back (std::move (*item));
        else if (member.size >= 0 && member.start.isNotEmpty() && searchedFor.insert (member.key).second)
            lookAgain = true; // gone since the index looked: looked for by the next
    }

    // A Library is never made const: what lists it is const, and asks for the index again. Not while one is built, which
    // looks for the members gone by then.
    if (lookAgain && ! isIndexing())
        const_cast<Library*> (this)->reindex();

    return items;
}

void Library::takeMovedMembers()
{
    std::map<juce::String, juce::String> moved;

    {
        const juce::ScopedLock lock (indexLock);
        moved = std::exchange (movedTo, {});
        searchedFor.insert (lookedFor.begin(), lookedFor.end());
    }

    const auto follow = [&moved] (std::vector<Member>& members)
    {
        auto any = false;

        for (auto& member : members)
        {
            if (const auto found = moved.find (member.key); found != moved.end())
            {
                member.key = found->second;
                any = true;
            }
        }

        return any;
    };

    if (follow (favorites))
        writeFavorites();

    auto collectionsMoved = false;

    for (auto& collection : collections)
        collectionsMoved = follow (collection.members) || collectionsMoved;

    if (collectionsMoved)
        writeCollections();
}

bool Library::isFavorite (const LibraryItem& item) const
{
    return favoriteKeys.count (item.getKey()) > 0;
}

void Library::setFavorite (const LibraryItem& item, bool favorite)
{
    const auto member = makeMember (item);

    // Once in favorites, under its key now or as it was before it moved.
    favorites.erase (
        std::remove_if (favorites.begin(), favorites.end(), [&] (const Member& m)
                        { return m.key == member.key || (member.size >= 0 && m.size == member.size && m.start == member.start); }),
        favorites.end());

    if (favorite)
        favorites.push_back (member);

    writeFavorites();
}

std::vector<LibraryItem> Library::getFavorites() const
{
    return getItems (favorites);
}

Library::Collection* Library::findCollection (const juce::String& name) const
{
    for (auto& collection : collections)
        if (collection.name == name)
            return &collection;

    return nullptr;
}

juce::StringArray Library::getCollections() const
{
    juce::StringArray names;

    for (const auto& collection : collections)
        names.add (collection.name);

    return names;
}

juce::String Library::addCollection (const juce::String& name)
{
    const auto wanted = name.trim();

    if (wanted.isEmpty())
        return {};

    // "Kicks", then "Kicks 2", "Kicks 3"...
    auto unique = wanted;

    for (int number = 2; findCollection (unique) != nullptr; ++number)
        unique = wanted + " " + juce::String (number);

    collections.push_back ({ unique });
    writeCollections();
    return unique;
}

bool Library::renameCollection (const juce::String& name, const juce::String& newName)
{
    const auto wanted = newName.trim();
    auto* collection = findCollection (name);

    if (collection == nullptr || wanted.isEmpty() || (wanted != name && findCollection (wanted) != nullptr))
        return false;

    collection->name = wanted;
    writeCollections();
    return true;
}

void Library::removeCollection (const juce::String& name)
{
    collections.erase (std::remove_if (collections.begin(), collections.end(), [&name] (const Collection& c) { return c.name == name; }),
                       collections.end());
    writeCollections();
}

bool Library::isInCollection (const LibraryItem& item, const juce::String& name) const
{
    const auto* collection = findCollection (name);
    const auto key = item.getKey();
    return collection != nullptr
           && std::any_of (collection->members.begin(), collection->members.end(), [&key] (const Member& m) { return m.key == key; });
}

void Library::setInCollection (const LibraryItem& item, const juce::String& name, bool in)
{
    auto* collection = findCollection (name);

    if (collection == nullptr)
        return;

    const auto member = makeMember (item);
    auto& members = collection->members;
    members.erase (
        std::remove_if (members.begin(), members.end(), [&] (const Member& m)
                        { return m.key == member.key || (member.size >= 0 && m.size == member.size && m.start == member.start); }),
        members.end());

    if (in)
        members.push_back (member);

    writeCollections();
}

std::vector<LibraryItem> Library::getCollection (const juce::String& name) const
{
    auto* collection = findCollection (name);

    if (collection == nullptr)
        return {};

    return getItems (collection->members);
}

//==============================================================================
void Library::noteUsed (const LibraryItem& item)
{
    auto list = settings.get (Settings::browserRecent);
    list.removeString (item.getKey());
    list.insert (0, item.getKey());

    while (list.size() > maxRecent)
        list.remove (list.size() - 1);

    settings.set (Settings::browserRecent, list);
}

std::vector<LibraryItem> Library::getRecent() const
{
    return getItems (settings.get (Settings::browserRecent));
}

std::vector<LibraryItem> Library::getItems (const juce::StringArray& keys) const
{
    std::vector<LibraryItem> items;

    for (const auto& key : keys)
        if (auto item = findItem (key))
            items.push_back (std::move (*item));

    return items;
}

std::optional<LibraryItem> Library::findItem (const juce::String& key) const
{
    if (key.startsWith (devicePrefix))
    {
        for (const auto kind : { LibraryItem::Kind::instrument, LibraryItem::Kind::effect })
            for (auto& item : listDevices (kind))
                if (item.getKey() == key)
                    return item;

        return std::nullopt;
    }

    if (! juce::File::isAbsolutePath (key))
        return std::nullopt;

    const juce::File file (key);

    if (! file.exists())
        return std::nullopt;

    return makeFileItem (file, findRoot (file));
}
} // namespace phazite
