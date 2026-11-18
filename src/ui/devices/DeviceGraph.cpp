// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceGraph.h"
#include "ui/components/DisplayTicker.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
      rights (8192),
      ways (8192)
{
}

void SpectrumTrace::setSampleRate (double newRate)
{
    sampleRate = newRate;
    left.setSampleRate (newRate);
    right.setSampleRate (newRate);

    if (middle != nullptr)
    {
        middle->setSampleRate (newRate);
        side->setSampleRate (newRate);
    }
}

void SpectrumTrace::analyze (bool middleAndSide)
{
    if (middleAndSide == middleAndSideShown)
        return;

    middleAndSideShown = middleAndSide;

    if (! middleAndSide)
        return;

    // From silence: what they heard before they stopped is long gone by.
    if (middle == nullptr)
    {
        middle = std::make_unique<devices::Spectrum>();
        side = std::make_unique<devices::Spectrum>();
    }

    for (auto* each : { middle.get(), side.get() })
    {
        each->setSampleRate (sampleRate);
        each->clear();
    }
}

void SpectrumTrace::add (int numPulled)
{
    left.add (lefts.data(), numPulled);
    right.add (rights.data(), numPulled);

    if (! middleAndSideShown)
        return;

    for (const auto& [shown, sign] : { std::pair { middle.get(), 1.0f }, std::pair { side.get(), -1.0f } })
    {
        for (int i = 0; i < numPulled; ++i)
            ways[(size_t) i] = 0.5f * (lefts[(size_t) i] + sign * rights[(size_t) i]);

        shown->add (ways.data(), numPulled);
    }
}

void SpectrumTrace::clear()
{
    left.clear();
    right.clear();

    if (middle != nullptr)
    {
        middle->clear();
        side->clear();
    }
}

void SpectrumTrace::pull (devices::StereoTap& tap)
{
    tap.setListening (true);

    for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
        add (n);
}

void SpectrumTrace::pull (devices::SampleTap& tap)
{
    tap.setListening (true);

    for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
        add (n);
}

bool SpectrumTrace::update()
{
    const auto fall = fallPerSecond / (float) DisplayTicker::ticksPerSecond;
    auto moved = left.update (fall);
    moved = right.update (fall) || moved;

    if (middleAndSideShown)
    {
        moved = middle->update (fall) || moved;
        moved = side->update (fall) || moved;
    }

    return moved;
}

float SpectrumTrace::getRawLevelOver (double from, double to, Channels channels) const
{
    switch (channels)
    {
        case Channels::left:
            return left.getLevelOver (from, to);
        case Channels::right:
            return right.getLevelOver (from, to);
        case Channels::middle:
            return middleAndSideShown ? middle->getLevelOver (from, to) : devices::Spectrum::floorDecibels;
        case Channels::side:
            return middleAndSideShown ? side->getLevelOver (from, to) : devices::Spectrum::floorDecibels;
        case Channels::louder:
            break;
    }

    return std::max (left.getLevelOver (from, to), right.getLevelOver (from, to));
}

float SpectrumTrace::getLevelAt (double frequency, Channels channels) const
{
    const auto at = [frequency] (const devices::Spectrum& spectrum) { return spectrum.getLevelAt (frequency); };
    float level = devices::Spectrum::floorDecibels;

    switch (channels)
    {
        case Channels::left:
            level = at (left);
            break;
        case Channels::right:
            level = at (right);
            break;
        case Channels::middle:
            level = middleAndSideShown ? at (*middle) : level;
            break;
        case Channels::side:
            level = middleAndSideShown ? at (*side) : level;
            break;
        case Channels::louder:
            level = std::max (at (left), at (right));
            break;
    }

    return level + tiltPerOctave * (float) std::log2 (frequency / 1000.0);
}

float SpectrumTrace::getLevelOver (double from, double to, Channels channels) const
{
    return getRawLevelOver (from, to, channels) + tiltPerOctave * (float) std::log2 (std::sqrt (from * to) / 1000.0);
}

void SpectrumTrace::paint (juce::Graphics& g, const FrequencyAxis& frequencies, const LevelAxis& levels, juce::Colour fill,
                           juce::Colour line, Channels channels) const
{
    const auto& area = frequencies.area;
    constexpr float step = 2.0f; // pixels between the curve's points, each the loudest of the frequencies around it
    const auto curve = makeCurve (
        area,
        [&] (float x)
        {
            return levels.yFor (
                getLevelOver (frequencies.frequencyAt (x - 0.5f * step), frequencies.frequencyAt (x + 0.5f * step), channels));
        },
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
    : values ((size_t) std::max (2, length), initialValue),
      times (values.size(), 0.0)
{
}

void LevelHistory::push (float value)
{
    push (value, 0.0);
}

void LevelHistory::push (float value, double milliseconds)
{
    values[(size_t) next] = value;
    times[(size_t) next] = milliseconds;
    next = (next + 1) % (int) values.size();
}

float LevelHistory::get (int index) const noexcept
{
    return values[(size_t) ((next + index) % (int) values.size())];
}

double LevelHistory::getTime (int index) const noexcept
{
    return times[(size_t) ((next + index) % (int) values.size())];
}

int LevelHistory::findAt (double milliseconds) const noexcept
{
    // From the newest back, to the oldest reading taken at the moment or after it whose one before was taken before it.
    auto found = -1;

    for (int index = size() - 1; index >= 0; --index)
    {
        const auto time = getTime (index);

        if (time <= 0.0)
            break;

        if (time < milliseconds)
            return found;

        found = index;
    }

    // The oldest with a time covers its own moment alone: what came before it isn't known.
    return found >= 0 && getTime (found) <= milliseconds ? found : -1;
}

void LevelHistory::clear (float value)
{
    std::fill (values.begin(), values.end(), value);
    std::fill (times.begin(), times.end(), 0.0);
    next = 0;
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
HandleDrag::HandleDrag (juce::Component& g)
    : graph (g)
{
    // A click on the graph leaves the keys where they were: else JUCE gives them to the first control beside it that takes
    // them (Component::grabKeyboardFocus() from a click on a component that doesn't), a knob or a button, which took them
    // from the drum sampler's pads, say.
    graph.setMouseClickGrabsKeyboardFocus (false);
}

HandleDrag::~HandleDrag()
{
    // A drag in progress is the handles' to end: each ends its gesture as it goes, keeping it. The graph's wheels go first
    // (they are declared after it).
    listen (false);
}

void HandleDrag::listen (bool shouldListen)
{
    if (window != nullptr)
        window->removeKeyListener (this);

    window = shouldListen ? graph.getTopLevelComponent() : nullptr;

    // Added last, it hears a key before the window's other listeners: its shortcuts, which use up every key while a mouse
    // button is down (KeyDispatcher), and the computer keyboard. It takes Escape alone.
    if (window != nullptr)
        window->addKeyListener (this);
}

void HandleDrag::take (std::vector<ParameterHandle*> taking)
{
    end(); // a drag that never let go (a press while another was held) keeps what it did

    // A turn of the wheel still open, the graph's or a knob's, is a step of its own, which the drag doesn't join.
    WheelBurst::endAll();

    handles = std::move (taking);
    starts.clear();

    for (auto* handle : handles)
        starts.push_back (handle != nullptr ? handle->getBaseValue() : 0.0f);

    // Its step begins now, as a knob's does as it is pressed, named after the first handle, whose gesture holds it and which
    // the others join; without a change it leaves nothing.
    for (auto* handle : handles)
        if (handle != nullptr)
            handle->beginGesture();

    taken = true;
    listen (true);
}

float HandleDrag::getStart (size_t handle) const
{
    return handle < starts.size() ? starts[handle] : 0.0f;
}

void HandleDrag::set (size_t handle, float value)
{
    if (! taken || handle >= handles.size() || handles[handle] == nullptr)
        return;

    const auto range = handles[handle]->getRange();
    const auto kept = juce::jlimit (range.start, range.end, value);

    if (! juce::exactlyEqual (kept, handles[handle]->getBaseValue()))
        handles[handle]->setValue (kept);
}

void HandleDrag::moveAlong (size_t handle, float moved, const std::function<float (float)>& placeOf,
                            const std::function<float (float)>& valueAt)
{
    // Not moved along this axis: as it was, exactly, as a round trip through the axis's pixels may not give it back.
    set (handle, juce::exactlyEqual (moved, 0.0f) ? getStart (handle) : valueAt (placeOf (getStart (handle)) + moved));
}

void HandleDrag::letGo()
{
    handles.clear();
    taken = false;
    listen (false);
}

void HandleDrag::end()
{
    if (! taken)
        return;

    // The others' first, the first's last: its gesture holds the step.
    for (auto each = handles.rbegin(); each != handles.rend(); ++each)
        if (*each != nullptr)
            (*each)->endGesture();

    letGo();
}

bool HandleDrag::cancel()
{
    if (! taken)
        return false;

    // The others' first, which joined the step; then the first's, which takes the whole step back.
    for (auto each = handles.rbegin(); each != handles.rend(); ++each)
        if (*each != nullptr)
            (*each)->cancelGesture();

    letGo();

    if (onCancelled != nullptr)
        onCancelled();

    return true;
}

bool HandleDrag::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    return key == juce::KeyPress::escapeKey && cancel();
}

//==============================================================================
WheelGesture::WheelGesture (HandleDrag& graphsDrag)
    : drag (graphsDrag),
      burst (graphsDrag.graph)
{
}

void WheelGesture::turn (ParameterHandle& handle, float newValue)
{
    // While the graph's drag is taken, the wheel would land in its step, which Escape takes back.
    if (drag.isTaken())
        return;

    // Another handle of the same graph (another band's Q) is a step of its own.
    if (turning != &handle)
        end();

    turning = &handle;
    burst.turn (
        [&handle]
        {
            if (! handle.isInGesture())
                handle.beginGesture();
        },
        [this]
        {
            if (turning != nullptr && turning->isInGesture())
                turning->endGesture();

            turning = nullptr;
        });

    handle.setValue (newValue);
}
} // namespace phazite::ui::graph
