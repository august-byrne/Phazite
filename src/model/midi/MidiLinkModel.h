// SPDX-License-Identifier: AGPL-3.0-or-later
// MIDI learn (docs/design/automation-and-modulation.md, "MIDI learn"): a knob or fader of a MIDI controller linked to a
// parameter, which it then turns as the parameter's control turns it, the link saved in the project, as Ableton Live, FL
// Studio, Bitwig and Reaper keep theirs. A controller, by its channel and number, turns one parameter, and a parameter has
// one link. Learning starts for one parameter and links the next controller moved on any MIDI input. For the UI, by
// handles and plain values. Message thread only.
#pragma once

#include "MidiLinkValues.h"
#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;
class ParameterHandle;
class ProjectContext;

// A link as the Project view lists it: its controller, what it turns ("Lead: Synth: Cutoff", as the automation lanes name a
// track, a device and a parameter), and what a click on it selects: the device it is of, or else the track.
struct MidiLinkInfo
{
    MidiController controller;
    juce::String turns;
    TrackID track;
    DeviceID device;

    bool operator== (const MidiLinkInfo&) const = default;
};

// The open song's links, by channel, then number; none while no project is open.
std::vector<MidiLinkInfo> getMidiLinks (const ProjectContext&);

// A control's parameter's link: its controller, or none. And whether its parameter can be learned: one the song has and
// saves (a device's, Phazite's or a plug-in's, a fader's volume and pan, a send's level, a chain's, a modulator's, a macro),
// which is there now (a send the first change makes, once it has).
std::optional<MidiController> getMidiLink (const ParameterHandle&);
bool canLearnMidi (const ParameterHandle&);

// Learning: started for a control's parameter (another learning stops), it links the next controller moved (0 to 119, on
// any input and channel), as "Link MIDI Controller", one undo step: that controller's link moves here, and this parameter's
// link, if it had one, goes. It ends then, when cancelled, when the parameter goes, or when another project opens; notes, the
// bend and pressure play on meanwhile. Not saved, and no undo step itself.
void startMidiLearn (const ParameterHandle&);
void cancelMidiLearn (ProjectContext&);
void cancelMidiLearn (const ParameterHandle&); // if it is the one learning
bool isLearningMidi (const ParameterHandle&);
bool isLearningMidi (const ProjectContext&);

// The name of the parameter learning now ("Cutoff"), for the window to say so; none while none learns.
std::optional<juce::String> getMidiLearnName (const ProjectContext&);

// What the last link learned made, as a sentence for the window ("CC 21 on channel 1 now turns Cutoff, instead of Volume."),
// once: none after it was taken, or before any.
std::optional<juce::String> takeMidiLinkMade (ProjectContext&);

// Removing a link, a step of its own, "Remove MIDI Link": a control's parameter's, or a controller's.
void removeMidiLink (const ParameterHandle&);
void removeMidiLink (ProjectContext&, MidiController);

// MIDI Learn for the parameter changed by hand last (AutomationModel.h: getLastTouchedParameter()), as Automate Last Touched
// finds it, in a plug-in's own window too; while one learns, the command cancels it. Its name: "MIDI Learn Cutoff", or
// "Cancel MIDI Learn" while one learns.
bool canLearnLastTouched (const ProjectContext&);
void learnLastTouched (ProjectContext&);
juce::String describeLearnLastTouched (const ProjectContext&);

// The command: MIDI Learn Last Touched Parameter (midi.learnLastTouched).
void addMidiLinkCommands (CommandRegistry&, ProjectContext&);
} // namespace phazite
