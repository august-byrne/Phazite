// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/WholeState.h"
#include "Song.h"

namespace phazite
{
juce::ValueTree copyWholeSong (const Song& song)
{
    JUCE_ASSERT_MESSAGE_THREAD
    return song.getState().createCopy();
}

juce::ValueTree copyWhole (const Song& song, const juce::ValueTree& part)
{
    JUCE_ASSERT_MESSAGE_THREAD
    jassert (part == song.getState() || part.isAChildOf (song.getState()));
    juce::ignoreUnused (song);
    return part.createCopy();
}

ValuesKept keepValues (const Song&)
{
    JUCE_ASSERT_MESSAGE_THREAD
    return {};
}

juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept&)
{
    return treeCopy;
}

bool holdSameNotes (const ValuesKept&, const ValuesKept&, ClipID)
{
    return true;
}
} // namespace phazite
