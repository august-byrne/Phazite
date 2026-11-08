// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BrowserModel.h"

#include <algorithm>
#include <map>

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

    const auto byName = [] (const LibraryItem& a, const LibraryItem& b) { return a.name.compareNatural (b.name) < 0; };

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

    // With filters set, the places with sounds that have their values, each with how many; Collections while any of its
    // collections has some.
    std::vector<BrowserPlace> filtered;

    for (auto& place : places)
    {
        if (place.kind == Kind::collections)
        {
            if (! getContents (place, filters).places.empty())
                filtered.push_back (std::move (place));

            continue;
        }

        if (! place.holdsItems() || place.kind == Kind::instruments || place.kind == Kind::effects)
            continue;

        auto total = 0;
        getFiltered (place, filters, total);

        if (total > 0)
        {
            place.count = total;
            filtered.push_back (std::move (place));
        }
    }

    return filtered;
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

        if (place.folder.isDirectory())
            for ([[maybe_unused]] const auto& entry :
                 juce::RangedDirectoryIterator (place.folder, true, juce::String ("*") + presetExtension, juce::File::findFiles))
                ++place.count;

        if (place.count > 0)
            found.push_back (std::move (place));
    }

    return found;
}

std::vector<LibraryItem> BrowserModel::getIndexedIn (const juce::File& folder, const BrowserFilters& filters) const
{
    std::vector<LibraryItem> items;

    if (folder == juce::File())
        return items;

    const auto path = folder.getFullPathName();

    const auto indexed = library.getIndex(); // held while it is gone through: the index thread may replace it meanwhile

    for (const auto& item : *indexed)
        if (filters.accepts (item) && isWithin (item, path))
            items.push_back (item);

    return items;
}

std::vector<LibraryItem> BrowserModel::getFiltered (const BrowserPlace& place, const BrowserFilters& filters, int& total) const
{
    using Kind = BrowserPlace::Kind;
    std::vector<LibraryItem> items;

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
            items = getIndexedIn (place.folder, filters);
            break;
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
    juce::String hint;

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

        auto total = 0;
        contents.items = getFiltered (place, filters, total);

        if (total > maxListed)
            contents.messages.add ("The first " + juce::String (maxListed) + " of " + juce::String (total) + ": filter them further");
        else if (contents.items.empty())
            contents.messages.add ("None of these with the filters set");

        return contents;
    }

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
            if (! soundPack.isInstalled())
            {
                contents.offersSoundPack = true;
                contents.messages.add ("Free drums, loops and effects to start with");
                return contents;
            }

            contents.items = library.listFolder (place.folder);
            hint = "No sounds here";
            break;
        case Kind::folder:
            if (place.folder.isDirectory())
                contents.items = library.listFolder (place.folder);

            hint = place.folder.isDirectory() ? "No sounds here" : "This folder isn't there any more";
            break;
        case Kind::instruments:
        case Kind::effects:
            contents.items =
                library.listDevices (place.kind == Kind::instruments ? LibraryItem::Kind::instrument : LibraryItem::Kind::effect);
            hint = "Your plug-ins show here once Scan for Plug-ins, in the gear's menu at the bottom left, has found them";
            break;
        case Kind::factoryPresets:
            if (place.folder.isDirectory())
                contents.items = library.listFolder (place.folder);

            hint = "None here";
            break;
        case Kind::presets:
            if (place.folder.isDirectory())
                contents.items = library.listFolder (place.folder);

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

BrowserContents BrowserModel::search (const juce::String& query, const BrowserFilters& filters) const
{
    BrowserContents contents;
    contents.items = library.search (query.trim(), 200, [&filters] (const LibraryItem& item) { return filters.accepts (item); });

    if (contents.items.empty())
        contents.messages.add (library.isIndexing() ? juce::String ("Still looking through your sounds...")
                               : filters.isEmpty()  ? "Nothing matches \"" + query.trim() + "\""
                                                    : "Nothing matches \"" + query.trim() + "\" with the filters set");

    if (library.isIndexFull())
        contents.messages.add ("Search covers the first " + juce::String (Library::maxIndexed) + " sounds");

    return contents;
}

FilterOptions BrowserModel::getFilterOptions (const juce::String& query, const BrowserFilters& filters) const
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

    if (query.trim().isNotEmpty())
    {
        library.forEachMatch (query.trim(), count);
    }
    else
    {
        const auto indexed = library.getIndex(); // held while it is gone through: the index thread may replace it meanwhile

        for (const auto& item : *indexed)
            count (item);
    }

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
