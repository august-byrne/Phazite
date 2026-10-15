// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's parameter as a control uses it (a knob, a fader): its value, range and default, and the changes to it,
// each one named undo step that this owns rather than the control (CLAUDE.md, "the UI decides nothing about the
// project"; roadmap phase A2). It finds its parameter by its plug-in and ID each time, so it is safe to keep after the
// plug-in is deleted: it then does nothing.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/Tracktion.h"

#include <memory>
#include <optional>

namespace phazite
{
class Gesture;

class ParameterHandle
{
public:
    ParameterHandle();

    // A plug-in's parameter by its ID; changes to it are undo steps named undoName ("Change Volume"), or "Change" and
    // the parameter's name.
    ParameterHandle (te::Plugin*, const juce::String& parameterID, const juce::String& undoName = {});

    ParameterHandle (ParameterHandle&&) noexcept;
    ParameterHandle& operator= (ParameterHandle&&) noexcept;
    ~ParameterHandle(); // ends a gesture in progress, keeping its change

    // Whether its parameter exists: none was given, or the plug-in has been deleted.
    bool isValid() const;

    // Whether it is the handle of that plug-in's parameter.
    bool refersTo (const te::Plugin*, const juce::String& parameterID) const;

    juce::String getName() const;
    juce::NormalisableRange<float> getRange() const; // with the parameter's skew and steps
    std::optional<float> getDefaultValue() const;

    // Its value now, automation and modulation included.
    float getValue() const;

    // How the plug-in itself writes a value ("-6.0 dB"), for plug-ins that aren't Phazite's own.
    juce::String getText (float value) const;

    // For a parameter of one of Phazite's own devices: its description (unit, choices), so that controls show it as
    // its unit says. Nothing for other plug-ins.
    std::optional<devices::ParameterInfo> getInfo() const;

    // Changing it. A drag is a gesture: beginGesture() starts its undo step, setValue() moves the value as often as
    // needed, and endGesture() keeps the change, all of it one step. A gesture begun while another is in progress for
    // the same project joins it, so a drag that moves two parameters at once (an EQ band's frequency and gain) is one
    // step, named after the first. setValue() outside a gesture (a key press, a double-click) is a step of its own.
    void beginGesture();
    void setValue (float);
    void endGesture();
    bool isInGesture() const;

private:
    te::SafeSelectable<te::Plugin> plugin;
    juce::String parameterID, undoName;
    std::unique_ptr<Gesture> gesture; // the gesture it began,
    bool joined = false;              // or whether it joined another change's

    te::AutomatableParameter::Ptr find() const;
};
} // namespace phazite
