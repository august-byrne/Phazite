// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's editor in its helper process as Phazite's window shows it (docs/tracktion-migration.md, T3 step 5): an
// editor of JUCE's for the plug-in's instance (model/plugins/internal/SandboxedInstance.h), which a plug-in's window
// holds as it would the plug-in's own (PluginWindow.h). The helper's window for the plug-in's editor is a child of the
// window this is in, which the helper places over it, again as it moves or is resized; the two are as large as the
// plug-in makes its editor, and the plug-in's as large as this, where the plug-in can be resized. Phazite never calls into
// the helper's window, so that a helper that hangs can't hang Phazite's window with it: everything goes through the
// control channel. The keys Phazite takes over a plug-in (ui/input/PluginKeys.h) the helper hands back, which run here,
// unless the window's plug-in gets every key; the helper is told which they are, and again as they change (the computer
// keyboard switched, Send All Keys to the Plug-in). A plug-in whose helper has gone shows nothing of its own.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace phazite
{
class PluginHost;
class SandboxedInstance;

// The editor of a plug-in in a helper, or nullptr for one that makes none; and the app's, given to the plug-in host for
// the instances it makes from now on (the app's services do as they start, and the tests' main).
juce::AudioProcessorEditor* createSandboxedEditor (SandboxedInstance&);
void useSandboxedEditors (PluginHost&);
} // namespace phazite
