// SPDX-License-Identifier: AGPL-3.0-or-later
// Making ParameterHandles (model/devices/ParameterHandle.h) of the song's parameters (Parameters.h), for the model, the app
// and tests. Not for the UI, which gets handles from the model's functions by IDs.
#pragma once

#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/devices/ParameterRef.h"

#include <functional>

namespace phazite
{
class ParameterHandle;

// A parameter of the song's by its reference; changes to it are undo steps named undoName, or, without one, as every change
// of the parameter by hand is (Parameters.h: getChangeName(), "Change Volume"). A modulator's parameter or a macro's value isn't one automation can name, so its handle has no
// reference to give (getRef()), with automatable false.
ParameterHandle makeParameterHandle (Song&, const ParameterRef&, const juce::String& undoName = {}, bool automatable = true);

// A parameter of a device that a track may not hold yet, found each time with locate(), which the first change makes with
// create(), in that change's undo step, named createName (a drag's first move, or a value set outside a drag): a send to a
// return, which each strip offers before there is one (model/mixer/MixerModel.h: getSendLevelTo()). Until then its value
// is the start of the range given. Later changes are undo steps named undoName, or as makeParameterHandle()'s. identity tells it from handles of other
// devices that aren't there either.
ParameterHandle makeCreatingHandle (Song&, std::function<DeviceID (const Song&)> locate, std::function<DeviceID (Song&)> create,
                                    const juce::String& identity, const juce::String& parameterID, juce::NormalisableRange<float>,
                                    const juce::String& createName, const juce::String& undoName);

// The song whose parameter a handle changes, or nullptr for a handle of nothing or once the song is gone.
Song* findSong (const ParameterHandle&);

// Its parameter by IDs as the song has it now, whatever it is (a modulator's and a macro's too, which getRef() gives none
// of): invalid for a handle of nothing, or of a parameter that isn't there (yet).
ParameterRef findParameterRef (const ParameterHandle&);
} // namespace phazite
