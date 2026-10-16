// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineGesture.h"

#include <algorithm>

namespace phazite::ui
{
juce::Point<int> TimelineGesture::getStart (const TimelineView& view, int scrollYNow, int unscrolledHeight) const
{
    const auto y = startPosition.y < unscrolledHeight ? startPosition.y : startPosition.y + startScrollY - scrollYNow;
    return { juce::roundToInt (view.beatToX (startBeat)), y };
}

void TimelineGesture::beginStep (ProjectContext& context, const juce::String& name)
{
    if (step == nullptr)
        step = std::make_unique<SongGesture> (context, name);
}

bool TimelineGesture::doOnce (NoteRef note)
{
    if (std::find (doneNotes.begin(), doneNotes.end(), note) != doneNotes.end())
        return false;

    doneNotes.push_back (note);
    return true;
}
} // namespace phazite::ui
