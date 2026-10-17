// SPDX-License-Identifier: AGPL-3.0-or-later
// Whether a clip is an automation clip (Automation.h), for the parts of the model that automation itself uses, such as
// the mixer, which can't include Automation.h without a loop.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// A MIDI clip that holds an automation curve, which sets a parameter while it plays, rather than notes. It stays one when
// its parameter's device is gone. Asking reads its state, and makes no curve list.
inline bool isAutomationClip (const te::Clip& clip)
{
    return dynamic_cast<const te::MidiClip*> (&clip) != nullptr
           && clip.state.getChildWithName (te::IDs::AUTOMATIONCURVES).getChildWithName (te::IDs::AUTOMATIONCURVEMODIFIER).isValid();
}
} // namespace phazite
