// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Schema.h"

namespace phazite
{
juce::ValueTree getPhaziteState (te::Edit& edit)
{
    return edit.state.getOrCreateChildWithName (schema::PHAZITE, nullptr);
}

juce::ValueTree findPhaziteState (const te::Edit& edit)
{
    return edit.state.getChildWithName (schema::PHAZITE);
}
} // namespace phazite
