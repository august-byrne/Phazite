// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows (docs/design.md, section 11; roadmap step 1.5): sounds (audio and MIDI files) in the library
// that comes with Phazite and in the user's own folders, and devices (instruments and effects, built in and plug-ins).
// They are found by place or by search, with tags read from their names, folders and loop data, and with favorites,
// collections and recent items, which Phazite's settings keep.
#pragma once

#include "Settings.h"
#include "model/base/BackgroundJobs.h"
#include "model/engine/AudioFiles.h"
#include "model/mixer/DeviceType.h"
#include "model/presets/PresetModel.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace phazite
{
class EngineContext;

//==============================================================================
// What the library's index read of a sound file, besides its name: what its loop data say (model/engine/AudioFiles.h), as
// the file was then (its size, and when it was last changed, in milliseconds).
struct SoundFacts
{
    juce::int64 size = 0, modified = 0;
    AudioFileInfo::LoopData loopData = AudioFileInfo::LoopData::none;
    double loopBeats = 0.0, seconds = 0.0;
    std::optional<SoundKey> key {};
};

// A sound's tags, from the words in its name and in the names of its folders below the root: what it is (Kick, Snare,
// Clap, Hat, Cymbal, Tom, Percussion, Bass, FX, Foley, Vocal, Synth), Loop or One-shot, its tempo ("120 BPM") and its
// key ("F# minor", getSoundKey()). A sound is a loop if its name or folders say so, or if its name has a tempo and it isn't
// an effect. With what its file's loop data say, those make it a loop or a one-shot, and give it a tempo and a key where its
// name doesn't.
juce::StringArray getSoundTags (const juce::File&, const juce::File& root, const SoundFacts* = nullptr);

// A sound's key: from its name (getKeyFromName()), else from the nearest of its folders below the root whose name has a
// key with minor or major ("Loops/F# Minor"), else from its loop data.
std::optional<SoundKey> getSoundKey (const juce::File&, const juce::File& root, const SoundFacts* = nullptr);

// The types the words of a name give, as getSoundTags() finds them: "808 Kick" is a Kick, "Closed Hat" a Hat.
juce::StringArray getSoundTypesIn (const juce::String& name);

//==============================================================================
struct LibraryItem
{
    enum class Kind
    {
        folder,
        sound, // audio
        midi,
        instrument,
        effect,
        preset // a device's, a container's or a track's (model/presets/PresetModel.h)
    };

    Kind kind = Kind::sound;
    juce::String name {};                // a file's name without its extension, or a device's name
    juce::File file {};                  // folders, sounds, MIDI files and presets
    std::optional<DeviceType> device {}; // instruments and effects
    std::optional<PresetInfo> preset {}; // presets
    juce::StringArray tags {};
    std::optional<SoundKey> key {}; // a sound's or a MIDI file's, as its tags name it

    bool isFile() const noexcept { return kind == Kind::folder || kind == Kind::sound || kind == Kind::midi || kind == Kind::preset; }

    // What favorites, collections and recent items keep: a file's full path, or a device's identifier.
    juce::String getKey() const;
};

//==============================================================================
// What search looks through for an item, made once, as the index is built, rather than at each key typed (known bug 128):
// in lowercase, its name, the words of its name (each after a space, for a word of the name that starts with the query's
// first), its name with its tags, and the names of its folders below the library's root, which it is in.
struct SearchKey
{
    std::string name, words, text, folders;
};

SearchKey makeSearchKey (const LibraryItem&, const juce::File& root);

// The index, as it was built last: the sounds, MIDI files and presets found, the folders they are in, and each one's search
// key, in the same order. Shared, never changed: the next one is another. full: the walk stopped at Library::maxIndexed files.
struct LibraryIndex
{
    std::vector<LibraryItem> files, folders;
    std::vector<SearchKey> fileKeys, folderKeys;
    bool full = false;

    void addFile (LibraryItem, const juce::File& root);
    void addFolder (LibraryItem, const juce::File& root);
};

// What a search found: the best, as many as it was asked for, and how many match in all; stopped, if it was cancelled
// before it was done (and found nothing).
struct SearchResults
{
    std::vector<LibraryItem> items;
    int total = 0;
    bool stopped = false;
};

//==============================================================================
class Library : private juce::AsyncUpdater
{
public:
    // The user's folders, favorites, collections and recent items are kept in the settings. The factory library is Phazite
    // Sounds' folder (SoundPack::getFolder()), whether it is installed yet or not; tests give none. The user's presets go in
    // the presets folder, in a folder for each device ("EQ"), containers and tracks; tests give one of their own, or none.
    // Phazite's own presets are in the factory presets' folder, a folder for each device with one for each kind of sound
    // in it ("Synth/Bass"), which nothing writes to. What the index reads of the sounds' files is kept in the facts file
    // from one start to the next, so that it reads only what is new or changed (none in tests: it reads all of it each
    // time).
    Library (EngineContext&, Settings&, juce::File factorySounds = {}, juce::File presets = {}, juce::File factoryPresets = {},
             juce::File factsFile = {});
    ~Library() override;

    // Documents/Phazite/Presets.
    static juce::File getDefaultPresetsFolder();
    juce::File getPresetsFolder() const { return presetsFolder; }

    // The presets that come with Phazite, next to the app ("presets", which the build copies there from
    // resources/presets).
    static juce::File getDefaultFactoryPresetsFolder();
    juce::File getFactoryPresetsFolder() const { return factoryPresetsFolder; }

    // Indexes the presets again, after one was saved, and tells onIndexChanged.
    void notePresetSaved() { reindex(); }

    // The folders the browser shows besides the factory library, in the order added.
    juce::Array<juce::File> getUserFolders() const;
    void addUserFolder (const juce::File&);
    void removeUserFolder (const juce::File&);

    // What a folder holds: its folders, then its sounds, MIDI files and presets, each by name.
    std::vector<LibraryItem> listFolder (const juce::File&) const;

    // The devices that can be added: instruments or effects.
    std::vector<LibraryItem> listDevices (LibraryItem::Kind) const;

    // Sounds, presets and folders (in the factory library, the user's folders and presets, and their folders below) and
    // devices that have every word of the query, in any order, in their names, their tags or the names of the folders
    // they are in, best first: names that start with the query, then names with a word that starts with its first word,
    // then names and tags that hold every word, then those that hold some only in their folders' names; then shorter names
    // first, then by name, as Ableton Live's browser and FL Studio's rank them; and the folders after everything else, as a
    // way to what they hold. Only those that accepts takes, when it is
    // given, are counted. At once, on the calling thread: the browser's search runs in the background (startSearch()).
    using Accepts = std::function<bool (const LibraryItem&)>;
    std::vector<LibraryItem> search (const juce::String& query, int maximum = 200, const Accepts& accepts = nullptr) const;

    // The same through an index and devices given, with how many match in all; stopped (with nothing) once shouldStop says
    // so, which it asks between thousands of items. Any thread.
    static SearchResults searchIndex (const LibraryIndex&, const std::vector<LibraryItem>& devices, const juce::String& query, int maximum,
                                      const Accepts& accepts, const std::function<bool()>& shouldStop = nullptr);

    // The browser's search, as the user types (known bug 128): through the index as it is now, on a thread of its own, so
    // that a key typed never waits for it. Each search started cancels the one before, so that only the newest text's
    // results arrive, in found, on the message thread; cancelSearch() cancels it with none. Message thread.
    void startSearch (const juce::String& query, int maximum, Accepts accepts, std::function<void (SearchResults)> found) const;
    void cancelSearch() const;

    // Tests: what a search's thread does before it looks (held until let go, so that its results come later).
    void setBeforeSearch (std::function<void()> before) const { beforeSearch = std::move (before); }

    // How many items the searches looked through on the message thread, since the program started: none for the browser's
    // search as the user types (the performance gates check that). Any thread.
    static juce::int64 getItemsSearchedOnMessageThread() noexcept { return searchedOnMessageThread.load(); }

    // Each item search would find for the query, every one, in no order: for counting what it finds.
    void forEachMatch (const juce::String& query, const std::function<void (const LibraryItem&)>&) const;

    // The instruments and the effects, as listDevices() gives them.
    std::vector<LibraryItem> getAllDevices() const;

    // Instruments and effects whose names, makers or kinds have every word of the query, best first; for an empty
    // query, all of them: instruments, then effects, each with the built-in ones first. The plug-in picker shows them.
    std::vector<LibraryItem> searchDevices (const juce::String& query) const;

    // Search finds the sounds in an index of the factory library, the user's folders and the presets, which a background
    // thread builds when the library is made and when the folders change: first from the files' names, then again with
    // what their loop data say, read from the WAV and AIFF files it hasn't read before. onIndexChanged is called on the
    // message thread each time it has been built.
    bool isIndexing() const;
    std::function<void()> onIndexChanged;

    // The index as it was built last: shared, never changed (the next one is another); and its files alone.
    std::shared_ptr<const LibraryIndex> getWholeIndex() const;
    std::shared_ptr<const std::vector<LibraryItem>> getIndex() const;

    // Indexes the sounds again, as after Phazite Sounds has been installed.
    void refresh() { reindex(); }

    // Whether the index stopped at maxIndexed sounds, so that search misses some.
    bool isIndexFull() const;

    static juce::StringArray getSoundTypes(); // the types getSoundTags() gives, then "Loop"

    //==============================================================================
    // Favorites and collections (docs/design.md, section 11): sets of sounds, presets, folders and devices, kept by file: a
    // file renamed or moved stays in them, found again by its size and what its start holds, on the index's thread as the
    // index is built, once for each index. A listing that meets a member that went since and that the index didn't look for
    // builds the index again, which tells onIndexChanged once it has found it. Favorites are the set of the star.
    bool isFavorite (const LibraryItem&) const;
    void setFavorite (const LibraryItem&, bool);
    std::vector<LibraryItem> getFavorites() const; // those still there

    // The collections, by name, in the order made. A new one's name is made unique ("Kicks 2"); renaming one to a name
    // that is empty or taken does nothing.
    juce::StringArray getCollections() const;
    juce::String addCollection (const juce::String& name); // its name, or nothing for an empty one
    bool renameCollection (const juce::String& name, const juce::String& newName);
    void removeCollection (const juce::String& name);
    bool isInCollection (const LibraryItem&, const juce::String& collection) const;
    void setInCollection (const LibraryItem&, const juce::String& collection, bool);
    std::vector<LibraryItem> getCollection (const juce::String& name) const; // those still there, in the order added

    // Items put into the song, newest first.
    void noteUsed (const LibraryItem&);
    std::vector<LibraryItem> getRecent() const; // those still there

    // The item a key names, if it is still there.
    std::optional<LibraryItem> findItem (const juce::String& key) const;

    // A folder, sound, MIDI file or preset, with what the index read of a sound's file.
    LibraryItem makeFileItem (const juce::File&, const juce::File& root) const;

    juce::File getFactorySounds() const { return factorySounds; } // the library that comes with Phazite

    static constexpr int maxRecent = 50;
    static constexpr int maxIndexed = 50000; // sounds; a larger library is searched as far as this

private:
    struct Indexer;

    // A member of a set (favorites or a collection): its key (a file's full path, where it was found last, or a device's
    // identifier) and, for a sound or a preset, its size and the SHA-256 of its first 64 KiB, by which it is found again
    // where it moved.
    struct Member
    {
        juce::String key;
        juce::int64 size = -1;
        juce::String start {};
    };

    struct Collection
    {
        juce::String name;
        std::vector<Member> members {};
    };

    EngineContext& engine;
    Settings& settings;
    const juce::File factorySounds, presetsFolder, factoryPresetsFolder, factsFile;
    juce::StringArray audioExtensions; // of the formats the engine reads, as ".wav"
    std::unique_ptr<Indexer> indexer;

    // The index, what it read of the sounds' files by path, and the members of the sets that went and where those found
    // again went (their keys before and after), guarded by indexLock.
    mutable juce::CriticalSection indexLock;
    std::shared_ptr<const LibraryIndex> index;
    std::map<juce::String, SoundFacts> facts;
    std::set<juce::String> lookedFor;
    std::map<juce::String, juce::String> movedTo;

    // Favorites and collections, as the settings keep them (message thread), and the keys of the members the last index
    // looked for, which a listing doesn't ask it to look for again.
    mutable std::vector<Member> favorites;
    mutable std::vector<Collection> collections;
    mutable std::set<juce::String> favoriteKeys; // for the star on every row
    mutable std::set<juce::String> searchedFor;

    bool isSoundFile (const juce::File&) const;
    static bool isPreset (const juce::File&); // a preset file Phazite can read
    bool isMidiFile (const juce::File&) const;
    juce::File findRoot (const juce::File&) const;
    void reindex(); // the index built again on its thread, the one before stopped
    void handleAsyncUpdate() override;

    // A file's item with what the index read of it, if it read anything; knownFile skips asking whether it is a folder.
    LibraryItem makeFileItem (const juce::File&, const juce::File& root, const SoundFacts*, bool knownFile) const;

    std::vector<LibraryItem> getItems (const juce::StringArray& keys) const;
    juce::Array<juce::File> getRoots() const; // the factory library, the user's folders, and the presets

    Member makeMember (const LibraryItem&) const;
    std::vector<LibraryItem> getItems (const std::vector<Member>&) const; // those still there
    static bool isStillThere (const Member&);                             // where it was found last
    std::vector<Member> getAllMembers() const;                            // of the favorites and the collections
    void takeMovedMembers();                                              // what the index found
    void readSets();
    void writeFavorites() const;
    void writeCollections() const;
    Collection* findCollection (const juce::String& name) const;

    // The browser's search (startSearch()): the number of the newest, which a search that isn't it stops at, and what it
    // calls back with its results (message thread); what a search's thread found, which the message thread takes; the
    // search's thread.
    struct SearchDelivery final : juce::AsyncUpdater
    {
        explicit SearchDelivery (const Library& l)
            : library (l)
        {
        }

        void handleAsyncUpdate() override { library.deliverSearch(); }

        const Library& library;
    };

    mutable std::atomic<int> searchNumber { 0 };
    mutable std::function<void (SearchResults)> searchFound;
    mutable std::function<void()> beforeSearch;
    mutable std::mutex deliveredLock;
    mutable std::optional<std::pair<int, SearchResults>> delivered;
    mutable SearchDelivery searchDelivery { *this };
    mutable BackgroundJobs searches { "Phazite browser search", 1, juce::Thread::Priority::normal };
    static inline std::atomic<juce::int64> searchedOnMessageThread { 0 };

    void deliverSearch() const;
};
} // namespace phazite
