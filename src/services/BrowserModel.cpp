// SPDX-License-Identifier: AGPL-3.0-or-later
#include "BrowserModel.h"

#include <algorithm>

namespace phazite
{
juce::String BrowserPlace::getTitle() const
{
    // A type's sounds, as plurals where English has them: Kicks, Hats, Loops; Percussion, Bass, FX, Foley.
    if (kind == Kind::type && ! juce::StringArray { "Percussion", "Bass", "FX", "Foley" }.contains (name))
        return name + "s";

    return name;
}

juce::String BrowserPlace::getTooltip() const
{
    switch (kind)
    {
        case Kind::favorites:
            return "What you starred: click the star next to a sound or device, or right-click it";
        case Kind::recent:
            return "What you put into the song lately, newest first";
        case Kind::types:
            return "Every sound, from Phazite's and your folders, by what it is: from the words in its name and folders";
        case Kind::type:
            return "Every sound tagged " + name + ", from Phazite's and your folders";
        case Kind::sounds:
            return "Phazite's free sounds: drums, loops, effects and foley, all public domain (CC0)";
        case Kind::instruments:
            return "Instruments turn a track's notes into sound";
        case Kind::effects:
            return "Effects change the sound of a track";
        case Kind::folder:
            return folder.getFullPathName();
        case Kind::addFolder:
            return "Adds a folder of your own sounds to the browser";
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

std::vector<BrowserPlace> BrowserModel::getPlaces() const
{
    using Kind = BrowserPlace::Kind;
    std::vector<BrowserPlace> places {
        { Kind::favorites, "Favorites", {} },     { Kind::recent, "Recent", {} },
        { Kind::types, "Sounds by Type", {} },    { Kind::sounds, "Phazite Sounds", library.getFactorySounds() },
        { Kind::instruments, "Instruments", {} }, { Kind::effects, "Effects", {} }
    };

    for (const auto& folder : library.getUserFolders())
        places.push_back ({ Kind::folder, folder.getFileName(), folder });

    places.push_back ({ Kind::addFolder, "Add Folder...", {} });
    return places;
}

BrowserContents BrowserModel::getContents (const BrowserPlace& place) const
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
        case Kind::types:
            for (const auto& soundType : Library::getSoundTypes())
                contents.places.push_back ({ Kind::type, soundType, {} });

            return contents;
        case Kind::type:
            contents.items = library.listByTag (place.name);
            hint = library.isIndexing() ? "Still looking through your sounds..." : "None yet";
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
            hint = "Your plug-ins show here once Options > Scan for Plug-ins has found them";
            break;
        case Kind::addFolder:
            break;
    }

    // Instruments and effects always have the built-in ones; the hint says where the plug-ins are until there are some.
    const auto& items = contents.items;
    const auto showHint =
        items.empty()
        || ((place.kind == Kind::instruments || place.kind == Kind::effects)
            && std::all_of (items.begin(), items.end(), [] (const LibraryItem& i)
                            { return i.device.has_value() && te::PluginManager::isBuiltInPlugin (i.device->description); }));

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

BrowserContents BrowserModel::search (const juce::String& query) const
{
    BrowserContents contents;
    contents.items = library.search (query.trim());

    if (contents.items.empty())
        contents.messages.add (library.isIndexing() ? juce::String ("Still looking through your sounds...")
                                                    : "Nothing matches \"" + query.trim() + "\"");

    if (library.isIndexFull())
        contents.messages.add ("Search covers the first " + juce::String (Library::maxIndexed) + " sounds");

    return contents;
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

    for (const auto& tag : item.tags)
        if (tag.endsWith (" BPM"))
            return tag;

    return {};
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
