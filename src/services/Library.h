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
#include <condition_variable>
#include <deque>
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
    std::optional<SoundKey> key {};      // a sound's or a MIDI file's, as its tags name it
    juce::int64 size = -1, modified = 0; // a file's, as the index or a listing read it: what tells a file changed in place

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

// The order a folder's rows go in: by name as people read them (Library::listFolder()), then by full path, so that two
// whose names compare the same ("Kick.wav" and "Kick.flac", "kick" and "Kick") always come in the same order.
bool isListedBefore (const LibraryItem&, const LibraryItem&);

// The index, as it was built last: the sounds, MIDI files and presets found, the folders they are in, and each one's search
// key, in the same order. Shared, never changed: the next one is another. full: the walk stopped at Library::maxIndexed files.
struct LibraryIndex
{
    std::vector<LibraryItem> files, folders;
    std::vector<SearchKey> fileKeys, folderKeys;
    std::map<juce::String, int> soundTags; // how many of the sounds and MIDI files have each tag ("Kick": 412)
    bool full = false;

    // The roots it walked to the end (none once it stopped at Library::maxIndexed), and each folder's own folders and files
    // there, by their places in folders and files, each by name, as Library::listFolder() lists them: what a folder of
    // theirs holds, found without the disk (listChildren(): none for a folder outside them).
    juce::Array<juce::File> wholeRoots;
    struct Children
    {
        std::vector<size_t> folders, files;
    };
    std::map<juce::String, Children> children;
    std::set<juce::String> loops; // folders the walk met inside themselves (a link to a folder it was in): nothing under them

    // When each folder was listed for it, by the walk or a patch, among every listing of the library's (Library::Read): a
    // listing that came before that tells nothing new of it. For a folder none listed, its nearest listed folder's.
    std::map<juce::String, std::int64_t> listedAt;
    std::int64_t getListedAt (const juce::File& folder) const;
    std::optional<std::vector<LibraryItem>> listChildren (const juce::File& folder) const;
    bool holdsWhole (const juce::File& folder) const; // within a whole root, and not within a loop
    void makeChildren();                              // once every file and folder is in, each once (roots may overlap)

    // A file or a folder by its full path, without the disk: none for one the index hasn't.
    std::optional<LibraryItem> find (const juce::String& path) const;
    std::map<juce::String, std::pair<bool, size_t>> byPath; // makeChildren()'s: a folder (true) or a file, and its place

    // How many files with an extension (".phazitepreset") a folder of a whole root holds, within its folders too; none if it
    // doesn't hold the folder whole.
    std::optional<int> countFilesWithin (const juce::File& folder, const juce::String& extension) const;

    // The same index with a folder's own files and folders as a listing read them now: those it held there and are gone,
    // with everything within those folders, taken away; the others put in or brought up to date, each with its search key
    // and its tags counted again. More than Library::maxIndexed, it holds no root whole any more. Whether a folder in the
    // listing is new (what it holds unknown: a walk is wanted).
    std::shared_ptr<const LibraryIndex> withListing (const juce::File& folder, const std::vector<LibraryItem>& listed,
                                                     const juce::File& root, bool& hasNewFolders) const;

    void addFile (LibraryItem, const juce::File& root);
    void addFolder (LibraryItem, const juce::File& root);
    void addFileWithKey (LibraryItem, SearchKey); // its search key made already
    void addFolderWithKey (LibraryItem, SearchKey);
    void countTags (const LibraryItem&); // a file's, into soundTags, as addFile() does
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

    // What a folder holds: its folders, then its sounds, MIDI files and presets, each by name. Read from the disk, on the
    // calling thread.
    std::vector<LibraryItem> listFolder (const juce::File&) const;

    // The same as the browser opens a folder, without the message thread touching the disk, and showing what the folder holds
    // now. Where the index holds the folder whole (a folder of a root it walked to the end), listed gets the index's rows at
    // once, before this returns; and the folder is read from the disk all the same, on a thread of the listings' own (not
    // the search's, which a drive that doesn't answer would hold up too): where that differs from the index (a file added,
    // removed, renamed or changed in place from outside Phazite), listed gets it again, on the message thread, and the
    // index is patched for that folder by its own rules (LibraryIndex::withListing()), so that search and the filters find
    // what is there; where it doesn't, nothing more comes. Elsewhere listed gets the disk's rows alone, once read, or none
    // for a folder that isn't there. The rows come from what is known of the files (the index, the loop data read before,
    // their names, and a preset's header, which is read for its rows: a file that isn't a preset Phazite reads is left out);
    // where the index is patched, the new sounds' headers are read after the rows are given, and listed gets the rows again
    // if what they read changes them (a loop, its tempo, its key). Nothing comes once what this returns has gone (or was
    // cancelled).
    //
    // The reads in flight are kept by folder: a listing of a folder already being read (or waiting to be) joins that read.
    // And a root (Phazite Sounds, a user's folder, a presets folder) has one read in flight at a time, the others of it
    // waiting for it until it has been inside the system for a few seconds (stuckMilliseconds: a link within the root to a
    // drive asleep), when the next goes on beside it: a drive that doesn't answer (a sleeping network drive) holds one of the
    // listings' threads, and no more, until it returns; the index's rows stay meanwhile; other roots' folders go on, on a
    // spare thread while every thread is held so (no more spare ones than roots, and one). A listing whose read has been
    // inside the system, or waited to start, for those few seconds says so (FolderListing::isLate()). As the library goes, what waits is
    // dropped, and a read stuck in the system is left behind, never waited for, doing nothing more once it returns.
    // Message thread.
    using Listed = std::function<void (std::optional<std::vector<LibraryItem>>)>;

    class FolderListing
    {
    public:
        ~FolderListing() { cancel(); }
        void cancel();
        bool isLate() const; // its read inside the system, or waiting to start, for a few seconds: the drive isn't answering

        struct State;
        explicit FolderListing (std::shared_ptr<State> s)
            : state (std::move (s))
        {
        }

    private:
        std::shared_ptr<State> state;
        JUCE_DECLARE_NON_COPYABLE (FolderListing)
    };

    std::unique_ptr<FolderListing> startListing (const juce::File& folder, Listed) const;

    // A folder open already looked at again: read from the disk on a listing thread alone (nothing from the index here),
    // compared on that thread with what the index holds of it; listed gets it, and the index is patched, only where it
    // differs (where the index doesn't hold the folder whole, listed gets every read). Joins a read in flight, as above.
    std::unique_ptr<FolderListing> checkListing (const juce::File& folder, Listed) const;

    // Whether a folder is being read for a listing, or waits to be (tests). Message thread.
    bool isReadingFolders() const;

    // How many times the library asked the disk about a file or a folder (whether it is there, is a folder, its listing, a
    // preset's header) on the message thread, since the program started (the speed gates). Any thread. And for what else
    // asks it there for the browser (BrowserModel: whether Phazite Sounds are installed).
    static juce::int64 getFileQueriesOnMessageThread() noexcept { return fileQueriesOnMessageThread.load(); }
    static void noteFileQuery() noexcept;

    // Tests: what a listing's thread does before it reads the disk (held until let go, as a drive that is slow to wake), and
    // whether the folders the index holds whole are listed from it (as Phazite does) or read from the disk as the others are.
    void setBeforeListing (std::function<void (const juce::File&)> before) const;
    void setListsFromIndex (bool fromIndex) const;

    // Tests: what a patch's thread does before it reads a new file's header, and what the index's walk does before it lists a
    // folder (both held until let go); and how many headers the listings read, since the program started (the gates).
    void setBeforeHeader (std::function<void (const juce::File&)> before) const;
    void setBeforeWalkListing (std::function<void (const juce::File&)> before) const;
    static juce::int64 getHeadersReadByListings() noexcept { return headersReadByListings.load(); }

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

    // Tests: what a search's thread does before it looks, and before a count (held until let go, so that its results come
    // later).
    void setBeforeSearch (std::function<void()> before) const { beforeSearch = std::move (before); }

    // How many items the searches looked through on the message thread, since the program started: none for the browser's
    // search as the user types (the performance gates check that). Any thread.
    static juce::int64 getItemsSearchedOnMessageThread() noexcept { return searchedOnMessageThread.load(); }

    // Each sound, MIDI file and preset of an index that search would find for the query, or every one for an empty query
    // (none for text without words, as search finds none), in the index's order: for counting what search finds (the browser's filters' counts). False, having stopped,
    // once shouldStop says so, which it asks between thousands of items. Any thread; on the message thread, what it looks
    // through counts with the searches' there (getItemsSearchedOnMessageThread()).
    static bool forEachFileMatch (const LibraryIndex&, const juce::String& query, const std::function<void (const LibraryItem&)>& matched,
                                  const std::function<bool()>& shouldStop = nullptr);

    // Work that goes through the index for the browser (the filters' counts, the places a filter leaves), run on the
    // search's thread as the browser's search is: given the index as it is now, and what says that it is to stop, it returns
    // whether it finished. Each lane's start() cancels what it started before (another lane's work, and a search, go on), so
    // that only its newest's done is called, on the message thread, once that work finished; cancel() cancels it with none,
    // as the lane's going does. Message thread; the library outlives its lanes.
    using Work = std::function<bool (const LibraryIndex&, const std::function<bool()>& shouldStop)>;

    class IndexWork
    {
    public:
        explicit IndexWork (const Library&);
        ~IndexWork();

        void start (Work, std::function<void()> done);
        void cancel();
        bool isPending() const;

    private:
        struct State;
        const Library& library;
        std::shared_ptr<State> state;

        JUCE_DECLARE_NON_COPYABLE (IndexWork)
    };

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
    using Facts = std::map<juce::String, SoundFacts>;
    std::shared_ptr<const Facts> facts = std::make_shared<const Facts>(); // shared, never changed: the next is another
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
    juce::File findRootByPath (const juce::File&) const; // the same by paths alone, without asking the disk

    // A folder read from the disk, as listFolder() lists it, in two steps: what it holds, by the system's call alone (none
    // for a folder that isn't there), then the items made of them, with the facts given. Any thread.
    struct Entry
    {
        juce::File file;
        bool isFolder = false;
        juce::int64 size = 0, modified = 0;
    };

    static std::optional<std::vector<Entry>> readEntries (const juce::File& folder);
    static std::vector<LibraryItem> makeItems (const std::vector<Entry>&, const juce::File& root, const juce::StringArray& audioExtensions,
                                               const Facts&);
    static std::optional<std::vector<LibraryItem>> readFolder (const juce::File& folder, const juce::File& root,
                                                               const juce::StringArray& audioExtensions, const Facts&);
    static LibraryItem makeItemOf (const juce::File&, const juce::File& root, const SoundFacts*, bool knownFile);
    std::shared_ptr<const Facts> getFacts() const;
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
    static inline std::atomic<juce::int64> searchedOnMessageThread { 0 };
    mutable BackgroundJobs searches { "Phazite browser search", 1, juce::Thread::Priority::normal };

    // The folders' listings (startListing()): what their jobs share with the library, which they may outlive (a read stuck in
    // the system as it goes): whether it is still there, the patches under way (which its going waits for: memory work
    // alone), the reads in flight or waiting by folder, the read holding each root's turn and those waiting for it, the
    // reads inside the system's call (and since when), the audio formats a patch reads new files' headers with (its own, so
    // that a thread left behind touches none of the program's), the order the listings and the walk listed folders in, and
    // the tests' hooks. Guarded by its lock.
    struct Waiter
    {
        std::weak_ptr<FolderListing::State> listing;
        std::optional<std::vector<LibraryItem>> shown; // what it compares the read with: the index's rows it gave
        bool againstIndex = false;                     // or the index as it is then (checkListing())
    };

    struct Read
    {
        juce::File folder, root;
        std::vector<Waiter> waiters;
        std::optional<double> inCallSince; // devices::Clock, while inside the system's call (its listing, a header)
        double createdAt = 0.0;            // devices::Clock: asked for
        bool started = false;              // taken by a thread
        std::int64_t sequence = 0;         // when it listed the folder, among the walk's and the other reads' listings
    };

    struct Listings
    {
        std::mutex lock;
        std::condition_variable idle; // working down to none
        Library* library = nullptr;
        int working = 0;
        std::map<juce::String, std::shared_ptr<Read>> reads;
        std::map<juce::String, std::deque<std::shared_ptr<Read>>> waiting;
        std::map<juce::String, std::shared_ptr<Read>> turns; // by root: the read whose turn it is
        std::set<std::shared_ptr<Read>> inFlight;            // their root's turn given: ready, or taken by a thread
        std::deque<std::shared_ptr<Read>> ready;             // to be taken by the next thread free (takeRead())
        int spareOffered = 0, spareRunning = 0;              // spare threads asked for, and reading
        std::function<void (const juce::File&)> before, beforeHeader, beforeWalk;
        bool fromIndex = true;
        std::atomic<int> delivering { 0 }; // rows read, on their way to the message thread
        std::atomic<std::int64_t> sequence { 0 };
        juce::AudioFormatManager formats;
    };

    const std::shared_ptr<Listings> listingsShared = std::make_shared<Listings>();
    mutable std::unique_ptr<BackgroundJobs> listings =
        std::make_unique<BackgroundJobs> ("Phazite folder listing", 4, juce::Thread::Priority::normal);
    mutable std::unique_ptr<BackgroundJobs> spareListings = // while every listing thread is stuck: one more, up to the roots + 1
        std::make_unique<BackgroundJobs> ("Phazite folder listing (spare)", 16, juce::Thread::Priority::normal);
    static inline std::atomic<juce::int64> fileQueriesOnMessageThread { 0 }, headersReadByListings { 0 };
    static void leaveBehind (std::unique_ptr<BackgroundJobs>); // never destroyed while a job of theirs still runs
    static constexpr double stuckMilliseconds = 3000.0;        // inside the system's call this long: its root's turn goes on

    std::unique_ptr<FolderListing> listFolderOf (const juce::File& folder, Listed, std::optional<std::vector<LibraryItem>> shown,
                                                 bool againstIndex) const;
    void queueRead (const std::shared_ptr<Read>&) const; // under listingsShared->lock
    void startRead (const std::shared_ptr<Read>&) const; // the same: on a listing thread, or a spare one while all are stuck
    void moveOnFromStuck() const;                        // the same: roots whose read is stuck give their turn to the next

    // While any read is out: whether one has been stuck long enough (its root's turn moved on, a spare thread offered, its
    // listings told they are late), looked at once a second on the message thread, so that a read waiting needs no other
    // listing to move on. Stopped once none is out.
    struct StuckCheck final : public juce::Timer
    {
        explicit StuckCheck (const Library& l)
            : library (l)
        {
        }
        void timerCallback() override;
        const Library& library;
    };

    mutable StuckCheck stuckCheck { *this };
    void offerSpare() const; // under listingsShared->lock: a spare thread for a read ready while every thread busy is stuck
    void markLate() const;   // the same: each listing told whether its read is late
    static void takeRead (const std::shared_ptr<Listings>&, bool spare); // a listing thread's job: the next read ready
    static void runRead (const std::shared_ptr<Listings>&, const std::shared_ptr<Read>&);
    static void endRead (Listings&, const std::shared_ptr<Read>&); // under its lock: its root's next read, if it held the turn

    // Patches of the index, one at a time (patchLock), each from the index as it is then, each only where it listed the
    // folder after the walk did (LibraryIndex::listedAt), so that neither puts back what the other found gone; kept by folder
    // until the walk under way has published its last, which takes in those newer than its own listings. And a walk wanted,
    // for a folder that came from outside (what it holds unknown).
    std::mutex patchLock;
    struct Patch
    {
        juce::File folder, root;
        std::vector<LibraryItem> listed;
        std::int64_t sequence = 0;
    };
    std::map<juce::String, Patch> patches; // guarded by patchLock
    std::atomic<bool> walkWanted { false };
    void patch (const Patch&, const Facts& fresh);
    std::shared_ptr<const LibraryIndex> withPatches (std::shared_ptr<const LibraryIndex>) const; // under patchLock

    void deliverSearch() const;
};
} // namespace phazite
