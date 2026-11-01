// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SynthParts.h"
#include "DeviceTint.h"
#include "devices/dsp/Envelopes.h"
#include "devices/dsp/Oscillators.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr float lowestGain = -36.0f, highestGain = 24.0f; // a filter's response, in decibels

    void paintFrame (juce::Graphics& g, const juce::Component& c)
    {
        graph::paintBackground (g, getPalette (c), c.getLocalBounds().toFloat());
    }
} // namespace

//==============================================================================
EnvelopeView::EnvelopeView (const juce::String& title, ParameterHandle a, ParameterHandle d, ParameterHandle s, ParameterHandle r,
                            std::function<float()> newestLevel)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      attack (std::move (a)),
      decay (std::move (d)),
      sustain (std::move (s)),
      release (std::move (r)),
      level (std::move (newestLevel))
{
    setTitle (title);
    setDescription ("The envelope's shape: its attack, decay, sustain and release, with a line at the newest note's level. Drag its "
                    "points: the peak sideways for the attack, the corner after the decay for the decay and the sustain, the end for "
                    "the release; a double-click puts a point back where it started.");
}

void EnvelopeView::setCurves (ParameterHandle a, ParameterHandle d, ParameterHandle r)
{
    attackCurve = std::move (a);
    decayCurve = std::move (d);
    releaseCurve = std::move (r);
    setDescription ("The envelope's shape: its attack, decay, sustain and release, with a line at the newest note's level. Drag its "
                    "points: the peak sideways for the attack, the corner after the decay for the decay and the sustain, the end for "
                    "the release; a double-click puts a point back where it started. Drag the handle in a stage's middle up or down "
                    "to bend it; a double-click on a stage puts its curve back.");
    repaint();
}

void EnvelopeView::tick()
{
    shownLevel = level != nullptr ? level() : 0.0f;

    if (drawn.changed ({ attack.getValue(), decay.getValue(), sustain.getValue(), release.getValue(), getCurve (Point::attack),
                         getCurve (Point::decay), getCurve (Point::release), shownLevel }))
        repaint();
}

EnvelopeView::Layout EnvelopeView::getLayout() const
{
    Layout layout;
    layout.area = getLocalBounds().toFloat().reduced (GraphTint::handleRadius + 2.0f);
    layout.attack = std::max (5.0f, attack.getValue());
    layout.decay = std::max (5.0f, decay.getValue());
    layout.release = std::max (5.0f, release.getValue());

    // While a point is dragged, the scale it had as the drag began.
    const auto keepsScale = dragged.has_value() && ! dragged->bend;
    layout.hold = keepsScale ? dragHold : (layout.attack + layout.decay + layout.release) * 0.4f;
    layout.total = keepsScale ? dragTotal : layout.attack + layout.decay + layout.hold + layout.release;
    return layout;
}

juce::Point<float> EnvelopeView::getPointPosition (Point point) const
{
    const auto layout = getLayout();
    const auto held = juce::jlimit (0.0f, 1.0f, sustain.getValue());

    switch (point)
    {
        case Point::attack:
            return { layout.x (layout.attack), layout.y (1.0f) };
        case Point::decay:
            return { layout.x (layout.attack + layout.decay), layout.y (held) };
        case Point::release:
        default:
            return { layout.x (layout.attack + layout.decay + layout.hold + layout.release), layout.y (0.0f) };
    }
}

float EnvelopeView::getCurve (Point stage) const
{
    const auto& curve = stage == Point::attack ? attackCurve : stage == Point::decay ? decayCurve : releaseCurve;

    if (curve.isValid())
        return curve.getValue();

    return stage == Point::attack ? devices::dsp::Adsr::analogAttackCurve : devices::dsp::Adsr::analogSettleCurve;
}

float EnvelopeView::getStageLevel (Point stage, float fraction) const
{
    const auto held = juce::jlimit (0.0f, 1.0f, sustain.getValue());
    const auto shape = devices::dsp::Adsr::getShape (getCurve (stage), fraction);

    switch (stage)
    {
        case Point::attack:
            return shape;
        case Point::decay:
            return 1.0f + (held - 1.0f) * shape;
        case Point::release:
        default:
            return held * (1.0f - shape);
    }
}

std::optional<juce::Point<float>> EnvelopeView::getBendPosition (Point stage) const
{
    const auto held = juce::jlimit (0.0f, 1.0f, sustain.getValue());
    const auto& curve = stage == Point::attack ? attackCurve : stage == Point::decay ? decayCurve : releaseCurve;

    if (! curve.isValid() || (stage == Point::decay && held >= 1.0f) || (stage == Point::release && held <= 0.0f))
        return {};

    const auto layout = getLayout();
    const auto starts = stage == Point::attack ? 0.0f : stage == Point::decay ? layout.attack : layout.attack + layout.decay + layout.hold;
    const auto lasts = stage == Point::attack ? layout.attack : stage == Point::decay ? layout.decay : layout.release;
    return juce::Point<float> { layout.x (starts + lasts / 2.0f), layout.y (getStageLevel (stage, 0.5f)) };
}

std::optional<EnvelopeView::Target> EnvelopeView::findTarget (juce::Point<float> position) const
{
    std::optional<Target> nearest;
    auto nearestDistance = GraphTint::grabDistance;

    const auto consider = [&] (Target target, juce::Point<float> at)
    {
        if (const auto distance = at.getDistanceFrom (position); distance <= nearestDistance)
        {
            nearest = target;
            nearestDistance = distance;
        }
    };

    for (const auto point : { Point::attack, Point::decay, Point::release })
    {
        consider ({ point, false }, getPointPosition (point));

        if (const auto bend = getBendPosition (point))
            consider ({ point, true }, *bend);
    }

    return nearest;
}

std::optional<EnvelopeView::Point> EnvelopeView::findStage (float x) const
{
    const auto layout = getLayout();

    if (x <= layout.x (layout.attack))
        return Point::attack;

    if (x <= layout.x (layout.attack + layout.decay))
        return Point::decay;

    if (x >= layout.x (layout.attack + layout.decay + layout.hold) && x <= getPointPosition (Point::release).x)
        return Point::release;

    return {}; // the held sustain, or past the end
}

ParameterHandle& EnvelopeView::getTimeOf (Point point)
{
    return point == Point::attack ? attack : point == Point::decay ? decay : release;
}

ParameterHandle& EnvelopeView::getCurveOf (Point point)
{
    return point == Point::attack ? attackCurve : point == Point::decay ? decayCurve : releaseCurve;
}

float EnvelopeView::getUpward (Point stage)
{
    // Below 0 a stage moves fast at first: the rising attack bulges up, the falling stages sag.
    return stage == Point::attack ? -1.0f : 1.0f;
}

void EnvelopeView::setHovered (std::optional<Target> target)
{
    if (target == hovered)
        return;

    hovered = target;
    setMouseCursor (! target.has_value() ? juce::MouseCursor::NormalCursor
                    : target->bend       ? juce::MouseCursor::UpDownResizeCursor
                                         : juce::MouseCursor::DraggingHandCursor);
    repaint();
}

void EnvelopeView::paint (juce::Graphics& g)
{
    paintFrame (g, *this);
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto layout = getLayout();
    const auto a = layout.attack, d = layout.decay, hold = layout.hold;
    const auto x = [&layout] (float time) { return layout.x (time); };
    const auto y = [&layout] (float value) { return layout.y (value); };
    const auto held = juce::jlimit (0.0f, 1.0f, sustain.getValue());

    // Each stage along its curve, a couple of pixels at a time.
    juce::Path shape;
    shape.startNewSubPath (x (0.0f), y (0.0f));

    const auto addStage = [&] (Point stage, float starts, float lasts)
    {
        const auto steps = std::max (2, juce::roundToInt ((x (starts + lasts) - x (starts)) / 2.0f));

        for (int i = 1; i <= steps; ++i)
        {
            const auto fraction = (float) i / (float) steps;
            shape.lineTo (x (starts + lasts * fraction), y (getStageLevel (stage, fraction)));
        }
    };

    addStage (Point::attack, 0.0f, a);
    addStage (Point::decay, a, d);
    shape.lineTo (x (a + d + hold), y (held));
    addStage (Point::release, a + d + hold, layout.release);

    // A point dragged past the picture's end goes out of sight rather than over its frame.
    juce::Graphics::ScopedSaveState clipped (g);
    g.reduceClipRegion (getLocalBounds().reduced (1));

    auto filled = shape;
    filled.lineTo (x (0.0f), y (0.0f));
    filled.closeSubPath();
    g.setColour (tint.withAlpha (GraphTint::partCurve));
    g.fillPath (filled);
    g.setColour (tint);
    g.strokePath (shape, juce::PathStrokeType (GraphTint::curveWidth));

    // The newest note's level.
    if (shownLevel > 0.001f)
    {
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.drawHorizontalLine (juce::roundToInt (y (shownLevel)), layout.area.getX(), layout.area.getRight());
        g.setColour (p.text);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ layout.area.getRight() - 3.0f, y (shownLevel) }));
    }

    // Its points filled and its stages' handles hollow, larger under the pointer and while dragged.
    const auto paintHandle = [&] (Target target, juce::Point<float> at)
    {
        const auto radius = target == hovered || target == dragged ? GraphTint::handleRadius * 1.5f : GraphTint::handleRadius;
        const auto dot = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (at);
        g.setColour (p.background);
        g.fillEllipse (dot.expanded (1.0f));
        g.setColour (tint);

        if (target.bend)
            g.drawEllipse (dot.reduced (GraphTint::handleOutline / 2.0f), GraphTint::handleOutline);
        else
            g.fillEllipse (dot);
    };

    for (const auto point : { Point::attack, Point::decay, Point::release })
    {
        if (const auto bend = getBendPosition (point))
            paintHandle ({ point, true }, *bend);

        paintHandle ({ point, false }, getPointPosition (point));
    }
}

void EnvelopeView::mouseMove (const juce::MouseEvent& e)
{
    setHovered (findTarget (e.position));
}

void EnvelopeView::mouseExit (const juce::MouseEvent&)
{
    setHovered ({});
}

void EnvelopeView::mouseDown (const juce::MouseEvent& e)
{
    const auto target = findTarget (e.position);

    if (! target.has_value() || e.mods.isPopupMenu())
        return;

    // The scale stays as it is while a point moves, so that the point stays under the pointer.
    const auto layout = getLayout();
    dragTotal = layout.total;
    dragHold = layout.hold;
    dragged = target;

    if (target->bend)
    {
        startCurve = getCurve (target->point);
        getCurveOf (target->point).beginGesture();
        return;
    }

    startTime = getTimeOf (target->point).getValue();
    startSustain = sustain.getValue();
    getTimeOf (target->point).beginGesture();

    if (target->point == Point::decay)
        sustain.beginGesture();
}

void EnvelopeView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragged.has_value())
        return;

    const auto layout = getLayout();
    const auto fine = e.mods.isShiftDown() ? GraphTint::fineDragFactor : 1.0f;
    const auto rise = (float) -e.getDistanceFromDragStartY() / std::max (1.0f, layout.area.getHeight()) * fine;

    if (dragged->bend)
    {
        // Up bends it up: twice the picture's height takes a curve across its whole range.
        auto& curve = getCurveOf (dragged->point);
        const auto range = curve.getRange();
        curve.setValue (juce::jlimit (range.start, range.end, startCurve + getUpward (dragged->point) * rise));
        repaint();
        return;
    }

    auto& time = getTimeOf (dragged->point);
    const auto timeRange = time.getRange();
    const auto moved = (float) e.getDistanceFromDragStartX() / std::max (1.0f, layout.area.getWidth()) * dragTotal * fine;
    time.setValue (juce::jlimit (timeRange.start, timeRange.end, startTime + moved));

    if (dragged->point == Point::decay)
        sustain.setValue (juce::jlimit (0.0f, 1.0f, startSustain + rise));

    repaint();
}

void EnvelopeView::mouseUp (const juce::MouseEvent&)
{
    if (! dragged.has_value())
        return;

    if (dragged->bend)
    {
        getCurveOf (dragged->point).endGesture();
    }
    else
    {
        if (dragged->point == Point::decay)
            sustain.endGesture();

        getTimeOf (dragged->point).endGesture();
    }

    dragged.reset();
    repaint(); // at its own scale again
}

void EnvelopeView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto target = findTarget (e.position);

    // On a stage that bends, or its handle: its curve back, one undo step.
    if (! target.has_value() || target->bend)
    {
        const auto stage = target.has_value() ? std::optional<Point> (target->point) : findStage (e.position.x);

        if (stage.has_value() && getBendPosition (*stage).has_value())
            if (const auto initial = getCurveOf (*stage).getDefaultValue())
                getCurveOf (*stage).setValue (*initial);

        repaint();
        return;
    }

    // A point back where it started, as a knob's double-click puts its value back: one undo step.
    std::vector<ParameterHandle*> handles { &getTimeOf (target->point) };

    if (target->point == Point::decay)
        handles.push_back (&sustain);

    for (auto* handle : handles)
        handle->beginGesture();

    for (auto* handle : handles)
        if (const auto initial = handle->getDefaultValue())
            handle->setValue (*initial);

    for (auto* handle : handles)
        handle->endGesture();

    repaint();
}

float getDrawnLfoValue (int shape, float phase)
{
    using devices::dsp::LfoShape;
    const auto lfoShape = (LfoShape) juce::jlimit (0, (int) LfoShape::random, shape);

    if (lfoShape == LfoShape::random)
        return std::sin (37.0f * std::floor (phase * 4.0f)) * 0.8f; // held values, a step at a time

    return devices::dsp::Lfo::getValue (lfoShape, phase);
}

//==============================================================================
LfoView::LfoView (const juce::String& title, std::function<float (float)> shape, std::function<float()> now)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      shapeAt (std::move (shape)),
      phase (std::move (now))
{
    setTitle (title);
    setDescription ("The LFO's wave, one cycle, with a dot where it is now.");
}

void LfoView::setControls (ParameterHandle newDepth, ParameterHandle newRate, ParameterHandle newSync, ParameterHandle newDivision)
{
    depth = std::move (newDepth);
    rate = std::move (newRate);
    sync = std::move (newSync);
    division = std::move (newDivision);
    setDescription (juce::String ("The LFO's wave, one cycle, with a dot where it is now.")
                    + (depth.exists() ? " Drag up or down for its depth; the wheel turns its rate." : " The wheel turns its rate."));
    setMouseCursor (depth.exists() ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

float LfoView::getDrawnDepth() const
{
    return depth.exists() ? juce::jlimit (0.0f, 1.0f, depth.getValue()) : 1.0f;
}

void LfoView::tick()
{
    // Where it is, and its wave across a cycle, which its settings change.
    shownPhase = phase != nullptr ? phase() : -1.0f;
    std::vector<float> numbers { shownPhase, getDrawnDepth() };

    for (int i = 0; i < drawnWavePoints; ++i)
        numbers.push_back (shapeAt ((float) i / (float) drawnWavePoints));

    if (drawn.changed (std::move (numbers)))
        repaint();
}

void LfoView::paint (juce::Graphics& g)
{
    paintFrame (g, *this);
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getLocalBounds().toFloat().reduced (4.0f);
    const auto height = area.getHeight() * 0.45f * getDrawnDepth();
    const auto yFor = [&area, height] (float value) { return area.getCentreY() - juce::jlimit (-1.0f, 1.0f, value) * height; };

    g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
    g.drawHorizontalLine (juce::roundToInt (area.getCentreY()), area.getX(), area.getRight());

    const auto curve = graph::makeCurve (area, [&] (float x) { return yFor (shapeAt ((x - area.getX()) / area.getWidth())); }, 1.0f);
    g.setColour (tint);
    g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

    if (shownPhase >= 0.0f)
    {
        const juce::Point<float> dot { area.getX() + shownPhase * area.getWidth(), yFor (shapeAt (shownPhase)) };
        g.setColour (p.text);
        g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
    }
}

void LfoView::mouseDown (const juce::MouseEvent& e)
{
    wheel.end(); // a step of its own, not the wheel's

    if (! depth.exists() || e.mods.isPopupMenu())
        return;

    startDepth = depth.getValue();
    depth.beginGesture();
}

void LfoView::mouseDrag (const juce::MouseEvent& e)
{
    if (! depth.isInGesture())
        return;

    // Up for more, the whole height for the whole range; Shift in fine steps.
    const auto fine = e.mods.isShiftDown() ? GraphTint::fineDragFactor : 1.0f;
    const auto change = (float) -e.getDistanceFromDragStartY() / std::max (1.0f, (float) getHeight()) * fine;
    depth.setValue (juce::jlimit (0.0f, 1.0f, startDepth + change));
    repaint();
}

void LfoView::mouseUp (const juce::MouseEvent&)
{
    if (depth.isInGesture())
        depth.endGesture();
}

void LfoView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& details)
{
    const auto step = details.deltaY > 0.0f ? 1 : details.deltaY < 0.0f ? -1 : 0;

    if (step == 0)
        return;

    // A note value a notch while synced; otherwise a tenth faster or slower a notch (Shift: a fiftieth).
    if (sync.exists() && sync.getValue() >= 0.5f && division.exists())
    {
        const auto range = division.getRange();
        wheel.turn (division, juce::jlimit (range.start, range.end, division.getValue() + (float) step));
    }
    else if (rate.exists())
    {
        const auto range = rate.getRange();
        const auto factor = std::pow (e.mods.isShiftDown() ? 1.02f : 1.1f, (float) step);
        wheel.turn (rate, juce::jlimit (range.start, range.end, rate.getValue() * factor));
    }
}

//==============================================================================
FilterView::FilterView (const juce::String& title, ParameterHandle c, ParameterHandle r, std::function<std::vector<float>()> read,
                        Response response, std::function<float()> nowPlaying)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      cutoff (std::move (c)),
      resonance (std::move (r)),
      readValues (std::move (read)),
      magnitude (std::move (response)),
      playing (std::move (nowPlaying))
{
    setTitle (title);
    setDescription ("The filter's response, and where the newest note plays it now. Drag the dot sideways for the cutoff and up "
                    "or down for the resonance.");
}

graph::FrequencyAxis FilterView::getFrequencies() const
{
    return { getLocalBounds().toFloat().reduced (2.0f) };
}

juce::Point<float> FilterView::getDotPosition() const
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    return { getFrequencies().xFor (cutoff.getValue()),
             area.getBottom() - juce::jlimit (0.0f, 1.0f, resonance.getValue()) * area.getHeight() };
}

void FilterView::tick()
{
    shownPlaying = playing != nullptr ? playing() : 0.0f;
    auto numbers = readValues();
    numbers.insert (numbers.end(), { cutoff.getValue(), resonance.getValue(), shownPlaying });

    if (drawn.changed (std::move (numbers)))
        repaint();
}

void FilterView::paint (juce::Graphics& g)
{
    paintFrame (g, *this);
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const auto frequencies = getFrequencies();
    const graph::LevelAxis levels { area, lowestGain, highestGain };
    graph::paintFrequencyGrid (g, p, frequencies, false);

    const auto values = readValues();
    const auto curveAt = [&] (double at)
    {
        return graph::makeCurve (area,
                                 [&] (float x)
                                 {
                                     const auto gain = magnitude (values, at, frequencies.frequencyAt (x));
                                     return levels.yFor ((float) juce::Decibels::gainToDecibels (gain, (double) lowestGain - 1.0));
                                 });
    };

    // Where the newest note plays it, faint, then as set.
    if (shownPlaying > 0.0f)
    {
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.strokePath (curveAt (shownPlaying), juce::PathStrokeType (1.0f));
    }

    g.setColour (tint);
    g.strokePath (curveAt (cutoff.getValue()), juce::PathStrokeType (GraphTint::curveWidth));

    const auto dot = getDotPosition();
    g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
    graph::paintReadout (g, p, area, graph::formatFrequency (cutoff.getValue()), juce::Justification::topLeft);
}

void FilterView::mouseDown (const juce::MouseEvent& e)
{
    cutoff.beginGesture();
    resonance.beginGesture();
    dragTo (e.position);
}

void FilterView::mouseDrag (const juce::MouseEvent& e)
{
    dragTo (e.position);
}

void FilterView::mouseUp (const juce::MouseEvent&)
{
    resonance.endGesture();
    cutoff.endGesture();
}

void FilterView::dragTo (juce::Point<float> position)
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const auto range = cutoff.getRange();
    cutoff.setValue (juce::jlimit (range.start, range.end, (float) getFrequencies().frequencyAt (position.x)));
    resonance.setValue (juce::jlimit (0.0f, 1.0f, (area.getBottom() - position.y) / std::max (1.0f, area.getHeight())));
    repaint();
}
} // namespace phazite::ui
