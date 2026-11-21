// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/WholeState.h"
#include "Song.h"
#include "internal/Schema.h"
#include "internal/SongIndex.h"

namespace phazite
{
namespace
{
    // The values written into the clips of a copy: each CLIP's (its notes, then its values) found where clips are, in the
    // song's element, its track list and its tracks (only tracks hold clips: Schema.h).
    void writeIn (juce::ValueTree element, const NotesByClip& notes)
    {
        if (element.hasType (schema::CLIP))
        {
            if (const auto* value = notes.find (ClipID::fromRaw ((std::uint64_t) (juce::int64) element[schema::clipID])))
                writeClipNotes (*value, element);

            return;
        }

        if (element.hasType (schema::PHAZITE) || element.hasType (schema::TRACKS) || element.hasType (schema::TRACK))
            for (auto child : element)
                writeIn (child, notes);
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

    if (const auto* values = song.getValues(); values != nullptr && (part == song.getState() || part.isAChildOf (song.getState())))
        writeIn (copy, values->notes.get());

    return copy;
}

juce::ValueTree copyWholeClip (const Song& song, ClipID clip)
{
    if (const auto clipState = clip.isValid() ? song.getIndex().findClip (clip.getRaw()) : juce::ValueTree(); clipState.isValid())
        return copyWhole (song, clipState);

    return {};
}

ValuesKept keepValues (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (const auto* values = song.getValues())
        return { values->notes.get() };

    return {};
}

juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept& kept)
{
    auto copy = treeCopy.createCopy();
    writeIn (copy, kept.notes);
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
} // namespace phazite
