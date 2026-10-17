// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Library.h"
#include "model/engine/AppInfo.h"
#include "model/engine/EngineContext.h"

#include <algorithm>
#include <set>

namespace phazite
{
namespace
{
    constexpr const char* devicePrefix = "device:";

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
} // namespace

//==============================================================================
std::optional<double> getTempoFromName (const juce::String& name)
{
    // A number from 50 to 250 just before or after "bpm".
    const auto tokens = getTokens (name);

    for (int i = 0; i < tokens.size(); ++i)
    {
        if (tokens[i] != "bpm")
            continue;

        for (const auto neighbour : { i - 1, i + 1 })
        {
            if (! juce::isPositiveAndBelow (neighbour, tokens.size()) || ! tokens[neighbour].containsOnly ("0123456789"))
                continue;

            if (const auto tempo = tokens[neighbour].getIntValue(); tempo >= 50 && tempo <= 250)
                return (double) tempo;
        }
    }

    return std::nullopt;
}

juce::StringArray getSoundTags (const juce::File& file, const juce::File& root)
{
    // The words of the name, and of the folders' names between the root and the file.
    auto tokens = joinTagWords (getTokens (file.getFileNameWithoutExtension()));

    for (auto folder = file.getParentDirectory(); folder.isAChildOf (root); folder = folder.getParentDirectory())
        tokens.addArray (joinTagWords (getTokens (folder.getFileName())));

    juce::StringArray tags;

    for (const auto& tagWords : getTagWords())
    {
        // "808" alone is the 808's bass drum, played as a bass; "808 Clap" is a clap.
        const auto is808Bass = juce::String (tagWords.tag) == "Bass" && tags.isEmpty() && tokens.contains ("808");

        if (is808Bass || hasWord (tokens, tagWords.words))
            tags.add (tagWords.tag);
    }

    // A riser made for 4 bars at 128 BPM is still a one-shot.
    const auto tempo = getTempoFromName (file.getFileNameWithoutExtension());
    const auto isLoop = hasWord (tokens, { "loop", "break", "breakbeat", "groove" }) || (tempo.has_value() && ! tags.contains ("FX"));
    tags.add (isLoop ? "Loop" : "One-shot");

    if (tempo.has_value())
        tags.add (juce::String (juce::roundToInt (*tempo)) + " BPM");

    return tags;
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

    void run() override
    {
        std::vector<LibraryItem> found;

        for (const auto& root : roots)
        {
            for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
            {
                if (threadShouldExit() || (int) found.size() >= maxIndexed)
                    break;

                if (const auto& file = entry.getFile(); owner.isSoundFile (file) || owner.isMidiFile (file) || isPreset (file))
                    found.push_back (owner.makeFileItem (file, root));
            }
        }

        if (threadShouldExit())
            return;

        {
            const juce::ScopedLock lock (owner.indexLock);
            owner.index = std::move (found);
        }

        owner.triggerAsyncUpdate();
    }

    Library& owner;
    const juce::Array<juce::File> roots;
};

//==============================================================================
Library::Library (EngineContext& e, Settings& s, juce::File factory, juce::File presets)
    : engine (e),
      settings (s),
      factorySounds (std::move (factory)),
      presetsFolder (std::move (presets)),
      audioExtensions (engine.getAudioFileExtensions()) // which the index can check on its own thread
{
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
    LibraryItem item;
    item.file = file;

    if (file.isDirectory())
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
    item.tags = getSoundTags (file, root);
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

std::vector<LibraryItem> Library::search (const juce::String& query, int maximum) const
{
    const Query wanted (query);

    if (wanted.words.isEmpty())
        return {};

    std::vector<std::pair<int, LibraryItem>> matches;

    const auto consider = [&matches, &wanted] (const LibraryItem& item)
    {
        if (const auto score = wanted.getScore (item); score >= 0)
            matches.emplace_back (score, item);
    };

    {
        const juce::ScopedLock lock (indexLock);

        for (const auto& item : index)
            consider (item);
    }

    for (const auto kind : { LibraryItem::Kind::instrument, LibraryItem::Kind::effect })
        for (const auto& item : listDevices (kind))
            consider (item);

    return getBest (std::move (matches), maximum);
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

bool Library::isIndexFull() const
{
    const juce::ScopedLock lock (indexLock);
    return (int) index.size() >= maxIndexed;
}

std::vector<LibraryItem> Library::listByTag (const juce::String& tag, int maximum) const
{
    std::vector<LibraryItem> items;

    {
        const juce::ScopedLock lock (indexLock);

        for (const auto& item : index)
            if (item.kind != LibraryItem::Kind::preset && item.tags.contains (tag)) // a synth's preset is no synth sound
                items.push_back (item);
    }

    std::sort (items.begin(), items.end(), [] (const LibraryItem& a, const LibraryItem& b) { return a.name.compareNatural (b.name) < 0; });

    if ((int) items.size() > maximum)
        items.resize ((size_t) std::max (0, maximum));

    return items;
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
    if (onIndexChanged != nullptr)
        onIndexChanged();
}

bool Library::isFavorite (const LibraryItem& item) const
{
    return settings.get (Settings::browserFavorites).contains (item.getKey());
}

void Library::setFavorite (const LibraryItem& item, bool favorite)
{
    auto list = settings.get (Settings::browserFavorites);
    list.removeString (item.getKey());

    if (favorite)
        list.add (item.getKey());

    settings.set (Settings::browserFavorites, list);
}

std::vector<LibraryItem> Library::getFavorites() const
{
    return getItems (settings.get (Settings::browserFavorites));
}

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
