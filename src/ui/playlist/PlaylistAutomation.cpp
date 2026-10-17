// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's automation lanes (docs/design.md, section 10): each lane's header, under its track's header, and its
// curve, under its track's clips, which the tools edit through the model (model/automation/AutomationModel.h).
#include "PlaylistParts.h"
#include "model/mixer/MixerModel.h"
#include "ui/components/ValueText.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    constexpr int resizeEdgeHeight = 4; // a lane header's bottom edge, which resizes the lane when dragged
    constexpr int colourStripWidth = 8; // as a track header's
    constexpr int indentPerLevel = 12;  // as a track header's
    constexpr int modeButtonWidth = 52; // "Touch", "Latch"
    constexpr int closeButtonSize = 20;
    constexpr int pixelsPerValue = 2;         // how finely a lane's line is drawn
    constexpr float bendPerLaneHeight = 2.0f; // a drag the lane's height up or down bends a line from straight to a step
    constexpr float fineFactor = 0.2f;        // Shift moves a value a fifth as far

    juce::String getModeName (AutomationMode mode)
    {
        switch (mode)
        {
            case AutomationMode::touch:
                return "Touch";
            case AutomationMode::latch:
                return "Latch";
            case AutomationMode::write:
                return "Write";
            case AutomationMode::read:
            default:
                return "Read";
        }
    }

    juce::String getShapeName (AutomationShape shape)
    {
        switch (shape)
        {
            case AutomationShape::line:
                return "Line";
            case AutomationShape::triangle:
                return "Triangle";
            case AutomationShape::square:
                return "Square";
            case AutomationShape::sawUp:
                return "Saw Up";
            case AutomationShape::sawDown:
                return "Saw Down";
            case AutomationShape::sine:
            default:
                return "Sine";
        }
    }

    // The lanes' names in menus and headers: "Volume", or "Synth: Cutoff" for a device's.
    juce::String describeTarget (const AutomationTarget& target)
    {
        return target.deviceName == "Mixer" ? target.name : target.deviceName + ": " + target.name;
    }
} // namespace

//==============================================================================
Playlist::AutomationLaneView::AutomationLaneView (Playlist& p, TrackID t, const ParameterRef& shown)
    : playlist (p),
      track (t),
      parameter (shown)
{
    setTitle ("Automation lane");
    setOpaque (false); // the playlist's grid shows through
    refreshPoints();
}

void Playlist::AutomationLaneView::update (juce::Colour trackColour)
{
    colour = trackColour;
    handle = getParameter (playlist.context, parameter);

    if (const auto target = getAutomationTarget (playlist.context, parameter))
    {
        kind = target->kind;
        setTitle ("Automation of " + describeTarget (*target));
    }

    refreshPoints();
}

void Playlist::AutomationLaneView::refreshPoints()
{
    points = getAutomationPoints (playlist.context, parameter);
    repaint();
}

//==============================================================================
juce::Rectangle<float> Playlist::AutomationLaneView::getCurveArea() const
{
    // Room for the points' dots at the top and the bottom, above the line under the lane.
    return getLocalBounds().toFloat().withTrimmedBottom (1.0f).reduced (0.0f, LaneTint::pointRadius + 1.0f);
}

float Playlist::AutomationLaneView::valueToY (float value) const
{
    const auto area = getCurveArea();
    const auto valueRange = handle.getRange();
    const auto proportion =
        valueRange.end > valueRange.start ? valueRange.convertTo0to1 (juce::jlimit (valueRange.start, valueRange.end, value)) : 0.5f;
    return area.getBottom() - proportion * area.getHeight();
}

float Playlist::AutomationLaneView::yToValue (float y) const
{
    const auto area = getCurveArea();
    const auto proportion = juce::jlimit (0.0f, 1.0f, (area.getBottom() - y) / std::max (1.0f, area.getHeight()));
    const auto valueRange = handle.getRange();
    return valueRange.end > valueRange.start ? valueRange.snapToLegalValue (valueRange.convertFrom0to1 (proportion)) : valueRange.start;
}

float Playlist::AutomationLaneView::beatToX (BeatPosition beat) const
{
    return (float) playlist.getView().beatToX (beat); // the lane starts where the lanes do
}

BeatPosition Playlist::AutomationLaneView::xToBeat (float x) const
{
    return playlist.getView().xToBeat (x);
}

std::optional<int> Playlist::AutomationLaneView::getPointAt (juce::Point<float> position) const
{
    // The nearest within reach.
    std::optional<int> nearest;
    auto best = LaneTint::grabDistance;

    for (int i = 0; i < (int) points.size(); ++i)
    {
        const auto& point = points[(size_t) i];

        if (const auto distance = position.getDistanceFrom ({ beatToX (point.beat), valueToY (point.value) }); distance <= best)
        {
            best = distance;
            nearest = i;
        }
    }

    return nearest;
}

std::optional<int> Playlist::AutomationLaneView::getLineAt (juce::Point<float> position) const
{
    for (int i = 0; i + 1 < (int) points.size(); ++i)
    {
        if (position.x <= beatToX (points[(size_t) i].beat) || position.x >= beatToX (points[(size_t) i + 1].beat))
            continue;

        // The line as the engine plays it, where the pointer is.
        const auto beat = xToBeat (position.x);
        const auto values = getAutomationValues (playlist.context, parameter, { beat, beat }, 1);

        if (! values.empty() && std::abs (valueToY (values.front()) - position.y) <= LaneTint::grabDistance)
            return i;

        return {};
    }

    return {};
}

juce::String Playlist::AutomationLaneView::getValueText (float value) const
{
    switch (kind)
    {
        case ParameterKind::volume:
            return formatDecibels (faderPositionToDecibels (value));
        case ParameterKind::pan:
            return formatPan (value);
        case ParameterKind::other:
        default:
            if (const auto info = handle.getInfo())
                return formatParameterValue (*info, value);

            return handle.getText (value);
    }
}

juce::String Playlist::AutomationLaneView::describeHere (juce::Point<float> position) const
{
    if (const auto point = getPointAt (position))
        return getValueText (points[(size_t) *point].value)
               + ". Drag the point to move it (Shift for fine steps, Alt to place it off the grid), double-click to delete it.";

    switch (playlist.getTool())
    {
        case Tool::smart:
            if (getLineAt (position).has_value())
                return "Drag the line to move it up or down, Alt+drag to bend it, and double-click to straighten it.";

            return "Click to add a point, and drag to select the points in a rectangle. Right-click for the lane's menu.";
        case Tool::draw:
            return "Drag to draw steps on the grid, or a free line with Alt. Drag a point to move it.";
        case Tool::paint:
            return "Drag to draw a " + getShapeName (playlist.paintShape).toLowerCase()
                   + " from where you start to where you let go, as high and low as you drag. The lane's menu chooses the shape.";
        case Tool::erase:
            return "Click or drag over points to delete them.";
        case Tool::select:
            return "Drag to select the points in a rectangle; Ctrl adds them to those selected.";
        case Tool::zoom:
            return describeTool (Tool::zoom);
        case Tool::slice:
        case Tool::slip:
        case Tool::mute:
        default:
            return describeTool (playlist.getTool()) + " It doesn't work on automation lanes: use the Smart Tool, Draw or Paint.";
    }
}

//==============================================================================
void Playlist::AutomationLaneView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    auto area = getLocalBounds();
    g.setColour (p.surface.withAlpha (LaneTint::background));
    g.fillRect (area);
    g.setColour (p.border);
    g.fillRect (area.removeFromBottom (1));

    const auto clip = g.getClipBounds();
    const auto curveArea = getCurveArea();

    if (points.empty())
    {
        // Without automation, where the parameter is now, dashed.
        const auto y = valueToY (handle.getValue());
        const float dashes[] = { 4.0f, 4.0f };
        g.setColour (colour.withAlpha (LaneTint::idleLine));
        g.drawDashedLine ({ (float) clip.getX(), y, (float) clip.getRight(), y }, dashes, 2, 1.0f);
    }
    else
    {
        // The line as the engine plays it, across what shows, with a wash under it.
        const auto left = clip.getX() - pixelsPerValue, right = clip.getRight() + pixelsPerValue;
        const auto numValues = std::max (2, (right - left) / pixelsPerValue + 1);
        const auto values =
            getAutomationValues (playlist.context, parameter, { xToBeat ((float) left), xToBeat ((float) right) }, numValues);
        juce::Path line;

        for (int i = 0; i < (int) values.size(); ++i)
        {
            const auto x = (float) left + (float) (right - left) * (float) i / (float) (numValues - 1);
            const juce::Point<float> at (x, valueToY (values[(size_t) i]));

            if (i == 0)
                line.startNewSubPath (at);
            else
                line.lineTo (at);
        }

        auto wash = line;
        wash.lineTo ((float) right, curveArea.getBottom());
        wash.lineTo ((float) left, curveArea.getBottom());
        wash.closeSubPath();
        g.setColour (colour.withAlpha (LaneTint::wash));
        g.fillPath (wash);
        g.setColour (colour);
        g.strokePath (line, juce::PathStrokeType (GraphTint::curveWidth));

        // The points: selected ones in the accent color.
        const auto selected = playlist.selection.getSelectedAutomationPoints();
        const auto isSelected = [&selected, this] (int index)
        {
            return selected.parameter == parameter
                   && std::find (selected.indices.begin(), selected.indices.end(), index) != selected.indices.end();
        };

        for (int i = 0; i < (int) points.size(); ++i)
        {
            const juce::Point<float> centre (beatToX (points[(size_t) i].beat), valueToY (points[(size_t) i].value));
            const auto dot = juce::Rectangle<float> (LaneTint::pointRadius * 2.0f, LaneTint::pointRadius * 2.0f).withCentre (centre);

            if (! dot.intersects (clip.toFloat()))
                continue;

            g.setColour (isSelected (i) ? p.accent : colour);
            g.fillEllipse (dot);
            g.setColour (p.background);
            g.drawEllipse (dot, 1.0f);
        }
    }

    if (! rubberBand.isEmpty())
    {
        g.setColour (p.accent.withAlpha (0.15f));
        g.fillRect (rubberBand);
        g.setColour (p.accent);
        g.drawRect (rubberBand, 1.0f);
    }
}

//==============================================================================
void Playlist::AutomationLaneView::mouseMove (const juce::MouseEvent& e)
{
    const auto onPoint = getPointAt (e.position).has_value();
    const auto onLine = ! onPoint && playlist.getTool() == Tool::smart && getLineAt (e.position).has_value();
    setMouseCursor (onPoint                             ? juce::MouseCursor::DraggingHandCursor
                    : onLine                            ? juce::MouseCursor::UpDownResizeCursor
                    : playlist.getTool() == Tool::smart ? juce::MouseCursor::NormalCursor
                                                        : juce::MouseCursor::CrosshairCursor);
    setDescription (describeHere (e.position));
    setTooltip (onPoint ? getValueText (points[(size_t) *getPointAt (e.position)].value) : juce::String());
}

void Playlist::AutomationLaneView::mouseDown (const juce::MouseEvent& e)
{
    drag = pendingDrag = Drag::none;
    gesture = {};
    gesture.startPosition = e.getPosition();
    gesture.startBeat = xToBeat (e.position.x);
    start = e.position;
    refreshPoints();
    before = points;
    moving.clear();
    pressed = -1;
    clickSelectsAlone = false;
    steps.clear();
    stroke.clear();
    drawn.reset();
    rubberBand = {};

    if (! playlist.context.hasProject())
        return;

    if (const auto selected = playlist.selection.getSelectedAutomationPoints(); selected.parameter == parameter)
        selectedBefore = selected.indices;
    else
        selectedBefore.clear();

    const auto point = getPointAt (e.position);

    if (e.mods.isPopupMenu())
    {
        // In FL Studio's keymap, right-click deletes, as it deletes clips: this point and those the mouse moves on to.
        if (playlist.getCommands().getKeymap().rightClickDeletes && ! e.mods.isShiftDown() && point.has_value())
        {
            gesture.beginStep (playlist.context, "Delete Automation Points");
            drag = Drag::erase;
            eraseAt (e.position);
        }
        else
        {
            playlist.showLaneMenu (track, parameter);
        }

        return;
    }

    switch (playlist.getTool())
    {
        case Tool::erase:
            gesture.beginStep (playlist.context, "Delete Automation Points");
            drag = Drag::erase;
            eraseAt (e.position);
            return;
        case Tool::select:
            drag = Drag::select;
            return;
        case Tool::paint:
            gesture.beginStep (playlist.context, "Draw Automation");
            drag = Drag::paint;
            paintTo (e);
            return;
        case Tool::zoom:
            drag = Drag::forward;
            playlist.lanesMouseDown (e.getEventRelativeTo (playlist.lanes.get()));
            return;
        case Tool::slice:
        case Tool::slip:
        case Tool::mute:
            return;
        case Tool::smart:
        case Tool::draw:
        default:
            break;
    }

    if (point.has_value())
    {
        // A point: selected (Ctrl or Shift adds it, or takes it out), and moved with the others selected.
        auto& songSelection = playlist.selection;
        const auto alreadySelected = songSelection.isSelected (parameter, *point);

        if (e.mods.isCommandDown() || e.mods.isShiftDown())
        {
            auto indices = songSelection.getSelectedAutomationPoints().indices;

            if (songSelection.getSelectedAutomationPoints().parameter != parameter)
                indices.clear();

            if (alreadySelected)
                indices.erase (std::remove (indices.begin(), indices.end(), *point), indices.end());
            else
                indices.push_back (*point);

            songSelection.selectAutomationPoints (parameter, indices);
        }
        else if (! alreadySelected)
        {
            songSelection.selectAutomationPoints (parameter, { *point });
        }
        else
        {
            clickSelectsAlone = true;
        }

        pressed = *point;
        pendingDrag = Drag::movePoints;
        drag = Drag::pending;
        return;
    }

    if (playlist.getTool() == Tool::smart)
    {
        if (const auto line = getLineAt (e.position))
        {
            pressed = *line;
            pendingDrag = e.mods.isAltDown() ? Drag::bend : Drag::moveLine;
            drag = Drag::pending;
            return;
        }

        pendingDrag = Drag::select; // a click adds a point instead
        drag = Drag::pending;
        return;
    }

    // Draw: steps on the grid, or a free line with Alt, from here.
    gesture.beginStep (playlist.context, "Draw Automation");
    drag = Drag::draw;
    drawTo (e);
}

void Playlist::AutomationLaneView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::pending)
    {
        if (! TimelineGesture::isPastDragThreshold (e))
            return;

        beginDrag (e);
    }

    switch (drag)
    {
        case Drag::movePoints:
            dragPoints (e);
            break;
        case Drag::moveLine:
            dragLine (e);
            break;
        case Drag::bend:
            bendLine (e);
            break;
        case Drag::draw:
            drawTo (e);
            break;
        case Drag::paint:
            paintTo (e);
            break;
        case Drag::select:
            rubberBand = juce::Rectangle<float> (start, e.position);
            selectIn (rubberBand, e.mods.isCommandDown());
            repaint();
            break;
        case Drag::erase:
            eraseAt (e.position);
            break;
        case Drag::forward:
            playlist.lanesMouseDrag (e.getEventRelativeTo (playlist.lanes.get()));
            break;
        case Drag::none:
        case Drag::pending:
        default:
            break;
    }
}

void Playlist::AutomationLaneView::beginDrag (const juce::MouseEvent&)
{
    drag = pendingDrag;

    switch (drag)
    {
        case Drag::movePoints:
        {
            // The pressed point with the others selected in this lane.
            const auto selected = playlist.selection.getSelectedAutomationPoints();
            moving = selected.parameter == parameter ? selected.indices : std::vector<int>();

            if (std::find (moving.begin(), moving.end(), pressed) == moving.end())
                moving = { pressed };

            clickSelectsAlone = false;
            gesture.beginStep (playlist.context, "Move Automation Points");
            break;
        }
        case Drag::moveLine:
            moving = { pressed, pressed + 1 };
            playlist.selection.selectAutomationPoints (parameter, moving);
            gesture.beginStep (playlist.context, "Move Automation Points");
            break;
        case Drag::bend:
            gesture.beginStep (playlist.context, "Bend Automation");
            break;
        case Drag::select:
        case Drag::none:
        case Drag::pending:
        case Drag::draw:
        case Drag::paint:
        case Drag::erase:
        case Drag::forward:
        default:
            break;
    }
}

void Playlist::AutomationLaneView::dragPoints (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (pressed, (int) before.size()))
        return;

    // The pressed point goes where the mouse takes it, on the grid unless Alt is held; the others move with it.
    const auto& from = before[(size_t) pressed];
    const auto target = playlist.snap (from.beat + (xToBeat (e.position.x) - xToBeat (start.x)), e.mods);
    const auto dy = (e.position.y - start.y) * (e.mods.isShiftDown() ? fineFactor : 1.0f);
    const auto value = yToValue (valueToY (from.value) + dy);

    refreshPoints();

    if (juce::isPositiveAndBelow (pressed, (int) points.size()))
    {
        const auto& now = points[(size_t) pressed];
        moveAutomationPoints (playlist.context, parameter, moving, target - now.beat, value - now.value);
        refreshPoints();
    }
}

void Playlist::AutomationLaneView::dragLine (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (pressed + 1, (int) before.size()))
        return;

    // Both ends up or down together, by as much as the mouse moves.
    const auto dy = (e.position.y - start.y) * (e.mods.isShiftDown() ? fineFactor : 1.0f);
    const auto value = yToValue (valueToY (before[(size_t) pressed].value) + dy);
    refreshPoints();

    if (juce::isPositiveAndBelow (pressed, (int) points.size()))
    {
        moveAutomationPoints (playlist.context, parameter, moving, {}, value - points[(size_t) pressed].value);
        refreshPoints();
    }
}

void Playlist::AutomationLaneView::bendLine (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (pressed + 1, (int) before.size()))
        return;

    // Dragging up bows the line up, whichever way it goes (Tracktion's curve bends a rising line up below 0, a falling
    // one above).
    const auto& first = before[(size_t) pressed];
    const auto rising = before[(size_t) pressed + 1].value > first.value;
    const auto up = (start.y - e.position.y) / std::max (1.0f, getCurveArea().getHeight());
    setAutomationCurve (playlist.context, parameter, pressed, first.curve + (rising ? -up : up) * bendPerLaneHeight);
    refreshPoints();
}

void Playlist::AutomationLaneView::drawTo (const juce::MouseEvent& e)
{
    const auto beat = std::max (BeatPosition(), xToBeat (e.position.x));
    const auto value = yToValue (e.position.y);
    std::vector<AutomationPoint> drawnPoints;

    if (playlist.getGrid().snap && ! e.mods.isAltDown())
    {
        // A step for each grid cell the mouse passes, at the value it had there: a point at the cell's start, and one at
        // its end, where the next cell's value jumps in.
        const auto cell = playlist.snapDown (beat, e.mods);
        steps[cell.inBeats()] = value;
        const auto step = playlist.getGridStepInBeats();

        // Cells the mouse went past too fast keep the value of the cell before them.
        for (auto i = steps.begin(); i != steps.end() && std::next (i) != steps.end(); ++i)
            for (auto gap = i->first + step; gap < std::next (i)->first - step / 2; gap += step)
                i = steps.emplace (gap, i->second).first;

        for (const auto& [cellStart, cellValue] : steps)
        {
            drawnPoints.push_back ({ BeatPosition::fromBeats (cellStart), cellValue });
            drawnPoints.push_back ({ BeatPosition::fromBeats (cellStart + step), cellValue });
        }
    }
    else
    {
        // A free line: a point wherever the mouse has been, a few pixels apart, by beat.
        const auto minimumGap = BeatDuration::fromBeats (3.0 / playlist.getView().pixelsPerBeat);
        stroke.erase (std::remove_if (stroke.begin(), stroke.end(), [beat, minimumGap] (const AutomationPoint& p)
                                      { return std::abs ((p.beat - beat).inBeats()) < minimumGap.inBeats(); }),
                      stroke.end());
        stroke.push_back ({ beat, value });
        std::sort (stroke.begin(), stroke.end(), [] (const auto& a, const auto& b) { return a.beat < b.beat; });
        drawnPoints = stroke;
    }

    if (drawnPoints.empty())
        return;

    const BeatRange drawnRange { drawnPoints.front().beat, drawnPoints.back().beat }; // before the points move into the call
    replaceDrawn (drawnRange, std::move (drawnPoints));
}

void Playlist::AutomationLaneView::paintTo (const juce::MouseEvent& e)
{
    // From where the drag began to the mouse, on the grid unless Alt is held, a cycle a grid step long.
    const auto from = playlist.snap (std::max (BeatPosition(), xToBeat (start.x)), e.mods);
    const auto to = playlist.snap (std::max (BeatPosition(), xToBeat (e.position.x)), e.mods);
    const auto drawnRange = BeatRange::between (from, to);
    const auto cycle = BeatDuration::fromBeats (playlist.getGridStepInBeats());
    auto shape = drawnRange.isEmpty()
                     ? std::vector<AutomationPoint> { { from, yToValue (start.y) } }
                     : makeAutomationShape (playlist.paintShape, drawnRange, cycle, yToValue (start.y), yToValue (e.position.y));
    replaceDrawn (drawnRange, std::move (shape));
}

void Playlist::AutomationLaneView::replaceDrawn (BeatRange drawnRange, std::vector<AutomationPoint> newPoints)
{
    // What a drag drew before, outside what it draws now, goes back to the points that were there.
    const auto replaced =
        drawn.has_value() ? BeatRange (std::min (drawn->getStart(), drawnRange.getStart()), std::max (drawn->getEnd(), drawnRange.getEnd()))
                          : drawnRange;

    for (const auto& point : before)
        if (point.beat >= replaced.getStart() && point.beat <= replaced.getEnd()
            && (point.beat < drawnRange.getStart() || point.beat > drawnRange.getEnd()))
            newPoints.push_back (point);

    std::stable_sort (newPoints.begin(), newPoints.end(), [] (const auto& a, const auto& b) { return a.beat < b.beat; });
    replaceAutomationPoints (playlist.context, parameter, replaced, newPoints);
    drawn = replaced;
    refreshPoints();
}

void Playlist::AutomationLaneView::selectIn (juce::Rectangle<float> area, bool addToSelection)
{
    // With Ctrl, added to what was selected when the drag began.
    auto inside = addToSelection ? selectedBefore : std::vector<int>();

    for (int i = 0; i < (int) points.size(); ++i)
        if (area.contains (beatToX (points[(size_t) i].beat), valueToY (points[(size_t) i].value)))
            inside.push_back (i);

    playlist.selection.selectAutomationPoints (parameter, inside);
}

void Playlist::AutomationLaneView::eraseAt (juce::Point<float> position)
{
    refreshPoints();

    if (const auto point = getPointAt (position))
    {
        deleteAutomationPoints (playlist.context, parameter, { *point });
        refreshPoints();
    }
}

void Playlist::AutomationLaneView::mouseUp (const juce::MouseEvent& e)
{
    if (drag == Drag::forward)
    {
        playlist.lanesMouseUp (e.getEventRelativeTo (playlist.lanes.get()));
    }
    else if (drag == Drag::pending)
    {
        if (pendingDrag == Drag::select)
        {
            // A click on empty space adds a point there, on the grid unless Alt is held, and selects it.
            gesture.beginStep (playlist.context, "Add Automation Point");
            const auto beat = playlist.snap (std::max (BeatPosition(), xToBeat (e.position.x)), e.mods);
            const auto index = addAutomationPoint (playlist.context, parameter, { beat, yToValue (e.position.y) });

            if (index >= 0)
                playlist.selection.selectAutomationPoints (parameter, { index });
        }
        else if (clickSelectsAlone && pressed >= 0)
        {
            playlist.selection.selectAutomationPoints (parameter, { pressed });
        }
    }

    gesture.endStep();
    drag = pendingDrag = Drag::none;
    rubberBand = {};
    refreshPoints();
}

void Playlist::AutomationLaneView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (playlist.getTool() != Tool::smart && playlist.getTool() != Tool::draw)
        return;

    // The press that came with it does nothing more when the mouse goes up.
    drag = pendingDrag = Drag::none;
    clickSelectsAlone = false;
    gesture.endStep();

    // A point goes; a bent line straightens.
    refreshPoints();

    if (const auto point = getPointAt (e.position))
    {
        deleteAutomationPoints (playlist.context, parameter, { *point });
        playlist.selection.selectAutomationPoints (parameter, {});
    }
    else if (const auto line = getLineAt (e.position))
    {
        setAutomationCurve (playlist.context, parameter, *line, 0.0f);
    }

    refreshPoints();
}

//==============================================================================
Playlist::AutomationLaneHeader::AutomationLaneHeader (Playlist& p, TrackID t, const ParameterRef& shown)
    : playlist (p),
      track (t),
      parameter (shown)
{
    setDescription ("An automation lane. Click its name to show another of the track's parameters, and the mode to choose how "
                    "moving the track's controls while the song plays writes their automation. Drag the bottom edge to resize "
                    "the lane, and right-click for its menu.");

    nameButton.setTooltip ("Shows another of the track's parameters in this lane");
    nameButton.onClick = [this] { showParameterMenu(); };
    modeButton.setTooltip ("How moving the track's controls while the song plays writes their automation: Read plays it, "
                           "Touch writes while a control is held, Latch from the first move until playback stops, and Write "
                           "overwrites from when playback starts");
    modeButton.onClick = [this] { showModeMenu(); };
    closeButton.setQuiet (true);
    closeButton.setTooltip ("Hides the lane; its automation stays");
    closeButton.onClick = [this] { hideAutomationLane (playlist.context, parameter); };

    for (auto* c : std::initializer_list<juce::Component*> { &nameButton, &modeButton, &closeButton })
    {
        c->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (c);
    }
}

void Playlist::AutomationLaneHeader::update (const TrackRow& shown)
{
    row = shown;
    const auto target = getAutomationTarget (playlist.context, parameter);
    const auto name = target.has_value() ? describeTarget (*target) : juce::String ("Automation");
    nameButton.setButtonText (name);
    setTitle ("Automation lane: " + name);

    const auto mode = getAutomationMode (playlist.context, track);
    modeButton.setButtonText (getModeName (mode));
    modeButton.setTitle ("Automation mode: " + getModeName (mode));

    // A mode that writes stands out, as an armed track does.
    const auto& p = getPalette (*this);
    modeButton.setColour (juce::TextButton::textColourOffId, mode == AutomationMode::read ? p.textMuted : p.danger);
    resized();
    repaint();
}

void Playlist::AutomationLaneHeader::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    auto area = getLocalBounds();
    g.setColour (p.surface.interpolatedWith (row.colour, LaneTint::header));
    g.fillRect (area);
    g.setColour (row.colour);
    g.fillRect (area.removeFromLeft (colourStripWidth).withTrimmedBottom (1));
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));
}

void Playlist::AutomationLaneHeader::resized()
{
    auto area = getLocalBounds()
                    .withTrimmedLeft (colourStripWidth + Space::xs + row.depth * indentPerLevel + indentPerLevel)
                    .withTrimmedRight (Space::xs)
                    .withTrimmedBottom (resizeEdgeHeight);
    auto top = area.removeFromTop (std::min (area.getHeight(), controlHeight));

    // A short lane has room for its name only.
    const auto roomForButtons = top.getHeight() >= closeButtonSize;
    modeButton.setVisible (roomForButtons);
    closeButton.setVisible (roomForButtons);

    if (roomForButtons)
    {
        closeButton.setBounds (top.removeFromRight (closeButtonSize).withSizeKeepingCentre (closeButtonSize, closeButtonSize));
        top.removeFromRight (Space::xs);
        modeButton.setBounds (top.removeFromRight (modeButtonWidth).reduced (0, 2));
        top.removeFromRight (Space::xs);
    }

    nameButton.setBounds (top.reduced (0, 2));
}

bool Playlist::AutomationLaneHeader::isOnResizeEdge (juce::Point<int> position) const
{
    return position.y >= getHeight() - resizeEdgeHeight;
}

void Playlist::AutomationLaneHeader::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isOnResizeEdge (e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void Playlist::AutomationLaneHeader::mouseDown (const juce::MouseEvent& e)
{
    resizing = false;

    if (e.mods.isPopupMenu())
    {
        playlist.showLaneMenu (track, parameter);
        return;
    }

    if (isOnResizeEdge (e.getPosition()))
    {
        resizing = true;
        heightAtDragStart = getHeight();
    }
}

void Playlist::AutomationLaneHeader::mouseDrag (const juce::MouseEvent& e)
{
    if (! resizing)
        return;

    // View state, without undo steps; shown at once.
    setAutomationLaneHeight (playlist.context, parameter, heightAtDragStart + e.getDistanceFromDragStartY());
    playlist.refreshLanes();
}

void Playlist::AutomationLaneHeader::mouseUp (const juce::MouseEvent&)
{
    resizing = false;
}

void Playlist::AutomationLaneHeader::showParameterMenu()
{
    // The track's parameters, a submenu for each of its devices, the one shown ticked. Choosing one shows it here.
    juce::PopupMenu menu;
    juce::PopupMenu devicePart;
    juce::String device;
    auto& projectContext = playlist.context;
    const auto shown = parameter;

    const auto addDevice = [&menu, &devicePart, &device]
    {
        if (devicePart.getNumItems() > 0)
            menu.addSubMenu (device, devicePart);

        devicePart = {};
    };

    for (const auto& target : getAutomationTargets (projectContext, track))
    {
        if (target.deviceName != device)
        {
            addDevice();
            device = target.deviceName;
        }

        const auto chosen = target.parameter;
        devicePart.addItem (
            juce::PopupMenu::Item (target.name)
                .setTicked (chosen == shown)
                .setAction ([&projectContext, shown, chosen] { setAutomationLaneParameter (projectContext, shown, chosen); }));
    }

    addDevice();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nameButton));
}

void Playlist::AutomationLaneHeader::showModeMenu()
{
    juce::PopupMenu menu;
    auto& projectContext = playlist.context;
    const auto id = track;
    const auto current = getAutomationMode (projectContext, id);

    for (const auto mode : { AutomationMode::read, AutomationMode::touch, AutomationMode::latch, AutomationMode::write })
        menu.addItem (juce::PopupMenu::Item (getModeName (mode))
                          .setTicked (mode == current)
                          .setAction ([&projectContext, id, mode] { setAutomationMode (projectContext, id, mode); }));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&modeButton));
}

//==============================================================================
void Playlist::showLaneMenu (TrackID track, const ParameterRef& parameter)
{
    auto& projectContext = context;
    const juce::Component::SafePointer<Playlist> safeThis (this);
    juce::PopupMenu menu;

    menu.addItem ("Clear Automation", ! getAutomationPoints (projectContext, parameter).empty(), false,
                  [&projectContext, parameter] { clearAutomation (projectContext, parameter); });
    menu.addItem ("Hide Lane", [&projectContext, parameter] { hideAutomationLane (projectContext, parameter); });
    menu.addSeparator();

    // The shape Paint draws in every lane.
    juce::PopupMenu shapes;

    for (const auto shape : { AutomationShape::sine, AutomationShape::triangle, AutomationShape::square, AutomationShape::sawUp,
                              AutomationShape::sawDown, AutomationShape::line })
        shapes.addItem (juce::PopupMenu::Item (getShapeName (shape))
                            .setTicked (shape == paintShape)
                            .setAction (
                                [safeThis, shape]
                                {
                                    if (safeThis != nullptr)
                                        safeThis->paintShape = shape;
                                }));

    menu.addSubMenu ("Shape for Paint", shapes);

    juce::PopupMenu modes;
    const auto current = getAutomationMode (projectContext, track);

    for (const auto mode : { AutomationMode::read, AutomationMode::touch, AutomationMode::latch, AutomationMode::write })
        modes.addItem (juce::PopupMenu::Item (getModeName (mode))
                           .setTicked (mode == current)
                           .setAction ([&projectContext, track, mode] { setAutomationMode (projectContext, track, mode); }));

    menu.addSubMenu ("Mode", modes);
    menu.addSeparator();
    addCommandItems (menu, getCommands(), { "edit.delete", "edit.duplicate" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

//==============================================================================
void Playlist::refreshLanes()
{
    for (auto& row : rows)
    {
        row.lanes.clear();

        for (const auto& lane : getAutomationLanes (context, row.track.id))
            row.lanes.push_back ({ lane });
    }

    rebuildLanes();
    layOutRows();
    updateLanes();
    noteLanes();
}

void Playlist::noteLanes()
{
    // A lane shown since the last time scrolls into view (as Automate shows one from a control in the mixer); the
    // lanes of a project just opened don't.
    std::vector<ParameterRef> now;
    std::optional<size_t> rowOfNewLane;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        for (const auto& lane : rows[i].lanes)
        {
            now.push_back (lane.lane.parameter);

            if (knownLanes.has_value() && ! rowOfNewLane.has_value()
                && std::find (knownLanes->begin(), knownLanes->end(), lane.lane.parameter) == knownLanes->end())
                rowOfNewLane = i;
        }
    }

    knownLanes = std::move (now);

    if (rowOfNewLane.has_value())
        scrollToShow ((int) *rowOfNewLane);
}

void Playlist::rebuildLanes()
{
    // The components of lanes that still show are kept; the others go.
    juce::OwnedArray<AutomationLaneHeader> keptHeaders;
    juce::OwnedArray<AutomationLaneView> keptViews;

    for (const auto& row : rows)
    {
        for (const auto& lane : row.lanes)
        {
            AutomationLaneHeader* header = nullptr;
            AutomationLaneView* laneView = nullptr;

            for (int i = 0; i < laneHeaders.size(); ++i)
            {
                if (laneHeaders[i]->track == row.track.id && laneHeaders[i]->parameter == lane.lane.parameter)
                {
                    header = laneHeaders.removeAndReturn (i);
                    laneView = laneViews.removeAndReturn (i);
                    break;
                }
            }

            if (header == nullptr)
            {
                header = new AutomationLaneHeader (*this, row.track.id, lane.lane.parameter);
                laneView = new AutomationLaneView (*this, row.track.id, lane.lane.parameter);
                headerContent.addAndMakeVisible (header); // layOutLanes() moves them where they go
                scrollingClips.addAndMakeVisible (laneView);
            }

            keptHeaders.add (header);
            keptViews.add (laneView);
        }
    }

    laneHeaders.swapWith (keptHeaders); // the kept arrays hold the lanes that went now, and delete them
    laneViews.swapWith (keptViews);
}

void Playlist::layOutLanes()
{
    for (int i = 0; i < laneHeaders.size(); ++i)
    {
        auto* header = laneHeaders[i];
        auto* laneView = laneViews[i];
        const auto rowIndex = getRowOf (header->track);
        const LaneRow* lane = nullptr;

        if (rowIndex >= 0)
            for (const auto& each : rows[(size_t) rowIndex].lanes)
                if (each.lane.parameter == header->parameter)
                    lane = &each;

        const auto shown = lane != nullptr && lane->height > 0;
        header->setVisible (shown);
        laneView->setVisible (shown);

        if (! shown)
            continue;

        // In the pinned band with a pinned track, or among the scrolling rows.
        const auto& row = rows[(size_t) rowIndex];
        auto& headerParent = row.track.pinned ? pinnedHeaders : headerContent;
        auto& layer = row.track.pinned ? pinnedClips : scrollingClips;

        if (header->getParentComponent() != &headerParent)
            headerParent.addAndMakeVisible (header);

        if (laneView->getParentComponent() != &layer)
            layer.addAndMakeVisible (laneView);

        header->setBounds (0, row.top + lane->top, headerWidth, lane->height);
        laneView->setBounds (juce::Rectangle<int> (0, getRowTop (row) + lane->top, lanes->getWidth(), lane->height) - layer.getPosition());
    }
}

void Playlist::updateLanes()
{
    for (int i = 0; i < laneHeaders.size(); ++i)
    {
        if (const auto rowIndex = getRowOf (laneHeaders[i]->track); rowIndex >= 0)
        {
            laneHeaders[i]->update (rows[(size_t) rowIndex].track);
            laneViews[i]->update (rows[(size_t) rowIndex].track.colour);
        }
    }
}
} // namespace phazite::ui
