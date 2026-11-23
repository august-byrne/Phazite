// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Automation.h"
#include "engine/automation/Curve.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TrackSwitchesValue.h"
#include "model/base/internal/TracksOutside.h"
#include "model/base/internal/TracksValue.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // A position along a control's travel (AutomationValues.h).
    float limitPosition (float position)
    {
        return juce::jlimit (0.0f, 1.0f, position);
    }

    float limitCurve (float curve)
    {
        return juce::jlimit (-1.0f, 1.0f, curve);
    }

    // A curve of the song's (CurveValues.h) as Phazite's engine plays it (engine/automation/Curve.h), in positions.
    engine::Curve makeEngineCurve (const CurveValue& curve)
    {
        std::vector<engine::CurvePoint> points;
        points.reserve (curve.points.size());

        for (const auto& point : curve.points)
            points.push_back ({ point.beat.inBeats(), point.position, point.curve });

        return engine::Curve (std::move (points));
    }

    // A device's ID as a lane keeps it (read through the tracks' view's conversion: TracksOutside.h, readLaneValue()).
    juce::String writeDeviceID (DeviceID id)
    {
        return juce::String ((juce::int64) id.getRaw());
    }

    // A parameter's position at a beat as its curve has it, as Phazite's engine plays it; without points, its value set by
    // hand's.
    float getCurvePositionAt (const Song& song, const ParameterRef& ref, const CurveValue& curve, BeatPosition beat)
    {
        if (curve.points.empty())
        {
            const auto travel = getControlTravel (song, ref);
            return travel.has_value() ? toControlPosition (*travel, getValueSetByHand (song, ref)) : 0.0f;
        }

        return makeEngineCurve (curve).getPositionAt (std::max (0.0, beat.inBeats()));
    }

    //==========================================================================
    // A point put on a curve where it plays as it did, so that what lies on either side keeps its shape whatever becomes of
    // the other (engine/automation/Curve.h). Within a line, the point before it bends to it, and it to the next, each
    // part as the whole line bent there: up to half a bend, a line eases as a curve of e (devices/dsp/Math.h:
    // getEasedProgress()), whose two parts are that curve again over a part of its span, so the bend shares out by the
    // parts' lengths. A stronger bend holds at one end, then eases all the way: a part that holds and eases a part of the
    // way only, which no one bend draws, ends its hold at a point of its own.
    struct PointsOnLine
    {
        int from = -1;                       // the point the line starts at, whose bend changes; none outside a line
        float fromCurve = 0.0f;              // its bend then
        std::vector<AutomationPoint> points; // added, in time order: the one at the beat, and before or after it where a hold ends
        size_t atBeat = 0;                   // which of them is at the beat
    };

    PointsOnLine planPointOnLine (const std::vector<AutomationPoint>& points, BeatPosition beat)
    {
        PointsOnLine plan;

        // The last point at or before the beat, and the next after it.
        auto before = -1;

        while (before + 1 < (int) points.size() && points[(size_t) before + 1].beat <= beat)
            ++before;

        // Before the first point or after the last the curve holds: a straight line holds it there too.
        if (before < 0 || before + 1 >= (int) points.size())
        {
            const auto position = points.empty() ? 0.0f : points[(size_t) std::max (0, before)].position;
            plan.points.push_back ({ beat, position, 0.0f });
            return plan;
        }

        const auto& from = points[(size_t) before];
        const auto& to = points[(size_t) before + 1];
        const auto length = (to.beat - from.beat).inBeats();
        const auto along = std::clamp ((beat - from.beat).inBeats() / length, 0.0, 1.0);
        const auto bend = (double) juce::jlimit (-1.0f, 1.0f, from.curve);
        const auto positionAlong = [&] (double place)
        { return from.position + (float) engine::getCurveProgress (place, bend) * (to.position - from.position); };
        const auto beatAlong = [&] (double place) { return from.beat + BeatDuration::fromBeats (place * length); };

        plan.from = before;
        plan.fromCurve = from.curve;

        // At the line's start, a second point where the first is, bending as it does.
        if (along <= 0.0)
        {
            plan.points.push_back ({ beat, from.position, from.curve });
            return plan;
        }

        const auto amount = std::abs (bend);
        const auto at = AutomationPoint { beat, positionAlong (along), 0.0f };

        if (amount <= 0.5)
        {
            plan.fromCurve = (float) (bend * along);
            plan.points.push_back ({ at.beat, at.position, (float) (bend * (1.0 - along)) });
            return plan;
        }

        const auto held = (amount - 0.5) * 2.0; // how much of the line holds: at its start bent up, at its end bent down

        if (bend > 0.0)
        {
            // Still holding: the first part holds, the rest holds for what is left of the hold, then eases all the way.
            if (along <= held)
            {
                plan.fromCurve = 0.0f;
                plan.points.push_back ({ at.beat, at.position, (float) (0.5 + 0.5 * (held - along) / (1.0 - along)) });
                return plan;
            }

            // Past the hold: a point where it ends, then each part eases over its share of the rest.
            const auto eased = (along - held) / (1.0 - held);
            plan.fromCurve = 0.0f;
            plan.points.push_back ({ beatAlong (held), from.position, (float) (0.5 * eased) });
            plan.points.push_back ({ at.beat, at.position, (float) (0.5 * (1.0 - eased)) });
            plan.atBeat = 1;
            return plan;
        }

        const auto easing = 1.0 - held; // bent down, it eases first, then holds the next point's value

        if (along >= easing)
        {
            plan.fromCurve = (float) -(0.5 + 0.5 * (along - easing) / along);
            plan.points.push_back ({ at.beat, at.position, 0.0f });
            return plan;
        }

        const auto eased = along / easing;
        plan.fromCurve = (float) (-0.5 * eased);
        plan.points.push_back ({ at.beat, at.position, (float) (-0.5 * (1.0 - eased)) });
        plan.points.push_back ({ beatAlong (easing), to.position, 0.0f });
        return plan;
    }

    // The same, on points of the model's own: where the one at the beat is among them.
    int addPointOnLine (std::vector<AutomationPoint>& points, BeatPosition beat)
    {
        const auto plan = planPointOnLine (points, beat);

        if (plan.from >= 0)
            points[(size_t) plan.from].curve = plan.fromCurve;

        auto index = -1;

        for (size_t i = 0; i < plan.points.size(); ++i)
        {
            const auto& point = plan.points[i];
            const auto place = std::upper_bound (points.begin(), points.end(), point.beat,
                                                 [] (BeatPosition b, const AutomationPoint& each) { return b < each.beat; });
            // Its place counted before it goes in: the points may move as one is added, and which side of a subtraction
            // is worked out first is the compiler's to choose (GCC took where they began before they moved).
            const auto added = (int) (place - points.begin());
            points.insert (place, point);

            if (i == plan.atBeat)
                index = added;
        }

        return index;
    }

    //==========================================================================
    // The song's curves (SongValues.h: SongValues::curves).

#if JUCE_DEBUG
    // The curves a SongCurve edits now, on this thread: one at a time for a parameter of a song.
    std::vector<std::pair<const Song*, ParameterRef>>& getCurvesOpen()
    {
        thread_local std::vector<std::pair<const Song*, ParameterRef>> open;
        return open;
    }
#endif

    // A parameter's curve in the song, edited point by point, in beats: an edit of its value (CurveValues.h), taken from the
    // curves' area as the edit is made, which commit() gives the area once (SongValues.h: setCurves(), in the mode of the
    // moment: one value for the whole edit, which the step's one action of the area holds), and which every edit commits
    // before it ends. Points keep time order, one added at a beat that has points going after them; a curve left without
    // points goes, and a point added to a parameter without a curve makes one, after the song's others (one emptied and
    // filled again in one edit too, as its element once went and came back). A point the edit keeps keeps its key, which the
    // selection of points holds (Selection: getPointKeys()); one it makes gets a new one (SongValues.h: givePointKey()). One
    // at a time for a parameter (asserted): each holds the curve as it found it.
    class SongCurve
    {
    public:
        SongCurve (Song& s, const ParameterRef& r)
            : song (s),
              ref (r)
        {
            if (const auto* found = findCurve (song, ref))
            {
                value = *found;
                inSong = wasInSong = true;
            }

#if JUCE_DEBUG
            auto& open = getCurvesOpen();
            jassert (
                std::none_of (open.begin(), open.end(), [this] (const auto& each) { return each.first == &song && each.second == ref; }));
            open.emplace_back (&song, ref);
#endif
        }

        ~SongCurve()
        {
            jassert (! edited); // every edit commits what it did

#if JUCE_DEBUG
            auto& open = getCurvesOpen();
            open.erase (
                std::find_if (open.begin(), open.end(), [this] (const auto& each) { return each.first == &song && each.second == ref; }));
#endif
        }

        // A point's beat, position and bend; for an index the curve hasn't (asserted), 0, as the element's missing child read.
        int getNumPoints() const { return (int) value.points.size(); }
        BeatPosition getPointBeat (int index) const { return getPointAt (index).beat; }
        float getPointPosition (int index) const { return getPointAt (index).position; }
        float getPointCurve (int index) const { return getPointAt (index).curve; }

        // Its position as Phazite's engine plays it; without points, the parameter's value set by hand's.
        float getPositionAt (BeatPosition beat) const { return getCurvePositionAt (song, ref, value, beat); }

        std::vector<AutomationPoint> getPoints() const
        {
            std::vector<AutomationPoint> points;
            points.reserve (value.points.size());

            for (const auto& point : value.points)
                points.push_back ({ point.beat, point.position, point.curve });

            return points;
        }

        // A point at a beat, after any there, where the curve plays as it did on both sides of it (planPointOnLine()): its
        // index.
        int addPointOnLine (BeatPosition beat)
        {
            if (getNumPoints() == 0)
                return addPoint (beat, getPositionAt (beat), 0.0f); // the value set by hand's

            const auto plan = planPointOnLine (getPoints(), beat);

            if (plan.from >= 0)
                setCurve (plan.from, plan.fromCurve);

            auto index = -1;

            for (size_t i = 0; i < plan.points.size(); ++i)
                if (const auto added = addPoint (plan.points[i].beat, plan.points[i].position, plan.points[i].curve); i == plan.atBeat)
                    index = added;

            return index;
        }

        // The same, only inside a bent line: a straight one keeps its shape however its ends move apart or together. Whether
        // it cut one.
        bool cutBentLineAt (BeatPosition beat)
        {
            auto before = -1;

            while (before + 1 < getNumPoints() && getPointBeat (before + 1) <= beat)
                ++before;

            if (before < 0 || before + 1 >= getNumPoints() || getPointBeat (before) >= beat
                || juce::exactlyEqual (getPointCurve (before), 0.0f))
                return false;

            addPointOnLine (beat);
            return true;
        }

        // The bend of the line that plays at a beat, after any points there: none before the first point or after the last.
        float getCurveAt (BeatPosition beat) const
        {
            auto before = -1;

            while (before + 1 < getNumPoints() && getPointBeat (before + 1) <= beat)
                ++before;

            return before >= 0 && before + 1 < getNumPoints() ? getPointCurve (before) : 0.0f;
        }

        int addPoint (BeatPosition beat, float position, float curve)
        {
            if (! ref.isValid())
                return -1;

            // A curve gone in this edit made again: after the song's others, as a curve the song hasn't is made.
            if (! inSong)
            {
                inSong = true;
                madeAgain = wasInSong;
            }

            // After those at or before its beat: where the first point after it is.
            size_t index = 0;

            for (const auto& each : value.points)
            {
                if (! (each.beat <= beat))
                    break;

                ++index;
            }

            auto* values = song.getValues();
            const PointValue point { BeatPosition::fromBeats (std::max (0.0, beat.inBeats())), position, readCurve (curve),
                                     values != nullptr ? values->givePointKey() : std::uint64_t { 0 } };
            value.points = value.points.insert (index, point);
            edited = true;
            return (int) index;
        }

        // An index the curve hasn't (asserted) changes nothing.
        void removePoint (int index)
        {
            if (! hasPoint (index))
                return;

            value.points = value.points.erase ((size_t) index);
            edited = true;

            // A curve without points goes.
            if (value.points.empty())
                inSong = false;
        }

        // A point moved or bent keeps its key.
        void setPoint (int index, BeatPosition beat, float position)
        {
            if (! hasPoint (index))
                return;

            auto point = value.points[(size_t) index];
            point.beat = BeatPosition::fromBeats (std::max (0.0, beat.inBeats()));
            point.position = position;
            value.points = value.points.set ((size_t) index, point);
            edited = true;
        }

        void setCurve (int index, float curve)
        {
            if (! hasPoint (index))
                return;

            auto point = value.points[(size_t) index];
            point.curve = readCurve (curve);
            value.points = value.points.set ((size_t) index, point);
            edited = true;
        }

        // What the edit did, given the song's curves' area (above): the curve as the edit has it, in its place, or after the
        // others where it is new or was made again, or taken out where it is gone; nothing where nothing was edited, and no
        // step where the curve is as it was (SongValues.h: setCurves()). A song that holds no values has no curves to edit.
        void commit()
        {
            if (! std::exchange (edited, false))
                return;

            auto* values = song.getValues();
            jassert (values != nullptr);

            if (values == nullptr)
                return;

            const auto& curves = values->curves.get();

            if (! inSong)
                values->setCurves (song, withoutCurve (curves, ref));
            else if (madeAgain)
                values->setCurves (song, withCurve (withoutCurve (curves, ref), ref, value));
            else
                values->setCurves (song, withCurve (curves, ref, value));

            wasInSong = inSong;
            madeAgain = false;
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

                const auto v = getPointPosition (i);
                const auto sameLeft = i <= 0 || std::abs (getPointPosition (i - 1) - v) < threshold;
                const auto sameRight = i >= getNumPoints() - 1 || std::abs (getPointPosition (i + 1) - v) < threshold;

                if ((sameLeft && sameRight)
                    || (i < getNumPoints() - 1 && std::abs (getPointPosition (i + 1) - v) < threshold
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
        CurveValue value;       // the curve as the edit has it now
        bool inSong = false;    // whether the song has a curve for the parameter, as the edit has it now
        bool wasInSong = false; // whether it had one as the edit found it, or as it last committed
        bool madeAgain = false; // gone and made again since: after the song's others
        bool edited = false;    // since it last committed

        // A bend as a file holds it: 0, straight, written as none, reads as 0.
        static float readCurve (float curve) { return juce::exactlyEqual (curve, 0.0f) ? 0.0f : curve; }

        // Whether the curve has a point at an index; an edit asks only of those it has (asserted).
        bool hasPoint (int index) const
        {
            const auto has = juce::isPositiveAndBelow (index, getNumPoints());
            jassert (has);
            return has;
        }

        PointValue getPointAt (int index) const { return hasPoint (index) ? value.points[(size_t) index] : PointValue {}; }

        JUCE_DECLARE_NON_COPYABLE (SongCurve)
    };

    // Whether what a curve names is in the song: a device's, a fader's, a send's or a container's chain's (its volume, pan
    // and crossover), each an automated parameter's owner as locateParameter() finds it, whether or not it knows the
    // parameter now (a plug-in that isn't installed, or doesn't load, knows none of its own).
    bool isCurveOwnerThere (const Song& song, DeviceID owner)
    {
        return holdsDevice (song, owner, true) || getSendValue (song, owner).has_value() || findChannelOfFader (song, owner).isValid();
    }

    // The parameters the song has curves for whose devices are there, a plug-in's that isn't loaded too; not those of
    // devices gone (whose curves the song keeps for undo).
    std::vector<ParameterRef> getParametersWithCurves (const Song& song)
    {
        std::vector<ParameterRef> parameters;

        for (const auto& ref : getCurveKeysWithPoints (song))
            if (isCurveOwnerThere (song, ref.device))
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
        if (beat - justBefore > BeatPosition())
            curve.addPointOnLine (beat - justBefore);

        curve.addPointOnLine (beat);
    }

    // Afterwards, the points at the edges that change nothing go (SongCurve::removeRedundantPoints()).
    void tidyEdges (SongCurve& curve, const std::vector<BeatPosition>& edges)
    {
        for (const auto beat : edges)
            curve.removeRedundantPoints ({ beat - justBefore * 2.0, beat + justBefore });
    }

    std::vector<DeviceID> getLaneDevices (const Song&, TrackID);

    // The parameters with points of the devices on a track (its fader, its sends, its devices and its containers' chains):
    // those of a plug-in that isn't loaded too, whose parameters only its instance knows.
    std::vector<ParameterRef> getAutomatedParameters (const Song& song, TrackID track)
    {
        std::vector<ParameterRef> automated;
        const auto devices = getLaneDevices (song, track);

        for (const auto& ref : getCurveKeysWithPoints (song))
            if (std::find (devices.begin(), devices.end(), ref.device) != devices.end())
                automated.push_back (ref);

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
        const auto value = readLaneValue (lane); // the tracks' view's conversion (TracksOutside.h)
        return { value.device, value.parameter };
    }

    void writeLane (juce::ValueTree& lane, const ParameterRef& parameter)
    {
        lane.setProperty (schema::laneDevice, writeDeviceID (parameter.device), nullptr);
        lane.setProperty (schema::laneParameter, parameter.parameter, nullptr);
    }

    // The devices whose parameters a track's lanes show, in the order the lanes of their automation come: its channel's
    // fader (or a plain folder's VCA), its sends, then its devices, a container's chains in its place, each before its
    // devices (a chain's volume, pan and crossover are its own).
    std::vector<DeviceID> getLaneDevices (const Song& song, TrackID track)
    {
        std::vector<DeviceID> ids;
        const auto state = findTrackState (song, track);

        // Read by the tracks' conversion (TracksValue.h).
        for (const auto id : { readChannelFader (state), readChannelVCA (state) })
            if (id.isValid())
                ids.push_back (id);

        for (const auto& send : getSendValues (song, track))
            ids.push_back (send.id);

        // Its devices, its containers' chains and their devices, in their order (Mixer.h: getDeviceOutline()), a track's the song
        // has.
        for (const auto& device : state.isValid() ? getDeviceOutline (song, track) : std::vector<DeviceOutline>())
            if (device.form != DeviceValue::Form::container)
                ids.push_back (device.id);

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

        if (const auto* info = getOwnDeviceInfo (song, parameter.device))
            if (const auto index = devices::findParameter (*info, parameter.parameter.toStdString()); index >= 0)
                return index;

        return std::numeric_limits<int>::max();
    }

    // The lanes a track keeps (Schema.h: LANES), none while it has kept none. The element that holds them is added after
    // the track's other elements as its first lane is kept, and stays: the undo history finds those other elements by their
    // places, which neither a lane nor its holder may move, being changed outside it.
    juce::ValueTree findLanesState (const Song& song, TrackID track)
    {
        return findTrackState (song, track).getChildWithName (schema::LANES);
    }

    void keepLane (Song& song, TrackID track, const ParameterRef& parameter)
    {
        if (auto state = findTrackState (song, track); state.isValid())
        {
            juce::ValueTree lane (schema::LANE);
            writeLane (lane, parameter);
            state.getOrCreateChildWithName (schema::LANES, nullptr).appendChild (lane, nullptr);
        }
    }

    juce::ValueTree findLaneState (const Song& song, TrackID track, const ParameterRef& parameter)
    {
        for (const auto& lane : findLanesState (song, track))
            if (readLane (lane) == parameter)
                return lane;

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
            if (! findLaneState (song, track, shown.parameter).isValid())
                keepLane (song, track, shown.parameter);
    }

    // A track's write mode as the song keeps it (Schema.h: trackAutomationMode).
    AutomationMode readAutomationMode (const juce::ValueTree& state)
    {
        // The switches' conversion (TrackSwitchesValue.h): none is read.
        if (const auto mode = readSavedAutomationMode (state))
        {
            switch (*mode)
            {
                case SavedAutomationMode::touch:
                    return AutomationMode::touch;
                case SavedAutomationMode::latch:
                    return AutomationMode::latch;
                case SavedAutomationMode::write:
                    return AutomationMode::write;
            }
        }

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
            return getTrackID (found->state); // a fader's channel (FoundParameter::state)
        case ParameterSource::send:
            return getSendTrack (song, parameter.device); // the track a send is on, by its ID
        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
        case ParameterSource::crossover:
            return getDeviceChannel (song, getChainContainer (song, parameter.device)); // its container's
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

    // Each device's, in the channel's order (Mixer.h: getDeviceOutline()): a container's chains after it, each its volume and
    // pan (and a band's crossover), then its devices'. A device's mix (Mixer.h: getMix()) is its own knob, not one of these;
    // a container has no parameters of its own.
    for (const auto& device : getDeviceOutline (song, track))
    {
        switch (device.form)
        {
            case DeviceValue::Form::container:
                break;

            case DeviceValue::Form::chain:
                for (const auto* parameter : { schema::chainVolumeParameter, schema::chainPanParameter, schema::crossoverParameter })
                    if (locateParameter (song, { device.id, parameter }).has_value())
                        parameters.push_back ({ device.id, parameter });

                break;

            // A plug-in's, as its instance lists them, by their own IDs: those it lets be automated.
            case DeviceValue::Form::plugin:
                if (auto* instance = findPluginInstance (song, device.id))
                    for (const auto& info : instance->getParameterInfos())
                        if (info.isAutomatable)
                            parameters.push_back ({ device.id, info.id });

                break;

            // One of Phazite's own devices', in its description's order.
            case DeviceValue::Form::own:
                if (const auto* own = findOwnDevice (device.type.toStdString()))
                    for (const auto& info : own->parameters)
                        if (own->kind == devices::DeviceKind::instrument || info.id != devices::mixParameterID)
                            parameters.push_back ({ device.id, juce::String (info.id.data(), info.id.size()) });

                break;
        }
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

        // A chain's, by its name: "Chain 2 Volume".
        case ParameterSource::chainVolume:
        case ParameterSource::chainPan:
        case ParameterSource::crossover:
            return found->state[schema::chainName].toString() + " " + describeParameter (song, *found).name;

        case ParameterSource::device:
        case ParameterSource::plugin:
        case ParameterSource::modulator:
        case ParameterSource::macro:
            break;
    }

    return describeParameter (song, *found).name;
}

juce::String getParameterDeviceName (const Song& song, const ParameterRef& parameter)
{
    if (isMixerControl (song, parameter))
        return "Mixer";

    // A container's chain's parameters are its container's.
    if (const auto container = getChainContainer (song, parameter.device); container.isValid())
        return getDeviceName (song, container);

    return getDeviceName (song, parameter.device);
}

//==============================================================================
std::vector<AutomationPoint> getPoints (const Song& song, const ParameterRef& parameter)
{
    std::vector<AutomationPoint> points;

    if (const auto* curve = findCurve (song, parameter))
    {
        points.reserve (curve->points.size());

        for (const auto& point : curve->points)
            points.push_back ({ point.beat, point.position, point.curve });
    }

    return points;
}

bool hasPoints (const Song& song, const ParameterRef& parameter)
{
    const auto* curve = findCurve (song, parameter);
    return curve != nullptr && ! curve->points.empty();
}

std::vector<float> getPositions (const Song& song, const ParameterRef& parameter, BeatRange range, int numValues)
{
    std::vector<BeatPosition> beats;
    beats.reserve ((size_t) std::max (0, numValues));
    const auto step = numValues > 1 ? range.getLength() / (double) (numValues - 1) : BeatDuration();

    for (int i = 0; i < numValues; ++i)
        beats.push_back (range.getStart() + step * (double) i);

    return getPositions (song, parameter, beats);
}

std::vector<float> getPositions (const Song& song, const ParameterRef& parameter, const std::vector<BeatPosition>& beats)
{
    // As Phazite's engine plays the song's points.
    std::vector<float> positions;
    const auto* value = beats.empty() ? nullptr : findCurve (song, parameter);

    if (value == nullptr || value->points.empty())
        return positions;

    const auto curve = makeEngineCurve (*value);
    positions.reserve (beats.size());

    for (const auto beat : beats)
        positions.push_back (curve.getPositionAt (std::max (0.0, beat.inBeats())));

    return positions;
}

int addPoint (Song& song, const ParameterRef& parameter, AutomationPoint point)
{
    SongCurve curve (song, parameter);
    const auto index = curve.addPoint (point.beat, limitPosition (point.position), limitCurve (point.curve));
    curve.commit();
    return index;
}

BeatDuration movePoints (Song& song, const ParameterRef& parameter, const std::vector<int>& requested, BeatDuration distance,
                         float positionChange)
{
    SongCurve curve (song, parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    if (indices.empty())
        return {};

    const auto points = curve.getPoints(); // as the curve it edits holds them, by its indices
    std::vector<bool> moving (points.size(), false);

    for (const auto index : indices)
        moving[(size_t) index] = true;

    // The distance, cut so that no point passes one that stays or goes before the song's start; the change, so that every
    // point stays on the control's travel.
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

        lowest = std::max (lowest, -point.position);
        highest = std::min (highest, 1.0f - point.position);
    }

    const auto beats = juce::jlimit (std::min (earliest, 0.0), std::max (latest, 0.0), distance.inBeats());
    const auto change = juce::jlimit (std::min (lowest, 0.0f), std::max (highest, 0.0f), positionChange);

    // In the order that keeps the points in time order all along.
    const auto move = [&] (int index)
    {
        const auto& point = points[(size_t) index];
        curve.setPoint (index, point.beat + BeatDuration::fromBeats (beats), limitPosition (point.position + change));
    };

    if (beats > 0.0)
        std::for_each (indices.rbegin(), indices.rend(), move);
    else
        std::for_each (indices.begin(), indices.end(), move);

    curve.commit();
    return BeatDuration::fromBeats (beats);
}

void setCurve (Song& song, const ParameterRef& parameter, int index, float curveValue)
{
    setCurves (song, parameter, { index }, curveValue);
}

void setCurves (Song& song, const ParameterRef& parameter, const std::vector<int>& indices, float curveValue)
{
    SongCurve curve (song, parameter);
    const auto bend = limitCurve (curveValue);

    // In the order given, each one the curve has, as a bend of each point alone, one after another, wrote them.
    for (const auto index : indices)
        if (juce::isPositiveAndBelow (index, curve.getNumPoints()))
            curve.setCurve (index, bend);

    curve.commit();
}

void deletePoints (Song& song, const ParameterRef& parameter, const std::vector<int>& requested)
{
    SongCurve curve (song, parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    for (auto i = indices.rbegin(); i != indices.rend(); ++i)
        curve.removePoint (*i);

    curve.commit();
}

void replacePoints (Song& song, const ParameterRef& parameter, BeatRange range, const std::vector<AutomationPoint>& points)
{
    SongCurve curve (song, parameter);

    for (auto i = curve.getNumPoints(); --i >= 0;)
        if (const auto beat = curve.getPointBeat (i); beat >= range.getStart() && beat <= range.getEnd())
            curve.removePoint (i);

    // Each after any at its beat already, so that two at one beat stay in the order given: a jump.
    for (const auto& point : points)
        curve.addPoint (point.beat, limitPosition (point.position), limitCurve (point.curve));

    curve.commit();
}

std::vector<int> duplicatePoints (Song& song, const ParameterRef& parameter, const std::vector<int>& requested, BeatDuration minimumSpan)
{
    SongCurve curve (song, parameter);
    const auto indices = getValidIndices (curve.getNumPoints(), requested);

    if (indices.empty())
        return {};

    const auto points = curve.getPoints(); // as the curve it edits holds them, by its indices
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
        copies.push_back (curve.addPoint (point.beat + span, point.position, point.curve));
    }

    curve.commit();
    return copies;
}

void moveOnlyPoint (Song& song, const ParameterRef& parameter, float value)
{
    if (auto* session = findSession (song); session != nullptr && session->getWriter().isWriting (parameter))
        return;

    // Every value set by hand comes here: its curve looked at in place, its points counted.
    if (const auto* held = findCurve (song, parameter); held == nullptr || held->points.size() != 1)
        return;

    SongCurve curve (song, parameter);

    if (curve.getNumPoints() == 1)
        if (const auto travel = getControlTravel (song, parameter))
            if (const auto position = toControlPosition (*travel, value); ! juce::exactlyEqual (curve.getPointPosition (0), position))
                curve.setPoint (0, curve.getPointBeat (0), position);

    curve.commit();
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
                    moved.push_back ({ beat + distance, curve.getPointPosition (i), curve.getPointCurve (i) });

            for (auto i = curve.getNumPoints(); --i >= 0;)
                if (const auto beat = curve.getPointBeat (i); (! copy && isInAny (beat, {})) || isInAny (beat, distance))
                    curve.removePoint (i);

            // In order, each after any at its beat already, so that a jump stays one.
            for (const auto& point : moved)
                curve.addPoint (point.beat, point.position, point.curve);

            tidyEdges (curve, edges);
            curve.commit();
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
                        curve.setPoint (i, beat - range->getLength(), curve.getPointPosition (i));
                }

                edges.push_back (range->getStart());

                if (! closeGap)
                    edges.push_back (range->getEnd());
            }

            tidyEdges (curve, edges);
            curve.commit();
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
            // As moveAutomation() takes a stretch: points at its edges, which keep the shape of the lines they cut, and the
            // points between them, from its start.
            for (const auto& range : ranges)
            {
                auto points = getPoints (song, ref);
                const auto first = addPointOnLine (points, range.getStart());
                const auto last = addPointOnLine (points, range.getEnd() - justBefore);
                AutomationCopy copy { ref, range.getLength(), {}, range.getStart() };

                for (auto i = first; i <= last; ++i)
                    copy.points.push_back ({ BeatPosition() + (points[(size_t) i].beat - range.getStart()), points[(size_t) i].position,
                                             i < last ? points[(size_t) i].curve : 0.0f });

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
        if (! isCurveOwnerThere (song, copy.parameter.device))
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
            curve.addPoint (landing.getStart() + (point.beat - BeatPosition()), limitPosition (point.position), limitCurve (point.curve));

        tidyEdges (curve, { landing.getStart(), landing.getEnd() });
        curve.commit();
    }
}

//==============================================================================
bool setCurveValue (Song& song, const ParameterRef& parameter, const CurveValue& value)
{
    auto* values = song.getValues();
    return values != nullptr && parameter.isValid() && values->setCurve (song, parameter, value);
}

bool eraseCurve (Song& song, const ParameterRef& parameter)
{
    auto* values = song.getValues();
    return values != nullptr && values->eraseCurve (song, parameter);
}

void renameCurve (Song& song, const ParameterRef& from, const ParameterRef& to)
{
    if (auto* values = song.getValues())
        values->renameCurve (song, from, to);
}

bool setCurveOrder (Song& song, const std::vector<ParameterRef>& order)
{
    auto* values = song.getValues();
    return values != nullptr && values->setCurveOrder (song, order);
}

AreaChoice chooseCurveClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A path into a curve of the song's AUTOMATION ("AUTOMATION:", "CURVE:1042/cutoff"): a curve merges whole
    // (SongMerge.cpp: mergesWhole()), so its clash is the whole element's, its path the curve's alone. The side chosen's
    // curve made the song's by its parameter (setCurveValue(), read as the song holds a curve: CurveValues.h,
    // readHeldCurve()), or taken out where that side had none (eraseCurve()), in the choice's step. Refused (nothing written)
    // for any other clash through a curve, which a merge of two songs Phazite holds never makes, but a commit's song may come
    // from anyone: a property, the AUTOMATION itself (the curves' place, which every song that holds its values has), an
    // element in a curve, an element of another type or of another parameter, one that stands in for what its version
    // couldn't read (SongPieces.h: holdsStandIn()), and a song that holds no values to choose it in.
    if (clash.path.isEmpty() || ! clash.path[0].startsWith (schema::AUTOMATION.toString() + ":"))
        return AreaChoice::notTheArea;

    if (clash.path.size() != 2 || ! clash.path[1].startsWith (schema::CURVE.toString() + ":"))
        return AreaChoice::refused;

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
    const auto parameter = readCurveKeyText (clash.path[1].fromFirstOccurrenceOf (":", false, false));

    if (clash.property.isValid() || ! juce::isPositiveAndBelow (value, 3) || ! parameter.has_value() || song.getValues() == nullptr)
        return AreaChoice::refused;

    const auto& chosen = *elements[value];

    if (chosen.isValid() && (! chosen.hasType (schema::CURVE) || holdsStandIn (chosen) || readCurveParameter (chosen) != *parameter))
        return AreaChoice::refused;

    if (chosen.isValid())
        setCurveValue (song, *parameter, readHeldCurve (chosen));
    else
        eraseCurve (song, *parameter);

    return AreaChoice::chosen;
}

const SongCurves& getCurvesValue (const Song& song)
{
    static const SongCurves none;
    const auto* values = song.getValues();
    return values != nullptr ? values->curves.get() : none;
}

const CurveValue* findCurve (const Song& song, const ParameterRef& parameter)
{
    return getCurvesValue (song).find (parameter);
}

std::vector<ParameterRef> getCurveKeys (const Song& song)
{
    const auto& order = getCurvesValue (song).order;
    return { order.begin(), order.end() };
}

std::vector<ParameterRef> getCurveKeysWithPoints (const Song& song)
{
    const auto& curves = getCurvesValue (song);
    std::vector<ParameterRef> parameters;

    for (const auto& parameter : curves.order)
        if (const auto* curve = curves.find (parameter); curve != nullptr && ! curve->points.empty())
            parameters.push_back (parameter);

    return parameters;
}

std::optional<CurveValue> getCurveValue (const Song& song, const ParameterRef& parameter)
{
    if (const auto* curve = findCurve (song, parameter))
        return *curve;

    return std::nullopt;
}

std::vector<std::uint64_t> getPointKeys (const Song& song, const ParameterRef& parameter)
{
    std::vector<std::uint64_t> keys;

    if (const auto* curve = findCurve (song, parameter))
    {
        keys.reserve (curve->points.size());

        for (const auto& point : curve->points)
            keys.push_back (point.key);
    }

    return keys;
}

juce::ValueTree copyWholeCurve (const Song& song, const ParameterRef& parameter)
{
    const auto* curve = findCurve (song, parameter);

    if (curve == nullptr)
        return {};

    juce::ValueTree automation (schema::AUTOMATION);
    writeCurveElement (parameter, *curve, automation);
    auto element = automation.getChild (0);
    automation.removeChild (element, nullptr);
    return element;
}

void insertAutomationTime (Song& song, BeatPosition at, BeatDuration length)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        // A bent line across the time cut first, so that each part keeps its shape (addPointOnLine()).
        SongCurve curve (song, parameter);
        const auto cut = curve.cutBentLineAt (at);
        const auto positionThere = curve.getPositionAt (at);

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (const auto beat = curve.getPointBeat (i); beat >= at)
                curve.setPoint (i, beat + length, curve.getPointPosition (i));

        if (cut || ! juce::approximatelyEqual (positionThere, curve.getPositionAt (at))) // a bent line's first part ends there
            curve.addPoint (at, positionThere, 0.0f);

        if (! juce::approximatelyEqual (positionThere, curve.getPositionAt (at + length)))
            curve.addPoint (at + length, positionThere, 0.0f);

        curve.commit();
    }
}

void removeAutomationTime (Song& song, BeatRange range)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        // Bent lines across its ends cut first, so that what lies before it and after it keeps its shape, what is after it
        // bending from its start as it did from its end.
        SongCurve curve (song, parameter);
        const auto cut = curve.cutBentLineAt (range.getStart());
        curve.cutBentLineAt (range.getEnd());
        const auto positionAtStart = curve.getPositionAt (range.getStart()), positionAtEnd = curve.getPositionAt (range.getEnd());
        const auto curveAtEnd = curve.getCurveAt (range.getEnd());

        for (auto i = curve.getNumPoints(); --i >= 0;)
            if (const auto beat = curve.getPointBeat (i); beat >= range.getStart() && beat <= range.getEnd())
                curve.removePoint (i);

        for (int i = 0; i < curve.getNumPoints(); ++i)
            if (const auto beat = curve.getPointBeat (i); beat >= range.getEnd())
                curve.setPoint (i, beat - range.getLength(), curve.getPointPosition (i));

        if (cut || ! juce::exactlyEqual (curve.getPositionAt (range.getStart()), positionAtStart)) // as above
            curve.addPoint (range.getStart(), positionAtStart, 0.0f);

        if (! juce::exactlyEqual (positionAtStart, positionAtEnd) || ! juce::exactlyEqual (curveAtEnd, 0.0f))
            curve.addPoint (range.getStart(), positionAtEnd, curveAtEnd);

        curve.commit();
    }
}

void addAutomationEdges (Song& song, const std::vector<BeatPosition>& beats)
{
    for (const auto& parameter : getParametersWithCurves (song))
    {
        SongCurve curve (song, parameter);

        for (const auto beat : beats)
            addEdge (curve, beat);

        curve.commit();
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

        curve.commit();
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
        if (isCurveOwnerThere (song, ref.device))
        {
            SongCurve curve (song, ref);

            for (const auto& [pointParameter, point] : points)
                if (pointParameter == ref)
                    curve.addPoint (point.beat, point.position, point.curve);

            curve.commit();
        }
    }
}

void forgetCurvesOfGoneDevices (Song& song)
{
    // What is there keeps its curves, those of parameters it has now and others (a plug-in that isn't installed has none
    // until it is): a device's, a fader's, a send's or a container's chain's (its volume, pan and crossover), each an
    // automated parameter's owner as locateParameter() finds it. (A modulator's parameters aren't automated.) A child that is
    // no curve names no device, and goes too. It runs as a song opens, before the song holds its values (Project.cpp:
    // makeSong()), and on a song made of a tree alone (a copy as it would open: tests/SongCheck.cpp): on its tree, without
    // an undo step, which nothing hears as the song's curves.
    jassert (song.getValues() == nullptr);
    auto automation = song.getState().getChildWithName (schema::AUTOMATION);

    for (auto i = automation.getNumChildren(); --i >= 0;)
        if (! isCurveOwnerThere (song, readCurveParameter (automation.getChild (i)).device))
            automation.removeChild (i, nullptr);
}

void forgetCurvesOfGoneDevices (Song& song, const std::function<bool (const ParameterRef&)>& which)
{
    // A curve's alone, each through eraseCurve(): an element of a kind Phazite doesn't know names no device.
    for (const auto& parameter : getCurveKeys (song))
        if (which (parameter) && ! isCurveOwnerThere (song, parameter.device))
            eraseCurve (song, parameter);
}

//==============================================================================
std::vector<AutomationLane> getLanes (const Song& song, TrackID track)
{
    std::vector<AutomationLane> lanes;

    for (const auto& child : findLanesState (song, track))
    {
        const auto lane = readLaneValue (child); // the tracks' view's conversion (TracksOutside.h)
        const ParameterRef parameter { lane.device, lane.parameter };
        const auto height =
            juce::jlimit (AutomationLane::minHeight, AutomationLane::maxHeight, lane.height.value_or (AutomationLane::defaultHeight));

        if (isOnTrack (song, track, parameter))
            lanes.push_back ({ parameter, height, lane.collapsed });
    }

    // Automation never hides: a parameter with automation that the track keeps no lane for shows one after the others, by
    // its device's place and then its own. A device's mix (Mixer.h: getMix()) has no lane.
    const auto devices = getLaneDevices (song, track);
    std::vector<std::pair<std::pair<size_t, int>, ParameterRef>> automated;

    for (const auto& ref : getCurveKeysWithPoints (song))
    {
        const auto device = std::find (devices.begin(), devices.end(), ref.device);

        if (device == devices.end() || ref.parameter == juce::String (devices::mixParameterID.data(), devices::mixParameterID.size()))
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

    keepLane (song, track, parameter);
}

void removeLane (Song& song, TrackID track, const ParameterRef& parameter)
{
    // From the element that holds the track's lanes alone, which stays, so that the track's other elements keep their places.
    if (auto lane = findLaneState (song, track, parameter); lane.isValid())
        lane.getParent().removeChild (lane, nullptr);
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
    // The song's curves as they are before the copies, in their order.
    for (const auto& parameter : getCurveKeys (song))
    {
        const auto found = copied.find (parameter.device.getRaw());

        if (found == copied.end())
            continue;

        // A macro's value is named by the macro's own ID (Parameters.h): its copy's by the copy's (else a duplicated track's
        // macro kept its automation as a curve named by the original's ID, a parameter the song hasn't).
        const auto isMacroValue =
            parameter.parameter == juce::String (parameter.device.getRaw()) && isMacro (song, ModulatorID::fromRaw (found->second));
        const ParameterRef copiedRef { DeviceID::fromRaw (found->second),
                                       isMacroValue ? juce::String (found->second) : parameter.parameter };

        // The original's value copied before the set (which moves the area on), its points given new keys there.
        if (findCurve (song, copiedRef) == nullptr)
            if (const auto curve = getCurveValue (song, parameter))
                setCurveValue (song, copiedRef, *curve);
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
    // A plug-in's values set by hand are its VALUEs' (a device's of Phazite's own, its VALUES' properties, are the devices'
    // area's, which the song's values tell; a modulator's modulation's area's).
    if (tree.hasType (schema::VALUE))
        return property == schema::valueSetByHand;

    return (tree.hasType (schema::TRACK) || tree.hasType (schema::MASTER))
           && (property == schema::channelVolume || property == schema::channelPan);
}

//==============================================================================
std::optional<PointSelection> getSelectedPoints (const Selection& selection)
{
    auto* song = selection.getSong();
    const auto& parameter = selection.getPointsParameter();

    // Of a curve whose device is there (one of a plug-in that isn't loaded too), as Selection::getNumObjectsSelected()
    // counts them.
    if (song == nullptr || ! parameter.isValid() || ! isCurveOwnerThere (*song, parameter.device))
        return {};

    // Where each selected point is in its curve now, by its key: the curve looked up once, and its points walked once, each
    // looked for among the selected keys (Selection: isPointSelected()); one that is gone (removed, or its curve with it) is
    // left out. In the curve's order, each once.
    const auto* curve = findCurve (*song, parameter);

    if (curve == nullptr)
        return {};

    std::vector<int> indices;
    auto index = 0;

    for (const auto& point : curve->points)
    {
        if (selection.isPointSelected (point.key))
            indices.push_back (index);

        ++index;
    }

    if (indices.empty())
        return {};

    return PointSelection { parameter, std::move (indices) };
}

void selectPoints (Selection& selection, const ParameterRef& parameter, const std::vector<int>& indices)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return;

    // The points at those places in the curve now, by their keys.
    const auto* curve = findCurve (*song, parameter);
    std::vector<std::uint64_t> keys;

    if (curve != nullptr)
        for (const auto index : getValidIndices ((int) curve->points.size(), indices))
            keys.push_back (curve->points[(size_t) index].key);

    selection.selectPoints (parameter, keys);
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
    // here, in the song's history: the step the change joins.
    if (byPlugin)
        moveOnlyPoint (*song, parameter, value);

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
