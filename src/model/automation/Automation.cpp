// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Automation.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/mirror/internal/Mirror.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Playback.h"

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

    float limitValue (const te::AutomatableParameter& parameter, float value)
    {
        return juce::jlimit (parameter.valueRange.start, parameter.valueRange.end, value);
    }

    float limitCurve (float curve)
    {
        return juce::jlimit (-1.0f, 1.0f, curve);
    }

    // While the song plays, the engine follows the curve; while it doesn't, the value at the playhead is set here, so
    // that controls show it.
    void followCurve (te::AutomatableParameter& parameter)
    {
        auto& edit = parameter.getEdit();

        if (! isSongPlaying (edit))
            parameter.updateToFollowCurve (getPlayheadTime (edit));
    }

    //==========================================================================
    // The song's curves (Schema.h: AUTOMATION).

    juce::ValueTree findCurveIn (const juce::ValueTree& automation, const ParameterRef& ref)
    {
        const auto device = toTracktion (ref.device).toString();

        for (const auto& curve : automation)
            if (curve[schema::curveDevice].toString() == device && curve[schema::curveParameter].toString() == ref.parameter)
                return curve;

        return {};
    }

    // A parameter's curve in the song, edited point by point as Tracktion's AutomationCurve is, in beats, each change with
    // the Edit's undo manager. Points keep time order, one added at a beat that has points going after them, as Tracktion
    // adds them. Tracktion's curve follows (mirror::reconcileCurve()) before the curve's values are asked for, which
    // Tracktion reckons until Phazite plays the song itself (T2), and as the edit ends, so that what plays is always the
    // song's. One at a time for a parameter: each holds the curve's state as it found it.
    class SongCurve
    {
    public:
        explicit SongCurve (te::AutomatableParameter& p)
            : parameter (p),
              edit (p.getEdit()),
              ref (getParameterRef (p)),
              state (findCurveState (edit, ref))
        {
        }

        ~SongCurve() { reconcile(); }

        int getNumPoints() const { return state.getNumChildren(); }
        BeatPosition getPointBeat (int index) const { return BeatPosition::fromBeats (state.getChild (index)[schema::pointBeat]); }
        float getPointValue (int index) const { return state.getChild (index)[schema::pointValue]; }
        float getPointCurve (int index) const { return state.getChild (index).getProperty (schema::pointCurve, 0.0f); }

        float getValueAt (BeatPosition beat) const
        {
            reconcile();
            const auto& curve = parameter.getCurve();
            return curve.getValueAt (toCurvePosition (curve, beat), parameter.getCurrentBaseValue());
        }

        // The parameter's value at the playhead in step with the curve (followCurve()).
        void follow() const
        {
            reconcile();
            followCurve (parameter);
        }

        int addPoint (BeatPosition beat, float value, float curve)
        {
            if (! ref.isValid())
                return -1;

            if (! state.isValid())
            {
                auto automation = getPhaziteState (edit).getOrCreateChildWithName (schema::AUTOMATION, getUndo());
                state = juce::ValueTree (schema::CURVE);
                state.setProperty (schema::curveDevice, toTracktion (ref.device).toString(), nullptr);
                state.setProperty (schema::curveParameter, ref.parameter, nullptr);
                automation.appendChild (state, getUndo());
            }

            auto index = 0;

            while (index < getNumPoints() && getPointBeat (index) <= beat)
                ++index;

            juce::ValueTree point (schema::POINT);
            point.setProperty (schema::pointBeat, std::max (0.0, beat.inBeats()), nullptr);
            point.setProperty (schema::pointValue, value, nullptr);

            if (! juce::exactlyEqual (curve, 0.0f))
                point.setProperty (schema::pointCurve, curve, nullptr);

            state.addChild (point, index, getUndo());
            changed = true;
            return index;
        }

        void removePoint (int index)
        {
            state.removeChild (index, getUndo());

            // A curve without points goes.
            if (getNumPoints() == 0)
            {
                state.getParent().removeChild (state, getUndo());
                state = {};
            }

            changed = true;
        }

        void setPoint (int index, BeatPosition beat, float value)
        {
            auto point = state.getChild (index);
            point.setProperty (schema::pointBeat, std::max (0.0, beat.inBeats()), getUndo());
            point.setProperty (schema::pointValue, value, getUndo());
            changed = true;
        }

        void setCurve (int index, float curve)
        {
            if (auto point = state.getChild (index); juce::exactlyEqual (curve, 0.0f))
                point.removeProperty (schema::pointCurve, getUndo());
            else
                point.setProperty (schema::pointCurve, curve, getUndo());

            changed = true;
        }

        // The points in a range that change nothing, as Tracktion's removeRedundantPoints() finds them: those whose
        // neighbours have the same value, or the next of which is the same point, or which lie at one time between two.
        void removeRedundantPoints (BeatRange range)
        {
            constexpr auto threshold = 0.0001;
            const auto secondsBetween = [this] (int a, int b)
            {
                const auto& tempo = edit.tempoSequence;
                return (tempo.toTime (toTracktion (getPointBeat (a))) - tempo.toTime (toTracktion (getPointBeat (b)))).inSeconds();
            };

            for (int i = getNumPoints(); --i >= 0;)
            {
                if (! range.contains (getPointBeat (i)))
                    continue;

                const auto v = getPointValue (i);
                const auto sameLeft = i <= 0 || std::abs (getPointValue (i - 1) - v) < threshold;
                const auto sameRight = i >= getNumPoints() - 1 || std::abs (getPointValue (i + 1) - v) < threshold;

                if ((sameLeft && sameRight)
                    || (i < getNumPoints() - 1 && std::abs (getPointValue (i + 1) - v) < threshold
                        && std::abs (secondsBetween (i + 1, i)) < threshold)
                    || (i > 0 && i < getNumPoints() - 1 && std::abs (secondsBetween (i - 1, i)) < threshold
                        && std::abs (secondsBetween (i + 1, i)) < threshold))
                {
                    removePoint (i);
                    continue;
                }

                if (getNumPoints() <= 1)
                    break;
            }
        }

    private:
        te::AutomatableParameter& parameter;
        te::Edit& edit;
        const ParameterRef ref;
        juce::ValueTree state;
        mutable bool changed = false;

        juce::UndoManager* getUndo() const { return &getSongUndoManager (edit); }

        void reconcile() const
        {
            if (std::exchange (changed, false))
                mirror::reconcileCurve (edit, ref);
        }

        JUCE_DECLARE_NON_COPYABLE (SongCurve)
    };

    int countPoints (const te::AutomatableParameter& parameter)
    {
        return findCurveState (parameter.getEdit(), getParameterRef (parameter)).getNumChildren();
    }

    // The parameters the song has curves for that are there, not those of devices gone (whose curves the song keeps for
    // undo).
    std::vector<te::AutomatableParameter::Ptr> getParametersWithCurves (const te::Edit& edit)
    {
        std::vector<te::AutomatableParameter::Ptr> parameters;

        for (const auto& curve : getCurveStates (edit))
            if (auto parameter = findParameter (edit, getCurveParameter (curve)))
                parameters.push_back (parameter);

        return parameters;
    }

    //==========================================================================
    // Automation that goes with clips (moveAutomation(), deleteAutomation()).
    const auto justBefore = BeatDuration::fromBeats (1.0e-4); // where an edge's point on the side before it goes

    // Each track's stretches in time order, those that overlap or touch as one.
    std::vector<std::pair<te::Track*, std::vector<BeatRange>>> byTrack (const std::vector<AutomationStretch>& stretches)
    {
        std::vector<std::pair<te::Track*, std::vector<BeatRange>>> tracks;

        for (const auto& stretch : stretches)
        {
            if (stretch.track == nullptr || stretch.range.isEmpty())
                continue;

            auto found = std::find_if (tracks.begin(), tracks.end(), [&stretch] (const auto& each) { return each.first == stretch.track; });

            if (found == tracks.end())
                found = tracks.insert (tracks.end(), { stretch.track, {} });

            found->second.push_back (stretch.range);
        }

        for (auto& entry : tracks)
        {
            auto& ranges = entry.second;
            std::sort (ranges.begin(), ranges.end(), [] (const BeatRange& a, const BeatRange& b) { return a.getStart() < b.getStart(); });
            std::vector<BeatRange> merged;

            for (const auto& range : ranges)
            {
                if (! merged.empty() && range.getStart() <= merged.back().getEnd())
                    merged.back() = { merged.back().getStart(), std::max (merged.back().getEnd(), range.getEnd()) };
                else
                    merged.push_back (range);
            }

            ranges = std::move (merged);
        }

        return tracks;
    }

    // A point where an edge is, at the curve's value there, and one just before it at the value there, so that what lies
    // on either side keeps its shape whatever becomes of the other.
    void addEdge (SongCurve& curve, BeatPosition beat)
    {
        const auto before = curve.getValueAt (beat - justBefore), at = curve.getValueAt (beat);

        if (beat - justBefore > BeatPosition())
            curve.addPoint (beat - justBefore, before, 0.0f);

        curve.addPoint (beat, at, 0.0f);
    }

    // Afterwards, the points at the edges that change nothing go, as Tracktion's removeRedundantPoints() finds them.
    void tidyEdges (SongCurve& curve, const std::vector<BeatPosition>& edges)
    {
        for (const auto beat : edges)
            curve.removeRedundantPoints ({ beat - justBefore * 2.0, beat + justBefore });
    }

    std::vector<te::AutomatableParameter*> getAutomatedParameters (te::Track& track)
    {
        std::vector<te::AutomatableParameter*> automated;

        for (auto* parameter : getAutomatableParameters (track))
            if (hasPoints (*parameter))
                automated.push_back (parameter);

        return automated;
    }

    // Indices of points that are there, each once, in order.
    std::vector<int> getValidIndices (int numPoints, std::vector<int> indices)
    {
        std::sort (indices.begin(), indices.end());
        indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
        indices.erase (std::remove_if (indices.begin(), indices.end(),
                                       [numPoints] (int index) { return ! juce::isPositiveAndBelow (index, numPoints); }),
                       indices.end());
        return indices;
    }

    //==============================================================================
    // A device's ID as a lane or a curve keeps it.
    DeviceID readDeviceID (const juce::var& id)
    {
        return DeviceID::fromRaw (te::EditItemID::fromVar (id).getRawID());
    }

    ParameterRef readLane (const juce::ValueTree& lane)
    {
        return { readDeviceID (lane[schema::laneDevice]), lane[schema::laneParameter].toString() };
    }

    void writeLane (juce::ValueTree& lane, const ParameterRef& parameter)
    {
        lane.setProperty (schema::laneDevice, toTracktion (parameter.device).toString(), nullptr);
        lane.setProperty (schema::laneParameter, parameter.parameter, nullptr);
    }

    // A copied track's lanes, and those of the tracks in a folder's copy, show the copies of the devices they showed.
    void showCopiedDevices (te::Track& copy, const te::EditItemID::IDMap& devices)
    {
        for (auto lane : findTrackState (copy))
        {
            if (! schema::isTrackViewElement (lane))
                continue;

            if (auto parameter = readLane (lane); devices.count (toTracktion (parameter.device)) > 0)
            {
                parameter.device = DeviceID::fromRaw (devices.at (toTracktion (parameter.device)).getRawID());
                writeLane (lane, parameter);
            }
        }

        for (auto* inside : copy.getAllSubTracks (false))
            showCopiedDevices (*inside, devices);
    }

    juce::ValueTree findLaneState (const te::Track& track, const ParameterRef& parameter)
    {
        for (const auto& child : findTrackState (track))
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

            juce::ValueTree lane (schema::LANE);
            writeLane (lane, shown.parameter);
            findTrackState (track).appendChild (lane, nullptr);
        }
    }

    // A track's write mode as the song keeps it (Schema.h: trackAutomationMode), and as Tracktion's track has it.
    AutomationMode readAutomationMode (const juce::ValueTree& state)
    {
        const auto mode = state[schema::trackAutomationMode].toString();

        if (mode == schema::automationTouch)
            return AutomationMode::touch;

        if (mode == schema::automationLatch)
            return AutomationMode::latch;

        if (mode == schema::automationWrite)
            return AutomationMode::write;

        return AutomationMode::read;
    }

    void writeAutomationMode (juce::ValueTree& state, AutomationMode mode)
    {
        switch (mode)
        {
            case AutomationMode::touch:
                state.setProperty (schema::trackAutomationMode, schema::automationTouch, nullptr);
                break;
            case AutomationMode::latch:
                state.setProperty (schema::trackAutomationMode, schema::automationLatch, nullptr);
                break;
            case AutomationMode::write:
                state.setProperty (schema::trackAutomationMode, schema::automationWrite, nullptr);
                break;
            case AutomationMode::read:
            default:
                state.removeProperty (schema::trackAutomationMode, nullptr);
                break;
        }
    }

    AutomationMode fromTracktion (te::AutomationMode mode)
    {
        switch (mode)
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

    // The IDs of the parameters whose values a property of a device's state holds: Tracktion's fader, folder's VCA and send
    // name it after something else; Phazite's devices after the parameter.
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
        return "Send to " + (returnTrack != nullptr ? getTrackName (*returnTrack) : juce::String ("Return"));
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

    for (const auto& point : findCurveState (parameter.getEdit(), getParameterRef (parameter)))
        points.push_back ({ BeatPosition::fromBeats (point[schema::pointBeat]), point[schema::pointValue],
                            point.getProperty (schema::pointCurve, 0.0f) });

    return points;
}

bool hasPoints (const te::AutomatableParameter& parameter)
{
    return countPoints (parameter) > 0;
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
    SongCurve curve (parameter);
    const auto index = curve.addPoint (point.beat, limitValue (parameter, point.value), limitCurve (point.curve));
    curve.follow();
    return index;
}

BeatDuration movePoints (te::AutomatableParameter& parameter, const std::vector<int>& requested, BeatDuration distance, float valueChange)
{
    SongCurve curve (parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

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

    // In the order that keeps the points in time order all along.
    const auto move = [&] (int index)
    {
        const auto& point = points[(size_t) index];
        curve.setPoint (index, point.beat + BeatDuration::fromBeats (beats), limitValue (parameter, point.value + change));
    };

    if (beats > 0.0)
        std::for_each (indices.rbegin(), indices.rend(), move);
    else
        std::for_each (indices.begin(), indices.end(), move);

    curve.follow();
    return BeatDuration::fromBeats (beats);
}

void setCurve (te::AutomatableParameter& parameter, int index, float curveValue)
{
    if (SongCurve curve (parameter); juce::isPositiveAndBelow (index, curve.getNumPoints()))
    {
        curve.setCurve (index, limitCurve (curveValue));
        curve.follow();
    }
}

void deletePoints (te::AutomatableParameter& parameter, const std::vector<int>& requested)
{
    SongCurve curve (parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    for (auto i = indices.rbegin(); i != indices.rend(); ++i)
        curve.removePoint (*i);

    curve.follow();
}

void replacePoints (te::AutomatableParameter& parameter, BeatRange range, const std::vector<AutomationPoint>& points)
{
    SongCurve curve (parameter);

    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = curve.getPointBeat (i); beat >= range.getStart() && beat <= range.getEnd())
            curve.removePoint (i);

    // Each after any at its beat already, so that two at one beat stay in the order given: a jump.
    for (const auto& point : points)
        curve.addPoint (point.beat, limitValue (parameter, point.value), limitCurve (point.curve));

    curve.follow();
}

std::vector<int> duplicatePoints (te::AutomatableParameter& parameter, const std::vector<int>& requested, BeatDuration minimumSpan)
{
    SongCurve curve (parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    if (indices.empty())
        return {};

    const auto points = getPoints (parameter);
    const auto first = points[(size_t) indices.front()].beat, last = points[(size_t) indices.back()].beat;
    const auto span = std::max (last - first, minimumSpan);

    // The copies replace what is after the last original, up to the last copy; a copy that lands on the last original's
    // beat goes after it, a jump.
    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = curve.getPointBeat (i); beat > first + span && beat <= last + span)
            curve.removePoint (i);

    std::vector<int> copies;

    for (const auto index : indices)
    {
        const auto& point = points[(size_t) index];
        copies.push_back (curve.addPoint (point.beat + span, point.value, point.curve));
    }

    curve.follow();
    return copies;
}

//==============================================================================
void moveAutomation (const std::vector<AutomationStretch>& stretches, BeatDuration distance, bool copy)
{
    if (juce::exactlyEqual (distance.inBeats(), 0.0))
        return;

    for (const auto& [track, ranges] : byTrack (stretches))
    {
        for (auto* parameter : getAutomatedParameters (*track))
        {
            SongCurve curve (*parameter);
            std::vector<BeatPosition> edges;

            // Edges where each stretch is and where it lands, so that each part keeps its shape.
            for (const auto& range : ranges)
                for (const auto beat : { range.getStart(), range.getEnd(), range.getStart() + distance, range.getEnd() + distance })
                    edges.push_back (beat);

            for (const auto beat : edges)
                addEdge (curve, beat);

            // The stretches' points as they are, then cleared where they come from (unless copied) and where they land.
            const auto isInAny = [&ranges] (BeatPosition beat, BeatDuration offset)
            {
                return std::any_of (ranges.begin(), ranges.end(), [beat, offset] (const BeatRange& range)
                                    { return BeatRange (range.getStart() + offset, range.getEnd() + offset).contains (beat); });
            };

            std::vector<AutomationPoint> moved;

            for (int i = 0; i < curve.getNumPoints(); ++i)
                if (const auto beat = curve.getPointBeat (i); isInAny (beat, {}))
                    moved.push_back ({ beat + distance, curve.getPointValue (i), curve.getPointCurve (i) });

            for (auto i = curve.getNumPoints(); --i >= 0;)
                if (const auto beat = curve.getPointBeat (i); (! copy && isInAny (beat, {})) || isInAny (beat, distance))
                    curve.removePoint (i);

            // In order, each after any at its beat already, so that a jump stays one.
            for (const auto& point : moved)
                curve.addPoint (point.beat, point.value, point.curve);

            tidyEdges (curve, edges);
            curve.follow();
        }
    }
}

void deleteAutomation (const std::vector<AutomationStretch>& stretches, bool closeGap)
{
    for (const auto& [track, ranges] : byTrack (stretches))
    {
        for (auto* parameter : getAutomatedParameters (*track))
        {
            SongCurve curve (*parameter);
            std::vector<BeatPosition> edges;

            // The latest stretch first, so that closing its gap doesn't move one still to go.
            for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
            {
                addEdge (curve, range->getStart());
                addEdge (curve, range->getEnd());

                for (auto i = curve.getNumPoints(); --i >= 0;)
                {
                    const auto beat = curve.getPointBeat (i);

                    if (range->contains (beat))
                        curve.removePoint (i);
                    else if (closeGap && beat >= range->getEnd())
                        curve.setPoint (i, beat - range->getLength(), curve.getPointValue (i));
                }

                edges.push_back (range->getStart());

                if (! closeGap)
                    edges.push_back (range->getEnd());
            }

            tidyEdges (curve, edges);
            curve.follow();
        }
    }
}

std::vector<AutomationCopy> copyAutomation (const std::vector<AutomationStretch>& stretches)
{
    std::vector<AutomationCopy> copies;

    for (const auto& [track, ranges] : byTrack (stretches))
    {
        for (auto* parameter : getAutomatedParameters (*track))
        {
            const auto ref = getParameterRef (*parameter);

            if (! ref.isValid())
                continue;

            const SongCurve curve (*parameter);

            // As moveAutomation() takes a stretch: its edges' values, and the points between them, from its start.
            for (const auto& range : ranges)
            {
                AutomationCopy copy { ref,
                                      range.getLength(),
                                      { { BeatPosition(), curve.getValueAt (range.getStart()), 0.0f } },
                                      range.getStart() };

                for (int i = 0; i < curve.getNumPoints(); ++i)
                    if (const auto beat = curve.getPointBeat (i); beat >= range.getStart() && beat < range.getEnd() - justBefore)
                        copy.points.push_back (
                            { BeatPosition() + (beat - range.getStart()), curve.getPointValue (i), curve.getPointCurve (i) });

                copy.points.push_back (
                    { BeatPosition() + (range.getLength() - justBefore), curve.getValueAt (range.getEnd() - justBefore), 0.0f });
                copies.push_back (std::move (copy));
            }
        }
    }

    return copies;
}

void pasteAutomation (const te::Edit& edit, const std::vector<AutomationCopy>& copies, BeatDuration distance)
{
    for (const auto& copy : copies)
    {
        const auto parameter = findParameter (edit, copy.parameter);

        if (parameter == nullptr)
            continue;

        // Where it lands, in place of what is there, what lies around it keeping its shape, as moveAutomation() copies.
        SongCurve curve (*parameter);
        const BeatRange landing (copy.start + distance, copy.start + distance + copy.length);
        addEdge (curve, landing.getStart());
        addEdge (curve, landing.getEnd());

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (landing.contains (curve.getPointBeat (i)))
                curve.removePoint (i);

        for (const auto& point : copy.points)
            curve.addPoint (landing.getStart() + (point.beat - BeatPosition()), limitValue (*parameter, point.value),
                            limitCurve (point.curve));

        tidyEdges (curve, { landing.getStart(), landing.getEnd() });
        curve.follow();
    }
}

//==============================================================================
juce::ValueTree findCurveState (const te::Edit& edit, const ParameterRef& parameter)
{
    if (! parameter.isValid())
        return {};

    return findCurveIn (findPhaziteState (edit).getChildWithName (schema::AUTOMATION), parameter);
}

std::vector<juce::ValueTree> getCurveStates (const te::Edit& edit)
{
    std::vector<juce::ValueTree> curves;

    for (const auto& curve : findPhaziteState (edit).getChildWithName (schema::AUTOMATION))
        curves.push_back (curve);

    return curves;
}

ParameterRef getCurveParameter (const juce::ValueTree& curve)
{
    return { readDeviceID (curve[schema::curveDevice]), curve[schema::curveParameter].toString() };
}

void insertAutomationTime (te::Edit& edit, BeatPosition at, BeatDuration length)
{
    for (const auto& parameter : getParametersWithCurves (edit))
    {
        SongCurve curve (*parameter);
        const auto valueThere = curve.getValueAt (at);

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (const auto beat = curve.getPointBeat (i); beat >= at)
                curve.setPoint (i, beat + length, curve.getPointValue (i));

        if (! juce::approximatelyEqual (valueThere, curve.getValueAt (at)))
            curve.addPoint (at, valueThere, 0.0f);

        if (! juce::approximatelyEqual (valueThere, curve.getValueAt (at + length)))
            curve.addPoint (at + length, valueThere, 0.0f);
    }
}

void removeAutomationTime (te::Edit& edit, BeatRange range)
{
    for (const auto& parameter : getParametersWithCurves (edit))
    {
        SongCurve curve (*parameter);
        const auto valueAtStart = curve.getValueAt (range.getStart()), valueAtEnd = curve.getValueAt (range.getEnd());

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (const auto beat = curve.getPointBeat (i); beat >= range.getStart() && beat <= range.getEnd())
                curve.removePoint (i);

        for (int i = 0; i < curve.getNumPoints(); ++i)
            if (const auto beat = curve.getPointBeat (i); beat >= range.getEnd())
                curve.setPoint (i, beat - range.getLength(), curve.getPointValue (i));

        if (! juce::exactlyEqual (curve.getValueAt (range.getStart()), valueAtStart))
            curve.addPoint (range.getStart(), valueAtStart, 0.0f);

        if (! juce::exactlyEqual (valueAtStart, valueAtEnd))
            curve.addPoint (range.getStart(), valueAtEnd, 0.0f);
    }
}

void addAutomationEdges (te::Edit& edit, const std::vector<BeatPosition>& beats)
{
    for (const auto& parameter : getParametersWithCurves (edit))
    {
        SongCurve curve (*parameter);

        for (const auto beat : beats)
        {
            const auto value = curve.getValueAt (beat);

            if (beat - justBefore > BeatPosition())
                curve.addPoint (beat - justBefore, value, 0.0f);

            curve.addPoint (beat, value, 0.0f);
        }
    }
}

std::vector<ParameterPoint> getAutomationPointsIn (te::Edit& edit, BeatRange range)
{
    std::vector<ParameterPoint> points;

    for (const auto& parameter : getParametersWithCurves (edit))
        for (const auto& point : getPoints (*parameter))
            if (range.contains (point.beat))
                points.push_back ({ getParameterRef (*parameter), point });

    return points;
}

void removeAutomationPointsIn (te::Edit& edit, BeatRange range)
{
    for (const auto& parameter : getParametersWithCurves (edit))
    {
        SongCurve curve (*parameter);

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (range.contains (curve.getPointBeat (i)))
                curve.removePoint (i);
    }
}

void addAutomationPoints (te::Edit& edit, const std::vector<ParameterPoint>& points)
{
    // Each parameter's through one curve, in the order given: a parameter's curve may have gone with its last point.
    std::vector<ParameterRef> parameters;

    for (const auto& point : points)
        if (std::find (parameters.begin(), parameters.end(), point.parameter) == parameters.end())
            parameters.push_back (point.parameter);

    for (const auto& ref : parameters)
    {
        if (const auto parameter = findParameter (edit, ref))
        {
            SongCurve curve (*parameter);

            for (const auto& [pointParameter, point] : points)
                if (pointParameter == ref)
                    curve.addPoint (point.beat, point.value, point.curve);
        }
    }
}

void adoptRecordedAutomation (te::Edit& edit)
{
    const auto& tempo = edit.tempoSequence;

    for (auto* device : te::getAllPlugins (edit, true))
    {
        for (auto* parameter : device->getAutomatableParameters())
        {
            // What Tracktion recorded is in its curves, which it never leaves empty; those it recorded nothing in are
            // the song's.
            const auto& recorded = parameter->getCurve();

            if (recorded.getNumPoints() == 0)
                continue;

            SongCurve curve (*parameter);
            const auto isSame = [&] (int i)
            {
                return juce::exactlyEqual (tempo.toTime (toTracktion (curve.getPointBeat (i))).inSeconds(),
                                           te::toTime (recorded.getPointPosition (i), tempo).inSeconds())
                       && juce::exactlyEqual (curve.getPointValue (i), recorded.getPointValue (i))
                       && juce::exactlyEqual (curve.getPointCurve (i), recorded.getPointCurve (i));
            };

            auto same = curve.getNumPoints() == recorded.getNumPoints();

            for (int i = 0; same && i < curve.getNumPoints(); ++i)
                same = isSame (i);

            if (same)
                continue;

            // Tracktion's points in place of the song's, at their beats.
            std::vector<AutomationPoint> points;

            for (int i = 0; i < recorded.getNumPoints(); ++i)
                points.push_back ({ fromTracktion (te::toBeats (recorded.getPointPosition (i), tempo)), recorded.getPointValue (i),
                                    recorded.getPointCurve (i) });

            for (auto i = curve.getNumPoints(); --i >= 0;)
                curve.removePoint (i);

            for (const auto& point : points)
                curve.addPoint (point.beat, point.value, point.curve);
        }
    }
}

void adoptMovedPoint (te::AutomatableParameter& parameter)
{
    // Tracktion moves a curve's one point to its parameter's value as the value changes, unless it records it
    // (AutomatableParameter::setParameterValue()), in the step of the change; but not as the mirror sets a value from the song
    // (mirror::setValueSetByHand(), after an undo or a redo), which plays the song's curve again.
    const auto& moved = parameter.getCurve();

    if (moved.getNumPoints() != 1 || parameter.isCurrentlyRecording() || mirror::isSettingValues())
        return;

    if (SongCurve curve (parameter); curve.getNumPoints() == 1 && ! juce::exactlyEqual (curve.getPointValue (0), moved.getPointValue (0)))
        curve.setPoint (0, curve.getPointBeat (0), moved.getPointValue (0));
}

void forgetCurvesOfGoneDevices (te::Edit& edit)
{
    // A device that is there keeps its curves, those of parameters it has now and others (a plug-in that isn't installed
    // has none until it is).
    auto automation = findPhaziteState (edit).getChildWithName (schema::AUTOMATION);

    for (auto i = automation.getNumChildren(); --i >= 0;)
        if (te::findPluginForID (edit, toTracktion (getCurveParameter (automation.getChild (i)).device)) == nullptr)
            automation.removeChild (i, nullptr);
}

//==============================================================================
std::vector<AutomationLane> getLanes (te::Track& track)
{
    std::vector<AutomationLane> lanes;

    for (const auto& child : findTrackState (track))
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

        if (hasPoints (*parameter)
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

    juce::ValueTree lane (schema::LANE);
    writeLane (lane, parameter);
    findTrackState (track).appendChild (lane, nullptr);
}

void removeLane (te::Track& track, const ParameterRef& parameter)
{
    if (auto lane = findLaneState (track, parameter); lane.isValid())
        findTrackState (track).removeChild (lane, nullptr);
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

void copyLanesAndCurves (const te::Track& original, te::Track& copy, const te::EditItemID::IDMap& devicesInContainers)
{
    auto devices = devicesInContainers;
    mapCopiedDevices (original, copy, devices);
    showCopiedDevices (copy, devices);

    auto& edit = copy.edit;
    auto automation = findPhaziteState (edit).getChildWithName (schema::AUTOMATION);

    for (const auto& curve : getCurveStates (edit))
    {
        const auto ref = getCurveParameter (curve);
        const auto copied = devices.find (toTracktion (ref.device));

        if (copied == devices.end())
            continue;

        if (const ParameterRef copiedRef { DeviceID::fromRaw (copied->second.getRawID()), ref.parameter };
            ! findCurveIn (automation, copiedRef).isValid())
        {
            auto copiedState = curve.createCopy();
            copiedState.setProperty (schema::curveDevice, toTracktion (copiedRef.device).toString(), nullptr);
            automation.appendChild (copiedState, &getSongUndoManager (edit));
            mirror::reconcileCurve (edit, copiedRef);
        }
    }
}

AutomationMode getAutomationMode (const te::Track& track)
{
    return readAutomationMode (findTrackState (track));
}

void setAutomationMode (te::Track& track, AutomationMode mode)
{
    if (auto state = findTrackState (track); state.isValid())
    {
        writeAutomationMode (state, mode);
        mirror::reconcileTrackSwitches (track.edit);
    }
}

void adoptAutomationMode (te::Track& track)
{
    if (auto state = findTrackState (track); state.isValid() && readAutomationMode (state) != fromTracktion (track.automationMode.get()))
        writeAutomationMode (state, fromTracktion (track.automationMode.get()));
}

bool isWritingAutomation (const te::Edit& edit)
{
    auto states = getTrackStates (edit);
    states.push_back (findPhaziteState (edit).getChildWithName (schema::MASTER));
    return std::any_of (states.begin(), states.end(),
                        [] (const juce::ValueTree& state) { return readAutomationMode (state) != AutomationMode::read; });
}

bool isParameterValue (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! tree.hasType (te::IDs::PLUGIN))
        return false;

    const auto type = tree[te::IDs::type].toString();

    // Tracktion's fader, VCA and send keep theirs under names of their own; Phazite's devices under the parameters' IDs.
    if (type == te::VolumeAndPanPlugin::xmlTypeName || type == te::VCAPlugin::xmlTypeName || type == te::AuxSendPlugin::xmlTypeName)
        return ! getParametersStoredIn (tree, property).isEmpty();

    for (const auto* info : getOwnDevices())
        if (type == juce::String (info->typeID.data(), info->typeID.size()))
            return devices::findParameter (*info, property.toString().toStdString()) >= 0;

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
std::optional<PointSelection> getSelectedPoints (const Selection& selection)
{
    auto* edit = selection.getEdit();

    if (edit == nullptr || ! selection.getPointsParameter().isValid())
        return {};

    const auto parameter = findParameter (*edit, selection.getPointsParameter());

    if (parameter == nullptr)
        return {};

    auto indices = getValidIndices (countPoints (*parameter), selection.getPoints());

    if (indices.empty())
        return {};

    return PointSelection { parameter, std::move (indices) };
}

void selectPoints (Selection& selection, const te::AutomatableParameter& parameter, const std::vector<int>& indices)
{
    selection.selectPoints (getParameterRef (parameter), getValidIndices (countPoints (parameter), indices));
}

//==============================================================================
AutomationWatcher::AutomationWatcher (te::Edit& e)
    : edit (e),
      wasPlaying (e.getTransport().isPlaying())
{
    e.getParameterChangeHandler().addListener (*this);
    e.getTransport().addChangeListener (this);
    e.state.addListener (this);
}

AutomationWatcher::~AutomationWatcher()
{
    // The Edit may have gone first: then so have its transport and its parameters' listeners.
    if (auto* e = edit.get())
    {
        e->state.removeListener (this);
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

    adoptMovedPoint (parameter);

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

    // What Tracktion recorded becomes the song's, in the step.
    if (std::exchange (stopRecording, false) && ! wasPlaying)
    {
        if (recording != nullptr && edit != nullptr)
            adoptRecordedAutomation (*edit);

        recording = nullptr;
    }
}

void AutomationWatcher::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property != te::IDs::automationMode || edit == nullptr)
        return;

    if (auto* track = te::findTrackForState (*edit, tree))
        adoptAutomationMode (*track);
}
} // namespace phazite
