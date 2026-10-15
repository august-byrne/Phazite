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

void TimelineGesture::beginStep (te::Edit& edit, const juce::String& name)
{
    if (step == nullptr)
        step = std::make_unique<phazite::Gesture> (edit, name);
}

bool TimelineGesture::doOnce (const juce::ValueTree& thing)
{
    if (std::find (done.begin(), done.end(), thing) != done.end())
        return false;

    done.push_back (thing);
    return true;
}
} // namespace phazite::ui
