// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation's Tracktion side (model/automation/AutomationModel.h), for the model, the app and tests: parameters by their
// ParameterRefs, the points of a parameter's curve in beats, the lanes a track shows (Schema.h: PHAZITELANE), the selected
// points as Tracktion's selection manager holds them, automation that keeps its beats when the tempo changes, and the
// watcher that remembers the parameter touched last and gives recorded automation an undo step of its own. Not for the
// UI.
#pragma once

#include "AutomationClips.h"
#include "model/automation/AutomationValues.h"
#include "model/base/internal/Tracktion.h"

#include <optional>
#include <vector>

namespace phazite
{
//==============================================================================
// The parameter a ParameterRef names, or nullptr.
te::AutomatableParameter::Ptr findParameter (const te::Edit&, const ParameterRef&);

// A device's parameter by IDs; an invalid ref for one that isn't a device's (a macro's, a modulator's).
ParameterRef getParameterRef (const te::AutomatableParameter&);

// The track whose device a parameter is: the master track for the master's volume and effects. nullptr for others.
te::Track* getTrackOfParameter (const te::AutomatableParameter&);

// The parameters of a track's devices that can be automated, in the mixer's order (AutomationModel.h:
// getAutomationTargets()).
juce::ReferenceCountedArray<te::AutomatableParameter> getAutomatableParameters (te::Track&);

//==============================================================================
// A curve (CurveRef): the parameter it sets, and the curve itself, the parameter's own or an automation clip's.
struct CurveAccess
{
    te::AutomatableParameter::Ptr parameter;
    te::AutomationCurve* curve = nullptr;
    te::Clip* clip = nullptr;                  // an automation clip's curve: positioned from the clip's content start
    te::AutomationCurveModifier::Ptr modifier; // and what holds it

    CurveRef getRef() const;
};

std::optional<CurveAccess> findCurve (const te::Edit&, const CurveRef&);
CurveAccess getOwnCurve (te::AutomatableParameter&); // the parameter's own

// A curve as points in song beats, and edits of it. They change the Edit through its undo manager without beginning a
// step: they join the caller's. Each keeps the parameter's value at the playhead in step with the curve while the song
// isn't playing, and an automation clip's aliases' curves the same as its own.
std::vector<AutomationPoint> getPoints (const CurveAccess&);
std::vector<float> getValues (const CurveAccess&, BeatRange, int numValues); // getAutomationValues()
int addPoint (const CurveAccess&, AutomationPoint);
BeatDuration movePoints (const CurveAccess&, const std::vector<int>& indices, BeatDuration, float valueChange);
void setCurve (const CurveAccess&, int index, float curve);
void deletePoints (const CurveAccess&, const std::vector<int>& indices);
void replacePoints (const CurveAccess&, BeatRange, const std::vector<AutomationPoint>&);

// Copies points right after themselves, as Duplicate does: each copy as far after its original as the points span (a
// grid step at least, for one point), replacing the points where they land. Returns the copies' indices.
std::vector<int> duplicatePoints (const CurveAccess&, const std::vector<int>& indices, BeatDuration minimumSpan);

//==============================================================================
// Automation clips (AutomationClips.h: isAutomationClip()): MIDI clips without notes that hold one absolute automation
// curve (Tracktion's clip automation), which sets its parameter while the clip plays.
te::AutomatableParameter::Ptr getAutomationClipParameter (const te::Clip&); // nullptr for another clip, or without its device

// Makes one on a track, over a range, setting a parameter to a value; nullptr if it couldn't. Joins the caller's step.
te::MidiClip* createAutomationClip (te::ClipTrack&, te::AutomatableParameter&, te::BeatRange, float value);

// An automation clip sets the parameter it was made for wherever it is, as FL Studio's do. Tracktion instead points the
// curve of a clip that is copied, split or moved to another track at the same parameter of the first device of its kind
// on that track, or drops it (the master's volume, which is on no track). This remembers a clip's curves before such an
// edit, and puts them back after it on the clip, or on a copy or part of it, joining the caller's step.
class AutomationTargets
{
public:
    explicit AutomationTargets (const te::Clip&);
    void setDevice (te::EditItemID from, te::EditItemID to); // curves of one device are to set another's
    void restore (te::Clip&) const;

private:
    juce::ValueTree curves; // a copy of the clip's AUTOMATIONCURVES, if it has curves
};

// After a track was copied: the automation clips the copy holds set the copy's devices where the original's set the
// original's, as its lanes show them (copyLanes()).
void copyAutomationTargets (const te::Track& original, te::Track& copy);

//==============================================================================
// The lanes a track shows, of devices on it (getAutomationLanes()), and changes to them, written without undo steps.
std::vector<AutomationLane> getLanes (const te::Track&);
void showLane (te::Track&, const ParameterRef&);
void hideLane (te::Track&, const ParameterRef&);
void hideAllLanes (te::Track&);
void replaceLane (te::Track&, const ParameterRef& lane, const ParameterRef& replacement);
void setLaneHeight (te::Track&, const ParameterRef&, int height);

// After a track was copied (a folder with its tracks): the copy's lanes show the copy's devices, as the original's show
// the original's.
void copyLanes (const te::Track& original, te::Track& copy);

AutomationMode getAutomationMode (const te::Track&);
void setAutomationMode (te::Track&, AutomationMode);

//==============================================================================
// Automation points selected: one curve's, by their indices, as Tracktion's selection manager holds them, so that
// selecting anything else deselects them, and the selection commands (arrangement/internal/Selection.h) find them.
// SongSelection owns the one it selects.
class SelectedAutomationPoints : public te::Selectable
{
public:
    ~SelectedAutomationPoints() override { notifyListenersOfDeletion(); } // as Tracktion asks of every Selectable

    juce::String getSelectableDescription() override { return "Automation Points"; }

    CurveRef curve;
    std::vector<int> indices; // in order
};

struct PointSelection
{
    CurveAccess curve;
    std::vector<int> indices; // of points that are there, in order
};

// The selected points, if points are what the selection manager holds.
std::optional<PointSelection> getSelectedPoints (const te::SelectionManager&);
void selectPoints (te::SelectionManager&, const CurveAccess&, const std::vector<int>& indices);

//==============================================================================
// Keeps every automation point at its beat across a change of the tempo or the time signature: Tracktion keeps track
// automation at its time in seconds. Make one before the change, in its undo step; it moves the points back to their
// beats when it goes.
class AutomationOnBeats
{
public:
    explicit AutomationOnBeats (te::Edit&);
    ~AutomationOnBeats();

private:
    struct Curve
    {
        te::AutomatableParameter::Ptr parameter;
        std::vector<te::BeatPosition> beats;
    };

    te::Edit& edit;
    std::vector<Curve> curves;

    JUCE_DECLARE_NON_COPYABLE (AutomationOnBeats)
};

//==============================================================================
// Watches an open Edit's automation: remembers the parameter last changed by hand (not by its automation), and holds a
// "Record Automation" undo step (Undo.h: RecordingStep) from when playback starts while a track writes automation until
// it stops, so that what Tracktion records joins that step rather than the change before it. A control's own drag
// (ParameterHandle) begins a step of its own, which what is recorded meanwhile joins.
class RecordingStep;

class AutomationWatcher : private te::ParameterChangeHandler::Listener,
                          private juce::ChangeListener,
                          private juce::AsyncUpdater
{
public:
    explicit AutomationWatcher (te::Edit&);
    ~AutomationWatcher() override;

    ParameterRef getLastTouched() const { return lastTouched; }

    // A track's write mode changed: a track that writes now, while the song plays, starts the step.
    void writingChanged();

private:
    static constexpr size_t maxTouchedAtOnce = 2; // an EQ band's dot moves two parameters at once

    te::SafeSelectable<te::Edit> edit; // the context can outlive it
    ParameterRef lastTouched;
    std::vector<ParameterRef> changedAtOnce; // in this round of messages
    std::unique_ptr<RecordingStep> recording;
    bool wasPlaying = false, stopRecording = false;

    void pluginParameterChanged (te::AutomatableParameter&, bool isFollowingAutomation) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the transport
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE (AutomationWatcher)
};

// Whether any of the Edit's tracks writes automation (a mode other than read).
bool isWritingAutomation (const te::Edit&);

// Whether a property of a device's state (a PLUGIN) is the stored value of a parameter with automation, or with an
// automation clip's curve, which Tracktion writes as the automation plays: no change to the song. The value set by hand
// is one, as an undo step (Edit::hasChangedSinceSaved()).
bool isAutomatedValue (const juce::ValueTree&, const juce::Identifier& property);

// Puts back, in a saved project's state before its Edit is made, the values its parameters were set to by hand where they
// were saved while automation set them to others (an automation clip's curve, between its points): Tracktion saves those
// in the device's state too, and puts them back for plug-ins, but not for its own devices and Phazite's, which would then
// start at the values the automation played.
void restoreValuesSetByHand (juce::ValueTree& editState);
} // namespace phazite
