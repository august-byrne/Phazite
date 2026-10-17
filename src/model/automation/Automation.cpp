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
    // Where a curve keeps a song beat: in seconds, as track automation is, or in beats. Never before the song's start.
    te::EditPosition toCurvePosition (const te::AutomationCurve& curve, BeatPosition beat)
    {
        const auto position = te::BeatPosition::fromBeats (std::max (0.0, beat.inBeats()));

        if (curve.timeBase == te::AutomationCurve::TimeBase::beats)
            return position;

        return curve.edit.tempoSequence.toTime (position);
    }

    BeatPosition getPointBeat (const te::AutomationCurve& curve, int index)
    {
        return fromTracktion (te::toBeats (curve.getPointPosition (index), curve.edit.tempoSequence));
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

    //==========================================================================
    // Version 4's automation clips (convertAutomationClips()): a MIDI clip that held a curve rather than notes.
    bool isVersion4AutomationClip (const te::Clip& clip)
    {
        return dynamic_cast<const te::MidiClip*> (&clip) != nullptr
               && clip.state.getChildWithName (te::IDs::AUTOMATIONCURVES).getChildWithName (te::IDs::AUTOMATIONCURVEMODIFIER).isValid();
    }

    // A clip's curve becomes the parameter's own over where the clip plays, without undo steps.
    void convertClipCurve (const te::Clip& clip, const te::AutomationCurve& clipCurve, te::AutomatableParameter& parameter)
    {
        if (clipCurve.getNumPoints() == 0)
            return;

        auto& tempo = clip.edit.tempoSequence;
        auto& own = parameter.getCurve();

        // Where the clip plays, and where its curve's beat 0 is (its content's start), in song beats.
        const auto times = clip.getPosition().time;
        const auto start = tempo.toBeats (times.getStart()), end = tempo.toBeats (times.getEnd());
        const auto origin = tempo.toBeats (clip.getTimeOfContentBeat (te::BeatPosition()));
        const auto valueOfClipAt = [&clipCurve, origin] (te::BeatPosition beat)
        { return clipCurve.getValueAt (te::BeatPosition::fromBeats ((beat - origin).inBeats()), clipCurve.getPointValue (0)); };

        // Before and after the clip, the parameter played its own curve, or its value.
        const auto value = parameter.getCurrentBaseValue();
        const auto before = own.getValueAt (times.getStart(), value), after = own.getValueAt (times.getEnd(), value);

        for (auto i = own.getNumPoints(); --i >= 0;)
            if (times.containsInclusive (te::toTime (own.getPointPosition (i), tempo)))
                own.removePoint (i, nullptr);

        const auto add = [&own] (te::BeatPosition beat, float pointValue, float bend)
        { own.addPoint (toCurvePosition (own, fromTracktion (beat)), pointValue, bend, nullptr); };

        // Jumps to the clip's curve at its start, which bends on to its next point as it did, and back at its end.
        const auto beatOf = [&clipCurve, &tempo, origin] (int index)
        { return origin + (te::toBeats (clipCurve.getPointPosition (index), tempo) - te::BeatPosition()); };
        auto bendAtStart = 0.0f;

        for (int i = 0; i < clipCurve.getNumPoints(); ++i)
            if (beatOf (i) <= start)
                bendAtStart = clipCurve.getPointCurve (i);

        add (start, before, 0.0f);
        add (start, valueOfClipAt (start), bendAtStart);

        for (int i = 0; i < clipCurve.getNumPoints(); ++i)
            if (const auto beat = beatOf (i); beat > start && beat < end)
                add (beat, clipCurve.getPointValue (i), clipCurve.getPointCurve (i));

        add (end, valueOfClipAt (end), 0.0f);
        add (end, after, 0.0f);
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

    // Before a change to a track's lanes: it keeps one for each lane it shows without keeping one (of a parameter with
    // automation), in the order they show, so that they stay in that order.
    void keepShownLanes (te::Track& track)
    {
        for (const auto& shown : getLanes (track))
        {
            if (findLaneState (track, shown.parameter).isValid())
                continue;

            juce::ValueTree lane (schema::PHAZITELANE);
            writeLane (lane, shown.parameter);
            track.state.appendChild (lane, nullptr);
        }
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
std::vector<AutomationPoint> getPoints (const te::AutomatableParameter& parameter)
{
    std::vector<AutomationPoint> points;
    const auto& curve = parameter.getCurve();

    for (int i = 0; i < curve.getNumPoints(); ++i)
        points.push_back ({ getPointBeat (curve, i), curve.getPointValue (i), curve.getPointCurve (i) });

    return points;
}

std::vector<float> getValues (const te::AutomatableParameter& parameter, BeatRange range, int numValues)
{
    std::vector<float> values;
    const auto& curve = parameter.getCurve();

    if (curve.getNumPoints() == 0 || numValues <= 0)
        return values;

    values.reserve ((size_t) numValues);
    const auto step = numValues > 1 ? range.getLength() / (double) (numValues - 1) : BeatDuration();

    for (int i = 0; i < numValues; ++i)
        values.push_back (
            curve.getValueAt (toCurvePosition (curve, range.getStart() + step * (double) i), parameter.getCurrentBaseValue()));

    return values;
}

int addPoint (te::AutomatableParameter& parameter, AutomationPoint point)
{
    auto& curve = parameter.getCurve();
    const auto index = curve.addPoint (toCurvePosition (curve, point.beat), limitValue (parameter, point.value), limitCurve (point.curve),
                                       undoManagerOf (parameter));
    followCurve (parameter);
    return index;
}

BeatDuration movePoints (te::AutomatableParameter& parameter, const std::vector<int>& requested, BeatDuration distance, float valueChange)
{
    auto& curve = parameter.getCurve();
    const auto indices = getValidIndices (curve, requested);

    if (indices.empty())
        return {};

    const auto points = getPoints (parameter);
    std::vector<bool> moving (points.size(), false);

    for (const auto index : indices)
        moving[(size_t) index] = true;

    // The distance, cut so that no point passes one that stays or goes before the song's start; the change, so that every
    // value stays in range.
    auto earliest = -std::numeric_limits<double>::max(), latest = std::numeric_limits<double>::max();
    auto lowest = -std::numeric_limits<float>::max(), highest = std::numeric_limits<float>::max();

    for (const auto index : indices)
    {
        const auto& point = points[(size_t) index];
        auto before = 0.0;

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
        curve.setPointPosition (index, toCurvePosition (curve, point.beat + BeatDuration::fromBeats (beats)), undoManager);
        curve.setPointValue (index, limitValue (parameter, point.value + change), undoManager);
    };

    if (beats > 0.0)
        std::for_each (indices.rbegin(), indices.rend(), move);
    else
        std::for_each (indices.begin(), indices.end(), move);

    followCurve (parameter);
    return BeatDuration::fromBeats (beats);
}

void setCurve (te::AutomatableParameter& parameter, int index, float curveValue)
{
    if (auto& curve = parameter.getCurve(); juce::isPositiveAndBelow (index, curve.getNumPoints()))
    {
        curve.setCurveValue (index, limitCurve (curveValue), undoManagerOf (parameter));
        followCurve (parameter);
    }
}

void deletePoints (te::AutomatableParameter& parameter, const std::vector<int>& requested)
{
    auto& curve = parameter.getCurve();
    const auto indices = getValidIndices (curve, requested);

    for (auto i = indices.rbegin(); i != indices.rend(); ++i)
        curve.removePoint (*i, undoManagerOf (parameter));

    followCurve (parameter);
}

void replacePoints (te::AutomatableParameter& parameter, BeatRange range, const std::vector<AutomationPoint>& points)
{
    auto& curve = parameter.getCurve();
    auto* undoManager = undoManagerOf (parameter);

    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = getPointBeat (curve, i); beat >= range.getStart() && beat <= range.getEnd())
            curve.removePoint (i, undoManager);

    // Each after any at its beat already, so that two at one beat stay in the order given: a jump.
    for (const auto& point : points)
        curve.addPoint (toCurvePosition (curve, point.beat), limitValue (parameter, point.value), limitCurve (point.curve), undoManager);

    followCurve (parameter);
}

std::vector<int> duplicatePoints (te::AutomatableParameter& parameter, const std::vector<int>& requested, BeatDuration minimumSpan)
{
    auto& curve = parameter.getCurve();
    const auto indices = getValidIndices (curve, requested);

    if (indices.empty())
        return {};

    const auto points = getPoints (parameter);
    const auto first = points[(size_t) indices.front()].beat, last = points[(size_t) indices.back()].beat;
    const auto span = std::max (last - first, minimumSpan);
    auto* undoManager = undoManagerOf (parameter);

    // The copies replace what is after the last original, up to the last copy; a copy that lands on the last original's
    // beat goes after it, a jump.
    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = getPointBeat (curve, i); beat > first + span && beat <= last + span)
            curve.removePoint (i, undoManager);

    std::vector<int> copies;

    for (const auto index : indices)
    {
        const auto& point = points[(size_t) index];
        copies.push_back (curve.addPoint (toCurvePosition (curve, point.beat + span), point.value, point.curve, undoManager));
    }

    followCurve (parameter);
    return copies;
}

//==============================================================================
void convertAutomationClips (te::Edit& edit)
{
    // Earliest first, so that where two set one parameter at once, the later one's curve stays.
    juce::Array<te::Clip*> clips;

    for (auto* track : te::getClipTracks (edit))
        for (auto* clip : track->getClips())
            if (isVersion4AutomationClip (*clip))
                clips.add (clip);

    std::stable_sort (clips.begin(), clips.end(),
                      [] (const te::Clip* a, const te::Clip* b) { return a->getPosition().getStart() < b->getPosition().getStart(); });

    juce::Array<te::AudioTrack*> holders;
    juce::ReferenceCountedArray<te::AutomatableParameter> converted;

    for (auto* clip : clips)
    {
        if (auto* list = clip->getAutomationCurveList (false))
        {
            for (const auto& modifier : list->getItems())
            {
                if (const auto parameter = te::getParameter (*modifier))
                {
                    convertClipCurve (*clip, modifier->getCurve (te::CurveModifierType::absolute).curve, *parameter);
                    parameter->removeModifier (*modifier);
                    converted.addIfNotAlreadyThere (parameter);
                }
            }
        }

        if (auto* track = dynamic_cast<te::AudioTrack*> (clip->getTrack()))
            holders.addIfNotAlreadyThere (track);

        clip->removeFromParent();
    }

    // The tracks Phazite made for the master's and folders' automation clips, which hold nothing now.
    for (auto* track : holders)
        if (track->getName() == "Automation" && track->getClips().isEmpty() && getDevices (*track).isEmpty())
            edit.deleteTrack (track);

    for (auto* parameter : converted)
        followCurve (*parameter);

    edit.getUndoManager().clearUndoHistory();
}

//==============================================================================
std::vector<AutomationLane> getLanes (te::Track& track)
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
            lanes.push_back ({ parameter, height, (bool) child[schema::laneCollapsed] });
    }

    // Automation never hides: a parameter with automation that the track keeps no lane for shows one after the others.
    for (auto* parameter : getAutomatableParameters (track))
    {
        const auto ref = getParameterRef (*parameter);

        if (parameter->hasAutomationPoints()
            && std::none_of (lanes.begin(), lanes.end(), [&ref] (const AutomationLane& lane) { return lane.parameter == ref; }))
            lanes.push_back ({ ref });
    }

    return lanes;
}

void showLane (te::Track& track, const ParameterRef& parameter)
{
    if (! isOnTrack (track, parameter))
        return;

    keepShownLanes (track);

    if (auto lane = findLaneState (track, parameter); lane.isValid())
    {
        lane.removeProperty (schema::laneCollapsed, nullptr);
        return;
    }

    juce::ValueTree lane (schema::PHAZITELANE);
    writeLane (lane, parameter);
    track.state.appendChild (lane, nullptr);
}

void removeLane (te::Track& track, const ParameterRef& parameter)
{
    if (auto lane = findLaneState (track, parameter); lane.isValid())
        track.state.removeChild (lane, nullptr);
}

void replaceLane (te::Track& track, const ParameterRef& parameter, const ParameterRef& replacement)
{
    keepShownLanes (track);
    auto lane = findLaneState (track, parameter);

    if (! lane.isValid() || parameter == replacement || ! isOnTrack (track, replacement))
        return;

    removeLane (track, replacement); // a lane that shows it already moves here
    writeLane (lane, replacement);
}

void setLaneHeight (te::Track& track, const ParameterRef& parameter, int height)
{
    keepShownLanes (track);

    if (auto lane = findLaneState (track, parameter); lane.isValid())
        lane.setProperty (schema::laneHeight, juce::jlimit (AutomationLane::minHeight, AutomationLane::maxHeight, height), nullptr);
}

void setLaneCollapsed (te::Track& track, const ParameterRef& parameter, bool collapsed)
{
    keepShownLanes (track);

    if (auto lane = findLaneState (track, parameter); lane.isValid())
    {
        if (collapsed)
            lane.setProperty (schema::laneCollapsed, true, nullptr);
        else
            lane.removeProperty (schema::laneCollapsed, nullptr);
    }
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

    // Its own curve, or what else moves it as the song plays: a modulator, assigned to it in its device's state.
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

    const auto parameter = findParameter (*edit, selected->parameter);

    if (parameter == nullptr)
        return {};

    auto indices = getValidIndices (parameter->getCurve(), selected->indices);

    if (indices.empty())
        return {};

    return PointSelection { parameter, std::move (indices) };
}

void selectPoints (te::SelectionManager& manager, const te::AutomatableParameter& parameter, const std::vector<int>& indices)
{
    if (auto* selected = manager.getFirstItemOfType<SelectedAutomationPoints>())
    {
        selected->parameter = getParameterRef (parameter);
        selected->indices = getValidIndices (parameter.getCurve(), indices);
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
