// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows (docs/design.md, section 11), without its window (roadmap phase A2): the places at the top of
// its tree and what each holds, what search finds, the row that downloads Phazite Sounds, and what an item's row says
// and does. ui::Browser shows it as a tree.
#pragma once

#include "Library.h"
#include "SoundPack.h"
#include "model/engine/Preview.h"

#include <vector>

namespace phazite
{
// A place in the browser, which lists sounds or devices: at the top of its tree, or a type under "Sounds by Type".
struct BrowserPlace
{
    // In this order: their numbers are in the keys of the places a user left open (Settings::browserOpenness).
    enum class Kind
    {
        favorites,
        recent,
        types,  // "Sounds by Type", which holds a place for each type
        type,   // the sounds of a type (its name), from every library
        sounds, // the ones that come with Phazite
        instruments,
        effects,
        folder, // one of the user's
        addFolder
    };

    Kind kind = Kind::favorites;
    juce::String name; // as the browser shows it; for a type, the type
    juce::File folder; // Phazite Sounds', or the user's folder

    juce::String getTitle() const; // as the row shows it: a type's sounds in the plural (Kicks, Loops; but Percussion, FX)
    juce::String getTooltip() const;
    juce::String getKey() const; // the same for the same place each time the browser lists it
    bool holdsItems() const { return kind != Kind::addFolder; }
};

// What a place or a folder holds, in the order its rows show it.
struct BrowserContents
{
    bool offersSoundPack = false;     // first, while Phazite Sounds aren't installed: the row that downloads them
    std::vector<BrowserPlace> places; // Sounds by Type's types
    std::vector<LibraryItem> items;
    juce::StringArray messages; // last: lines of text, such as what a place is for while it holds nothing
};

class BrowserModel
{
public:
    BrowserModel (const Library&, const SoundPack&);

    // At the top of the tree: Favorites, Recent, Sounds by Type, Phazite Sounds, Instruments, Effects, each of the user's
    // folders, and Add Folder.
    std::vector<BrowserPlace> getPlaces() const;
    BrowserContents getContents (const BrowserPlace&) const;
    BrowserContents getContents (const LibraryItem& folder) const;

    // What search finds in all the places, or why it finds nothing; and a line when it covers only part of the sounds.
    BrowserContents search (const juce::String& query) const;

    // The row that downloads Phazite Sounds: what it says as the download goes on, and its tooltip. A click on it starts
    // the download when it can; otherwise it shows the download, or how to get the sounds without one.
    juce::String getSoundPackText() const;
    juce::String getSoundPackTooltip() const;
    bool canStartSoundPackDownload() const;
    bool isSoundPackBusy() const; // downloading or installing

    // An item's row: what it says after the name (a loop's tempo, or what a device is), and its tooltip.
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
};
} // namespace phazite
