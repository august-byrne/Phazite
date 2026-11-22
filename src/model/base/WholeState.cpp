// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/WholeState.h"
#include "Song.h"
#include "internal/ChordLaneArea.h"
#include "internal/ClipsArea.h"
#include "internal/CurvesArea.h"
#include "internal/Schema.h"
#include "internal/TempoMapArea.h"

namespace phazite
{
namespace
{
    // Whether a child of an element in a copy of the song leads to what an area keeps beside the tree: each area asked in
    // turn, a line each (the track list, a track or a folder's tracks, for the clips; the song's TEMPOMAP, for the tempo map;
    // the song's CHORDLANE, for the chord lane).
    bool leadsToKept (const juce::ValueTree& parent, const juce::ValueTree& child)
    {
        if (leadsToClips (parent, child))
            return true;

        if (leadsToTempoMap (parent, child))
            return true;

        if (leadsToChordLane (parent, child))
            return true;

        return false;
    }

    // What the areas keep of a copy's element written in, each area in turn, a line each (a track's clips, first among its
    // elements, in the track's order, each with its notes: ClipsArea.h; the tempo map in the song's TEMPOMAP, the place the
    // song keeps for it: TempoMapArea.h; the chord lane in the song's CHORDLANE: ChordLaneArea.h), each of an element of its
    // own, so that their order is no matter; then the same of each element in it that leads to what an area keeps, found where
    // the areas' elements are (the song's element, its track list and its folders, its TEMPOMAP, its CHORDLANE).
    void writeIn (juce::ValueTree element, const ValuesKept& kept)
    {
        writeClipsIn (element, kept);
        writeTempoMapIn (element, kept);
        writeChordLaneIn (element, kept);

        for (auto child : element)
            if (leadsToKept (element, child))
                writeIn (child, kept);
    }
} // namespace

juce::ValueTree copyWholeSong (const Song& song)
{
    return copyWhole (song, song.getState());
}

juce::ValueTree copyWhole (const Song& song, const juce::ValueTree& part)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto copy = part.createCopy();

    if (song.getValues() != nullptr && (part == song.getState() || part.isAChildOf (song.getState())))
        writeIn (copy, keepValues (song));

    return copy;
}

juce::ValueTree copyWholeClip (const Song& song, ClipID clip)
{
    if (const auto* values = song.getValues())
        if (const auto* value = values->findClip (clip))
            return writeClipElement (*value, values->findClipNotes (clip));

    return {};
}

ValuesKept keepValues (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD
    ValuesKept kept;

    // Each area's value as it is now, a line each.
    if (const auto* values = song.getValues())
    {
        kept.notes = values->notes.get();
        kept.clips = values->clips.get();
        kept.tempoMap = values->tempoMap.get();
        kept.chordLane = values->chordLane.get();
    }

    return kept;
}

juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept& kept)
{
    auto copy = treeCopy.createCopy();
    writeIn (copy, kept);
    return copy;
}

bool holdSameKeptOf (const juce::ValueTree& element, const ValuesKept& kept, const juce::ValueTree& otherElement,
                     const ValuesKept& otherKept)
{
    // Each area in turn, a line each, in this order (a track's clips, then the tempo map, then the chord lane, then the
    // curves), as each is compared once the element's own properties are.
    if (! holdSameClipsOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameTempoMapOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameChordLaneOf (element, kept, otherElement, otherKept))
        return false;

    if (! holdSameCurvesOf (element, kept, otherElement, otherKept))
        return false;

    return true;
}

bool isPlaceOfKept (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    // Each area whose place in the tree is an element of its own, a line each: the tempo map's TEMPOMAP, the chord lane's
    // CHORDLANE; a curve's CURVE in an AUTOMATION, compared as the curves' values (holdSameCurvesOf()). The clips' are none:
    // the copy's tree holds none of them.
    if (isTempoMapPlace (parent, child))
        return true;

    if (isChordLanePlace (parent, child))
        return true;

    if (isCurvesPlace (parent, child))
        return true;

    return false;
}
} // namespace phazite
