// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulatorEditor.h"
#include "model/commands/SongGesture.h"
#include "ui/theme/LookAndFeel.h"

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
// step it passes, as one undo step.
struct ModulatorEditor::Steps : public juce::Component
{
    Steps (ProjectContext& c, ModulatorID m)
        : context (c),
          modulator (m)
    {
        setTitle ("Steps");
        setDescription ("Click or drag to set the steps.");
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
            g.setColour (p.modulation);
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (step.getX(), std::min (y, middle), step.getRight(),
                                                                    std::max (y, middle) + 1.0f));
        }

        g.setColour (p.border.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (middle), area.getX(), area.getRight());
    }

    void setAt (juce::Point<float> position)
    {
        const auto area = getLocalBounds().toFloat().reduced (1.0f);

        for (int i = 0; i < (int) values.size(); ++i)
        {
            if (getStepArea (i).getHorizontalRange().contains (position.x))
            {
                const auto value = 1.0f - 2.0f * juce::jlimit (0.0f, 1.0f, (position.y - area.getY()) / std::max (1.0f, area.getHeight()));
                setStep (context, modulator, i, value);
                refresh();
                return;
            }
        }
    }

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
};

//==============================================================================
ModulatorEditor::ModulatorEditor (ProjectContext& projectContext, ModulatorID id)
    : context (projectContext),
      modulator (id)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    const auto info = phazite::getModulator (context, modulator);
    const auto name = info.has_value() ? info->name : juce::String ("Modulator");
    setTitle (name);
    title.setText (name, juce::dontSendNotification);
    title.setFont (juce::FontOptions (TextSize::title));
    addAndMakeVisible (title);

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

    for (const auto& parameter : getModulatorParameters (context, modulator))
    {
        auto* knob = knobs.add (new Knob (parameter.name, 0.0));
        auto* attachment = attachments.add (new ParameterAttachment (*knob));
        attachment->setParameter (getModulatorParameter (context, modulator, parameter.id));
        attachment->showDeviceText(); // Tracktion's own texts: "Sine", "1/4"
        addAndMakeVisible (knob);

        auto* label = names.add (new juce::Label ({}, parameter.name));
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        label->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
        parameterIDs.push_back (parameter.id);
    }

    const auto rows = (knobs.size() + columns - 1) / columns;
    const auto graphs = (scope != nullptr ? graphHeight + Space::s : 0) + (steps != nullptr ? graphHeight + Space::s : 0);
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

void ModulatorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void ModulatorEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::m);
    title.setBounds (area.removeFromTop (titleHeight));

    for (auto* graph : std::initializer_list<juce::Component*> { scope.get(), steps.get() })
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

    for (auto* attachment : attachments)
        attachment->update();
}
} // namespace phazite::ui
