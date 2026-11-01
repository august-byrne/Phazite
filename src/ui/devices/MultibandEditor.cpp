// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MultibandEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/dynamics/Multiband.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Multiband;

    constexpr const char* bandNames[] = { "Low", "Mid", "High", "Master" };
    constexpr float gridDecibels[] = { -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 3.0f, 6.0f, 9.0f };
    constexpr int toolbarHeight = 26;
    constexpr int historySize = 1024;       // the monitor's columns, at most
    constexpr double bandsShownMs = 1500.0; // Bands, brought up while a crossover or a pre gain moves under Monitor
    constexpr float snapDistance = 6.0f;    // pixels from a grid line within which a dragged point snaps to it
    constexpr float tensionDrag = 2.0f;     // a drag the curve's height bends a line by this much of its range
    constexpr float fineTension = 0.2f;     // with Ctrl

    juce::Colour getBandColour (int band)
    {
        return juce::Colour (partColours[(size_t) band]);
    }

    std::string_view idOf (int index)
    {
        return Multiband::info.parameters[(size_t) index].id;
    }

    float axisOf (float decibels)
    {
        return Multiband::axisForDecibels (decibels);
    }

    // A point of a curve: across, up, its line's tension and its line's shape.
    using CurvePoint = std::array<float, Multiband::pointFields>;

    CurvePoint makePoint (float across, float up)
    {
        return { across, up, 0.0f, (float) Multiband::PointType::singleCurve };
    }

    // The shapes the curve's menu starts from, as Maximus's manual shows its curves: each point by its levels in and out.
    struct Shape
    {
        const char* name;
        std::vector<CurvePoint> points;
    };

    const std::vector<Shape>& getShapes()
    {
        static const std::vector<Shape> shapes {
            { "Limit at 0 dB", { makePoint (0.0f, 0.0f), makePoint (axisOf (0.0f), axisOf (0.0f)), makePoint (1.0f, axisOf (0.0f)) } },
            { "No Compression", { makePoint (0.0f, 0.0f), makePoint (1.0f, 1.0f) } },
            { "Compress 2:1 over -12 dB",
              { makePoint (0.0f, 0.0f), makePoint (axisOf (-12.0f), axisOf (-12.0f)), makePoint (axisOf (0.0f), axisOf (-6.0f)),
                makePoint (1.0f, axisOf (0.0f)) } },
            { "Compress 4:1 over -18 dB",
              { makePoint (0.0f, 0.0f), makePoint (axisOf (-18.0f), axisOf (-18.0f)), makePoint (axisOf (0.0f), axisOf (-13.5f)),
                makePoint (1.0f, axisOf (-10.5f)) } },
            { "Gate under -30 dB",
              { makePoint (0.0f, 0.0f), makePoint (axisOf (-31.0f), 0.0f), makePoint (axisOf (-30.0f), axisOf (-30.0f)),
                makePoint (1.0f, 1.0f) } },
            { "Expand under -24 dB",
              { makePoint (0.0f, 0.0f), makePoint (axisOf (-40.0f), axisOf (-56.0f)), makePoint (axisOf (-24.0f), axisOf (-24.0f)),
                makePoint (1.0f, 1.0f) } },
            { "Lift under -30 dB",
              { makePoint (0.0f, 0.0f), makePoint (axisOf (-60.0f), axisOf (-45.0f)), makePoint (axisOf (-30.0f), axisOf (-30.0f)),
                makePoint (1.0f, 1.0f) } },
            { "Squeeze Both Ways",
              { makePoint (0.0f, 0.0f), makePoint (axisOf (-60.0f), axisOf (-42.0f)), makePoint (axisOf (-30.0f), axisOf (-30.0f)),
                makePoint (axisOf (0.0f), axisOf (-14.0f)), makePoint (1.0f, axisOf (-11.0f)) } },
        };
        return shapes;
    }

    // Copy Curve's curve, for Paste Curve, any band's, for the session.
    std::optional<std::vector<CurvePoint>>& getCopiedCurve()
    {
        static std::optional<std::vector<CurvePoint>> copied;
        return copied;
    }
} // namespace

//==============================================================================
// The selected band's compression curve, as Maximus's envelope editor shows and edits it.
class MultibandEditor::Curve : public juce::Component
{
public:
    explicit Curve (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Compression curve");
        setDescription ("How loud each level comes out of the selected band's compressor: level in across, level out up. Drag a "
                        "point (Shift keeps it level, Ctrl keeps it where it is across); drag a hollow handle up or down to bend "
                        "the line before it (Ctrl in fine steps) and right-click it to straighten it; right-click a point for its "
                        "line's shape or to delete it (Alt+click deletes it too); right-click elsewhere to add a point.");
    }

    bool frozen = false, snaps = true;

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (GraphTint::handleRadius * 1.5f + 1.0f); }

    juce::Point<float> toScreen (float across, float up) const
    {
        const auto area = getArea();
        return { area.getX() + across * area.getWidth(), area.getBottom() - up * area.getHeight() };
    }

    float acrossAt (float x) const { return juce::jlimit (0.0f, 1.0f, (x - getArea().getX()) / getArea().getWidth()); }
    float upAt (float y) const { return juce::jlimit (0.0f, 1.0f, (getArea().getBottom() - y) / getArea().getHeight()); }

    int getBand() const noexcept { return editor.selectedBand; }

    int getCount() const
    {
        return juce::jlimit (2, Multiband::maxPoints,
                             juce::roundToInt (editor.values[(size_t) Multiband::getBandParameter (getBand(), Multiband::points)]));
    }

    float getValue (int point, Multiband::PointField field) const
    {
        return editor.values[(size_t) Multiband::getPointParameter (getBand(), point, field)];
    }

    CurvePoint getPoint (int point) const
    {
        return { getValue (point, Multiband::pointX), getValue (point, Multiband::pointY), getValue (point, Multiband::pointTension),
                 getValue (point, Multiband::pointType) };
    }

    std::vector<CurvePoint> getPoints() const
    {
        std::vector<CurvePoint> points;

        for (int i = 0; i < getCount(); ++i)
            points.push_back (getPoint (i));

        return points;
    }

    juce::Point<float> getPointPosition (int point) const
    {
        return toScreen (getValue (point, Multiband::pointX), getValue (point, Multiband::pointY));
    }

    std::optional<juce::Point<float>> getTensionPosition (int point) const
    {
        if (point <= 0 || point >= getCount())
            return {};

        const auto middle = 0.5f * (getValue (point - 1, Multiband::pointX) + getValue (point, Multiband::pointX));
        return toScreen (middle, Multiband::getCurveAt (editor.values, getBand(), middle));
    }

    // Writes a whole curve, its points in order (the last one repeated in the slots it leaves), as one undo step.
    void setPoints (std::vector<CurvePoint> points, const juce::String& undoName)
    {
        if (points.size() < 2)
            return;

        points.resize (std::min (points.size(), (size_t) Multiband::maxPoints));
        const auto band = getBand();
        auto holder = editor.device.getParameter (idOf (Multiband::getBandParameter (band, Multiband::points)), undoName);
        holder.beginGesture();

        for (int i = 0; i < Multiband::maxPoints; ++i)
        {
            const auto& point = points[(size_t) std::min (i, (int) points.size() - 1)];

            for (int field = 0; field < Multiband::pointFields; ++field)
            {
                auto handle = editor.device.getParameter (idOf (Multiband::getPointParameter (band, i, (Multiband::PointField) field)));

                if (! juce::approximatelyEqual (handle.getValue(), point[(size_t) field]))
                {
                    handle.beginGesture();
                    handle.setValue (point[(size_t) field]);
                    handle.endGesture();
                }
            }
        }

        holder.setValue ((float) points.size());
        holder.endGesture();
        editor.refresh();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto colour = getBandColour (getBand());
        const auto area = getArea();
        graph::paintBackground (g, p, getLocalBounds().toFloat());

        // The grid at Maximus's levels, the 0 dB lines through the middle stronger, and level in equalling level out.
        g.setFont (juce::FontOptions (TextSize::small));

        for (const auto decibels : gridDecibels)
        {
            const auto place = axisOf (decibels);
            const auto at = toScreen (place, place);
            g.setColour (p.text.withAlpha (GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (at.x), area.getY(), area.getBottom());
            g.drawHorizontalLine (juce::roundToInt (at.y), area.getX(), area.getRight());
        }

        const auto middle = toScreen (0.5f, 0.5f);
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawVerticalLine (juce::roundToInt (middle.x), area.getY(), area.getBottom());
        g.drawHorizontalLine (juce::roundToInt (middle.y), area.getX(), area.getRight());
        g.drawLine ({ toScreen (0.0f, 0.0f), toScreen (1.0f, 1.0f) });
        g.setColour (p.textMuted);
        g.drawText ("OUT", area.reduced (Space::xs).toNearestInt(), juce::Justification::topLeft);
        g.drawText ("IN", area.reduced (Space::xs).toNearestInt(), juce::Justification::bottomRight);

        // The curve, over a wash of what it lets through.
        const auto line = graph::makeCurve (
            area, [this, area] (float x)
            { return area.getBottom() - Multiband::getCurveAt (editor.values, getBand(), acrossAt (x)) * area.getHeight(); }, 1.0f);
        auto wash = line;
        wash.lineTo (area.getBottomRight());
        wash.lineTo (area.getBottomLeft());
        wash.closeSubPath();
        g.setColour ((frozen ? p.textMuted : colour).withAlpha (GraphTint::partCurve));
        g.fillPath (wash);
        g.setColour (frozen ? p.textMuted : colour);
        g.strokePath (line, juce::PathStrokeType (GraphTint::curveWidth));

        // What the band's compressor hears now, riding the curve.
        if (editor.shownHeard[(size_t) getBand()] > -90.0f)
        {
            const auto in = std::min (1.0f, axisOf (editor.shownHeard[(size_t) getBand()]));
            const auto dot = toScreen (in, Multiband::getCurveAt (editor.values, getBand(), in));
            g.setColour (p.text);
            g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
        }

        // Its points filled, its tension handles hollow; larger under the pointer and while dragged. Frozen, none.
        if (frozen)
            return;

        for (int i = 0; i < getCount(); ++i)
        {
            if (const auto handle = getTensionPosition (i))
                paintHandle (g, p, colour, { false, i }, *handle);

            paintHandle (g, p, colour, { true, i }, getPointPosition (i));
        }
    }

    // What a press takes: a point, or the tension handle of the line before it.
    struct Target
    {
        bool point = true;
        int index = 0;

        bool operator== (const Target&) const = default;
    };

    std::optional<Target> findTarget (juce::Point<float> position) const
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

        for (int i = 0; i < getCount(); ++i)
        {
            consider ({ true, i }, getPointPosition (i));

            if (const auto handle = getTensionPosition (i))
                consider ({ false, i }, *handle);
        }

        return nearest;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto target = frozen ? std::optional<Target>() : findTarget (e.position);

        if (target != hovered)
        {
            hovered = target;
            setMouseCursor (! target.has_value() ? juce::MouseCursor::NormalCursor
                            : target->point      ? juce::MouseCursor::DraggingHandCursor
                                                 : juce::MouseCursor::UpDownResizeCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered.reset();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (frozen)
            return;

        const auto target = findTarget (e.position);

        // The right button: a point's menu, a handle straightened, or a point added where it is.
        if (e.mods.isPopupMenu())
        {
            if (! target.has_value())
                addPoint (e.position);
            else if (target->point)
                showPointMenu (target->index);
            else
                setValue (target->index, Multiband::pointTension, 0.0f, "Straighten Line");

            return;
        }

        if (! target.has_value())
            return;

        if (target->point && e.mods.isAltDown())
        {
            editor.deletePoint (target->index);
            return;
        }

        dragged = target;
        const auto band = getBand();
        const auto field = [band, &target] (Multiband::PointField f)
        { return idOf (Multiband::getPointParameter (band, target->index, f)); };

        if (target->point)
        {
            startAcross = getValue (target->index, Multiband::pointX);
            startUp = getValue (target->index, Multiband::pointY);
            first = editor.device.getParameter (field (Multiband::pointX), "Move Point");
            second = editor.device.getParameter (field (Multiband::pointY));
            first.beginGesture(); // its gesture holds the step, which the other's joins
            second.beginGesture();
        }
        else
        {
            startUp = getValue (target->index, Multiband::pointTension);
            first = editor.device.getParameter (field (Multiband::pointTension), "Bend Line");
            first.beginGesture();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragged.has_value())
            return;

        const auto area = getArea();
        const auto up = (float) -e.getDistanceFromDragStartY() / std::max (1.0f, area.getHeight());
        const auto i = dragged->index;

        if (! dragged->point)
        {
            // Up bends the line up; Ctrl in fine steps.
            const auto fine = e.mods.isCtrlDown() ? fineTension : 1.0f;
            first.setValue (juce::jlimit (-1.0f, 1.0f, startUp + tensionDrag * up * fine));
            editor.refresh();
            return;
        }

        // Shift keeps it level, Ctrl keeps it where it is across; the first and last points stay at the curve's ends.
        const auto along = (float) e.getDistanceFromDragStartX() / std::max (1.0f, area.getWidth());
        const auto isEnd = i == 0 || i == getCount() - 1;
        auto across = isEnd || e.mods.isCtrlDown() ? startAcross : startAcross + along;
        auto height = e.mods.isShiftDown() ? startUp : startUp + up;

        if (snaps)
        {
            across = snap (across, area.getWidth());
            height = snap (height, area.getHeight());
        }

        const auto earliest = i > 0 ? getValue (i - 1, Multiband::pointX) : 0.0f;
        const auto latest = i < getCount() - 1 ? getValue (i + 1, Multiband::pointX) : 1.0f;
        first.setValue (isEnd ? startAcross : juce::jlimit (earliest, std::max (earliest, latest), across));
        second.setValue (juce::jlimit (0.0f, 1.0f, height));
        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragged.has_value())
            return;

        if (second.isInGesture())
            second.endGesture();

        if (first.isInGesture())
            first.endGesture(); // its gesture holds the step: last

        dragged.reset();
        repaint();
    }

    // A point's line's shape, by the menu; one undo step.
    void setValue (int point, Multiband::PointField field, float value, const juce::String& undoName)
    {
        auto handle = editor.device.getParameter (idOf (Multiband::getPointParameter (getBand(), point, field)), undoName);
        handle.setValue (value);
        editor.refresh();
    }

    void addPoint (juce::Point<float> position)
    {
        auto points = getPoints();

        if ((int) points.size() >= Multiband::maxPoints)
            return;

        const auto across = acrossAt (position.x), height = upAt (position.y);
        auto at = points.begin() + 1;

        while (at != points.end() - 1 && (*at)[Multiband::pointX] < across)
            ++at;

        points.insert (at, makePoint (across, height));
        setPoints (points, "Add Point");
    }

private:
    MultibandEditor& editor;
    std::optional<Target> hovered, dragged;
    ParameterHandle first, second; // what a drag moves
    float startAcross = 0.0f, startUp = 0.0f;

    // To the nearest of the grid's lines, the middle and the ends, within a few pixels of it.
    static float snap (float place, float size)
    {
        auto best = place, bestDistance = snapDistance / std::max (1.0f, size);

        const auto consider = [&] (float line)
        {
            if (const auto distance = std::abs (line - place); distance <= bestDistance)
            {
                best = line;
                bestDistance = distance;
            }
        };

        for (const auto decibels : gridDecibels)
            consider (axisOf (decibels));

        for (const auto line : { 0.0f, 0.5f, 1.0f })
            consider (line);

        return best;
    }

    void paintHandle (juce::Graphics& g, const Palette& p, juce::Colour colour, Target target, juce::Point<float> at) const
    {
        const auto radius = target == hovered || target == dragged ? GraphTint::handleRadius * 1.5f : GraphTint::handleRadius;
        const auto dot = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (at);
        g.setColour (p.background);
        g.fillEllipse (dot.expanded (1.0f));
        g.setColour (colour);

        if (target.point)
            g.fillEllipse (dot);
        else
            g.drawEllipse (dot.reduced (GraphTint::handleOutline / 2.0f), GraphTint::handleOutline);
    }

    void showPointMenu (int point)
    {
        juce::PopupMenu menu;
        const auto current = juce::roundToInt (getValue (point, Multiband::pointType));
        const auto& names =
            Multiband::info.parameters[(size_t) Multiband::getPointParameter (getBand(), point, Multiband::pointType)].choices;

        for (int type = 0; type < (int) names.size(); ++type)
            menu.addItem (juce::PopupMenu::Item (juce::String (names[(size_t) type].data(), names[(size_t) type].size()))
                              .setID (type + 1)
                              .setEnabled (point > 0)
                              .setTicked (point > 0 && type == current));

        menu.addSeparator();
        menu.addItem (juce::PopupMenu::Item ("Delete Point").setID (100).setEnabled (point > 0 && point < getCount() - 1));

        const juce::Component::SafePointer<Curve> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safeThis, point] (int result)
                            {
                                if (safeThis == nullptr || result <= 0)
                                    return;

                                if (result == 100)
                                    safeThis->editor.deletePoint (point);
                                else
                                    safeThis->editor.setPointType (point, result - 1);
                            });
    }
};

//==============================================================================
// The analysis display: Monitor or Bands, as Maximus's.
class MultibandEditor::Analysis : public juce::Component
{
public:
    explicit Analysis (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Analysis");
        setDescription ("Monitor: the last seconds of the selected band, what comes in, what goes out, what its compressor hears and "
                        "its gain, scrolling; a click pauses it. Bands: the three bands across the spectrum, as high as their pre "
                        "gains: drag a band's edge to move its crossover, a band up or down for its pre gain.");
        // Silence at first, so that a history that stays silent is the same all along and needn't repaint.
        Column silence {};
        silence.fill (-120.0f);

        for (int band = 0; band < Multiband::numBands; ++band)
            silence[(size_t) band * 4 + 3] = 0.0f; // no reduction

        columns.assign (historySize, silence);
    }

    // The traces Monitor shows, each by its switch.
    enum Trace
    {
        mainInput,
        bandInput,
        bandOutput,
        heard,
        gain,
        allGains,
        levels,
        numTraces
    };

    std::array<bool, numTraces> traces { true, false, true, true, true, false, true };
    bool showsBands = false, paused = false, showsSpectrum = true;
    double bandsUntil = 0.0;      // while a crossover or a pre gain moves under Monitor, Bands shows until then
    bool wasBandsShowing = false; // at the last refresh, so that Monitor comes back when the moment is over
    float speed = 0.5f;           // from 0 (slowest) to 1
    graph::SpectrumTrace spectrum;

    bool isBandsShowing() const { return showsBands || juce::Time::getMillisecondCounterHiRes() < bandsUntil; }

    // A column: each band's level in, out, heard and gain, then what came into the device, in decibels.
    using Column = std::array<float, 4 * Multiband::numBands + 1>;

    void push (const Column& column)
    {
        if (paused)
            return;

        // Repainted while what shows changes: a history all alike, silence, say, looks the same scrolled.
        sameInARow = column == columns[(size_t) newest] ? std::min (sameInARow + 1, historySize) : 0;
        newest = (newest + 1) % historySize;
        columns[(size_t) newest] = column;

        if (sameInARow < historySize && ! isBandsShowing())
            repaint();
    }

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (1.0f); }

    float yForPlace (float place) const { return getArea().getBottom() - juce::jlimit (0.0f, 1.0f, place) * getArea().getHeight(); }
    float yForDecibels (float decibels) const { return yForPlace (axisOf (decibels)); }

    graph::FrequencyAxis getFrequencies() const { return { getArea() }; }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        graph::paintBackground (g, p, getLocalBounds().toFloat());

        if (isBandsShowing())
            paintBands (g, p);
        else
            paintMonitor (g, p);

        // The levels' scale down the right, as on the curve.
        const auto area = getArea();
        g.setFont (juce::FontOptions (TextSize::small));

        for (const auto decibels : gridDecibels)
        {
            const auto y = yForDecibels (decibels);
            g.setColour (p.text.withAlpha (GraphTint::grid));
            g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
            g.setColour (p.textMuted);
            g.drawText (juce::String ((int) decibels), juce::Rectangle<float> (area.getRight() - 28.0f, y - 7.0f, 24.0f, 14.0f),
                        juce::Justification::centredRight);
        }

        const auto zero = yForDecibels (0.0f);
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (zero), area.getX(), area.getRight());
        g.setColour (p.textMuted);
        g.drawText ("0", juce::Rectangle<float> (area.getRight() - 28.0f, zero - 7.0f, 24.0f, 14.0f), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isBandsShowing())
        {
            paused = ! paused; // Monitor: a click pauses it, another goes on
            repaint();
            return;
        }

        // Bands: a crossover's edge near the pointer, or the band under it for its pre gain.
        const auto axis = getFrequencies();
        dragged.reset();

        for (const auto global : { Multiband::lowFrequency, Multiband::highFrequency })
            if (std::abs (axis.xFor (editor.values[(size_t) global]) - e.position.x) <= GraphTint::grabDistance)
                dragged = (int) global;

        if (! dragged.has_value())
        {
            const auto frequency = axis.frequencyAt (e.position.x);
            const auto band = frequency < editor.values[(size_t) Multiband::lowFrequency]    ? Multiband::low
                              : frequency < editor.values[(size_t) Multiband::highFrequency] ? Multiband::mid
                                                                                             : Multiband::high;
            dragged = Multiband::getBandParameter (band, Multiband::pre);
        }

        handle = editor.device.getParameter (idOf (*dragged));
        handle.beginGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragged.has_value())
            return;

        if (*dragged == Multiband::lowFrequency || *dragged == Multiband::highFrequency)
        {
            handle.setValue (
                devices::constrain (Multiband::info.parameters[(size_t) *dragged], (float) getFrequencies().frequencyAt (e.position.x)));
        }
        else
        {
            // The band's level under the pointer: its pre gain, on the curves' scale.
            const auto place = juce::jlimit (0.0f, 1.0f, (getArea().getBottom() - e.position.y) / getArea().getHeight());
            const auto decibels = toDecibels (Multiband::fromAxis (place));
            handle.setValue (devices::constrain (Multiband::info.parameters[(size_t) *dragged], decibels));
        }

        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (handle.isInGesture())
            handle.endGesture();

        dragged.reset();
    }

private:
    MultibandEditor& editor;
    std::vector<Column> columns;
    int newest = 0, sameInARow = historySize; // how many columns in a row came in alike: at first, all of them
    std::optional<int> dragged;               // a crossover's or a band's pre gain's parameter
    ParameterHandle handle;

    static float toDecibels (float gain) { return gain <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (gain); }

    void paintMonitor (juce::Graphics& g, const Palette& p)
    {
        const auto area = getArea();
        const auto band = editor.selectedBand;
        const auto colour = getBandColour (band);
        const auto step = 1.0f + 3.0f * speed; // pixels a column takes: faster, wider
        const auto count = std::min (historySize, (int) (area.getWidth() / step) + 1);

        // A trace's path, newest at the right, from a column's value.
        const auto trace = [&] (auto valueOf, bool filled)
        {
            juce::Path path;

            for (int i = 0; i < count; ++i)
            {
                const auto& column = columns[(size_t) ((newest - i + historySize) % historySize)];
                const auto x = area.getRight() - (float) i * step;
                const auto y = yForDecibels (valueOf (column));

                if (i == 0)
                    path.startNewSubPath (x, filled ? area.getBottom() : y);

                path.lineTo (x, y);
            }

            if (filled)
            {
                path.lineTo (area.getRight() - (float) (count - 1) * step, area.getBottom());
                path.closeSubPath();
            }

            return path;
        };

        const auto b = (size_t) band * 4;

        if (traces[mainInput])
        {
            g.setColour (p.text.withAlpha (GraphTint::spectrumFill));
            g.fillPath (trace ([] (const Column& c) { return c.back(); }, true));
        }

        if (traces[bandInput])
        {
            g.setColour (colour.withAlpha (GraphTint::spectrumFill));
            g.fillPath (trace ([b] (const Column& c) { return c[b]; }, true));
        }

        if (traces[bandOutput])
        {
            g.setColour (colour.withAlpha (GraphTint::partCurve * 2.0f));
            g.fillPath (trace ([b] (const Column& c) { return c[b + 1]; }, true));
        }

        if (traces[heard])
        {
            g.setColour (p.modulation);
            g.strokePath (trace ([b] (const Column& c) { return c[b + 2]; }, false), juce::PathStrokeType (1.5f));
        }

        if (traces[allGains])
        {
            for (int other = 0; other < Multiband::numBands; ++other)
            {
                const auto o = (size_t) other * 4;
                g.setColour (getBandColour (other));
                g.strokePath (trace ([o] (const Column& c) { return -c[o + 3]; }, false), juce::PathStrokeType (1.0f));
            }
        }

        if (traces[gain])
        {
            g.setColour (p.text);
            g.strokePath (trace ([b] (const Column& c) { return -c[b + 3]; }, false), juce::PathStrokeType (1.5f));
        }

        // The band's pre and post gains, as level marks.
        if (traces[levels])
        {
            const auto pre = editor.values[(size_t) Multiband::getBandParameter (band, Multiband::pre)];
            const auto post = editor.values[(size_t) Multiband::getBandParameter (band, Multiband::post)];
            g.setColour (getBandColour (6)); // purple
            g.drawHorizontalLine (juce::roundToInt (yForDecibels (pre)), area.getX(), area.getRight());
            g.setColour (getBandColour (5)); // blue
            g.drawHorizontalLine (juce::roundToInt (yForDecibels (post)), area.getX(), area.getRight());
        }

        if (paused)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText ("Paused", area.reduced (Space::s).toNearestInt(), juce::Justification::topLeft);
        }
    }

    void paintBands (juce::Graphics& g, const Palette& p)
    {
        const auto area = getArea();
        const auto axis = getFrequencies();
        const auto low = editor.values[(size_t) Multiband::lowFrequency], high = editor.values[(size_t) Multiband::highFrequency];
        const std::array<std::pair<double, double>, 3> spans { std::pair { 20.0, (double) low }, std::pair { (double) low, (double) high },
                                                               std::pair { (double) high, 20000.0 } };

        // Each band as high as its pre gain, in its colour; faint while it is off or muted.
        for (int band = 0; band < 3; ++band)
        {
            const auto mode = juce::roundToInt (editor.values[(size_t) Multiband::getBandParameter (band, Multiband::mode)]);
            const auto quiet = mode == (int) Multiband::Mode::muted || mode == (int) Multiband::Mode::off;
            const auto left = axis.xFor (spans[(size_t) band].first), right = axis.xFor (spans[(size_t) band].second);
            const auto top = yForDecibels (editor.values[(size_t) Multiband::getBandParameter (band, Multiband::pre)]);
            const auto fill = juce::Rectangle<float>::leftTopRightBottom (left, top, right, area.getBottom());
            g.setColour (getBandColour (band).withAlpha (quiet ? GraphTint::spectrumFill : GraphTint::partCurve));
            g.fillRect (fill);
            g.setColour (getBandColour (band));
            g.drawHorizontalLine (juce::roundToInt (top), left, right);
        }

        // What it plays, over them.
        if (showsSpectrum)
            spectrum.paint (g, axis, { area, -90.0f, 6.0f }, p.text.withAlpha (GraphTint::spectrumFill),
                            p.text.withAlpha (GraphTint::spectrumLine));

        // The crossovers, which a drag moves.
        g.setColour (p.text.withAlpha (GraphTint::marker));

        for (const auto frequency : { low, high })
            g.drawVerticalLine (juce::roundToInt (axis.xFor (frequency)), area.getY(), area.getBottom());

        graph::paintFrequencyGrid (g, p, axis, true);
    }
};

//==============================================================================
// The editor's one panel, in Maximus's layout: the curve and the analysis display over their toolbars, and the selected band's
// controls and the global ones below.
class MultibandEditor::Panel : public juce::Component
{
public:
    explicit Panel (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Multiband");

        // The curve's toolbar: its menu, Freeze and Snap.
        menuButton.setTooltip ("The curve's menu: shapes to start from, copy and paste, flip, smooth all points, reset");
        menuButton.onClick = [this] { showCurveMenu(); };
        freezeButton.setClickingTogglesState (true);
        freezeButton.setTooltip ("Freeze: the curve can't be edited while this is on");
        freezeButton.onClick = [this]
        {
            editor.curve->frozen = freezeButton.getToggleState();
            editor.curve->repaint();
        };
        snapButton.setClickingTogglesState (true);
        snapButton.setToggleState (true, juce::dontSendNotification);
        snapButton.setTooltip ("Snap: points dragged near the grid's lines go onto them");
        snapButton.onClick = [this] { editor.curve->snaps = snapButton.getToggleState(); };

        for (auto* button : { &menuButton, &freezeButton, &snapButton })
            addAndMakeVisible (button);

        // The display's toolbar: its traces' switches, its speed, the spectrum, and Monitor or Bands.
        static constexpr const char* traceNames[] = { "In", "Band In", "Out", "Heard", "Gain", "All Gains", "Pre/Post" };
        static constexpr const char* traceTips[] = {
            "Shows what comes into the multiband",
            "Shows what comes into the selected band, after its pre gain",
            "Shows what the selected band puts out",
            "Shows what the selected band's compressor hears",
            "Shows the selected band's gain",
            "Shows every band's gain, each in its colour",
            "Shows the selected band's pre (purple) and post (blue) gains",
        };

        for (int i = 0; i < Analysis::numTraces; ++i)
        {
            auto& button = *traceButtons.add (std::make_unique<juce::TextButton> (traceNames[i]));
            button.setClickingTogglesState (true);
            button.setToggleState (editor.analysis->traces[(size_t) i], juce::dontSendNotification);
            button.setTooltip (traceTips[i]);
            button.onClick = [this, i, &button]
            {
                editor.analysis->traces[(size_t) i] = button.getToggleState();
                editor.analysis->repaint();
            };
            addAndMakeVisible (button);
        }

        speed.setSliderStyle (juce::Slider::LinearHorizontal);
        speed.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        speed.setRange (0.0, 1.0);
        speed.setValue (editor.analysis->speed, juce::dontSendNotification);
        speed.setTitle ("Speed");
        speed.setTooltip ("How fast Monitor scrolls: slower to the left, faster to the right");
        speed.onValueChange = [this] { editor.analysis->speed = (float) speed.getValue(); };
        addAndMakeVisible (speed);
        speedName.setFont (juce::FontOptions (TextSize::small));
        speedName.setJustificationType (juce::Justification::centredRight);
        speedName.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (speedName);

        spectrumButton.setClickingTogglesState (true);
        spectrumButton.setToggleState (true, juce::dontSendNotification);
        spectrumButton.setTooltip ("Shows the spectrum of what the multiband plays over Bands");
        spectrumButton.onClick = [this] { editor.analysis->showsSpectrum = spectrumButton.getToggleState(); };
        addAndMakeVisible (spectrumButton);

        for (auto* button : { &monitorButton, &bandsButton })
        {
            button->setClickingTogglesState (true);
            button->setRadioGroupId (1);
            addAndMakeVisible (button);
        }

        monitorButton.setToggleState (true, juce::dontSendNotification);
        monitorButton.setTooltip ("Monitor: the last seconds of the selected band");
        bandsButton.setTooltip ("Bands: the bands across the spectrum, with their crossovers and pre gains to drag");
        monitorButton.onClick = [this] { editor.showBands (false); };
        bandsButton.onClick = [this] { editor.showBands (true); };

        // The band tabs, low in red, mid in orange, high in yellow and the master in green, as Maximus colours them.
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            auto& button = *bandButtons.add (std::make_unique<juce::TextButton> (bandNames[band]));
            button.setClickingTogglesState (true);
            button.setRadioGroupId (2);
            button.setColour (juce::TextButton::buttonOnColourId, getBandColour (band).withAlpha (0.6f));
            button.setTooltip (juce::String ("Shows the ")
                               + (band == Multiband::master ? "master band's" : bandNames[band] + juce::String (" band's"))
                               + " curve and controls");
            button.onClick = [this, band] { editor.selectBand (band); };
            addAndMakeVisible (button);
        }
    }

    // Each band's own controls, of which the selected band's show.
    struct BandControls
    {
        ChoiceButtons* mode = nullptr;
        ParameterControls* solo = nullptr; // none for the master
        ParameterControls* stereo = nullptr;
        ParameterControls* gains = nullptr;
        ParameterControls* saturation = nullptr;
        ParameterControls* envelope = nullptr;
        ParameterControls* curves = nullptr;
        ChoiceButtons* detection = nullptr;
        ParameterControls* release2 = nullptr;

        std::vector<juce::Component*> getAll() const
        {
            return { mode, solo, stereo, gains, saturation, envelope, curves, detection, release2 };
        }
    };

    std::array<BandControls, Multiband::numBands> bands;
    ParameterControls *lmhDelay = nullptr, *lmhMix = nullptr, *frequencies = nullptr, *lowCut = nullptr, *midDrives = nullptr;
    ChoiceButtons *lowSlope = nullptr, *highSlope = nullptr;
    juce::OwnedArray<juce::TextButton> bandButtons;

    void addControl (juce::Component* control, bool visible)
    {
        if (control == nullptr)
            return;

        addChildComponent (control);
        control->setVisible (visible);
    }

    void showBand (int shown)
    {
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            for (auto* control : bands[(size_t) band].getAll())
                if (control != nullptr)
                    control->setVisible (band == shown);

            bandButtons[band]->setToggleState (band == shown, juce::dontSendNotification);
        }

        resized();
    }

    void syncViewButtons()
    {
        monitorButton.setToggleState (! editor.analysis->showsBands, juce::dontSendNotification);
        bandsButton.setToggleState (editor.analysis->showsBands, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (std::max (200, area.getHeight() / 2));
        area.removeFromTop (Space::s);

        // The curve, square, over its toolbar; the display beside it, over its own.
        auto curveColumn = top.removeFromLeft (top.getHeight() - toolbarHeight);
        auto curveTools = curveColumn.removeFromBottom (toolbarHeight).reduced (0, 2);
        editor.curve->setBounds (curveColumn);
        menuButton.setBounds (curveTools.removeFromLeft (toolbarHeight + Space::s));
        curveTools.removeFromLeft (Space::xs);
        freezeButton.setBounds (curveTools.removeFromLeft (toolbarHeight + Space::s));
        curveTools.removeFromLeft (Space::xs);
        snapButton.setBounds (curveTools.removeFromLeft (toolbarHeight + Space::s));

        top.removeFromLeft (Space::s);
        auto displayTools = top.removeFromBottom (toolbarHeight).reduced (0, 2);
        editor.analysis->setBounds (top);
        bandsButton.setBounds (displayTools.removeFromRight (64));
        monitorButton.setBounds (displayTools.removeFromRight (72));
        displayTools.removeFromRight (Space::s);
        spectrumButton.setBounds (displayTools.removeFromRight (72));
        displayTools.removeFromRight (Space::xs);
        auto speedArea = displayTools.removeFromRight (std::min (128, displayTools.getWidth() / 3));
        const auto nameWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (speedName.getFont(), speedName.getText()));
        speedName.setBounds (speedArea.removeFromLeft (nameWidth + Space::s));
        speed.setBounds (speedArea.reduced (Space::xs, 0));
        displayTools.removeFromRight (Space::s);

        const auto traceWidth = std::min (64, displayTools.getWidth() / std::max (1, traceButtons.size()));

        for (auto* button : traceButtons)
            button->setBounds (displayTools.removeFromLeft (traceWidth));

        // Below: the band tabs and Solo; the mode and stereo; the gains and saturation; the envelope; LMH; the crossovers.
        const auto& shown = bands[(size_t) editor.selectedBand];
        auto tabs = area.removeFromLeft (88);

        for (auto* button : bandButtons)
            button->setBounds (tabs.removeFromTop (controlHeight));

        tabs.removeFromTop (Space::s);

        if (shown.solo != nullptr)
            shown.solo->setBounds (tabs.removeFromTop (shown.solo->getPreferredHeight())
                                       .withSizeKeepingCentre (shown.solo->getPreferredWidth(), shown.solo->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto modes = area.removeFromLeft (96);
        shown.mode->setBounds (modes.removeFromTop (shown.mode->getPreferredHeight()));
        modes.removeFromTop (Space::s);
        shown.stereo->setBounds (modes.removeFromTop (shown.stereo->getPreferredHeight())
                                     .withSizeKeepingCentre (shown.stereo->getPreferredWidth(), shown.stereo->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto gains = area.removeFromLeft (shown.gains->getPreferredWidth());
        shown.gains->setBounds (gains.removeFromTop (shown.gains->getPreferredHeight()));
        gains.removeFromTop (Space::s);
        shown.saturation->setBounds (gains.removeFromTop (shown.saturation->getPreferredHeight()));

        // The envelope: its knobs, its curve boxes under them, and Release 2 with Peak or RMS beside it, as wide as its widest row.
        area.removeFromLeft (Space::m);
        const auto releasesWidth = shown.release2->getPreferredWidth() + Space::s + shown.detection->getPreferredWidth();
        auto envelope =
            area.removeFromLeft (std::max ({ shown.envelope->getPreferredWidth(), shown.curves->getPreferredWidth(), releasesWidth }));
        shown.envelope->setBounds (
            envelope.removeFromTop (shown.envelope->getPreferredHeight()).removeFromLeft (shown.envelope->getPreferredWidth()));
        shown.curves->setBounds (
            envelope.removeFromTop (shown.curves->getPreferredHeight()).removeFromLeft (shown.curves->getPreferredWidth()));
        auto releases = envelope.removeFromTop (std::max (shown.release2->getPreferredHeight(), shown.detection->getPreferredHeight()));
        shown.release2->setBounds (
            releases.removeFromLeft (shown.release2->getPreferredWidth()).withHeight (shown.release2->getPreferredHeight()));
        releases.removeFromLeft (Space::s);
        shown.detection->setBounds (
            releases.removeFromLeft (shown.detection->getPreferredWidth()).withHeight (shown.detection->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto lmh = area.removeFromLeft (lmhDelay->getPreferredWidth());
        lmhDelay->setBounds (lmh.removeFromTop (lmhDelay->getPreferredHeight()));
        lmhMix->setBounds (lmh.removeFromTop (lmhMix->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto crossovers = area.removeFromLeft (frequencies->getPreferredWidth());
        frequencies->setBounds (crossovers.removeFromTop (frequencies->getPreferredHeight()));
        auto slopes = crossovers.removeFromTop (lowSlope->getPreferredHeight());
        lowSlope->setBounds (slopes.removeFromLeft (slopes.getWidth() / 2).reduced (Space::xs, 0));
        highSlope->setBounds (slopes.reduced (Space::xs, 0));
        crossovers.removeFromTop (Space::xs);
        auto last = crossovers.removeFromTop (lowCut->getPreferredHeight());
        lowCut->setBounds (last.removeFromLeft (lowCut->getPreferredWidth()));
        midDrives->setBounds (last.removeFromLeft (midDrives->getPreferredWidth()));
    }

    // Wide enough for every part, and as tall as its controls need under the graphs.
    static constexpr int preferredWidth = 1000, preferredHeight = 470, minimumHeight = 430;

private:
    MultibandEditor& editor;
    IconButton menuButton { "Curve menu", Icon::menu }, freezeButton { "Freeze", Icon::frozen }, snapButton { "Snap", Icon::magnet };
    juce::OwnedArray<juce::TextButton> traceButtons;
    juce::Slider speed;
    juce::Label speedName { {}, "Speed" };
    juce::TextButton spectrumButton { "Spectrum" }, monitorButton { "Monitor" }, bandsButton { "Bands" };

    void showCurveMenu()
    {
        juce::PopupMenu shapes;

        for (const auto& name : getShapeNames())
            shapes.addItem (name,
                            [safe = juce::Component::SafePointer<Panel> (this), name]
                            {
                                if (safe != nullptr)
                                    safe->editor.applyShape (name);
                            });

        juce::PopupMenu menu;
        menu.addSubMenu ("Shapes", shapes);
        menu.addSeparator();
        const juce::Component::SafePointer<Panel> safe (this);
        menu.addItem ("Copy Curve",
                      [safe]
                      {
                          if (safe != nullptr)
                              getCopiedCurve() = safe->editor.curve->getPoints();
                      });
        menu.addItem ("Paste Curve", getCopiedCurve().has_value(), false,
                      [safe]
                      {
                          if (safe != nullptr && getCopiedCurve().has_value())
                              safe->editor.curve->setPoints (*getCopiedCurve(), "Paste Curve");
                      });
        menu.addItem ("Flip Vertically",
                      [safe]
                      {
                          if (safe == nullptr)
                              return;

                          auto points = safe->editor.curve->getPoints();

                          for (auto& point : points)
                              point[Multiband::pointY] = 1.0f - point[Multiband::pointY];

                          safe->editor.curve->setPoints (points, "Flip Curve");
                      });
        menu.addItem ("Smooth All Points",
                      [safe]
                      {
                          if (safe == nullptr)
                              return;

                          auto points = safe->editor.curve->getPoints();

                          for (auto& point : points)
                              point[Multiband::pointType] = (float) Multiband::PointType::singleCurve;

                          safe->editor.curve->setPoints (points, "Smooth Curve");
                      });
        menu.addItem ("Reset Curve",
                      [safe]
                      {
                          if (safe != nullptr)
                              safe->editor.applyShape (getShapes().front().name);
                      });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
    }
};

//==============================================================================
MultibandEditor::MultibandEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    for (const auto& parameter : Multiband::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    auto& shownCurve = keep (std::make_unique<Curve> (*this));
    curve = &shownCurve;
    auto& shownAnalysis = keep (std::make_unique<Analysis> (*this));
    analysis = &shownAnalysis;
    auto& shownPanel = keep (std::make_unique<Panel> (*this));
    panel = &shownPanel;
    panel->addAndMakeVisible (shownCurve);
    panel->addAndMakeVisible (shownAnalysis);

    // Each band's controls, by its parameters' IDs, named as Maximus names them.
    const auto id = [] (int band, Multiband::BandField field) { return idOf (Multiband::getBandParameter (band, field)); };

    for (int band = 0; band < Multiband::numBands; ++band)
    {
        auto& shown = panel->bands[(size_t) band];
        const auto visible = band == selectedBand;
        auto& mode = keep (std::make_unique<ChoiceButtons> (device, id (band, Multiband::mode), ChoiceButtons::Direction::down,
                                                            std::vector<juce::String> {
                                                                "The band is on",
                                                                "The band's compressor is off; its gains, saturation and stereo still work",
                                                                "The band is silent",
                                                                band == Multiband::mid || band == Multiband::master
                                                                    ? "The band passes as it comes in"
                                                                    : "The band is off: the mid band takes its room",
                                                            }));
        addControl (id (band, Multiband::mode), mode);
        shown.mode = &mode;

        if (band != Multiband::master)
            shown.solo = &makeControls ({ id (band, Multiband::solo) }, 1, ParameterControls::Size::compact, {}, { "Solo" });

        shown.stereo = &makeControls ({ id (band, Multiband::stereo) }, 1, ParameterControls::Size::compact, {}, { "Stereo" });
        shown.gains = &makeControls ({ id (band, Multiband::pre), id (band, Multiband::post) }, 2, ParameterControls::Size::compact, {},
                                     { "Pre", "Post" });
        shown.saturation = &makeControls ({ id (band, Multiband::saturation), id (band, Multiband::ceiling) }, 2,
                                          ParameterControls::Size::compact, {}, { "Sat", "Ceiling" });
        shown.envelope = &makeControls ({ id (band, Multiband::attack), id (band, Multiband::release), id (band, Multiband::sustain) }, 3,
                                        ParameterControls::Size::compact, {}, { "Attack", "Release", "Sustain" });
        shown.curves = &makeControls ({ id (band, Multiband::curve), id (band, Multiband::releaseCurve) }, 2,
                                      ParameterControls::Size::compact, {}, { "Curve", "Rel Curve" });
        auto& detection = keep (std::make_unique<ChoiceButtons> (
            device, id (band, Multiband::detection), ChoiceButtons::Direction::down,
            std::vector<juce::String> { "Peak: the compressor hears each peak, held for the sustain: the fastest",
                                        "RMS: the compressor hears the average over the sustain, so transients pass" }));
        addControl (id (band, Multiband::detection), detection);
        shown.detection = &detection;
        shown.release2 = &makeControls ({ id (band, Multiband::release2) }, 1, ParameterControls::Size::compact, {}, { "Release 2" });

        for (auto* control : shown.getAll())
            panel->addControl (control, visible);
    }

    panel->lmhDelay = &makeControls ({ idOf (Multiband::lmhDelay) }, 1, ParameterControls::Size::compact, {}, { "LMH Delay" });
    panel->lmhMix = &makeControls ({ idOf (Multiband::lmhMix) }, 1, ParameterControls::Size::compact, {}, { "LMH Mix" });
    panel->frequencies = &makeControls ({ idOf (Multiband::lowFrequency), idOf (Multiband::highFrequency) }, 2,
                                        ParameterControls::Size::compact, {}, { "Low", "High" });
    panel->lowCut = &makeControls ({ idOf (Multiband::lowCut) }, 1, ParameterControls::Size::compact, {}, { "Low Cut" });
    panel->midDrives = &makeControls ({ idOf (Multiband::midDrivesMaster) }, 1, ParameterControls::Size::compact, {}, { "Mid Drives" });
    auto& lowSlope = keep (std::make_unique<ChoiceButtons> (
        device, idOf (Multiband::lowSlope), ChoiceButtons::Direction::down,
        std::vector<juce::String> { "The low crossover: 12 dB per octave",
                                    "The low crossover: 24 dB per octave, the bands kept further apart" }));
    auto& highSlope = keep (std::make_unique<ChoiceButtons> (
        device, idOf (Multiband::highSlope), ChoiceButtons::Direction::down,
        std::vector<juce::String> { "The high crossover: 12 dB per octave",
                                    "The high crossover: 24 dB per octave, the bands kept further apart" }));
    addControl (idOf (Multiband::lowSlope), lowSlope);
    addControl (idOf (Multiband::highSlope), highSlope);
    panel->lowSlope = &lowSlope;
    panel->highSlope = &highSlope;

    for (auto* control : std::initializer_list<juce::Component*> { panel->lmhDelay, panel->lmhMix, panel->frequencies, panel->lowCut,
                                                                   panel->midDrives, panel->lowSlope, panel->highSlope })
        panel->addControl (control, true);

    // The meters' first readings would show what played before the editor opened.
    if (auto* multiband = device.getProcessor<Multiband>())
    {
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            multiband->getInputMeter (band).take();
            multiband->getOutputMeter (band).take();
            multiband->getHeardMeter (band).take();
            multiband->getReductionMeter (band).take();
        }

        multiband->getMainInputMeter().take();
    }

    panel->showBand (selectedBand);
    setParts ({ { { "Multiband", &shownPanel, Panel::preferredWidth, Panel::preferredHeight, Panel::minimumHeight, true } } });
}

MultibandEditor::~MultibandEditor()
{
    if (auto* scope = device.getScope())
        scope->setListening (false);
}

void MultibandEditor::selectBand (int band)
{
    selectedBand = juce::jlimit (0, Multiband::numBands - 1, band);
    panel->showBand (selectedBand);
    curve->repaint();
    analysis->repaint();
}

juce::Button& MultibandEditor::getBandButton (int band)
{
    return *panel->bandButtons[band];
}

juce::Component& MultibandEditor::getCurve() const noexcept
{
    return *curve;
}

juce::Component& MultibandEditor::getDisplay() const noexcept
{
    return *analysis;
}

juce::Point<float> MultibandEditor::getCurvePosition (float across, float up) const
{
    return curve->toScreen (across, up);
}

juce::Point<float> MultibandEditor::getPointPosition (int point) const
{
    return curve->getPointPosition (point);
}

std::optional<juce::Point<float>> MultibandEditor::getTensionPosition (int point) const
{
    return curve->getTensionPosition (point);
}

void MultibandEditor::setPointType (int point, int type)
{
    if (point > 0 && point < curve->getCount())
        curve->setValue (point, Multiband::pointType, (float) type, "Change Line");
}

void MultibandEditor::deletePoint (int point)
{
    auto points = curve->getPoints();

    if (point <= 0 || point >= (int) points.size() - 1)
        return;

    points.erase (points.begin() + point);
    curve->setPoints (points, "Delete Point");
}

bool MultibandEditor::applyShape (const juce::String& name)
{
    for (const auto& shape : getShapes())
    {
        if (name == shape.name)
        {
            curve->setPoints (shape.points, "Change Curve");
            return true;
        }
    }

    return false;
}

juce::StringArray MultibandEditor::getShapeNames()
{
    juce::StringArray names;

    for (const auto& shape : getShapes())
        names.add (shape.name);

    return names;
}

bool MultibandEditor::isShowingBands() const
{
    return analysis->isBandsShowing();
}

void MultibandEditor::showBands (bool bands)
{
    analysis->showsBands = bands;
    analysis->bandsUntil = 0.0;
    panel->syncViewButtons();
    analysis->repaint();
}

float MultibandEditor::getBandsX (double frequency) const
{
    return analysis->getFrequencies().xFor (frequency);
}

float MultibandEditor::getBandsY (float decibels) const
{
    return analysis->yForDecibels (decibels);
}

void MultibandEditor::refresh()
{
    // A crossover or a pre gain changed under Monitor brings Bands up for a moment, as in Maximus.
    const auto moved = [this] (int index)
    {
        return parameters[(size_t) index].isValid()
               && ! juce::approximatelyEqual (values[(size_t) index], parameters[(size_t) index].getValue());
    };
    const auto bandsMoved = moved (Multiband::lowFrequency) || moved (Multiband::highFrequency)
                            || moved (Multiband::getBandParameter (Multiband::low, Multiband::pre))
                            || moved (Multiband::getBandParameter (Multiband::mid, Multiband::pre))
                            || moved (Multiband::getBandParameter (Multiband::high, Multiband::pre));

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    if (bandsMoved && ! analysis->showsBands)
        analysis->bandsUntil = juce::Time::getMillisecondCounterHiRes() + bandsShownMs;

    if (const auto bands = analysis->isBandsShowing(); bands != analysis->wasBandsShowing)
    {
        analysis->wasBandsShowing = bands;
        analysis->repaint();
    }

    // The meters into Monitor's newest column, and what the curve's dot rides at.
    if (auto* multiband = device.getProcessor<Multiband>())
    {
        Analysis::Column column {};

        for (int band = 0; band < Multiband::numBands; ++band)
        {
            const auto b = (size_t) band * 4;
            column[b] = multiband->getInputMeter (band).take();
            column[b + 1] = multiband->getOutputMeter (band).take();
            column[b + 2] = multiband->getHeardMeter (band).take();
            const auto reduction = multiband->getReductionMeter (band).take();
            column[b + 3] = reduction <= -120.0f ? 0.0f : reduction;
            shownHeard[(size_t) band] = column[b + 2];
        }

        column.back() = multiband->getMainInputMeter().take();
        analysis->push (column);
    }

    if (auto* scope = device.getScope())
    {
        scope->setListening (analysis->isBandsShowing() && analysis->showsSpectrum);
        analysis->spectrum.setSampleRate (scope->getSampleRate());

        if (auto* output = scope->getOutput())
            analysis->spectrum.pull (*output);

        if (analysis->spectrum.update() && analysis->isBandsShowing())
            analysis->repaint();
    }

    auto numbers = values;
    numbers.push_back (shownHeard[(size_t) selectedBand]);

    if (drawnCurve.changed (std::move (numbers)))
    {
        curve->repaint();
        analysis->repaint();
    }
}
} // namespace phazite::ui
