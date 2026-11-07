// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Library.h"
#include "model/engine/AppInfo.h"
#include "model/engine/EngineContext.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <set>

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

    // A search, and how well an item matches it: 0 if the item's name starts with the query, 1 if a word of its name
    // starts with the query's first word, 2 if its name and tags merely hold every word; -1 if they don't.
    struct Query
    {
        explicit Query (const juce::String& text)
            : words (getQueryWords (text)),
              phrase (words.joinIntoString (" "))
        {
        }

        int getScore (const LibraryItem& item) const
        {
            const auto name = item.name.toLowerCase();
            const auto haystack = name + " " + item.tags.joinIntoString (" ").toLowerCase();

            for (const auto& word : words)
                if (! haystack.contains (word))
                    return -1;

            if (name.startsWith (phrase))
                return 0;

            const auto tokens = getTokens (item.name);
            return std::any_of (tokens.begin(), tokens.end(), [this] (const juce::String& t) { return t.startsWith (words[0]); }) ? 1 : 2;
        }

        const juce::StringArray words;
        const juce::String phrase;
    };

    // The matches, best first: by score, then shorter names, then by name.
    std::vector<LibraryItem> getBest (std::vector<std::pair<int, LibraryItem>> matches, int maximum)
    {
        std::stable_sort (matches.begin(), matches.end(),
                          [] (const auto& a, const auto& b)
                          {
                              if (a.first != b.first)
                                  return a.first < b.first;

                              if (a.second.name.length() != b.second.name.length())
                                  return a.second.name.length() < b.second.name.length();

                              return a.second.name.compareNatural (b.second.name) < 0;
                          });

        std::vector<LibraryItem> results;

        for (size_t i = 0; i < matches.size() && (int) i < maximum; ++i)
            results.push_back (std::move (matches[i].second));

        return results;
    }

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
            facts.loopBeats = fields[3].getDoubleValue();
            facts.seconds = fields[4].getDoubleValue();

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

        file.getParentDirectory().createDirectory();
        file.replaceWithText (text.toString());
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
    Indexer (Library& o, juce::Array<juce::File> rootsToScan)
        : juce::Thread ("Phazite library index"),
          owner (o),
          roots (std::move (rootsToScan))
    {
    }

    ~Indexer() override { stopThread (4000); }

    // A file found, with its root, its size and when it was last changed (in milliseconds).
    struct Found
    {
        juce::File file, root;
        juce::int64 size = 0, modified = 0;
    };

    void run() override
    {
        std::vector<Found> files;

        for (const auto& root : roots)
        {
            for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if (threadShouldExit() || (int) files.size() >= maxIndexed)
                    break;

                if (const auto& file = entry.getFile(); owner.isSoundFile (file) || owner.isMidiFile (file) || isPreset (file))
                    files.push_back ({ file, root, entry.getFileSize(), entry.getModificationTime().toMilliseconds() });
            }
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

        publish (items, read, files);

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
            publish (items, read, files);

        // Kept for the next start, when anything changed.
        if (owner.factsFile != juce::File() && (! unread.empty() || read.size() != known.size()))
            writeFactsFile (owner.factsFile, read);
    }

    void publish (const std::vector<LibraryItem>& items, const std::map<juce::String, SoundFacts>& read, const std::vector<Found>& files)
    {
        auto built = std::make_shared<const std::vector<LibraryItem>> (items);
        std::multimap<juce::int64, juce::File> bySize;

        for (const auto& found : files)
            bySize.emplace (found.size, found.file);

        {
            const juce::ScopedLock lock (owner.indexLock);
            owner.index = std::move (built);
            owner.facts = read;
            owner.filesBySize = std::move (bySize);
        }

        owner.triggerAsyncUpdate();
    }

    Library& owner;
    const juce::Array<juce::File> roots;
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
      index (std::make_shared<const std::vector<LibraryItem>>())
{
    readSets();
    reindex();
}

Library::~Library()
{
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

std::vector<LibraryItem> Library::search (const juce::String& query, int maximum,
                                          const std::function<bool (const LibraryItem&)>& accepts) const
{
    const Query wanted (query);

    if (wanted.words.isEmpty())
        return {};

    std::vector<std::pair<int, LibraryItem>> matches;

    const auto consider = [&matches, &wanted, &accepts] (const LibraryItem& item)
    {
        if (accepts != nullptr && ! accepts (item))
            return;

        if (const auto score = wanted.getScore (item); score >= 0)
            matches.emplace_back (score, item);
    };

    const auto indexed = getIndex(); // held while it is gone through: the index thread may replace it meanwhile

    for (const auto& item : *indexed)
        consider (item);

    for (const auto kind : { LibraryItem::Kind::instrument, LibraryItem::Kind::effect })
        for (const auto& item : listDevices (kind))
            consider (item);

    return getBest (std::move (matches), maximum);
}

void Library::forEachMatch (const juce::String& query, const std::function<void (const LibraryItem&)>& matched) const
{
    const Query wanted (query);

    if (wanted.words.isEmpty())
        return;

    const auto indexed = getIndex(); // held while it is gone through: the index thread may replace it meanwhile

    for (const auto& item : *indexed)
        if (wanted.getScore (item) >= 0)
            matched (item);

    for (const auto kind : { LibraryItem::Kind::instrument, LibraryItem::Kind::effect })
        for (const auto& item : listDevices (kind))
            if (wanted.getScore (item) >= 0)
                matched (item);
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

    const Query wanted (query);

    if (wanted.words.isEmpty())
        return devices;

    std::vector<std::pair<int, LibraryItem>> matches;

    for (auto& device : devices)
        if (const auto score = wanted.getScore (device); score >= 0)
            matches.emplace_back (score, std::move (device));

    const auto count = (int) matches.size();
    return getBest (std::move (matches), count);
}

bool Library::isIndexing() const
{
    return indexer != nullptr && indexer->isThreadRunning();
}

std::shared_ptr<const std::vector<LibraryItem>> Library::getIndex() const
{
    const juce::ScopedLock lock (indexLock);
    return index;
}

bool Library::isIndexFull() const
{
    return (int) getIndex()->size() >= maxIndexed;
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
    indexer = std::make_unique<Indexer> (*this, getRoots());
    indexer->startThread (juce::Thread::Priority::low);
}

void Library::handleAsyncUpdate()
{
    // Members of the sets renamed or moved since, found again, before the browser lists them.
    findMovedMembers();

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

    const auto parsed = juce::JSON::parse (settings.get (Settings::browserCollections)); // which the list points into

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

bool Library::isStillThere (const Member& member) const
{
    if (member.key.startsWith (devicePrefix) || ! juce::File::isAbsolutePath (member.key))
        return true; // a device, found by findItem()

    const juce::File file (member.key);
    return member.size < 0 ? file.exists() : file.existsAsFile() && file.getSize() == member.size;
}

bool Library::findAgain (Member& member) const
{
    if (member.size < 0 || member.start.isEmpty())
        return false;

    // The files of its size in the folder it was in, renamed there since the index was built perhaps; then the index's,
    // anywhere in the library. Among them, the one that starts as it does.
    std::vector<juce::File> candidates;

    for (const auto& entry :
         juce::RangedDirectoryIterator (juce::File (member.key).getParentDirectory(), false, "*", juce::File::findFiles))
        if (entry.getFileSize() == member.size && (int) candidates.size() < mostCandidates)
            candidates.push_back (entry.getFile());

    {
        const juce::ScopedLock lock (indexLock);
        const auto [from, to] = filesBySize.equal_range (member.size);

        for (auto i = from; i != to && (int) candidates.size() < 2 * mostCandidates; ++i)
            candidates.push_back (i->second);
    }

    for (const auto& file : candidates)
    {
        if (file.getFullPathName() != member.key && file.existsAsFile() && readStart (file) == member.start)
        {
            member.key = file.getFullPathName();
            return true;
        }
    }

    return false;
}

std::vector<LibraryItem> Library::getItems (std::vector<Member>& members, bool& moved) const
{
    std::vector<LibraryItem> items;

    for (auto& member : members)
    {
        if (! isStillThere (member))
            moved = findAgain (member) || moved;

        if (auto item = findItem (member.key))
            items.push_back (std::move (*item));
    }

    return items;
}

void Library::findMovedMembers()
{
    const auto findAll = [this] (std::vector<Member>& members)
    {
        auto moved = false;

        for (auto& member : members)
            if (! isStillThere (member))
                moved = findAgain (member) || moved;

        return moved;
    };

    if (findAll (favorites))
        writeFavorites();

    auto collectionsMoved = false;

    for (auto& collection : collections)
        collectionsMoved = findAll (collection.members) || collectionsMoved;

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
    auto moved = false;
    auto items = getItems (favorites, moved);

    if (moved)
        writeFavorites();

    return items;
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

    auto moved = false;
    auto items = getItems (collection->members, moved);

    if (moved)
        writeCollections();

    return items;
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
