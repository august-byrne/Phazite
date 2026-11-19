// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows, without its window (src/services/BrowserModel.h): its places, what each holds, the presets under
// each device, what search finds, the Phazite Sounds row, and what an item's row says.
#include "SoundPackTestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/Preview.h"
#include "model/plugins/internal/PluginHost.h"
#include "services/BrowserModel.h"

#include <algorithm>
#include <functional>
#include <optional>

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

//==============================================================================
// A preset's file, as Phazite writes its first element (what the browser reads of it): a device's, a container's or a
// track's.
void writePreset (const juce::File& file, const juce::String& kind, const juce::String& what)
{
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.replaceWithText ("{ \"kind\": \"" + kind + "\", \"" + kind + "\": " + what + " }"));
}

void writeDevicePreset (const juce::File& file, const juce::String& type, const juce::String& device, bool instrument)
{
    writePreset (file, "device",
                 "{ \"type\": \"" + type + "\", \"name\": \"" + device + "\", \"instrument\": " + (instrument ? "true" : "false")
                     + ", \"parameters\": {} }");
}

void writeContainerPreset (const juce::File& file, bool instrument)
{
    writePreset (file, "container", juce::String ("{ \"name\": \"Container\", \"instrument\": ") + (instrument ? "true" : "false") + " }");
}

// Phazite's presets (made here, or those that come with it) and the user's, and the browser's model of them.
struct PresetLibraries
{
    explicit PresetLibraries (std::function<void (const juce::File& factory, const juce::File& own)> write, juce::File factoryFolder = {})
        : factory (factoryFolder != juce::File() ? factoryFolder : temp.folder.getChildFile ("Factory"))
    {
        write (factory, own);
        library.refresh();
        REQUIRE (phazite::test::runTimersUntil ([this] { return ! library.isIndexing(); }));
    }

    phazite::BrowserPlace find (Kind kind) const
    {
        const auto places = model.getPlaces();
        const auto found = std::find_if (places.begin(), places.end(), [kind] (const phazite::BrowserPlace& p) { return p.kind == kind; });
        REQUIRE (found != places.end());
        return *found;
    }

    // A device's row under Instruments or Effects.
    phazite::LibraryItem findDevice (Kind place, const juce::String& name) const
    {
        for (const auto& item : model.getContents (find (place)).items)
            if (item.name == name)
                return item;

        FAIL ("no device called " << name);
        return {};
    }

    phazite::test::TemporaryFolder temp;
    const juce::File factory, own = temp.folder.getChildFile ("Presets");
    phazite::SoundPack pack { temp.folder.getChildFile ("Packs") }; // not installed
    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings, {}, own, factory };
    phazite::BrowserModel model { library, pack };
};

// Every preset the tree leads to from its places, as the user opens its rows: each place, the containers' rows in it, each
// device's row that holds presets, and each folder.
void collectPresets (const phazite::BrowserModel& model, const phazite::BrowserContents& contents, juce::StringArray& found)
{
    for (const auto& place : contents.places)
        collectPresets (model, model.getContents (place), found);

    for (const auto& item : contents.items)
    {
        if (item.kind == phazite::LibraryItem::Kind::preset)
            found.add (item.file.getFullPathName());
        else if (item.kind == phazite::LibraryItem::Kind::folder || contents.getPresets (item) != 0)
            collectPresets (model, model.getContents (item), found);
    }
}
} // namespace

TEST_CASE ("The browser's places come in order, the user's folders before Add Folder, each with a key that stays the same")
{
    Libraries libraries (true);
    const auto places = libraries.model.getPlaces();
    CHECK (getTitles (places)
           == juce::StringArray { "Favorites", "Recent", "Collections", "Sounds", "Instruments", "Effects", "Track Presets", "My Sounds",
                                  "Add Folder..." });
    CHECK_FALSE (places.back().holdsItems());

    // Nothing at the top says Phazite or pack: Phazite Sounds are the sounds to start with, and the presets are under what
    // they belong to.
    for (const auto& title : getTitles (places))
        CHECK_FALSE (title.containsIgnoreCase ("Phazite"));

    // The keys are those of the places a user left open, which the settings keep from one session to the next.
    CHECK (places[0].getKey() == juce::String ("0:Favorites:"));
    CHECK (places[2].getKey() == juce::String ("8:Collections:"));
    CHECK (places[3].getKey() == "2:Sounds:" + libraries.pack.getFolder().getFullPathName());
    CHECK (places[6].getKey().startsWith ("5:Track Presets:"));
    CHECK (places[7].getKey() == "6:My Sounds:" + libraries.userFolder.getFullPathName());
    CHECK (places[7].getTooltip() == libraries.userFolder.getFullPathName());
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
    REQUIRE (getTitles (withKicks) == juce::StringArray { "Sounds", "My Sounds" });
    CHECK (getNames (model.getContents (withKicks[0], kicks).items) == juce::StringArray { "Kick" });
    CHECK (getNames (model.getContents (withKicks[1], kicks).items) == juce::StringArray { "Deep Kick" });

    // Sounds list Phazite Sounds' folders, and a folder its sounds.
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

TEST_CASE ("Until Phazite Sounds are installed, Sounds offers to download them")
{
    Libraries libraries (false);
    CHECK (libraries.find (Kind::sounds).name == juce::String ("Sounds"));
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
    REQUIRE (phazite::test::runTimersUntil ([&libraries] { return ! libraries.library.isIndexing(); }));

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
    REQUIRE (phazite::test::runTimersUntil ([&libraries] { return ! libraries.library.isIndexing(); }));

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

//==============================================================================
TEST_CASE ("A device's row opens to its presets, Phazite's and the user's as one list by name; a device without any to nothing")
{
    // Phazite's for the synth, by kind of sound and one at the top; the user's, one of them in a kind of Phazite's and one
    // named as one of Phazite's.
    PresetLibraries presets (
        [] (const juce::File& factory, const juce::File& own)
        {
            writeDevicePreset (factory.getChildFile ("Synth/Bass/Acid Bass.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (factory.getChildFile ("Synth/Lead/Saw Lead.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (factory.getChildFile ("Synth/Warm Pad.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/My Synth.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/Warm Pad.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/Bass/My Bass.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (factory.getChildFile ("EQ/Mixing/Warm Lows.phazitepreset"), "phaziteEq", "EQ", false);
        });

    auto& model = presets.model;

    // Instruments: the synth's row holds all six; the sampler's none, so it doesn't open. No container's row: there are no
    // containers' presets.
    const auto instruments = model.getContents (presets.find (Kind::instruments));
    const auto synth = presets.findDevice (Kind::instruments, "Synth");
    const auto sampler = presets.findDevice (Kind::instruments, "Sampler");
    CHECK (instruments.getPresets (synth) == 6);
    CHECK (instruments.getPresets (sampler) == 0);
    CHECK (instruments.places.empty());
    CHECK (model.getContents (presets.find (Kind::effects)).getPresets (presets.findDevice (Kind::effects, "EQ")) == 1);

    // Its folders first, one of each name, then the presets by name: the user's of a name before Phazite's, neither marked.
    const auto inSynth = model.getContents (synth);
    REQUIRE (getNames (inSynth.items) == juce::StringArray { "Bass", "Lead", "My Synth", "Warm Pad", "Warm Pad" });
    CHECK (inSynth.messages.isEmpty());
    CHECK (inSynth.items[3].file == presets.own.getChildFile ("Synth/Warm Pad.phazitepreset"));
    CHECK (inSynth.items[4].file == presets.factory.getChildFile ("Synth/Warm Pad.phazitepreset"));
    CHECK (phazite::BrowserModel::getDetail (inSynth.items[3]) == phazite::BrowserModel::getDetail (inSynth.items[4]));

    // A kind's folder holds Phazite's and the user's of it.
    CHECK (getNames (model.getContents (inSynth.items[0]).items) == juce::StringArray { "Acid Bass", "My Bass" });

    // As the browser opens it: from the index, at once; and read from the disk on a listing thread, the same.
    std::optional<phazite::BrowserContents> opened;
    const auto atOnce = model.open (synth, [&opened] (phazite::BrowserContents contents) { opened = std::move (contents); });
    REQUIRE (opened.has_value());
    CHECK (getNames (opened->items) == getNames (inSynth.items));

    presets.library.setListsFromIndex (false);
    opened.reset();
    const auto fromDisk = model.open (inSynth.items[0], [&opened] (phazite::BrowserContents contents) { opened = std::move (contents); });
    CHECK_FALSE (opened.has_value());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return opened.has_value(); }));
    CHECK (getNames (opened->items) == juce::StringArray { "Acid Bass", "My Bass" });

    // A device without presets says so, if it is opened at all (as its swap opens it).
    const auto none = model.getContents (sampler);
    CHECK (none.items.empty());
    CHECK (none.messages == juce::StringArray { "No presets for Sampler yet" });

    // Search finds both of one name.
    CHECK (getNames (model.search ("warm pad").items) == juce::StringArray { "Warm Pad", "Warm Pad" });
}

TEST_CASE ("Every preset that comes with Phazite, and a user's of each kind, shows in the tree where it belongs")
{
    // The presets that come with Phazite, and the user's: an effect's, an instrument's, a container's of effects and one of
    // instruments, and a track's.
    PresetLibraries presets (
        [] (const juce::File&, const juce::File& own)
        {
            writeDevicePreset (own.getChildFile ("EQ/My EQ.phazitepreset"), "phaziteEq", "EQ", false);
            writeDevicePreset (own.getChildFile ("Synth/My Synth.phazitepreset"), "phaziteSynth", "Synth", true);
            writeContainerPreset (own.getChildFile ("Containers/Vocal Rack.phazitepreset"), false);
            writeContainerPreset (own.getChildFile ("Containers/Keys Rack.phazitepreset"), true);
            writePreset (own.getChildFile ("Tracks/Lead Track.phazitepreset"), "track", "{ \"name\": \"Lead\" }");

            // And kept where no device's row lists them: one at the top of the user's presets, one in the folder of a
            // plug-in not scanned, and an effect's saved (through Save Preset's dialog) into an instrument's folder.
            writeDevicePreset (own.getChildFile ("Loose.phazitepreset"), "phaziteEq", "EQ", false);
            writeDevicePreset (own.getChildFile ("Plug-ins/Faraway Synth/Big Lead.phazitepreset"), "plugin", "Faraway Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/EQ In Synth.phazitepreset"), "phaziteEq", "EQ", false);
        },
        phazite::test::getFactoryPresetsFolder());

    auto& model = presets.model;
    juce::StringArray found;

    // Other Presets, after Track Presets, holds those of no device.
    const auto titles = getTitles (model.getPlaces());
    CHECK (titles.indexOf ("Other Presets") == titles.indexOf ("Track Presets") + 1);
    CHECK (getNames (model.getContents (presets.find (Kind::otherPresets)).items) == juce::StringArray { "Faraway Synth", "Loose" });

    for (const auto& place : model.getPlaces())
        if (place.holdsItems())
            collectPresets (model, model.getContents (place), found);

    // Each found where the user would open: every one of Phazite's.
    auto shipped = 0;

    for (const auto& entry :
         juce::RangedDirectoryIterator (presets.factory, true, juce::String ("*") + phazite::presetExtension, juce::File::findFiles))
    {
        CAPTURE (entry.getFile().getFullPathName());
        CHECK (found.contains (entry.getFile().getFullPathName()));
        ++shipped;
    }

    CHECK (shipped > 100);

    // The user's, each once, where it belongs: a device's under its device, a container's under Instrument Container or
    // Container, a track's under Track Presets.
    const auto own = [&presets] (const char* path) { return presets.own.getChildFile (path); };

    for (const auto* path : { "EQ/My EQ.phazitepreset", "Synth/My Synth.phazitepreset", "Containers/Vocal Rack.phazitepreset",
                              "Containers/Keys Rack.phazitepreset", "Tracks/Lead Track.phazitepreset" })
    {
        CAPTURE (path);
        CHECK (std::count (found.begin(), found.end(), own (path).getFullPathName()) == 1);
    }

    // Every one of the user's, wherever it is kept, found too.
    auto usersOwn = 0;

    for (const auto& entry :
         juce::RangedDirectoryIterator (presets.own, true, juce::String ("*") + phazite::presetExtension, juce::File::findFiles))
    {
        CAPTURE (entry.getFile().getFullPathName());
        CHECK (found.contains (entry.getFile().getFullPathName()));
        ++usersOwn;
    }

    CHECK (usersOwn == 8);

    // The effect's in the instrument's folder shows under the instrument, as the folder's are.
    const auto synth = presets.findDevice (Kind::instruments, "Synth");
    CHECK (getNames (model.getContents (synth).items).contains ("EQ In Synth"));

    const auto effects = model.getContents (presets.find (Kind::effects));
    REQUIRE (getTitles (effects.places) == juce::StringArray { "Container" });
    CHECK (getNames (model.getContents (effects.places[0]).items) == juce::StringArray { "Vocal Rack" });
    const auto instruments = model.getContents (presets.find (Kind::instruments));
    REQUIRE (getTitles (instruments.places) == juce::StringArray { "Instrument Container" });
    CHECK (getNames (model.getContents (instruments.places[0]).items) == juce::StringArray { "Keys Rack" });
    CHECK (getNames (model.getContents (presets.find (Kind::trackPresets)).items) == juce::StringArray { "Lead Track" });

    // Where a saved one is shown: its place, and the row it is in there.
    const auto homeOf = [&model, &own] (const char* path) { return model.findHome (own (path)); };
    REQUIRE (homeOf ("EQ/My EQ.phazitepreset").has_value());
    CHECK (homeOf ("EQ/My EQ.phazitepreset")->place == Kind::effects);
    CHECK (homeOf ("EQ/My EQ.phazitepreset")->deviceFolder == juce::String ("EQ"));
    REQUIRE (homeOf ("Synth/My Synth.phazitepreset").has_value());
    CHECK (homeOf ("Synth/My Synth.phazitepreset")->place == Kind::instruments);
    REQUIRE (homeOf ("Containers/Keys Rack.phazitepreset").has_value());
    CHECK (homeOf ("Containers/Keys Rack.phazitepreset")->place == Kind::instruments);
    CHECK (homeOf ("Containers/Keys Rack.phazitepreset")->container == Kind::instrumentContainers);
    REQUIRE (homeOf ("Tracks/Lead Track.phazitepreset").has_value());
    CHECK (homeOf ("Tracks/Lead Track.phazitepreset")->place == Kind::trackPresets);

    // Those of no device in Other Presets; one in another device's folder under that device, whatever device it is for.
    for (const auto* path : { "Loose.phazitepreset", "Plug-ins/Faraway Synth/Big Lead.phazitepreset" })
    {
        CAPTURE (path);
        REQUIRE (homeOf (path).has_value());
        CHECK (homeOf (path)->place == Kind::otherPresets);
    }

    REQUIRE (homeOf ("Synth/EQ In Synth.phazitepreset").has_value());
    CHECK (homeOf ("Synth/EQ In Synth.phazitepreset")->place == Kind::instruments);
    CHECK (homeOf ("Synth/EQ In Synth.phazitepreset")->deviceFolder == juce::String ("Synth"));
    CHECK (model.leadsTo (model.getContents (presets.find (Kind::otherPresets)).items[0],
                          own ("Plug-ins/Faraway Synth/Big Lead.phazitepreset")));

    // With a filter set, presets have none of its values: no place with presets shows.
    CHECK (model.getPlaces ({ .type = "Kick" }).empty());
}

namespace
{
// A plug-in the scan found, in the engine's list (which every test shares): taken away again as it goes.
struct ScannedPlugin
{
    explicit ScannedPlugin (const juce::String& name)
    {
        description.name = name;
        description.pluginFormatName = "VST3";
        description.fileOrIdentifier = "C:/Plug-ins/" + name + ".vst3";
        getKnown().addType (description);
    }

    ~ScannedPlugin() { getKnown().removeType (description); }

    static juce::KnownPluginList& getKnown() { return phazite::getPluginHost (phazite::test::getEngineContext()).getKnownPlugins(); }

    juce::PluginDescription description;
};
} // namespace

TEST_CASE (
    "Other Presets holds the user's presets of no device listed, while it holds any; a plug-in scanned takes its folder under its row")
{
    PresetLibraries presets (
        [] (const juce::File& factory, const juce::File& own)
        {
            writeDevicePreset (factory.getChildFile ("Synth/Bass/Acid Bass.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/My Synth.phazitepreset"), "phaziteSynth", "Synth", true);
            writeContainerPreset (own.getChildFile ("Containers/Vocal Rack.phazitepreset"), false);
        });

    auto& model = presets.model;

    // Every one of the user's under its device or a container's row: no Other Presets.
    CHECK_FALSE (getTitles (model.getPlaces()).contains ("Other Presets"));

    // One saved for a plug-in not scanned yet (after a fresh install, or a plug-in removed): in Other Presets, in its folder.
    const auto wide = presets.own.getChildFile ("Plug-ins/Faraway Echo/Wide.phazitepreset");
    writeDevicePreset (wide, "plugin", "Faraway Echo", false);
    presets.library.refresh();
    REQUIRE (phazite::test::runTimersUntil ([&presets] { return ! presets.library.isIndexing(); }));
    REQUIRE (getTitles (model.getPlaces()).contains ("Other Presets"));
    const auto other = model.getContents (presets.find (Kind::otherPresets));
    REQUIRE (getNames (other.items) == juce::StringArray { "Faraway Echo" });
    CHECK (getNames (model.getContents (other.items[0]).items) == juce::StringArray { "Wide" });
    REQUIRE (model.findHome (wide).has_value());
    CHECK (model.findHome (wide)->place == Kind::otherPresets);

    // The plug-in scanned: its folder is its row's, and Other Presets goes, with nothing read again.
    {
        const ScannedPlugin echo ("Faraway Echo");
        CHECK_FALSE (getTitles (model.getPlaces()).contains ("Other Presets"));
        const auto row = presets.findDevice (Kind::effects, "Faraway Echo");
        CHECK (model.getContents (presets.find (Kind::effects)).getPresets (row) == 1);
        CHECK (getNames (model.getContents (row).items) == juce::StringArray { "Wide" });
        const auto home = model.findHome (wide);
        REQUIRE (home.has_value());
        CHECK (home->place == Kind::effects);
        CHECK (home->deviceFolder == juce::String ("Plug-ins/Faraway Echo"));
    }

    // Gone again (removed): back in Other Presets.
    CHECK (getTitles (model.getPlaces()).contains ("Other Presets"));
}

TEST_CASE ("A plug-in named as one of Phazite's devices has its presets in the plug-ins' folder, under its own row alone; Other "
           "Presets shows those of plug-ins not scanned by their names, not the plug-ins' folder (known bug 209)")
{
    // Phazite's reverb's preset, and presets of two plug-ins named as Phazite's devices: a reverb that is scanned, and an EQ
    // that isn't (removed, or not scanned yet), each in its folder among the plug-ins' (PresetModel.h: getPresetFolderName()).
    PresetLibraries presets (
        [] (const juce::File&, const juce::File& own)
        {
            writeDevicePreset (own.getChildFile ("Reverb/Room.phazitepreset"), "phaziteReverb", "Reverb", false);
            writeDevicePreset (own.getChildFile ("Plug-ins/Reverb/Big Hall.phazitepreset"), "plugin", "Reverb", false);
            writeDevicePreset (own.getChildFile ("Plug-ins/EQ/Gone.phazitepreset"), "plugin", "EQ", false);
        });

    auto& model = presets.model;
    const auto findReverb = [&presets, &model] (bool builtIn)
    {
        for (const auto& item : model.getContents (presets.find (Kind::effects)).items)
            if (item.name == "Reverb" && item.device.has_value() && item.device->isBuiltIn == builtIn)
                return item;

        FAIL ("no reverb");
        return phazite::LibraryItem {};
    };

    // The EQ not scanned: in Other Presets, by its name; the plug-ins' folder isn't a row there, nor Phazite's EQ's row
    // the plug-in's preset.
    {
        REQUIRE (getTitles (model.getPlaces()).contains ("Other Presets"));
        const auto other = model.getContents (presets.find (Kind::otherPresets));
        REQUIRE (getNames (other.items) == juce::StringArray { "EQ", "Reverb" });
        CHECK (getNames (model.getContents (other.items[0]).items) == juce::StringArray { "Gone" });
        CHECK (model.getContents (presets.findDevice (Kind::effects, "EQ")).items.empty());
    }

    // The reverb scanned: its row holds its own preset, and Phazite's reverb's its own; Other Presets the EQ's folder alone.
    const ScannedPlugin reverb ("Reverb");
    const auto own = findReverb (true), plugin = findReverb (false);
    CHECK (phazite::BrowserModel::getShelf (plugin)->folderName == juce::String ("Plug-ins/Reverb"));
    const auto effects = model.getContents (presets.find (Kind::effects));
    CHECK (effects.getPresets (own) == 1);
    CHECK (effects.getPresets (plugin) == 1);
    CHECK (getNames (model.getContents (own).items) == juce::StringArray { "Room" });
    CHECK (getNames (model.getContents (plugin).items) == juce::StringArray { "Big Hall" });
    CHECK (getNames (model.getContents (presets.find (Kind::otherPresets)).items) == juce::StringArray { "EQ" });
    CHECK (model.getDeviceFolders().contains ("Plug-ins/Reverb"));

    // Where each saved one shows: under its own device's row; the EQ's in Other Presets.
    const auto home = model.findHome (presets.own.getChildFile ("Plug-ins/Reverb/Big Hall.phazitepreset"));
    REQUIRE (home.has_value());
    CHECK (home->place == Kind::effects);
    CHECK (home->deviceFolder == juce::String ("Plug-ins/Reverb"));
    const auto room = model.findHome (presets.own.getChildFile ("Reverb/Room.phazitepreset"));
    REQUIRE (room.has_value());
    CHECK (room->deviceFolder == juce::String ("Reverb"));
    const auto gone = model.findHome (presets.own.getChildFile ("Plug-ins/EQ/Gone.phazitepreset"));
    REQUIRE (gone.has_value());
    CHECK (gone->place == Kind::otherPresets);

    // What tells Other Presets' rows: a folder at the top named as one of Phazite's devices is that device's; one in the
    // plug-ins' folder named as a plug-in scanned is that plug-in's; the plug-ins' folder itself none.
    const auto folders = model.getDeviceFolders();
    const auto folder = [] (const juce::String& name)
    { return phazite::LibraryItem { .kind = phazite::LibraryItem::Kind::folder, .name = name }; };
    CHECK_FALSE (phazite::BrowserModel::isOtherPreset (folder ("Reverb"), folders, false));
    CHECK_FALSE (phazite::BrowserModel::isOtherPreset (folder ("Plug-ins"), folders, false));
    CHECK_FALSE (phazite::BrowserModel::isOtherPreset (folder ("Reverb"), folders, true));
    CHECK (phazite::BrowserModel::isOtherPreset (folder ("EQ"), folders, true));
    CHECK (phazite::BrowserModel::isOtherPreset (folder ("Faraway"), folders, false));
}

TEST_CASE ("Where the index doesn't hold the presets' folders whole, every device's row can open, and opens to its presets from the disk")
{
    PresetLibraries presets (
        [] (const juce::File& factory, const juce::File& own)
        {
            writeDevicePreset (factory.getChildFile ("Synth/Bass/Acid Bass.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/My Synth.phazitepreset"), "phaziteSynth", "Synth", true);
        });

    auto& model = presets.model;
    const auto synth = presets.findDevice (Kind::instruments, "Synth");
    const auto sampler = presets.findDevice (Kind::instruments, "Sampler");

    // As a library past what the index holds: no folder of it from the index. Each row may hold presets.
    presets.library.setListsFromIndex (false);
    const auto instruments = model.getContents (presets.find (Kind::instruments));
    CHECK (instruments.getPresets (synth) == -1);
    CHECK (instruments.getPresets (sampler) == -1);

    // Opened: read on a listing thread, Phazite's and the user's as one; one without says so.
    std::optional<phazite::BrowserContents> opened;
    auto listing = model.open (synth, [&opened] (phazite::BrowserContents contents) { opened = std::move (contents); });
    CHECK_FALSE (opened.has_value());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return opened.has_value(); }));
    CHECK (getNames (opened->items) == juce::StringArray { "Bass", "My Synth" });

    opened.reset();
    listing = model.open (sampler, [&opened] (phazite::BrowserContents contents) { opened = std::move (contents); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&opened] { return opened.has_value(); }));
    CHECK (opened->messages == juce::StringArray { "No presets for Sampler yet" });
    listing = nullptr;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&presets] { return ! presets.library.isReadingFolders(); }));
    presets.library.setListsFromIndex (true);

    // Through an index that walked its roots without holding any whole (50,000 folders or more): not known; a presets'
    // folder in no root it walked (the user's, before any preset was saved): none.
    phazite::LibraryIndex index;
    index.walkedRoots.add (presets.factory);
    const auto shelf = phazite::PresetShelf::ofDevice ("Synth", "Synth");
    const auto whole = [&index] (const juce::File& folder) { return index.holdsWhole (folder); };
    CHECK_FALSE (phazite::BrowserModel::countPresets (index, { presets.factory.getChildFile ("Synth") }, shelf, whole).has_value());
    CHECK (phazite::BrowserModel::countPresets (index, { presets.own.getChildFile ("Synth") }, shelf, whole) == 0);
}

TEST_CASE ("A device without a name has no presets, not every preset")
{
    PresetLibraries presets ([] (const juce::File& factory, const juce::File&)
                             { writeDevicePreset (factory.getChildFile ("Synth/Init.phazitepreset"), "phaziteSynth", "Synth", true); });

    CHECK (presets.library.getPresetFolders ("").isEmpty());
    const auto nameless = phazite::PresetShelf::ofDevice ("", "Nameless");
    CHECK (presets.model.countPresets (nameless) == 0);
    CHECK (presets.model.getContents (nameless).items.empty());
}

TEST_CASE (
    "A presets' folder opens to Phazite's and the user's of its name whatever its case, and within a user's folder that holds the presets")
{
    // The user's Bass folder named in lowercase, as a user may make it by hand (on Linux, another folder than Phazite's).
    PresetLibraries presets (
        [] (const juce::File& factory, const juce::File& own)
        {
            writeDevicePreset (factory.getChildFile ("Synth/Bass/Acid Bass.phazitepreset"), "phaziteSynth", "Synth", true);
            writeDevicePreset (own.getChildFile ("Synth/bass/My Bass.phazitepreset"), "phaziteSynth", "Synth", true);
        });

    auto& model = presets.model;
    const auto openFromIndex = [&model] (const phazite::LibraryItem& item)
    {
        std::optional<phazite::BrowserContents> opened;
        const auto listing = model.open (item, [&opened] (phazite::BrowserContents contents) { opened = std::move (contents); });
        REQUIRE (opened.has_value());
        return getNames (opened->items);
    };

    // One Bass folder, which holds both.
    const auto inSynth = model.getContents (presets.findDevice (Kind::instruments, "Synth"));
    REQUIRE (inSynth.items.size() == 1);
    CHECK (inSynth.items[0].name.equalsIgnoreCase ("Bass"));
    CHECK (openFromIndex (inSynth.items[0]) == juce::StringArray { "Acid Bass", "My Bass" });

    // A user's folder that holds both presets' folders: the index walks them as part of it, and the folder still holds both.
    presets.library.addUserFolder (presets.temp.folder);
    REQUIRE (phazite::test::runTimersUntil ([&presets] { return ! presets.library.isIndexing(); }));
    const phazite::LibraryItem bass { .kind = phazite::LibraryItem::Kind::folder,
                                      .name = "bass",
                                      .file = presets.own.getChildFile ("Synth/bass") };
    CHECK (openFromIndex (bass) == juce::StringArray { "Acid Bass", "My Bass" });
    CHECK (getNames (model.getContents (bass).items) == juce::StringArray { "Acid Bass", "My Bass" });
}

TEST_CASE ("A device whose folder of the user's isn't there opens at once, and isn't listed again while nothing differs")
{
    // The user's presets' folder there without the synth's; then not there at all (a fresh install).
    for (const auto usersThere : { true, false })
    {
        CAPTURE (usersThere);
        PresetLibraries presets (
            [usersThere] (const juce::File& factory, const juce::File& own)
            {
                writeDevicePreset (factory.getChildFile ("Synth/Init.phazitepreset"), "phaziteSynth", "Synth", true);

                if (usersThere)
                    writeDevicePreset (own.getChildFile ("EQ/Mine.phazitepreset"), "phaziteEq", "EQ", false);
            });

        auto& model = presets.model;
        const auto synth = presets.findDevice (Kind::instruments, "Synth");
        auto told = 0;
        const auto listing = model.open (synth, [&told] (const phazite::BrowserContents&) { ++told; });
        CHECK (told == 1);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&presets] { return ! presets.library.isReadingFolders(); }));
        phazite::test::dispatchPendingMessages();
        CHECK (told == 1);

        told = 0;
        const auto checked = model.check (synth, [&told] (const phazite::BrowserContents&) { ++told; });
        REQUIRE (phazite::test::dispatchMessagesUntil ([&presets] { return ! presets.library.isReadingFolders(); }));
        phazite::test::dispatchPendingMessages();
        CHECK (told == 0);
    }
}
