// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows, without its window (src/services/BrowserModel.h): its places, what each holds, what search
// finds, the Phazite Sounds row, and what an item's row says.
#include "SoundPackTestHelpers.h"
#include "UiTestHelpers.h"
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
        titles.add (place.name);

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
        REQUIRE (phazite::test::runTimersUntil ([this] { return ! library.isIndexing(); }));
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
           == juce::StringArray { "Favorites", "Recent", "Collections", "Phazite Sounds", "Instruments", "Effects", "Phazite Presets",
                                  "Presets", "My Sounds", "Add Folder..." });
    CHECK_FALSE (places.back().holdsItems());

    // The keys are those of the places a user left open, which the settings keep from one session to the next.
    CHECK (places[0].getKey() == juce::String ("0:Favorites:"));
    CHECK (places[2].getKey() == juce::String ("9:Collections:"));
    CHECK (places[8].getKey() == "5:My Sounds:" + libraries.userFolder.getFullPathName());
    CHECK (places[6].getKey().startsWith ("8:Phazite Presets:"));
    CHECK (places[7].getKey().startsWith ("7:Presets:"));
    CHECK (places[8].getTooltip() == libraries.userFolder.getFullPathName());
}

TEST_CASE ("Each place holds its items, or says what it is for while it holds none")
{
    Libraries libraries (true);
    const auto& model = libraries.model;

    // Favorites start empty, and say how they fill.
    const auto favorites = model.getContents (libraries.find (Kind::favorites));
    CHECK (favorites.items.empty());
    CHECK (favorites.messages == juce::StringArray { "Nothing yet: click the star next to a sound or device" });

    // The Type filter finds a type's sounds in every library: Phazite Sounds' kick and the user's.
    phazite::BrowserFilters kicks;
    kicks.type = "Kick";
    const auto withKicks = model.getPlaces (kicks);
    REQUIRE (getTitles (withKicks) == juce::StringArray { "Phazite Sounds", "My Sounds" });
    CHECK (getNames (model.getContents (withKicks[0], kicks).items) == juce::StringArray { "Kick" });
    CHECK (getNames (model.getContents (withKicks[1], kicks).items) == juce::StringArray { "Deep Kick" });

    // Phazite Sounds list their folders, and a folder its sounds.
    const auto sounds = model.getContents (libraries.find (Kind::sounds));
    CHECK_FALSE (sounds.offersSoundPack);
    REQUIRE (getNames (sounds.items) == juce::StringArray { "Drums", "Loops" });
    CHECK (getNames (model.getContents (sounds.items[0]).items) == juce::StringArray { "Kick" });

    // The instruments are Phazite's own until a scan finds plug-ins, and say so.
    const auto instruments = model.getContents (libraries.find (Kind::instruments));
    CHECK (getNames (instruments.items).contains ("Synth"));
    CHECK (
        instruments.messages
        == juce::StringArray { "Your plug-ins show here once Scan for Plug-ins, in the gear's menu at the bottom left, has found them" });

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

TEST_CASE ("The browser's search runs in the background, each search cancelling the one before, and the newest text's results "
           "alone arrive")
{
    Libraries libraries (true);
    juce::StringArray arrived; // the texts whose results arrived, in order

    const auto search = [&libraries, &arrived] (const juce::String& text)
    {
        libraries.model.startSearch (text, {},
                                     [&arrived, text] (const phazite::BrowserContents& found)
                                     {
                                         CHECK (getNames (found.items).contains ("Deep Kick"));
                                         arrived.add (text);
                                     });
    };

    // Typed key by key: nothing arrives in the call, and only the last text's results come (known bug 128).
    search ("d");
    search ("de");
    search ("dee");
    CHECK (arrived.isEmpty());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&arrived] { return ! arrived.isEmpty(); }));
    CHECK (arrived == juce::StringArray { "dee" });

    // Cancelled, a search brings nothing: once a search after it has arrived (they run in order), it hasn't.
    search ("deep");
    libraries.model.cancelSearch();
    search ("kick");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&arrived] { return arrived.size() == 2; }));
    CHECK (arrived == juce::StringArray { "dee", "kick" });
}

TEST_CASE ("Search finds folders by their names, and sounds by their folders' names after those whose own names match")
{
    Libraries libraries (true);
    phazite::test::writeSound (libraries.userFolder.getChildFile ("Vintage Drums/Snare Tight.wav"), 0.1);
    phazite::test::writeSound (libraries.userFolder.getChildFile ("Vintage Snare.wav"), 0.1);
    libraries.library.refresh();
    REQUIRE (phazite::test::runTimersUntil ([&libraries] { return ! libraries.library.isIndexing(); }, 60000));

    // A sound named for it first, then one in a folder named for it, then the folder itself.
    const auto found = libraries.model.search ("vintage").items;
    CHECK (getNames (found) == juce::StringArray { "Vintage Snare", "Snare Tight", "Vintage Drums" });
    REQUIRE (found.size() == 3);
    CHECK (found[2].kind == phazite::LibraryItem::Kind::folder);

    // Every word, in any order, in the name, the tags or the folders' names.
    CHECK (getNames (libraries.model.search ("snare vintage").items) == juce::StringArray { "Vintage Snare", "Snare Tight" });
    CHECK (getNames (libraries.model.search ("drums vintage").items) == juce::StringArray { "Snare Tight", "Vintage Drums" });

    // With a filter set, folders don't show, having none of its values.
    CHECK_FALSE (getNames (libraries.model.search ("vintage", { .type = "Snare" }).items).contains ("Vintage Drums"));
}

TEST_CASE ("Where search cuts its list, it says how many match in all")
{
    Libraries libraries (true);

    for (int i = 0; i < phazite::BrowserModel::maxFound + 34; ++i)
        libraries.userFolder.getChildFile ("Many/Clap " + juce::String (i) + ".wav").create();

    libraries.library.refresh();
    REQUIRE (phazite::test::runTimersUntil ([&libraries] { return ! libraries.library.isIndexing(); }, 60000));

    const auto found = libraries.model.search ("clap");
    CHECK ((int) found.items.size() == phazite::BrowserModel::maxFound);
    CHECK (found.messages == juce::StringArray { "The first 200 of 234: type more to find fewer" });
    CHECK (libraries.model.search ("clap 1").messages.isEmpty()); // "Clap 1", "Clap 10"...: fewer than 200
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

    const auto previewerOwner = phazite::createPreviewer (phazite::test::getEngineContext());
    auto& previewer = *previewerOwner;
    CHECK (phazite::BrowserModel::getPreviewStatus (false, previewer) == juce::String ("Previews are off"));
    CHECK (phazite::BrowserModel::getPreviewStatus (true, previewer) == juce::String ("Click a sound to hear it, drag it into the song"));
}

TEST_CASE ("Search keeps equals in the index's order, from one search to the next and as the index grows")
{
    // The same sound in many packs: equal in every way search ranks by.
    phazite::LibraryIndex index;
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Packs");
    const auto addPacks = [&index, &root] (int from, int to)
    {
        for (int pack = from; pack < to; ++pack)
            index.addFile ({ .kind = phazite::LibraryItem::Kind::sound,
                             .name = "Kick 01",
                             .file = root.getChildFile ("Pack " + juce::String (pack) + "/Kick 01.wav"),
                             .tags = { "Kick", "One-shot" } },
                           root);
    };

    addPacks (0, 300);
    const auto first = phazite::Library::searchIndex (index, {}, "kick", 200, nullptr);
    REQUIRE (first.items.size() == 200);
    CHECK (first.total == 300);

    for (size_t i = 0; i < first.items.size(); ++i)
        CHECK (first.items[i].file == index.files[i].file);

    CHECK (phazite::Library::searchIndex (index, {}, "kick", 200, nullptr).items.back().file == first.items.back().file);

    // Grown: the same best 200, in the same order.
    addPacks (300, 900);
    const auto grown = phazite::Library::searchIndex (index, {}, "kick", 200, nullptr);
    REQUIRE (grown.items.size() == 200);

    for (size_t i = 0; i < grown.items.size(); ++i)
        CHECK (grown.items[i].file == first.items[i].file);
}
