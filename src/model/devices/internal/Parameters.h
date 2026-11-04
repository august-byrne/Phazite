// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's parameters by their references (model/devices/ParameterRef.h), as controls (ParameterHandle.h), automation and
// presets reach them: what each is, its value set by hand and the value it plays.
// A reference names one of a device's parameters (one of Phazite's own devices', by its ID in their description, or a
// plug-in's, by the plug-in's own ID for it), a channel's fader's volume or pan, a send's level (Schema.h: faderVolume to
// sendLevelParameter), or, by the modulator's or the macro's ID, a modulator's parameter or a macro's value (by the
// macro's ID as its parameter's). The song holds every value set by hand, but a plug-in's that no control set, which its
// instance holds (and the song stores as it saves). For the model, the app and tests. Message thread only.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/Song.h"
#include "model/devices/ParameterRef.h"

#include <optional>

namespace phazite
{
enum class ParameterSource
{
    device,    // one of Phazite's own devices' (DeviceInfo.h)
    plugin,    // a plug-in's, which its instance holds
    volume,    // a fader's volume (or a plain folder's VCA's), as the fader's position, from 0 to 1
    pan,       // a fader's pan, from -1 (left) to 1 (right)
    send,      // a send's level, as a fader's position
    modulator, // one of a modulator's parameters
    macro      // a track's or a container's macro's value, from 0 to 1
};

// What a parameter is: what it is of, its name, its range (with its skew and steps) and its default; and for one of Phazite's
// own devices' parameters, its description, so that controls show it as its unit says.
struct ParameterDescription
{
    ParameterSource source = ParameterSource::device;
    juce::String name;
    juce::NormalisableRange<float> range;
    std::optional<float> defaultValue; // none for a plug-in's that has none
    std::optional<devices::ParameterInfo> info;
};

// A parameter the song has (nothing for one it hasn't, or a device's parameter it hasn't), and how a value of it reads
// ("-6.00 dB", "Centre", "0.350").
std::optional<ParameterDescription> describeParameter (const Song&, const ParameterRef&);
juce::String getParameterText (const Song&, const ParameterRef&, float value);

// The value it is set to by hand, which automation and modulation move from; and setting it, in the caller's undo step,
// clipped to its range, which tells the song's listeners (Song::tellValueSetByHand()). A parameter whose curve has one
// point (model/automation/) plays that point's value everywhere, so its point moves to the value too, as the control's
// change.
float getValueSetByHand (const Song&, const ParameterRef&);
void setValueSetByHand (Song&, const ParameterRef&, float value);

// The value it plays now, its automation and its modulation included (or without its modulation): while Phazite's engine
// plays the song, the one it played last, or the one set by hand where nothing moves it; else the one set by hand.
float getCurrentValue (const Song&, const ParameterRef&, bool withModulation);

//==============================================================================
// A parameter found in the song, for what reads one often (a control, at each of the display ticker's ticks): what it is of,
// the element that holds its value set by hand, and for one of Phazite's own devices' parameters, its description. The
// functions above, given one, take it as found, which holds while its element is the song's and still names it
// (isStillThere()).
struct FoundParameter
{
    // Made by locateParameter() alone; a constructor of its own, so that a braced reference ({ device, "gain" }) is never
    // taken for one.
    FoundParameter (const ParameterRef& r, ParameterSource s, const juce::ValueTree& st, const devices::ParameterInfo* i)
        : ref (r),
          source (s),
          state (st),
          info (i)
    {
    }

    ParameterRef ref;
    ParameterSource source = ParameterSource::device;
    juce::ValueTree state;
    const devices::ParameterInfo* info = nullptr;
};

std::optional<FoundParameter> locateParameter (const Song&, const ParameterRef&);
bool isStillThere (const Song&, const FoundParameter&);

ParameterDescription describeParameter (const Song&, const FoundParameter&);
juce::String getParameterText (const Song&, const FoundParameter&, float value);
float getValueSetByHand (const Song&, const FoundParameter&);
void setValueSetByHand (Song&, const FoundParameter&, float value);
float getCurrentValue (const Song&, const FoundParameter&, bool withModulation);
} // namespace phazite
