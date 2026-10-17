// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows (docs/design.md, section 11; roadmap step 1.5): sounds (audio and MIDI files) in the library
// that comes with Phazite and in the user's own folders, and devices (instruments and effects, built in and plug-ins).
// They are found by place or by search, with tags read from their names and folders, and with favorites and recent
// items, which Phazite's settings keep.
#pragma once

#include "Settings.h"
#include "model/mixer/DeviceType.h"
#include "model/presets/PresetModel.h"

#include <optional>
#include <vector>

namespace phazite
{
class EngineContext;

//==============================================================================
// A sound's tempo, from its name ("Break 120 BPM", "loop_128bpm"): 50 to 250 BPM.
std::optional<double> getTempoFromName (const juce::String&);

// A sound's tags, from the words in its name and in the names of its folders below the root: what it is (Kick, Snare,
// Clap, Hat, Cymbal, Tom, Percussion, Bass, FX, Foley, Vocal, Synth), Loop or One-shot, and its tempo ("120 BPM") if
// its name says. A sound is a loop if its name or folders say so, or if its name has a tempo and it isn't an effect.
juce::StringArray getSoundTags (const juce::File&, const juce::File& root);

//==============================================================================
struct LibraryItem
{
    enum class Kind
    {
        folder,
        sound, // audio
        midi,
        instrument,
        effect,
        preset // a device's, a container's or a track's (model/presets/PresetModel.h)
    };

    Kind kind = Kind::sound;
    juce::String name {};                // a file's name without its extension, or a device's name
    juce::File file {};                  // folders, sounds, MIDI files and presets
    std::optional<DeviceType> device {}; // instruments and effects
    std::optional<PresetInfo> preset {}; // presets
    juce::StringArray tags {};

    bool isFile() const noexcept { return kind == Kind::folder || kind == Kind::sound || kind == Kind::midi || kind == Kind::preset; }

    // What favorites and recent items keep: a file's full path, or a device's identifier.
    juce::String getKey() const;
};

//==============================================================================
class Library : private juce::AsyncUpdater
{
public:
    // The user's folders, favorites and recent items are kept in the settings. The factory library is Phazite Sounds'
    // folder (SoundPack::getFolder()), whether it is installed yet or not; tests give none. The user's presets go in the
    // presets folder, in a folder for each device ("EQ"), containers and tracks; tests give one of their own, or none.
    Library (EngineContext&, Settings&, juce::File factorySounds = {}, juce::File presets = {});
    ~Library() override;

    // Documents/Phazite/Presets.
    static juce::File getDefaultPresetsFolder();
    juce::File getPresetsFolder() const { return presetsFolder; }

    // Indexes the presets again, after one was saved, and tells onIndexChanged.
    void notePresetSaved() { reindex(); }

    // The folders the browser shows besides the factory library, in the order added.
    juce::Array<juce::File> getUserFolders() const;
    void addUserFolder (const juce::File&);
    void removeUserFolder (const juce::File&);

    // What a folder holds: its folders, then its sounds, MIDI files and presets, each by name.
    std::vector<LibraryItem> listFolder (const juce::File&) const;

    // The devices that can be added: instruments or effects.
    std::vector<LibraryItem> listDevices (LibraryItem::Kind) const;

    // Sounds and presets (in the factory library, the user's folders and presets, and their folders below) and devices
    // whose names or tags have every word of the query, best first: names that start with the query, then names with a
    // word that does.
    std::vector<LibraryItem> search (const juce::String& query, int maximum = 200) const;

    // Instruments and effects whose names, makers or kinds have every word of the query, best first; for an empty
    // query, all of them: instruments, then effects, each with the built-in ones first. The plug-in picker shows them.
    std::vector<LibraryItem> searchDevices (const juce::String& query) const;

    // Search finds the sounds in an index of the factory library and the user's folders, which a background thread
    // builds when the library is made and when the folders change. onIndexChanged is called on the message thread when
    // it has been rebuilt.
    bool isIndexing() const;
    std::function<void()> onIndexChanged;

    // Indexes the sounds again, as after Phazite Sounds has been installed.
    void refresh() { reindex(); }

    // Whether the index stopped at maxIndexed sounds, so that search misses some.
    bool isIndexFull() const;

    // The indexed sounds with a tag (a type, such as "Kick", or "Loop"), by name, at most maximum. The browser lists
    // them by type, from every library, as Ableton Live's labels and Bitwig's "Drum Hits" do.
    std::vector<LibraryItem> listByTag (const juce::String& tag, int maximum = 2000) const;
    static juce::StringArray getSoundTypes(); // the types getSoundTags() gives, then "Loop"

    bool isFavorite (const LibraryItem&) const;
    void setFavorite (const LibraryItem&, bool);
    std::vector<LibraryItem> getFavorites() const; // those still there

    // Items put into the song, newest first.
    void noteUsed (const LibraryItem&);
    std::vector<LibraryItem> getRecent() const; // those still there

    // The item a key names, if it is still there.
    std::optional<LibraryItem> findItem (const juce::String& key) const;

    LibraryItem makeFileItem (const juce::File&, const juce::File& root) const; // a folder, sound or MIDI file

    juce::File getFactorySounds() const { return factorySounds; } // the library that comes with Phazite

    static constexpr int maxRecent = 50;
    static constexpr int maxIndexed = 50000; // sounds; a larger library is searched as far as this

private:
    struct Indexer;

    EngineContext& engine;
    Settings& settings;
    const juce::File factorySounds, presetsFolder;
    juce::StringArray audioExtensions; // of the formats the engine reads, as ".wav"
    std::unique_ptr<Indexer> indexer;
    mutable juce::CriticalSection indexLock;
    std::vector<LibraryItem> index; // the sounds and presets in the roots, guarded by indexLock

    bool isSoundFile (const juce::File&) const;
    static bool isPreset (const juce::File&); // a preset file Phazite can read
    bool isMidiFile (const juce::File&) const;
    juce::File findRoot (const juce::File&) const;
    void reindex();
    void handleAsyncUpdate() override;

    std::vector<LibraryItem> getItems (const juce::StringArray& keys) const;
    juce::Array<juce::File> getRoots() const; // the factory library, the user's folders and presets
};
} // namespace phazite
