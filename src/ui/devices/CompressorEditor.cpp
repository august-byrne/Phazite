// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CompressorEditor.h"
#include "DeviceTint.h"
#include "devices/dynamics/Compressor.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Compressor;

    constexpr float lowestLevel = -60.0f; // the graph's levels, in and out, go from here to 0 dB
    constexpr float meterRange = 24.0f;   // the meter shows the sound turned down by up to this many dB
    constexpr float meterFall = 0.5f;     // dB per refresh: 15 dB a second

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;

    const devices::ParameterInfo& getInfo (int index)
    {
        return Compressor::info.parameters[(size_t) index];
    }
} // namespace

//==============================================================================
class CompressorEditor::Graph : public juce::Component
{
public:
    explicit Graph (CompressorEditor& e)
        : editor (e)
    {
        setTitle ("Compressor curve");
        setDescription ("How loud each level comes out, with a dot at the level the compressor hears, and a meter of how far it "
                        "turns the sound down. Drag across the curve to move the threshold.");
    }

    juce::Rectangle<float> getCurveArea() const
    {
        return getLocalBounds().toFloat().withTrimmedRight ((float) (GraphTint::meterWidth + Space::s));
    }

    juce::Point<float> getPosition (float in, float out) const
    {
        const auto area = getCurveArea();
        return { area.getX() + (in - lowestLevel) / -lowestLevel * area.getWidth(),
                 area.getBottom() - (out - lowestLevel) / -lowestLevel * area.getHeight() };
    }

    float levelFor (float x) const
    {
        const auto area = getCurveArea();
        return lowestLevel + (x - area.getX()) / area.getWidth() * -lowestLevel;
    }

    // How loud a level comes out once the compressor has settled: turned down, then made up.
    float outputFor (float in) const { return in - Compressor::getReduction (editor.values, in) + editor.values[Compressor::makeup]; }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        const auto area = getCurveArea();
        g.setColour (palette.background);
        g.fillRoundedRectangle (area, cornerRadius);
        paintGrid (g, palette, area);

        // The curve, within the graph.
        juce::Path curve;

        for (auto x = area.getX(); x <= area.getRight(); x += 1.5f)
        {
            const auto point = getPosition (levelFor (x), outputFor (levelFor (x)));
            const auto clipped = point.withY (juce::jlimit (area.getY(), area.getBottom(), point.y));

            if (x <= area.getX())
                curve.startNewSubPath (clipped);
            else
                curve.lineTo (clipped);
        }

        g.setColour (palette.text);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        // The level the compressor hears, riding the curve.
        if (editor.shownLevel > lowestLevel)
        {
            const auto in = std::min (editor.shownLevel, 0.0f);
            const auto dot = getPosition (in, juce::jlimit (lowestLevel, 0.0f, outputFor (in)));
            g.setColour (getDeviceTint (*this));
            g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
        }

        paintMeter (g, palette);
    }

    // Dragging sideways moves the threshold by as much as the graph's levels move, as one undo step.
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! threshold.isInGesture())
        {
            threshold = editor.device.getParameter (getInfo (Compressor::threshold).id);
            threshold.beginGesture();
            startThreshold = threshold.getValue();
        }

        const auto moved = (float) e.getDistanceFromDragStartX() / getCurveArea().getWidth() * -lowestLevel;
        threshold.setValue (devices::constrain (getInfo (Compressor::threshold), startThreshold + moved));
        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override { threshold.endGesture(); }

private:
    CompressorEditor& editor;
    ParameterHandle threshold;
    float startThreshold = 0.0f;

    void paintGrid (juce::Graphics& g, const Palette& palette, juce::Rectangle<float> area)
    {
        g.setFont (juce::FontOptions (TextSize::small));

        for (auto level = -48.0f; level < 0.0f; level += 12.0f)
        {
            const auto at = getPosition (level, level);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (at.x), area.getY(), area.getBottom());
            g.drawHorizontalLine (juce::roundToInt (at.y), area.getX(), area.getRight());
            g.setColour (palette.textMuted);
            g.drawText (
                juce::String ((int) level),
                juce::Rectangle<float> (at.x + Space::xs, area.getBottom() - (float) labelHeight, (float) knobSize, (float) labelHeight),
                juce::Justification::centredLeft);
        }

        // Level in equals level out, and the threshold.
        g.setColour (palette.text.withAlpha (GraphTint::zeroLine));
        g.drawLine ({ getPosition (lowestLevel, lowestLevel), getPosition (0.0f, 0.0f) });
        g.setColour (getDeviceTint (*this).withAlpha (GraphTint::marker));
        g.drawVerticalLine (juce::roundToInt (getPosition (editor.values[Compressor::threshold], 0.0f).x), area.getY(), area.getBottom());
    }

    // How far the sound goes down, from the top, with the number of decibels beside it.
    void paintMeter (juce::Graphics& g, const Palette& palette)
    {
        const auto strip = getLocalBounds().toFloat().removeFromRight ((float) GraphTint::meterWidth);
        g.setColour (palette.background);
        g.fillRoundedRectangle (strip, cornerRadius);
        g.setColour (palette.warning);
        g.fillRect (strip.withHeight (strip.getHeight() * juce::jlimit (0.0f, 1.0f, editor.shownReduction / meterRange)));

        auto area = getCurveArea();
        g.setColour (palette.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (editor.shownReduction >= 0.05f ? "-" + juce::String (editor.shownReduction, 1) + " dB" : juce::String ("0 dB"),
                    area.removeFromTop ((float) labelHeight).reduced ((float) Space::xs, 0.0f), juce::Justification::centredRight);
    }
};

//==============================================================================
// The key's cuts, and Listen beside them: while it is on and the editor shows, the compressor plays its key, through its
// cuts, instead of the sound (Compressor::listenToKey(), which the refresh asks for again and again), to hear what makes it
// duck. Hidden, or with the editor gone, the sound comes back within half a second.
class CompressorEditor::KeyPart : public juce::Component
{
public:
    explicit KeyPart (ParameterControls& c)
        : controls (c)
    {
        listen.setTitle ("Listen");
        listen.setTooltip ("Plays what the compressor listens to, through its cuts, instead of the sound, to hear what makes it duck");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (controls);
        addAndMakeVisible (listen);
    }

    int getPreferredWidth() const { return controls.getPreferredWidth() + Space::xs + listenWidth; }

    void resized() override
    {
        auto area = getLocalBounds();
        controls.setBounds (area.removeFromLeft (controls.getPreferredWidth()));
        area.removeFromLeft (Space::xs);
        listen.setBounds (area.withSizeKeepingCentre (area.getWidth(), controlHeight));
    }

    ParameterControls& controls;
    juce::TextButton listen { "Listen" };

private:
    static constexpr int listenWidth = 64;
};

//==============================================================================
// The curve, then the history beside it.
class CompressorEditor::Activity : public juce::Component
{
public:
    explicit Activity (CompressorEditor& editor)
        : graph (editor),
          history ("Compressor history", lowestLevel, meterRange, 4 * DeviceTicker::ticksPerSecond),
          threshold (editor.device.getParameter (getInfo (Compressor::threshold).id))
    {
        setTitle ("Activity");
        history.setLine (&threshold, "Threshold");
        addAndMakeVisible (graph);
        addAndMakeVisible (history);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        graph.setBounds (area.removeFromLeft (std::min (area.getWidth() / 2, area.getHeight() + Space::s + GraphTint::meterWidth)));
        area.removeFromLeft (Space::s);
        history.setBounds (area);
    }

    Graph graph;
    LevelHistoryView history;
    ParameterHandle threshold; // the history's line
};

//==============================================================================
CompressorEditor::CompressorEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DeviceTicker::Display (static_cast<juce::Component&> (*this)),
      shownLevel (lowestLevel)
{
    for (const auto& parameter : Compressor::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    auto& shownActivity = keep (std::make_unique<Activity> (*this));
    activity = &shownActivity;

    // The meters' first readings would show what played before the editor opened.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        compressor->getLevelMeter().take();
        compressor->getReductionMeter().take();
    }

    const auto main = makeControlsPart ("Squeeze", { "threshold", "ratio" }, 2, ParameterControls::Size::large);
    const auto timing = makeControlsPart ("Timing", { "attack", "release" }, 2);
    const auto shape = makeControlsPart ("Shape", { "knee", "makeup" }, 2);
    auto& cuts = makeControls ({ "keyLowCut", "keyHighCut" }, 2, ParameterControls::Size::compact, "Key", { "Low Cut", "High Cut" });
    auto& shownKey = keep (std::make_unique<KeyPart> (cuts));
    keyPart = &shownKey;

    setParts ({ { { "Activity", &shownActivity, 340, 196, 110, true } },
                { main, timing },
                { shape, { "Key", &shownKey, shownKey.getPreferredWidth(), cuts.getPreferredHeight(), cuts.getPreferredHeight() } } });
}

CompressorEditor::~CompressorEditor()
{
    if (auto* scope = device.getScope())
        scope->setListening (false);
}

juce::Component& CompressorEditor::getGraph()
{
    return activity->graph;
}

LevelHistoryView& CompressorEditor::getHistory()
{
    return activity->history;
}

juce::Button& CompressorEditor::getListenButton()
{
    return keyPart->listen;
}

juce::Point<float> CompressorEditor::getGraphPosition (float inputDecibels, float outputDecibels) const
{
    return activity->graph.getPosition (inputDecibels, outputDecibels);
}

void CompressorEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    // The meters jump up and fall back smoothly; the history takes each reading as it comes, with what goes out.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        const auto level = compressor->getLevelMeter().take(), reduction = compressor->getReductionMeter().take();
        shownLevel = std::max (level, shownLevel - meterFall);
        shownReduction = std::max (reduction, shownReduction - meterFall);

        auto output = devices::DeviceScope::silentDecibels;

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);
            output = scope->getOutputPeak().take();
        }

        activity->history.push (level, output, reduction);

        // Listen: the key, for as long as the editor shows and asks.
        if (keyPart->listen.getToggleState())
            compressor->listenToKey();
    }

    activity->graph.repaint();
}
} // namespace phazite::ui
