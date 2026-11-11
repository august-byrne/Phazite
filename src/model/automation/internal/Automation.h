// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation (model/automation/AutomationModel.h), for the model, the app and tests: parameters by their ParameterRefs
// (model/devices/internal/Parameters.h), the points of a parameter's curve in beats, which the song holds (Schema.h:
// AUTOMATION) and Phazite's engine plays (model/playback/internal/SongPlayer.h), the lanes a track shows (Schema.h: LANE),
// the selected points as Phazite's selection holds them, and the watcher that remembers the parameter touched last. Not
// for the UI.
#pragma once

#include "model/automation/AutomationValues.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"

#include <juce_events/juce_events.h>

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The track whose device, fader or send a parameter is: the master's ID for the master's fader and effects, and a
// container's track for its devices' and its chains'. None for others.
TrackID getParameterTrack (const Song&, const ParameterRef&);

// The parameters of a track's channel and devices that can be automated, in the mixer's order: its fader's volume and pan
// (a plain folder's VCA's volume), its sends, then its devices', after each container its chains', each one's volume and
// pan (and a band's crossover) then its devices' (AutomationModel.h: getAutomationTargets()): one of Phazite's own devices'
// in its description's order, a plug-in's as its instance lists those it lets be automated, by their own IDs (none while
// it doesn't load).
std::vector<ParameterRef> getAutomatableParameters (const Song&, TrackID);

// The track's own volume, pan and sends are the mixer's controls, not a device's.
bool isMixerControl (const Song&, const ParameterRef&);

// A parameter's name as menus, lanes and undo steps show it: "Volume", "Pan", "Send to Reverb", a container's chain's by
// the chain's name ("Chain 2 Volume"), or the device's own name for it. And the name of what it is of, as lanes show it:
// "Mixer" for the mixer's controls, a chain's container's name, or its device's.
juce::String getParameterName (const Song&, const ParameterRef&);
juce::String getParameterDeviceName (const Song&, const ParameterRef&);

//==============================================================================
// A parameter's curve as points in song beats, and edits of it. They change the song through its undo manager without
// beginning a step: they join the caller's.
std::vector<AutomationPoint> getPoints (const Song&, const ParameterRef&);
bool hasPoints (const Song&, const ParameterRef&);
std::vector<float> getValues (const Song&, const ParameterRef&, BeatRange, int numValues); // getAutomationValues()
std::vector<float> getValues (const Song&, const ParameterRef&, const std::vector<BeatPosition>&);
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

// A track's write mode, the song's (Schema.h: trackAutomationMode): set without an undo step.
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
// The song's curves (Schema.h: AUTOMATION): a parameter's, invalid for one without; every one; and the parameter each is
// for.
juce::ValueTree findCurveState (const Song&, const ParameterRef&);
std::vector<juce::ValueTree> getCurveStates (const Song&);
ParameterRef getCurveParameter (const juce::ValueTree& curveState);

// The time edits' automation (model/arrangement/internal/Arrangement.h), on every automated parameter of the song: room
// made at a beat, the points from there on moving later and the line holding its value at the beat across the room; a
// range taken out, what comes after moving back, and the line jumping from its value at the range's start to its value at
// the end; points where edges are (one just before each beat and one at it, at the value at the beat, so that each part
// keeps its shape); the points in a range, from its start up to its end; those taken out; and points added, each after
// any at its beat. They join the caller's undo step.
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
// Watches the song's values set by hand (Song::ParameterListener): remembers the parameter changed last by hand (by a
// control, or in a plug-in's own window), but not a modulator's or a macro's, which can't be automated; and as a plug-in
// sets one of its values itself, moves the one point of the parameter's curve to it, as a value set by a control moves it
// (model/devices/internal/Parameters.h: setValueSetByHand()), unless the automation writer writes the parameter
// (model/playback/internal/AutomationWriter.h). The project context keeps one for the open song.
class AutomationWatcher : private Song::ParameterListener,
                          private juce::AsyncUpdater
{
public:
    explicit AutomationWatcher (Song&);
    ~AutomationWatcher() override;

    ParameterRef getLastTouched() const { return lastTouched; }

private:
    static constexpr size_t maxTouchedAtOnce = 2; // an EQ band's dot moves two parameters at once

    Song::Ptr song;
    ParameterRef lastTouched;
    std::vector<ParameterRef> changedAtOnce; // in this round of messages

    void valueSetByHand (const ParameterRef&, float value, bool byPlugin) override;
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE (AutomationWatcher)
};

// Whether any of the song's tracks writes automation (a mode other than read).
bool isWritingAutomation (const Song&);

// Whether a property of an element of the song is where a parameter keeps its value set by hand: a property of a device's
// or a modulator's VALUES, a plug-in's VALUE's value, a channel's volume or pan, a send's level, or a macro's value. A knob
// turned, a fader moved: controls read values as they show them, so views needn't hear of these
// (model/project/ProjectContext.h).
bool isParameterValue (const juce::ValueTree&, const juce::Identifier& property);
} // namespace phazite
