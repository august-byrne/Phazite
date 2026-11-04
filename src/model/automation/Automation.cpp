// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Automation.h"
#include "engine/automation/Curve.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <limits>

namespace phazite
{
namespace
{
    // A value within its parameter's range (model/devices/internal/Parameters.h).
    float limitValue (const Song& song, const ParameterRef& ref, float value)
    {
        const auto description = describeParameter (song, ref);
        return description.has_value() ? description->range.getRange().clipValue (value) : value;
    }

    float limitCurve (float curve)
    {
        return juce::jlimit (-1.0f, 1.0f, curve);
    }

    // A curve of the song's (Schema.h: CURVE) as Phazite's engine plays it (engine/automation/Curve.h).
    engine::Curve makeEngineCurve (const juce::ValueTree& curveState)
    {
        std::vector<engine::CurvePoint> points;

        for (const auto& point : curveState)
            points.push_back ({ (double) point[schema::pointBeat], (float) point[schema::pointValue],
                                (float) point.getProperty (schema::pointCurve, 0.0f) });

        return engine::Curve (std::move (points));
    }

    // A device's ID as a lane or a curve keeps it, and the other way.
    DeviceID readDeviceID (const juce::var& id)
    {
        return DeviceID::fromRaw ((std::uint64_t) (juce::int64) id);
    }

    juce::String writeDeviceID (DeviceID id)
    {
        return juce::String ((juce::int64) id.getRaw());
    }

    // A parameter's value at a beat as its curve has it, as Phazite's engine plays it; without points, its value set by hand.
    float getCurveValueAt (const Song& song, const ParameterRef& ref, const juce::ValueTree& curveState, BeatPosition beat)
    {
        if (curveState.getNumChildren() == 0)
            return getValueSetByHand (song, ref);

        return makeEngineCurve (curveState).getValueAt (std::max (0.0, beat.inBeats()));
    }

    //==========================================================================
    // The song's curves (Schema.h: AUTOMATION).

    juce::ValueTree findCurveIn (const juce::ValueTree& automation, const ParameterRef& ref)
    {
        const auto device = writeDeviceID (ref.device);

        for (const auto& curve : automation)
            if (curve[schema::curveDevice].toString() == device && curve[schema::curveParameter].toString() == ref.parameter)
                return curve;

        return {};
    }

    // A parameter's curve in the song, edited point by point, in beats, each change with the song's undo manager. Points
    // keep time order, one added at a beat that has points going after them. What plays the song hears each change. One at
    // a time for a parameter: each holds the curve's state as it found it.
    class SongCurve
    {
    public:
        SongCurve (Song& s, const ParameterRef& r)
            : song (s),
              ref (r),
              state (findCurveState (song, ref))
        {
        }

        int getNumPoints() const { return state.getNumChildren(); }
        BeatPosition getPointBeat (int index) const { return BeatPosition::fromBeats (state.getChild (index)[schema::pointBeat]); }
        float getPointValue (int index) const { return state.getChild (index)[schema::pointValue]; }
        float getPointCurve (int index) const { return state.getChild (index).getProperty (schema::pointCurve, 0.0f); }

        // As Phazite's engine plays it; without points, the parameter's value set by hand.
        float getValueAt (BeatPosition beat) const { return getCurveValueAt (song, ref, state, beat); }

        int addPoint (BeatPosition beat, float value, float curve)
        {
            if (! ref.isValid())
                return -1;

            if (! state.isValid())
            {
                auto automation = song.getState().getOrCreateChildWithName (schema::AUTOMATION, getUndo());
                state = juce::ValueTree (schema::CURVE);
                state.setProperty (schema::curveDevice, writeDeviceID (ref.device), nullptr);
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
        }

        void setPoint (int index, BeatPosition beat, float value)
        {
            auto point = state.getChild (index);
            point.setProperty (schema::pointBeat, std::max (0.0, beat.inBeats()), getUndo());
            point.setProperty (schema::pointValue, value, getUndo());
        }

        void setCurve (int index, float curve)
        {
            if (auto point = state.getChild (index); juce::exactlyEqual (curve, 0.0f))
                point.removeProperty (schema::pointCurve, getUndo());
            else
                point.setProperty (schema::pointCurve, curve, getUndo());
        }

        // The points in a range that change nothing: those whose neighbours have the same value, or the next of which is
        // the same point, or which lie at one time between two.
        void removeRedundantPoints (BeatRange range)
        {
            constexpr auto threshold = 0.0001;
            const auto secondsBetween = [this] (int a, int b)
            { return toSeconds (song, getPointBeat (a)) - toSeconds (song, getPointBeat (b)); };

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
        Song& song;
        const ParameterRef ref;
        juce::ValueTree state;

        juce::UndoManager* getUndo() const { return &song.getUndoManager(); }

        JUCE_DECLARE_NON_COPYABLE (SongCurve)
    };

    // The parameters the song has curves for that are there, not those of devices gone (whose curves the song keeps for
    // undo).
    std::vector<ParameterRef> getParametersWithCurves (const Song& song)
    {
        std::vector<ParameterRef> parameters;

        for (const auto& curve : getCurveStates (song))
            if (const auto ref = getCurveParameter (curve); locateParameter (song, ref).has_value())
                parameters.push_back (ref);

        return parameters;
    }

    //==========================================================================
    // Automation that goes with clips (moveAutomation(), deleteAutomation()).
    const auto justBefore = BeatDuration::fromBeats (1.0e-4); // where an edge's point on the side before it goes

    // Each track's stretches in time order, those that overlap or touch as one.
    std::vector<std::pair<TrackID, std::vector<BeatRange>>> byTrack (const Song& song, const std::vector<AutomationStretch>& stretches)
    {
        std::vector<std::pair<TrackID, std::vector<BeatRange>>> tracks;

        for (const auto& stretch : stretches)
        {
            const auto track = stretch.track;

            if (! findTrackState (song, track).isValid() || stretch.range.isEmpty())
                continue;

            auto found = std::find_if (tracks.begin(), tracks.end(), [track] (const auto& each) { return each.first == track; });

            if (found == tracks.end())
                found = tracks.insert (tracks.end(), { track, {} });

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

    // Afterwards, the points at the edges that change nothing go (SongCurve::removeRedundantPoints()).
    void tidyEdges (SongCurve& curve, const std::vector<BeatPosition>& edges)
    {
        for (const auto beat : edges)
            curve.removeRedundantPoints ({ beat - justBefore * 2.0, beat + justBefore });
    }

    std::vector<ParameterRef> getAutomatedParameters (const Song& song, TrackID track)
    {
        std::vector<ParameterRef> automated;

        for (const auto& parameter : getAutomatableParameters (song, track))
            if (hasPoints (song, parameter))
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
    ParameterRef readLane (const juce::ValueTree& lane)
    {
        return { readDeviceID (lane[schema::laneDevice]), lane[schema::laneParameter].toString() };
    }

    void writeLane (juce::ValueTree& lane, const ParameterRef& parameter)
    {
        lane.setProperty (schema::laneDevice, writeDeviceID (parameter.device), nullptr);
        lane.setProperty (schema::laneParameter, parameter.parameter, nullptr);
    }

    // The devices whose parameters a track's lanes show, in the order the lanes of their automation come: its channel's
    // fader (or a plain folder's VCA), its sends, then its devices, those in its containers in their place.
    std::vector<DeviceID> getLaneDevices (const Song& song, TrackID track)
    {
        std::vector<DeviceID> ids;
        const auto state = findTrackState (song, track);

        for (const auto* property : { &schema::channelFader, &schema::channelVCA })
            if (const auto* id = state.getPropertyPointer (*property))
                ids.push_back (DeviceID::fromRaw ((std::uint64_t) (juce::int64) *id));

        for (const auto& child : state)
            if (child.hasType (schema::SEND))
                ids.push_back (DeviceID::fromRaw ((std::uint64_t) (juce::int64) child[schema::sendID]));

        for (const auto& device : getDeviceStates (state))
        {
            if (! device.hasType (schema::CONTAINER))
                ids.push_back (getDeviceID (device));

            for (const auto& inside : getDeviceStates (device))
                ids.push_back (getDeviceID (inside));
        }

        return ids;
    }

    // Where a parameter's lane comes among its device's: a fader's volume before its pan, and one of Phazite's own devices'
    // parameters in their order; others after, in the order their automation was made.
    int getLaneOrder (const Song& song, const ParameterRef& parameter)
    {
        if (parameter.parameter == "volume" || parameter.parameter == "master volume" || parameter.parameter == "vca")
            return 0;

        if (parameter.parameter == "pan" || parameter.parameter == "master pan")
            return 1;

        if (const auto* info = findOwnDevice (findDeviceState (song, parameter.device)[schema::deviceType].toString().toStdString()))
            if (const auto index = devices::findParameter (*info, parameter.parameter.toStdString()); index >= 0)
                return index;

        return std::numeric_limits<int>::max();
    }

    juce::ValueTree findLaneState (const Song& song, TrackID track, const ParameterRef& parameter)
    {
        for (const auto& child : findTrackState (song, track))
            if (schema::isTrackViewElement (child) && readLane (child) == parameter)
                return child;

        return {};
    }

    // Whether a lane shows a parameter of a device on this track.
    bool isOnTrack (const Song& song, TrackID track, const ParameterRef& parameter)
    {
        const auto devices = getLaneDevices (song, track);
        return parameter.parameter.isNotEmpty() && std::find (devices.begin(), devices.end(), parameter.device) != devices.end();
    }

    // Before a change to a track's lanes: it keeps one for each lane it shows without keeping one (of a parameter with
    // automation), in the order they show, so that they stay in that order.
    void keepShownLanes (Song& song, TrackID track)
    {
        for (const auto& shown : getLanes (song, track))
        {
            if (findLaneState (song, track, shown.parameter).isValid())
                continue;

            juce::ValueTree lane (schema::LANE);
            writeLane (lane, shown.parameter);
            findTrackState (song, track).appendChild (lane, nullptr);
        }
    }

    // A track's write mode as the song keeps it (Schema.h: trackAutomationMode).
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

    // A plug-in's value that it set itself moves the one point of its parameter's curve, as one set by a control does
    // (model/devices/internal/Parameters.h), in the song's history: the step the change joins. Not while the automation
    // writer writes the parameter, which writes its own points.
    void moveOnePoint (Song& song, const ParameterRef& parameter, float value)
    {
        if (auto* session = findSession (song); session != nullptr && session->getWriter().isWriting (parameter))
            return;

        if (SongCurve curve (song, parameter); curve.getNumPoints() == 1 && ! juce::exactlyEqual (curve.getPointValue (0), value))
            curve.setPoint (0, curve.getPointBeat (0), value);
    }
} // namespace

//==============================================================================
TrackID getParameterTrack (const Song& song, const ParameterRef& parameter)
{
    const auto found = locateParameter (song, parameter);

    if (! found.has_value())
        return {};

    switch (found->source)
    {
        case ParameterSource::device:
        case ParameterSource::plugin:
            return getDeviceChannel (song, parameter.device); // a container's devices are on its channel
        case ParameterSource::volume:
        case ParameterSource::pan:
            return getTrackID (found->state);
        case ParameterSource::send:
            return getTrackID (found->state.getParent());
        case ParameterSource::modulator:
        case ParameterSource::macro:
            break;
    }

    return {};
}

std::vector<ParameterRef> getAutomatableParameters (const Song& song, TrackID track)
{
    std::vector<ParameterRef> parameters;

    // The channel's fader's (or a plain folder's VCA's), the master's for the master; then a track's sends.
    const auto chain = findChainState (song, track);
    const auto fader = getFader (song, track);

    if (! chain.isValid() || ! fader.isValid())
        return parameters;

    parameters.push_back ({ fader, getVolumeParameterID (song, track) });

    if (const auto pan = getPanParameterID (song, track); pan.isNotEmpty())
        parameters.push_back ({ fader, pan });

    for (const auto send : getSends (song, track))
        parameters.push_back ({ send, schema::sendLevelParameter });

    // Each device's (those in its containers after each container). A device's mix (Mixer.h: getMix()) is its own knob, not
    // one of these; a container has no parameters of its own.
    const auto addParametersOf = [&] (DeviceID device)
    {
        const auto state = findDeviceState (song, device);

        if (isContainerState (state))
            return;

        // A plug-in's, as its instance lists them, by their own IDs: those it lets be automated.
        if (isPluginState (state))
        {
            if (auto* instance = findPluginInstance (song, device))
                for (const auto& info : instance->getParameterInfos())
                    if (info.isAutomatable)
                        parameters.push_back ({ device, info.id });

            return;
        }

        // One of Phazite's own devices', in its description's order.
        if (const auto* own = findOwnDevice (state[schema::deviceType].toString().toStdString()))
            for (const auto& info : own->parameters)
                if (own->kind == devices::DeviceKind::instrument || info.id != devices::mixParameterID)
                    parameters.push_back ({ device, juce::String (info.id.data(), info.id.size()) });
    };

    for (const auto device : getDevices (song, track))
    {
        addParametersOf (device);

        for (const auto inside : getContainerDevices (song, device))
            addParametersOf (inside);
    }

    return parameters;
}

bool isMixerControl (const Song& song, const ParameterRef& parameter)
{
    const auto found = locateParameter (song, parameter);
    return found.has_value()
           && (found->source == ParameterSource::volume || found->source == ParameterSource::pan || found->source == ParameterSource::send);
}

juce::String getParameterName (const Song& song, const ParameterRef& parameter)
{
    const auto found = locateParameter (song, parameter);

    if (! found.has_value())
        return {};

    switch (found->source)
    {
        case ParameterSource::send:
        {
            const auto target = getSendTarget (song, parameter.device);
            return "Send to " + (findTrackState (song, target).isValid() ? getTrackName (song, target) : juce::String ("Return"));
        }

        case ParameterSource::volume:
            return "Volume"; // a folder's too, which is a VCA

        case ParameterSource::pan:
            return "Pan";

        case ParameterSource::device:
        case ParameterSource::plugin:
        case ParameterSource::modulator:
        case ParameterSource::macro:
            break;
    }

    return describeParameter (song, *found).name;
}

//==============================================================================
std::vector<AutomationPoint> getPoints (const Song& song, const ParameterRef& parameter)
{
    std::vector<AutomationPoint> points;

    for (const auto& point : findCurveState (song, parameter))
        points.push_back ({ BeatPosition::fromBeats (point[schema::pointBeat]), point[schema::pointValue],
                            point.getProperty (schema::pointCurve, 0.0f) });

    return points;
}

bool hasPoints (const Song& song, const ParameterRef& parameter)
{
    return findCurveState (song, parameter).getNumChildren() > 0;
}

std::vector<float> getValues (const Song& song, const ParameterRef& parameter, BeatRange range, int numValues)
{
    // As Phazite's engine plays the song's points.
    std::vector<float> values;
    const auto state = findCurveState (song, parameter);

    if (! state.isValid() || state.getNumChildren() == 0 || numValues <= 0)
        return values;

    const auto curve = makeEngineCurve (state);
    values.reserve ((size_t) numValues);
    const auto step = numValues > 1 ? range.getLength() / (double) (numValues - 1) : BeatDuration();

    for (int i = 0; i < numValues; ++i)
        values.push_back (curve.getValueAt (std::max (0.0, (range.getStart() + step * (double) i).inBeats())));

    return values;
}

int addPoint (Song& song, const ParameterRef& parameter, AutomationPoint point)
{
    SongCurve curve (song, parameter);
    return curve.addPoint (point.beat, limitValue (song, parameter, point.value), limitCurve (point.curve));
}

BeatDuration movePoints (Song& song, const ParameterRef& parameter, const std::vector<int>& requested, BeatDuration distance,
                         float valueChange)
{
    SongCurve curve (song, parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);
    const auto description = describeParameter (song, parameter);

    if (indices.empty() || ! description.has_value())
        return {};

    const auto points = getPoints (song, parameter);
    const auto& range = description->range;
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

        lowest = std::max (lowest, range.start - point.value);
        highest = std::min (highest, range.end - point.value);
    }

    const auto beats = juce::jlimit (std::min (earliest, 0.0), std::max (latest, 0.0), distance.inBeats());
    const auto change = juce::jlimit (std::min (lowest, 0.0f), std::max (highest, 0.0f), valueChange);

    // In the order that keeps the points in time order all along.
    const auto move = [&] (int index)
    {
        const auto& point = points[(size_t) index];
        curve.setPoint (index, point.beat + BeatDuration::fromBeats (beats), limitValue (song, parameter, point.value + change));
    };

    if (beats > 0.0)
        std::for_each (indices.rbegin(), indices.rend(), move);
    else
        std::for_each (indices.begin(), indices.end(), move);

    return BeatDuration::fromBeats (beats);
}

void setCurve (Song& song, const ParameterRef& parameter, int index, float curveValue)
{
    if (SongCurve curve (song, parameter); juce::isPositiveAndBelow (index, curve.getNumPoints()))
        curve.setCurve (index, limitCurve (curveValue));
}

void deletePoints (Song& song, const ParameterRef& parameter, const std::vector<int>& requested)
{
    SongCurve curve (song, parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    for (auto i = indices.rbegin(); i != indices.rend(); ++i)
        curve.removePoint (*i);
}

void replacePoints (Song& song, const ParameterRef& parameter, BeatRange range, const std::vector<AutomationPoint>& points)
{
    SongCurve curve (song, parameter);

    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = curve.getPointBeat (i); beat >= range.getStart() && beat <= range.getEnd())
            curve.removePoint (i);

    // Each after any at its beat already, so that two at one beat stay in the order given: a jump.
    for (const auto& point : points)
        curve.addPoint (point.beat, limitValue (song, parameter, point.value), limitCurve (point.curve));
}

std::vector<int> duplicatePoints (Song& song, const ParameterRef& parameter, const std::vector<int>& requested, BeatDuration minimumSpan)
{
    SongCurve curve (song, parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    if (indices.empty())
        return {};

    const auto points = getPoints (song, parameter);
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

    return copies;
}

//==============================================================================
void moveAutomation (Song& song, const std::vector<AutomationStretch>& stretches, BeatDuration distance, bool copy)
{
    if (juce::exactlyEqual (distance.inBeats(), 0.0))
        return;

    for (const auto& [track, ranges] : byTrack (song, stretches))
    {
        for (const auto& parameter : getAutomatedParameters (song, track))
        {
            SongCurve curve (song, parameter);
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
        }
    }
}

void deleteAutomation (Song& song, const std::vector<AutomationStretch>& stretches, bool closeGap)
{
    for (const auto& [track, ranges] : byTrack (song, stretches))
    {
        for (const auto& parameter : getAutomatedParameters (song, track))
        {
            SongCurve curve (song, parameter);
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
        }
    }
}

std::vector<AutomationCopy> copyAutomation (const Song& song, const std::vector<AutomationStretch>& stretches)
{
    std::vector<AutomationCopy> copies;

    for (const auto& [track, ranges] : byTrack (song, stretches))
    {
        for (const auto& ref : getAutomatedParameters (song, track))
        {
            const auto curveState = findCurveState (song, ref);
            const auto valueAt = [&] (BeatPosition beat) { return getCurveValueAt (song, ref, curveState, beat); };

            // As moveAutomation() takes a stretch: its edges' values, and the points between them, from its start.
            for (const auto& range : ranges)
            {
                AutomationCopy copy { ref, range.getLength(), { { BeatPosition(), valueAt (range.getStart()), 0.0f } }, range.getStart() };

                for (const auto& point : curveState)
                    if (const auto beat = BeatPosition::fromBeats (point[schema::pointBeat]);
                        beat >= range.getStart() && beat < range.getEnd() - justBefore)
                        copy.points.push_back ({ BeatPosition() + (beat - range.getStart()), point[schema::pointValue],
                                                 point.getProperty (schema::pointCurve, 0.0f) });

                copy.points.push_back ({ BeatPosition() + (range.getLength() - justBefore), valueAt (range.getEnd() - justBefore), 0.0f });
                copies.push_back (std::move (copy));
            }
        }
    }

    return copies;
}

void pasteAutomation (Song& song, const std::vector<AutomationCopy>& copies, BeatDuration distance)
{
    for (const auto& copy : copies)
    {
        if (! locateParameter (song, copy.parameter).has_value())
            continue;

        // Where it lands, in place of what is there, what lies around it keeping its shape, as moveAutomation() copies.
        SongCurve curve (song, copy.parameter);
        const BeatRange landing (copy.start + distance, copy.start + distance + copy.length);
        addEdge (curve, landing.getStart());
        addEdge (curve, landing.getEnd());

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (landing.contains (curve.getPointBeat (i)))
                curve.removePoint (i);

        for (const auto& point : copy.points)
            curve.addPoint (landing.getStart() + (point.beat - BeatPosition()), limitValue (song, copy.parameter, point.value),
                            limitCurve (point.curve));

        tidyEdges (curve, { landing.getStart(), landing.getEnd() });
    }
}

//==============================================================================
juce::ValueTree findCurveState (const Song& song, const ParameterRef& parameter)
{
    if (! parameter.isValid())
        return {};

    return findCurveIn (song.getState().getChildWithName (schema::AUTOMATION), parameter);
}

std::vector<juce::ValueTree> getCurveStates (const Song& song)
{
    std::vector<juce::ValueTree> curves;

    for (const auto& curve : song.getState().getChildWithName (schema::AUTOMATION))
        curves.push_back (curve);

    return curves;
}

ParameterRef getCurveParameter (const juce::ValueTree& curve)
{
    return { readDeviceID (curve[schema::curveDevice]), curve[schema::curveParameter].toString() };
}

void insertAutomationTime (Song& song, BeatPosition at, BeatDuration length)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        SongCurve curve (song, parameter);
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

void removeAutomationTime (Song& song, BeatRange range)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        SongCurve curve (song, parameter);
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

void addAutomationEdges (Song& song, const std::vector<BeatPosition>& beats)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        SongCurve curve (song, parameter);

        for (const auto beat : beats)
        {
            const auto value = curve.getValueAt (beat);

            if (beat - justBefore > BeatPosition())
                curve.addPoint (beat - justBefore, value, 0.0f);

            curve.addPoint (beat, value, 0.0f);
        }
    }
}

std::vector<ParameterPoint> getAutomationPointsIn (Song& song, BeatRange range)
{
    std::vector<ParameterPoint> points;

    for (const auto& parameter : getParametersWithCurves (song))
        for (const auto& point : getPoints (song, parameter))
            if (range.contains (point.beat))
                points.push_back ({ parameter, point });

    return points;
}

void removeAutomationPointsIn (Song& song, BeatRange range)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        SongCurve curve (song, parameter);

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (range.contains (curve.getPointBeat (i)))
                curve.removePoint (i);
    }
}

void addAutomationPoints (Song& song, const std::vector<ParameterPoint>& points)
{
    // Each parameter's through one curve, in the order given: a parameter's curve may have gone with its last point.
    std::vector<ParameterRef> parameters;

    for (const auto& point : points)
        if (std::find (parameters.begin(), parameters.end(), point.parameter) == parameters.end())
            parameters.push_back (point.parameter);

    for (const auto& ref : parameters)
    {
        if (locateParameter (song, ref).has_value())
        {
            SongCurve curve (song, ref);

            for (const auto& [pointParameter, point] : points)
                if (pointParameter == ref)
                    curve.addPoint (point.beat, point.value, point.curve);
        }
    }
}

void forgetCurvesOfGoneDevices (Song& song)
{
    // A device that is there keeps its curves, those of parameters it has now and others (a plug-in that isn't installed
    // has none until it is): a device's, a fader's or a send's.
    auto automation = song.getState().getChildWithName (schema::AUTOMATION);

    for (auto i = automation.getNumChildren(); --i >= 0;)
        if (const auto device = getCurveParameter (automation.getChild (i)).device; ! findDeviceState (song, device).isValid()
                                                                                    && ! findSendState (song, device).isValid()
                                                                                    && ! findChannelOfFader (song, device).isValid())
            automation.removeChild (i, nullptr);
}

//==============================================================================
std::vector<AutomationLane> getLanes (const Song& song, TrackID track)
{
    std::vector<AutomationLane> lanes;

    for (const auto& child : findTrackState (song, track))
    {
        if (! schema::isTrackViewElement (child))
            continue;

        const auto parameter = readLane (child);
        const auto height = juce::jlimit (AutomationLane::minHeight, AutomationLane::maxHeight,
                                          (int) child.getProperty (schema::laneHeight, AutomationLane::defaultHeight));

        if (isOnTrack (song, track, parameter))
            lanes.push_back ({ parameter, height, (bool) child[schema::laneCollapsed] });
    }

    // Automation never hides: a parameter with automation that the track keeps no lane for shows one after the others, by
    // its device's place and then its own. A device's mix (Mixer.h: getMix()) has no lane.
    const auto devices = getLaneDevices (song, track);
    std::vector<std::pair<std::pair<size_t, int>, ParameterRef>> automated;

    for (const auto& curve : song.getState().getChildWithName (schema::AUTOMATION))
    {
        const auto ref = getCurveParameter (curve);
        const auto device = std::find (devices.begin(), devices.end(), ref.device);

        if (device == devices.end() || curve.getNumChildren() == 0
            || ref.parameter == juce::String (devices::mixParameterID.data(), devices::mixParameterID.size())
            || ref.parameter == "dry level" || ref.parameter == "wet level")
            continue;

        if (std::none_of (lanes.begin(), lanes.end(), [&ref] (const AutomationLane& lane) { return lane.parameter == ref; }))
            automated.push_back ({ { (size_t) (device - devices.begin()), getLaneOrder (song, ref) }, ref });
    }

    std::stable_sort (automated.begin(), automated.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& each : automated)
        lanes.push_back ({ each.second });

    return lanes;
}

void showLane (Song& song, TrackID track, const ParameterRef& parameter)
{
    if (! isOnTrack (song, track, parameter))
        return;

    keepShownLanes (song, track);

    if (auto lane = findLaneState (song, track, parameter); lane.isValid())
    {
        lane.removeProperty (schema::laneCollapsed, nullptr);
        return;
    }

    juce::ValueTree lane (schema::LANE);
    writeLane (lane, parameter);
    findTrackState (song, track).appendChild (lane, nullptr);
}

void removeLane (Song& song, TrackID track, const ParameterRef& parameter)
{
    if (auto lane = findLaneState (song, track, parameter); lane.isValid())
        findTrackState (song, track).removeChild (lane, nullptr);
}

void replaceLane (Song& song, TrackID track, const ParameterRef& parameter, const ParameterRef& replacement)
{
    keepShownLanes (song, track);
    auto lane = findLaneState (song, track, parameter);

    if (! lane.isValid() || parameter == replacement || ! isOnTrack (song, track, replacement))
        return;

    removeLane (song, track, replacement); // a lane that shows it already moves here
    writeLane (lane, replacement);
}

void setLaneHeight (Song& song, TrackID track, const ParameterRef& parameter, int height)
{
    keepShownLanes (song, track);

    if (auto lane = findLaneState (song, track, parameter); lane.isValid())
        lane.setProperty (schema::laneHeight, juce::jlimit (AutomationLane::minHeight, AutomationLane::maxHeight, height), nullptr);
}

void setLaneCollapsed (Song& song, TrackID track, const ParameterRef& parameter, bool collapsed)
{
    keepShownLanes (song, track);

    if (auto lane = findLaneState (song, track, parameter); lane.isValid())
    {
        if (collapsed)
            lane.setProperty (schema::laneCollapsed, true, nullptr);
        else
            lane.removeProperty (schema::laneCollapsed, nullptr);
    }
}

void copyCurves (Song& song, const CopiedIDs& copied)
{
    auto automation = song.getState().getChildWithName (schema::AUTOMATION);
    std::vector<juce::ValueTree> curves;

    for (const auto& curve : automation)
        curves.push_back (curve);

    for (const auto& curve : curves)
    {
        const auto ref = getCurveParameter (curve);
        const auto found = copied.find (ref.device.getRaw());

        if (found == copied.end())
            continue;

        if (const ParameterRef copiedRef { DeviceID::fromRaw (found->second), ref.parameter };
            ! findCurveIn (automation, copiedRef).isValid())
        {
            auto copiedState = curve.createCopy();
            copiedState.setProperty (schema::curveDevice, juce::String ((juce::int64) found->second), nullptr);
            automation.appendChild (copiedState, &song.getUndoManager());
        }
    }
}

AutomationMode getAutomationMode (const Song& song, TrackID track)
{
    return readAutomationMode (findTrackState (song, track));
}

void setAutomationMode (Song& song, TrackID track, AutomationMode mode)
{
    if (auto state = findTrackState (song, track); state.isValid())
        writeAutomationMode (state, mode);
}

bool isWritingAutomation (const Song& song)
{
    auto states = getTrackStates (song);
    states.push_back (song.getState().getChildWithName (schema::MASTER));
    return std::any_of (states.begin(), states.end(),
                        [] (const juce::ValueTree& state) { return readAutomationMode (state) != AutomationMode::read; });
}

bool isParameterValue (const juce::ValueTree& tree, const juce::Identifier& property)
{
    // A device's and a modulator's values set by hand are their VALUES' properties, by the parameters' IDs.
    if (tree.hasType (schema::VALUES))
        return true;

    if (tree.hasType (schema::VALUE))
        return property == schema::valueSetByHand;

    if (tree.hasType (schema::TRACK) || tree.hasType (schema::MASTER))
        return property == schema::channelVolume || property == schema::channelPan;

    if (tree.hasType (schema::SEND))
        return property == schema::sendLevel;

    return tree.hasType (schema::MACRO) && property == schema::macroValue;
}

//==============================================================================
std::optional<PointSelection> getSelectedPoints (const Selection& selection)
{
    auto* song = selection.getSong();
    const auto& parameter = selection.getPointsParameter();

    if (song == nullptr || ! locateParameter (*song, parameter).has_value())
        return {};

    auto indices = getValidIndices (findCurveState (*song, parameter).getNumChildren(), selection.getPoints());

    if (indices.empty())
        return {};

    return PointSelection { parameter, std::move (indices) };
}

void selectPoints (Selection& selection, const ParameterRef& parameter, const std::vector<int>& indices)
{
    if (auto* song = selection.getSong())
        selection.selectPoints (parameter, getValidIndices (findCurveState (*song, parameter).getNumChildren(), indices));
}

//==============================================================================
AutomationWatcher::AutomationWatcher (Song& s)
    : song (&s)
{
    song->addParameterListener (this);
}

AutomationWatcher::~AutomationWatcher()
{
    song->removeParameterListener (this);
    cancelPendingUpdate();
}

void AutomationWatcher::valueSetByHand (const ParameterRef& parameter, float value, bool byPlugin)
{
    // A modulator's parameters and a macro's value aren't automated: no touch of theirs is one to automate.
    const auto found = locateParameter (*song, parameter);

    if (! found.has_value() || found->source == ParameterSource::modulator || found->source == ParameterSource::macro)
        return;

    // A value set by a control has moved the one point already (setValueSetByHand()); one the plug-in set itself moves it
    // here.
    if (byPlugin)
        moveOnePoint (*song, parameter, value);

    if (std::find (changedAtOnce.begin(), changedAtOnce.end(), parameter) == changedAtOnce.end())
        changedAtOnce.push_back (parameter);

    triggerAsyncUpdate();
}

void AutomationWatcher::handleAsyncUpdate()
{
    // Many parameters changed at once (a device made, a preset loaded) are no one's touch.
    if (! changedAtOnce.empty() && changedAtOnce.size() <= maxTouchedAtOnce)
        lastTouched = changedAtOnce.back();

    changedAtOnce.clear();
}
} // namespace phazite
