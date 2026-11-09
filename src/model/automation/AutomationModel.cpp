// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AutomationModel.h"
#include "internal/Automation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/ParameterHandle.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>
#include <numeric>

namespace phazite
{
namespace
{
    // How a lane shows a parameter's values: a pan as a pan (a container's chain's too); a fader's volume, a folder's VCA,
    // a send's level and a chain's volume as decibels from a fader's positions.
    ParameterKind getKind (const Song& song, const ParameterRef& parameter)
    {
        if (const auto found = locateParameter (song, parameter))
        {
            if (found->source == ParameterSource::pan || found->source == ParameterSource::chainPan)
                return ParameterKind::pan;

            if (found->source == ParameterSource::volume || found->source == ParameterSource::send
                || found->source == ParameterSource::chainVolume)
                return ParameterKind::volume;
        }

        return ParameterKind::other;
    }

    AutomationTarget makeTarget (const Song& song, const ParameterRef& parameter, TrackID track)
    {
        AutomationTarget target;
        target.parameter = parameter;
        target.track = track;
        target.deviceName = getParameterDeviceName (song, parameter);
        target.name = getParameterName (song, parameter);
        target.kind = getKind (song, parameter);
        target.automated = hasPoints (song, parameter);
        return target;
    }

    // A lane that shows needs its track, and the folders it is in, open (view state, without undo steps).
    void openToShow (Song& song, TrackID track)
    {
        for (auto t = track; t.isValid(); t = getParentFolder (song, t))
            if (isCollapsed (song, t))
                setCollapsed (song, t, false);
    }

    void showLaneOf (Song& song, const ParameterRef& parameter)
    {
        if (const auto track = getParameterTrack (song, parameter); track.isValid())
        {
            showLane (song, track, parameter);
            openToShow (song, track);
        }
    }

    // A parameter's automation, all its points as one step, and its lane, which is view state: an undo brings back the
    // automation, and the lane with it.
    void deleteAutomationOf (Song& song, const ParameterRef& parameter)
    {
        if (const auto points = getPoints (song, parameter); ! points.empty())
        {
            std::vector<int> indices (points.size());
            std::iota (indices.begin(), indices.end(), 0);
            const UndoStep step (song, "Delete Automation");
            deletePoints (song, parameter, indices);
        }

        if (const auto track = getParameterTrack (song, parameter); track.isValid())
            removeLane (song, track, parameter);
    }

    std::vector<TrackID> getSelectedTracksOf (const ProjectContext& context)
    {
        std::vector<TrackID> tracks;
        auto* song = findSong (context);

        if (song != nullptr)
            for (const auto id : context.getSongSelection().getTracks())
                if (findTrackState (*song, id).isValid())
                    tracks.push_back (id);

        return tracks;
    }

    // The track whose channel a device, a fader or a send is on (a container's devices' too).
    TrackID findChannel (const Song& song, DeviceID device)
    {
        if (findDeviceState (song, device).isValid())
            return getDeviceChannel (song, device);

        if (const auto channel = findChannelOfFader (song, device); channel.isValid())
            return getTrackID (channel);

        if (const auto send = findSendState (song, device); send.isValid())
            return getTrackID (send.getParent());

        return {};
    }
} // namespace

//==============================================================================
std::vector<AutomationTarget> getAutomationTargets (const ProjectContext& context, TrackID id)
{
    std::vector<AutomationTarget> targets;

    if (auto* song = findSong (context))
        for (const auto& parameter : getAutomatableParameters (*song, id))
            targets.push_back (makeTarget (*song, parameter, id));

    return targets;
}

std::vector<AutomationTarget> getAutomationTargets (const ProjectContext& context, DeviceID device)
{
    std::vector<AutomationTarget> targets;
    auto* song = findSong (context);
    const auto track = song != nullptr ? findChannel (*song, device) : TrackID();

    // A container's are its chains' volumes, pans and crossovers.
    if (track.isValid())
        for (const auto& parameter : getAutomatableParameters (*song, track))
            if (parameter.device == device || getChainContainer (*song, parameter.device) == device)
                targets.push_back (makeTarget (*song, parameter, track));

    return targets;
}

std::optional<AutomationTarget> getAutomationTarget (const ProjectContext& context, const ParameterRef& parameter)
{
    auto* song = findSong (context);
    const auto track = song != nullptr ? getParameterTrack (*song, parameter) : TrackID();

    if (! track.isValid())
        return {};

    return makeTarget (*song, parameter, track);
}

ParameterHandle getParameter (ProjectContext& context, const ParameterRef& parameter)
{
    auto* song = findSong (context);

    if (song == nullptr || ! locateParameter (*song, parameter).has_value())
        return {};

    return makeParameterHandle (*song, parameter, "Change " + getParameterName (*song, parameter));
}

TrackID getMasterTrack (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getMasterID (*song) : TrackID();
}

//==============================================================================
std::vector<AutomationLane> getAutomationLanes (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr && findTrackState (*song, id).isValid() ? getLanes (*song, id) : std::vector<AutomationLane>();
}

TrackID showAutomationLane (ProjectContext& context, const ParameterRef& parameter)
{
    auto* song = findSong (context);
    const auto track = song != nullptr ? getParameterTrack (*song, parameter) : TrackID();

    if (! track.isValid())
        return {};

    showLane (*song, track, parameter);
    openToShow (*song, track);
    return track;
}

void removeAutomationLane (ProjectContext& context, const ParameterRef& parameter)
{
    if (auto* song = findSong (context); song != nullptr && locateParameter (*song, parameter).has_value())
        deleteAutomationOf (*song, parameter);
}

void setAutomationLaneParameter (ProjectContext& context, const ParameterRef& lane, const ParameterRef& replacement)
{
    if (auto* song = findSong (context))
        if (const auto track = getParameterTrack (*song, lane); track.isValid())
            replaceLane (*song, track, lane, replacement);
}

void setAutomationLaneHeight (ProjectContext& context, const ParameterRef& parameter, int height)
{
    if (auto* song = findSong (context))
        if (const auto track = getParameterTrack (*song, parameter); track.isValid())
            setLaneHeight (*song, track, parameter, height);
}

void setAutomationLaneCollapsed (ProjectContext& context, const ParameterRef& parameter, bool collapsed)
{
    if (auto* song = findSong (context))
        if (const auto track = getParameterTrack (*song, parameter); track.isValid())
            setLaneCollapsed (*song, track, parameter, collapsed);
}

void setAutomationLanesCollapsed (ProjectContext& context, TrackID id, bool collapsed)
{
    if (auto* song = findSong (context); song != nullptr && findTrackState (*song, id).isValid())
        for (const auto& lane : getLanes (*song, id))
            setLaneCollapsed (*song, id, lane.parameter, collapsed);
}

//==============================================================================
std::vector<AutomationPoint> getAutomationPoints (const ProjectContext& context, const ParameterRef& parameter)
{
    auto* song = findSong (context);
    return song != nullptr ? getPoints (*song, parameter) : std::vector<AutomationPoint>();
}

std::vector<float> getAutomationValues (const ProjectContext& context, const ParameterRef& parameter, BeatRange range, int numValues)
{
    auto* song = findSong (context);
    return song != nullptr ? getValues (*song, parameter, range, numValues) : std::vector<float>();
}

int addAutomationPoint (ProjectContext& context, const ParameterRef& parameter, AutomationPoint point)
{
    auto* song = findSong (context);

    if (song == nullptr || ! locateParameter (*song, parameter).has_value())
        return -1;

    const UndoStep step (*song, "Add Automation Point");
    return addPoint (*song, parameter, point);
}

BeatDuration moveAutomationPoints (ProjectContext& context, const ParameterRef& parameter, const std::vector<int>& indices,
                                   BeatDuration distance, float valueChange)
{
    auto* song = findSong (context);

    if (song == nullptr || ! locateParameter (*song, parameter).has_value())
        return {};

    const UndoStep step (*song, "Move Automation Points");
    return movePoints (*song, parameter, indices, distance, valueChange);
}

void setAutomationCurve (ProjectContext& context, const ParameterRef& parameter, int index, float bend)
{
    if (auto* song = findSong (context); song != nullptr && locateParameter (*song, parameter).has_value())
    {
        const UndoStep step (*song, "Bend Automation");
        setCurve (*song, parameter, index, bend);
    }
}

void setAutomationPointValues (ProjectContext& context, const ParameterRef& parameter, const std::vector<int>& indices, float value)
{
    auto* song = findSong (context);

    if (song == nullptr || ! locateParameter (*song, parameter).has_value())
        return;

    const UndoStep step (*song, "Set Automation Value");
    const auto points = getPoints (*song, parameter);

    for (const auto index : indices)
        if (juce::isPositiveAndBelow (index, (int) points.size()))
            movePoints (*song, parameter, { index }, {}, value - points[(size_t) index].value);
}

void setAutomationCurves (ProjectContext& context, const ParameterRef& parameter, const std::vector<int>& indices, float bend)
{
    if (auto* song = findSong (context); song != nullptr && locateParameter (*song, parameter).has_value())
    {
        const UndoStep step (*song, "Bend Automation");

        for (const auto index : indices)
            setCurve (*song, parameter, index, bend);
    }
}

void deleteAutomationPoints (ProjectContext& context, const ParameterRef& parameter, const std::vector<int>& indices)
{
    if (auto* song = findSong (context); song != nullptr && locateParameter (*song, parameter).has_value())
    {
        const UndoStep step (*song, "Delete Automation Points");
        deletePoints (*song, parameter, indices);
    }
}

void replaceAutomationPoints (ProjectContext& context, const ParameterRef& parameter, BeatRange range,
                              const std::vector<AutomationPoint>& points)
{
    if (auto* song = findSong (context); song != nullptr && locateParameter (*song, parameter).has_value())
    {
        const UndoStep step (*song, "Draw Automation");
        replacePoints (*song, parameter, range, points);
    }
}

bool isAutomated (const ParameterHandle& handle)
{
    auto* song = findSong (handle);
    return song != nullptr && hasPoints (*song, handle.getRef());
}

void showAutomationLane (const ParameterHandle& handle)
{
    if (auto* song = findSong (handle); song != nullptr && handle.getRef().isValid())
        showLaneOf (*song, handle.getRef());
}

void deleteAutomation (const ParameterHandle& handle)
{
    if (auto* song = findSong (handle); song != nullptr && handle.getRef().isValid())
        deleteAutomationOf (*song, handle.getRef());
}

//==============================================================================
AutomationMode getAutomationMode (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);
    return song != nullptr ? getAutomationMode (*song, id) : AutomationMode::read;
}

void setAutomationMode (ProjectContext& context, TrackID id, AutomationMode mode)
{
    if (auto* song = findSong (context))
        setAutomationMode (*song, id, mode);
}

std::optional<ParameterRef> getLastTouchedParameter (const ProjectContext& context)
{
    auto* watcher = getAutomationWatcher (context);
    auto* song = findSong (context);

    if (watcher == nullptr || song == nullptr)
        return {};

    if (const auto parameter = watcher->getLastTouched(); getParameterTrack (*song, parameter).isValid())
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

bool areAutomationLanesExpanded (const ProjectContext& context)
{
    const auto tracks = getSelectedTracksOf (context);
    const auto* song = findSong (context);

    return std::any_of (tracks.begin(), tracks.end(),
                        [song] (TrackID track)
                        {
                            const auto lanes = getLanes (*song, track);
                            return std::any_of (lanes.begin(), lanes.end(), [] (const AutomationLane& lane) { return ! lane.collapsed; });
                        });
}

void toggleAutomationLanes (ProjectContext& context)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return;

    const auto collapse = areAutomationLanesExpanded (context);

    for (const auto track : getSelectedTracksOf (context))
    {
        // A track without lanes gets its volume's, as Ableton Live shows a new lane; expanded lanes need their track open.
        if (! collapse)
        {
            if (const auto parameters = getAutomatableParameters (*song, track); getLanes (*song, track).empty() && ! parameters.empty())
                showLane (*song, track, parameters.front());

            openToShow (*song, track);
        }

        setAutomationLanesCollapsed (context, track, collapse);
    }
}

bool hasSelectedAutomationPoints (const ProjectContext& context)
{
    return getSelectedPoints (getSelection (context)).has_value();
}

void moveSelectedAutomationPoints (ProjectContext& context, BeatDuration distance, float partOfRange)
{
    auto* song = findSong (context);
    const auto selected = getSelectedPoints (getSelection (context));
    const auto description = song != nullptr && selected.has_value() ? describeParameter (*song, selected->parameter) : std::nullopt;

    if (description.has_value())
    {
        const UndoStep step (*song, "Move Automation Points");
        movePoints (*song, selected->parameter, selected->indices, distance,
                    partOfRange * (description->range.end - description->range.start));
    }
}

bool isAutomationModeOfSelection (const ProjectContext& context, AutomationMode mode)
{
    const auto tracks = getSelectedTracksOf (context);
    const auto* song = findSong (context);
    return ! tracks.empty()
           && std::all_of (tracks.begin(), tracks.end(), [song, mode] (TrackID track) { return getAutomationMode (*song, track) == mode; });
}

void setAutomationModeOfSelection (ProjectContext& context, AutomationMode mode)
{
    for (const auto track : getSelectedTracksOf (context))
        setAutomationMode (context, track, mode);
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
                    .description = "Expands the selected tracks' automation lanes, or collapses them to thin rows while one shows "
                                   "expanded. A track without lanes gets one for its volume.",
                    .synonyms = { "automation lanes", "envelopes", "collapse automation", "expand automation" },
                    .perform = [&context] { toggleAutomationLanes (context); },
                    .isEnabled = [&context] { return areTracksSelected (context); },
                    .isTicked = [&context] { return areAutomationLanesExpanded (context); } });

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
