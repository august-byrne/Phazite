// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows (docs/design.md, section 11), without its window (roadmap phase A2): the places at the top of
// its tree and what each holds, what search finds, the filters over them and what each offers, the row that downloads
// Phazite Sounds, and what an item's row says and does. ui::Browser shows it as a tree.
#pragma once

#include "Library.h"
#include "SoundPack.h"
#include "model/engine/Preview.h"

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
        sounds, // the ones that come with Phazite
        instruments,
        effects,
        folder, // one of the user's
        addFolder,
        presets,        // the user's presets, listed after the effects and Phazite's presets
        factoryPresets, // the presets that come with Phazite
        collections,    // the user's collections, each a place of its own (its name), then the row that makes one
        collection,
        newCollection
    };

    Kind kind = Kind::favorites;
    juce::String name; // as its row shows it; for a collection, its name
    juce::File folder; // Phazite Sounds', or the user's folder
    int count = -1;    // while filters are set, how many of its sounds have their values; else -1

    juce::String getTooltip() const;
    juce::String getKey() const; // the same for the same place each time the browser lists it
    bool holdsItems() const { return kind != Kind::addFolder && kind != Kind::newCollection; }
};

// What a place or a folder holds, in the order its rows show it.
struct BrowserContents
{
    bool offersSoundPack = false;     // first, while Phazite Sounds aren't installed: the row that downloads them
    std::vector<BrowserPlace> places; // the collections, under Collections
    std::vector<LibraryItem> items;
    juce::StringArray messages; // last: lines of text, such as what a place is for while it holds nothing
};

//==============================================================================
// The browser's filters (docs/design.md, section 11), over what it shows: what a sound is, whether it is a loop or a
// one-shot, its tempo and its key. Each one set narrows what shows to the sounds and MIDI files that have its value; with
// any set, each place lists those of its own as one list, its folders' and theirs within included, and the places without
// any don't show.
struct BrowserFilters
{
    juce::String type;         // a sound's type, as Library::getSoundTypes() names them ("Kick"), but "Loop"
    std::optional<bool> loops; // loops, or one-shots
    std::optional<int> tempo;  // in beats per minute, as a sound's tags give it
    std::optional<SoundKey> key;

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
    std::vector<std::pair<juce::String, int>> types;
    int loops = 0, oneShots = 0;
    std::vector<std::pair<int, int>> tempos;
    std::vector<std::pair<SoundKey, int>> keys;
};

// A sound's tempo, from its tags ("120 BPM"); none for one whose tags give none.
std::optional<int> getTempo (const LibraryItem&);

//==============================================================================
class BrowserModel
{
public:
    BrowserModel (const Library&, const SoundPack&);

    // At the top of the tree: Favorites, Recent, Collections, Phazite Sounds, Instruments, Effects, Phazite Presets, Presets,
    // each of the user's folders, and Add Folder. With filters set, those that hold sounds with their values, each with how
    // many it holds (the Type filter lists a type's sounds from every library, as Ableton Live's labels and Bitwig's "Drum
    // Hits" do).
    std::vector<BrowserPlace> getPlaces (const BrowserFilters& = {}) const;
    BrowserContents getContents (const BrowserPlace&, const BrowserFilters& = {}) const;
    BrowserContents getContents (const LibraryItem& folder) const;

    // What search finds in all the places, with the filters' values, or why it finds nothing; and a line when it covers only
    // part of the sounds.
    BrowserContents search (const juce::String& query, const BrowserFilters& = {}) const;

    // What each filter offers, over what search finds for the query, or over every sound without one.
    FilterOptions getFilterOptions (const juce::String& query, const BrowserFilters&) const;

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

    // What a place lists while filters are set: its sounds with their values, by name, as many as maxListed; and how many
    // it has in all.
    std::vector<LibraryItem> getFiltered (const BrowserPlace&, const BrowserFilters&, int& total) const;
    std::vector<LibraryItem> getIndexedIn (const juce::File& folder, const BrowserFilters&) const;
};
} // namespace phazite
