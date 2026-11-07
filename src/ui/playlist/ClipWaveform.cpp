// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipWaveform.h"

#include <utility>

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

std::unique_ptr<ClipWaveform> makeClipWaveform (ProjectContext& context, ClipID clip, std::function<void()> loaded)
{
    const auto& maker = getMaker();
    return maker != nullptr ? maker (context, clip, std::move (loaded)) : nullptr;
}
} // namespace phazite::ui
