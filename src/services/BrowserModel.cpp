// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BrowserModel.h"
#include "model/base/WholeFile.h"

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
            return "Free sounds to start with: drums, loops, effects and foley, all public domain (CC0)";
        case Kind::instruments:
            return "Instruments turn a track's notes into sound. Open one to see its presets";
        case Kind::effects:
            return "Effects change the sound of a track. Open one to see its presets";
        case Kind::trackPresets:
            return "Your track presets: a track's devices, mix and modulators, saved to make a new track with them. Drag a track "
                   "into the browser to save one";
        case Kind::containers:
            return "Your presets of containers of effects, each with its devices and macros";
        case Kind::instrumentContainers:
            return "Your presets of instrument containers, each with its devices and macros";
        case Kind::otherPresets:
            return "Your presets of no device listed here: those at the top of " + folder.getFullPathName()
                   + ", and the folders of plug-ins not scanned or no longer installed (once scanned, a plug-in's show under it)";
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
    const auto presets = library.getPresetsFolder();
    std::vector<BrowserPlace> places { { Kind::favorites, "Favorites", {} },
                                       { Kind::recent, "Recent", {} },
                                       { Kind::collections, "Collections", {} },
                                       { Kind::sounds, "Sounds", library.getFactorySounds() },
                                       { Kind::instruments, "Instruments", {} },
                                       { Kind::effects, "Effects", {} },
                                       { Kind::trackPresets, "Track Presets",
                                         presets != juce::File() ? presets.getChildFile (trackPresetsFolder) : juce::File() } };

    if (hasOtherPresets())
        places.push_back ({ Kind::otherPresets, "Other Presets", presets });

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
    return place.kind == Kind::sounds || place.kind == Kind::folder;
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
        case Kind::trackPresets: // presets have none of the filters' values
        case Kind::containers:
        case Kind::instrumentContainers:
        case Kind::otherPresets:
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

    // A folder's, read here; or a list of presets'.
    if (const auto shelf = getShelf (place))
        return getContents (*shelf);

    if (listsFolder (place))
        return describePlace (place, place.folder.isDirectory() ? std::optional (library.listFolder (place.folder)) : std::nullopt);

    return describePlace (place, std::nullopt);
}

BrowserContents BrowserModel::getContents (const PresetShelf& shelf) const
{
    return describeShelf (shelf, library.listFolders (library.getPresetFolders (shelf.folderName)));
}

bool BrowserModel::listsFolder (const BrowserPlace& place) const
{
    using Kind = BrowserPlace::Kind;
    return place.kind == Kind::folder || place.kind == Kind::otherPresets || (place.kind == Kind::sounds && isSoundPackInstalled());
}

//==============================================================================
int BrowserContents::getPresets (const LibraryItem& item) const
{
    const auto found = presets.find (item.getKey());
    return found != presets.end() ? found->second : 0;
}

bool PresetShelf::accepts (const LibraryItem& item) const
{
    if (! instrumentContainers.has_value() || item.kind != LibraryItem::Kind::preset)
        return true;

    // Of instrument containers under Instrument Container; the others (any other preset kept there too) under Container.
    const auto ofInstrumentContainer = item.preset.has_value() && item.preset->kind == PresetKind::container && item.preset->isInstrument;
    return ofInstrumentContainer == *instrumentContainers;
}

PresetShelf PresetShelf::ofDevice (const juce::String& folderName, const juce::String& deviceName)
{
    return { folderName, "No presets for " + deviceName + " yet" };
}

std::optional<PresetShelf> BrowserModel::getShelf (const LibraryItem& item)
{
    if (! item.device.has_value())
        return std::nullopt;

    // Its folder among the presets, as getPresetFolderName() names a device's in the song: by its name.
    return PresetShelf::ofDevice (makeUsableFileName (item.device->name), item.device->name);
}

std::optional<PresetShelf> BrowserModel::getShelf (const BrowserPlace& place) const
{
    using Kind = BrowserPlace::Kind;

    switch (place.kind)
    {
        case Kind::trackPresets:
            return PresetShelf { trackPresetsFolder, "None yet: drag a track here, or right-click one and save its preset" };
        case Kind::containers:
            return PresetShelf { "Containers", "No presets of containers yet", false };
        case Kind::instrumentContainers:
            return PresetShelf { "Containers", "No presets of instrument containers yet", true };
        case Kind::otherPresets: // a folder of the user's: listed as one (describePlace())
        case Kind::favorites:
        case Kind::recent:
        case Kind::sounds:
        case Kind::instruments:
        case Kind::effects:
        case Kind::folder:
        case Kind::addFolder:
        case Kind::collections:
        case Kind::collection:
        case Kind::newCollection:
            break;
    }

    return std::nullopt;
}

std::optional<int> BrowserModel::countPresets (const PresetShelf& shelf) const
{
    // Not while the index is first made.
    const auto index = library.getWholeIndex(); // held while it is gone through: the index thread may replace it meanwhile

    if (isIndexFirstMade())
        return std::nullopt;

    return countPresets (*index, library.getPresetFolders (shelf.folderName), shelf,
                         [this, &index] (const juce::File& folder) { return library.isListedFromIndex (*index, folder); });
}

std::optional<int> BrowserModel::countPresets (const LibraryIndex& index, const juce::Array<juce::File>& folders, const PresetShelf& shelf,
                                               const std::function<bool (const juce::File&)>& listedFromIndex)
{
    // A folder in no root the index walked wasn't there then (a fresh install's presets' folder), and holds none; one in a
    // root it walked is counted where the index holds it whole, else isn't known (a library larger than the index holds,
    // where the row opens from the disk).
    auto count = 0;

    for (const auto& folder : folders)
    {
        if (std::none_of (index.walkedRoots.begin(), index.walkedRoots.end(),
                          [&folder] (const juce::File& root) { return folder == root || folder.isAChildOf (root); }))
            continue;

        if (! listedFromIndex (folder))
            return std::nullopt;

        // Its presets, within its folders too; at its top, those it shows.
        std::vector<juce::String> toCount { folder.getFullPathName() };

        for (size_t next = 0; next < toCount.size(); ++next)
        {
            const auto found = index.children.find (toCount[next]);

            if (found == index.children.end())
                continue;

            for (const auto i : found->second.files)
                if (const auto& file = index.files[i]; file.kind == LibraryItem::Kind::preset && (next > 0 || shelf.accepts (file)))
                    ++count;

            for (const auto i : found->second.folders)
                toCount.push_back (index.folders[i].file.getFullPathName());
        }
    }

    return count;
}

bool BrowserModel::isIndexFirstMade() const
{
    return library.getWholeIndex()->walkedRoots.isEmpty() && library.isIndexing();
}

int BrowserModel::countForRow (const PresetShelf& shelf) const
{
    // While the index doesn't know: none while it is first made (its rows come again once it is, Browser), else maybe some,
    // which the row's opening reads from the disk.
    if (const auto counted = countPresets (shelf))
        return *counted;

    return isIndexFirstMade() ? 0 : -1;
}

juce::StringArray BrowserModel::getDeviceFolders() const
{
    juce::StringArray folders;

    for (const auto& item : library.getAllDevices())
        if (const auto shelf = getShelf (item))
            folders.add (shelf->folderName);

    return folders;
}

bool BrowserModel::isOtherPreset (const LibraryItem& item, const juce::StringArray& deviceFolders)
{
    if (item.kind == LibraryItem::Kind::preset)
        return true;

    const auto& name = item.name;
    return item.kind == LibraryItem::Kind::folder && ! name.equalsIgnoreCase ("Containers") && ! name.equalsIgnoreCase (trackPresetsFolder)
           && ! deviceFolders.contains (name, true);
}

bool BrowserModel::hasOtherPresets() const
{
    // From the index: the user's presets' folder, if it was there as the index walked, holding any. Where the index doesn't
    // hold it whole, it may hold some: shown, and read from the disk as it opens.
    const auto root = library.getPresetsFolder();
    const auto index = library.getWholeIndex();

    if (root == juce::File()
        || std::none_of (index->walkedRoots.begin(), index->walkedRoots.end(),
                         [&root] (const juce::File& walked) { return root == walked || root.isAChildOf (walked); }))
        return false;

    const auto listed = library.isListedFromIndex (*index, root) ? index->listChildren (root) : std::nullopt;

    if (! listed.has_value())
        return true;

    const auto deviceFolders = getDeviceFolders();
    return std::any_of (listed->begin(), listed->end(),
                        [&deviceFolders] (const LibraryItem& item) { return isOtherPreset (item, deviceFolders); });
}

void BrowserModel::describeDevices (BrowserContents& contents, bool instruments) const
{
    contents.items = library.listDevices (instruments ? LibraryItem::Kind::instrument : LibraryItem::Kind::effect);

    for (const auto& item : contents.items)
        if (const auto count = countForRow (*getShelf (item)); count != 0)
            contents.presets[item.getKey()] = count;

    // The containers' presets first, as a rack's are first in Ableton Live's devices: while there are some.
    const BrowserPlace containers { instruments ? BrowserPlace::Kind::instrumentContainers : BrowserPlace::Kind::containers,
                                    instruments ? "Instrument Container" : "Container",
                                    library.getPresetsFolder() != juce::File() ? library.getPresetsFolder().getChildFile ("Containers")
                                                                               : juce::File() };

    if (countForRow (*getShelf (containers)) != 0)
        contents.places.push_back (containers);
}

BrowserContents BrowserModel::describeShelf (const PresetShelf& shelf, std::optional<std::vector<LibraryItem>> listed)
{
    BrowserContents contents;

    for (auto& item : listed.value_or (std::vector<LibraryItem>()))
        if (shelf.accepts (item))
            contents.items.push_back (std::move (item));

    if (contents.items.empty())
        contents.messages.add (shelf.emptyText);

    return contents;
}

std::unique_ptr<Library::FolderListing> BrowserModel::listShelf (const PresetShelf& shelf, std::function<void (BrowserContents)> opened,
                                                                 bool check) const
{
    const auto folders = library.getPresetFolders (shelf.folderName);
    auto described = [shelf, opened = std::move (opened)] (std::optional<std::vector<LibraryItem>> listed)
    { opened (describeShelf (shelf, std::move (listed))); };

    return check ? library.checkListing (folders, std::move (described)) : library.startListing (folders, std::move (described));
}

std::unique_ptr<Library::FolderListing> BrowserModel::open (const PresetShelf& shelf, std::function<void (BrowserContents)> opened) const
{
    return listShelf (shelf, std::move (opened), false);
}

std::unique_ptr<Library::FolderListing> BrowserModel::check (const PresetShelf& shelf, std::function<void (BrowserContents)> opened) const
{
    return listShelf (shelf, std::move (opened), true);
}

std::optional<BrowserModel::PresetHome> BrowserModel::findHome (const juce::File& preset) const
{
    using Kind = BrowserPlace::Kind;
    const auto item = library.getWholeIndex()->find (preset.getFullPathName());

    if (! item.has_value() || ! item->preset.has_value())
        return std::nullopt;

    // The folder of the presets' it is in: its path's first folder in its presets' folder; and whether it is the user's.
    juce::String folderName, within; // its first folder, and its path in that
    auto isUsers = false;

    for (const auto& root : { library.getPresetsFolder(), library.getFactoryPresetsFolder() })
    {
        if (root != juce::File() && preset.isAChildOf (root))
        {
            const auto relative = preset.getRelativePathFrom (root);
            folderName = relative.upToFirstOccurrenceOf (juce::File::getSeparatorString(), false, false);
            within = relative.fromFirstOccurrenceOf (juce::File::getSeparatorString(), false, false);
            isUsers = root == library.getPresetsFolder();
            break;
        }
    }

    // Where its folder shows: a loose one at the top of the user's, Other Presets (Phazite's: in no list).
    if (folderName.isEmpty() || folderName == preset.getFileName())
        return isUsers ? std::optional (PresetHome { Kind::otherPresets, std::nullopt, {} }) : std::nullopt;

    if (folderName.equalsIgnoreCase (trackPresetsFolder))
        return PresetHome { Kind::trackPresets, std::nullopt, {} };

    // At the containers' top, under Instrument Container or Container as PresetShelf::accepts() shows it; within a folder of
    // theirs, under both (Container's taken).
    if (folderName.equalsIgnoreCase ("Containers"))
    {
        const auto atTop = within == preset.getFileName();
        const auto& info = *item->preset;
        const auto ofInstruments = atTop && info.kind == PresetKind::container && info.isInstrument;
        return PresetHome { ofInstruments ? Kind::instruments : Kind::effects,
                            ofInstruments ? Kind::instrumentContainers : Kind::containers,
                            {} };
    }

    // A device's folder: under that device's row, whatever device the preset is for (one saved into another's folder).
    for (const auto& device : library.getAllDevices())
        if (const auto shelf = getShelf (device); shelf.has_value() && shelf->folderName.equalsIgnoreCase (folderName))
            return PresetHome { device.kind == LibraryItem::Kind::instrument ? Kind::instruments : Kind::effects, std::nullopt,
                                shelf->folderName };

    // A folder of no device listed (a plug-in not scanned): the user's in Other Presets.
    return isUsers ? std::optional (PresetHome { Kind::otherPresets, std::nullopt, {} }) : std::nullopt;
}

bool BrowserModel::leadsTo (const LibraryItem& folder, const juce::File& preset) const
{
    if (folder.kind != LibraryItem::Kind::folder)
        return false;

    // In either presets' folder: the same folder of the other shows as it.
    for (const auto& each : library.getPresetFoldersWith (folder.file))
        if (preset.isAChildOf (each))
            return true;

    return false;
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
        case Kind::otherPresets:
        {
            // What the user's presets' folder holds that no other row lists: its own presets, and the folders of no device.
            const auto deviceFolders = getDeviceFolders();

            for (auto& item : listed.value_or (std::vector<LibraryItem>()))
                if (isOtherPreset (item, deviceFolders))
                    contents.items.push_back (std::move (item));

            hint = "None: each of your presets shows under its device";
            break;
        }
        case Kind::instruments:
        case Kind::effects:
            describeDevices (contents, place.kind == Kind::instruments);
            hint = "Your plug-ins show here once Scan for Plug-ins, in the gear's menu at the bottom left, has found them";
            break;
        case Kind::trackPresets: // lists of presets: describeShelf()
        case Kind::containers:
        case Kind::instrumentContainers:
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

BrowserContents BrowserModel::getContents (const LibraryItem& item) const
{
    if (const auto shelf = getShelf (item))
        return getContents (*shelf);

    BrowserContents contents;
    contents.items = library.listFolders (library.getPresetFoldersWith (item.file)).value_or (std::vector<LibraryItem>());

    if (contents.items.empty())
        contents.messages.add ("No sounds here");

    return contents;
}

std::unique_ptr<Library::FolderListing> BrowserModel::open (const BrowserPlace& place, std::function<void (BrowserContents)> opened) const
{
    if (const auto shelf = getShelf (place))
        return open (*shelf, std::move (opened));

    if (! listsFolder (place))
    {
        opened (describePlace (place, std::nullopt));
        return {};
    }

    return library.startListing (place.folder, [this, place, opened = std::move (opened)] (std::optional<std::vector<LibraryItem>> listed)
                                 { opened (describePlace (place, std::move (listed))); });
}

std::unique_ptr<Library::FolderListing> BrowserModel::open (const LibraryItem& item, std::function<void (BrowserContents)> opened) const
{
    if (const auto shelf = getShelf (item))
        return open (*shelf, std::move (opened));

    return library.startListing (library.getPresetFoldersWith (item.file), describeFolder (std::move (opened)));
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
    if (const auto shelf = getShelf (place))
        return check (*shelf, std::move (opened));

    if (! listsFolder (place))
        return {};

    return library.checkListing (place.folder, [this, place, opened = std::move (opened)] (std::optional<std::vector<LibraryItem>> listed)
                                 { opened (describePlace (place, std::move (listed))); });
}

std::unique_ptr<Library::FolderListing> BrowserModel::check (const LibraryItem& item, std::function<void (BrowserContents)> opened) const
{
    if (const auto shelf = getShelf (item))
        return check (*shelf, std::move (opened));

    return library.checkListing (library.getPresetFoldersWith (item.file), describeFolder (std::move (opened)));
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
