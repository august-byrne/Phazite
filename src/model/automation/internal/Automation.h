// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation (model/automation/AutomationModel.h), for the model, the app and tests: parameters by their ParameterRefs
// (model/devices/internal/Parameters.h), the points of a parameter's curve in beats, which the song holds (Schema.h:
// AUTOMATION) and Phazite's engine plays (model/playback/internal/SongPlayer.h), the lanes a track shows (Schema.h: LANE),
// the selected points as Phazite's selection holds them, and the watcher that remembers the parameter touched last. Not
// for the UI.
#pragma once

#include "model/automation/AutomationValues.h"
#include "model/base/Song.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/Selection.h"

#include <juce_events/juce_events.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
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
// A parameter's curve as points in song beats, each at a position along the parameter's control's travel
// (AutomationValues.h), and edits of it. What reads it reads its value (getCurveValue(), below); the edits change the song
// through its undo manager without beginning a step: they join the caller's.
std::vector<AutomationPoint> getPoints (const Song&, const ParameterRef&);
bool hasPoints (const Song&, const ParameterRef&);
std::vector<float> getPositions (const Song&, const ParameterRef&, BeatRange, int numValues); // getAutomationPositions()
std::vector<float> getPositions (const Song&, const ParameterRef&, const std::vector<BeatPosition>&);
int addPoint (Song&, const ParameterRef&, AutomationPoint);
BeatDuration movePoints (Song&, const ParameterRef&, const std::vector<int>& indices, BeatDuration, float positionChange);
void setCurve (Song&, const ParameterRef&, int index, float curve);

// The same bend given to several points of a curve, in the order given (an index the curve hasn't is left out), the curve
// read and written once, however many they are: the same changes as setCurve() for each, one after another.
void setCurves (Song&, const ParameterRef&, const std::vector<int>& indices, float curve);

void deletePoints (Song&, const ParameterRef&, const std::vector<int>& indices);
void replacePoints (Song&, const ParameterRef&, BeatRange, const std::vector<AutomationPoint>&);

// Copies points right after themselves, as Duplicate does: each copy as far after its original as the points span (a
// grid step at least, for one point), replacing the points where they land. Returns the copies' indices.
std::vector<int> duplicatePoints (Song&, const ParameterRef&, const std::vector<int>& indices, BeatDuration minimumSpan);

// A parameter whose curve has one point plays that point's position everywhere: the point moves to a value set by hand, in
// the parameter's own units (model/devices/internal/Parameters.h: setValueSetByHand()), in the caller's undo step, as the
// control's change. Not while the automation writer writes the parameter (model/playback/internal/AutomationWriter.h), which
// writes its own points.
void moveOnlyPoint (Song&, const ParameterRef&, float value);

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
// automation (setCurveValue(), below), in the caller's undo step, after the song's other curves, in their order; a copied device
// that has a curve of the parameter already keeps its own. A copied macro's value (named by the macro's own ID) is its copy's,
// named by the copy's: so the copied IDs hold the macros' (Modulation.h: copyModulation()).
void copyCurves (Song&, const CopiedIDs&);

// A track's write mode, the song's (Schema.h: trackAutomationMode): set without an undo step.
AutomationMode getAutomationMode (const Song&, TrackID);
void setAutomationMode (Song&, TrackID, AutomationMode);

//==============================================================================
// Automation points selected: one parameter's, in the selection (Selection: getPointKeys(), which holds the points' keys,
// CurveValues.h, so that it follows them as points are added or removed before them, as they move, and as undo and redo
// change the curve), so that selecting anything else deselects them, and the selection commands
// (arrangement/internal/Selection.h) find them; by their indices in the curve as it is now. Each finds the curve once and
// walks its points once.
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
// The song's curves as values (model/base/internal/CurveValues.h), the curves' area's (SongValues.h: SongValues::curves),
// which is how everything outside the writers below reads them: every one, by its parameter and in the order the song made
// them (the file's order, and the order a plug-in's parameters' lanes show in: getLanes()), in place, as the area holds them
// (a reference valid until the area's next change: none is kept across a set); a parameter's curve in place, none for one
// without; the parameters they automate, in that order; those of the curves with points; a parameter's curve as a copy
// (cheap: it shares what it holds), none for one without; and its points' keys, in its order (CurveValues.h: PointValue).
// A song that holds no values (one made of a tree alone, as a check's copy) has none. A parameter has one curve: the loader
// keeps the first of a file's curves of one parameter, its device's ID read as a number however it is written ("14",
// "0014"), and the song's area holds one of each.
const SongCurves& getCurvesValue (const Song&);
const CurveValue* findCurve (const Song&, const ParameterRef&);
std::vector<ParameterRef> getCurveKeys (const Song&);
std::vector<ParameterRef> getCurveKeysWithPoints (const Song&);
std::optional<CurveValue> getCurveValue (const Song&, const ParameterRef&);
std::vector<std::uint64_t> getPointKeys (const Song&, const ParameterRef&);

// A parameter's curve as a CURVE element that holds everything the song holds of it, an invalid tree for one without: what
// compares a curve as a file holds it (the versions' changes: model/versions/internal/ChangesWorker.h), written from its
// value as whole state writes it (CurveValues.h: writeCurveElement()). Message thread.
juce::ValueTree copyWholeCurve (const Song&, const ParameterRef&);

//==============================================================================
// A parameter's whole curve, by its parameter: what makes, takes out, renames, copies or moves a whole curve does it through
// these (a copy's curves: copyCurves(); a folder made a bus and back: model/mixer/Buses.cpp; a version applied:
// model/versions/ApplySong.cpp; a merge's choice: chooseCurveClash()), the point edits above through their own (a curve made
// by its first point, gone with its last). Each sets the curves' area (SongValues.h: setCurve(), eraseCurve(),
// renameCurve(), setCurveOrder()), joining the caller's undo step, as the song is written now: in the history's step in
// progress, or outside it while the song is written outside it (Song::OutsideHistory). Whether it wrote. Nothing for a song
// that holds no values.
//   - setCurveValue(): the parameter's whole curve made this value, where setCurve() (above) bends one point of it. One the
//     song hasn't is added after the others. One it has keeps its place, and its points are made the value's by their
//     places, as a version's are: a point at a place both have keeps its key (which the selection of points holds), those
//     past the value's last taken out, the value's past the curve's last given new keys. The value's own keys are no
//     matter (a copy's curve gets keys of its own). A curve as the value is (its points alike, keys aside) writes nothing.
//   - eraseCurve(): the parameter's curve taken out.
//   - renameCurve(): the curve of one parameter given to another, in its place, its points' keys kept (a folder's fader's
//     curve given to the fader that takes its place), what hears the curves change told both at once. Nothing for a
//     parameter without a curve, or for one that has a curve already (asserted).
//   - setCurveOrder(): the song's curves of these parameters (each once) put first, in this order, the others after them
//     in theirs (a version's order: model/versions/ApplySong.cpp); a parameter without a curve left out.
bool setCurveValue (Song&, const ParameterRef&, const CurveValue&);
bool eraseCurve (Song&, const ParameterRef&);
void renameCurve (Song&, const ParameterRef& from, const ParameterRef& to);
bool setCurveOrder (Song&, const std::vector<ParameterRef>& order);

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

// Forgets the curves of owners the song hasn't (a file's, which whoever wrote it may have left), and any child of its
// AUTOMATION that is no curve, as a project opens: without an undo step, on the tree of a song that holds no values yet
// (model/project/internal/Project.h: makeSong(), before it holds them; asserted), which no one else hears, whose AUTOMATION
// holds its curves as its file does. A curve's owner is there as for the form below (model/mixer/internal/Mixer.h:
// getItemIDs()): a device, a chain, a fader or a VCA, a send, a modulator or a macro. A device keeps those of parameters it
// hasn't, as a plug-in that isn't installed. A song that holds its values forgets them through the form below.
void forgetCurvesOfGoneDevices (Song&);

// The curves of what the song no longer holds taken out, in the caller's undo step (or outside the history while the song is
// written outside it), so that its Undo brings them back: each curve whose owner's ID (Schema.h: curveDevice, a device's, a
// container's chain's, a fader's or a VCA's, a send's, a modulator's or a macro's) isn't among the items still there, which
// the caller gathered (model/mixer/internal/Mixer.h: getItemIDs(), in forgetWhatNamesTheGone(), at every removal, and so
// after every apply of a song from the versions: model/versions/internal/ApplySong.h, removeBrokenReferences()). The
// curves' order walked by their parameters alone, each owner looked for among the items: no curve's points read, and
// nothing allocated while none is gone; those gone taken out of one next value of the curves' area, set once, so that what
// hears the curves is told once. Nothing for a song that holds no values.
void forgetCurvesOfTheGone (Song&, const std::set<std::uint64_t>& items);

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
} // namespace phazite
