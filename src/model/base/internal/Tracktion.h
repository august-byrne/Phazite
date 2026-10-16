// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion Engine as Phazite's model includes it: the engine's one module header and the te alias. Model code
// includes this, not <tracktion_engine/tracktion_engine.h>, so the model's use of the engine starts in one place. It
// also turns Phazite's beats and IDs (Beats.h, IDs.h), which the model's public functions take, into Tracktion's and
// back.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <tracktion_engine/tracktion_engine.h>

namespace phazite
{
namespace te = tracktion;

inline te::BeatPosition toTracktion (BeatPosition position)
{
    return te::BeatPosition::fromBeats (position.inBeats());
}

inline te::BeatDuration toTracktion (BeatDuration duration)
{
    return te::BeatDuration::fromBeats (duration.inBeats());
}

inline te::BeatRange toTracktion (BeatRange range)
{
    return { toTracktion (range.getStart()), toTracktion (range.getEnd()) };
}

inline BeatPosition fromTracktion (te::BeatPosition position)
{
    return BeatPosition::fromBeats (position.inBeats());
}

inline BeatDuration fromTracktion (te::BeatDuration duration)
{
    return BeatDuration::fromBeats (duration.inBeats());
}

inline BeatRange fromTracktion (te::BeatRange range)
{
    return { fromTracktion (range.getStart()), fromTracktion (range.getEnd()) };
}

template <typename Kind>
te::EditItemID toTracktion (ItemID<Kind> id)
{
    return te::EditItemID::fromRawID (id.getRaw());
}

te::EditItemID toTracktion (NoteID) = delete; // a note's ID is Phazite's, not one of Tracktion's (model/notes/internal/Notes.h)

// An item's ID: a track's, a clip's, a section's (an arranger clip's) or a device's.
inline TrackID getTrackID (const te::Track& track)
{
    return TrackID::fromRaw (track.itemID.getRawID());
}

inline ClipID getClipID (const te::Clip& clip)
{
    return ClipID::fromRaw (clip.itemID.getRawID());
}

inline SectionID getSectionID (const te::Clip& section)
{
    return SectionID::fromRaw (section.itemID.getRawID());
}

inline DeviceID getDeviceID (const te::Plugin& device)
{
    return DeviceID::fromRaw (device.itemID.getRawID());
}
} // namespace phazite
