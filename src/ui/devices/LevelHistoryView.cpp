// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LevelHistoryView.h"
#include "DeviceTint.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    constexpr int numberedDips = 3;     // the deepest reductions shown with their decibels
    constexpr float smallestDip = 0.5f; // dB: shallower ones aren't numbered
    constexpr double jumpSlack = 0.25;  // beats further than its tempo would have taken the song: it jumped there
    constexpr size_t mostLinesAtOnce = 64;

    // Levels and reductions so near silence that their traces don't show (a tenth of a pixel or so).
    constexpr float unseenLevel = 0.1f, unseenReduction = 0.05f;
} // namespace

LevelHistoryView::LevelHistoryView (const juce::String& title, float lowestLevel, float range)
    : lowest (lowestLevel),
      reductionRange (range)
{
    setTitle (title);
    describe();

    drag.onCancelled = [this]
    {
        dragging = false;
        repaint();
    };

    // Near the line, the line: its parameter.
    drag.findHandle = [this] (juce::Point<float> at) -> std::optional<graph::Handle>
    {
        if (! isNearLine (at.y))
            return std::nullopt;

        return graph::Handle ({ line }, { at.x, getYForLevel (line->getValue()) });
    };
}

void LevelHistoryView::describe()
{
    const auto shown =
        keyName.isNotEmpty() ? "how loud the sound came in (pale) and went out (over it, each duck a dip under what came in), and "
                                   + keyName + ", what it listens to, as a line in its colour"
        : gainRange > 0.0f
            ? juce::String ("how loud the sound came in (pale) and went out, and how far it was turned up or down (a line above or "
                            "below the middle)")
            : juce::String ("how loud the sound came in (pale) and went out, and how far it was turned down (red, from the top)");
    setDescription ("The last two bars: " + shown + ", with a faint line at each beat and a stronger one at each bar."
                    + (line != nullptr ? " Drag the " + lineName.toLowerCase() + "'s line up or down to move it." : juce::String()));
}

void LevelHistoryView::push (double milliseconds, const SongTime& song, float input, float output, float reduction, float key)
{
    const Levels levels[] = { { input, output, reduction, key } };
    push (milliseconds, song, levels);
}

void LevelHistoryView::push (double milliseconds, const SongTime& song, std::span<const Levels> levels)
{
    // Two bars at the tempo where the playhead is.
    span = barsShown * std::max (song.barLength, 0.25) * 60000.0 / std::max (song.bpm, 1.0);

    // The beats and bars the song passed since the last reading, while it plays.
    if (song.playing && ! readings.empty())
        addLines (readings.back().milliseconds, milliseconds, song);

    // Each reading over its share of the time since the last, those in one half column as one: the loudest of what came in
    // and went out and the key, and the furthest it turned (down, or up below 0).
    const auto from = readings.empty() || readings.back().milliseconds >= milliseconds ? milliseconds : readings.back().milliseconds;
    const auto bucket = span / std::max (2.0, 2.0 * (double) getLevels().area.getWidth());
    const auto bucketOf = [bucket] (double time) { return std::ceil (time / bucket - 1.0e-9); };

    for (size_t i = 0; i < levels.size(); ++i)
    {
        const auto& each = levels[i];
        const auto time = from + (milliseconds - from) * (double) (i + 1) / (double) levels.size();

        if (! readings.empty() && bucketOf (readings.back().milliseconds) >= bucketOf (time))
        {
            auto& last = readings.back();
            last.milliseconds = time;
            last.input = std::max (last.input, each.input);
            last.output = std::max (last.output, each.output);
            last.key = std::max (last.key, each.key);

            if (std::abs (each.reduction) > std::abs (last.reduction))
                last.reduction = each.reduction;

            continue;
        }

        readings.push_back ({ time, each.input, each.output, each.reduction, each.key });
    }

    lastSong = song;
    ++pushes;

    // What scrolled out at the left goes, but the reading the traces come in from.
    const auto oldest = milliseconds - span;

    while (readings.size() > 1 && readings[1].milliseconds <= oldest)
        readings.pop_front();

    while (! lines.empty() && lines.front().milliseconds < oldest)
        lines.pop_front();

    // Only what moves the picture repaints: a history of silence doesn't, scrolled on (docs/archive/performance-audit.md, section 8).
    std::vector<float> numbers { line != nullptr ? line->getValue() : 0.0f, isLineLit() ? 1.0f : 0.0f, (float) span, gainRange };

    if (! isFlat())
        numbers.push_back ((float) (pushes % 1000000));

    if (drawn.changed (std::move (numbers)))
        repaint();
}

void LevelHistoryView::addLines (double fromMilliseconds, double toMilliseconds, const SongTime& song)
{
    const auto elapsed = toMilliseconds - fromMilliseconds;
    const auto perMillisecond = song.bpm / 60000.0;

    if (elapsed <= 0.0 || perMillisecond <= 0.0 || song.beatLength <= 0.0 || song.barLength <= 0.0)
        return;

    // Where the song was at the last reading; or, where it jumped since (a loop going round, a click in a ruler) or didn't
    // play, where its tempo would have brought it from.
    auto from = lastSong.beat;

    if (const auto moved = song.beat - from; ! lastSong.playing || moved <= 0.0 || moved > 2.0 * elapsed * perMillisecond + jumpSlack)
        from = song.beat - elapsed * perMillisecond;

    // Each of the time signature's beats passed since, counted from the start of the bar the playhead is in, every bar's
    // length from it a bar's: each at the moment the song passed it, between the two readings.
    std::vector<Line> passed;

    for (auto n = std::floor ((song.beat - song.barStart) / song.beatLength); passed.size() < mostLinesAtOnce; n -= 1.0)
    {
        const auto beat = song.barStart + n * song.beatLength;

        if (beat <= from)
            break;

        const auto intoBars = n * song.beatLength / song.barLength;
        passed.push_back (
            { fromMilliseconds + (beat - from) / (song.beat - from) * elapsed, std::abs (intoBars - std::round (intoBars)) < 1.0e-6 });
    }

    lines.insert (lines.end(), passed.rbegin(), passed.rend());
}

bool LevelHistoryView::isFlat() const
{
    return lines.empty()
           && std::all_of (readings.begin(), readings.end(),
                           [this] (const Reading& reading)
                           {
                               return reading.input <= lowest + unseenLevel && reading.output <= lowest + unseenLevel
                                      && std::abs (reading.reduction) < unseenReduction
                                      && (showsReduction() || reading.key <= lowest + unseenLevel);
                           });
}

float LevelHistoryView::getXForTime (double milliseconds) const
{
    const auto area = getLevels().area;
    const auto newest = readings.empty() ? milliseconds : readings.back().milliseconds;
    return area.getRight() - (float) ((newest - milliseconds) / span) * area.getWidth();
}

std::vector<LevelHistoryView::Line> LevelHistoryView::getLines() const
{
    std::vector<Line> shown;

    if (readings.empty())
        return shown;

    const auto oldest = readings.back().milliseconds - span;

    for (const auto& each : lines)
        if (each.milliseconds >= oldest)
            shown.push_back (each);

    return shown;
}

void LevelHistoryView::setLine (ParameterHandle* handle, const juce::String& name)
{
    // Taken away (or another given) mid-drag: the drag's step ends as it is.
    if (handle != line)
    {
        drag.end();
        dragging = false;
    }

    if (handle == nullptr)
        setLineLit (false);

    line = handle;
    lineName = name;
    describe();
    repaint();
}

void LevelHistoryView::setReductionScale (float decibels, float shareOfHeight)
{
    reductionRange = std::max (1.0f, decibels);
    reductionShare = juce::jlimit (0.1f, 1.0f, shareOfHeight);
    repaint();
}

void LevelHistoryView::setGainAroundMiddle (float decibels)
{
    if (juce::exactlyEqual (decibels, gainRange))
        return;

    gainRange = std::max (0.0f, decibels);
    describe();
    repaint();
}

void LevelHistoryView::setKey (const juce::String& name, juce::Colour colour)
{
    if (name == keyName && colour == keyColour)
        return;

    keyName = name;
    keyColour = colour;
    describe();
    repaint();
}

float LevelHistoryView::getNewestReduction() const
{
    return readings.empty() ? 0.0f : readings.back().reduction;
}

float LevelHistoryView::getNewestInput() const
{
    return readings.empty() ? lowest : readings.back().input;
}

graph::LevelAxis LevelHistoryView::getLevels() const
{
    return { getLocalBounds().toFloat().reduced (1.0f), lowest, 0.0f };
}

float LevelHistoryView::getYForLevel (float decibels) const
{
    return getLevels().yFor (decibels);
}

juce::Path LevelHistoryView::makeTrace (float Reading::* value, const std::function<float (float)>& yFor) const
{
    // A line through the readings, each at the middle of the time it covers (since the one before), from the start of the
    // first's to the end of the last's: so that readings fewer than the columns draw as lines between them, not as steps, and
    // a tick that came late spreads over as long as it lasted.
    juce::Path trace;

    if (readings.empty())
        return trace;

    trace.startNewSubPath (getReadingSpan (0).getStart(), yFor (readings.front().*value));

    for (size_t i = 0; i < readings.size(); ++i)
    {
        const auto across = getReadingSpan ((int) i);
        trace.lineTo (0.5f * (across.getStart() + across.getEnd()), yFor (readings[i].*value));
    }

    trace.lineTo (getReadingSpan ((int) readings.size() - 1).getEnd(), yFor (readings.back().*value));
    return trace;
}

int LevelHistoryView::getNumReadings() const noexcept
{
    return (int) readings.size();
}

juce::Range<float> LevelHistoryView::getReadingSpan (int index) const
{
    if (! juce::isPositiveAndBelow (index, (int) readings.size()))
        return {};

    const auto& reading = readings[(size_t) index];
    const auto from = index > 0 ? readings[(size_t) index - 1].milliseconds : reading.milliseconds;
    return { getXForTime (from), getXForTime (reading.milliseconds) };
}

float LevelHistoryView::getReduction (int index) const
{
    return juce::isPositiveAndBelow (index, (int) readings.size()) ? readings[(size_t) index].reduction : 0.0f;
}

void LevelHistoryView::clear()
{
    readings.clear();
    lines.clear();
    pushes = 0;

    if (drawn.changed ({}))
        repaint();
}

void LevelHistoryView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto tint = getDeviceTint (*this);
    const auto levels = getLevels();
    const auto inner = levels.area;
    graph::paintBackground (g, p, getLocalBounds().toFloat());

    {
        // Within the graph: what scrolled past its left edge doesn't show.
        const juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (inner.getSmallestIntegerContainer());

        // The beats, faint, and the bars, stronger, under the sound, where the song was as it played.
        for (const auto& each : lines)
        {
            g.setColour (p.text.withAlpha (each.bar ? GraphTint::zeroLine : GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (getXForTime (each.milliseconds)), inner.getY(), inner.getBottom());
        }

        graph::paintLevelGrid (g, p, levels, 12.0f, true);

        if (! readings.empty())
        {
            const auto yFor = [&levels, this] (float decibels) { return levels.yFor (juce::jlimit (lowest, 0.0f, decibels)); };
            const auto reductionY = [&inner, this] (float reduction)
            { return inner.getY() + juce::jlimit (0.0f, 1.0f, reduction / reductionRange) * inner.getHeight() * reductionShare; };
            const auto firstX = getXForTime (readings.front().milliseconds), lastX = getXForTime (readings.back().milliseconds);
            const auto filled = [firstX, lastX] (juce::Path trace, float baseY)
            {
                trace.lineTo (lastX, baseY);
                trace.lineTo (firstX, baseY);
                trace.closeSubPath();
                return trace;
            };

            // What comes in, pale, and what goes out, in the device's colour over it, outlined: where the device turns the
            // sound down, what went out dips under what came in.
            const auto in = makeTrace (&Reading::input, yFor);
            g.setColour (p.textMuted.withAlpha (GraphTint::spectrumFill * 2.0f));
            g.fillPath (filled (in, inner.getBottom()));

            const auto out = makeTrace (&Reading::output, yFor);
            g.setColour (tint.withAlpha (GraphTint::partCurve * 2.0f));
            g.fillPath (filled (out, inner.getBottom()));
            g.setColour (tint);
            g.strokePath (out, juce::PathStrokeType (1.0f));

            // Turning up as well as down, its gain as a line about the middle, up where it turned up and down where it turned
            // down.
            if (showsGainAroundMiddle())
            {
                const auto middle = inner.getCentreY();
                const auto gainY = [&inner, middle, this] (float reduction)
                { return middle + juce::jlimit (-1.0f, 1.0f, reduction / gainRange) * inner.getHeight() * 0.5f; };
                g.setColour (p.text.withAlpha (GraphTint::zeroLine));
                g.drawHorizontalLine (juce::roundToInt (middle), inner.getX(), inner.getRight());
                g.setColour (p.danger);
                g.strokePath (makeTrace (&Reading::reduction, gainY), juce::PathStrokeType (1.5f));
            }
            // Listening to another track, that track as a line in its colour, named, which the threshold's line crosses where
            // the sound dips: how far it turned down is the dips themselves.
            else if (! showsReduction())
            {
                g.setColour (keyColour);
                g.strokePath (makeTrace (&Reading::key, yFor), juce::PathStrokeType (1.5f));
                g.setFont (juce::FontOptions (TextSize::small));
                g.drawText (keyName, inner.reduced ((float) Space::xs), juce::Justification::bottomLeft, true);
            }
            else
            {
                // Turned down: hanging from the top, as deep as it went.
                const auto down = makeTrace (&Reading::reduction, reductionY);
                g.setColour (p.danger.withAlpha (GraphTint::partCurve * 2.0f));
                g.fillPath (filled (down, inner.getY()));
                g.setColour (p.danger);
                g.strokePath (down, juce::PathStrokeType (1.0f));

                // The deepest dips numbered, each the deepest of its third of what shows.
                const auto start = readings.back().milliseconds - span;

                for (int stretch = 0; stretch < numberedDips; ++stretch)
                {
                    const auto from = start + span * stretch / numberedDips, to = start + span * (stretch + 1) / numberedDips;
                    const Reading* deepest = nullptr;

                    for (const auto& reading : readings)
                        if (reading.milliseconds > from && reading.milliseconds <= to
                            && (deepest == nullptr || reading.reduction > deepest->reduction))
                            deepest = &reading;

                    if (deepest != nullptr && deepest->reduction >= smallestDip)
                    {
                        const auto x = getXForTime (deepest->milliseconds);
                        graph::paintReadout (
                            g, p,
                            juce::Rectangle<float> (x - 30.0f, reductionY (deepest->reduction), 60.0f, TextSize::small + 6.0f)
                                .constrainedWithin (inner),
                            "-" + juce::String (deepest->reduction, 1), juce::Justification::centredTop);
                    }
                }
            }
        }
    }

    // The line across, with its name and level: lit while the pointer is near it, and while it is dragged.
    if (line != nullptr)
    {
        const auto y = levels.yFor (line->getValue());
        const auto lit = isLineLit();
        g.setColour (p.text.withAlpha (lit ? 1.0f : GraphTint::marker * 1.5f));
        g.drawLine (inner.getX(), y, inner.getRight(), y, lit ? 2.0f : 1.0f);
        graph::paintReadout (g, p,
                             juce::Rectangle<float> (inner.getX(), y - TextSize::small - 4.0f, inner.getWidth(), TextSize::small + 4.0f),
                             lineName + " " + graph::formatDecibels (line->getValue()), juce::Justification::bottomRight);
    }
}

//==============================================================================
bool LevelHistoryView::isNearLine (float y) const
{
    return line != nullptr && line->isValid() && std::abs (y - getYForLevel (line->getValue())) <= GraphTint::grabDistance * 0.5f;
}

void LevelHistoryView::setLineLit (bool lit)
{
    if (lit == lineLit)
        return;

    // Near the line: an up-and-down arrow, the line lit, and the hint bar says what a drag does (its tooltip).
    lineLit = lit;
    setMouseCursor (lit ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    setTooltip (lit ? "Drag up or down to move the " + lineName.toLowerCase()
                          + " (Shift in fine steps); double-click to put it back; right-click for its menu"
                    : juce::String());
    repaint();
}

void LevelHistoryView::mouseMove (const juce::MouseEvent& e)
{
    setLineLit (isNearLine (e.position.y));
}

void LevelHistoryView::mouseExit (const juce::MouseEvent&)
{
    if (! dragging)
        setLineLit (false);
}

void LevelHistoryView::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! isNearLine (e.position.y))
        return;

    dragging = true;
    drag.take ({ line }); // from its value set by hand (graph::HandleDrag)
    repaint();
}

void LevelHistoryView::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging || ! drag.isTaken())
        return;

    const auto levels = getLevels();
    const auto perPixel = (levels.high - levels.low) / std::max (1.0f, levels.area.getHeight());
    const auto range = line->getRange();
    drag.set (0, range.snapToLegalValue (juce::jlimit (range.start, range.end, drag.getStart (0) - drag.getMoved (e).y * perPixel)));
    repaint();
}

void LevelHistoryView::mouseUp (const juce::MouseEvent& e)
{
    drag.end();
    dragging = false;
    setLineLit (getLocalBounds().contains (e.getPosition()) && isNearLine (e.position.y));
    repaint();
}
} // namespace phazite::ui
