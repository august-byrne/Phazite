// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/WholeState.h"
#include "Song.h"
#include "internal/Schema.h"
#include "internal/TempoMapValue.h"

namespace phazite
{
namespace
{
    // The clips of a copy's tracks written in (only tracks hold clips: Schema.h), found where tracks are, in the song's element,
    // its track list and its folders: under each track, its clips from the values kept, first among its elements, in the
    // track's order, each with its notes (SongValues.h: writeClipElement()). And the tempo map in the song's TEMPOMAP, the
    // place the song keeps for it (TempoMapValue.h: writeTempoMap(): tempos then time signatures, each kind by beat).
    void writeIn (juce::ValueTree element, const ValuesKept& kept)
    {
        // Kept of a song that holds its values, which holds at least one of each kind (none kept of one that keeps everything in
        // its tree, whose TEMPOMAP holds its entries).
        if (element.hasType (schema::TEMPOMAP))
        {
            if (! kept.tempoMap.tempos.empty())
            {
                jassert (element.getNumChildren() == 0); // the song's tree holds none of its entries (SongValues.h)
                writeTempoMap (kept.tempoMap, element);
            }

            return;
        }

        if (element.hasType (schema::TRACK))
        {
            const auto track = (std::uint64_t) (juce::int64) element[schema::trackID];

            if (const auto* list = kept.clips.tracks.find (track))
            {
                int place = 0;

                for (const auto id : *list)
                    if (const auto* clip = kept.clips.find (id))
                        element.addChild (writeClipElement (*clip, kept.notes.find (id)), place++, nullptr);
            }
        }

        if (element.hasType (schema::PHAZITE) || element.hasType (schema::TRACKS) || element.hasType (schema::TRACK))
            for (auto child : element)
                if (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK)
                    || (child.hasType (schema::TEMPOMAP) && element.hasType (schema::PHAZITE)))
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

    if (const auto* values = song.getValues())
        return { values->notes.get(), values->clips.get(), values->tempoMap.get() };

    return {};
}

juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept& kept)
{
    auto copy = treeCopy.createCopy();
    writeIn (copy, kept);
    return copy;
}

bool holdSameNotes (const ValuesKept& a, const ValuesKept& b, ClipID clip)
{
    const auto* first = a.notes.find (clip);
    const auto* second = b.notes.find (clip);

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second;
}

bool holdSameClip (const ValuesKept& a, const ValuesKept& b, ClipID clip)
{
    const auto* first = a.clips.find (clip);
    const auto* second = b.clips.find (clip);

    if (first == nullptr || second == nullptr)
        return first == second;

    return *first == *second && holdSameNotes (a, b, clip);
}

bool holdSameTrackClips (const ValuesKept& a, const ValuesKept& b, TrackID track)
{
    const auto* first = a.clips.tracks.find (track.getRaw());
    const auto* second = b.clips.tracks.find (track.getRaw());

    if (first == nullptr || second == nullptr)
        return first == second;

    if (first->identity() != second->identity() && ! (*first == *second))
        return false;

    for (const auto clip : *first)
        if (! holdSameClip (a, b, clip))
            return false;

    return true;
}

bool holdSameTempoMap (const juce::ValueTree& song, const ValuesKept& kept, const juce::ValueTree& otherSong, const ValuesKept& otherKept)
{
    // The values kept, each tree's TEMPOMAP the place of its map alone.
    if (song.getChildWithName (schema::TEMPOMAP).isValid() != otherSong.getChildWithName (schema::TEMPOMAP).isValid())
        return false;

    return kept.tempoMap.isSameAs (otherKept.tempoMap) || kept.tempoMap == otherKept.tempoMap;
}
} // namespace phazite
