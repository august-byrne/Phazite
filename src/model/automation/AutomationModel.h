// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation as the UI reaches it (docs/design.md, section 10; roadmap step 2.1), by IDs and plain values: the parameters
// a track's devices offer, the lanes the playlist shows under a track, the points on a lane and their edits, each track's
// write mode, and the parameter changed by hand last. Positions are song beats, and values are in the parameter's own
// range (ParameterHandle::getRange()). Each function takes the open project's context and gives nothing, or does
// nothing, for an ID of something that is gone. Edits are undo steps of their own, named as each says, unless a gesture is
// open (SongGesture.h): then they join its one step. The Tracktion side is internal/Automation.h.
#pragma once

#include "AutomationValues.h"
#include "model/devices/ParameterHandle.h"
#include "model/devices/ParameterRef.h"

#include <optional>
#include <vector>

namespace phazite
{
class CommandRegistry;
class ProjectContext;

//==============================================================================
// What a parameter's values stand for, so that a lane shows them as the parameter's control does.
enum class ParameterKind
{
    volume, // a fader's position from 0 to 1 (MixerModel.h: faderPositionToDecibels())
    pan,    // from -1 (left) to 1 (right)
    other   // the device writes them (ParameterHandle::getText()), or one of Phazite's devices by its unit (getInfo())
};

// A parameter that can be automated, with the names a menu or a lane shows for it.
struct AutomationTarget
{
    ParameterRef parameter {};
    TrackID track {};           // whose device it is: a track, a folder, a return or the master (getMasterTrack())
    juce::String deviceName {}; // "Synth", or "Mixer" for the track's own volume, pan and sends
    juce::String name {};       // "Cutoff"
    ParameterKind kind = ParameterKind::other;
    bool automated = false; // it has automation points
};

// The parameters of a track's devices that can be automated, in the mixer's order: its volume and pan, its sends'
// levels, then its instrument's and its effects' parameters. The master's: its volume, then its effects'.
std::vector<AutomationTarget> getAutomationTargets (const ProjectContext&, TrackID);
std::vector<AutomationTarget> getAutomationTargets (const ProjectContext&, DeviceID); // one device's, as its menu lists them
std::optional<AutomationTarget> getAutomationTarget (const ProjectContext&, const ParameterRef&);

// The parameter itself: its range, value and texts, and changing it as a control does.
ParameterHandle getParameter (ProjectContext&, const ParameterRef&);

// The master's track, whose lanes are the master's parameters': the playlist shows it under the tracks while it has
// lanes. An empty ID without a project.
TrackID getMasterTrack (const ProjectContext&);

//==============================================================================
// Lanes (AutomationValues.h): the parameters whose automation a track shows under it in the playlist, in their order. View
// state: saved, but written without undo steps, and not unsaved changes.
std::vector<AutomationLane> getAutomationLanes (const ProjectContext&, TrackID);

// Shows a parameter's lane under the track whose device it is, after the lanes it shows already; a lane it shows already
// stays where it is. Returns the track, or an empty ID for a parameter that isn't there.
TrackID showAutomationLane (ProjectContext&, const ParameterRef&);
void hideAutomationLane (ProjectContext&, const ParameterRef&);

// The lane shows another parameter of its track's devices instead, in its place (one shown already moves there).
void setAutomationLaneParameter (ProjectContext&, const ParameterRef& lane, const ParameterRef& replacement);
void setAutomationLaneHeight (ProjectContext&, const ParameterRef&, int height); // kept within the limits

// Shows a lane for each of a track's parameters that has automation, or hides them all.
void showAutomatedLanes (ProjectContext&, TrackID);
void hideAutomationLanes (ProjectContext&, TrackID);

//==============================================================================
// Points (AutomationValues.h).
std::vector<AutomationPoint> getAutomationPoints (const ProjectContext&, const ParameterRef&); // in time order

// The automated value at evenly spaced beats across a range (the first at its start, the last at its end), as the engine
// plays it, for drawing a lane; empty without points.
std::vector<float> getAutomationValues (const ProjectContext&, const ParameterRef&, BeatRange, int numValues);

// Edits. Indices are points' places in time order. Values are kept within the parameter's range, and beats at or after
// the song's start.

// Adds a point, after any at the same beat, as "Add Automation Point". Returns its index.
int addAutomationPoint (ProjectContext&, const ParameterRef&, AutomationPoint);

// Moves points (their indices in order) later by a distance (earlier, if negative) and up by an amount (down, if
// negative), as "Move Automation Points". They keep their order among the points that don't move, and their values stay in
// the parameter's range, so the distance and the amount are cut to fit. Returns the distance they moved.
BeatDuration moveAutomationPoints (ProjectContext&, const ParameterRef&, const std::vector<int>& indices, BeatDuration, float valueChange);

// Bends the line after a point (AutomationPoint::curve, -1 to 1), as "Bend Automation".
void setAutomationCurve (ProjectContext&, const ParameterRef&, int index, float curve);

void deleteAutomationPoints (ProjectContext&, const ParameterRef&, const std::vector<int>& indices); // "Delete Automation Points"

// Replaces the points in a range, its start and end included, with others, as drawing in a lane does: "Draw Automation".
void replaceAutomationPoints (ProjectContext&, const ParameterRef&, BeatRange, const std::vector<AutomationPoint>&);

void clearAutomation (ProjectContext&, const ParameterRef&); // all its points: "Clear Automation"

// The same for a control's parameter, as its menu reaches it (ui/components/Controls.h: ParameterAttachment): whether it
// has automation, showing its lane (Automate), and deleting its points (Clear Automation). Nothing for a handle of
// nothing.
bool isAutomated (const ParameterHandle&);
void showAutomationLane (const ParameterHandle&);
void clearAutomation (const ParameterHandle&);

//==============================================================================
// Write modes (AutomationValues.h): each track has one.
AutomationMode getAutomationMode (const ProjectContext&, TrackID);
void setAutomationMode (ProjectContext&, TrackID, AutomationMode); // not an undo step, as mute and solo aren't

//==============================================================================
// The parameter last changed by hand (not by automation) in the open project, in a plug-in's own window too, for Automate
// Last Touched: nothing before any, or once its device is gone.
std::optional<ParameterRef> getLastTouchedParameter (const ProjectContext&);

//==============================================================================
// What the automation commands do (addAutomationCommands()).

// Automate Last Touched: shows the lane of the parameter changed by hand last, "Automate Cutoff".
bool canAutomateLastTouched (const ProjectContext&);
void automateLastTouched (ProjectContext&);
juce::String describeAutomateLastTouched (const ProjectContext&);

// Show Automation: the selected tracks show a lane for each automated parameter (a track without automation, its
// volume's), or, while one of them shows lanes, none.
bool areTracksSelected (const ProjectContext&);
bool areAutomationLanesShown (const ProjectContext&); // a selected track shows lanes
void toggleAutomationLanes (ProjectContext&);

// The selected points (SongSelection): whether there are any, and moving them later by a distance (earlier, if negative)
// and up by a part of their parameter's range (1 is all of it; down, if negative), as "Move Automation Points".
bool hasSelectedAutomationPoints (const ProjectContext&);
void moveSelectedAutomationPoints (ProjectContext&, BeatDuration, float partOfRange);

// The selected tracks' write mode: whether each of them is in this one, and putting them all in it.
bool isAutomationModeOfSelection (const ProjectContext&, AutomationMode);
void setAutomationModeOfSelection (ProjectContext&, AutomationMode);

// The automation commands: Automate Last Touched Parameter (its key works over plug-in windows too), Show Automation,
// raising and lowering the selected points (Command::notePriority, so that the arrow keys move them while points are
// selected), and the selected tracks' write modes.
void addAutomationCommands (CommandRegistry&, ProjectContext&);
} // namespace phazite
