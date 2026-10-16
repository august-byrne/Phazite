// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows, without its window (src/services/BrowserModel.h): its places, what each holds, what search
// finds, the Phazite Sounds row, and what an item's row says.
#include "SoundPackTestHelpers.h"
#include "model/engine/internal/Preview.h"
#include "services/BrowserModel.h"

#include <algorithm>

namespace
{
using Kind = phazite::BrowserPlace::Kind;

juce::StringArray getTitles (const std::vector<phazite::BrowserPlace>& places)
{
    juce::StringArray titles;

    for (const auto& place : places)
        titles.add (place.getTitle());

    return titles;
}

juce::StringArray getNames (const std::vector<phazite::LibraryItem>& items)
{
    juce::StringArray names;

    for (const auto& item : items)
        names.add (item.name);

    return names;
}

// Phazite Sounds in a folder of packs, unless they aren't installed yet.
juce::File makePacks (const juce::File& packs, bool installed)
{
    if (installed)
        phazite::test::writePack (packs.getChildFile (phazite::SoundPack::getPinnedRelease().name));

    return packs;
}

// The library, with Phazite Sounds (or not) and a folder of the user's, and the browser's model of it.
struct Libraries
{
    explicit Libraries (bool packInstalled)
        : pack (makePacks (temp.folder.getChildFile ("Packs"), packInstalled))
    {
        phazite::test::writeSound (userFolder.getChildFile ("Kicks/Deep Kick.wav"), 0.3);
        library.addUserFolder (userFolder);
        REQUIRE (phazite::test::runTimersUntil ([this] { return ! library.isIndexing(); }, 10000));
    }

    phazite::BrowserPlace find (Kind kind) const
    {
        const auto places = model.getPlaces();
        const auto found = std::find_if (places.begin(), places.end(), [kind] (const phazite::BrowserPlace& p) { return p.kind == kind; });
        REQUIRE (found != places.end());
        return *found;
    }

    phazite::test::TemporaryFolder temp;
    phazite::SoundPack pack;
    const juce::File userFolder = temp.folder.getChildFile ("My Sounds");
    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings, pack.getFolder() };
    phazite::BrowserModel model { library, pack };
};
} // namespace

TEST_CASE ("The browser's places come in order, the user's folders before Add Folder, each with a key that stays the same")
{
    Libraries libraries (true);
    const auto places = libraries.model.getPlaces();
    CHECK (getTitles (places)
           == juce::StringArray { "Favorites", "Recent", "Sounds by Type", "Phazite Sounds", "Instruments", "Effects", "My Sounds",
                                  "Add Folder..." });
    CHECK_FALSE (places.back().holdsItems());

    // The keys are those of the places a user left open, which the settings keep: they don't change between versions.
    CHECK (places[0].getKey() == juce::String ("0:Favorites:"));
    CHECK (places[6].getKey() == "7:My Sounds:" + libraries.userFolder.getFullPathName());
    CHECK (places[6].getTooltip() == libraries.userFolder.getFullPathName());
}

TEST_CASE ("Each place holds its items, or says what it is for while it holds none")
{
    Libraries libraries (true);
    const auto& model = libraries.model;

    // Favorites start empty, and say how they fill.
    const auto favorites = model.getContents (libraries.find (Kind::favorites));
    CHECK (favorites.items.empty());
    CHECK (favorites.messages == juce::StringArray { "Nothing yet: click the star next to a sound or device" });

    // Sounds by Type holds a place for each type, which lists that type's sounds from every library, in the plural where
    // English has one.
    const auto types = model.getContents (libraries.find (Kind::types));
    CHECK ((int) types.places.size() == phazite::Library::getSoundTypes().size());
    const auto kicks =
        std::find_if (types.places.begin(), types.places.end(), [] (const phazite::BrowserPlace& p) { return p.name == "Kick"; });
    REQUIRE (kicks != types.places.end());
    CHECK (kicks->getTitle() == juce::String ("Kicks"));
    CHECK (getNames (model.getContents (*kicks).items).contains ("Deep Kick"));
    CHECK (getNames (model.getContents (*kicks).items).contains ("Kick"));
    CHECK (phazite::BrowserPlace { Kind::type, "Percussion", {} }.getTitle() == juce::String ("Percussion"));

    // Phazite Sounds list their folders, and a folder its sounds.
    const auto sounds = model.getContents (libraries.find (Kind::sounds));
    CHECK_FALSE (sounds.offersSoundPack);
    REQUIRE (getNames (sounds.items) == juce::StringArray { "Drums", "Loops" });
    CHECK (getNames (model.getContents (sounds.items[0]).items) == juce::StringArray { "Kick" });

    // The instruments are Phazite's own until a scan finds plug-ins, and say so.
    const auto instruments = model.getContents (libraries.find (Kind::instruments));
    CHECK (getNames (instruments.items).contains ("Synth"));
    CHECK (instruments.messages == juce::StringArray { "Your plug-ins show here once Options > Scan for Plug-ins has found them" });

    // A folder of the user's that has gone says so.
    const auto folder = libraries.find (Kind::folder);
    REQUIRE (libraries.userFolder.deleteRecursively());
    CHECK (model.getContents (folder).messages == juce::StringArray { "This folder isn't there any more" });
}

TEST_CASE ("Until Phazite Sounds are installed, their place offers to download them")
{
    Libraries libraries (false);
    const auto sounds = libraries.model.getContents (libraries.find (Kind::sounds));
    CHECK (sounds.offersSoundPack);
    CHECK (sounds.items.empty());
    CHECK (sounds.messages == juce::StringArray { "Free drums, loops and effects to start with" });

    CHECK (libraries.model.getSoundPackText().startsWith (phazite::SoundPack::canDownload() ? "Download (" : "Get Them ("));
    CHECK (libraries.model.canStartSoundPackDownload() == phazite::SoundPack::canDownload());
    CHECK_FALSE (libraries.model.isSoundPackBusy());
}

TEST_CASE ("Search finds sounds in every place, or says why it finds nothing")
{
    Libraries libraries (true);
    CHECK (getNames (libraries.model.search ("  kick ").items).contains ("Deep Kick"));

    const auto nothing = libraries.model.search ("nothing is called this");
    CHECK (nothing.items.empty());
    CHECK (nothing.messages == juce::StringArray { "Nothing matches \"nothing is called this\"" });
}

TEST_CASE ("An item's row says what it is and what it does, and the line under the tree what plays")
{
    phazite::LibraryItem loop { .kind = phazite::LibraryItem::Kind::sound, .name = "Break", .tags = { "Loop", "120 BPM" } };
    CHECK (phazite::BrowserModel::getDetail (loop) == juce::String ("120 BPM"));
    CHECK (phazite::BrowserModel::canPreview (loop));
    CHECK (phazite::BrowserModel::previewsInTime (loop));
    CHECK (phazite::BrowserModel::canPlace (loop));
    CHECK (phazite::BrowserModel::getTooltip (loop).startsWith ("Click to hear it"));

    const phazite::LibraryItem folder { .kind = phazite::LibraryItem::Kind::folder, .name = "Drums" };
    CHECK_FALSE (phazite::BrowserModel::canPreview (folder));
    CHECK_FALSE (phazite::BrowserModel::canPlace (folder));
    CHECK (phazite::BrowserModel::getTooltip (folder).isEmpty());
    CHECK (phazite::BrowserModel::getDetail (folder).isEmpty());

    const auto previewerOwner = phazite::createPreviewer (phazite::test::getEngine());
    auto& previewer = *previewerOwner;
    CHECK (phazite::BrowserModel::getPreviewStatus (false, previewer) == juce::String ("Previews are off"));
    CHECK (phazite::BrowserModel::getPreviewStatus (true, previewer) == juce::String ("Click a sound to hear it, drag it into the song"));
}
