// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceGraph.h"
#include "ui/components/DisplayTicker.h"

#include <cmath>

namespace phazite::ui::graph
{
float FrequencyAxis::xFor (double frequency) const
{
    const auto proportion = std::log (std::max (frequency, 1.0) / low) / std::log (high / low);
    return area.getX() + (float) proportion * area.getWidth();
}

double FrequencyAxis::frequencyAt (float x) const
{
    const auto proportion = (double) ((x - area.getX()) / std::max (1.0f, area.getWidth()));
    return low * std::pow (high / low, proportion);
}

float LevelAxis::yFor (float decibels) const
{
    return area.getBottom() - (decibels - low) / (high - low) * area.getHeight();
}

float LevelAxis::levelAt (float y) const
{
    return low + (area.getBottom() - y) / std::max (1.0f, area.getHeight()) * (high - low);
}

//==============================================================================
void paintBackground (juce::Graphics& g, const Palette& p, juce::Rectangle<float> area)
{
    g.setColour (p.background);
    g.fillRoundedRectangle (area, cornerRadius);
}

void paintFrequencyGrid (juce::Graphics& g, const Palette& p, const FrequencyAxis& axis, bool withNumbers)
{
    const auto& area = axis.area;

    for (double decade = 10.0; decade < axis.high; decade *= 10.0)
    {
        for (int step = 1; step < 10; ++step)
        {
            const auto frequency = decade * step;

            if (frequency <= axis.low || frequency >= axis.high)
                continue;

            const auto x = axis.xFor (frequency);
            g.setColour (p.text.withAlpha (step == 1 ? GraphTint::grid * 2.0f : GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());

            // Its number beside it, where all of it fits in the graph: in a narrow one, the last decade's would run past its
            // edge ("10 k" half drawn).
            if (withNumbers && step == 1 && frequency >= 100.0)
            {
                const juce::Font font { juce::FontOptions (TextSize::small) };
                const auto text = formatFrequency (frequency);
                const auto width = juce::GlyphArrangement::getStringWidth (font, text);

                if (x + 2.0f + width > area.getRight())
                    continue;

                g.setColour (p.textMuted);
                g.setFont (font);
                g.drawText (text,
                            juce::Rectangle<float> (x + 2.0f, area.getBottom() - TextSize::small - 2.0f, width + 1.0f, TextSize::small),
                            juce::Justification::centredLeft, false);
            }
        }
    }
}

void paintLevelGrid (juce::Graphics& g, const Palette& p, const LevelAxis& axis, float step, bool withNumbers)
{
    const auto& area = axis.area;

    for (auto level = std::ceil (axis.low / step) * step; level <= axis.high; level += step)
    {
        const auto y = axis.yFor (level);
        g.setColour (p.text.withAlpha (juce::approximatelyEqual (level, 0.0f) ? GraphTint::zeroLine : GraphTint::grid));
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());

        if (withNumbers && y - TextSize::small > area.getY() && y < area.getBottom() - 2.0f)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (juce::String (juce::roundToInt (level)),
                        juce::Rectangle<float> (area.getX() + 2.0f, y - TextSize::small, 40.0f, TextSize::small),
                        juce::Justification::centredLeft, false);
        }
    }
}

juce::Path makeCurve (juce::Rectangle<float> area, const std::function<float (float)>& yAt, float step)
{
    juce::Path curve;

    for (auto x = area.getX(); x <= area.getRight() + step * 0.5f; x += step)
    {
        const auto shownX = std::min (x, area.getRight());
        const juce::Point<float> point { shownX, juce::jlimit (area.getY(), area.getBottom(), yAt (shownX)) };

        // Not Path::isEmpty(), which holds while a path has nothing but its start, so that every point began a path of its own.
        if (juce::exactlyEqual (x, area.getX()))
            curve.startNewSubPath (point);
        else
            curve.lineTo (point);
    }

    return curve;
}

void paintReadout (juce::Graphics& g, const Palette& p, juce::Rectangle<float> area, const juce::String& text, juce::Justification where)
{
    if (text.isEmpty())
        return;

    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (text, area.reduced ((float) Space::xs), where, true);
}

void paintReadouts (juce::Graphics& g, const Palette& p, juce::Rectangle<float> area, const juce::String& left, const juce::String& right)
{
    const juce::Font font { juce::FontOptions (TextSize::small) };
    const auto both =
        juce::GlyphArrangement::getStringWidth (font, left) + juce::GlyphArrangement::getStringWidth (font, right) + (float) Space::m;
    paintReadout (g, p, area, left, juce::Justification::topLeft);

    if (left.isEmpty() || both <= area.reduced ((float) Space::xs).getWidth())
        paintReadout (g, p, area, right, juce::Justification::topRight);
}

juce::String formatFrequency (double hertz)
{
    if (hertz >= 1000.0)
        return juce::String (hertz / 1000.0, hertz >= 10000.0 ? 1 : 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + " kHz";

    return juce::String (juce::roundToInt (hertz)) + " Hz";
}

juce::String formatDecibels (float decibels)
{
    const auto rounded = std::abs (decibels) < 10.0f ? juce::String (decibels, 1) : juce::String (juce::roundToInt (decibels));
    return (decibels > 0.0f ? "+" : "") + rounded + " dB";
}

//==============================================================================
SpectrumTrace::SpectrumTrace()
    : lefts (8192),
      rights (8192)
{
}

void SpectrumTrace::setSampleRate (double rate)
{
    spectrum.setSampleRate (rate);
}

void SpectrumTrace::pull (devices::StereoTap& tap)
{
    tap.setListening (true);

    for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
    {
        for (int i = 0; i < n; ++i)
            lefts[(size_t) i] = 0.5f * (lefts[(size_t) i] + rights[(size_t) i]);

        spectrum.add (lefts.data(), n);
    }
}

void SpectrumTrace::pull (devices::SampleTap& tap)
{
    tap.setListening (true);

    for (int n; (n = tap.pull (lefts.data(), (int) lefts.size())) > 0;)
        spectrum.add (lefts.data(), n);
}

bool SpectrumTrace::update()
{
    return spectrum.update (fallPerSecond / (float) DisplayTicker::ticksPerSecond);
}

float SpectrumTrace::getLevelAt (double frequency) const
{
    return spectrum.getLevelAt (frequency) + tiltPerOctave * (float) std::log2 (frequency / 1000.0);
}

float SpectrumTrace::getLevelOver (double from, double to) const
{
    return spectrum.getLevelOver (from, to) + tiltPerOctave * (float) std::log2 (std::sqrt (from * to) / 1000.0);
}

void SpectrumTrace::paint (juce::Graphics& g, const FrequencyAxis& frequencies, const LevelAxis& levels, juce::Colour fill,
                           juce::Colour line) const
{
    const auto& area = frequencies.area;
    constexpr float step = 2.0f; // pixels between the curve's points, each the loudest of the frequencies around it
    const auto curve = makeCurve (
        area, [&] (float x)
        { return levels.yFor (getLevelOver (frequencies.frequencyAt (x - 0.5f * step), frequencies.frequencyAt (x + 0.5f * step))); },
        step);

    auto filled = curve;
    filled.lineTo (area.getBottomRight());
    filled.lineTo (area.getBottomLeft());
    filled.closeSubPath();

    g.setColour (fill);
    g.fillPath (filled);
    g.setColour (line);
    g.strokePath (curve, juce::PathStrokeType (1.0f));
}

//==============================================================================
LevelHistory::LevelHistory (int length, float initialValue)
    : values ((size_t) std::max (2, length), initialValue)
{
}

void LevelHistory::push (float value)
{
    values[(size_t) next] = value;
    next = (next + 1) % (int) values.size();
}

float LevelHistory::get (int index) const noexcept
{
    return values[(size_t) ((next + index) % (int) values.size())];
}

void LevelHistory::appendTo (std::vector<float>& numbers) const
{
    numbers.insert (numbers.end(), values.begin() + next, values.end());
    numbers.insert (numbers.end(), values.begin(), values.begin() + next);
}

juce::Path LevelHistory::makeLine (juce::Rectangle<float> area, const std::function<float (float)>& yFor) const
{
    juce::Path line;
    const auto step = area.getWidth() / (float) (size() - 1);

    for (int i = 0; i < size(); ++i)
    {
        const juce::Point<float> point { area.getX() + (float) i * step, juce::jlimit (area.getY(), area.getBottom(), yFor (get (i))) };

        if (i == 0)
            line.startNewSubPath (point);
        else
            line.lineTo (point);
    }

    return line;
}

juce::Path LevelHistory::makeArea (juce::Rectangle<float> area, const std::function<float (float)>& yFor, float baseY) const
{
    auto shape = makeLine (area, yFor);
    shape.lineTo (area.getRight(), baseY);
    shape.lineTo (area.getX(), baseY);
    shape.closeSubPath();
    return shape;
}

//==============================================================================
WheelGesture::~WheelGesture()
{
    end();
}

void WheelGesture::turn (ParameterHandle& handle, float newValue)
{
    if (turning != &handle)
        end();

    if (! handle.isInGesture())
        handle.beginGesture();

    turning = &handle;
    handle.setValue (newValue);
    startTimer (500);
}

void WheelGesture::end()
{
    stopTimer();

    if (turning != nullptr && turning->isInGesture())
        turning->endGesture();

    turning = nullptr;
}
} // namespace phazite::ui::graph
