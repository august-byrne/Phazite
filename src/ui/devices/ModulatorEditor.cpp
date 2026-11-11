// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulatorEditor.h"
#include "DeviceGraph.h"
#include "GraphDot.h"
#include "devices/dsp/Math.h"
#include "model/commands/SongGesture.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <array>
#include <optional>

namespace phazite::ui
{
namespace
{
    constexpr int titleHeight = 24, graphHeight = 64, nameHeight = 16, refreshRateHz = 30;
    constexpr double scopeSeconds = 2.0;

    // Where a value from -1 to 1 (or 0 to 1) is in an area, from its bottom to its top.
    float valueToY (juce::Rectangle<float> area, float value, bool bipolar)
    {
        const auto proportion = bipolar ? (value + 1.0f) / 2.0f : value;
        return area.getBottom() - juce::jlimit (0.0f, 1.0f, proportion) * area.getHeight();
    }
} // namespace

//==============================================================================
// The modulator's output over the last seconds, newest at the right.
struct ModulatorEditor::Scope : public juce::Component
{
    Scope (ProjectContext& c, ModulatorID m)
        : context (c),
          modulator (m)
    {
        setTitle ("Output");
        setInterceptsMouseClicks (false, false);
    }

    // Repainted when what it shows moved: a modulator at rest moves nothing (docs/performance-audit.md, section 8).
    void refresh()
    {
        auto now = getModulatorOutput (context, modulator, scopeSeconds);
        const auto nowBipolar = isBipolar (context, modulator);

        if (now != values || nowBipolar != bipolar)
        {
            values = std::move (now);
            bipolar = nowBipolar;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (p.surfaceRaised);
        g.fillRoundedRectangle (area, cornerRadius);

        // The middle, where a bipolar modulator moves nothing.
        g.setColour (p.border.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (valueToY (area, 0.0f, bipolar)), area.getX(), area.getRight());

        if (values.size() < 2)
            return;

        juce::Path line;

        for (size_t i = 0; i < values.size(); ++i)
        {
            const juce::Point<float> at (area.getX() + area.getWidth() * (float) i / (float) (values.size() - 1),
                                         valueToY (area, values[i], bipolar));

            if (i == 0)
                line.startNewSubPath (at);
            else
                line.lineTo (at);
        }

        g.setColour (p.modulation);
        g.strokePath (line, juce::PathStrokeType (GraphTint::curveWidth));
    }

    ProjectContext& context;
    const ModulatorID modulator;
    std::vector<float> values;
    bool bipolar = false;
};

//==============================================================================
// A step sequencer's steps, as bars from the middle (each from -1 to 1): a click sets the step under it, and a drag each
// step it passes, as one undo step, each as high as the pointer: the crosshair over them, the step under it lit.
struct ModulatorEditor::Steps : public juce::Component
{
    Steps (ProjectContext& c, ModulatorID m)
        : context (c),
          modulator (m)
    {
        setTitle ("Steps");
        setDescription ("Click a step to set it as high as the pointer, or drag across the steps to draw them.");
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
    }

    void refresh()
    {
        if (auto now = getSteps (context, modulator); now != values)
        {
            values = std::move (now);
            repaint();
        }
    }

    juce::Rectangle<float> getStepArea (int index) const
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto width = area.getWidth() / (float) juce::jmax (1, (int) values.size());
        return { area.getX() + width * (float) index, area.getY(), width, area.getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (p.surfaceRaised);
        g.fillRoundedRectangle (area, cornerRadius);

        const auto middle = valueToY (area, 0.0f, true);

        for (int i = 0; i < (int) values.size(); ++i)
        {
            const auto step = getStepArea (i).reduced (1.0f, 0.0f);
            const auto y = valueToY (area, values[(size_t) i], true);
            const auto bar = juce::Rectangle<float>::leftTopRightBottom (step.getX(), std::min (y, middle), step.getRight(),
                                                                         std::max (y, middle) + 1.0f);
            g.setColour (p.modulation);
            g.fillRect (bar);

            if (i == hovered)
                paintLitRing (g, p, bar, 0.0f);
        }

        g.setColour (p.border.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (middle), area.getX(), area.getRight());
    }

    // The step under a place across, or -1.
    int findStep (float x) const
    {
        for (int i = 0; i < (int) values.size(); ++i)
            if (getStepArea (i).getHorizontalRange().contains (x))
                return i;

        return -1;
    }

    void setAt (juce::Point<float> position)
    {
        const auto i = findStep (position.x);
        setHovered (i);

        if (i < 0)
            return;

        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto value = 1.0f - 2.0f * juce::jlimit (0.0f, 1.0f, (position.y - area.getY()) / std::max (1.0f, area.getHeight()));
        setStep (context, modulator, i, value);
        refresh();
    }

    void setHovered (int step)
    {
        if (step != hovered)
        {
            hovered = step;
            repaint();
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (findStep (e.position.x)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        gesture = std::make_unique<SongGesture> (context, "Change Steps");
        setAt (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override { setAt (e.position); }
    void mouseUp (const juce::MouseEvent&) override { gesture = nullptr; }

    ProjectContext& context;
    const ModulatorID modulator;
    std::vector<float> values;
    std::unique_ptr<SongGesture> gesture; // a drag's one undo step
    int hovered = -1;                     // the step under the pointer, lit
};

//==============================================================================
// A breakpoint envelope's points over its cycle: from its start level through each point to the last, the rest of the
// cycle holding there, each line bent by its curve as the envelope plays it. A point drags (the start up and down, the
// others along too, between their neighbours); the hollow handle in a line's middle drags up or down to bend it, as
// most envelope editors bend theirs (none found bends with the wheel, which scrolls the view around it: docs/design.md,
// B123); a double-click on a line straightens it. Shift drags finely, as on knobs. Each drag is one undo step.
struct ModulatorEditor::Breakpoints : public juce::Component
{
    Breakpoints (ProjectContext& c, ModulatorID m)
        : context (c),
          modulator (m)
    {
        setTitle ("Breakpoints");
        setDescription ("The envelope's points over its cycle. Drag a point, or the handle in a line's middle up or down to bend "
                        "the line; a double-click straightens it.");

        static constexpr const char* stages[] = { "Zero", "One", "Two", "Three", "Four" };

        for (int i = 0; i <= maxPoints; ++i)
        {
            const auto stage = juce::String ("stage") + stages[i];
            levels[(size_t) i] = getModulatorParameter (context, modulator, stage + "Value");

            if (i > 0)
            {
                times[(size_t) i] = getModulatorParameter (context, modulator, stage + "Time");
                curves[(size_t) i] = getModulatorParameter (context, modulator, stage + "Curve");
            }
        }

        count = getModulatorParameter (context, modulator, "numActivePoints");
    }

    static constexpr int maxPoints = 4;

    // What a press takes: a point, or the handle that bends the line ending at a point.
    struct Target
    {
        enum Kind
        {
            point,
            bend
        } kind = point;

        int index = 0;

        bool operator== (const Target&) const = default;
    };

    int getCount() const { return juce::jlimit (1, maxPoints, juce::roundToInt (count.getBaseValue())); }
    float getTime (int i) const { return i == 0 ? 0.0f : juce::jlimit (0.0f, 1.0f, times[(size_t) i].getBaseValue()); }
    float getLevel (int i) const { return juce::jlimit (0.0f, 1.0f, levels[(size_t) i].getBaseValue()); }
    float getCurve (int i) const { return curves[(size_t) i].getBaseValue(); }

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (GraphTint::handleRadius * 1.5f + 1.0f); }

    juce::Point<float> toScreen (juce::Point<float> timeAndLevel) const
    {
        const auto area = getArea();
        return { area.getX() + timeAndLevel.x * area.getWidth(), area.getBottom() - timeAndLevel.y * area.getHeight() };
    }

    juce::Point<float> getPointPosition (int i) const { return toScreen ({ getTime (i), getLevel (i) }); }

    // Where the line that ends at point i (from 1) is, a part of the way along it, as the breakpoint modulator bends it: its
    // curve, from -0.5 to 0.5, is half the ease of devices/dsp/Math.h (getEasedProgress()).
    juce::Point<float> getAlong (int i, float along) const
    {
        const auto ease = std::clamp ((double) getCurve (i) * 2.0, -1.0, 1.0);
        const auto progress = (float) devices::dsp::getEasedProgress (along, ease);
        return { getTime (i - 1) + (getTime (i) - getTime (i - 1)) * along,
                 getLevel (i - 1) + (getLevel (i) - getLevel (i - 1)) * progress };
    }

    // The middle of the line that ends at point i (from 1), where its handle is.
    juce::Point<float> getBendPosition (int i) const { return toScreen (getAlong (i, 0.5f)); }

    // How the curve of the line ending at point i changes to bend it up: lower for a rising line, higher for a falling one.
    float getUpward (int i) const { return getLevel (i) >= getLevel (i - 1) ? -1.0f : 1.0f; }

    // Repainted when what it draws moved.
    void refresh()
    {
        std::vector<float> now { count.getBaseValue() };

        for (int i = 0; i <= maxPoints; ++i)
            now.insert (now.end(), { getLevel (i), getTime (i), i > 0 ? getCurve (i) : 0.0f });

        if (now != shown)
        {
            shown = std::move (now);
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        g.setColour (p.surfaceRaised);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), cornerRadius);

        juce::Path line;
        line.startNewSubPath (getPointPosition (0));

        // Each line in short pieces, as it bends.
        constexpr int pieces = 24;

        for (int i = 1; i <= getCount(); ++i)
            for (int k = 1; k <= pieces; ++k)
                line.lineTo (toScreen (getAlong (i, (float) k / (float) pieces)));

        line.lineTo (toScreen ({ 1.0f, getLevel (getCount()) })); // the rest of the cycle, holding
        g.setColour (p.modulation);
        g.strokePath (line, juce::PathStrokeType (GraphTint::curveWidth));

        for (int i = 0; i <= getCount(); ++i)
        {
            paintHandle (g, p, { Target::point, i }, getPointPosition (i));

            if (i > 0)
                paintHandle (g, p, { Target::bend, i }, getBendPosition (i));
        }
    }

    // A point filled, a line's handle hollow; larger under the pointer and while dragged.
    void paintHandle (juce::Graphics& g, const Palette& p, Target target, juce::Point<float> at) const
    {
        const auto radius = target == hovered || target == dragged ? GraphTint::handleRadius * 1.5f : GraphTint::handleRadius;
        const auto dot = juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (at);
        g.setColour (p.surfaceRaised);
        g.fillEllipse (dot.expanded (1.0f));
        g.setColour (p.modulation);

        if (target.kind == Target::point)
            g.fillEllipse (dot);
        else
            g.drawEllipse (dot.reduced (GraphTint::handleOutline / 2.0f), GraphTint::handleOutline);
    }

    // The point or handle nearest the pointer, if one is near enough to take.
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

        for (int i = 0; i <= getCount(); ++i)
        {
            consider ({ Target::point, i }, getPointPosition (i));

            if (i > 0)
                consider ({ Target::bend, i }, getBendPosition (i));
        }

        return nearest;
    }

    // The line under a place across the picture, by the point it ends at; 0 past the last point, where the level holds.
    int findLine (float x) const
    {
        for (int i = 1; i <= getCount(); ++i)
            if (x <= getPointPosition (i).x)
                return i;

        return 0;
    }

    void setHovered (std::optional<Target> target)
    {
        if (target == hovered)
            return;

        hovered = target;

        // The start and the lines' handles move up and down only.
        if (! target.has_value())
            setMouseCursor (juce::MouseCursor::NormalCursor);
        else if (target->kind == Target::bend || target->index == 0)
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        else
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);

        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (findTarget (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered ({}); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto target = findTarget (e.position);

        if (! target.has_value() || e.mods.isPopupMenu())
            return;

        dragged = target;
        const auto i = target->index;

        if (target->kind == Target::bend)
        {
            startCurve = getCurve (i);
            curves[(size_t) i].beginGesture();
            return;
        }

        startLevel = getLevel (i);
        startTime = getTime (i);
        levels[(size_t) i].beginGesture(); // its gesture holds the step, which the time's joins (the start has none)
        times[(size_t) i].beginGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragged.has_value())
            return;

        const auto area = getArea();
        const auto fine = e.mods.isShiftDown() ? GraphTint::fineDragFactor : 1.0f;
        const auto up = (float) -e.getDistanceFromDragStartY() / std::max (1.0f, area.getHeight()) * fine;
        const auto i = dragged->index;

        if (dragged->kind == Target::bend)
        {
            const auto range = curves[(size_t) i].getRange();
            curves[(size_t) i].setValue (juce::jlimit (range.start, range.end, startCurve + getUpward (i) * up));
        }
        else
        {
            levels[(size_t) i].setValue (juce::jlimit (0.0f, 1.0f, startLevel + up));

            if (i > 0)
            {
                const auto along = (float) e.getDistanceFromDragStartX() / std::max (1.0f, area.getWidth()) * fine;
                const auto earliest = getTime (i - 1), latest = i < getCount() ? getTime (i + 1) : 1.0f;
                times[(size_t) i].setValue (juce::jlimit (earliest, std::max (earliest, latest), startTime + along));
            }
        }

        refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragged.has_value())
            return;

        const auto i = (size_t) dragged->index;

        for (auto* handle : { &curves[i], &times[i], &levels[i] }) // the level's last: its gesture holds the step
            handle->endGesture();

        dragged.reset();
        repaint();
    }

    // Straight again: the line under the pointer, or whose handle it is on. A double-click on a point itself does
    // nothing.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto target = findTarget (e.position);

        if (target.has_value() && target->kind == Target::point)
            return;

        if (const auto i = target.has_value() ? target->index : findLine (e.position.x); i > 0)
        {
            curves[(size_t) i].setValue (0.0f);
            refresh();
        }
    }

    ProjectContext& context;
    const ModulatorID modulator;
    std::array<ParameterHandle, maxPoints + 1> levels, times, curves; // the start's time and curve stay empty: it has none
    ParameterHandle count;
    std::vector<float> shown;
    std::optional<Target> hovered, dragged;
    float startLevel = 0.0f, startTime = 0.0f, startCurve = 0.0f;
};

//==============================================================================
ModulatorEditor::ModulatorEditor (ProjectContext& projectContext, ModulatorID id)
    : context (projectContext),
      modulator (id)
{
    // Clear, over the frosted glass of the pop-up it shows in, as the window's other pop-ups are.
    const auto info = phazite::getModulator (context, modulator);
    const auto name = info.has_value() ? info->name : juce::String ("Modulator");
    setTitle (name);
    title.setText (name, juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::title));
    addAndMakeVisible (title);

    // Its switch: off, it moves nothing, to hear the sound without it. A macro has none.
    if (info.has_value() && info->type != ModulatorType::macro)
    {
        power.setTooltip ("Turns " + name + " on or off, to hear the sound with and without it");
        power.setToggleState (info->isOn, juce::dontSendNotification);
        power.onClick = [this] { setModulatorOn (context, modulator, power.getToggleState()); };
        addAndMakeVisible (power);
    }

    // A macro is a knob; a modulator moves, which its output shows.
    if (info.has_value() && info->type != ModulatorType::macro)
    {
        scope = std::make_unique<Scope> (context, modulator);
        addAndMakeVisible (*scope);
    }

    if (info.has_value() && info->type == ModulatorType::steps)
    {
        steps = std::make_unique<Steps> (context, modulator);
        addAndMakeVisible (*steps);
    }

    if (info.has_value() && info->type == ModulatorType::breakpoints)
    {
        breakpoints = std::make_unique<Breakpoints> (context, modulator);
        addAndMakeVisible (*breakpoints);
    }

    for (const auto& parameter : getModulatorParameters (context, modulator))
    {
        auto* knob = knobs.add (new Knob (parameter.name, 0.0));
        auto* attachment = attachments.add (new ParameterAttachment (*knob));
        attachment->setParameter (getModulatorParameter (context, modulator, parameter.id));
        attachment->showDeviceText(); // the parameters' own texts: "Sine", "1/4"
        addAndMakeVisible (knob);

        auto* label = names.add (new juce::Label ({}, parameter.name));
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        label->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
        parameterIDs.push_back (parameter.id);
    }

    const auto rows = (knobs.size() + columns - 1) / columns;
    const auto graphs = (scope != nullptr ? graphHeight + Space::s : 0) + (steps != nullptr ? graphHeight + Space::s : 0)
                        + (breakpoints != nullptr ? graphHeight + Space::s : 0);
    setSize (columns * (knobSize + Space::s) + 2 * Space::m, titleHeight + graphs + rows * (knobSize + nameHeight) + 2 * Space::m);

    refresh();
    startTimerHz (refreshRateHz);
}

ModulatorEditor::~ModulatorEditor() = default;

Knob* ModulatorEditor::findKnob (const juce::String& parameterID) const
{
    for (size_t i = 0; i < parameterIDs.size(); ++i)
        if (parameterIDs[i] == parameterID)
            return knobs[(int) i];

    return nullptr;
}

void ModulatorEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::m);
    auto top = area.removeFromTop (titleHeight);
    power.setBounds (top.removeFromRight (titleHeight));
    title.setBounds (top);

    for (auto* graph : std::initializer_list<juce::Component*> { scope.get(), steps.get(), breakpoints.get() })
    {
        if (graph != nullptr)
        {
            graph->setBounds (area.removeFromTop (graphHeight));
            area.removeFromTop (Space::s);
        }
    }

    for (int i = 0; i < knobs.size(); ++i)
    {
        const auto column = i % columns, row = i / columns;
        const auto cell = juce::Rectangle<int> (area.getX() + column * (knobSize + Space::s), area.getY() + row * (knobSize + nameHeight),
                                                knobSize, knobSize + nameHeight);
        knobs[i]->setBounds (cell.withHeight (knobSize).reduced (Space::xs));
        names[i]->setBounds (cell.withTrimmedTop (knobSize));
    }
}

void ModulatorEditor::timerCallback()
{
    if (isShowing()) // nothing to show behind another tab of the Devices view
        refresh();
}

void ModulatorEditor::refresh()
{
    if (scope != nullptr)
        scope->refresh();

    if (steps != nullptr)
        steps->refresh();

    if (breakpoints != nullptr)
        breakpoints->refresh();

    for (auto* attachment : attachments)
        attachment->update();

    if (power.isVisible())
        power.setToggleState (isModulatorOn (context, modulator), juce::dontSendNotification);
}
} // namespace phazite::ui
