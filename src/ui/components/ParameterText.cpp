// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterText.h"
#include "ValueText.h"
#include "model/mixer/MixerModel.h"

namespace phazite::ui
{
juce::String formatParameterText (const ParameterHandle& handle, ParameterKind kind, float value)
{
    switch (kind)
    {
        case ParameterKind::volume:
            return formatDecibels (faderPositionToDecibels (value));
        case ParameterKind::pan:
            return formatPan (value);
        case ParameterKind::other:
        default:
            if (const auto info = handle.getInfo())
                return formatParameterValue (*info, value);

            return handle.getText (value);
    }
}

std::optional<float> parseParameterText (const ParameterHandle& handle, ParameterKind kind, const juce::String& text)
{
    switch (kind)
    {
        case ParameterKind::volume:
        {
            const auto dB = parseDecibels (text);
            return dB.has_value() ? std::optional (decibelsToFaderPosition ((float) *dB)) : std::nullopt;
        }

        case ParameterKind::pan:
        {
            const auto pan = parsePan (text);
            return pan.has_value() ? std::optional ((float) *pan) : std::nullopt;
        }

        case ParameterKind::other:
        default:
            if (const auto info = handle.getInfo())
                return parseParameterValue (*info, text);

            return handle.readText (text); // as the plug-in reads it
    }
}
} // namespace phazite::ui
