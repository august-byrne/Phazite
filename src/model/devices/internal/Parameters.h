// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's parameters by their references (model/devices/ParameterRef.h), as controls (ParameterHandle.h), automation and
// presets reach them: what each is, its value set by hand and the value it plays.
// A reference names one of a device's parameters (one of Phazite's own devices', by its ID in their description, or a
// plug-in's, by the plug-in's own ID for it), a channel's fader's volume or pan, a send's level (Schema.h: faderVolume to
// sendLevelParameter), by a container's chain's ID its volume, pan or crossover (chainVolumeParameter to
// crossoverParameter), or, by the modulator's or the macro's ID, a modulator's parameter or a macro's value (by the
// macro's ID as its parameter's). The song holds every value set by hand, but a plug-in's that no control set, or that one
// set back to its default, which its instance holds (and the song stores as it saves). For the model, the app and tests.
// Message thread only.
#pragma once

#include "devices/core/DeviceInfo.h"
#include "model/base/Song.h"
#include "model/base/internal/SavedValues.h"
#include "model/devices/ParameterRef.h"

#include <functional>
#include <optional>

namespace phazite
{
class AutomationWriter;

enum class ParameterSource
{
    device,      // one of Phazite's own devices' (DeviceInfo.h)
    plugin,      // a plug-in's, which its instance holds
    volume,      // a fader's volume (or a plain folder's VCA's), as the fader's position, from 0 to 1
    pan,         // a fader's pan, from -1 (left) to 1 (right)
    send,        // a send's level, as a fader's position
    modulator,   // one of a modulator's parameters
    macro,       // a track's or a container's macro's value, from 0 to 1
    chainVolume, // a container's chain's volume, as a fader's position, from 0 to 1
    chainPan,    // a container's chain's pan, from -1 (left) to 1 (right)
    crossover    // where the band of a chain of a container split into bands starts, in hertz (not the first chain's)
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

// The travel of a parameter's control, its scale and steps (devices::Travel), which its automation's points are positions
// along (model/automation/AutomationValues.h) and Phazite's engine plays them by: one of Phazite's own devices' by its
// description; a plug-in's, any parameter of a plug-in's element, from 0 to 1, in the steps its instance says it has
// once loaded; a chain's crossover's whether or not its container is split into bands now, so that what its automation
// plays doesn't hang on that; the mixer's controls' by their ranges. Nothing for a parameter the song hasn't.
std::optional<devices::Travel> getControlTravel (const Song&, const ParameterRef&);

// The value it is set to by hand, which automation and modulation move from; and setting it, in the caller's undo step,
// clipped to its range, which tells the song's listeners (Song::tellValueSetByHand()). A parameter whose curve has one
// point (model/automation/) plays that point's value everywhere, so its point moves to the value too, as the control's
// change.
float getValueSetByHand (const Song&, const ParameterRef&);
void setValueSetByHand (Song&, const ParameterRef&, float value);

// The name of the undo step of a change of it by hand, by a control (ParameterHandle.h) or a MIDI controller's knob
// (model/midi/internal/MidiLinks.h): "Change Volume", "Change Master Volume", "Change Pan", "Change Send Level", "Change
// Chain 2 Volume", a macro's or another parameter's by its name ("Change Filter Cutoff"). None for one the song hasn't.
juce::String getChangeName (const Song&, const ParameterRef&);

// While one lives, the values set by hand are a change of many at once (a preset loaded or tried, a sampler's sound with
// its settings), which writes no automation: a track in Latch or Write punches in for a control moved by itself
// (model/playback/internal/AutomationWriter.h), not for each of these. Message thread only.
class ManyValuesSetByHand
{
public:
    explicit ManyValuesSetByHand (const Song&);
    ~ManyValuesSetByHand();

private:
    AutomationWriter* writer = nullptr; // the song's session's, if one plays it

    JUCE_DECLARE_NON_COPYABLE (ManyValuesSetByHand)
};

// The value it plays now, its automation and its modulation included (or without its modulation): while Phazite's engine
// plays the song, the one it played last, or the one set by hand where nothing moves it; else the one set by hand.
float getCurrentValue (const Song&, const ParameterRef&, bool withModulation);

//==============================================================================
// A parameter found in the song, for what reads one often (a control, at each of the display ticker's ticks): what it is of,
// its element (state), and for one of Phazite's own devices' parameters, its description. The element is the one that holds
// its value set by hand: a device's or a plug-in's DEVICE, a fader's channel (its TRACK or the MASTER), a container's chain's
// CHAIN; none for a modulator's parameter and a macro's value, which are looked for by their modulator's ID in modulation's
// area (model/modulation/internal/Modulation.h: getModulatorValue()), the modulator's still of the kind it was found of, nor
// for a send's level, looked for by its send's ID in the sends' area (model/mixer/internal/Mixer.h: getSendValue()), one look
// at each tick. The functions above, given one, take it as found, which holds while its element is the song's and still
// names it, and for a modulator, a macro or a send, while its area holds it so (isStillThere()).
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

// The half of locateParameter() that looks in the song's tree alone, which it asks after its looks in modulation's area and,
// for a send's level, the sends': the sources the tree holds (a device's, a plug-in's, a chain's and a fader's parameters);
// none for a modulator's parameter, a macro's value and a send's level. What a tree callback asks, which reads no area of the
// song's values (MidiLinker's, model/midi/internal/MidiLinks.h).
std::optional<FoundParameter> locateParameterInTree (const Song&, const ParameterRef&);

bool isStillThere (const Song&, const FoundParameter&);

ParameterDescription describeParameter (const Song&, const FoundParameter&);

// How a value reads; a plug-in's as its helper wrote it last, asked again without waiting (SandboxedInstance::describeValue():
// whenKnown called as its answer comes).
juce::String getParameterText (const Song&, const FoundParameter&, float value, std::function<void()> whenKnown = {});
float getValueSetByHand (const Song&, const FoundParameter&);
void setValueSetByHand (Song&, const FoundParameter&, float value);
float getCurrentValue (const Song&, const FoundParameter&, bool withModulation);

//==============================================================================
// A value of a parameter of one of Phazite's own devices, kept in its element's VALUES (Schema.h), in that
// undo history: none at the parameter's default, and no VALUES left without values, so that a value set back to its default
// leaves the song as it was, no change in source control nor unsaved. Every writer of VALUES goes through it. (Whether a
// value is the default, and its saved form: model/base/internal/SavedValues.h, isDefaultValue(), toSavedValue(),
// readSavedValue().)
void keepValueSetByHand (juce::ValueTree element, const devices::ParameterInfo&, float value, juce::UndoManager*);

// The value set by hand of a parameter of an element (a device's), as its VALUES keep it, or its default.
// For the presets' reader alone, which the devices' next piece moves (roadmap step S5.20): a device's readers read it through
// the devices' conversion (model/base/internal/DevicesValue.h: readDeviceParameterValue()), and keepValueSetByHand() is the
// devices' writers' until their own piece.
float readValueSetByHand (const juce::ValueTree& element, const devices::ParameterInfo&);
} // namespace phazite
