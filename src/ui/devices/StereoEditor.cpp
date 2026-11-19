// SPDX-License-Identifier: AGPL-3.0-or-later
#include "StereoEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/stereo/Stereo.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Stereo;

    constexpr int maxDots = 1024;   // the newest pairs drawn, of each
    constexpr int meterHeight = 18; // the correlation's bar
    constexpr float dotSize = 1.6f;
    constexpr float inputAlpha = 0.25f; // what comes in, faint
    constexpr float quarterTurn = 0.78539816f;
} // namespace

class StereoEditor::Image : public juce::Component,
                            public juce::TooltipClient,
                            private DisplayTicker::Display
{
public:
    explicit Image (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          width (device.getParameter ("width")),
          balance (device.getParameter ("balance")),
          mono (device.getParameter ("mono")),
          swap (device.getParameter ("swap")),
          lefts ((size_t) maxDots),
          rights ((size_t) maxDots)
    {
        setTitle ("Stereo image");
        setDescription ("What it plays, each moment a dot: mono stands straight up, the sides out of step lie flat, and a fan as "
                        "wide as its width: drag across the disc to open or close it, or turn the wheel. Under it, how alike the "
                        "two sides are, from -1 to +1; below 0 the mono sum loses sound.");
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        drag.onCancelled = [this] { repaint(); };

        // One handle, all of it: the width, which the wheel turns too.
        drag.findHandle = [this] (juce::Point<float> at) { return graph::Handle ({ &width }, at, &width); };
    }

    ~Image() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    // The fan as the device plays its values: open as wide as the Width (closed by Mono), each half as long as the Balance
    // leaves of its side, and the sides' names swapped with Swap Sides.
    StereoEditor::Fan getFan() const
    {
        const auto opening =
            mono.getValue() > 0.5f ? 0.0f : juce::jlimit (0.0f, 1.0f, width.getValue() * 0.5f) * juce::MathConstants<float>::halfPi;
        return { opening, Stereo::getBalanceGain (balance.getValue(), 0), Stereo::getBalanceGain (balance.getValue(), 1),
                 swap.getValue() > 0.5f };
    }

    float getCorrelation() const noexcept { return correlation; }
    float getInputCorrelation() const noexcept { return inputCorrelation; }
    int getNumDots() const noexcept { return (int) outputs.size(); }

    float getDotsAcross() const
    {
        auto sum = 0.0f;

        for (const auto& dot : outputs)
            sum += dot.x;

        return outputs.empty() ? 0.0f : sum / (float) outputs.size();
    }

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

    // The fan's edge turns as far as the pointer turns around the disc's centre (its angle from straight up is as wide as the
    // fan opens), from the width set by hand, as one undo step; a press changes nothing (graph::HandleDrag). The wheel opens
    // or closes it a step at a time.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        drag.take ({ &width });
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        // Where the pointer would be had it gone a fifth as far while Shift was held.
        const auto turned = getAngle (e.mouseDownPosition + drag.getMoved (e)) - getAngle (e.mouseDownPosition);
        drag.set (0, juce::exactlyEqual (turned, 0.0f) ? drag.getStart (0)
                                                       : drag.getStart (0) + turned / juce::MathConstants<float>::halfPi * 2.0f);
        repaint();
    }

    // A place's angle from straight up over the disc's centre, either way: 0 straight up, a quarter of a turn flat.
    float getAngle (juce::Point<float> at) const
    {
        const auto centre = getDisc().first;
        return std::abs (std::atan2 (at.x - centre.x, std::max (0.0f, centre.y - at.y)));
    }

    void mouseUp (const juce::MouseEvent&) override { drag.end(); }
    juce::String getTooltip() override { return drag.getTooltip(); }

    // Anywhere on it, the wheel opens or closes the fan, in finer steps with Shift.
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& details) override
    {
        const auto fine = graph::getFineFactor (e.mods);

        if (const auto step = details.deltaY > 0.0f   ? 0.05f * fine
                              : details.deltaY < 0.0f ? -0.05f * fine
                                                      : 0.0f;
            ! juce::approximatelyEqual (step, 0.0f))
        {
            const auto range = width.getRange();
            wheel.turn (width, juce::jlimit (range.start, range.end, width.getBaseValue() + step));
        }
    }

    // The half disc's centre, at its bottom, and its radius, where paintDisc() draws it.
    std::pair<juce::Point<float>, float> getDisc() const
    {
        auto area = getLocalBounds().toFloat();
        area.removeFromBottom ((float) meterHeight + (float) Space::xs);
        area = area.reduced (4.0f);
        return { { area.getCentreX(), area.getBottom() }, std::min (area.getWidth() * 0.5f, area.getHeight()) };
    }

private:
    struct Dot
    {
        float x, y; // on the disc, from -1 to 1 across, 0 to 1 up
    };

    DeviceHandle device;
    ParameterHandle width, balance, mono, swap;
    graph::HandleDrag drag { *this };   // after the handle it moves
    graph::WheelGesture wheel { drag }; // and turns
    std::vector<float> lefts, rights;
    std::vector<Dot> outputs, inputs;
    float correlation = Stereo::noReading, inputCorrelation = Stereo::noReading;
    DrawnNumbers drawn;

    // A pair as a dot: turned an eighth of a turn, so that mono stands up, the left alone lies along the L line (up and to
    // the left) and the right alone along the R line; the lower half folded up, as a half disc.
    static Dot toDot (float l, float r)
    {
        auto x = (r - l) * 0.70710678f, y = (l + r) * 0.70710678f;

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

    // Shown again: the dots of the sound now, not of what played while nobody looked.
    void shownAgain() override
    {
        if (auto* scope = device.getScope())
            scope->drain();

        outputs.clear();
        inputs.clear();
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

        if (auto* stereo = device.getProcessor<Stereo>())
        {
            correlation = stereo->getOutputCorrelation().get();
            inputCorrelation = stereo->getInputCorrelation().get();
        }

        std::vector<float> numbers { width.getValue(), balance.getValue(), mono.getValue(),       swap.getValue(),
                                     correlation,      inputCorrelation,   (float) outputs.size() };

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

        // The sides' names, which change places while Swap Sides puts each side's sound on the other.
        const auto fanShown = getFan();
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (fanShown.swapped ? "R" : "L",
                    juce::Rectangle<float> (14.0f, 14.0f).withCentre (centre.getPointOnCircumference (radius + 2.0f, -quarterTurn)),
                    juce::Justification::centred, false);
        g.drawText (fanShown.swapped ? "L" : "R",
                    juce::Rectangle<float> (14.0f, 14.0f).withCentre (centre.getPointOnCircumference (radius + 2.0f, quarterTurn)),
                    juce::Justification::centred, false);

        // The fan: as wide as the width sets it (the L and R lines at 100 %, flat at 200 %), a line straight up with Mono on;
        // each half as long as the Balance leaves of its side, the line as long as it leaves of the middle.
        const auto circle = [&centre] (float length) { return juce::Rectangle<float> (2.0f * length, 2.0f * length).withCentre (centre); };
        g.setColour (tint.withAlpha (GraphTint::spectrumFill));

        if (fanShown.opening > 0.0f)
        {
            juce::Path fan;
            fan.addPieSegment (circle (radius * fanShown.leftLength), -fanShown.opening, 0.0f, 0.0f);
            fan.addPieSegment (circle (radius * fanShown.rightLength), 0.0f, fanShown.opening, 0.0f);
            g.fillPath (fan);
        }
        else
        {
            const auto length = radius * 0.5f * (fanShown.leftLength + fanShown.rightLength);
            g.drawLine ({ centre, centre.translated (0.0f, -length) }, GraphTint::curveWidth);
        }

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
StereoEditor::StereoEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Image> (device));
    image = &shown;

    // In the order the sound goes through them: four side by side are wider than the dock, so Bass under Width and Output
    // under Sides.
    const auto widthPart = makeControlsPart ("Width", { "width", "spread" }, 2, ParameterControls::Size::large);
    auto bass = makeControlsPart ("Bass", { "bassMonoFrequency" }, 1, ParameterControls::Size::compact, { "Frequency" });
    bass.titleSwitch = &makePartSwitch ("bassMono", *bass.content); // Bass Mono switches the part, at its title
    const auto sides = makeControlsPart ("Sides", { "balance", "mono", "swap", "invertLeft", "invertRight" }, 3,
                                         ParameterControls::Size::compact, { "Balance", "Mono", "Swap", "Flip Left", "Flip Right" });
    const auto output = makeControlsPart ("Output", { "dcFilter", "gain" }, 2, ParameterControls::Size::compact, { "DC Filter", "Gain" });
    setParts ({ { { "Image", &shown, 240, 196, 120, true, 160 } }, { widthPart, bass }, { sides, output } });
}

StereoEditor::~StereoEditor() = default;

juce::Component& StereoEditor::getImage() const noexcept
{
    return *image;
}

float StereoEditor::getShownCorrelation() const
{
    return image->getCorrelation();
}

float StereoEditor::getShownInputCorrelation() const
{
    return image->getInputCorrelation();
}

int StereoEditor::getNumDots() const
{
    return image->getNumDots();
}

float StereoEditor::getDotsAcross() const
{
    return image->getDotsAcross();
}

juce::Point<float> StereoEditor::getDiscCentre() const
{
    return image->getDisc().first;
}

StereoEditor::Fan StereoEditor::getShownFan() const
{
    return image->getFan();
}
} // namespace phazite::ui
