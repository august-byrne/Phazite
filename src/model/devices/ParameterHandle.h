// SPDX-License-Identifier: AGPL-3.0-or-later
// A device's parameter as a control uses it (a knob, a fader): its value, range and default, and the changes to it,
// each one named undo step that this owns rather than the control (CLAUDE.md, "the UI decides nothing about the
// project"; roadmap phase A2). It finds its parameter each time, so it is safe to keep after the device is deleted: it
// then does nothing. The model hands handles out (model/mixer/MixerModel.h, DeviceHandle.h), which it makes of the
// song's parameters (internal/ParameterHandle.h).
#pragma once

#include "ParameterRef.h"
#include "devices/core/DeviceInfo.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>

namespace phazite
{
class ParameterHandle
{
public:
    ParameterHandle(); // of nothing
    ParameterHandle (ParameterHandle&&) noexcept;
    ParameterHandle& operator= (ParameterHandle&&) noexcept;
    ~ParameterHandle(); // ends a gesture in progress, keeping its change

    // Whether there is a parameter to control: one that exists (not if none was given, or its device has been deleted),
    // or one the first change makes, such as a track's send to a return before there is one (MixerModel.h:
    // getSendLevelTo()). exists() tells them apart: the parameter is there now.
    bool isValid() const;
    bool exists() const;

    // Whether both are handles of the same parameter of the same device.
    bool refersToSameParameter (const ParameterHandle&) const;

    // Its parameter by IDs (ParameterRef.h), for automation; an invalid one for a handle of nothing.
    ParameterRef getRef() const;

    juce::String getName() const;
    juce::NormalisableRange<float> getRange() const; // with the parameter's skew and steps
    std::optional<float> getDefaultValue() const;

    // Its value now, automation and modulation included; and without its modulation, which a control shows while a ring
    // around it shows the modulation (model/modulation/ModulationModel.h).
    float getValue() const;
    float getBaseValue() const;

    // How the device itself writes a value ("-6.0 dB"), for plug-ins that aren't Phazite's own: a plug-in's as its helper
    // wrote it last, asked again without waiting where it hasn't written that value (known bug 166), whenKnown called once, on
    // the message thread, as its answer comes (never for a text known already, nor for any but a plug-in's parameter).
    juce::String getText (float value) const;
    juce::String getText (float value, std::function<void()> whenKnown) const;

    // Reads a value as getText() writes it, or as someone might type it in its units ("-6 dB", "30% left", "1.2k", a choice's
    // name). Kept to its range; nothing if it can't be read, is no number (known bug 181), or for a parameter that isn't
    // there. A plug-in's isn't read by the first at all, and is read as the plug-in itself reads text by the second, which
    // asks its helper without waiting (known bug 166) and calls back once it has, however long that takes while the helper
    // lives (at once for any other parameter): with none if the song's history moved meanwhile (an undo, a redo, another
    // step begun) or the parameter has gone, as what it was typed for may be other then, or if the plug-in or its helper went
    // first. What changes the song as it plays (automation written, a take recorded) doesn't drop it. Never a number the
    // plug-in didn't give.
    std::optional<float> readText (const juce::String&) const;
    void readText (const juce::String&, std::function<void (std::optional<float>)> then) const;
    bool readsTextLater() const; // whether the second calls back later: a plug-in's parameter, its plug-in loaded

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

    // Takes back all the gesture changed, leaving nothing to redo: Escape during a drag, as in Ableton Live and most DAWs. A
    // gesture that joined another's (above) only ends: that one's owner keeps or takes back the whole step.
    void cancelGesture();

    struct Impl; // the device and the parameter's ID (internal/ParameterHandle.h makes handles)
    explicit ParameterHandle (std::unique_ptr<Impl>);
    const Impl* getImpl() const noexcept { return impl.get(); } // nullptr for a handle of nothing

private:
    std::unique_ptr<Impl> impl;

    void makeAndSet (float); // a parameter the first change makes: makes it, then sets it
};
} // namespace phazite
