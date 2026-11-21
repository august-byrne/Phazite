// SPDX-License-Identifier: AGPL-3.0-or-later
// The song whole, or a part of it whole: what needs the song or a part of it with everything it holds (a save, a version's
// pieces, a comparison, a merge, the clipboard, a duplicate, an export) asks these, never the song's tree alone
// (Song::getState().createCopy()). The song is moving to values held beside its tree, one area at a time (roadmap step
// S5.20; model/base/internal/ValueArea.h): a MIDI clip's notes and its controllers' values first, which its CLIP element
// in the tree will then no longer hold. These write what is kept beside the tree back in, so that what asks gets the same
// tree a song that kept everything in its tree would give. While nothing is kept beside the tree yet, each answer is a
// plain copy of the tree.
#pragma once

#include "model/base/IDs.h"

#include <juce_data_structures/juce_data_structures.h>

namespace phazite
{
class Song;

// The song's tree, or a part of it (a TRACK, a CLIP, any element of the song), as a copy that holds everything the song
// holds of it: what is kept beside the tree written back in (a clip's NOTE and CONTROL elements, from the notes' values).
// A copy no one else holds, which any thread may then read. Message thread.
juce::ValueTree copyWholeSong (const Song&);
juce::ValueTree copyWhole (const Song&, const juce::ValueTree& part);

// What the song keeps beside its tree, as it is now: kept for nothing (values share what they hold), for what must write or
// compare the song later than it copies it (a save in the background, the copy a project compares itself with: model/
// project/internal/Project.h). Together with a copy of the tree taken at the same moment it gives the whole state then.
// Nothing yet: the notes' value comes with the notes (the next piece of S5.20). Any thread may read one once kept.
struct ValuesKept
{
};

ValuesKept keepValues (const Song&); // message thread

// A copy of the song's tree taken with keepValues() at the same moment, made whole: the kept values written in. While
// nothing is kept beside the tree, the tree given itself, not copied (no caller paid for a copy here before): so a caller
// that keeps the tree given and hands the result to another thread copies it first. Any thread.
juce::ValueTree copyWhole (const juce::ValueTree& treeCopy, const ValuesKept&);

// Whether two kept values hold the same notes and controllers' values for a clip (by its ID): the half of a comparison of
// the song with its saved copy that the trees no longer hold (Project's holdTheSameProject()), asked for each CLIP the
// trees compare. Always so while nothing is kept beside the tree. Any thread.
bool holdSameNotes (const ValuesKept&, const ValuesKept&, ClipID);
} // namespace phazite
