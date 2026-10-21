// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ReverbEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/reverb/Reverb.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Reverb;

    constexpr double lowestPitch = 50.0, highestPitch = 16000.0; // the map's frequencies, bottom to top
    constexpr int rows = 40;                                     // bands of pitch drawn
    constexpr float grabDistance = 10.0f;                        // pixels from a handle that a press takes it
    constexpr int historyTicks = 6 * DeviceTicker::ticksPerSecond;
} // namespace

class ReverbEditor::Graph : public juce::Component,
                            private DeviceTicker::Display
{
public:
    explicit Graph (const DeviceHandle& d)
        : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          hits (historyTicks, 0.0f)
    {
        setTitle ("Reverb decay");
        setDescription ("How the tail fades: time across, pitch up, each pitch lit until it has faded. Drag the wedge's end for the "
                        "decay, the gap at its start for the pre-delay, its top corner for the damping, and its floor for the low cut.");

        for (const auto& parameter : Reverb::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        readValues();
    }

    ~Graph() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    void readValues()
    {
        for (size_t i = 0; i < parameters.size(); ++i)
            if (parameters[i].isValid())
                values[i] = parameters[i].getValue();
    }

    double getSampleRate() const
    {
        auto* scope = device.getScope();
        return scope != nullptr ? scope->getSampleRate() : 48000.0;
    }

    double getPredelaySeconds() const { return values[Reverb::predelay] * 0.001; }

    double getShownSeconds() const
    {
        auto slowest = 0.0;

        for (int row = 0; row < rows; ++row)
            slowest = std::max (slowest, Reverb::getDecayAt (values, getPitch (row + 0.5f), getSampleRate()));

        return (getPredelaySeconds() + slowest) * 1.15;
    }

    juce::Rectangle<float> getMapArea() const
    {
        return getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom ((float) TextSize::small + 2.0f);
    }

    float xFor (double seconds) const
    {
        const auto area = getMapArea();
        return area.getX() + (float) (seconds / getShownSeconds()) * area.getWidth();
    }

    double secondsAt (float x) const
    {
        const auto area = getMapArea();
        return (double) ((x - area.getX()) / std::max (1.0f, area.getWidth())) * getShownSeconds();
    }

    // Pitches up the map, on a log scale.
    float yFor (double pitch) const
    {
        const auto area = getMapArea();
        return area.getBottom() - (float) (std::log (pitch / lowestPitch) / std::log (highestPitch / lowestPitch)) * area.getHeight();
    }

    double pitchAt (float y) const
    {
        const auto area = getMapArea();
        return lowestPitch * std::pow (highestPitch / lowestPitch, (double) ((area.getBottom() - y) / std::max (1.0f, area.getHeight())));
    }

    double getPitch (float row) const { return lowestPitch * std::pow (highestPitch / lowestPitch, (double) row / rows); }

    juce::Point<float> getDecayHandle() const
    {
        const auto bass = std::max (lowestPitch, (double) values[Reverb::lowCut]) * 1.5;
        return { xFor (getPredelaySeconds() + Reverb::getDecayAt (values, bass, getSampleRate())), yFor (bass) };
    }

    juce::Point<float> getPredelayHandle() const { return { xFor (getPredelaySeconds()), getMapArea().getCentreY() }; }

    juce::Point<float> getDampingHandle() const
    {
        const auto damping = (double) values[Reverb::damping];
        return { xFor (getPredelaySeconds() + Reverb::getDecayAt (values, damping, getSampleRate())), yFor (damping) };
    }

    float getLowCutY() const { return yFor (std::max (lowestPitch, (double) values[Reverb::lowCut])); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        const auto area = getMapArea();
        graph::paintBackground (g, p, getLocalBounds().toFloat());

        // Each band of pitch, lit from the pre-delay to where it has faded, fading as it goes.
        const auto start = xFor (getPredelaySeconds());
        const auto rowHeight = area.getHeight() / rows;

        for (int row = 0; row < rows; ++row)
        {
            const auto pitch = getPitch ((float) row + 0.5f);

            if (pitch < values[Reverb::lowCut])
                continue;

            const auto end = xFor (getPredelaySeconds() + Reverb::getDecayAt (values, pitch, getSampleRate()));
            const auto band = juce::Rectangle<float>::leftTopRightBottom (start, area.getBottom() - (float) (row + 1) * rowHeight, end,
                                                                          area.getBottom() - (float) row * rowHeight);
            g.setGradientFill (
                juce::ColourGradient (tint.withAlpha (0.55f), band.getX(), 0.0f, tint.withAlpha (0.05f), band.getRight(), 0.0f, false));
            g.fillRect (band.reduced (0.0f, 0.5f));
        }

        // What played, drifting in after the pre-delay and fading.
        for (int i = 0; i < hits.size(); ++i)
        {
            const auto age = (double) (hits.size() - 1 - i) / DeviceTicker::ticksPerSecond;
            const auto level = hits.get (i);
            const auto decay = Reverb::getDecayAt (values, 500.0, getSampleRate());

            if (level < 0.02f || age > decay)
                continue;

            const auto x = xFor (getPredelaySeconds() + age);
            const auto fade = (float) std::pow (10.0, -3.0 * age / decay);
            g.setColour (p.text.withAlpha (std::min (0.6f, level * fade)));
            g.fillRect (juce::Rectangle<float> (x, area.getY(), 2.0f, area.getHeight()));
        }

        // The edges that drag: the decay's end, the pre-delay's gap, the damping's corner and the low cut's floor.
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.drawVerticalLine (juce::roundToInt (start), area.getY(), area.getBottom());
        g.drawHorizontalLine (juce::roundToInt (getLowCutY()), area.getX(), area.getRight());

        for (const auto handle : { getDecayHandle(), getDampingHandle() })
        {
            g.setColour (tint);
            g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (handle));
        }

        // Its time scale, and what it says.
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        const auto seconds = getShownSeconds();
        const auto step = seconds > 8.0 ? 2.0 : seconds > 3.0 ? 1.0 : 0.5;

        for (auto t = step; t < seconds; t += step)
            g.drawText (juce::String (t, step < 1.0 ? 1 : 0) + " s",
                        juce::Rectangle<float> (xFor (t) - 20.0f, area.getBottom(), 40.0f, TextSize::small + 2.0f),
                        juce::Justification::centred, false);

        graph::paintReadout (g, p, area,
                             juce::String (values[Reverb::decay], 1) + " s  Pre-delay "
                                 + juce::String (juce::roundToInt (values[Reverb::predelay])) + " ms",
                             juce::Justification::topRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // The handle nearest, within reach: the decay's end, the damping's corner, the pre-delay's gap, or the low cut's floor.
        dragged = -1;
        const auto at = e.position;

        if (at.getDistanceFrom (getDecayHandle()) < grabDistance)
            dragged = Reverb::decay;
        else if (at.getDistanceFrom (getDampingHandle()) < grabDistance)
            dragged = Reverb::damping;
        else if (std::abs (at.x - getPredelayHandle().x) < grabDistance)
            dragged = Reverb::predelay;
        else if (std::abs (at.y - getLowCutY()) < grabDistance)
            dragged = Reverb::lowCut;
        else if (at.x > getPredelayHandle().x)
            dragged = Reverb::decay; // anywhere else in the tail: its length

        if (dragged >= 0)
        {
            parameters[(size_t) dragged].beginGesture();
            dragStartSeconds = getShownSeconds(); // the scale stays as it was while dragging
            mouseDrag (e);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged < 0)
            return;

        auto& handle = parameters[(size_t) dragged];
        const auto range = handle.getRange();
        const auto area = getMapArea();
        const auto seconds = (double) ((e.position.x - area.getX()) / std::max (1.0f, area.getWidth())) * dragStartSeconds;
        auto value = handle.getValue();

        if (dragged == Reverb::decay)
            value = (float) std::max (0.0, seconds - getPredelaySeconds());
        else if (dragged == Reverb::predelay)
            value = (float) (seconds * 1000.0);
        else
            value = (float) pitchAt (e.position.y);

        handle.setValue (juce::jlimit (range.start, range.end, value));
        readValues();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged >= 0)
            parameters[(size_t) dragged].endGesture();

        dragged = -1;
    }

private:
    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the reverb's order, for the map
    std::vector<float> values;
    graph::LevelHistory hits; // what came in, a peak a tick, as a gain
    int dragged = -1;
    double dragStartSeconds = 1.0;

    void tick() override
    {
        readValues();

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);
            hits.push (juce::Decibels::decibelsToGain (scope->getInputPeak().take(), devices::DeviceScope::silentDecibels));
        }

        repaint();
    }
};

//==============================================================================
ReverbEditor::ReverbEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Graph> (device));
    graph = &shown;

    const auto space = makeControlsPart ("Space", { "decay", "size" }, 2, ParameterControls::Size::large);
    const auto tone = makeControlsPart ("Tone", { "predelay", "damping", "lowCut", "width" }, 4);
    setParts ({ { { "Decay", &shown, 300, 196, 110, true } }, { space, tone } });
}

ReverbEditor::~ReverbEditor() = default;

juce::Component& ReverbEditor::getGraph() const noexcept
{
    return *graph;
}

double ReverbEditor::getShownSeconds() const
{
    return graph->getShownSeconds();
}

juce::Point<float> ReverbEditor::getDecayHandle() const
{
    return graph->getDecayHandle();
}

juce::Point<float> ReverbEditor::getPredelayHandle() const
{
    return graph->getPredelayHandle();
}

void ReverbEditor::refresh()
{
    graph->readValues();
    graph->repaint();
}
} // namespace phazite::ui
