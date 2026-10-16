// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipWaveform.h"

namespace phazite::ui
{
namespace
{
    ClipWaveformMaker& getMaker()
    {
        static ClipWaveformMaker maker;
        return maker;
    }
} // namespace

void setClipWaveformMaker (ClipWaveformMaker maker)
{
    getMaker() = std::move (maker);
}

std::unique_ptr<ClipWaveform> makeClipWaveform (ProjectContext& context, ClipID clip, juce::Component& toRepaint)
{
    const auto& maker = getMaker();
    return maker != nullptr ? maker (context, clip, toRepaint) : nullptr;
}
} // namespace phazite::ui
