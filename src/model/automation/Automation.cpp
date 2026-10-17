// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"

#include <algorithm>
#include <limits>

namespace phazite
{
namespace
{
    // Where a curve's beat 0 is in the song: the song's start for a parameter's own curve, the clip's content start for
    // an automation clip's (as its notes are placed).
    BeatPosition getOrigin (const CurveAccess& access)
    {
        if (access.clip == nullptr)
            return {};

        auto& edit = access.clip->edit;
        return fromTracktion (edit.tempoSequence.toBeats (access.clip->getTimeOfContentBeat (te::BeatPosition())));
    }

    // Where a curve keeps a song beat: in seconds, as track automation is, or in beats from its origin. Never before
    // the origin.
    te::EditPosition toCurvePosition (const CurveAccess& access, BeatPosition beat)
    {
        const auto& curve = *access.curve;
        const auto position = te::BeatPosition::fromBeats (std::max (0.0, (beat - getOrigin (access)).inBeats()));

        if (curve.timeBase == te::AutomationCurve::TimeBase::beats)
            return position;

        return curve.edit.tempoSequence.toTime (position);
    }

    BeatPosition getPointBeat (const CurveAccess& access, int index)
    {
        const auto& curve = *access.curve;
        return getOrigin (access)
               + (fromTracktion (te::toBeats (curve.getPointPosition (index), curve.edit.tempoSequence)) - BeatPosition());
    }

    float limitValue (const te::AutomatableParameter& parameter, float value)
    {
        return juce::jlimit (parameter.valueRange.start, parameter.valueRange.end, value);
    }

    float limitCurve (float curve)
    {
        return juce::jlimit (-1.0f, 1.0f, curve);
    }

    juce::UndoManager* undoManagerOf (const te::AutomatableParameter& parameter)
    {
        return &parameter.getEdit().getUndoManager();
    }

    // While the song plays, the engine follows the curve; while it doesn't, the value at the playhead is set here, so
    // that controls show it.
    void followCurve (te::AutomatableParameter& parameter)
    {
        auto& transport = parameter.getEdit().getTransport();

        if (! transport.isPlaying())
            parameter.updateToFollowCurve (transport.getPosition());
    }

    // An automation clip's curve, if it has one for a parameter's ID.
    te::AutomationCurveModifier::Ptr findClipCurve (te::Clip& clip, const juce::String& parameterID)
    {
        // Asking a clip for its curve list adds an empty one to its state, as an undo step: only one that has curves.
        if (clip.state.getChildWithName (te::IDs::AUTOMATIONCURVES).getNumChildren() == 0)
            return {};

        if (auto* list = clip.getAutomationCurveList (false))
            for (auto& item : list->getItems())
                if (item->getDestID().paramID == parameterID)
                    return item;

        return {};
    }

    // An automation clip's aliases share its curve (Tracktion links their notes, not their curves): theirs become the
    // same as its own, in the same undo step.
    void copyToAliases (const CurveAccess& access)
    {
        if (access.clip == nullptr || ! access.clip->isLinked())
            return;

        const auto& source = *access.curve;
        auto* undoManager = undoManagerOf (*access.parameter);

        for (auto* alias : access.clip->edit.findClipsInLinkGroup (access.clip->getLinkGroupID()))
        {
            if (alias == access.clip)
                continue;

            if (const auto modifier = findClipCurve (*alias, access.parameter->paramID))
            {
                auto& curve = modifier->getCurve (te::CurveModifierType::absolute).curve;
                curve.clear (undoManager);

                for (int i = 0; i < source.getNumPoints(); ++i)
                    curve.addPoint (source.getPointPosition (i), source.getPointValue (i), source.getPointCurve (i), undoManager);
            }
        }
    }

    // After an edit of a curve: its aliases' curves, and the value it plays.
    void curveEdited (const CurveAccess& access)
    {
        copyToAliases (access);
        followCurve (*access.parameter);
    }

    // Indices of points that are there, each once, in order.
    std::vector<int> getValidIndices (const te::AutomationCurve& curve, std::vector<int> indices)
    {
        std::sort (indices.begin(), indices.end());
        indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
        indices.erase (std::remove_if (indices.begin(), indices.end(),
                                       [&curve] (int index) { return ! juce::isPositiveAndBelow (index, curve.getNumPoints()); }),
                       indices.end());
        return indices;
    }

    //==============================================================================
    ParameterRef readLane (const juce::ValueTree& lane)
    {
        return { DeviceID::fromRaw (te::EditItemID::fromVar (lane[schema::laneDevice]).getRawID()),
                 lane[schema::laneParameter].toString() };
    }

    void writeLane (juce::ValueTree& lane, const ParameterRef& parameter)
    {
        lane.setProperty (schema::laneDevice, toTracktion (parameter.device).toString(), nullptr);
        lane.setProperty (schema::laneParameter, parameter.parameter, nullptr);
    }

    juce::ValueTree findLaneState (const te::Track& track, const ParameterRef& parameter)
    {
        for (const auto& child : track.state)
            if (schema::isTrackViewElement (child) && readLane (child) == parameter)
                return child;

        return {};
    }

    // Whether a lane shows a parameter of a device on this track.
    bool isOnTrack (const te::Track& track, const ParameterRef& parameter)
    {
        const auto found = findParameter (track.edit, parameter);
        return found != nullptr && getTrackOfParameter (*found) == &track;
    }

    te::AutomationMode toTracktion (AutomationMode mode)
    {
        switch (mode)
        {
            case AutomationMode::touch:
                return te::AutomationMode::touch;
            case AutomationMode::latch:
                return te::AutomationMode::latch;
            case AutomationMode::write:
                return te::AutomationMode::write;
            case AutomationMode::read:
            default:
                return te::AutomationMode::read;
        }
    }

    // The IDs of the parameters whose values a property of a device's state holds: Tracktion's own devices name it after
    // something else; Phazite's name it after the parameter (Schema.h).
    juce::StringArray getParametersStoredIn (const juce::ValueTree& device, const juce::Identifier& property)
    {
        const auto type = device[te::IDs::type].toString();

        if (type == te::VolumeAndPanPlugin::xmlTypeName)
        {
            if (property == te::IDs::volume)
                return { "volume", "master volume" };

            if (property == te::IDs::pan)
                return { "pan", "master pan" };

            return {};
        }

        if (type == te::VCAPlugin::xmlTypeName)
            return property == te::IDs::volume ? juce::StringArray { "vca" } : juce::StringArray();

        if (type == te::AuxSendPlugin::xmlTypeName)
            return property == te::IDs::auxSendSliderPos ? juce::StringArray { "send level" } : juce::StringArray();

        return { property.toString() };
    }
} // namespace

//==============================================================================
te::AutomatableParameter::Ptr findParameter (const te::Edit& edit, const ParameterRef& parameter)
{
    if (! parameter.isValid())
        return {};

    // A device's, a modulator's or a track's macros' (ParameterRef.h), while it is in the song: Tracktion keeps a device
    // that was removed, for undo.
    const auto id = toTracktion (parameter.device);

    if (const auto device = te::findPluginForID (edit, id))
        return device->getAutomatableParameterByID (parameter.parameter);

    if (const auto modulator = te::findModifierForID (edit, id))
        return modulator->getAutomatableParameterByID (parameter.parameter);

    for (auto* macros : te::getAllMacroParameterLists (edit))
        if (macros->itemID == id)
            return macros->getAutomatableParameterByID (parameter.parameter);

    return {};
}

ParameterRef getParameterRef (const te::AutomatableParameter& parameter)
{
    if (auto* device = parameter.getPlugin())
        return { getDeviceID (*device), parameter.paramID };

    return {};
}

te::Track* getTrackOfParameter (const te::AutomatableParameter& parameter)
{
    auto* device = parameter.getPlugin();

    if (device == nullptr)
        return nullptr;

    if (auto* track = device->getOwnerTrack())
        return track;

    if (device->isInRack())
        return getTrackOfContainerDevice (*device);

    // The master's volume is a device of the Edit's own.
    auto& edit = device->edit;

    if (device == edit.getMasterVolumePlugin().get() || edit.getMasterPluginList().contains (device))
        return edit.getMasterTrack();

    return nullptr;
}

juce::ReferenceCountedArray<te::AutomatableParameter> getAutomatableParameters (te::Track& track)
{
    juce::ReferenceCountedArray<te::AutomatableParameter> parameters;
    juce::Array<te::Plugin*> devices;

    if (track.isMasterTrack())
    {
        if (const auto volume = track.edit.getMasterVolumePlugin())
        {
            parameters.add (volume->volParam);
            parameters.add (volume->panParam);
        }

        devices = getMasterDevices (track.edit);
    }
    else
    {
        if (auto* volume = dynamic_cast<te::VolumeAndPanPlugin*> (getVolumeDevice (track)))
        {
            parameters.add (volume->volParam);
            parameters.add (volume->panParam);
        }
        else if (auto* vca = dynamic_cast<te::VCAPlugin*> (getVolumeDevice (track)))
        {
            parameters.add (vca->volParam);
        }

        for (auto* send : getSends (track))
            parameters.add (send->gain);

        devices = getDevices (track);
    }

    // A device's mix (Mixer.h: getMix()) is its dry and wet levels together, which its knob sets: not two parameters. A
    // container's own parameters are Tracktion's levels in and out, which Phazite doesn't show: its devices' follow it.
    const auto addParametersOf = [&parameters] (te::Plugin& device)
    {
        const auto mix = getMixParameters (device);

        for (auto* parameter : device.getAutomatableParameters())
            if (! mix.contains (parameter))
                parameters.add (parameter);
    };

    for (auto* device : devices)
    {
        if (! isContainer (*device))
            addParametersOf (*device);

        for (auto* inside : getContainerDevices (*device))
            addParametersOf (*inside);
    }

    parameters.removeObject (nullptr);
    return parameters;
}

bool isMixerControl (const te::Plugin* device)
{
    return dynamic_cast<const te::VolumeAndPanPlugin*> (device) != nullptr || dynamic_cast<const te::VCAPlugin*> (device) != nullptr
           || dynamic_cast<const te::AuxSendPlugin*> (device) != nullptr;
}

juce::String getParameterName (const te::AutomatableParameter& parameter)
{
    auto* device = parameter.getPlugin();

    if (auto* send = dynamic_cast<te::AuxSendPlugin*> (device))
    {
        const auto* returnTrack = findReturnTrack (send->edit, send->getBusNumber());
        return "Send to " + (returnTrack != nullptr ? returnTrack->getName() : juce::String ("Return"));
    }

    if (auto* volume = dynamic_cast<te::VolumeAndPanPlugin*> (device))
        return &parameter == volume->panParam.get() ? "Pan" : "Volume";

    if (isMixerControl (device))
        return "Volume"; // a folder's, which is a VCA

    return parameter.getParameterName();
}

//==============================================================================
CurveRef CurveAccess::getRef() const
{
    return { parameter != nullptr ? getParameterRef (*parameter) : ParameterRef(), clip != nullptr ? getClipID (*clip) : ClipID() };
}

CurveAccess getOwnCurve (te::AutomatableParameter& parameter)
{
    return { &parameter, &parameter.getCurve(), nullptr, {} };
}

std::optional<CurveAccess> findCurve (const te::Edit& edit, const CurveRef& ref)
{
    const auto parameter = findParameter (edit, ref.parameter);

    if (parameter == nullptr)
        return {};

    if (! ref.clip.isValid())
        return getOwnCurve (*parameter);

    auto* clip = te::findClipForID (edit, toTracktion (ref.clip));
    const auto modifier = clip != nullptr ? findClipCurve (*clip, parameter->paramID) : nullptr;

    if (modifier == nullptr)
        return {};

    return CurveAccess { parameter, &modifier->getCurve (te::CurveModifierType::absolute).curve, clip, modifier };
}

//==============================================================================
std::vector<AutomationPoint> getPoints (const CurveAccess& access)
{
    std::vector<AutomationPoint> points;
    const auto& curve = *access.curve;

    for (int i = 0; i < curve.getNumPoints(); ++i)
        points.push_back ({ getPointBeat (access, i), curve.getPointValue (i), curve.getPointCurve (i) });

    return points;
}

std::vector<float> getValues (const CurveAccess& access, BeatRange range, int numValues)
{
    std::vector<float> values;
    const auto& curve = *access.curve;

    if (curve.getNumPoints() == 0 || numValues <= 0)
        return values;

    values.reserve ((size_t) numValues);
    const auto step = numValues > 1 ? range.getLength() / (double) (numValues - 1) : BeatDuration();

    for (int i = 0; i < numValues; ++i)
        values.push_back (
            curve.getValueAt (toCurvePosition (access, range.getStart() + step * (double) i), access.parameter->getCurrentBaseValue()));

    return values;
}

int addPoint (const CurveAccess& access, AutomationPoint point)
{
    const auto index = access.curve->addPoint (toCurvePosition (access, point.beat), limitValue (*access.parameter, point.value),
                                               limitCurve (point.curve), undoManagerOf (*access.parameter));
    curveEdited (access);
    return index;
}

BeatDuration movePoints (const CurveAccess& access, const std::vector<int>& requested, BeatDuration distance, float valueChange)
{
    auto& curve = *access.curve;
    const auto& parameter = *access.parameter;
    const auto indices = getValidIndices (curve, requested);

    if (indices.empty())
        return {};

    const auto points = getPoints (access);
    std::vector<bool> moving (points.size(), false);

    for (const auto index : indices)
        moving[(size_t) index] = true;

    // The distance, cut so that no point passes one that stays or goes before the curve's origin (the song's start, or
    // an automation clip's content start); the change, so that every value stays in range.
    auto earliest = -std::numeric_limits<double>::max(), latest = std::numeric_limits<double>::max();
    auto lowest = -std::numeric_limits<float>::max(), highest = std::numeric_limits<float>::max();
    const auto origin = getOrigin (access).inBeats();

    for (const auto index : indices)
    {
        const auto& point = points[(size_t) index];
        auto before = origin;

        for (auto i = index - 1; i >= 0; --i)
        {
            if (! moving[(size_t) i])
            {
                before = points[(size_t) i].beat.inBeats();
                break;
            }
        }

        earliest = std::max (earliest, before - point.beat.inBeats());

        for (auto i = (size_t) index + 1; i < points.size(); ++i)
        {
            if (! moving[i])
            {
                latest = std::min (latest, points[i].beat.inBeats() - point.beat.inBeats());
                break;
            }
        }

        lowest = std::max (lowest, parameter.valueRange.start - point.value);
        highest = std::min (highest, parameter.valueRange.end - point.value);
    }

    const auto beats = juce::jlimit (std::min (earliest, 0.0), std::max (latest, 0.0), distance.inBeats());
    const auto change = juce::jlimit (std::min (lowest, 0.0f), std::max (highest, 0.0f), valueChange);
    auto* undoManager = undoManagerOf (parameter);

    // In the order that keeps the points in time order all along.
    const auto move = [&] (int index)
    {
        const auto& point = points[(size_t) index];
        curve.setPointPosition (index, toCurvePosition (access, point.beat + BeatDuration::fromBeats (beats)), undoManager);
        curve.setPointValue (index, limitValue (parameter, point.value + change), undoManager);
    };

    if (beats > 0.0)
        std::for_each (indices.rbegin(), indices.rend(), move);
    else
        std::for_each (indices.begin(), indices.end(), move);

    curveEdited (access);
    return BeatDuration::fromBeats (beats);
}

void setCurve (const CurveAccess& access, int index, float curveValue)
{
    if (juce::isPositiveAndBelow (index, access.curve->getNumPoints()))
    {
        access.curve->setCurveValue (index, limitCurve (curveValue), undoManagerOf (*access.parameter));
        curveEdited (access);
    }
}

void deletePoints (const CurveAccess& access, const std::vector<int>& requested)
{
    const auto indices = getValidIndices (*access.curve, requested);

    for (auto i = indices.rbegin(); i != indices.rend(); ++i)
        access.curve->removePoint (*i, undoManagerOf (*access.parameter));

    curveEdited (access);
}

void replacePoints (const CurveAccess& access, BeatRange range, const std::vector<AutomationPoint>& points)
{
    auto& curve = *access.curve;
    auto* undoManager = undoManagerOf (*access.parameter);

    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = getPointBeat (access, i); beat >= range.getStart() && beat <= range.getEnd())
            curve.removePoint (i, undoManager);

    // Each after any at its beat already, so that two at one beat stay in the order given: a jump.
    for (const auto& point : points)
        curve.addPoint (toCurvePosition (access, point.beat), limitValue (*access.parameter, point.value), limitCurve (point.curve),
                        undoManager);

    curveEdited (access);
}

std::vector<int> duplicatePoints (const CurveAccess& access, const std::vector<int>& requested, BeatDuration minimumSpan)
{
    auto& curve = *access.curve;
    const auto indices = getValidIndices (curve, requested);

    if (indices.empty())
        return {};

    const auto points = getPoints (access);
    const auto first = points[(size_t) indices.front()].beat, last = points[(size_t) indices.back()].beat;
    const auto span = std::max (last - first, minimumSpan);
    auto* undoManager = undoManagerOf (*access.parameter);

    // The copies replace what is after the last original, up to the last copy; a copy that lands on the last original's
    // beat goes after it, a jump.
    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = getPointBeat (access, i); beat > first + span && beat <= last + span)
            curve.removePoint (i, undoManager);

    std::vector<int> copies;

    for (const auto index : indices)
    {
        const auto& point = points[(size_t) index];
        copies.push_back (curve.addPoint (toCurvePosition (access, point.beat + span), point.value, point.curve, undoManager));
    }

    curveEdited (access);
    return copies;
}

//==============================================================================
te::AutomatableParameter::Ptr getAutomationClipParameter (const te::Clip& clip)
{
    if (! isAutomationClip (clip))
        return {};

    // The first curve's parameter.
    for (const auto& modifier : clip.state.getChildWithName (te::IDs::AUTOMATIONCURVES))
    {
        const auto device = te::EditItemID::fromProperty (modifier, te::IDs::source);
        const auto parameterID = modifier[te::IDs::paramID].toString();

        if (const auto found = findParameter (clip.edit, { DeviceID::fromRaw (device.getRawID()), parameterID }))
            return found;
    }

    return {};
}

AutomationTargets::AutomationTargets (const te::Clip& clip)
{
    if (isAutomationClip (clip))
        curves = clip.state.getChildWithName (te::IDs::AUTOMATIONCURVES).createCopy();
}

void AutomationTargets::setDevice (te::EditItemID from, te::EditItemID to)
{
    for (auto curve : curves)
        if (te::EditItemID::fromProperty (curve, te::IDs::source) == from)
            to.setProperty (curve, te::IDs::source, nullptr);
}

void AutomationTargets::restore (te::Clip& clip) const
{
    if (! curves.isValid() || dynamic_cast<te::MidiClip*> (&clip) == nullptr)
        return;

    auto& edit = clip.edit;
    auto* undoManager = &edit.getUndoManager();
    auto* list = clip.getAutomationCurveList (true); // so that a curve added below reaches its parameter

    if (list == nullptr)
        return;

    auto state = clip.state.getChildWithName (te::IDs::AUTOMATIONCURVES);

    for (const auto& saved : curves)
    {
        // The clip's curve for the parameter, as Tracktion left it: pointed at another device, or gone.
        auto current = state.getChildWithProperty (te::IDs::paramID, saved[te::IDs::paramID]);

        if (current.isValid())
        {
            if (current[te::IDs::source] != saved[te::IDs::source])
                current.setProperty (te::IDs::source, saved[te::IDs::source], undoManager); // it moves its assignment
        }
        else
        {
            juce::ValueTree holder ("CURVES"), added = saved.createCopy();
            holder.appendChild (added, nullptr);
            te::assignNewIDsToAutomationCurveModifiers (edit, holder);
            holder.removeChild (added, nullptr);
            state.appendChild (added, undoManager);
        }
    }

    // Each curve set its parameter, also one whose device was right already.
    for (const auto& modifier : list->getItems())
        if (const auto parameter = te::getParameter (*modifier);
            parameter != nullptr && ! parameter->getModifiers().contains (modifier.get()))
            parameter->addModifier (*modifier);
}

void copyAutomationTargets (const te::Track& original, te::Track& copy, const te::EditItemID::IDMap& devicesInContainers)
{
    // The copy's devices are where the original's are in its list, those in its containers as the map says, and its
    // clips where the original's are in theirs.
    const auto originalDevices = original.getAllPlugins(), copiedDevices = copy.getAllPlugins();
    auto* originalClips = dynamic_cast<const te::ClipTrack*> (&original);
    auto* copiedClips = dynamic_cast<te::ClipTrack*> (&copy);

    if (originalClips != nullptr && copiedClips != nullptr)
    {
        const auto& clips = originalClips->getClips();
        const auto& copies = copiedClips->getClips();

        for (int i = 0; i < clips.size() && i < copies.size(); ++i)
        {
            AutomationTargets targets (*clips[i]);

            for (int j = 0; j < originalDevices.size() && j < copiedDevices.size(); ++j)
                targets.setDevice (originalDevices[j]->itemID, copiedDevices[j]->itemID);

            for (const auto& [from, to] : devicesInContainers)
                targets.setDevice (from, to);

            targets.restore (*copies[i]);
        }
    }

    // A folder's copy holds copies of its tracks, in the same order.
    const auto originalTracks = original.getAllSubTracks (false), copiedTracks = copy.getAllSubTracks (false);

    for (int i = 0; i < originalTracks.size() && i < copiedTracks.size(); ++i)
        copyAutomationTargets (*originalTracks[i], *copiedTracks[i], devicesInContainers);
}

te::MidiClip* createAutomationClip (te::ClipTrack& track, te::AutomatableParameter& parameter, te::BeatRange range, float value)
{
    auto& edit = track.edit;
    auto* clip = track.insertMIDIClip (edit.tempoSequence.toTime (range), nullptr).get();

    if (clip == nullptr)
        return nullptr;

    auto* list = clip->getAutomationCurveList (true);
    const auto modifier = list != nullptr ? list->addCurve (parameter) : nullptr;

    if (modifier == nullptr)
    {
        clip->removeFromParent();
        return nullptr;
    }

    // A flat line at the value, across the clip.
    const CurveAccess access { &parameter, &modifier->getCurve (te::CurveModifierType::absolute).curve, clip, modifier };
    const auto start = fromTracktion (range.getStart());
    addPoint (access, { start, value });
    addPoint (access, { start + fromTracktion (range.getLength()), value });
    return clip;
}

//==============================================================================
std::vector<AutomationLane> getLanes (const te::Track& track)
{
    std::vector<AutomationLane> lanes;

    for (const auto& child : track.state)
    {
        if (! schema::isTrackViewElement (child))
            continue;

        const auto parameter = readLane (child);
        const auto height = juce::jlimit (AutomationLane::minHeight, AutomationLane::maxHeight,
                                          (int) child.getProperty (schema::laneHeight, AutomationLane::defaultHeight));

        if (isOnTrack (track, parameter))
            lanes.push_back ({ parameter, height });
    }

    return lanes;
}

void showLane (te::Track& track, const ParameterRef& parameter)
{
    if (findLaneState (track, parameter).isValid() || ! isOnTrack (track, parameter))
        return;

    juce::ValueTree lane (schema::PHAZITELANE);
    writeLane (lane, parameter);
    track.state.appendChild (lane, nullptr);
}

void hideLane (te::Track& track, const ParameterRef& parameter)
{
    if (auto lane = findLaneState (track, parameter); lane.isValid())
        track.state.removeChild (lane, nullptr);
}

void hideAllLanes (te::Track& track)
{
    for (auto i = track.state.getNumChildren(); --i >= 0;)
        if (schema::isTrackViewElement (track.state.getChild (i)))
            track.state.removeChild (i, nullptr);
}

void replaceLane (te::Track& track, const ParameterRef& parameter, const ParameterRef& replacement)
{
    auto lane = findLaneState (track, parameter);

    if (! lane.isValid() || parameter == replacement || ! isOnTrack (track, replacement))
        return;

    hideLane (track, replacement); // a lane that shows it already moves here
    writeLane (lane, replacement);
}

void setLaneHeight (te::Track& track, const ParameterRef& parameter, int height)
{
    if (auto lane = findLaneState (track, parameter); lane.isValid())
        lane.setProperty (schema::laneHeight, juce::jlimit (AutomationLane::minHeight, AutomationLane::maxHeight, height), nullptr);
}

void copyLanes (const te::Track& original, te::Track& copy, const te::EditItemID::IDMap& devicesInContainers)
{
    // The copy's devices are where the original's are in its list, and those in its containers as the map says.
    const auto originals = original.getAllPlugins(), copies = copy.getAllPlugins();

    for (auto lane : copy.state)
    {
        if (! schema::isTrackViewElement (lane))
            continue;

        auto parameter = readLane (lane);

        for (int i = 0; i < originals.size() && i < copies.size(); ++i)
            if (getDeviceID (*originals[i]) == parameter.device)
                parameter.device = getDeviceID (*copies[i]);

        if (const auto inside = devicesInContainers.find (toTracktion (parameter.device)); inside != devicesInContainers.end())
            parameter.device = DeviceID::fromRaw (inside->second.getRawID());

        writeLane (lane, parameter);
    }

    // A folder's copy holds copies of its tracks, in the same order.
    const auto originalTracks = original.getAllSubTracks (false), copiedTracks = copy.getAllSubTracks (false);

    for (int i = 0; i < originalTracks.size() && i < copiedTracks.size(); ++i)
        copyLanes (*originalTracks[i], *copiedTracks[i], devicesInContainers);
}

AutomationMode getAutomationMode (const te::Track& track)
{
    switch (track.automationMode.get())
    {
        case te::AutomationMode::touch:
            return AutomationMode::touch;
        case te::AutomationMode::latch:
            return AutomationMode::latch;
        case te::AutomationMode::write:
            return AutomationMode::write;
        case te::AutomationMode::read:
        default:
            return AutomationMode::read;
    }
}

void setAutomationMode (te::Track& track, AutomationMode mode)
{
    track.state.setProperty (te::IDs::automationMode, te::toString (toTracktion (mode)), nullptr);
}

bool isWritingAutomation (const te::Edit& edit)
{
    for (auto* track : te::getAllTracks (edit))
        if (track->automationMode.get() != te::AutomationMode::read)
            return true;

    return false;
}

bool isAutomatedValue (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! tree.hasType (te::IDs::PLUGIN))
        return false;

    const auto parameters = getParametersStoredIn (tree, property);

    // Its own curve, or what else sets it as the song plays: an automation clip's curve (or later a modulator), assigned
    // to it in its device's state.
    for (const auto& child : tree)
        if (child.hasType (te::IDs::AUTOMATIONCURVE) && parameters.contains (child[te::IDs::paramID].toString()))
            return true;

    for (const auto& assignment : tree.getChildWithName (te::IDs::MODIFIERASSIGNMENTS))
        if (parameters.contains (assignment[te::IDs::paramID].toString()))
            return true;

    return false;
}

void restoreValuesSetByHand (juce::ValueTree& state)
{
    for (auto child : state)
        restoreValuesSetByHand (child);

    // Plug-ins put theirs back themselves.
    if (! state.hasType (te::IDs::PLUGIN) || state[te::IDs::type].toString() == te::ExternalPlugin::xmlTypeName)
        return;

    // Tracktion writes each parameter's ID and value one after the other (AutomatableEditItem::saveChangedParametersToState()).
    const auto* saved = state[te::IDs::parameters].getBinaryData();

    if (saved == nullptr)
        return;

    juce::MemoryInputStream stream (*saved, false);

    while (! stream.isExhausted())
    {
        const auto parameterID = stream.readString();
        const auto value = stream.readFloat();

        for (int i = 0; i < state.getNumProperties(); ++i)
            if (const auto property = state.getPropertyName (i); getParametersStoredIn (state, property).contains (parameterID))
                state.setProperty (property, value, nullptr);
    }
}

//==============================================================================
std::optional<PointSelection> getSelectedPoints (const te::SelectionManager& manager)
{
    auto* selected = manager.getFirstItemOfType<SelectedAutomationPoints>();
    auto* edit = manager.getEdit();

    if (selected == nullptr || edit == nullptr)
        return {};

    const auto access = findCurve (*edit, selected->curve);

    if (! access.has_value())
        return {};

    auto indices = getValidIndices (*access->curve, selected->indices);

    if (indices.empty())
        return {};

    return PointSelection { *access, std::move (indices) };
}

void selectPoints (te::SelectionManager& manager, const CurveAccess& access, const std::vector<int>& indices)
{
    if (auto* selected = manager.getFirstItemOfType<SelectedAutomationPoints>())
    {
        selected->curve = access.getRef();
        selected->indices = getValidIndices (*access.curve, indices);
        manager.sendChangeMessage();
    }
}

//==============================================================================
AutomationOnBeats::AutomationOnBeats (te::Edit& e)
    : edit (e)
{
    for (const auto& device : te::getAllPlugins (edit, true))
    {
        for (auto* parameter : device->getAutomatableParameters())
        {
            const auto& curve = parameter->getCurve();

            if (curve.getNumPoints() == 0 || curve.timeBase != te::AutomationCurve::TimeBase::time)
                continue;

            Curve kept { parameter, {} };

            for (int i = 0; i < curve.getNumPoints(); ++i)
                kept.beats.push_back (te::toBeats (curve.getPointPosition (i), edit.tempoSequence));

            curves.push_back (std::move (kept));
        }
    }
}

AutomationOnBeats::~AutomationOnBeats()
{
    for (const auto& kept : curves)
    {
        auto& curve = kept.parameter->getCurve();

        for (int i = 0; i < curve.getNumPoints() && i < (int) kept.beats.size(); ++i)
            curve.setPointPosition (i, edit.tempoSequence.toTime (kept.beats[(size_t) i]), &edit.getUndoManager());

        followCurve (*kept.parameter);
    }
}

//==============================================================================
AutomationWatcher::AutomationWatcher (te::Edit& e)
    : edit (e),
      wasPlaying (e.getTransport().isPlaying())
{
    e.getParameterChangeHandler().addListener (*this);
    e.getTransport().addChangeListener (this);
}

AutomationWatcher::~AutomationWatcher()
{
    // The Edit may have gone first: then so have its transport and its parameters' listeners.
    if (auto* e = edit.get())
    {
        e->getTransport().removeChangeListener (this);
        e->getParameterChangeHandler().removeListener (*this);
    }
}

void AutomationWatcher::writingChanged()
{
    if (auto* e = edit.get(); e != nullptr && wasPlaying && recording == nullptr && isWritingAutomation (*e))
        recording = std::make_unique<RecordingStep> (*e, "Record Automation");
}

void AutomationWatcher::pluginParameterChanged (te::AutomatableParameter& parameter, bool isFollowingAutomation)
{
    if (isFollowingAutomation || edit == nullptr || edit->isLoading() || ! juce::MessageManager::existsAndIsCurrentThread())
        return;

    if (const auto changed = getParameterRef (parameter); changed.isValid())
    {
        if (std::find (changedAtOnce.begin(), changedAtOnce.end(), changed) == changedAtOnce.end())
            changedAtOnce.push_back (changed);

        triggerAsyncUpdate();
    }
}

void AutomationWatcher::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto playing = edit != nullptr && edit->getTransport().isPlaying();

    if (playing == wasPlaying)
        return;

    wasPlaying = playing;

    if (playing)
    {
        writingChanged();
    }
    else
    {
        // Tracktion writes what it recorded last as it hears of the stop, in this same round of messages: the step ends
        // after it.
        stopRecording = true;
        triggerAsyncUpdate();
    }
}

void AutomationWatcher::handleAsyncUpdate()
{
    // Many parameters changed at once (a device made, a preset loaded) are no one's touch.
    if (! changedAtOnce.empty() && changedAtOnce.size() <= maxTouchedAtOnce)
        lastTouched = changedAtOnce.back();

    changedAtOnce.clear();

    if (std::exchange (stopRecording, false) && ! wasPlaying)
        recording = nullptr;
}
} // namespace phazite
