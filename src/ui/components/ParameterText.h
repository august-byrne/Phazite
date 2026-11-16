// SPDX-License-Identifier: AGPL-3.0-or-later
// A parameter's value as its automation lane, the inspector and the field typed in over an automation point show it, and a
// value typed for it read back: a fader's volume in decibels ("-6.0 dB"), a pan ("L20"), one of Phazite's devices' parameters
// in its units ("2.50 kHz", "40%"), and a plug-in's as the plug-in writes and reads it, which its helper is asked without
// waiting (known bug 166).
#pragma once

#include "model/automation/AutomationModel.h"
#include "model/devices/ParameterHandle.h"
#include "model/project/ProjectContext.h"

#include <functional>
#include <memory>
#include <optional>

namespace phazite::ui
{
// A plug-in's as its helper wrote it last, whenKnown called (once) as the text asked for now comes (ParameterHandle::getText()).
juce::String formatParameterText (const ParameterHandle&, ParameterKind, float value, std::function<void()> whenKnown = {});

// The value a text typed for the parameter means, in the parameter's own terms (a fader's position, a pan from -1 to 1);
// nothing for text that names none. The first reads no plug-in's (the plug-in reads its own); the second reads a plug-in's
// as the plug-in reads it, calling back once it has (at once for any other: ParameterHandle::readText()).
std::optional<float> parseParameterText (const ParameterHandle&, ParameterKind, const juce::String&);
void parseParameterText (const ParameterHandle&, ParameterKind, const juce::String&, std::function<void (std::optional<float>)> then);

// A value typed for a parameter, read as its device reads it (ParameterHandle::readText()): a plug-in's, which its helper
// reads without the message thread waiting, for as long as that takes while the helper lives, with the window's one hold
// held meanwhile, so that nothing is changed under the value typed before it is set: the hold for work the app sets
// (setTypedValueHold()), whose progress window shows "Waiting for the plug-in to read the value" after a second, and whose
// Escape (before that window, or its Cancel) gives the wait up, the value dropped and an answer after that too. Dropped
// (none) as well if the song's history moved meanwhile, the parameter went, or the plug-in or its helper went (the
// reviews of known bug 166). Any other at once.
void readTypedValue (const ParameterHandle&, const juce::String&, std::function<void (std::optional<float>)> then);

// What holds the window while a plug-in reads a value typed: the open project's hold for work (ProjectContext::holdForWork(),
// the window's one hold, src/app/InputHold.h), which the app sets as its main window opens, and clears as it goes; as a
// scoped token, let go of as the value is read or given up. Without one (most tests), nothing holds. Message thread.
using TypedValueHold = std::function<std::unique_ptr<ProjectContext::WorkHold> (const juce::String& title, ProjectContext::Cancel)>;
void setTypedValueHold (TypedValueHold);
} // namespace phazite::ui
