// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterText.h"
#include "ValueText.h"
#include "model/mixer/MixerModel.h"

#include <memory>
#include <utility>

namespace phazite::ui
{
juce::String formatParameterText (const ParameterHandle& handle, ParameterKind kind, float value, std::function<void()> whenKnown)
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

            return handle.getText (value, std::move (whenKnown));
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

            return handle.readText (text); // a plug-in's as a number
    }
}

void parseParameterText (const ParameterHandle& handle, ParameterKind kind, const juce::String& text,
                         std::function<void (std::optional<float>)> then)
{
    if (then == nullptr)
        return;

    // A plug-in's as the plug-in reads it, asked of its helper without waiting; any other at once.
    if (kind == ParameterKind::other && ! handle.getInfo().has_value())
        readTypedValue (handle, text, std::move (then));
    else
        then (parseParameterText (handle, kind, text));
}

namespace
{
    TypedValueHold& getTypedValueHold()
    {
        static TypedValueHold hold;
        return hold;
    }
} // namespace

void setTypedValueHold (TypedValueHold hold)
{
    getTypedValueHold() = std::move (hold);
}

void readTypedValue (const ParameterHandle& handle, const juce::String& text, std::function<void (std::optional<float>)> then)
{
    if (then == nullptr)
        return;

    if (! handle.readsTextLater())
    {
        handle.readText (text, std::move (then));
        return;
    }

    // Held until the plug-in's answer comes, however long it takes while its helper lives (never a number it didn't give:
    // the reviews of known bug 166), with the window's one hold, whose progress window shows the wait after a second; its
    // Escape or Cancel gives it up, the value dropped, the old text shown again, and an answer that comes after that dropped
    // too. Called back once.
    struct Wait
    {
        std::unique_ptr<ProjectContext::WorkHold> hold;
        std::function<void (std::optional<float>)> then;

        void finish (std::optional<float> value)
        {
            hold = nullptr; // the window's input its own again first

            if (const auto call = std::exchange (then, nullptr))
                call (value);
        }
    };

    auto wait = std::make_shared<Wait>();
    wait->then = std::move (then);

    if (const auto& holdFor = getTypedValueHold(); holdFor != nullptr)
        wait->hold = holdFor ("Waiting for the plug-in to read the value. Press Esc to stop waiting.",
                              [weak = std::weak_ptr<Wait> (wait)] (const juce::String&)
                              {
                                  if (const auto waiting = weak.lock())
                                      waiting->finish (std::nullopt);
                              });

    handle.readText (text, [wait] (std::optional<float> value) { wait->finish (value); });
}
} // namespace phazite::ui
