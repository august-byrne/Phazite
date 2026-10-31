// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion Engine as Phazite's model includes it: the engine's one module header and the te alias. Model code and the
// tests include this, not <tracktion_engine/tracktion_engine.h>, so the model's use of the engine starts in one place.
// It also turns Phazite's beats and IDs (Beats.h, IDs.h), which the model's public functions take, into Tracktion's and
// back.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

// One of GCC's warnings is off for Tracktion's headers. The zlib in choc, which Tracktion includes, points its decoder's
// tables 257 entries before their starts (base -= 257), as zlib itself did before 1.2.12, and GCC's -O3 calls that out
// of bounds where it specialises the decoder for lengths. Every file that includes Tracktion compiles that decoder, used
// or not: the first virtual function of choc's InflaterStream, its destructor, is defined inline after its class, so
// GCC emits the class's vtable, and the decoder it calls, wherever the header goes. Whether a file warns then depends on
// what the optimiser makes of the whole file, not on anything the file uses.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Warray-bounds")
#include <tracktion_engine/tracktion_engine.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

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

te::EditItemID toTracktion (NoteID) = delete;    // a note's ID is Phazite's, not one of Tracktion's (model/notes/internal/Notes.h)
te::EditItemID toTracktion (SectionID) = delete; // a section is Phazite's own, not one of Tracktion's items (Sections.h)

// An item's ID: a track's, a clip's or a device's. A section's is in Phazite's own state (model/arrangement/internal/Sections.h).
inline TrackID getTrackID (const te::Track& track)
{
    return TrackID::fromRaw (track.itemID.getRawID());
}

inline ClipID getClipID (const te::Clip& clip)
{
    return ClipID::fromRaw (clip.itemID.getRawID());
}

inline DeviceID getDeviceID (const te::Plugin& device)
{
    return DeviceID::fromRaw (device.itemID.getRawID());
}
} // namespace phazite
