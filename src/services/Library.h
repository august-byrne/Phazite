// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows (docs/design.md, section 11; roadmap step 1.5): sounds (audio and MIDI files) in the library
// that comes with Phazite and in the user's own folders, and devices (instruments and effects, built in and plug-ins).
// They are found by place or by search, with tags read from their names and folders, and with favorites and recent
// items, which Phazite's settings keep.
#pragma once

#include "Settings.h"
#include "model/mixer/Mixer.h"

#include <optional>
#include <vector>

namespace phazite
{
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
        effect
    };

    Kind kind = Kind::sound;
    juce::String name {};                // a file's name without its extension, or a device's name
    juce::File file {};                  // folders, sounds and MIDI files
    std::optional<DeviceType> device {}; // instruments and effects
    juce::StringArray tags {};

    bool isFile() const noexcept { return kind == Kind::folder || kind == Kind::sound || kind == Kind::midi; }

    // What favorites and recent items keep: a file's full path, or a device's identifier.
    juce::String getKey() const;
};

// An item dragged from the browser: its description says what it is, so that whatever it is dropped on can tell (a file,
// by its path, or a device, by its plug-in description), without asking the library.
juce::var toDragDescription (const LibraryItem&);
std::optional<juce::File> getDraggedFile (const juce::var& description);
std::optional<DeviceType> getDraggedDevice (const juce::var& description);
juce::String getDraggedKey (const juce::var& description); // the item's key (LibraryItem::getKey()), or empty

// Whatever put a dropped item into the song marks its description, so that the browser can note the item as used.
void markPlaced (const juce::var& description);
bool wasPlaced (const juce::var& description);

//==============================================================================
class Library : private juce::AsyncUpdater
{
public:
    // The user's folders, favorites and recent items are kept in the settings. The factory library is Phazite Sounds'
    // folder (SoundPack::getFolder()), whether it is installed yet or not; tests give none.
    Library (te::Engine&, Settings&, juce::File factorySounds = {});
    ~Library() override;

    // The folders the browser shows besides the factory library, in the order added.
    juce::Array<juce::File> getUserFolders() const;
    void addUserFolder (const juce::File&);
    void removeUserFolder (const juce::File&);

    // What a folder holds: its folders, then its sounds and MIDI files, each by name.
    std::vector<LibraryItem> listFolder (const juce::File&) const;

    // The devices that can be added: instruments or effects.
    std::vector<LibraryItem> listDevices (LibraryItem::Kind) const;

    // Sounds (in the factory library and the user's folders, and their folders below) and devices whose names or tags
    // have every word of the query, best first: names that start with the query, then names with a word that does.
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

    te::Engine& engine;
    Settings& settings;
    const juce::File factorySounds;
    juce::StringArray audioExtensions; // of the formats Tracktion reads, as ".wav"
    std::unique_ptr<Indexer> indexer;
    mutable juce::CriticalSection indexLock;
    std::vector<LibraryItem> index; // the sounds in the roots, guarded by indexLock

    bool isSoundFile (const juce::File&) const;
    bool isMidiFile (const juce::File&) const;
    juce::File findRoot (const juce::File&) const;
    void reindex();
    void handleAsyncUpdate() override;

    std::vector<LibraryItem> getItems (const juce::StringArray& keys) const;
    juce::Array<juce::File> getRoots() const; // the factory library and the user's folders
};
} // namespace phazite
