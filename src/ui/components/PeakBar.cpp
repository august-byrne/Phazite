// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PeakBar.h"
#include "ValueText.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr float fallDbPerTick = 20.0f / (0.5f * (float) DisplayTicker::ticksPerSecond); // 20 dB in half a second
    constexpr float moveDb = 0.25f;                                                         // the least it shows moving
    constexpr int clipTicksLit = (int) (PeakBarTint::clipSeconds * (float) DisplayTicker::ticksPerSecond);
} // namespace

PeakBar::PeakBar()
    : DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    setTitle ("Output level");
    setInterceptsMouseClicks (false, false);
}

PeakBar::~PeakBar() = default;

void PeakBar::setSource (LevelSource newSource)
{
    source = std::move (newSource);
    level = shown = LevelSource::silenceDb;
    clipTicks = 0;
    tick();
    repaint();
}

void PeakBar::setFillColour (juce::Colour newFill)
{
    if (newFill != fill)
    {
        fill = newFill;
        repaint();
    }
}

void PeakBar::tick()
{
    source.refresh();
    const auto wasMeasuring = std::exchange (measuring, source.isMeasuring());

    // The louder channel: rises at once, falls steadily, holds no peak.
    auto measured = (float) LevelSource::silenceDb;

    if (measuring)
        for (int ch = 0; ch < std::max (1, source.getNumChannels()); ++ch)
            measured = std::max (measured, source.getAndClearLevel (ch));

    level = juce::jmax (measured, level - fallDbPerTick, (float) LevelSource::silenceDb);

    // A clip lights its top for a while, from the last one.
    const auto wasLit = clipTicks > 0;
    clipTicks = measured > maxDb ? clipTicksLit : std::max (0, clipTicks - 1);

    // Only what moves what is drawn repaints: levels under its range all look the same, so in silence it repaints nothing.
    const auto drawnMoved = std::abs (juce::jmax (level, minDb) - juce::jmax (shown, minDb)) > moveDb || (level > minDb) != (shown > minDb);

    if (drawnMoved || wasLit != (clipTicks > 0) || measuring != wasMeasuring)
    {
        shown = level;
        repaint();
    }
}

void PeakBar::paint (juce::Graphics& g)
{
    if (! measuring)
        return;

    // The trough, faint in the track's colour, its whole height; the level up from the bottom, brighter.
    const auto bar = getLocalBounds().toFloat();
    g.setColour (fill.withAlpha (PeakBarTint::trough));
    g.fillRect (bar);

    if (shown > minDb)
    {
        const auto proportion = (juce::jlimit (minDb, maxDb, shown) - minDb) / (maxDb - minDb);
        g.setColour (fill.brighter (PeakBarTint::brighter));
        g.fillRect (bar.withTop (bar.getBottom() - proportion * bar.getHeight()));
    }

    if (clipTicks > 0)
    {
        g.setColour (getPalette (*this).danger);
        g.fillRect (bar.withHeight ((float) PeakBarTint::clipHeight));
    }
}

juce::String PeakBar::getLevelText() const
{
    if (! measuring)
        return "No sound of its own";

    const auto text = shown > minDb ? formatDecibels (shown) : juce::String ("Silent");
    return clipTicks > 0 ? text + ", clipped" : text;
}

std::unique_ptr<juce::AccessibilityHandler> PeakBar::createAccessibilityHandler()
{
    // Screen readers read it as a value that can't be changed: the level now.
    class Value final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit Value (const PeakBar& b)
            : bar (b)
        {
        }

        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return bar.getLevelText(); }
        void setValueAsString (const juce::String&) override {}

    private:
        const PeakBar& bar;
    };

    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::progressBar, juce::AccessibilityActions(),
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}
} // namespace phazite::ui
