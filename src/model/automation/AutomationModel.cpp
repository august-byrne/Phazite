// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AutomationModel.h"
#include "internal/Automation.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>

namespace phazite
{
namespace
{
    te::Track* findTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;
    }

    te::AutomatableParameter::Ptr find (const ProjectContext& context, const ParameterRef& parameter)
    {
        auto* edit = getEdit (context);
        return edit != nullptr ? findParameter (*edit, parameter) : nullptr;
    }

    te::Track* findTrackOf (const ProjectContext& context, const ParameterRef& parameter)
    {
        const auto found = find (context, parameter);
        return found != nullptr ? getTrackOfParameter (*found) : nullptr;
    }

    // The track's own volume, pan and sends are the mixer's controls, not a device's.
    bool isMixerControl (const te::Plugin* device)
    {
        return dynamic_cast<const te::VolumeAndPanPlugin*> (device) != nullptr || dynamic_cast<const te::VCAPlugin*> (device) != nullptr
               || dynamic_cast<const te::AuxSendPlugin*> (device) != nullptr;
    }

    ParameterKind getKind (const te::AutomatableParameter& parameter)
    {
        auto* device = parameter.getPlugin();

        if (auto* volume = dynamic_cast<te::VolumeAndPanPlugin*> (device))
            return &parameter == volume->panParam.get() ? ParameterKind::pan : ParameterKind::volume;

        if (isMixerControl (device))
            return ParameterKind::volume; // a folder's volume, a send's level: fader positions too

        return ParameterKind::other;
    }

    juce::String getName (const te::AutomatableParameter& parameter)
    {
        auto* device = parameter.getPlugin();

        if (auto* send = dynamic_cast<te::AuxSendPlugin*> (device))
        {
            const auto* returnTrack = findReturnTrack (send->edit, send->getBusNumber());
            return "Send to " + (returnTrack != nullptr ? returnTrack->getName() : juce::String ("Return"));
        }

        if (isMixerControl (device))
            return getKind (parameter) == ParameterKind::pan ? "Pan" : "Volume";

        return parameter.getParameterName();
    }

    AutomationTarget makeTarget (const te::AutomatableParameter& parameter, const te::Track& track)
    {
        auto* device = parameter.getPlugin();

        AutomationTarget target;
        target.parameter = getParameterRef (parameter);
        target.track = getTrackID (track);
        target.deviceName = isMixerControl (device) ? juce::String ("Mixer") : device->getName();
        target.name = getName (parameter);
        target.kind = getKind (parameter);
        target.automated = parameter.hasAutomationPoints();
        return target;
    }

    // A lane that shows needs its track, and the folders it is in, open.
    void openToShow (ProjectContext& context, const te::Track& track)
    {
        for (const auto* t = &track; t != nullptr; t = t->getParentFolderTrack())
            if (const auto row = getTrackRow (context, getTrackID (*t)); row.has_value() && row->collapsed)
                setCollapsed (context, row->id, false);
    }

    std::vector<te::Track*> getSelectedTracksOf (const ProjectContext& context)
    {
        std::vector<te::Track*> tracks;

        for (const auto id : context.getSongSelection().getTracks())
            if (auto* track = findTrack (context, id))
                tracks.push_back (track);

        return tracks;
    }
} // namespace

//==============================================================================
std::vector<AutomationTarget> getAutomationTargets (const ProjectContext& context, TrackID id)
{
    std::vector<AutomationTarget> targets;

    if (auto* track = findTrack (context, id))
        for (auto* parameter : getAutomatableParameters (*track))
            targets.push_back (makeTarget (*parameter, *track));

    return targets;
}

std::optional<AutomationTarget> getAutomationTarget (const ProjectContext& context, const ParameterRef& parameter)
{
    const auto found = find (context, parameter);
    auto* track = found != nullptr ? getTrackOfParameter (*found) : nullptr;

    if (track == nullptr)
        return {};

    return makeTarget (*found, *track);
}

ParameterHandle getParameter (ProjectContext& context, const ParameterRef& parameter)
{
    const auto found = find (context, parameter);
    return found != nullptr ? makeParameterHandle (found->getPlugin(), found->paramID, "Change " + getName (*found)) : ParameterHandle();
}

TrackID getMasterTrack (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    auto* master = edit != nullptr ? edit->getMasterTrack() : nullptr;
    return master != nullptr ? getTrackID (*master) : TrackID();
}

//==============================================================================
std::vector<AutomationLane> getAutomationLanes (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    return track != nullptr ? getLanes (*track) : std::vector<AutomationLane>();
}

TrackID showAutomationLane (ProjectContext& context, const ParameterRef& parameter)
{
    auto* track = findTrackOf (context, parameter);

    if (track == nullptr)
        return {};

    showLane (*track, parameter);
    openToShow (context, *track);
    return getTrackID (*track);
}

void hideAutomationLane (ProjectContext& context, const ParameterRef& parameter)
{
    if (auto* track = findTrackOf (context, parameter))
        hideLane (*track, parameter);
}

void setAutomationLaneParameter (ProjectContext& context, const ParameterRef& lane, const ParameterRef& replacement)
{
    if (auto* track = findTrackOf (context, lane))
        replaceLane (*track, lane, replacement);
}

void setAutomationLaneHeight (ProjectContext& context, const ParameterRef& parameter, int height)
{
    if (auto* track = findTrackOf (context, parameter))
        setLaneHeight (*track, parameter, height);
}

void showAutomatedLanes (ProjectContext& context, TrackID id)
{
    if (auto* track = findTrack (context, id))
    {
        for (auto* parameter : getAutomatableParameters (*track))
            if (parameter->hasAutomationPoints())
                showLane (*track, getParameterRef (*parameter));

        openToShow (context, *track);
    }
}

void hideAutomationLanes (ProjectContext& context, TrackID id)
{
    if (auto* track = findTrack (context, id))
        hideAllLanes (*track);
}

//==============================================================================
std::vector<AutomationPoint> getAutomationPoints (const ProjectContext& context, const ParameterRef& parameter)
{
    const auto found = find (context, parameter);
    return found != nullptr ? getPoints (*found) : std::vector<AutomationPoint>();
}

std::vector<float> getAutomationValues (const ProjectContext& context, const ParameterRef& parameter, BeatRange range, int numValues)
{
    const auto found = find (context, parameter);
    return found != nullptr ? getValues (*found, range, numValues) : std::vector<float>();
}

int addAutomationPoint (ProjectContext& context, const ParameterRef& parameter, AutomationPoint point)
{
    const auto found = find (context, parameter);

    if (found == nullptr)
        return -1;

    const UndoStep step (found->getEdit(), "Add Automation Point");
    return addPoint (*found, point);
}

BeatDuration moveAutomationPoints (ProjectContext& context, const ParameterRef& parameter, const std::vector<int>& indices,
                                   BeatDuration distance, float valueChange)
{
    const auto found = find (context, parameter);

    if (found == nullptr)
        return {};

    const UndoStep step (found->getEdit(), "Move Automation Points");
    return movePoints (*found, indices, distance, valueChange);
}

void setAutomationCurve (ProjectContext& context, const ParameterRef& parameter, int index, float curve)
{
    if (const auto found = find (context, parameter))
    {
        const UndoStep step (found->getEdit(), "Bend Automation");
        setCurve (*found, index, curve);
    }
}

void deleteAutomationPoints (ProjectContext& context, const ParameterRef& parameter, const std::vector<int>& indices)
{
    if (const auto found = find (context, parameter))
    {
        const UndoStep step (found->getEdit(), "Delete Automation Points");
        deletePoints (*found, indices);
    }
}

void replaceAutomationPoints (ProjectContext& context, const ParameterRef& parameter, BeatRange range,
                              const std::vector<AutomationPoint>& points)
{
    if (const auto found = find (context, parameter))
    {
        const UndoStep step (found->getEdit(), "Draw Automation");
        replacePoints (*found, range, points);
    }
}

void clearAutomation (ProjectContext& context, const ParameterRef& parameter)
{
    if (const auto found = find (context, parameter); found != nullptr && found->hasAutomationPoints())
    {
        const UndoStep step (found->getEdit(), "Clear Automation");
        found->getCurve().clear (&found->getEdit().getUndoManager());
        replacePoints (*found, {}, {}); // follows what is left: nothing
    }
}

//==============================================================================
AutomationMode getAutomationMode (const ProjectContext& context, TrackID id)
{
    auto* track = findTrack (context, id);
    return track != nullptr ? getAutomationMode (*track) : AutomationMode::read;
}

void setAutomationMode (ProjectContext& context, TrackID id, AutomationMode mode)
{
    if (auto* track = findTrack (context, id))
    {
        setAutomationMode (*track, mode);

        if (auto* watcher = getAutomationWatcher (context))
            watcher->writingChanged();
    }
}

std::optional<ParameterRef> getLastTouchedParameter (const ProjectContext& context)
{
    auto* watcher = getAutomationWatcher (context);

    if (watcher == nullptr)
        return {};

    if (const auto parameter = watcher->getLastTouched(); findTrackOf (context, parameter) != nullptr)
        return parameter;

    return {};
}

//==============================================================================
bool canAutomateLastTouched (const ProjectContext& context)
{
    return getLastTouchedParameter (context).has_value();
}

void automateLastTouched (ProjectContext& context)
{
    if (const auto parameter = getLastTouchedParameter (context))
        showAutomationLane (context, *parameter);
}

juce::String describeAutomateLastTouched (const ProjectContext& context)
{
    if (const auto parameter = getLastTouchedParameter (context))
        if (const auto target = getAutomationTarget (context, *parameter))
            return "Automate " + target->name;

    return "Automate Last Touched Parameter";
}

bool areTracksSelected (const ProjectContext& context)
{
    return ! getSelectedTracksOf (context).empty();
}

bool areAutomationLanesShown (const ProjectContext& context)
{
    const auto tracks = getSelectedTracksOf (context);
    return std::any_of (tracks.begin(), tracks.end(), [] (const te::Track* track) { return ! getLanes (*track).empty(); });
}

void toggleAutomationLanes (ProjectContext& context)
{
    const auto hide = areAutomationLanesShown (context);

    for (auto* track : getSelectedTracksOf (context))
    {
        if (hide)
        {
            hideAllLanes (*track);
            continue;
        }

        showAutomatedLanes (context, getTrackID (*track));

        // A track without automation shows its volume's lane, as Ableton Live shows a new lane.
        if (const auto parameters = getAutomatableParameters (*track); getLanes (*track).empty() && ! parameters.isEmpty())
            showAutomationLane (context, getParameterRef (*parameters.getFirst()));
    }
}

bool hasSelectedAutomationPoints (const ProjectContext& context)
{
    return getSelectedPoints (getSelection (context)).has_value();
}

void moveSelectedAutomationPoints (ProjectContext& context, BeatDuration distance, float partOfRange)
{
    if (const auto selected = getSelectedPoints (getSelection (context)))
    {
        auto& parameter = *selected->parameter;
        const UndoStep step (parameter.getEdit(), "Move Automation Points");
        movePoints (parameter, selected->indices, distance, partOfRange * (parameter.valueRange.end - parameter.valueRange.start));
    }
}

bool isAutomationModeOfSelection (const ProjectContext& context, AutomationMode mode)
{
    const auto tracks = getSelectedTracksOf (context);
    return ! tracks.empty()
           && std::all_of (tracks.begin(), tracks.end(), [mode] (const te::Track* track) { return getAutomationMode (*track) == mode; });
}

void setAutomationModeOfSelection (ProjectContext& context, AutomationMode mode)
{
    for (auto* track : getSelectedTracksOf (context))
        setAutomationMode (context, getTrackID (*track), mode);
}

//==============================================================================
void addAutomationCommands (CommandRegistry& registry, ProjectContext& context)
{
    const auto undoTarget = [&context] { return context.getUndoManager(); };

    registry.add ({ .id = "automation.automateLastTouched",
                    .name = "Automate Last Touched Parameter",
                    .category = "Automation",
                    .description = "Shows a lane for the parameter you changed last, in a device's window too, under its track.",
                    .synonyms = { "last tweaked", "automate", "automation lane", "envelope" },
                    .perform = [&context] { automateLastTouched (context); },
                    .isEnabled = [&context] { return canAutomateLastTouched (context); },
                    .currentName = [&context] { return describeAutomateLastTouched (context); },
                    .overPlugins = true });

    registry.add ({ .id = "automation.showLanes",
                    .name = "Show Automation",
                    .category = "Automation",
                    .description = "Shows the selected tracks' automation lanes, or hides them while they show.",
                    .synonyms = { "automation lanes", "envelopes", "hide automation" },
                    .perform = [&context] { toggleAutomationLanes (context); },
                    .isEnabled = [&context] { return areTracksSelected (context); },
                    .isTicked = [&context] { return areAutomationLanesShown (context); } });

    registry.add ({ .id = "automation.raisePoints",
                    .name = "Raise Automation Points",
                    .category = "Automation",
                    .description = "Raises the selected automation points by a hundredth of their range.",
                    .synonyms = { "up", "increase" },
                    .perform = [&context] { moveSelectedAutomationPoints (context, {}, 0.01f); },
                    .isEnabled = [&context] { return hasSelectedAutomationPoints (context); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "automation.lowerPoints",
                    .name = "Lower Automation Points",
                    .category = "Automation",
                    .description = "Lowers the selected automation points by a hundredth of their range.",
                    .synonyms = { "down", "decrease" },
                    .perform = [&context] { moveSelectedAutomationPoints (context, {}, -0.01f); },
                    .isEnabled = [&context] { return hasSelectedAutomationPoints (context); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "automation.raisePointsMore",
                    .name = "Raise Automation Points More",
                    .category = "Automation",
                    .description = "Raises the selected automation points by a tenth of their range.",
                    .synonyms = { "up", "increase" },
                    .perform = [&context] { moveSelectedAutomationPoints (context, {}, 0.1f); },
                    .isEnabled = [&context] { return hasSelectedAutomationPoints (context); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "automation.lowerPointsMore",
                    .name = "Lower Automation Points More",
                    .category = "Automation",
                    .description = "Lowers the selected automation points by a tenth of their range.",
                    .synonyms = { "down", "decrease" },
                    .perform = [&context] { moveSelectedAutomationPoints (context, {}, -0.1f); },
                    .isEnabled = [&context] { return hasSelectedAutomationPoints (context); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "automation.read",
                    .name = "Read Automation",
                    .category = "Automation",
                    .description = "The selected tracks play their automation; a control moved by hand goes back to its lane.",
                    .synonyms = { "automation mode", "play automation" },
                    .perform = [&context] { setAutomationModeOfSelection (context, AutomationMode::read); },
                    .isEnabled = [&context] { return areTracksSelected (context); },
                    .isTicked = [&context] { return isAutomationModeOfSelection (context, AutomationMode::read); } });

    registry.add ({ .id = "automation.touch",
                    .name = "Touch Automation",
                    .category = "Automation",
                    .description = "While the song plays, the selected tracks' controls write their automation while you hold them.",
                    .synonyms = { "automation mode", "record automation", "write automation" },
                    .perform = [&context] { setAutomationModeOfSelection (context, AutomationMode::touch); },
                    .isEnabled = [&context] { return areTracksSelected (context); },
                    .isTicked = [&context] { return isAutomationModeOfSelection (context, AutomationMode::touch); } });

    registry.add ({ .id = "automation.latch",
                    .name = "Latch Automation",
                    .category = "Automation",
                    .description = "While the song plays, the selected tracks' controls write their automation from your first move "
                                   "until playback stops.",
                    .synonyms = { "automation mode", "record automation", "write automation" },
                    .perform = [&context] { setAutomationModeOfSelection (context, AutomationMode::latch); },
                    .isEnabled = [&context] { return areTracksSelected (context); },
                    .isTicked = [&context] { return isAutomationModeOfSelection (context, AutomationMode::latch); } });

    registry.add ({ .id = "automation.write",
                    .name = "Write Automation",
                    .category = "Automation",
                    .description = "From when the song plays, the selected tracks' automated controls overwrite their lanes until you "
                                   "let go of them; then the tracks latch.",
                    .synonyms = { "automation mode", "record automation", "overwrite automation" },
                    .perform = [&context] { setAutomationModeOfSelection (context, AutomationMode::write); },
                    .isEnabled = [&context] { return areTracksSelected (context); },
                    .isTicked = [&context] { return isAutomationModeOfSelection (context, AutomationMode::write); } });
}
} // namespace phazite
