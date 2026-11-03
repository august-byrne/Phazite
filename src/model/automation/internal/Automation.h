// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation (model/automation/AutomationModel.h), for the model, the app and tests: parameters by their ParameterRefs
// (model/devices/internal/Parameters.h), the points of a parameter's curve in beats, which the song holds (Schema.h:
// AUTOMATION) and Tracktion's curves mirror, the lanes a track shows (Schema.h: LANE), the selected points as Phazite's selection holds
// them, the watcher that remembers the parameter touched last and gives recorded automation an undo step of its own. Not
// for the UI.
#pragma once

#include "model/automation/AutomationValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"
#include "model/base/internal/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The track whose device, fader or send a parameter is: the master's ID for the master's fader and effects, and a
// container's track for its devices'. None for others.
TrackID getParameterTrack (const Song&, const ParameterRef&);

// The parameters of a track's channel and devices that can be automated, in the mixer's order: its fader's volume and pan
// (a plain folder's VCA's volume), its sends, then its devices', those of the devices in its containers after each
// container (AutomationModel.h: getAutomationTargets()).
std::vector<ParameterRef> getAutomatableParameters (const Song&, TrackID);

// The track's own volume, pan and sends are the mixer's controls, not a device's.
bool isMixerControl (const Song&, const ParameterRef&);

// A parameter's name as menus, lanes and undo steps show it: "Volume", "Pan", "Send to Reverb", or the device's own name
// for it.
juce::String getParameterName (const Song&, const ParameterRef&);

//==============================================================================
// A parameter's curve as points in song beats, and edits of it. They change the song through its undo manager without
// beginning a step: they join the caller's. Each keeps Tracktion's parameter's value at the playhead in step with the
// curve while the song isn't playing.
std::vector<AutomationPoint> getPoints (const Song&, const ParameterRef&);
bool hasPoints (const Song&, const ParameterRef&);
std::vector<float> getValues (const Song&, const ParameterRef&, BeatRange, int numValues); // getAutomationValues()
int addPoint (Song&, const ParameterRef&, AutomationPoint);
BeatDuration movePoints (Song&, const ParameterRef&, const std::vector<int>& indices, BeatDuration, float valueChange);
void setCurve (Song&, const ParameterRef&, int index, float curve);
void deletePoints (Song&, const ParameterRef&, const std::vector<int>& indices);
void replacePoints (Song&, const ParameterRef&, BeatRange, const std::vector<AutomationPoint>&);

// Copies points right after themselves, as Duplicate does: each copy as far after its original as the points span (a
// grid step at least, for one point), replacing the points where they land. Returns the copies' indices.
std::vector<int> duplicatePoints (Song&, const ParameterRef&, const std::vector<int>& indices, BeatDuration minimumSpan);

//==============================================================================
// Automation that goes with the clips above it (docs/design.md, section 10), as Ableton Live, Bitwig and Studio One move
// automation with clips: the points of a track's parameters in a stretch of the song, moved or copied by a distance (later,
// or earlier if negative), replacing what is where they land. A stretch keeps its shape at its edges wherever it goes, and
// what lies around it keeps its own; where a moved stretch was, the line runs straight across. A track's stretches that
// overlap or touch go as one. They join the caller's undo step.
struct AutomationStretch
{
    TrackID track {};
    BeatRange range {};
};

void moveAutomation (Song&, const std::vector<AutomationStretch>&, BeatDuration distance, bool copy);

// Deletes the automation in stretches, as deleting time does, the parts on either side keeping their shape; with closeGap,
// what comes after a stretch moves back by its length, as the track's clips do (arrangement/internal/Clips.h:
// deleteTimeRange()).
void deleteAutomation (Song&, const std::vector<AutomationStretch>&, bool closeGap);

// The automation in stretches as it is now, for Paste to put back later (arrangement/internal/Clipboard.h): for each
// automated parameter of a stretch's track, a stretch's points as moveAutomation() takes them, with its values at its
// edges, from its start; and, pasted, the same a distance from where it was, in place of what lies there, what lies
// around it keeping its shape. A parameter that is gone by then is left out. Pasting joins the caller's undo step.
struct AutomationCopy
{
    ParameterRef parameter {};
    BeatDuration length {};
    std::vector<AutomationPoint> points {}; // their beats from the stretch's start
    BeatPosition start {};                  // where the stretch was
};

std::vector<AutomationCopy> copyAutomation (const Song&, const std::vector<AutomationStretch>&);
void pasteAutomation (Song&, const std::vector<AutomationCopy>&, BeatDuration distance);

//==============================================================================
// The lanes a track shows (getAutomationLanes()), the master's too: those it keeps (Schema.h: LANE), of devices on it,
// then one for each of its parameters with automation in the song that it keeps none for, so that automation never
// hides. Changes to them are written without undo steps, and keep a lane for each of those first, so that the lanes stay
// in the order shown.
std::vector<AutomationLane> getLanes (const Song&, TrackID);
void showLane (Song&, TrackID, const ParameterRef&);   // after the others, or where it is; expanded
void removeLane (Song&, TrackID, const ParameterRef&); // one for a parameter with automation shows again, after the others
void replaceLane (Song&, TrackID, const ParameterRef& lane, const ParameterRef& replacement);
void setLaneHeight (Song&, TrackID, const ParameterRef&, int height);
void setLaneCollapsed (Song&, TrackID, const ParameterRef&, bool);

// After a part of the song was copied with new IDs (giveNewIDs(), Schema.h): each copied device gets its original's
// automation, in the caller's undo step.
void copyCurves (Song&, const CopiedIDs&);

// A track's write mode, the song's (Schema.h: trackAutomationMode), which Tracktion's track mirrors: set without an undo
// step.
AutomationMode getAutomationMode (const Song&, TrackID);
void setAutomationMode (Song&, TrackID, AutomationMode);

//==============================================================================
// Automation points selected: one parameter's, by their indices, as the selection holds them (Selection: getPoints()), so
// that selecting anything else deselects them, and the selection commands (arrangement/internal/Selection.h) find them.
struct PointSelection
{
    ParameterRef parameter;
    std::vector<int> indices; // of points that are there, in order
};

// The selected points of a parameter that is there, those still there, if points are what is selected; and selecting
// points of a parameter, those there (deselecting anything else, or all of them with none).
std::optional<PointSelection> getSelectedPoints (const Selection&);
void selectPoints (Selection&, const ParameterRef&, const std::vector<int>& indices);

//==============================================================================
// The song's curves (Schema.h: AUTOMATION; T1 step 4), which Tracktion's curves mirror (model/mirror/internal/Mirror.h):
// a parameter's, invalid for one without; every one; and the parameter each is for.
juce::ValueTree findCurveState (const Song&, const ParameterRef&);
std::vector<juce::ValueTree> getCurveStates (const Song&);
ParameterRef getCurveParameter (const juce::ValueTree& curveState);

// The time edits' automation (model/arrangement/internal/Arrangement.h), on every automated parameter of the song: room
// made at a beat, as Tracktion's insertSpace() makes it; a range taken out, what comes after moving back, as its
// removeRegionAndCloseGap() does; points where edges are (one just before each beat and one at it, at the value at the
// beat, so that each part keeps its shape); the points in a range, from its start up to its end; those taken out; and
// points added, each after any at its beat. They join the caller's undo step.
struct ParameterPoint
{
    ParameterRef parameter;
    AutomationPoint point;
};

void insertAutomationTime (Song&, BeatPosition, BeatDuration);
void removeAutomationTime (Song&, BeatRange);
void addAutomationEdges (Song&, const std::vector<BeatPosition>&);
std::vector<ParameterPoint> getAutomationPointsIn (Song&, BeatRange);
void removeAutomationPointsIn (Song&, BeatRange);
void addAutomationPoints (Song&, const std::vector<ParameterPoint>&);

// Forgets the curves of devices the song hasn't any more (the song keeps them while an undo can bring the device back), as
// a project opens: without an undo step. A device keeps those of parameters it hasn't, as a plug-in that isn't installed.
void forgetCurvesOfGoneDevices (Song&);

//==============================================================================
// Watches an open Edit's automation: remembers the parameter last changed by hand (not by its automation), and takes
// into the song a curve's one point that Tracktion moved as a control turned. Touch, latch and write are Phazite's
// engine's (model/playback/internal/AutomationWriter.h): Tracktion's transport plays no project, so it records nothing.
class AutomationWatcher : private te::ParameterChangeHandler::Listener,
                          private juce::AsyncUpdater
{
public:
    explicit AutomationWatcher (te::Edit&);
    ~AutomationWatcher() override;

    ParameterRef getLastTouched() const { return lastTouched; }

private:
    static constexpr size_t maxTouchedAtOnce = 2; // an EQ band's dot moves two parameters at once

    te::SafeSelectable<te::Edit> edit; // the context can outlive it
    ParameterRef lastTouched;
    std::vector<ParameterRef> changedAtOnce; // in this round of messages

    void pluginParameterChanged (te::AutomatableParameter&, bool isFollowingAutomation) override;
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE (AutomationWatcher)
};

// Whether any of the song's tracks writes automation (a mode other than read).
bool isWritingAutomation (const Song&);

// Whether a property of a device's state is where one of its parameters keeps its value: a knob turned, a fader moved.
// Controls read values as they show them, so views needn't hear of these (model/project/ProjectContext.h).
bool isParameterValue (const juce::ValueTree&, const juce::Identifier& property);

// Puts back, in a saved project's state before its Edit is made, the values its parameters were set to by hand where they
// were saved while something else set them to others (a modulator): Tracktion saves those in the device's state too, and
// puts them back for plug-ins, but not for Phazite's devices, which would then start at the values played.
void restoreValuesSetByHand (juce::ValueTree& editState);

//==============================================================================
// The same by Tracktion's parameters and tracks, for the code that still holds them (the automation writer, the mirror,
// presets, modulation, tests) until it moves to references (docs/tracktion-migration.md, T6 step 4): the parameter a
// reference names, or nullptr; a device's parameter's reference, an invalid one for one that isn't a device's (a
// macro's, a modulator's); and the rest as above.
te::AutomatableParameter::Ptr findParameter (const te::Edit&, const ParameterRef&);
ParameterRef getParameterRef (const te::AutomatableParameter&);
te::Track* getTrackOfParameter (const te::AutomatableParameter&);
juce::String getParameterName (const te::AutomatableParameter&);
std::vector<AutomationPoint> getPoints (const te::AutomatableParameter&);
bool hasPoints (const te::AutomatableParameter&);
int addPoint (te::AutomatableParameter&, AutomationPoint);
juce::ValueTree findCurveState (const te::Edit&, const ParameterRef&);
std::vector<juce::ValueTree> getCurveStates (const te::Edit&);
AutomationMode getAutomationMode (const te::Track&);
void setAutomationMode (te::Track&, AutomationMode);
bool isWritingAutomation (const te::Edit&);

// Takes what Tracktion writes into its curves itself into the song: the one point of a curve, which it moves to its
// parameter's value as the value changes through its parameter (a plug-in's own window), in the change's step.
void adoptMovedPoint (te::AutomatableParameter&);
} // namespace phazite
