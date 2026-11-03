// SPDX-License-Identifier: AGPL-3.0-or-later
// The song (docs/tracktion-migration.md, T6 step 1): Phazite's own record of a project, the PHAZITE element whose every
// name model/base/internal/Schema.h defines, with its undo history and its count of IDs. Every change to it is an undo
// step in its history (model/commands/internal/Undo.h). While Tracktion's Edit mirrors it (model/mirror/internal/Mirror.h),
// the Edit holds it (getSong(), Schema.h) and the song tells the mirror what changed; the model's functions that need
// nothing of Tracktion's take the song.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>

namespace phazite
{
class Song final : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<Song>;

    explicit Song (juce::ValueTree state); // a PHAZITE element
    ~Song() override;

    juce::ValueTree getState() const noexcept { return state; }

    // Its undo history: 30 steps at least.
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    // A new ID for an item of the song (a track, a channel's fader or VCA, a clip, a note, an alias group, a device, a
    // container, a send, a modulator, a macro, a section): the song's next (Schema.h: nextID), never given twice, nor
    // taken back by an undo.
    std::uint64_t makeID(); // in Schema.cpp, with the name of the count

    //==============================================================================
    // What a change was to, which the mirror brings Tracktion's Edit in line with.
    enum class Part
    {
        tempoMap,
        tracks,
        clips,
        clip, // one, by its ID
        automation,
        devices,
        modulation,
        transport,
        everything
    };

    // Tracktion's side, while an Edit mirrors the song, which the Edit sets: its own count of IDs, which the song's keep
    // past (makeID()) so that the two never meet; and what brings the Edit in line after a change to a part of the song.
    void setTracktionIDs (std::function<std::uint64_t()> takeNext);
    void setMirror (std::function<void (Part, std::uint64_t id)> reconcile);
    void reconcile (Part, std::uint64_t id = 0); // nothing without a mirror; the ID of the one it is of

private:
    juce::ValueTree state;
    juce::UndoManager undoManager;
    std::function<std::uint64_t()> takeTracktionID;
    std::function<void (Part, std::uint64_t)> mirror;

    JUCE_DECLARE_NON_COPYABLE (Song)
};
} // namespace phazite
