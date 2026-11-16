// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BrowserModel.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>

namespace phazite
{
namespace
{
    bool isSoundOrMidi (const LibraryItem& item)
    {
        return item.kind == LibraryItem::Kind::sound || item.kind == LibraryItem::Kind::midi;
    }

    // The types a sound is: its tags that name one (Kick, Snare...).
    juce::StringArray getTypesOf (const LibraryItem& item)
    {
        static const auto types = []
        {
            auto all = Library::getSoundTypes();
            all.removeString ("Loop");
            return all;
        }();

        juce::StringArray found;

        for (const auto& tag : item.tags)
            if (types.contains (tag))
                found.add (tag);

        return found;
    }

    bool isLoop (const LibraryItem& item)
    {
        return item.tags.contains ("Loop");
    }

    const auto byName = isListedBefore; // by name, then by path: ties always in the same order

    // A count as the browser writes it: "3,412".
    juce::String withThousands (int number)
    {
        auto text = juce::String (std::abs (number));

        for (auto at = text.length() - 3; at > 0; at -= 3)
            text = text.substring (0, at) + "," + text.substring (at);

        return number < 0 ? "-" + text : text;
    }

    // Whether a file is within a folder, by their paths: the index's files are found from the folders the places name, so
    // that their paths start with the folders' as written.
    bool isWithin (const LibraryItem& item, const juce::String& folderPath)
    {
        const auto path = item.file.getFullPathName();
        return path.length() > folderPath.length() && path.startsWith (folderPath)
               && path.substring (folderPath.length(), folderPath.length() + 1) == juce::File::getSeparatorString();
    }
} // namespace

std::optional<int> getTempo (const LibraryItem& item)
{
    for (const auto& tag : item.tags)
        if (tag.endsWith (" BPM"))
            return tag.getIntValue();

    return std::nullopt;
}

bool BrowserFilters::accepts (const LibraryItem& item) const
{
    if (isEmpty())
        return true;

    return isSoundOrMidi (item) && (type.isEmpty() || item.tags.contains (type)) && (! loops.has_value() || isLoop (item) == *loops)
           && (! tempo.has_value() || getTempo (item) == tempo) && (! key.has_value() || item.key == key);
}

//==============================================================================
juce::String BrowserPlace::getTooltip() const
{
    switch (kind)
    {
        case Kind::favorites:
            return "What you starred: click the star next to a sound or device, or right-click it";
        case Kind::recent:
            return "What you put into the song lately, newest first";
        case Kind::sounds:
            return "Phazite's free sounds: drums, loops, effects and foley, all public domain (CC0)";
        case Kind::instruments:
            return "Instruments turn a track's notes into sound";
        case Kind::effects:
            return "Effects change the sound of a track";
        case Kind::factoryPresets:
            return "Phazite's presets for its instruments and effects, by device and by kind of sound: drag one onto a track, "
                   "or pick one from a device's Presets menu";
        case Kind::presets:
            return "Your presets: devices', containers' and tracks' settings, saved to use again, in " + folder.getFullPathName();
        case Kind::folder:
            return folder.getFullPathName();
        case Kind::addFolder:
            return "Adds a folder of your own sounds to the browser";
        case Kind::collections:
            return "Your own sets of sounds, presets and devices: right-click one to add it to a collection. A sound you rename "
                   "or move stays in its collections";
        case Kind::collection:
            return "Your collection " + name + ": right-click it to rename or delete it";
        case Kind::newCollection:
            return "Makes a collection, to keep sounds, presets and devices in";
    }

    return {};
}

juce::String BrowserPlace::getKey() const
{
    return juce::String ((int) kind) + ":" + name + ":" + folder.getFullPathName();
}

//==============================================================================
BrowserModel::BrowserModel (const Library& l, const SoundPack& pack)
    : library (l),
      soundPack (pack)
{
}

std::vector<BrowserPlace> BrowserModel::getPlaces (const BrowserFilters& filters) const
{
    using Kind = BrowserPlace::Kind;
    std::vector<BrowserPlace> places { { Kind::favorites, "Favorites", {} },
                                       { Kind::recent, "Recent", {} },
                                       { Kind::collections, "Collections", {} },
                                       { Kind::sounds, "Phazite Sounds", library.getFactorySounds() },
                                       { Kind::instruments, "Instruments", {} },
                                       { Kind::effects, "Effects", {} },
                                       { Kind::factoryPresets, "Phazite Presets", library.getFactoryPresetsFolder() },
                                       { Kind::presets, "Presets", library.getPresetsFolder() } };

    for (const auto& folder : library.getUserFolders())
        places.push_back ({ Kind::folder, folder.getFileName(), folder });

    places.push_back ({ Kind::addFolder, "Add Folder...", {} });

    if (filters.isEmpty())
        return places;

    std::vector<BrowserPlace> ofIndex;
    std::copy_if (places.begin(), places.end(), std::back_inserter (ofIndex), isOfIndex);
    const auto indexed = library.getWholeIndex(); // held while it is gone through: the index thread may replace it meanwhile
    return assemble (filters, std::move (places), *listIndexed (*indexed, ofIndex, filters)).places;
}

bool BrowserModel::isOfIndex (const BrowserPlace& place)
{
    using Kind = BrowserPlace::Kind;
    return place.kind == Kind::sounds || place.kind == Kind::folder || place.kind == Kind::presets || place.kind == Kind::factoryPresets;
}

std::optional<std::vector<BrowserModel::Listed>> BrowserModel::listIndexed (const LibraryIndex& index,
                                                                            const std::vector<BrowserPlace>& places,
                                                                            const BrowserFilters& filters,
                                                                            const std::function<bool()>& shouldStop)
{
    // One walk for all of them, each item looked for in each place's folder by its path.
    juce::StringArray paths;

    for (const auto& place : places)
        paths.add (place.folder != juce::File() ? place.folder.getFullPathName() : juce::String());

    std::vector<Listed> listed (places.size());

    const auto take = [&] (const LibraryItem& item)
    {
        if (! filters.accepts (item))
            return;

        for (int i = 0; i < paths.size(); ++i)
            if (paths[i].isNotEmpty() && isWithin (item, paths[i]))
                listed[(size_t) i].items.push_back (item);
    };

    if (! Library::forEachFileMatch (index, {}, take, shouldStop))
        return std::nullopt;

    for (auto& each : listed)
    {
        std::sort (each.items.begin(), each.items.end(), byName);
        each.total = (int) each.items.size();

        if (each.total > maxListed)
            each.items.resize ((size_t) maxListed);
    }

    return listed;
}

BrowserContents BrowserModel::describeFiltered (const BrowserPlace&, Listed listed) const
{
    BrowserContents contents;
    contents.items = std::move (listed.items);

    if (listed.total > maxListed)
        contents.messages.add ("The first " + juce::String (maxListed) + " of " + juce::String (listed.total) + ": filter them further");
    else if (contents.items.empty())
        contents.messages.add ("None of these with the filters set");

    return contents;
}

BrowserModel::FilteredPlaces BrowserModel::assemble (const BrowserFilters& filters, std::vector<BrowserPlace> places,
                                                     std::vector<Listed> ofIndex) const
{
    // The places with sounds that have the filters' values, each with how many; Collections while any of its collections
    // has some. The index's places as listed, in their order; the user's own lists at once.
    using Kind = BrowserPlace::Kind;
    FilteredPlaces filtered;
    filtered.filters = filters;
    size_t nextOfIndex = 0;

    for (auto& place : places)
    {
        if (place.kind == Kind::collections)
        {
            if (auto contents = getContents (place, filters); ! contents.places.empty())
            {
                filtered.contents[place.getKey()] = std::move (contents);
                filtered.places.push_back (std::move (place));
            }

            continue;
        }

        if (! place.holdsItems() || place.kind == Kind::instruments || place.kind == Kind::effects)
            continue;

        Listed listed;

        if (isOfIndex (place))
        {
            if (nextOfIndex < ofIndex.size())
                listed = std::move (ofIndex[nextOfIndex]);

            ++nextOfIndex;
        }
        else
        {
            listed.items = getFiltered (place, filters, listed.total);
        }

        if (listed.total > 0)
        {
            place.count = listed.total;
            filtered.contents[place.getKey()] = describeFiltered (place, std::move (listed));
            filtered.places.push_back (std::move (place));
        }
    }

    return filtered;
}

void BrowserModel::startFilteredPlaces (const BrowserFilters& filters, std::function<void (FilteredPlaces)> found) const
{
    auto places = getPlaces();
    std::vector<BrowserPlace> ofIndex;
    std::copy_if (places.begin(), places.end(), std::back_inserter (ofIndex), isOfIndex);
    const auto listed = std::make_shared<std::vector<Listed>>();

    listings.start (
        [listed, ofIndex, filters] (const LibraryIndex& index, const std::function<bool()>& shouldStop)
        {
            auto made = listIndexed (index, ofIndex, filters, shouldStop);

            if (! made.has_value())
                return false;

            *listed = std::move (*made);
            return true;
        },
        [this, listed, filters, places = std::move (places), found = std::move (found)]
        { found (assemble (filters, places, std::move (*listed))); });
}

void BrowserModel::cancelFilteredPlaces() const
{
    listings.cancel();
}

std::vector<BrowserPlace> BrowserModel::getPresetPlaces (const juce::String& folderName) const
{
    using Kind = BrowserPlace::Kind;
    std::vector<BrowserPlace> found;

    if (folderName.isEmpty())
        return found;

    for (auto& place : getPlaces())
    {
        if ((place.kind != Kind::factoryPresets && place.kind != Kind::presets) || place.folder == juce::File())
            continue;

        place.folder = place.folder.getChildFile (folderName);
        place.count = 0;

        // From the index, which holds the presets' folders whole once walked: no disk. Else counted on the disk (the index
        // first being made): the device's presets alone.
        if (const auto counted = library.getWholeIndex()->countFilesWithin (place.folder, presetExtension))
        {
            place.count = *counted;
        }
        else
        {
            Library::noteFileQuery();

            if (place.folder.isDirectory())
                for ([[maybe_unused]] const auto& entry :
                     juce::RangedDirectoryIterator (place.folder, true, juce::String ("*") + presetExtension, juce::File::findFiles))
                    ++place.count;
        }

        if (place.count > 0)
            found.push_back (std::move (place));
    }

    return found;
}

std::vector<LibraryItem> BrowserModel::getFiltered (const BrowserPlace& place, const BrowserFilters& filters, int& total) const
{
    using Kind = BrowserPlace::Kind;
    std::vector<LibraryItem> items;

    // A place of the index: gone through, on the calling thread.
    if (isOfIndex (place))
    {
        const auto indexed = library.getWholeIndex(); // held while it is gone through: the index thread may replace it meanwhile
        auto listed = std::move (listIndexed (*indexed, { place }, filters)->front());
        total = listed.total;
        return std::move (listed.items);
    }

    const auto keep = [&filters, &items] (std::vector<LibraryItem> all)
    {
        for (auto& item : all)
            if (filters.accepts (item))
                items.push_back (std::move (item));
    };

    switch (place.kind)
    {
        case Kind::favorites:
            keep (library.getFavorites());
            break;
        case Kind::recent:
            keep (library.getRecent());
            break;
        case Kind::collection:
            keep (library.getCollection (place.name));
            break;
        case Kind::sounds:
        case Kind::folder:
        case Kind::presets:
        case Kind::factoryPresets:
        case Kind::collections:
        case Kind::instruments:
        case Kind::effects:
        case Kind::addFolder:
        case Kind::newCollection:
            break;
    }

    // Favorites, recent items and collections keep their own order; the others go by name.
    if (place.kind != Kind::favorites && place.kind != Kind::recent && place.kind != Kind::collection)
        std::sort (items.begin(), items.end(), byName);

    total = (int) items.size();

    if (total > maxListed)
        items.resize ((size_t) maxListed);

    return items;
}

BrowserContents BrowserModel::getContents (const BrowserPlace& place, const BrowserFilters& filters) const
{
    using Kind = BrowserPlace::Kind;
    BrowserContents contents;

    // With filters set: the place's sounds with their values, as one list; the collections with any, each with how many.
    if (! filters.isEmpty())
    {
        if (place.kind == Kind::collections)
        {
            for (const auto& name : library.getCollections())
            {
                BrowserPlace inside { Kind::collection, name, {} };
                auto total = 0;
                getFiltered (inside, filters, total);

                if (total > 0)
                {
                    inside.count = total;
                    contents.places.push_back (inside);
                }
            }

            return contents;
        }

        Listed listed;
        listed.items = getFiltered (place, filters, listed.total);
        return describeFiltered (place, std::move (listed));
    }

    // A folder's, read here.
    if (listsFolder (place))
        return describePlace (place, place.folder.isDirectory() ? std::optional (library.listFolder (place.folder)) : std::nullopt);

    return describePlace (place, std::nullopt);
}

bool BrowserModel::listsFolder (const BrowserPlace& place) const
{
    using Kind = BrowserPlace::Kind;
    return place.kind == Kind::folder || place.kind == Kind::factoryPresets || place.kind == Kind::presets
           || (place.kind == Kind::sounds && isSoundPackInstalled());
}

BrowserContents BrowserModel::describePlace (const BrowserPlace& place, std::optional<std::vector<LibraryItem>> listed) const
{
    using Kind = BrowserPlace::Kind;
    BrowserContents contents;
    juce::String hint;

    switch (place.kind)
    {
        case Kind::favorites:
            contents.items = library.getFavorites();
            hint = "Nothing yet: click the star next to a sound or device";
            break;
        case Kind::recent:
            contents.items = library.getRecent();
            hint = "What you put into the song shows here";
            break;
        case Kind::collections:
            for (const auto& name : library.getCollections())
                contents.places.push_back ({ Kind::collection, name, {} });

            contents.places.push_back ({ Kind::newCollection, "New Collection...", {} });

            if (library.getCollections().isEmpty())
                contents.messages.add ("Right-click a sound or a device to add it to a collection");

            return contents;
        case Kind::collection:
            contents.items = library.getCollection (place.name);
            hint = "Empty: right-click a sound or a device, then Add to Collection";
            break;
        case Kind::sounds:
            if (! isSoundPackInstalled())
            {
                contents.offersSoundPack = true;
                contents.messages.add ("Free drums, loops and effects to start with");
                return contents;
            }

            contents.items = listed.value_or (std::vector<LibraryItem>());
            hint = "No sounds here";
            break;
        case Kind::folder:
            contents.items = listed.value_or (std::vector<LibraryItem>());
            hint = listed.has_value() ? "No sounds here" : "This folder isn't there any more";
            break;
        case Kind::instruments:
        case Kind::effects:
            contents.items =
                library.listDevices (place.kind == Kind::instruments ? LibraryItem::Kind::instrument : LibraryItem::Kind::effect);
            hint = "Your plug-ins show here once Scan for Plug-ins, in the gear's menu at the bottom left, has found them";
            break;
        case Kind::factoryPresets:
            contents.items = listed.value_or (std::vector<LibraryItem>());
            hint = "None here";
            break;
        case Kind::presets:
            contents.items = listed.value_or (std::vector<LibraryItem>());
            hint = "None yet: right-click a device or a track to save its settings, or drag a track here";
            break;
        case Kind::addFolder:
        case Kind::newCollection:
            break;
    }

    // Instruments and effects always have the built-in ones; the hint says where the plug-ins are until there are some.
    const auto& items = contents.items;
    const auto showHint =
        items.empty()
        || ((place.kind == Kind::instruments || place.kind == Kind::effects)
            && std::all_of (items.begin(), items.end(), [] (const LibraryItem& i) { return i.device.has_value() && i.device->isBuiltIn; }));

    if (showHint && hint.isNotEmpty())
        contents.messages.add (hint);

    return contents;
}

BrowserContents BrowserModel::getContents (const LibraryItem& folder) const
{
    BrowserContents contents;
    contents.items = library.listFolder (folder.file);

    if (contents.items.empty())
        contents.messages.add ("No sounds here");

    return contents;
}

std::unique_ptr<Library::FolderListing> BrowserModel::open (const BrowserPlace& place, std::function<void (BrowserContents)> opened) const
{
    if (! listsFolder (place))
    {
        opened (describePlace (place, std::nullopt));
        return {};
    }

    return library.startListing (place.folder, [this, place, opened = std::move (opened)] (std::optional<std::vector<LibraryItem>> listed)
                                 { opened (describePlace (place, std::move (listed))); });
}

std::unique_ptr<Library::FolderListing> BrowserModel::open (const LibraryItem& folder, std::function<void (BrowserContents)> opened) const
{
    return library.startListing (folder.file, describeFolder (std::move (opened)));
}

Library::Listed BrowserModel::describeFolder (std::function<void (BrowserContents)> opened)
{
    return [opened = std::move (opened)] (std::optional<std::vector<LibraryItem>> listed)
    {
        BrowserContents contents;
        contents.items = listed.value_or (std::vector<LibraryItem>());

        if (contents.items.empty())
            contents.messages.add ("No sounds here");

        opened (std::move (contents));
    };
}

std::unique_ptr<Library::FolderListing> BrowserModel::check (const BrowserPlace& place, std::function<void (BrowserContents)> opened) const
{
    if (! listsFolder (place))
        return {};

    return library.checkListing (place.folder, [this, place, opened = std::move (opened)] (std::optional<std::vector<LibraryItem>> listed)
                                 { opened (describePlace (place, std::move (listed))); });
}

std::unique_ptr<Library::FolderListing> BrowserModel::check (const LibraryItem& folder, std::function<void (BrowserContents)> opened) const
{
    return library.checkListing (folder.file, describeFolder (std::move (opened)));
}

bool BrowserModel::isSoundPackInstalled() const
{
    // Asked of the disk once, and again only once the pack changed (soundPackChanged()): not at each opening.
    if (! installed.has_value())
    {
        Library::noteFileQuery();
        installed = soundPack.isInstalled();
    }

    return *installed;
}

BrowserContents BrowserModel::search (const juce::String& query, const BrowserFilters& filters) const
{
    const auto indexed = library.getWholeIndex(); // held while it is gone through: the index thread may replace it meanwhile
    return describeSearch (query, filters,
                           Library::searchIndex (*indexed, library.getAllDevices(), query.trim(), maxFound,
                                                 [&filters] (const LibraryItem& item) { return filters.accepts (item); }));
}

void BrowserModel::startSearch (const juce::String& query, const BrowserFilters& filters, std::function<void (BrowserContents)> found) const
{
    library.startSearch (
        query.trim(), maxFound, [filters] (const LibraryItem& item) { return filters.accepts (item); },
        [this, query, filters, found = std::move (found)] (SearchResults results)
        { found (describeSearch (query, filters, std::move (results))); });
}

void BrowserModel::cancelSearch() const
{
    library.cancelSearch();
}

BrowserContents BrowserModel::describeSearch (const juce::String& query, const BrowserFilters& filters, SearchResults results) const
{
    BrowserContents contents;
    contents.items = std::move (results.items);
    const auto typed = query.trim();

    if (contents.items.empty())
        contents.messages.add (library.isIndexing() ? juce::String ("Still looking through your sounds...")
                               : filters.isEmpty()  ? "Nothing matches \"" + typed + "\""
                                                    : "Nothing matches \"" + typed + "\" with the filters set");

    // Where the list is cut, how many more there are (known bug 128).
    if (results.total > (int) contents.items.size())
        contents.messages.add ("The first " + withThousands ((int) contents.items.size()) + " of " + withThousands (results.total)
                               + ": type more to find fewer");

    if (library.isIndexFull())
        contents.messages.add ("Search covers the first " + withThousands (Library::maxIndexed) + " sounds");

    return contents;
}

FilterOptions BrowserModel::getFilterOptions (const juce::String& query, const BrowserFilters& filters) const
{
    const auto indexed = library.getWholeIndex(); // held while it is gone through: the index thread may replace it meanwhile
    return *countFilterOptions (*indexed, query, filters);
}

void BrowserModel::startFilterOptions (const juce::String& query, const BrowserFilters& filters,
                                       std::function<void (FilterOptions)> found) const
{
    const auto options = std::make_shared<FilterOptions>();
    counts.start (
        [options, query, filters] (const LibraryIndex& index, const std::function<bool()>& shouldStop)
        {
            auto counted = countFilterOptions (index, query, filters, shouldStop);

            if (! counted.has_value())
                return false;

            *options = std::move (*counted);
            return true;
        },
        [options, found = std::move (found)] { found (std::move (*options)); });
}

void BrowserModel::cancelFilterOptions() const
{
    counts.cancel();
}

int BrowserModel::countSoundsOfType (const juce::String& type) const
{
    const auto indexed = library.getWholeIndex();
    const auto found = indexed->soundTags.find (type);
    return found != indexed->soundTags.end() ? found->second : 0;
}

std::optional<FilterOptions> BrowserModel::countFilterOptions (const LibraryIndex& index, const juce::String& query,
                                                               const BrowserFilters& filters, const std::function<bool()>& shouldStop)
{
    // Each item counts for a filter's values where it has the other filters' values: so a filter offers what the others
    // leave, and choosing another of its values never leaves nothing.
    std::map<juce::String, int> types;
    std::map<int, int> tempos;
    std::map<SoundKey, int> keys;
    FilterOptions options;

    const auto count = [&] (const LibraryItem& item)
    {
        if (! isSoundOrMidi (item))
            return;

        const auto tempo = getTempo (item);
        const auto hasType = filters.type.isEmpty() || item.tags.contains (filters.type);
        const auto hasLoop = ! filters.loops.has_value() || isLoop (item) == *filters.loops;
        const auto hasTempo = ! filters.tempo.has_value() || tempo == filters.tempo;
        const auto hasKey = ! filters.key.has_value() || item.key == filters.key;

        if (hasLoop && hasTempo && hasKey)
            for (const auto& type : getTypesOf (item))
                ++types[type];

        if (hasType && hasTempo && hasKey)
            ++(isLoop (item) ? options.loops : options.oneShots);

        if (hasType && hasLoop && hasKey && tempo.has_value())
            ++tempos[*tempo];

        if (hasType && hasLoop && hasTempo && item.key.has_value())
            ++keys[*item.key];
    };

    // What search finds for the query, or every sound without one: devices count for no filter.
    if (! Library::forEachFileMatch (index, query.trim(), count, shouldStop))
        return std::nullopt;

    for (const auto& type : Library::getSoundTypes())
        if (const auto found = types.find (type); found != types.end())
            options.types.emplace_back (type, found->second);

    options.tempos.assign (tempos.begin(), tempos.end());
    options.keys.assign (keys.begin(), keys.end());
    return options;
}

//==============================================================================
juce::String BrowserModel::getSoundPackText() const
{
    switch (soundPack.getState())
    {
        case SoundPack::State::downloading:
            return "Downloading... " + juce::String (juce::roundToInt (soundPack.getProgress() * 100.0)) + "%";
        case SoundPack::State::installing:
            return "Installing...";
        case SoundPack::State::failed:
            return "The download didn't finish";
        case SoundPack::State::notInstalled:
        case SoundPack::State::installed: // but its folder has gone
            break;
    }

    const auto size = juce::File::descriptionOfSizeInBytes (soundPack.getRelease().size);
    return SoundPack::canDownload() ? "Download (" + size + ")" : "Get Them (" + size + ")...";
}

juce::String BrowserModel::getSoundPackTooltip() const
{
    switch (soundPack.getState())
    {
        case SoundPack::State::downloading:
        case SoundPack::State::installing:
            return "Click to see the download, or to cancel it";
        case SoundPack::State::failed:
            return soundPack.getError() + " Click for more.";
        case SoundPack::State::notInstalled:
        case SoundPack::State::installed:
            break;
    }

    if (! SoundPack::canDownload())
        return "Opens a page that tells how to download them in a browser, and install them from the file";

    return "Downloads free drums, loops and effects, all public domain (CC0), into "
           + soundPack.getFolder().getParentDirectory().getFullPathName();
}

bool BrowserModel::canStartSoundPackDownload() const
{
    const auto state = soundPack.getState();
    return (state == SoundPack::State::notInstalled || state == SoundPack::State::installed) && SoundPack::canDownload();
}

bool BrowserModel::isSoundPackBusy() const
{
    const auto state = soundPack.getState();
    return state == SoundPack::State::downloading || state == SoundPack::State::installing;
}

//==============================================================================
juce::String BrowserModel::getDetail (const LibraryItem& item)
{
    if (item.device.has_value())
        return item.device->category;

    // What a preset holds: a track, a container, or its device ("EQ").
    if (item.preset.has_value())
    {
        switch (item.preset->kind)
        {
            case PresetKind::track:
                return "Track";
            case PresetKind::container:
                return "Container";
            case PresetKind::device:
                break;
        }

        return item.preset->device;
    }

    // A sound's key and tempo: "Am · 120 BPM".
    juce::StringArray detail;

    if (item.key.has_value())
        detail.add (item.key->getShortName());

    for (const auto& tag : item.tags)
        if (tag.endsWith (" BPM"))
            detail.add (tag);

    return detail.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 "));
}

juce::String BrowserModel::getTooltip (const LibraryItem& item)
{
    switch (item.kind)
    {
        case LibraryItem::Kind::sound:
            return "Click to hear it. Drag it onto a track, or below the tracks for a new one; Enter puts it on the selected track";
        case LibraryItem::Kind::midi:
            return "Notes: click to hear them on a simple synth. Drag them onto a track, or press Enter for the selected track";
        case LibraryItem::Kind::instrument:
            return "Drag it onto a track or a mixer channel, or press Enter to give it to the selected track";
        case LibraryItem::Kind::effect:
            return "Drag it onto a track or a mixer channel, or press Enter to add it to the selected track";
        case LibraryItem::Kind::preset:
            if (item.preset.has_value() && item.preset->kind == PresetKind::track)
                return "A track's devices and mix: drag it into the playlist, or press Enter, for a new track with them";

            return "A preset: drag it onto a track or a mixer channel, or press Enter to add it to the selected track";
        case LibraryItem::Kind::folder:
            break;
    }

    return {};
}

bool BrowserModel::canPreview (const LibraryItem& item)
{
    return item.kind == LibraryItem::Kind::sound || item.kind == LibraryItem::Kind::midi;
}

bool BrowserModel::previewsInTime (const LibraryItem& item)
{
    return item.kind == LibraryItem::Kind::midi || item.tags.contains ("Loop");
}

bool BrowserModel::canPlace (const LibraryItem& item)
{
    return item.kind != LibraryItem::Kind::folder;
}

juce::String BrowserModel::getPreviewStatus (bool previewsOn, const Previewer& previewer)
{
    if (! previewsOn)
        return "Previews are off";

    if (! previewer.isPlaying())
        return "Click a sound to hear it, drag it into the song";

    const auto name = previewer.getFile().getFileNameWithoutExtension();
    return previewer.isInTime() ? name + ", in time with the song" : name;
}
} // namespace phazite
