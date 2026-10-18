// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation's Tracktion side (model/automation/AutomationModel.h), for the model, the app and tests: parameters by their
// ParameterRefs, the points of a parameter's curve in beats, the lanes a track shows (Schema.h: PHAZITELANE), the selected
// points as Tracktion's selection manager holds them, automation that keeps its beats when the tempo changes, the
// watcher that remembers the parameter touched last and gives recorded automation an undo step of its own. Not for the
// UI.
#pragma once

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

// The track whose device a parameter is: the master track for the master's volume and effects, and a container's track
// for its devices'. nullptr for others.
te::Track* getTrackOfParameter (const te::AutomatableParameter&);

// The parameters of a track's devices that can be automated, in the mixer's order, those of the devices in its
// containers after each container (AutomationModel.h: getAutomationTargets()).
juce::ReferenceCountedArray<te::AutomatableParameter> getAutomatableParameters (te::Track&);

// The track's own volume, pan and sends are the mixer's controls, not a device's.
bool isMixerControl (const te::Plugin*);

// A parameter's name as menus, lanes and undo steps show it: "Volume", "Pan", "Send to Reverb", or the device's own name
// for it.
juce::String getParameterName (const te::AutomatableParameter&);

//==============================================================================
// A parameter's curve as points in song beats, and edits of it. They change the Edit through its undo manager without
// beginning a step: they join the caller's. Each keeps the parameter's value at the playhead in step with the curve
// while the song isn't playing.
std::vector<AutomationPoint> getPoints (const te::AutomatableParameter&);
std::vector<float> getValues (const te::AutomatableParameter&, BeatRange, int numValues); // getAutomationValues()
int addPoint (te::AutomatableParameter&, AutomationPoint);
BeatDuration movePoints (te::AutomatableParameter&, const std::vector<int>& indices, BeatDuration, float valueChange);
void setCurve (te::AutomatableParameter&, int index, float curve);
void deletePoints (te::AutomatableParameter&, const std::vector<int>& indices);
void replacePoints (te::AutomatableParameter&, BeatRange, const std::vector<AutomationPoint>&);

// Copies points right after themselves, as Duplicate does: each copy as far after its original as the points span (a
// grid step at least, for one point), replacing the points where they land. Returns the copies' indices.
std::vector<int> duplicatePoints (te::AutomatableParameter&, const std::vector<int>& indices, BeatDuration minimumSpan);

//==============================================================================
// Automation that goes with the clips above it (docs/design.md, section 10), as Ableton Live, Bitwig and Studio One move
// automation with clips: the points of a track's parameters in a stretch of the song, moved or copied by a distance (later,
// or earlier if negative), replacing what is where they land. A stretch keeps its shape at its edges wherever it goes, and
// what lies around it keeps its own; where a moved stretch was, the line runs straight across. A track's stretches that
// overlap or touch go as one. They join the caller's undo step.
struct AutomationStretch
{
    te::Track* track = nullptr;
    BeatRange range {};
};

void moveAutomation (const std::vector<AutomationStretch>&, BeatDuration distance, bool copy);

// Deletes the automation in stretches, as deleting time does, the parts on either side keeping their shape; with closeGap,
// what comes after a stretch moves back by its length, as the track's clips do (arrangement/internal/Clips.h:
// deleteTimeRange()).
void deleteAutomation (const std::vector<AutomationStretch>&, bool closeGap);

//==============================================================================
// The lanes a track shows (getAutomationLanes()): those it keeps (Schema.h: PHAZITELANE), of devices on it, then one for
// each of its parameters with automation that it keeps none for, so that automation never hides. Changes to them are
// written without undo steps, and keep a lane for each of those first, so that the lanes stay in the order shown.
std::vector<AutomationLane> getLanes (te::Track&);
void showLane (te::Track&, const ParameterRef&);   // after the others, or where it is; expanded
void removeLane (te::Track&, const ParameterRef&); // one for a parameter with automation shows again, after the others
void replaceLane (te::Track&, const ParameterRef& lane, const ParameterRef& replacement);
void setLaneHeight (te::Track&, const ParameterRef&, int height);
void setLaneCollapsed (te::Track&, const ParameterRef&, bool);

// After a track was copied (a folder with its tracks): the copy's lanes show the copy's devices, as the original's show
// the original's, those in its containers too.
void copyLanes (const te::Track& original, te::Track& copy, const te::EditItemID::IDMap& devicesInContainers = {});

AutomationMode getAutomationMode (const te::Track&);
void setAutomationMode (te::Track&, AutomationMode);

//==============================================================================
// Automation points selected: one parameter's, by their indices, as Tracktion's selection manager holds them, so that
// selecting anything else deselects them, and the selection commands (arrangement/internal/Selection.h) find them.
// SongSelection owns the one it selects.
class SelectedAutomationPoints : public te::Selectable
{
public:
    ~SelectedAutomationPoints() override { notifyListenersOfDeletion(); } // as Tracktion asks of every Selectable

    juce::String getSelectableDescription() override { return "Automation Points"; }

    ParameterRef parameter;
    std::vector<int> indices; // in order
};

struct PointSelection
{
    te::AutomatableParameter::Ptr parameter;
    std::vector<int> indices; // of points that are there, in order
};

// The selected points, if points are what the selection manager holds.
std::optional<PointSelection> getSelectedPoints (const te::SelectionManager&);
void selectPoints (te::SelectionManager&, const te::AutomatableParameter&, const std::vector<int>& indices);

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

// Whether a property of a device's state (a PLUGIN) is the stored value of a parameter with automation, or one a
// modulator moves, which Tracktion writes as they play: no change to the song. The value set by hand is one, as an undo
// step (Edit::hasChangedSinceSaved()).
bool isAutomatedValue (const juce::ValueTree&, const juce::Identifier& property);

// Puts back, in a saved project's state before its Edit is made, the values its parameters were set to by hand where they
// were saved while something else set them to others (a modulator): Tracktion saves those in the device's state too, and
// puts them back for plug-ins, but not for Phazite's devices, which would then start at the values played.
void restoreValuesSetByHand (juce::ValueTree& editState);
} // namespace phazite
