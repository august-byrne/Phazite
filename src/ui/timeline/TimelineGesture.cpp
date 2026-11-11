// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineGesture.h"

#include <algorithm>

namespace phazite::ui
{
juce::Point<int> TimelineGesture::getStart (const TimelineView& view, int scrollYNow, int unscrolledHeight) const
{
    // In the column the beat's own place rounds to, as lines and edges are (TimelineView::beatToColumn()): rounded where it is
    // in view, a start half a pixel in went one way at one step of a scroll and the other at the next.
    const auto y = startPosition.y < unscrolledHeight ? startPosition.y : startPosition.y + startScrollY - scrollYNow;
    return { view.beatToColumn (startBeat), y };
}

void TimelineGesture::beginStep (ProjectContext& context, const juce::String& name)
{
    if (step == nullptr)
        step = std::make_unique<SongGesture> (context, name);
}

void TimelineGesture::rollBack()
{
    if (step != nullptr)
        step->rollBack();

    step = nullptr;
}

bool TimelineGesture::doOnce (NoteRef note)
{
    if (std::find (doneNotes.begin(), doneNotes.end(), note) != doneNotes.end())
        return false;

    doneNotes.push_back (note);
    return true;
}
} // namespace phazite::ui
