// SPDX-License-Identifier: AGPL-3.0-or-later
// Consolidate's insides (model/mixer/FreezeModel.h says what it does, and renders the audio it joins): what it joins on each
// track, and the clips it makes there in place of what it joins, in the caller's undo step.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <vector>

namespace phazite
{
class RangeSelection;

// What Consolidate joins on a track: a range of it, and whether audio clips and MIDI clips play there (unmuted ones).
struct ConsolidatePart
{
    TrackID track {};
    BeatRange range {};
    bool audio = false, midi = false;
};

// What the selection asks to join, track by track in the song's order: with a time range selected across tracks, what lies
// in it on each of its tracks; otherwise the selected clips on each track, from the first one's start to the last one's end.
// Only tracks where clips play in the range (not folders or returns); none while nothing is selected.
std::vector<ConsolidatePart> getConsolidateParts (const Song&, const Selection&, const RangeSelection&);

// Joins what plays in a part's range, in place of every clip there: one MIDI clip of the notes and controller events its
// MIDI clips play there, each note as it plays, cut at the clips' and the range's edges; and, given a file rendered from its
// audio clips there, one audio clip playing it, which follows the song's tempo if every audio clip it replaces did and the
// tempo holds steady over it. A clip that every clip it replaces was called by keeps that name. Returns the clips it made.
std::vector<ClipID> consolidatePart (Song&, const ConsolidatePart&, const juce::File& renderedAudio);
} // namespace phazite
