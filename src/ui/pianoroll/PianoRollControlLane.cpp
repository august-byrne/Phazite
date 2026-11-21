// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's controller lane, in the velocity lane's place while a controller is chosen, and the chooser of what the
// lane under the notes shows (docs/design/piano-roll.md, "Controller lanes").
#include "PianoRollParts.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace phazite::ui
{
namespace
{
    // A cell's end, where the next cell starts, is left out of the range a draw replaces, which takes its ends in.
    const auto tinyBeats = BeatDuration::fromBeats (1.0e-6);

    constexpr int mostCellsAnEvent = 100000; // however far the pointer jumps between two of the mouse's reports

    // "Sustain Pedal (CC 64)", "Sustain Pedal (CC 64) and Pitch Bend", "Mod Wheel (CC 1), Sustain Pedal (CC 64) and Pitch Bend".
    juce::String joinNames (const std::vector<juce::String>& names)
    {
        juce::String joined;

        for (size_t i = 0; i < names.size(); ++i)
            joined << (i == 0 ? "" : (i + 1 == names.size() ? " and " : ", ")) << names[i];

        return joined;
    }
} // namespace

//==============================================================================
PianoRoll::LaneButton::LaneButton()
    : juce::TextButton ("Velocity")
{
    setTitle ("Lane: Velocity"); // the lane shown, for a screen reader (PianoRoll::updateLaneButton())
    setDescription ("What the lane under the notes shows: the notes' velocities, or a controller's values");
    setTooltip ("Velocity");
    setMouseClickGrabsKeyboardFocus (false); // as the fold chevron beside it: the keys stay with the piano roll
}

void PianoRoll::LaneButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    juce::TextButton::paintButton (g, highlighted, down);

    // The clips shown hold values this lane doesn't show: they play, so the chooser says so.
    if (marked)
    {
        const auto size = ControlLaneTint::mark;
        g.setColour (getPalette (*this).accent);
        g.fillEllipse (juce::Rectangle<float> (size, size).withPosition ((float) getWidth() - size - 1.0f, 1.0f));
    }
}

//==============================================================================
void PianoRoll::showLane (const std::optional<ControlLane>& lane)
{
    if (lane == laneShown)
        return;

    controlLane->endDrag();
    laneShown = lane;
    controlLane->drawn.reset();
    updateLaneButton();
    resized();
    repaint (velocityRow);
}

const std::vector<ControlLane>& PianoRoll::getLanesHeld()
{
    const auto version = context.getSongVersion();

    if (! lanesHeld.has_value() || lanesHeld->songVersion != version || lanesHeld->shown != clips)
        lanesHeld = LanesHeld { getControlLanes (context, clips), version, clips };

    return lanesHeld->lanes;
}

bool PianoRoll::isDraggingInNotesOrLane() const
{
    return grid->isDragging() || velocityLane->isDragging() || controlLane->isDragging();
}

void PianoRoll::updateLaneButton()
{
    // Its name, for the eye and a screen reader.
    const auto name = laneShown.has_value() ? laneShown->getName() : juce::String ("Velocity");
    laneButton->setButtonText (name);
    laneButton->setTitle ("Lane: " + name);

    // What the clips hold that the lane shown doesn't show, which plays: the chooser marks it, and its tooltip names it. Not
    // at each step of a drag, which changes the song at each, but once it is over (the drags' ends ask again).
    if (isDraggingInNotesOrLane())
        return;

    std::vector<juce::String> unseen;

    for (const auto& lane : getLanesHeld())
        if (laneShown != lane)
            unseen.push_back (lane.getName());

    laneButton->setTooltip (unseen.empty() ? name : name + "\nThese clips also hold " + joinNames (unseen));

    if (const auto marked = ! unseen.empty(); marked != laneButton->marked)
    {
        laneButton->marked = marked;
        laneButton->repaint();
    }
}

void PianoRoll::showLaneMenu()
{
    getLaneMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (laneButton.get()));
}

juce::PopupMenu PianoRoll::getLaneMenu()
{
    juce::PopupMenu menu;
    const juce::Component::SafePointer<PianoRoll> safeThis (this);

    const auto choose = [safeThis] (std::optional<ControlLane> lane)
    {
        return [safeThis, lane]
        {
            if (safeThis != nullptr)
                safeThis->showLane (lane);
        };
    };

    const auto addLane = [this, &menu, &choose] (const ControlLane& lane)
    { menu.addItem (lane.getName(), true, laneShown == lane, choose (lane)); };

    // The velocities; what the clips shown hold, first; then the usual ones they don't.
    menu.addItem ("Velocity", true, ! laneShown.has_value(), choose (std::nullopt));
    const auto held = getLanesHeld();

    if (! held.empty())
    {
        menu.addSectionHeader ("In these clips");

        for (const auto& lane : held)
            addLane (lane);
    }

    menu.addSeparator();

    for (const auto& lane : getUsualControlLanes())
        if (std::find (held.begin(), held.end(), lane) == held.end())
            addLane (lane);

    // Any controller, on the channel shown; and the same kind on another channel.
    const auto channel = laneShown.has_value() ? laneShown->channel : 1;
    juce::PopupMenu others;

    for (int number = 0; number < 120; ++number)
    {
        const auto lane = makeControlLane (ControlKind::controller, number, channel);
        others.addItem (lane.getName(), true, laneShown == lane, choose (lane));
    }

    menu.addSeparator();
    menu.addSubMenu ("Other Controller", others);
    juce::PopupMenu channels;

    for (int each = 1; each <= 16; ++each)
    {
        auto lane = laneShown;

        if (lane.has_value())
            lane->channel = each;

        channels.addItem ("Channel " + juce::String (each), laneShown.has_value(), channel == each && laneShown.has_value(), choose (lane));
    }

    menu.addSubMenu ("Channel", channels, laneShown.has_value());

    // All the lane's values that the clips shown play, as one step.
    if (laneShown.has_value())
    {
        const auto lane = *laneShown;
        menu.addSeparator();
        menu.addItem ("Delete All " + lane.getShortName() + " Values", std::find (held.begin(), held.end(), lane) != held.end(), false,
                      [safeThis, lane]
                      {
                          if (safeThis != nullptr)
                              deleteAllControlValues (safeThis->context, safeThis->clips, lane);
                      });
    }

    return menu;
}

//==============================================================================
PianoRoll::ControlLaneView::ControlLaneView (PianoRoll& r)
    : roll (r)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Controller lane");
    setDescription ("A controller's values, each holding until the next. Drag to draw them, right-drag to draw a straight ramp, and "
                    "drag across them with the Delete tool to delete them. The chooser beside the lane's chevron picks what it shows.");
    setWantsKeyboardFocus (true); // a press here gives the piano roll the keys: Escape takes the drag back
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

const PianoRoll::ControlLaneView::Drawn& PianoRoll::ControlLaneView::getDrawn()
{
    const auto lane = roll.laneShown.value_or (ControlLane {});
    const auto version = roll.context.getSongVersion();

    if (! drawn.has_value() || drawn->songVersion != version || drawn->shown != roll.clips || drawn->lane != lane)
    {
        Drawn fresh;
        fresh.songVersion = version;
        fresh.shown = roll.clips;
        fresh.lane = lane;

        if (roll.hasProject() && roll.laneShown.has_value())
        {
            for (const auto& clip : roll.getShownClips())
            {
                const auto values = getControlValues (roll.context, { clip.id }, lane);
                Drawn::Clip shown { clip.id, clip.range, clip.colour, fresh.points.size(), fresh.points.size() + values.size() };
                fresh.points.insert (fresh.points.end(), values.begin(), values.end());
                fresh.clips.push_back (shown);
            }
        }

        drawn = std::move (fresh);
    }

    return *drawn;
}

int PianoRoll::ControlLaneView::valueAt (float y) const
{
    // Some 240 of the bend's values to a pixel: at half the lane's heights no row's own value is its middle, which is where it
    // rests, and a bend drawn there would never come back to none. The row the rest is within is the rest.
    const auto lane = roll.laneShown.value_or (ControlLane {});

    if ((int) std::floor (y) == getRestRow())
        return lane.getRest();

    const auto highest = lane.getHighestValue();
    const auto usable = std::max (1.0f, (float) getHeight() - topRow - footRows);
    return juce::jlimit (0, highest, juce::roundToInt ((float) highest * (1.0f - (y - topRow) / usable)));
}

int PianoRoll::ControlLaneView::getRestRow() const
{
    return juce::roundToInt (yOf (roll.laneShown.value_or (ControlLane {}).getRest()));
}

float PianoRoll::ControlLaneView::yOf (int value) const
{
    const auto highest = roll.laneShown.value_or (ControlLane {}).getHighestValue();
    const auto usable = std::max (1.0f, (float) getHeight() - topRow - footRows);
    return topRow + usable * (1.0f - (float) value / (float) highest);
}

ClipID PianoRoll::ControlLaneView::getClipAt (BeatPosition beat)
{
    for (const auto& clip : getDrawn().clips)
        if (clip.range.contains (beat))
            return clip.id;

    return {};
}

BeatRange PianoRoll::ControlLaneView::getCell (BeatPosition beat, juce::ModifierKeys mods) const
{
    // With Snap on, a step of the grid from its line; else a pixel's width, but no shorter than the gap between two values.
    beat = std::max (BeatPosition(), beat);
    const auto shortest = getShortestControlGap().inBeats();

    if (roll.getGrid().snap && ! mods.isAltDown())
    {
        const auto start = roll.snapDown (beat, mods);
        return { start, start + BeatDuration::fromBeats (std::max (shortest, roll.getGridStepInBeats())) };
    }

    const auto length = std::max (shortest, 1.0 / roll.getView().pixelsPerBeat);
    const auto start = std::floor (beat.inBeats() / length) * length;
    return { BeatPosition::fromBeats (start), BeatPosition::fromBeats (start + length) };
}

std::int64_t PianoRoll::ControlLaneView::getCellNumber (BeatPosition beat) const
{
    // Counted from the draw's first cell, each as long as it was: a grid that changes as the view zooms in the middle of the
    // draw doesn't change its cells. (A hair over, so that a cell's own start, worked out again, is in it.)
    const auto along = (std::max (BeatPosition(), beat) - cellOrigin).inBeats() / std::max (1.0e-9, cellLength.inBeats());
    return (std::int64_t) std::floor (along + 1.0e-9);
}

BeatPosition PianoRoll::ControlLaneView::getCellStart (std::int64_t number) const
{
    return cellOrigin + cellLength * (double) number;
}

std::vector<std::pair<ClipID, BeatRange>> PianoRoll::ControlLaneView::getClipParts (BeatRange range)
{
    // Each clip's part of the range, but where a clip shown before it plays too, which takes the values there. The clips as
    // the song has them now, not as last drawn: what is drawn is read again only to paint.
    std::vector<std::pair<ClipID, BeatRange>> parts;
    const auto shownClips = roll.getShownClips();

    for (size_t i = 0; i < shownClips.size(); ++i)
    {
        std::vector<BeatRange> pieces { range.getIntersectionWith (shownClips[i].range) };

        for (size_t before = 0; before < i; ++before)
        {
            const auto earlier = shownClips[before].range;
            std::vector<BeatRange> rest;

            for (const auto& piece : pieces)
            {
                if (! piece.overlaps (earlier))
                {
                    rest.push_back (piece);
                    continue;
                }

                if (piece.getStart() < earlier.getStart())
                    rest.push_back ({ piece.getStart(), earlier.getStart() });

                if (earlier.getEnd() < piece.getEnd())
                    rest.push_back ({ earlier.getEnd(), piece.getEnd() });
            }

            pieces = std::move (rest);
        }

        for (const auto& piece : pieces)
            if (! piece.isEmpty())
                parts.emplace_back (shownClips[i].id, piece);
    }

    return parts;
}

//==============================================================================
void PianoRoll::ControlLaneView::mouseMove (const juce::MouseEvent& e)
{
    setDescription (describeAt (e));
}

juce::String PianoRoll::ControlLaneView::describeAt (const juce::MouseEvent& e)
{
    if (! roll.hasProject() || ! roll.laneShown.has_value() || roll.getClips().empty())
        return {};

    const auto& lane = *roll.laneShown;
    const auto beat = roll.getView().xToBeat (e.position.x);

    if (roll.getTool() == Tool::erase)
        return "Drag across the " + lane.getShortName() + " values to delete them";

    if (! lane.canDraw())
        return "A note's pressure can't be drawn yet: drag across it with the Delete tool to delete it";

    if (! getClipAt (beat).isValid())
        return "No clip plays here: values are drawn only where a clip plays";

    return lane.describeValue (valueAt (e.position.y)) + " at " + roll.describePlace (getCell (beat, e.mods).getStart())
           + ": drag to draw, right-drag for a straight ramp";
}

void PianoRoll::ControlLaneView::mouseDown (const juce::MouseEvent& e)
{
    if (! roll.hasProject() || ! roll.laneShown.has_value() || roll.getClips().empty() || e.mods.isMiddleButtonDown())
        return;

    dragged = *roll.laneShown;
    valuesAtPress = getDrawn().points;
    drawnCells.clear();
    passed.reset();
    lastPosition = e.position;

    // The Delete tool deletes what it passes over.
    if (roll.getTool() == Tool::erase)
    {
        drag = Drag::erase;
        step = std::make_unique<SongGesture> (roll.context, "Delete " + dragged.getShortName());
        eraseTo (e);
        return;
    }

    if (! dragged.canDraw())
    {
        drag = Drag::nothing;
        roll.setDragReadout ("A note's pressure can't be drawn yet");
        return;
    }

    step = std::make_unique<SongGesture> (roll.context, "Draw " + dragged.getShortName());

    // The right button draws a straight ramp, as in the velocity lane and FL Studio's event editor: shown as it goes, put in
    // place as the button comes up.
    if (e.mods.isPopupMenu())
    {
        drag = Drag::ramp;
        rampFrom = rampTo = roll.snap (roll.getView().xToBeat (e.position.x), e.mods);
        rampFromValue = rampToValue = valueAt (e.position.y);
        roll.setDragReadout (dragged.describeValue (rampToValue) + " at " + roll.describePlace (rampTo));
        repaint();
        return;
    }

    drag = Drag::draw;
    const auto first = getCell (roll.getView().xToBeat (e.position.x), e.mods);
    cellOrigin = first.getStart();
    cellLength = first.getLength();
    drawTo (e);
}

void PianoRoll::ControlLaneView::mouseDrag (const juce::MouseEvent& e)
{
    switch (drag)
    {
        case Drag::draw:
            drawTo (e);
            break;
        case Drag::erase:
            eraseTo (e);
            break;
        case Drag::ramp:
            rampTo = roll.snap (roll.getView().xToBeat (e.position.x), e.mods);
            rampToValue = valueAt (e.position.y);
            roll.setDragReadout (dragged.describeValue (rampToValue) + " at " + roll.describePlace (rampTo));
            repaint();
            break;
        case Drag::nothing:
        case Drag::none:
        default:
            break;
    }
}

void PianoRoll::ControlLaneView::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::ramp)
        putRamp();

    // A drag that leaves the values as they were (a click on the value there, a draw out and back) is no step: what it did
    // on the way is taken back, as a note's drag that went nowhere is.
    if (step != nullptr && drag != Drag::none && roll.hasProject())
    {
        const auto& now = getDrawn().points;
        const auto same = [] (const ControlPoint& a, const ControlPoint& b)
        { return a.clip == b.clip && a.beat == b.beat && a.value == b.value && a.number == b.number; };

        if (std::equal (now.begin(), now.end(), valuesAtPress.begin(), valuesAtPress.end(), same))
            step->rollBack();
    }

    endDrag(); // the drag's undo step is done
    repaint();
}

void PianoRoll::ControlLaneView::cancelDrag()
{
    if (drag == Drag::none)
        return;

    if (step != nullptr)
        step->rollBack();

    endDrag();
    repaint();
}

void PianoRoll::ControlLaneView::endDrag()
{
    if (drag != Drag::none)
        roll.setDragReadout ({});

    const auto wasDragging = drag != Drag::none;
    step = nullptr;
    drag = Drag::none;
    valuesAtPress.clear();
    drawnCells.clear();
    songVersionDrawn = -1;
    passed.reset();

    if (wasDragging)
        roll.updateLaneButton(); // which waited for the drag's end
}

void PianoRoll::ControlLaneView::drawTo (const juce::MouseEvent& e)
{
    // Each cell between where the pointer was and where it is now, with the value under the pointer as it passed its start
    // (the pointer's own in the cell it is in at a press).
    const auto& timeline = roll.getView();
    const auto from = lastPosition, to = e.position;
    lastPosition = to;
    const auto left = std::min (from.x, to.x), right = std::max (from.x, to.x);
    const auto first = getCellNumber (timeline.xToBeat (left)), last = getCellNumber (timeline.xToBeat (right));
    std::optional<std::int64_t> firstChanged, lastChanged;

    for (auto number = first; number <= last && number - first < mostCellsAnEvent; ++number)
    {
        const auto x = juce::jlimit (left, right, (float) timeline.beatToX (getCellStart (number)));
        const auto along = std::abs (to.x - from.x) < 1.0f ? 1.0f : (x - from.x) / (to.x - from.x);
        const auto value = valueAt (from.y + (to.y - from.y) * along);
        const auto [cell, isNew] = drawnCells.try_emplace (number, value);

        if (! isNew && cell->second == value)
            continue;

        cell->second = value;
        firstChanged = firstChanged.value_or (number);
        lastChanged = number;
    }

    roll.setDragReadout (dragged.describeValue (valueAt (to.y)) + " at "
                         + roll.describePlace (getCellStart (getCellNumber (timeline.xToBeat (to.x)))));

    // The pointer still, or moved within a cell at the same value: nothing changes.
    if (! firstChanged.has_value() || ! lastChanged.has_value())
        return;

    // The cells whose values changed, and the start of the next cell drawn after them: only that one can start or stop being a
    // repeat of the value before it, and so be put or left out. The cells after it are as they were.
    const auto next = drawnCells.upper_bound (*lastChanged);
    const BeatRange rewritten { getCellStart (*firstChanged),
                                next != drawnCells.end() ? getCellStart (next->first) + tinyBeats * 2.0 : getCellStart (*lastChanged + 1) };
    // Where every change since the piano roll last heard of one is this draw's, the columns it repaints are all that changed.
    const auto versionBefore = roll.context.getSongVersion();
    putCells (rewritten);
    songVersionDrawn = versionBefore == songVersionHeard || versionBefore == songVersionDrawn ? roll.context.getSongVersion() : -1;
    repaintDrawn (rewritten);
}

void PianoRoll::ControlLaneView::putCells (BeatRange range)
{
    for (const auto& [clip, part] : getClipParts (range))
    {
        // The cells in the part, the first from the part's start where it began before it (a clip's start between lines).
        std::vector<ControlPoint> values;

        for (auto it = drawnCells.lower_bound (getCellNumber (part.getStart() + tinyBeats));
             it != drawnCells.end() && getCellStart (it->first) < part.getEnd() - tinyBeats; ++it)
            values.push_back ({ clip, std::max (part.getStart(), getCellStart (it->first)), it->second, 0 });

        setControlValues (roll.context, clip, dragged, { part.getStart(), part.getEnd() - tinyBeats }, values);
    }
}

void PianoRoll::ControlLaneView::repaintDrawn (BeatRange range)
{
    // What changed is drawn from the first cell's column; up to where the value after them starts, which the last holds
    // until (and a value's dot shows by how far apart it is from its neighbours, so a dot's width more); and the dot of the
    // value before them. Found in what was drawn last, which is as the song is outside what this step changed.
    const auto& timeline = roll.getView();
    const auto margin = juce::roundToInt (ControlLaneTint::dot) + ControlLaneTint::dotSpacing;
    std::optional<int> before, after;

    if (drawn.has_value() && drawn->lane == dragged)
    {
        for (const auto& clip : drawn->clips)
        {
            const auto begin = drawn->points.begin() + (std::ptrdiff_t) clip.first, end = drawn->points.begin() + (std::ptrdiff_t) clip.end;
            const auto byBeat = [] (const ControlPoint& point, BeatPosition beat) { return point.beat < beat; };
            const auto start = std::lower_bound (begin, end, range.getStart(), byBeat);
            const auto beyond = std::lower_bound (start, end, range.getEnd(), byBeat);

            if (start != begin)
                before = std::max (before.value_or (std::numeric_limits<int>::min()), timeline.beatToColumn (std::prev (start)->beat));

            if (beyond != end)
                after = std::min (after.value_or (std::numeric_limits<int>::max()), timeline.beatToColumn (beyond->beat));
            else if (clip.range.getEnd() >= range.getEnd())
                after = std::min (after.value_or (std::numeric_limits<int>::max()), timeline.beatToColumn (clip.range.getEnd()));
        }
    }

    const auto left = timeline.beatToColumn (range.getStart()) - margin;
    const auto right = std::max (timeline.beatToColumn (range.getEnd()), after.value_or (getWidth())) + margin;
    repaint (juce::Rectangle<int>::leftTopRightBottom (left, 0, right, getHeight()));

    if (before.has_value())
        repaint (*before - margin, 0, margin * 2, getHeight());
}

void PianoRoll::ControlLaneView::eraseTo (const juce::MouseEvent& e)
{
    const auto& timeline = roll.getView();
    const auto from = lastPosition, to = e.position;
    lastPosition = to;
    const BeatRange range { getCell (timeline.xToBeat (std::min (from.x, to.x)), e.mods).getStart(),
                            getCell (timeline.xToBeat (std::max (from.x, to.x)), e.mods).getEnd() };
    passed = passed.has_value() ? passed->getUnionWith (range) : range;
    deleteControlValues (roll.context, roll.getClips(), dragged, { range.getStart(), range.getEnd() - tinyBeats });
    roll.setDragReadout ("Delete " + dragged.getShortName() + " from " + roll.describePlace (passed->getStart()) + " to "
                         + roll.describePlace (passed->getEnd()));
    repaint();
}

void PianoRoll::ControlLaneView::putRamp()
{
    // Between its ends, both included, in place of what was there.
    const auto points = makeControlRamp (dragged, rampFrom, rampFromValue, rampTo, rampToValue);
    const auto range = BeatRange::between (rampFrom, rampTo);

    for (const auto& [clip, part] : getClipParts ({ range.getStart(), range.getEnd() + tinyBeats * 2.0 }))
    {
        std::vector<ControlPoint> values;

        for (const auto& point : points)
            if (part.contains (point.beat))
                values.push_back ({ clip, point.beat, point.value, 0 });

        setControlValues (roll.context, clip, dragged, { part.getStart(), part.getEnd() - tinyBeats }, values);
    }
}

//==============================================================================
void PianoRoll::ControlLaneView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (! roll.hasProject() || ! roll.laneShown.has_value())
        return;

    const auto area = g.getClipBounds();
    const auto& timeline = roll.getView();
    const auto& shown = getDrawn();

    // Where the clips are, as the note grid shows them: a wash in the clip's colour, a line at each end. Nothing is drawn
    // outside them.
    for (const auto& clip : shown.clips)
    {
        const auto left = timeline.beatToColumn (clip.range.getStart()), right = timeline.beatToColumn (clip.range.getEnd());
        g.setColour (clip.colour.withAlpha (ControlLaneTint::clipWash));
        g.fillRect (left, area.getY(), right - left, area.getHeight());
        g.setColour (clip.colour.withAlpha (ControlLaneTint::clipEdge));
        g.fillRect (left, area.getY(), 1, area.getHeight());
        g.fillRect (right - 1, area.getY(), 1, area.getHeight());
    }

    paintGridLines (g, roll, area);

    // The bend's middle, where it rests, on the row a draw gives the rest on; lines at a quarter, half and three quarters of
    // the others' range. Each on a whole pixel row, sharp.
    if (shown.lane.kind == ControlKind::pitchWheel)
    {
        g.setColour (p.border);
        g.fillRect (0, getRestRow(), getWidth(), 1);
    }
    else
    {
        g.setColour (p.border.withMultipliedAlpha (0.5f));

        for (const auto fraction : { 0.25f, 0.5f, 0.75f })
            g.fillRect (0, juce::roundToInt (topRow + ((float) getHeight() - topRow - footRows) * (1.0f - fraction)), getWidth(), 1);
    }

    for (const auto& clip : shown.clips)
        paintValues (g, shown, clip, area);

    // A ramp being drawn, and what the Delete tool has passed.
    if (drag == Drag::ramp)
    {
        g.setColour (p.accent);
        g.drawLine ({ (float) timeline.beatToX (rampFrom), yOf (rampFromValue), (float) timeline.beatToX (rampTo), yOf (rampToValue) },
                    ControlLaneTint::line);
    }

    if (drag == Drag::erase && passed.has_value())
    {
        const auto left = timeline.beatToColumn (passed->getStart()), right = timeline.beatToColumn (passed->getEnd());
        g.setColour (p.danger.withAlpha (ControlLaneTint::eraseWash));
        g.fillRect (left, 0, right - left, getHeight());
    }
}

void PianoRoll::ControlLaneView::paintValues (juce::Graphics& g, const Drawn& shown, const Drawn::Clip& clip, juce::Rectangle<int> area)
{
    if (clip.first == clip.end)
        return;

    // Only what is in view, found by a search: a clip can hold a hundred thousand values (a long take's bend).
    const auto& timeline = roll.getView();
    const auto begin = shown.points.begin() + (std::ptrdiff_t) clip.first, end = shown.points.begin() + (std::ptrdiff_t) clip.end;
    const auto viewStart = timeline.xToBeat (area.getX()), viewEnd = timeline.xToBeat (area.getRight() + 1);
    auto it = std::lower_bound (begin, end, viewStart, [] (const ControlPoint& point, BeatPosition beat) { return point.beat < beat; });
    const auto& p = getPalette (*this);

    // A note's pressure: a stem for each value, as the velocities show, since the notes' values overlap.
    if (shown.lane.kind == ControlKind::aftertouch)
    {
        g.setColour (clip.colour);

        for (; it != end && it->beat <= viewEnd; ++it)
        {
            const auto x = (float) timeline.beatToColumn (it->beat);
            const auto top = yOf (it->value);
            g.fillRect (x - VelocityTint::stem / 2.0f, top, VelocityTint::stem, yOf (0) - top);
            g.fillEllipse (juce::Rectangle<float> (VelocityTint::ball, VelocityTint::ball).withCentre ({ x, top }));
        }

        return;
    }

    // A program change: a line at its place with its number (counted from 1), whose text reaches into view from before it.
    if (shown.lane.kind == ControlKind::programChange)
    {
        if (it != begin)
            --it;

        g.setFont (juce::FontOptions (TextSize::small));

        for (; it != end && it->beat <= viewEnd; ++it)
        {
            const auto x = timeline.beatToColumn (it->beat);
            g.setColour (clip.colour);
            g.fillRect ((float) x, 0.0f, ControlLaneTint::programLine, (float) getHeight());
            g.setColour (p.text);
            g.drawText (juce::String (it->value + 1), x + Space::xs, Space::xs / 2, getWidth(),
                        juce::roundToInt (TextSize::small) + Space::xs, juce::Justification::topLeft, false);
        }

        return;
    }

    // Steps, at most one for each pixel column (getControlSteps()): a hundred thousand values zoomed out are as many
    // rectangles as the lane is wide, not three for each value.
    const auto base = (float) getRestRow(); // the bend's middle line, or the foot
    const auto line = ControlLaneTint::line;
    const auto steps = getControlSteps ({ shown.points.data() + clip.first, clip.end - clip.first }, timeline,
                                        { area.getX(), area.getRight() }, timeline.beatToColumn (clip.range.getEnd()));

    for (const auto& each : steps)
    {
        const auto left = (float) each.left, width = (float) (each.right - each.left);
        const auto y = yOf (each.value);
        g.setColour (clip.colour.withAlpha (ControlLaneTint::fill));
        g.fillRect (juce::Rectangle<float> (left, std::min (y, base), width, std::abs (base - y)));
        g.setColour (clip.colour);
        g.fillRect (juce::Rectangle<float> (left, y - line / 2.0f, width, line));

        // Up and down its column, from the value before it, through every value in the column.
        if (each.lowest != each.highest)
        {
            const auto top = yOf (each.highest), bottom = yOf (each.lowest);
            g.fillRect (juce::Rectangle<float> (left, top - line / 2.0f, line, bottom - top + line));
        }

        if (each.dot)
            g.fillEllipse (juce::Rectangle<float> (ControlLaneTint::dot, ControlLaneTint::dot).withCentre ({ left, y }));
    }
}

std::vector<PianoRoll::ControlStep> PianoRoll::getControlSteps (std::span<const ControlPoint> values, const TimelineView& timeline,
                                                                juce::Range<int> columns, int clipEndColumn)
{
    std::vector<ControlStep> steps;

    // From the value that holds at the first column, found by a search: a clip can hold a hundred thousand values.
    const auto byBeat = [] (const ControlPoint& point, BeatPosition beat) { return point.beat < beat; };
    auto it = std::lower_bound (values.begin(), values.end(), timeline.xToBeat (columns.getStart()), byBeat);

    if (it != values.begin())
        --it;

    std::optional<int> held; // the value that holds before the step

    if (it != values.begin())
        held = std::prev (it)->value;

    while (it != values.end())
    {
        const auto left = timeline.beatToColumn (it->beat);

        if (left > columns.getEnd())
            break;

        // Every value that starts in the column: the last holds from it, and the others and the one before span it.
        ControlStep step { left, left, it->value, held.value_or (it->value), held.value_or (it->value), false };
        int count = 0;

        for (; it != values.end() && timeline.beatToColumn (it->beat) == left; ++it, ++count)
        {
            step.value = it->value;
            step.lowest = std::min (step.lowest, it->value);
            step.highest = std::max (step.highest, it->value);
        }

        step.right = std::min (clipEndColumn, it != values.end() ? timeline.beatToColumn (it->beat) : clipEndColumn);

        // A dot at its start, where it stands alone, far enough from the values beside it to tell them.
        step.dot = count == 1 && step.right - left >= ControlLaneTint::dotSpacing
                   && (steps.empty() || left - steps.back().left >= ControlLaneTint::dotSpacing);
        held = step.value;
        steps.push_back (step);
    }

    return steps;
}
} // namespace phazite::ui
