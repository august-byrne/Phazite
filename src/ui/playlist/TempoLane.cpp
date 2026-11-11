// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TempoLane.h"
#include "model/arrangement/TimelineModel.h"
#include "model/project/ProjectContext.h"
#include "ui/components/ValueText.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phazite::ui
{
namespace
{
    constexpr int pixelsPerValue = 2;         // how finely the line is drawn
    constexpr float bendPerLaneHeight = 1.0f; // a drag half the lane's height bends a ramp from straight to the most it bends
    constexpr float maxBend = 0.5f;           // bent further, a ramp would hold at an end (TempoChange::curve)
    constexpr float fineFactor = 0.2f;        // Shift moves a tempo a fifth as far
    constexpr double minimumGap = 1.0 / 64.0; // in beats, between a dragged tempo and those beside it

    // A ramp narrower than this has no bend handle: in its middle, the handle would be within reach of its points.
    constexpr float minimumBendWidth = 3.0f * LaneTint::grabDistance;

    const juce::String laneHint ("The song's tempo along it: click to add a tempo change, drag a point to move it, and "
                                 "right-click a line to make it ramp or hold. Remove Tempo Changes takes the lane away.");

    bool isRamp (float curve)
    {
        return std::abs (curve) < 1.0f;
    }

    bool isSameBeat (BeatPosition a, BeatPosition b)
    {
        return std::abs ((a - b).inBeats()) < 1.0e-6;
    }

    // The tempos a lane shows: the song's, with room above and below.
    juce::Range<double> fitTempos (const std::vector<TempoChange>& tempos)
    {
        auto low = tempos.front().bpm, high = low;

        for (const auto& tempo : tempos)
        {
            low = std::min (low, tempo.bpm);
            high = std::max (high, tempo.bpm);
        }

        const auto room = std::max (TempoTint::minimumRoom, (high - low) * TempoTint::room);
        return { std::max (minTempo, low - room), std::min (maxTempo, high + room) };
    }

    // A tempo's number alone, as the lane writes it beside its point: "111", "98.5".
    juce::String formatTempoNumber (double bpm)
    {
        return formatTempo (bpm).upToFirstOccurrenceOf (" ", false, false);
    }
} // namespace

TempoLane::TempoLane (TimelineEditor& e)
    : editor (e),
      context (e.getContext())
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Tempo");
    setDescription (laneHint);
    refresh();
}

void TempoLane::showProject()
{
    drag = pendingDrag = Drag::none;
    gesture = {};
    pressed = selected = hovered = -1;
    refresh();
}

void TempoLane::tempoChanged()
{
    refresh();
}

void TempoLane::refresh()
{
    tempos = getTempoChanges (context);

    if (tempos.empty())
        tempos.push_back ({});

    if (drag == Drag::none)
        shown = fitTempos (tempos);

    if (selected >= (int) tempos.size())
        selected = -1;

    repaint();
}

//==============================================================================
juce::Rectangle<float> TempoLane::getLineArea() const
{
    // Room for the points' dots at the top and the bottom, above the line under the lane.
    return getLocalBounds().toFloat().withTrimmedBottom (1.0f).reduced (0.0f, LaneTint::selectedPointRadius + 1.0f);
}

float TempoLane::tempoToY (double bpm) const
{
    const auto area = getLineArea();
    const auto proportion = shown.getLength() > 0.0 ? (bpm - shown.getStart()) / shown.getLength() : 0.5;
    return area.getBottom() - (float) proportion * area.getHeight();
}

double TempoLane::yToTempo (float y) const
{
    const auto area = getLineArea();
    const auto proportion = area.getHeight() > 0.0f ? (area.getBottom() - y) / area.getHeight() : 0.5f;
    return juce::jlimit (minTempo, maxTempo, shown.getStart() + (double) proportion * shown.getLength());
}

float TempoLane::beatToX (BeatPosition beat) const
{
    return (float) editor.getView().beatToX (beat);
}

BeatPosition TempoLane::xToBeat (float x) const
{
    return editor.getView().xToBeat ((double) x);
}

juce::Point<float> TempoLane::getPointPosition (int index) const
{
    // On the line, in its beat's column (TimelineView::getLineSamples()).
    const auto& tempo = tempos[(size_t) juce::jlimit (0, (int) tempos.size() - 1, index)];
    return { TimelineView::getColumnMiddle (editor.getView().beatToColumn (tempo.beat)), tempoToY (tempo.bpm) };
}

std::optional<int> TempoLane::getPointAt (juce::Point<float> position) const
{
    // The nearest within reach.
    std::optional<int> found;
    auto nearest = LaneTint::grabDistance;

    for (int i = 0; i < (int) tempos.size(); ++i)
    {
        if (const auto distance = getPointPosition (i).getDistanceFrom (position); distance <= nearest)
        {
            found = i;
            nearest = distance;
        }
    }

    return found;
}

int TempoLane::getLineUnder (float x) const
{
    for (int i = 0; i + 1 < (int) tempos.size(); ++i)
        if (x > beatToX (tempos[(size_t) i].beat) && x < beatToX (tempos[(size_t) i + 1].beat))
            return i;

    return -1;
}

std::optional<juce::Point<float>> TempoLane::getHandle (int line) const
{
    // In the middle of a ramp, on it as the engine plays it. A step, or a ramp between two tempos alike, doesn't bend.
    if (line < 0 || line + 1 >= (int) tempos.size() || ! isRamp (tempos[(size_t) line].curve))
        return {};

    const auto& first = tempos[(size_t) line];
    const auto& second = tempos[(size_t) line + 1];
    const auto left = beatToX (first.beat), right = beatToX (second.beat);

    if (right - left < minimumBendWidth || std::abs (tempoToY (first.bpm) - tempoToY (second.bpm)) < 1.0f)
        return {};

    const auto x = (left + right) / 2.0f;
    return juce::Point<float> (x, tempoToY (getTempoAt (context, xToBeat (x))));
}

std::optional<int> TempoLane::getHandleAt (juce::Point<float> position) const
{
    const auto line = getLineUnder (position.x);

    if (const auto centre = getHandle (line); centre.has_value() && centre->getDistanceFrom (position) <= LaneTint::grabDistance)
        return line;

    return {};
}

bool TempoLane::isOnLine (juce::Point<float> position) const
{
    return context.hasProject() && std::abs (tempoToY (getTempoAt (context, xToBeat (position.x))) - position.y) <= LaneTint::grabDistance;
}

juce::String TempoLane::describeHere (juce::Point<float> position) const
{
    if (const auto point = getPointAt (position))
    {
        const auto tempo = formatTempo (tempos[(size_t) *point].bpm);

        if (*point == 0)
            return tempo + ", the song's first tempo. Drag it up or down to change it.";

        return tempo
               + ". Drag the point to move it, and double-click it to delete it. Hold Alt to place it freely, and Shift "
                 "for fine steps.";
    }

    if (getHandleAt (position).has_value())
        return "Drag the handle up or down to bend the ramp, and double-click it to straighten the ramp.";

    if (isOnLine (position))
        return "Click to add a tempo change on the line, or drag to pull one from it. Right-click to make the line ramp or hold.";

    return laneHint;
}

//==============================================================================
void TempoLane::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));

    if (! context.hasProject())
        return;

    // The line as the engine plays it, across what shows, with a wash under it: worked out along the song's own pixels and
    // drawn in the columns that move with the song (TimelineView::getLineSamples()), so that it keeps its shape as the view
    // scrolls, its jumps straight up their beats' columns.
    const auto clip = g.getClipBounds();
    std::vector<BeatPosition> pointBeats;
    pointBeats.reserve (tempos.size());

    for (const auto& tempo : tempos)
        pointBeats.push_back (tempo.beat);

    const auto samples = editor.getView().getLineSamples (clip.getX(), clip.getRight(), pixelsPerValue, pointBeats);
    std::vector<BeatPosition> beats;
    beats.reserve (samples.size());

    for (const auto& sample : samples)
        beats.push_back (sample.beat);

    const auto values = getTempoValues (context, beats);
    juce::Path line;

    for (size_t i = 0; i < values.size() && i < samples.size(); ++i)
    {
        const juce::Point<float> at (TimelineView::getColumnMiddle (samples[i].column), tempoToY (values[i]));

        if (i == 0)
            line.startNewSubPath (at);
        else
            line.lineTo (at);
    }

    const auto washBottom = (float) getHeight() - 1.0f;
    const auto left = line.getBounds().getX(), right = line.getBounds().getRight();
    auto wash = line;
    wash.lineTo (right, washBottom);
    wash.lineTo (left, washBottom);
    wash.closeSubPath();
    g.setColour (p.accent.withAlpha (TempoTint::wash));
    g.fillPath (wash);
    g.setColour (p.accent);
    g.strokePath (line, juce::PathStrokeType (GraphTint::curveWidth));

    // The points, each with its tempo right of it, above it unless the lane's top is too near, while there is room before
    // the next.
    const juce::Font font { juce::FontOptions (TextSize::small) };
    g.setFont (font);

    for (int i = 0; i < (int) tempos.size(); ++i)
    {
        const auto centre = getPointPosition (i);
        const auto chosen = i == selected;
        const auto radius = chosen ? LaneTint::selectedPointRadius : LaneTint::pointRadius;
        const auto text = formatTempoNumber (tempos[(size_t) i].bpm);
        const auto textWidth = juce::GlyphArrangement::getStringWidth (font, text) + (float) Space::xs;
        const auto next = i + 1 < (int) tempos.size() ? beatToX (tempos[(size_t) i + 1].beat) : std::numeric_limits<float>::max();

        if (next - centre.x > textWidth + radius * 2.0f)
        {
            const auto textHeight = TextSize::small + 2.0f;
            const auto above = centre.y - radius - textHeight >= 0.0f;
            const juce::Rectangle<float> box (centre.x + radius, above ? centre.y - radius - textHeight : centre.y + 1.0f, textWidth,
                                              textHeight);

            if (box.intersects (clip.toFloat()))
            {
                g.setColour (p.textMuted);
                g.drawText (text, box, juce::Justification::centredLeft, false);
            }
        }

        const auto dot = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

        if (! dot.intersects (clip.toFloat()))
            continue;

        g.setColour (chosen ? p.text : p.accent);
        g.fillEllipse (dot);
        g.setColour (chosen ? p.accent : p.background);
        g.drawEllipse (dot, chosen ? LaneTint::selectedRing : 1.0f);
    }

    // The bend handle of the ramp under the mouse: hollow, and filled while it bends the ramp.
    const auto bending = drag == Drag::bend || (drag == Drag::pending && pendingDrag == Drag::bend);

    if (bending || drag == Drag::none)
    {
        if (const auto centre = getHandle (bending ? pressed : hovered))
        {
            const auto ring = juce::Rectangle<float> (LaneTint::handleRadius * 2.0f, LaneTint::handleRadius * 2.0f).withCentre (*centre);
            g.setColour (bending ? p.accent : p.background);
            g.fillEllipse (ring);
            g.setColour (p.accent);
            g.drawEllipse (ring, GraphTint::curveWidth);
        }
    }
}

//==============================================================================
void TempoLane::mouseMove (const juce::MouseEvent& e)
{
    const auto point = getPointAt (e.position);

    if (const auto line = getLineUnder (e.position.x); line != hovered)
    {
        hovered = line;
        repaint();
    }

    const auto onHandle = ! point.has_value() && getHandleAt (e.position).has_value();
    const auto onLine = ! point.has_value() && ! onHandle && isOnLine (e.position);
    setMouseCursor (point.has_value() || onLine ? juce::MouseCursor::DraggingHandCursor
                    : onHandle                  ? juce::MouseCursor::UpDownResizeCursor
                                                : juce::MouseCursor::NormalCursor);
    setDescription (describeHere (e.position));
    setTooltip (point.has_value() ? formatTempo (tempos[(size_t) *point].bpm) : juce::String());
}

void TempoLane::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    setDescription (laneHint);
    repaint();
}

void TempoLane::mouseDown (const juce::MouseEvent& e)
{
    drag = pendingDrag = Drag::none;
    gesture = {};
    gesture.startPosition = e.getPosition();
    gesture.startBeat = xToBeat (e.position.x);
    start = e.position;
    pressed = -1;

    if (! context.hasProject())
        return;

    refresh();
    before = tempos;

    if (e.mods.isPopupMenu())
    {
        getMenuAt (e.position).showMenuAsync (juce::PopupMenu::Options());
        return;
    }

    // A point drags; the handle in a ramp's middle bends it; a press on the line puts a tempo change there; anywhere else, a
    // click adds one.
    if (const auto point = getPointAt (e.position))
    {
        pressPoint (*point);
    }
    else if (const auto line = getHandleAt (e.position))
    {
        pressed = *line;
        pendingDrag = Drag::bend;
        drag = Drag::pending;
        repaint();
    }
    else if (isOnLine (e.position))
    {
        pullPointFrom (e);
    }
    else
    {
        pendingDrag = Drag::add;
        drag = Drag::pending;
    }
}

void TempoLane::pressPoint (int index)
{
    selected = pressed = index;
    pendingDrag = Drag::move;
    drag = Drag::pending;
    repaint();
}

void TempoLane::pullPointFrom (const juce::MouseEvent& e)
{
    // A tempo change on the grid unless Alt is held, at the line's tempo there, so that nothing changes until it moves. With
    // one already there on the grid, the press takes that one instead.
    const auto beat = std::max (BeatPosition(), editor.snap (xToBeat (e.position.x), e.mods));

    for (int i = 0; i < (int) tempos.size(); ++i)
    {
        if (isSameBeat (tempos[(size_t) i].beat, beat))
        {
            pressPoint (i);
            return;
        }
    }

    gesture.beginStep (context, "Add Tempo Change");
    const auto index = addTempoChange (context, beat);
    refresh();
    before = tempos;

    if (index >= 0)
        pressPoint (index);
}

void TempoLane::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::pending)
    {
        if (! TimelineGesture::isPastDragThreshold (e))
            return;

        drag = pendingDrag;

        if (drag == Drag::move)
        {
            gesture.beginStep (context, "Move Tempo Change");         // or, for a tempo change pulled from the line, its step
            beginDragAutoRepeat (TimelineEditor::autoRepeatInterval); // the time line goes on scrolling while the mouse rests past an edge
        }
        else if (drag == Drag::bend)
        {
            gesture.beginStep (context, "Bend Tempo Ramp");
        }
        else
        {
            drag = Drag::none; // a drag from empty space adds nothing
        }
    }

    if (drag == Drag::move)
        movePoint (e);
    else if (drag == Drag::bend)
        bendLine (e);
}

void TempoLane::movePoint (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (pressed, (int) before.size()))
        return;

    editor.autoScroll (e.getPosition(), getLocalBounds(), TimelineEditor::ScrollDirections::sideways);
    const auto& from = before[(size_t) pressed];
    auto beat = BeatPosition();

    // The first stays at the song's start; the others go where the mouse takes them, on the grid unless Alt is held, between
    // the tempos before and after them, which keep their order.
    if (pressed > 0)
    {
        const auto target = editor.snap (from.beat + (xToBeat (e.position.x) - gesture.startBeat), e.mods).inBeats();
        const auto earliest = before[(size_t) pressed - 1].beat.inBeats() + minimumGap;
        const auto latest = pressed + 1 < (int) before.size() ? before[(size_t) pressed + 1].beat.inBeats() - minimumGap
                                                              : std::numeric_limits<double>::max();
        beat = BeatPosition::fromBeats (juce::jlimit (earliest, std::max (earliest, latest), target));
    }

    // Up and down in whole beats per minute, or a fifth as far in tenths with Shift.
    const auto fine = e.mods.isShiftDown();
    const auto dy = (e.position.y - start.y) * (fine ? fineFactor : 1.0f);
    const auto step = fine ? 0.1 : 1.0;
    const auto change = std::round ((yToTempo (tempoToY (from.bpm) + dy) - from.bpm) / step) * step;
    moveTempoChange (context, pressed, beat, juce::jlimit (minTempo, maxTempo, from.bpm + change));
    refresh();
}

void TempoLane::bendLine (const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (pressed + 1, (int) before.size()))
        return;

    // Dragging up bows the ramp up, whichever way it goes, as an automation lane's line bows: a curve below 0 is quick at
    // first, which bows a rising ramp up and a falling one down.
    const auto& first = before[(size_t) pressed];
    const auto rising = before[(size_t) pressed + 1].bpm > first.bpm;
    const auto up = (start.y - e.position.y) / std::max (1.0f, getLineArea().getHeight());
    const auto bend = first.curve + (rising ? -up : up) * bendPerLaneHeight;
    setTempoCurve (context, pressed, juce::jlimit (-maxBend, maxBend, bend));
    refresh();
}

void TempoLane::mouseUp (const juce::MouseEvent& e)
{
    // A click on empty space: a tempo change there, on the grid unless Alt is held, at the whole tempo clicked.
    if (drag == Drag::pending && pendingDrag == Drag::add && context.hasProject())
    {
        gesture.beginStep (context, "Add Tempo Change");
        const auto beat = std::max (BeatPosition(), editor.snap (xToBeat (e.position.x), e.mods));
        selected = addTempoChange (context, beat, std::round (yToTempo (e.position.y)));
    }

    gesture.endStep();
    drag = pendingDrag = Drag::none;
    pressed = -1;
    refresh();
}

void TempoLane::mouseDoubleClick (const juce::MouseEvent& e)
{
    // The press that came with it does nothing more when the mouse goes up.
    drag = pendingDrag = Drag::none;
    gesture.endStep();
    refresh();

    // A tempo change goes (never the first); a bent ramp straightens at its handle.
    if (const auto point = getPointAt (e.position); point.has_value() && *point > 0)
    {
        deleteTempoChange (context, *point);
        selected = -1;
    }
    else if (const auto line = getHandleAt (e.position))
    {
        setTempoCurve (context, *line, tempoRamp);
    }

    refresh();
}

//==============================================================================
juce::PopupMenu TempoLane::getMenuAt (juce::Point<float> position)
{
    juce::PopupMenu menu;

    if (! context.hasProject())
        return menu;

    refresh();
    auto& ctx = context;

    if (const auto point = getPointAt (position))
    {
        const auto index = *point;
        const auto& tempo = tempos[(size_t) index];
        menu.addSectionHeader (formatTempo (tempo.bpm) + " from bar " + juce::String (getBarAt (context, tempo.beat) + 1));

        if (index + 1 < (int) tempos.size())
            addLineMenu (menu, index);

        menu.addItem ("Delete Tempo Change", index > 0, false, [&ctx, index] { deleteTempoChange (ctx, index); });
    }
    else
    {
        if (const auto line = getLineUnder (position.x); line >= 0)
        {
            menu.addSectionHeader (formatTempo (tempos[(size_t) line].bpm) + " to " + formatTempo (tempos[(size_t) line + 1].bpm));
            addLineMenu (menu, line);
            menu.addSeparator();
        }

        const auto beat = std::max (BeatPosition(), editor.snap (xToBeat (position.x), {}));
        menu.addItem ("Add Tempo Change Here", [&ctx, beat] { addTempoChange (ctx, beat); });
    }

    menu.addSeparator();
    addCommandItem (menu, editor.getCommands(), "transport.removeTempoChanges");
    return menu;
}

void TempoLane::addLineMenu (juce::PopupMenu& menu, int line)
{
    // How the tempo goes on to the next: a ramp, which keeps its bend, or a step.
    auto& ctx = context;
    const auto ramps = isRamp (tempos[(size_t) line].curve);
    menu.addItem ("Ramp to the Next Tempo", true, ramps,
                  [&ctx, line, ramps]
                  {
                      if (! ramps)
                          setTempoCurve (ctx, line, tempoRamp);
                  });
    menu.addItem ("Hold Until the Next Tempo", true, ! ramps, [&ctx, line] { setTempoCurve (ctx, line, tempoJump); });
}

//==============================================================================
TempoLaneHeader::TempoLaneHeader (TimelineEditor& e)
    : editor (e),
      context (e.getContext())
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Tempo lane");
    nameLabel.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    addAndMakeVisible (nameLabel);

    moreButton.setTitle ("Tempo lane menu");
    moreButton.setTooltip ("Add Tempo Change, where the playhead is, and Remove Tempo Changes, which takes the lane away.");
    moreButton.onClick = [this] { getMoreMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton)); };
    addAndMakeVisible (moreButton);

    update();
}

void TempoLaneHeader::update()
{
    moreButton.setEnabled (context.hasProject());
}

juce::PopupMenu TempoLaneHeader::getMoreMenu()
{
    juce::PopupMenu menu;
    addCommandItems (menu, editor.getCommands(), { "transport.addTempoChange", "transport.removeTempoChanges" });
    return menu;
}

void TempoLaneHeader::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (getLocalBounds().removeFromBottom (1));
    g.fillRect (getLocalBounds().removeFromRight (1));
}

void TempoLaneHeader::resized()
{
    // Its name and button in a row along its top, as the chord lane's header has them.
    auto area = getLocalBounds().reduced (Space::s, 1).removeFromTop (ChordTint::laneHeight);
    moreButton.setBounds (area.removeFromRight (area.getHeight() + Space::xs));
    area.removeFromRight (Space::xs);
    nameLabel.setBounds (area);
}
} // namespace phazite::ui
