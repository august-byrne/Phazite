// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Mirror.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"

namespace phazite::mirror
{
namespace
{
    bool isNear (te::TimePosition a, te::TimePosition b)
    {
        return std::abs ((a - b).inSeconds()) < 1.0e-6;
    }
} // namespace

void reconcileTransport (te::Edit& edit)
{
    auto& transport = edit.getTransport();

    if (const auto on = isLoopOn (edit); transport.looping.get() != on)
        transport.looping = on;

    const auto loop = edit.tempoSequence.toTime (toTracktion (getLoopBeats (edit)));

    if (const auto now = transport.getLoopRange(); ! isNear (now.getStart(), loop.getStart()) || ! isNear (now.getEnd(), loop.getEnd()))
        transport.setLoopRange (loop);

    if (const auto start = getPlayStart (edit); ! isNear (transport.startPosition.get(), start))
        transport.startPosition = start;

    // The bars' first beats louder, as in most DAWs.
    if (const auto on = isMetronomeOn (edit); edit.clickTrackEnabled.get() != on)
    {
        if (on)
            edit.clickTrackEmphasiseBars = true;

        edit.clickTrackEnabled = on;
    }
}
} // namespace phazite::mirror
