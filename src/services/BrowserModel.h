// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows (docs/design.md, section 11), without its window (roadmap phase A2): the places at the top of
// its tree and what each holds, the presets under each device, what search finds, the filters over them and what each
// offers, the row that downloads Phazite Sounds, and what an item's row says and does. ui::Browser shows it as a tree.
#pragma once

#include "Library.h"
#include "SoundPack.h"
#include "model/engine/Preview.h"

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace phazite
{
// A place in the browser, which lists sounds or devices: at the top of its tree, or a collection under "Collections".
struct BrowserPlace
{
    // In this order: their numbers are in the keys of the places a user left open (Settings::browserOpenness).
    enum class Kind
    {
        favorites,
        recent,
        sounds, // Phazite Sounds, once downloaded: the folders of the pack
        instruments,
        effects,
        trackPresets, // the user's presets of tracks, which belong to no device
        folder,       // one of the user's
        addFolder,
        collections, // the user's collections, each a place of its own (its name), then the row that makes one
        collection,
        newCollection,
        containers,           // under Effects, while it has any: the presets of containers of effects
        instrumentContainers, // under Instruments, the same of instrument containers
        otherPresets // after Track Presets, while it holds any: the user's presets of no device listed (BrowserModel::isOtherPreset())
    };

    Kind kind = Kind::favorites;
    juce::String name; // as its row shows it; for a collection, its name
    juce::File folder; // Phazite Sounds', the user's folder, or the user's presets' folder that holds its presets (Other
                       // Presets': the user's presets' folder itself)
    int count = -1;    // how many of its sounds have the filters' values; else -1

    juce::String getTooltip() const;
    juce::String getKey() const; // the same for the same place each time the browser lists it
    bool holdsItems() const { return kind != Kind::addFolder && kind != Kind::newCollection; }
};

// What a place or a folder holds, in the order its rows show it.
struct BrowserContents
{
    bool offersSoundPack = false;     // first, while Phazite Sounds aren't installed: the row that downloads them
    std::vector<BrowserPlace> places; // the collections, under Collections; a container's presets, under Instruments or Effects
    std::vector<LibraryItem> items;
    juce::StringArray messages; // last: lines of text, such as what a place is for while it holds nothing

    // Under Instruments and Effects: how many presets each device's row holds, by the device's key (LibraryItem::getKey()),
    // for those that hold any; -1 for each while the index doesn't know (in a library larger than it holds), when the row
    // may hold some.
    std::map<juce::String, int> presets;
    int getPresets (const LibraryItem& item) const;
};

// A list of presets the browser shows under a row (docs/design/browser.md), as Library::getPresetFolders() gives the folder
// of its name: the user's presets and Phazite's in one list. A device's, under its row in Instruments or Effects, are in the
// folder of its name ("EQ"); the containers', in Containers, show under Instrument Container (those of instrument
// containers at its top) or Container (the others at its top: a folder in it shows under both); the tracks', in Tracks,
// under Track Presets.
struct PresetShelf
{
    juce::String folderName;                     // PresetModel.h: getPresetFolderName(), or trackPresetsFolder
    juce::String emptyText;                      // what it says while it holds none
    std::optional<bool> instrumentContainers {}; // the containers': of instruments, or of effects

    bool accepts (const LibraryItem& atItsTop) const; // what shows at its top: of the containers', each at one of the two
    static PresetShelf ofDevice (const juce::String& folderName, const juce::String& deviceName);
};

//==============================================================================
// The browser's filters (docs/design.md, section 11), over what it shows: what a sound is, whether it is a loop or a
// one-shot, its tempo and its key. Each one set narrows what shows to the sounds and MIDI files that have its value; with
// any set, each place lists those of its own as one list, its folders' and theirs within included, and the places without
// any don't show.
struct BrowserFilters
{
    juce::String type {};         // a sound's type, as Library::getSoundTypes() names them ("Kick"), but "Loop"
    std::optional<bool> loops {}; // loops, or one-shots
    std::optional<int> tempo {};  // in beats per minute, as a sound's tags give it
    std::optional<SoundKey> key {};

    bool isEmpty() const noexcept { return type.isEmpty() && ! loops.has_value() && ! tempo.has_value() && ! key.has_value(); }

    // A sound or a MIDI file with every value set; anything else only while none is.
    bool accepts (const LibraryItem&) const;

    bool operator== (const BrowserFilters&) const = default;
};

// What the filters offer: the values of what the browser shows, as the other filters and the search words leave it, each
// with how many have it, in the order its menu shows them: types in Library::getSoundTypes()' order, tempos from the
// slowest, keys from C, each root alone, major, then minor.
struct FilterOptions
{
    std::vector<std::pair<juce::String, int>> types {};
    int loops = 0, oneShots = 0;
    std::vector<std::pair<int, int>> tempos {};
    std::vector<std::pair<SoundKey, int>> keys {};
};

// A sound's tempo, from its tags ("120 BPM"); none for one whose tags give none.
std::optional<int> getTempo (const LibraryItem&);

//==============================================================================
class BrowserModel
{
public:
    BrowserModel (const Library&, const SoundPack&);

    // At the top of the tree: Favorites, Recent, Collections, Sounds (Phazite Sounds, or the row that downloads them),
    // Instruments, Effects, Track Presets, Other Presets (while it holds any), each of the user's folders, and Add Folder.
    // With filters set, those that hold sounds with their values, each with how many it holds (the Type filter lists a
    // type's sounds from every library, as Ableton Live's labels and Bitwig's "Drum Hits" do). The presets that come with
    // Phazite and the user's show where they belong (PresetShelf): a device's under the device's row, which opens to them, a
    // container's under Instruments or Effects, a track's under Track Presets, and the user's that are of no device listed
    // under Other Presets.
    std::vector<BrowserPlace> getPlaces (const BrowserFilters& = {}) const;
    BrowserContents getContents (const BrowserPlace&, const BrowserFilters& = {}) const;
    BrowserContents getContents (const LibraryItem& folderOrDevice) const;
    BrowserContents getContents (const PresetShelf&) const;

    // The same as the browser opens a place (without filters), a folder or a device's row, without the message thread
    // touching the disk: what comes from no folder (favorites, recent items, collections, devices) and what the index holds
    // whole, given to opened at once, returning nothing; else read on a listing thread (Library::startListing()), given to
    // opened on the message thread, unless what this returns has gone first (or was cancelled). Phazite Sounds, the user's
    // folders and the presets' folders are the index's roots: their folders come from the index once it has walked them
    // whole (not while it is first built, nor in a library larger than Library::maxIndexed), else from the disk. A list of
    // presets, and a folder in either presets' folder, is what the user's and Phazite's hold together, as one.
    std::unique_ptr<Library::FolderListing> open (const BrowserPlace&, std::function<void (BrowserContents)> opened) const;
    std::unique_ptr<Library::FolderListing> open (const LibraryItem& folderOrDevice, std::function<void (BrowserContents)> opened) const;
    std::unique_ptr<Library::FolderListing> open (const PresetShelf&, std::function<void (BrowserContents)> opened) const;

    // A place, a folder or a device's row shown open, looked at again (Library::checkListing()): read on a listing thread
    // alone, opened called only where it differs from the index (every read for a folder the index doesn't hold whole).
    // Nothing for a place that lists no folder.
    std::unique_ptr<Library::FolderListing> check (const BrowserPlace&, std::function<void (BrowserContents)> opened) const;
    std::unique_ptr<Library::FolderListing> check (const LibraryItem& folderOrDevice, std::function<void (BrowserContents)> opened) const;
    std::unique_ptr<Library::FolderListing> check (const PresetShelf&, std::function<void (BrowserContents)> opened) const;

    // The presets a place, a device's row or a list holds: a device's row's (none for anything else but a device), and those
    // of Track Presets and the containers' rows.
    static std::optional<PresetShelf> getShelf (const LibraryItem&);
    std::optional<PresetShelf> getShelf (const BrowserPlace&) const;

    // How many presets a list holds, from the index, none of the disk: nothing while the index doesn't know (while it is
    // first made, or where it doesn't hold a presets' folder that was there whole, as in a library larger than it holds). And
    // the same through an index given, for its folders (Library::getPresetFolders()): one in no root the index walked wasn't
    // there, and holds none.
    std::optional<int> countPresets (const PresetShelf&) const;
    static std::optional<int> countPresets (const LibraryIndex&, const juce::Array<juce::File>& folders, const PresetShelf&,
                                            const std::function<bool (const juce::File&)>& listedFromIndex);

    // The folders of the devices' presets, one for each device listed (as getShelf() names them: "EQ", a plug-in's name), in
    // the order the devices are listed: what tells a folder of no device's (Other Presets) from the others. No disk.
    juce::StringArray getDeviceFolders() const;

    // Whether an item at the top of the user's presets' folder shows in Other Presets: a preset, or a folder named for no
    // device listed, and neither Containers nor Tracks (without regard to case, as the folders are found).
    static bool isOtherPreset (const LibraryItem&, const juce::StringArray& deviceFolders);

    // Where a preset of the presets' folders shows: the place at the top it is under (Instruments, Effects, Track Presets or
    // Other Presets), the container's row it is in under that, if any, and the folder name of the device whose row it is
    // under, if any; and whether a row is a folder that leads to it, in either presets' folder (a folder there shows both). By
    // its folder: one in a device's folder shows under that device, whatever device it is for. Nothing for a file that isn't
    // a preset in a presets' folder, as the index has it, nor for one of Phazite's that is in no list.
    struct PresetHome
    {
        BrowserPlace::Kind place = BrowserPlace::Kind::trackPresets;
        std::optional<BrowserPlace::Kind> container {};
        juce::String deviceFolder {};
    };

    std::optional<PresetHome> findHome (const juce::File& preset) const;
    bool leadsTo (const LibraryItem& folder, const juce::File& preset) const;

    // Whether Phazite Sounds are installed: asked of the disk once, and again after soundPackChanged() (its download or
    // install told).
    bool isSoundPackInstalled() const;
    void soundPackChanged() const { installed.reset(); }

    // The places with filters set and what each lists, as getPlaces() and getContents() give them, by each place's key
    // (BrowserPlace::getKey()); the collections' own under Collections aren't among them (getContents() lists one, a list of
    // the user's own).
    struct FilteredPlaces
    {
        BrowserFilters filters;
        std::vector<BrowserPlace> places;
        std::map<juce::String, BrowserContents> contents;
    };

    // The same as the browser lists them while filters are set: the places of the index (Phazite Sounds, the user's folders)
    // gone through on the search's thread (Library::IndexWork), so that a filter set or the index changed goes
    // through none of it on the message thread; Favorites, Recent and the collections, the user's own lists, at once. Each
    // listing cancels the one before, so that found has the newest's alone, on the message thread; or none, once cancelled.
    void startFilteredPlaces (const BrowserFilters&, std::function<void (FilteredPlaces)> found) const;
    void cancelFilteredPlaces() const;
    bool isListingPlaces() const { return listings.isPending(); }

    // What search finds in all the places, with the filters' values, the best maxFound (Library::search() says how they
    // are ranked), or why it finds nothing; a line where the list is cut, saying how many match ("The first 200 of 3,412");
    // and a line when it covers only part of the sounds. At once, on the calling thread.
    BrowserContents search (const juce::String& query, const BrowserFilters& = {}) const;
    static constexpr int maxFound = 200;

    // The same as the browser searches while the user types: in the background, each search cancelling the one before, so
    // that found has the newest text's alone, on the message thread (Library::startSearch()); or none, once cancelled.
    void startSearch (const juce::String& query, const BrowserFilters&, std::function<void (BrowserContents)> found) const;
    void cancelSearch() const;

    // What each filter offers, over what search finds for the query, or over every sound without one. At once, on the
    // calling thread; through an index given, on any thread, stopping (with nothing) once shouldStop says so.
    FilterOptions getFilterOptions (const juce::String& query, const BrowserFilters&) const;
    static std::optional<FilterOptions> countFilterOptions (const LibraryIndex&, const juce::String& query, const BrowserFilters&,
                                                            const std::function<bool()>& shouldStop = nullptr);

    // The same as a filter's menu opens: counted in the background, on the search's thread (Library::IndexWork), so that
    // a click on a filter goes through none of the library on the message thread; each count cancelling the one before, so
    // that found has the newest's alone, on the message thread; or none, once cancelled.
    void startFilterOptions (const juce::String& query, const BrowserFilters&, std::function<void (FilterOptions)> found) const;
    void cancelFilterOptions() const;

    // How many of the sounds and MIDI files are of a type ("Kick"), as the Type filter counts them over every sound: from
    // the index, without going through it.
    int countSoundsOfType (const juce::String& type) const;

    // The longest list a place shows while filters are set; a line after it says how many more there are.
    static constexpr int maxListed = 2000;

    // The row that downloads Phazite Sounds: what it says as the download goes on, and its tooltip. A click on it starts
    // the download when it can; otherwise it shows the download, or how to get the sounds without one.
    juce::String getSoundPackText() const;
    juce::String getSoundPackTooltip() const;
    bool canStartSoundPackDownload() const;
    bool isSoundPackBusy() const; // downloading or installing

    // An item's row: what it says after the name (a sound's key and tempo, what a device is, or what a preset holds), and
    // its tooltip.
    static juce::String getDetail (const LibraryItem&);
    static juce::String getTooltip (const LibraryItem&);
    static bool canPreview (const LibraryItem&);     // sounds and MIDI files
    static bool previewsInTime (const LibraryItem&); // MIDI files and loops play in time with the song
    static bool canPlace (const LibraryItem&);       // everything but folders goes into the song

    // The line under the tree: what plays.
    static juce::String getPreviewStatus (bool previewsOn, const Previewer&);

private:
    const Library& library;
    const SoundPack& soundPack;
    mutable Library::IndexWork counts { library }, listings { library }; // the filters' counts; the filtered places
    mutable std::optional<bool> installed;                               // isSoundPackInstalled()'s
    static Library::Listed describeFolder (std::function<void (BrowserContents)> opened);

    // What a place lists while filters are set: its sounds with their values, by name, as many as maxListed; and how many
    // it has in all. And what the places of the index list, in one walk through it, any thread (none, once shouldStop says).
    std::vector<LibraryItem> getFiltered (const BrowserPlace&, const BrowserFilters&, int& total) const;
    static bool isOfIndex (const BrowserPlace&);
    struct Listed
    {
        std::vector<LibraryItem> items;
        int total = 0;
    };
    static std::optional<std::vector<Listed>> listIndexed (const LibraryIndex&, const std::vector<BrowserPlace>&, const BrowserFilters&,
                                                           const std::function<bool()>& shouldStop = nullptr);
    BrowserContents describeFiltered (const BrowserPlace&, Listed) const;

    // Whether a place without filters lists a folder (Phazite Sounds once installed, a folder); what it holds, given what the
    // folder holds (none: the folder isn't there). And a list of presets', given what its folders hold.
    bool listsFolder (const BrowserPlace&) const;
    BrowserContents describePlace (const BrowserPlace&, std::optional<std::vector<LibraryItem>> listed) const;
    static BrowserContents describeShelf (const PresetShelf&, std::optional<std::vector<LibraryItem>> listed);
    std::unique_ptr<Library::FolderListing> listShelf (const PresetShelf&, std::function<void (BrowserContents)> opened, bool check) const;

    // Instruments' or Effects' rows: the devices, each with how many presets its row holds, and the containers' row while it
    // holds some.
    void describeDevices (BrowserContents&, bool instruments) const;
    int countForRow (const PresetShelf&) const; // countPresets(), or while the index doesn't know, -1 (none while it is made)
    bool isIndexFirstMade() const;              // the index not made yet: none of its roots walked, its walk under way
    bool hasOtherPresets() const;               // Other Presets' row: what it holds, from the index; or, unknown, shown
    FilteredPlaces assemble (const BrowserFilters&, std::vector<BrowserPlace> places, std::vector<Listed> ofIndex) const;
    BrowserContents describeSearch (const juce::String& query, const BrowserFilters&, SearchResults) const;
};
} // namespace phazite
