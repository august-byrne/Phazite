// SPDX-License-Identifier: AGPL-3.0-or-later
// A song as the versions keep it (docs/versions.md; model/base/internal/Schema.h: schema::versions): in pieces, each named
// by the SHA-256 of what it holds, so that a version stores only the pieces no version before it has. Its tracks, clips,
// devices, containers and curves are pieces of their own, a clip's many notes and a curve's many points are in runs, each a
// piece, and a plug-in's state is a piece of its own, in its bytes. The same song always makes the same pieces, and
// joined again they make the song as it was, its children in their order. Plain functions of trees and bytes, for any
// thread, of a tree no other thread uses meanwhile.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace phazite
{
// A piece's name: the SHA-256 of what it holds, in 64 lowercase hexadecimal digits.
juce::String getPieceName (const juce::MemoryBlock&);

// A plug-in's parameter's value as the plug-in told it, which a version keeps beside the plug-in's state: by the plug-in's
// own ID, with its name.
struct PluginParameterValue
{
    juce::String id, name;
    float value = 0.0f; // 0 to 1

    bool operator== (const PluginParameterValue&) const = default;
};

// The values of the parameters of a song's plug-ins, by their devices' IDs.
using PluginParameterValues = std::map<std::uint64_t, std::vector<PluginParameterValue>>;

// What a song is made of: its own piece's name, and every piece of it by its name, its own too.
struct SongPieces
{
    juce::String song;
    std::map<juce::String, juce::MemoryBlock> pieces;
};

// The pieces of a song (Schema.h: PHAZITE), or of any element of one (a track's, say), with its plug-ins' parameters'
// values if given.
SongPieces splitSong (const juce::ValueTree& song, const PluginParameterValues& = {});

// A piece's bytes by its name, or nothing for one that is missing or damaged.
using ReadPiece = std::function<std::optional<juce::MemoryBlock> (const juce::String& name)>;

// The element a piece holds, made again of its pieces, with its plug-ins' parameters' values if asked; an invalid tree if a
// piece can't be read.
juce::ValueTree joinSong (const juce::String& name, const ReadPiece&, PluginParameterValues* parameters = nullptr);

// An element as a piece writes it: its properties in the order of their names, its children in their order, with no
// pieces of their own. For other plain records the versions keep (a version's own, Schema.h: VERSION).
juce::MemoryBlock writeElement (const juce::ValueTree&);
juce::ValueTree readElement (const juce::MemoryBlock&); // invalid for what isn't one
} // namespace phazite
