// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SaturatorEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/saturation/Saturator.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Saturator;

    constexpr int scopeLength = 2048;   // samples kept for the wave
    constexpr float reachFall = 0.05f;  // of the reach, per refresh, as it falls back
    constexpr float driveRange = 36.0f; // decibels a drag the curve's height turns the drive

    const devices::ParameterInfo& getInfo (int index)
    {
        return Saturator::info.parameters[(size_t) index];
    }

    // The curve, for an input from -extent to extent, driven, as a path across an area.
    juce::Path makeShape (Saturator::Shape shape, float driveGain, juce::Rectangle<float> area, float extent)
    {
        return graph::makeCurve (area,
                                 [&] (float x)
                                 {
                                     const auto in = ((x - area.getX()) / area.getWidth() * 2.0f - 1.0f) * extent;
                                     const auto out = juce::jlimit (-1.2f, 1.2f, Saturator::shapeSample (shape, in * driveGain));
                                     return area.getCentreY() - out / 1.2f * area.getHeight() * 0.5f;
                                 });
    }
} // namespace

//==============================================================================
class SaturatorEditor::Display : public juce::Component,
                                 private DeviceTicker::Display
{
public:
    explicit Display (const DeviceHandle& d)
        : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          drive (device.getParameter (getInfo (Saturator::drive).id)),
          shape (device.getParameter (getInfo (Saturator::shape).id)),
          inputs ((size_t) scopeLength, 0.0f),
          outputs ((size_t) scopeLength, 0.0f),
          lefts ((size_t) scopeLength),
          rights ((size_t) scopeLength)
    {
        setTitle ("Saturation");
        setDescription ("The curve the sound is driven into, lit as far as the sound reaches, and the wave coming in and going out. "
                        "Drag up or down to turn the drive.");
    }

    ~Display() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    float getReach() const noexcept { return reach; }

    juce::Rectangle<float> getCurveArea() const
    {
        const auto area = getLocalBounds().toFloat();
        return area.withWidth (std::min (area.getHeight(), area.getWidth() * 0.5f));
    }

    juce::Rectangle<float> getWaveArea() const { return getLocalBounds().toFloat().withTrimmedLeft (getCurveArea().getWidth() + Space::s); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        paintCurve (g, p, tint);
        paintWave (g, p, tint);
    }

    // A drag up or down turns the drive, as one undo step.
    void mouseDown (const juce::MouseEvent&) override
    {
        drive.beginGesture();
        startDrive = drive.getValue();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto range = drive.getRange();
        const auto moved = -(float) e.getDistanceFromDragStartY() / std::max (1.0f, (float) getHeight()) * driveRange;
        drive.setValue (juce::jlimit (range.start, range.end, startDrive + moved));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { drive.endGesture(); }

private:
    DeviceHandle device;
    ParameterHandle drive, shape;
    float startDrive = 0.0f, reach = 0.0f;
    std::vector<float> inputs, outputs; // the newest samples, oldest first
    std::vector<float> lefts, rights;   // pulled from the taps

    Saturator::Shape getShape() const { return (Saturator::Shape) juce::roundToInt (shape.getValue()); }
    float getDriveGain() const { return juce::Decibels::decibelsToGain (drive.getValue()); }

    // Takes a tap's newest samples into a store of the last ones, both channels averaged.
    void pull (devices::StereoTap& tap, std::vector<float>& store)
    {
        for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
        {
            const auto kept = std::min (n, scopeLength);
            std::move (store.begin() + kept, store.end(), store.begin());

            for (int i = 0; i < kept; ++i)
                store[(size_t) (scopeLength - kept + i)] = 0.5f * (lefts[(size_t) (n - kept + i)] + rights[(size_t) (n - kept + i)]);
        }
    }

    void tick() override
    {
        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            if (auto* in = scope->getInput())
                pull (*in, inputs);

            if (auto* out = scope->getOutput())
                pull (*out, outputs);

            // How far the loudest coming in reaches into the curve once driven: up at once, falling back slowly.
            const auto peak = juce::Decibels::decibelsToGain (scope->getInputPeak().take(), devices::DeviceScope::silentDecibels);
            reach = std::max (peak * getDriveGain(), reach * (1.0f - reachFall));
        }

        repaint();
    }

    void paintCurve (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getCurveArea();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::xs);

        // Its axes, and in equals out.
        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());
        g.drawVerticalLine (juce::roundToInt (inner.getCentreX()), inner.getY(), inner.getBottom());

        // The curve for an input up to full scale, then lit as far as the sound reaches.
        constexpr float extent = 1.0f;
        const auto driveGain = getDriveGain();
        g.setColour (tint.withAlpha (GraphTint::partOff));
        g.strokePath (makeShape (getShape(), driveGain, inner, extent), juce::PathStrokeType (GraphTint::curveWidth));

        if (const auto lit = juce::jlimit (0.0f, extent, reach / std::max (driveGain, 1.0e-3f)); lit > 0.01f)
        {
            g.saveState();
            g.reduceClipRegion (inner.withSizeKeepingCentre (inner.getWidth() * lit / extent, inner.getHeight()).toNearestInt());
            g.setColour (tint);
            g.strokePath (makeShape (getShape(), driveGain, inner, extent), juce::PathStrokeType (GraphTint::curveWidth + 1.0f));
            g.restoreState();
        }

        graph::paintReadout (g, p, area, "Drive " + graph::formatDecibels (drive.getValue()), juce::Justification::topLeft);
    }

    void paintWave (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getWaveArea();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::xs);
        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());

        // About 10 ms, from where the wave coming in last rose through zero, so that it stands still.
        const auto shown = std::min (
            scopeLength / 2, juce::roundToInt (0.01 * (device.getScope() != nullptr ? device.getScope()->getSampleRate() : 48000.0)));
        auto start = scopeLength - shown;

        for (int i = scopeLength - shown - 1; i > 0; --i)
        {
            if (inputs[(size_t) i - 1] < 0.0f && inputs[(size_t) i] >= 0.0f)
            {
                start = i;
                break;
            }
        }

        // What goes out comes later by the saturator's latency.
        auto* saturator = device.getProcessor<Saturator>();
        const auto latency = saturator != nullptr ? saturator->getLatencySamples() : 0;

        const auto trace = [&inner, shown] (const std::vector<float>& samples, int from)
        {
            juce::Path path;

            for (int i = 0; i < shown; ++i)
            {
                const auto index = juce::jlimit (0, scopeLength - 1, from + i);
                const juce::Point<float> point {
                    inner.getX() + (float) i / (float) (shown - 1) * inner.getWidth(),
                    inner.getCentreY() - juce::jlimit (-1.2f, 1.2f, samples[(size_t) index]) / 1.2f * inner.getHeight() * 0.5f
                };

                if (i == 0)
                    path.startNewSubPath (point);
                else
                    path.lineTo (point);
            }

            return path;
        };

        g.setColour (p.textMuted.withAlpha (GraphTint::marker));
        g.strokePath (trace (inputs, start), juce::PathStrokeType (1.0f));
        g.setColour (tint);
        g.strokePath (trace (outputs, std::min (start + latency, scopeLength - shown)), juce::PathStrokeType (GraphTint::curveWidth));
    }
};

//==============================================================================
// One button for each shape, drawn with its curve: the shape chosen lit.
class SaturatorEditor::ShapeButtons : public juce::Component,
                                      private DeviceTicker::Display
{
public:
    explicit ShapeButtons (const DeviceHandle& device)
        : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
          shape (device.getParameter (getInfo (Saturator::shape).id))
    {
        setTitle ("Shape");
        const auto& choices = getInfo (Saturator::shape).choices;

        for (size_t i = 0; i < choices.size(); ++i)
        {
            auto* button = buttons.add (new ShapeButton (juce::String (choices[i].data(), choices[i].size()), (Saturator::Shape) i));
            button->setRadioGroupId (1);
            button->setClickingTogglesState (false);
            button->onClick = [this, i]
            {
                shape.setValue ((float) i);
                tick();
            };
            addAndMakeVisible (button);
        }

        tick();
    }

    juce::Button& getButton (int index) const { return *buttons[index]; }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto width = area.getWidth() / std::max (1, buttons.size());

        for (auto* button : buttons)
            button->setBounds (area.removeFromLeft (width).reduced (1, 0));
    }

    void tick() override
    {
        const auto chosen = juce::roundToInt (shape.getValue());

        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == chosen, juce::dontSendNotification);
    }

private:
    struct ShapeButton : public juce::Button
    {
        ShapeButton (const juce::String& name, Saturator::Shape s)
            : juce::Button (name),
              curve (s)
        {
            setTitle (name);
            setTooltip (name + ": " + describe (s));
        }

        static juce::String describe (Saturator::Shape s)
        {
            switch (s)
            {
                case Saturator::Shape::tape:
                    return "rounds off early and gently, warm and wide";
                case Saturator::Shape::tube:
                    return "lopsided, adding even harmonics: rich";
                case Saturator::Shape::hard:
                    return "clips the tops off: gritty";
                case Saturator::Shape::fold:
                    return "folds the tops back: a synthetic edge";
                case Saturator::Shape::soft:
                default:
                    return "rounds the tops off smoothly";
            }
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto& p = getPalette (*this);
            const auto tint = getDeviceTint (*this);
            const auto area = getLocalBounds().toFloat();
            g.setColour (getToggleState() ? tint.withAlpha (GraphTint::partCurve) : p.surfaceRaised.brighter (highlighted ? 0.1f : 0.0f));
            g.fillRoundedRectangle (area, cornerRadius);

            if (getToggleState() || down)
            {
                g.setColour (tint);
                g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);
            }

            auto inner = area.reduced (4.0f);
            const auto label = inner.removeFromBottom (TextSize::small);
            g.setColour (getToggleState() ? tint : p.textMuted);
            g.strokePath (makeShape (curve, 2.5f, inner, 1.0f), juce::PathStrokeType (1.5f));
            g.setColour (p.text);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (getName(), label, juce::Justification::centred, false);

            if (hasKeyboardFocus (false))
            {
                g.setColour (p.accent);
                g.drawRoundedRectangle (area.reduced (1.0f), cornerRadius, 1.5f);
            }
        }

        const Saturator::Shape curve;
    };

    ParameterHandle shape;
    juce::OwnedArray<ShapeButton> buttons;
};

//==============================================================================
SaturatorEditor::SaturatorEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Display> (device));
    display = &shown;
    auto& buttons = keep (std::make_unique<ShapeButtons> (device));
    shapes = &buttons;
    addControl ("shape", buttons);

    const auto driveControl = makeControlsPart ("Drive", { "drive" }, 1, ParameterControls::Size::large);
    const auto colour = makeControlsPart ("Colour", { "tone", "output", "autoGain" }, 3);
    constexpr int displayWidth = 300;

    setParts ({ { { "Saturation", &shown, displayWidth, 140, 80, true }, { "Shape", &buttons, displayWidth, 48, 48 } },
                { driveControl, colour } });
}

SaturatorEditor::~SaturatorEditor() = default;

juce::Component& SaturatorEditor::getCurve() const noexcept
{
    return *display;
}

juce::Button& SaturatorEditor::getShapeButton (int shape) const
{
    return shapes->getButton (shape);
}

float SaturatorEditor::getShownReach() const
{
    return display->getReach();
}
} // namespace phazite::ui
