// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StereoExpanderEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/stereo/StereoExpander.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::StereoExpander;

    constexpr int maxDots = 1024;   // the newest pairs drawn, of each
    constexpr int meterHeight = 18; // the correlation's bar
    constexpr float dotSize = 1.6f;
    constexpr float inputAlpha = 0.25f; // what comes in, faint
    constexpr float quarterTurn = 0.78539816f;
} // namespace

class StereoExpanderEditor::Image : public juce::Component,
                                    private DisplayTicker::Display
{
public:
    explicit Image (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          width (device.getParameter ("width")),
          lefts ((size_t) maxDots),
          rights ((size_t) maxDots)
    {
        setTitle ("Stereo image");
        setDescription ("What it plays, each moment a dot: mono stands straight up, the sides out of step lie flat. Under it, how "
                        "alike the two sides are, from -1 to +1; below 0 the mono sum loses sound.");
    }

    ~Image() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    float getCorrelation() const noexcept { return correlation; }
    float getInputCorrelation() const noexcept { return inputCorrelation; }
    int getNumDots() const noexcept { return (int) outputs.size(); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        auto area = getLocalBounds().toFloat();
        const auto meter = area.removeFromBottom ((float) meterHeight);
        area.removeFromBottom ((float) Space::xs);
        graph::paintBackground (g, p, area);
        paintDisc (g, p, tint, area.reduced (4.0f));
        paintMeter (g, p, tint, meter);
    }

private:
    struct Dot
    {
        float x, y; // on the disc, from -1 to 1 across, 0 to 1 up
    };

    DeviceHandle device;
    ParameterHandle width;
    std::vector<float> lefts, rights;
    std::vector<Dot> outputs, inputs;
    float correlation = StereoExpander::noReading, inputCorrelation = StereoExpander::noReading;
    DrawnNumbers drawn;

    // A pair as a dot: turned an eighth of a turn, so that mono stands up; the lower half folded up, as a half disc.
    static Dot toDot (float l, float r)
    {
        auto x = (l - r) * 0.70710678f, y = (l + r) * 0.70710678f;

        if (y < 0.0f)
        {
            x = -x;
            y = -y;
        }

        return { juce::jlimit (-1.0f, 1.0f, x), juce::jlimit (0.0f, 1.0f, y) };
    }

    void pull (devices::StereoTap& tap, std::vector<Dot>& dots)
    {
        dots.clear();

        for (int n; (n = tap.pull (lefts.data(), rights.data(), maxDots)) > 0;)
        {
            dots.clear();

            // The newest pairs only: each pull replaces what the one before brought.
            for (int i = 0; i < n; ++i)
                dots.push_back (toDot (lefts[(size_t) i], rights[(size_t) i]));
        }
    }

    void tick() override
    {
        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            if (auto* tap = scope->getOutput())
                pull (*tap, outputs);

            if (auto* tap = scope->getInput())
                pull (*tap, inputs);
        }

        if (auto* expander = device.getProcessor<StereoExpander>())
        {
            correlation = expander->getOutputCorrelation().get();
            inputCorrelation = expander->getInputCorrelation().get();
        }

        std::vector<float> numbers { width.getValue(), correlation, inputCorrelation, (float) outputs.size() };

        for (const auto& dots : { &outputs, &inputs })
            for (const auto& dot : *dots)
                numbers.insert (numbers.end(), { dot.x, dot.y });

        if (drawn.changed (std::move (numbers)))
            repaint();
    }

    void paintDisc (juce::Graphics& g, const Palette& p, juce::Colour tint, juce::Rectangle<float> area)
    {
        const auto radius = std::min (area.getWidth() * 0.5f, area.getHeight());
        const juce::Point<float> centre { area.getCentreX(), area.getBottom() };

        // The half disc, the L and R lines (an eighth of a turn each way) and mono, straight up.
        juce::Path disc;
        disc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, -juce::MathConstants<float>::halfPi,
                            juce::MathConstants<float>::halfPi, true);
        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.strokePath (disc, juce::PathStrokeType (1.0f));

        for (const auto angle : { -quarterTurn, 0.0f, quarterTurn })
            g.drawLine ({ centre, centre.getPointOnCircumference (radius, angle) }, 1.0f);

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText ("L", juce::Rectangle<float> (14.0f, 14.0f).withCentre (centre.getPointOnCircumference (radius + 2.0f, -quarterTurn)),
                    juce::Justification::centred, false);
        g.drawText ("R", juce::Rectangle<float> (14.0f, 14.0f).withCentre (centre.getPointOnCircumference (radius + 2.0f, quarterTurn)),
                    juce::Justification::centred, false);

        // The fan: as wide as the width sets it (closed in mono, the L and R lines at 100 %, flat at 200 %).
        const auto opening = juce::jlimit (0.0f, 1.0f, width.getValue() * 0.5f) * juce::MathConstants<float>::halfPi;
        juce::Path fan;
        fan.addPieSegment (area.withSizeKeepingCentre (2.0f * radius, 2.0f * radius).withCentre (centre), -opening, opening, 0.0f);
        g.setColour (tint.withAlpha (GraphTint::spectrumFill));
        g.fillPath (fan);

        // What comes in, faint, then what goes out, scaled so the loudest reaches the rim.
        auto loudest = 0.05f;

        for (const auto* dots : { &inputs, &outputs })
            for (const auto& dot : *dots)
                loudest = std::max ({ loudest, std::abs (dot.x), dot.y });

        const auto draw = [&] (const std::vector<Dot>& dots, juce::Colour colour)
        {
            g.setColour (colour);

            for (const auto& dot : dots)
                g.fillRect (centre.x + dot.x / loudest * radius - dotSize * 0.5f, centre.y - dot.y / loudest * radius - dotSize * 0.5f,
                            dotSize, dotSize);
        };

        draw (inputs, p.textMuted.withAlpha (inputAlpha));
        draw (outputs, tint);

        graph::paintReadout (g, p, area, "Width " + juce::String (juce::roundToInt (width.getValue() * 100.0f)) + "%",
                             juce::Justification::topLeft);
    }

    void paintMeter (juce::Graphics& g, const Palette& p, juce::Colour tint, juce::Rectangle<float> area)
    {
        graph::paintBackground (g, p, area);
        const auto xFor = [&area] (float value) { return area.getX() + (value + 1.0f) * 0.5f * area.getWidth(); };

        // Below 0, red: what the mono sum loses.
        g.setColour (p.danger.withAlpha (GraphTint::partCurve));
        g.fillRect (area.withRight (xFor (0.0f)));
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.drawVerticalLine (juce::roundToInt (xFor (0.0f)), area.getY(), area.getBottom());

        if (inputCorrelation <= 1.0f)
        {
            g.setColour (p.textMuted);
            g.fillRect (juce::Rectangle<float> (2.0f, area.getHeight()).withCentre ({ xFor (inputCorrelation), area.getCentreY() }));
        }

        if (correlation <= 1.0f)
        {
            g.setColour (correlation < 0.0f ? p.danger : tint);
            g.fillRoundedRectangle (
                juce::Rectangle<float> (6.0f, area.getHeight() - 2.0f).withCentre ({ xFor (correlation), area.getCentreY() }), 2.0f);
        }

        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText ("-1", area.reduced (4.0f, 0.0f), juce::Justification::centredLeft, false);
        g.drawText ("+1", area.reduced (4.0f, 0.0f), juce::Justification::centredRight, false);
    }
};

//==============================================================================
StereoExpanderEditor::StereoExpanderEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Image> (device));
    image = &shown;

    const auto widthPart = makeControlsPart ("Width", { "width", "spread" }, 2, ParameterControls::Size::large);
    const auto rest = makeControlsPart ("Bass and Polarity", { "bassMono", "bassMonoFrequency", "gain", "invertLeft", "invertRight" }, 3,
                                        ParameterControls::Size::compact, { "Bass Mono", "Frequency", "Gain", "Flip Left", "Flip Right" });
    setParts ({ { { "Image", &shown, 240, 196, 120, true } }, { widthPart }, { rest } });
}

StereoExpanderEditor::~StereoExpanderEditor() = default;

juce::Component& StereoExpanderEditor::getImage() const noexcept
{
    return *image;
}

float StereoExpanderEditor::getShownCorrelation() const
{
    return image->getCorrelation();
}

float StereoExpanderEditor::getShownInputCorrelation() const
{
    return image->getInputCorrelation();
}

int StereoExpanderEditor::getNumDots() const
{
    return image->getNumDots();
}
} // namespace phazite::ui
