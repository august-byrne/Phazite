// SPDX-License-Identifier: AGPL-3.0-or-later
// A song as the versions keep it (docs/versions-format.md; model/base/internal/Schema.h: schema::versions): in pieces, each
// named by the SHA-256 of what it holds, so that a version stores only the pieces no version before it has. Its tracks,
// clips, devices, containers and curves are pieces of their own, a clip's many notes and a curve's many points are in
// runs, each a piece, and a property whose value is bytes (a plug-in's state) is a piece of its own, in its bytes
// (schema::isPieceOfItsOwn(), goesInRuns(), isBytesProperty()). Each piece is the element's text as SongText.h writes it,
// on one line. The same song always makes the same pieces, and joined again they make the song as it was, its children in
// their order. Plain functions of trees and bytes, for any thread, of a tree no other thread uses meanwhile.
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

// How many whole songs (PHAZITE) were split since the program started, and how many of them on the message thread: what the
// performance tests count (tests/PerformanceGateTests.cpp), as splitting a song is a costly part of keeping it.
struct SongsSplit
{
    int all = 0, onMessageThread = 0;
};

SongsSplit countSongsSplit();

// A piece's bytes by its name, or nothing for one that is missing or damaged.
using ReadPiece = std::function<std::optional<juce::MemoryBlock> (const juce::String& name)>;

// A piece of a song that couldn't be read: where it was, each element from the song down by its type and key ("TRACK:12",
// as a merge's clash names one), with what was lost last ("CLIP:66", "_run", "_bytes:state"); and the piece's name.
struct MissingPiece
{
    juce::StringArray path;
    juce::String piece;
};

// The element a piece holds, made again of its pieces, with its plug-ins' parameters' values if asked. Whole or not at all
// (an invalid tree if a piece can't be read); or, given somewhere to list what is missing, as much as can be read: an
// element whose piece is missing becomes a stand-in of its type and key (Schema.h: schema::versions::missing), and so does
// one whose run of notes or whose bytes are, so that making the song like it leaves those as they are now. Invalid still if
// the element's own piece can't be read.
// A whole song (PHAZITE) comes out as a project file opens, whoever wrote its pieces (another computer's, another tool's):
// made consistent by the loader's checks (model/project/internal/Project.h: makeConsistent()), its sound files' paths
// against the project's folder if given, which change nothing in a song Phazite wrote (but what only an Undo needs: the
// curves of a device deleted, which a project opened leaves out too); its stand-ins are left out of them, as they are, and
// what names an ID it hasn't is kept where it was read in part (ReadAs::inPart). Whether they gave items IDs anew (an ID
// held twice, or none), if asked: such a song can't be merged, its IDs another's than its line's.
juce::ValueTree joinSong (const juce::String& name, const ReadPiece&, PluginParameterValues* parameters = nullptr,
                          std::vector<MissingPiece>* missing = nullptr, const juce::File& projectFolder = {}, bool* idsGivenAnew = nullptr);

// Whether an element joined so, or anything in it, is such a stand-in: what a merge's choice of a whole element refuses
// (model/versions/internal/AreaClashes.h: the chord lane's and a curve's choosers).
bool holdsStandIn (const juce::ValueTree& element);

// The pieces a piece names, in its order: its elements that are pieces of their own ("TRACK:12"), its runs ("_run"), its
// bytes ("_bytes:state") and its plug-in's parameters ("_parameters"). Nothing for what isn't a piece.
struct PieceReference
{
    juce::String piece, what;
};

std::vector<PieceReference> findReferences (const juce::MemoryBlock& piece);

// An element as a piece writes it: its properties in the order of their names, its children in their order, with no
// pieces of their own. For other plain records the versions keep (a version's own, Schema.h: VERSION).
juce::MemoryBlock writeElement (const juce::ValueTree&);
juce::ValueTree readElement (const juce::MemoryBlock&); // invalid for what isn't one
} // namespace phazite
