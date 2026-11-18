// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DelayEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "GraphDot.h"
#include "devices/delay/Delay.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Delay;

    constexpr int numEchoes = 8;                                     // the steps shown after the sound coming in, the left's time each
    constexpr int mostEchoes = 64;                                   // drawn at most, for a side whose time is far shorter
    constexpr float litFall = 0.85f;                                 // of a hit's light, kept per refresh
    constexpr int historyTicks = 4 * DisplayTicker::ticksPerSecond;  // what came in, for the echoes' light
    constexpr int firstEcho = -1, nothing = -2, firstEchoRight = -3; // what a press takes, besides the band's ends (by their parameters)

    const juce::String about ("The sound coming in, then its echoes, fading as the feedback takes them down: with Link off or in "
                              "ping-pong, the left's above the middle and the right's below. Drag anywhere on them to move the first "
                              "echo: sideways for the time, up or down for the feedback. Under them, the band the echoes keep: drag "
                              "its ends.");

    // What the hint bar says of what a press takes.
    juce::String describe (int target)
    {
        if (target == firstEcho)
            return "Drag to move the first echo, from where it is: sideways for the time, up or down for the feedback.";

        if (target == firstEchoRight)
            return "Drag to move the right side's first echo, from where it is: sideways for its time, up or down for the feedback.";

        if (target == Delay::lowCut)
            return "Drag sideways to move the band's low end, the low cut: the echoes keep nothing below it.";

        if (target == Delay::highCut)
            return "Drag sideways to move the band's high end, the high cut: the echoes keep nothing above it.";

        return about;
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Delay::info.parameters[(size_t) index];
    }

    std::unique_ptr<Knob> makeKnob (int index)
    {
        const auto& parameter = getInfo (index);
        auto knob = std::make_unique<Knob> (juce::String (parameter.name.data(), parameter.name.size()), parameter.defaultValue);
        showAsParameter (*knob, parameter);
        return knob;
    }

    // An echo as the picture draws it: how long after the sound coming in, which side, which of the side's it is (1 for its
    // first), and which pass of the loop (1 for the first echo of all).
    struct Echo
    {
        double seconds = 0.0;
        int side = 0, number = 1, pass = 1;
    };
} // namespace

//==============================================================================
class DelayEditor::Echoes : public juce::Component,
                            private DisplayTicker::Display
{
public:
    explicit Echoes (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          history (historyTicks, 0.0f)
    {
        setTitle ("Echoes");
        setDescription (about);

        for (const auto& parameter : Delay::info.parameters)
            parameters.push_back (device.getParameter (parameter.id));

        for (auto [knob, attachment, index] : { std::tuple { &lowCutKnob, &lowCutAttachment, (int) Delay::lowCut },
                                                std::tuple { &highCutKnob, &highCutAttachment, (int) Delay::highCut } })
        {
            *knob = makeKnob (index);
            *attachment = std::make_unique<ParameterAttachment> (**knob);
            (*attachment)->setParameter (device.getParameter (getInfo (index).id));
            (*knob)->setTooltip (describeParameter (getInfo (index), device.getParameter (getInfo (index).id)));
            addAndMakeVisible (**knob);
        }

        drag.onCancelled = [this] { repaint(); };
    }

    ~Echoes() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    Knob& getLowCutKnob() const { return *lowCutKnob; }
    Knob& getHighCutKnob() const { return *highCutKnob; }

    juce::Rectangle<float> getEchoArea() const
    {
        return getLocalBounds().toFloat().withTrimmedBottom ((float) (compactKnobSize + Space::xs));
    }

    juce::Rectangle<int> getBandArea() const
    {
        return getLocalBounds()
            .removeFromBottom (compactKnobSize)
            .withTrimmedLeft (compactKnobSize + Space::xs)
            .withTrimmedRight (compactKnobSize + Space::xs);
    }

    void resized() override
    {
        auto strip = getLocalBounds().removeFromBottom (compactKnobSize);
        lowCutKnob->setBounds (strip.removeFromLeft (compactKnobSize));
        highCutKnob->setBounds (strip.removeFromRight (compactKnobSize));
    }

    // A side's time shown, in seconds, at the tempo the delay last played at, in the song's bar there.
    double getSeconds (int side = 0) const { return Delay::getDelaySeconds (getValues(), getTempo(), getBarLength(), side); }

    // A side's echo n's place (0 is the sound coming in), its bar's foot on the middle line, or on the bottom while the sides
    // are drawn as one.
    juce::Point<float> getEchoPosition (int n, int side) const { return { getX (n == 0 ? 0.0 : getEchoSeconds (n, side)), getFootY() }; }

    // How tall a side's echo n is drawn, as a share of the sound coming in's: the feedback's for each pass before it, all as
    // tall frozen, and as much lower as the duck takes them down now.
    float getEchoLevel (int n, int side) const { return n == 0 ? 1.0f : getLevel (getPass (n, side)); }

    // How lit the sound coming in is now, and an echo of a time ago at its level 1.
    float getInputLight() const noexcept { return lit; }
    float getLightAgo (double seconds) const { return getLight (seconds, 1.0f); }

    juce::String getReadout() const
    {
        const auto timeOf = [this] (int side)
        {
            auto text = juce::String (juce::roundToInt (getSeconds (side) * 1000.0)) + " ms";

            if (isSynced())
            {
                const auto index = (int) (side == 0 ? Delay::note : Delay::noteRight);
                const auto& choices = getInfo (index).choices;
                const auto chosen = juce::jlimit (0, (int) choices.size() - 1, juce::roundToInt (parameters[(size_t) index].getValue()));
                text = juce::String (choices[(size_t) chosen].data(), choices[(size_t) chosen].size()) + "  " + text;
            }

            return text;
        };

        const auto times = isLinked() ? timeOf (0) : "L " + timeOf (0) + "  R " + timeOf (1);
        const auto feedback = juce::roundToInt (parameters[Delay::feedback].getValue() * 100.0f);
        return times + "  Feedback " + juce::String (feedback) + "%" + (isFrozen() ? "  Frozen" : "");
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        paintEchoes (g, p, tint);
        paintBand (g, p, tint);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        const auto target = findTarget (e.position);
        draggingBand = target != firstEcho && target != firstEchoRight;
        draggedSide = target == firstEchoRight ? 1 : 0;

        // A band's end, or a side's first echo (its time and its feedback, as one undo step named after the first), from
        // where they are set by hand: the press changes nothing (graph::HandleDrag).
        if (draggingBand)
        {
            draggedCut = target;
            drag.take ({ &parameters[(size_t) draggedCut] });
        }
        else
        {
            drag.take ({ &parameters[(size_t) timeParameter (draggedSide)], &parameters[Delay::feedback] });
            const auto values = getBaseValues();
            startUnit = Delay::getDelaySeconds (values, getTempo(), getBarLength(), 0);
            startSeconds = Delay::getDelaySeconds (values, getTempo(), getBarLength(), draggedSide);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        if (draggingBand)
            drag.moveAlong (
                0, (float) e.getDistanceFromDragStartX(), [this] (float hertz) { return getBandAxis().xFor (hertz); },
                [this] (float x) { return (float) getBandAxis().frequencyAt (x); });
        else
            moveFirstEchoBy (e.getOffsetFromDragStart());

        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        drag.end();
        hover (getLocalBounds().toFloat().contains (e.position) ? findTarget (e.position) : nothing);
    }

    // Over the echoes, the grabbing hand, and the first echo a press there moves lit; over the band, the pointer that moves
    // sideways, and its nearer end lit, which a press takes.
    void mouseMove (const juce::MouseEvent& e) override { hover (findTarget (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { hover (nothing); }

private:
    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the delay's order
    graph::HandleDrag drag { *this };        // after the handles it moves
    std::unique_ptr<Knob> lowCutKnob, highCutKnob;
    std::unique_ptr<ParameterAttachment> lowCutAttachment, highCutAttachment;
    graph::LevelHistory history; // what came in, a peak a tick, as a gain
    float lit = 0.0f;            // the light of what is coming in now
    DrawnNumbers drawn;
    bool draggingBand = false;
    int draggedCut = Delay::lowCut, draggedSide = 0;
    int hovered = nothing;     // what a press under the pointer takes, lit: a side's first echo or one of the band's ends
    double startSeconds = 0.0; // the dragged side's time, set by hand, as the press took it
    double startUnit = 0.0;    // and the left's, which a step along is

    // In the band, its nearer end, by its parameter; under the middle line while the sides differ, the right's first echo;
    // elsewhere, the left's.
    int findTarget (juce::Point<float> position) const
    {
        if (! getBandArea().toFloat().contains (position))
            return ! isLinked() && position.y > getEchoArea().getCentreY() ? firstEchoRight : firstEcho;

        const auto axis = getBandAxis();
        const auto lowX = axis.xFor (parameters[Delay::lowCut].getValue()), highX = axis.xFor (parameters[Delay::highCut].getValue());
        return std::abs (position.x - lowX) <= std::abs (position.x - highX) ? Delay::lowCut : Delay::highCut;
    }

    void hover (int target)
    {
        setMouseCursor (target == firstEcho || target == firstEchoRight ? juce::MouseCursor::DraggingHandCursor
                        : target == nothing                             ? juce::MouseCursor::NormalCursor
                                                                        : juce::MouseCursor::LeftRightResizeCursor);
        setDescription (describe (target));

        if (target != hovered)
        {
            hovered = target;
            repaint();
        }
    }

    // Every parameter's value as it plays, and as it is set by hand.
    std::vector<float> getValues() const { return readValues (false); }
    std::vector<float> getBaseValues() const { return readValues (true); }

    std::vector<float> readValues (bool setByHand) const
    {
        std::vector<float> values;

        for (size_t i = 0; i < parameters.size(); ++i)
            values.push_back (! parameters[i].isValid() ? Delay::info.parameters[i].defaultValue
                              : setByHand               ? parameters[i].getBaseValue()
                                                        : parameters[i].getValue());

        return values;
    }

    double getTempo() const
    {
        auto* delay = device.getProcessor<Delay>();
        return delay != nullptr ? std::max (1.0f, delay->getShownTempo().get()) : 120.0;
    }

    double getBarLength() const
    {
        auto* delay = device.getProcessor<Delay>();
        return delay != nullptr ? std::max (0.25f, delay->getShownBarLength().get()) : 4.0;
    }

    // How far its duck takes the echoes down now, as a gain.
    float getDuckGain() const
    {
        auto* delay = device.getProcessor<Delay>();
        return delay != nullptr ? juce::Decibels::decibelsToGain (-delay->getShownDuck().get()) : 1.0f;
    }

    bool isSynced() const { return parameters[Delay::sync].getValue() > 0.5f; }
    bool isLinked() const { return parameters[Delay::link].getValue() > 0.5f; }
    bool bounces() const { return parameters[Delay::pingPong].getValue() > 0.5f; }
    bool isFrozen() const { return parameters[Delay::freeze].getValue() > 0.5f; }
    bool isSplit() const { return bounces() || ! isLinked(); } // the left's above the middle line, the right's below

    int timeParameter (int side) const
    {
        if (side == 0)
            return isSynced() ? Delay::note : Delay::time;

        return isSynced() ? Delay::noteRight : Delay::timeRight;
    }

    // Where a time after the sound coming in is drawn: a step along for each of the left's time.
    juce::Rectangle<float> getInner() const { return getEchoArea().reduced ((float) Space::s, 0.0f); }
    float getStep() const { return getInner().getWidth() / (float) (numEchoes + 1); }
    float getOrigin() const { return getInner().getX() + getStep() * 0.5f; }

    float getX (double seconds) const
    {
        const auto unit = getSeconds (0);
        return getOrigin() + (float) (unit > 0.0 ? seconds / unit : 0.0) * getStep();
    }

    float getFootY() const
    {
        const auto area = getEchoArea().reduced ((float) Space::s, 0.0f);
        return isSplit() ? area.getCentreY() : area.getBottom() - (float) TextSize::small - 2.0f;
    }

    // A side's echo n: how long after the sound coming in, and which pass of the loop it is. In ping-pong the sound goes in on
    // the left and the echoes cross from side to side, so the left's n is pass 2n - 1, after n of its times and n - 1 of the
    // right's, and the right's n is pass 2n, after n of each.
    double getEchoSeconds (int n, int side) const
    {
        if (bounces())
            return side == 0 ? n * getSeconds (0) + (n - 1) * getSeconds (1) : n * (getSeconds (0) + getSeconds (1));

        return n * getSeconds (side);
    }

    int getPass (int n, int side) const { return bounces() ? (side == 0 ? 2 * n - 1 : 2 * n) : n; }

    float getLevel (int pass) const
    {
        const auto level = isFrozen() ? 1.0f : std::pow (parameters[Delay::feedback].getValue(), (float) (pass - 1));
        return level * getDuckGain();
    }

    // The echoes drawn, as far as the picture reaches: one side's while they are one, each side's while they differ.
    std::vector<Echo> getDrawnEchoes() const
    {
        std::vector<Echo> drawnEchoes;
        const auto right = getInner().getRight();

        for (int side = 0; side < (isSplit() ? 2 : 1); ++side)
        {
            for (int n = 1; n <= mostEchoes; ++n)
            {
                const auto seconds = getEchoSeconds (n, side);

                if (getX (seconds) > right)
                    break;

                drawnEchoes.push_back ({ seconds, side, n, getPass (n, side) });
            }
        }

        return drawnEchoes;
    }

    // The first echo moved as far as the pointer from where it was set by hand as the press took it: the left's time in
    // proportion to how far along from the sound coming in it is taken (its first echo is drawn a step along, whatever its
    // time), the right's to where the pointer puts its first echo on that scale; the feedback by how far up. Each move works
    // from the press's values, not from what the last one wrote, and an axis the pointer hasn't moved along keeps its value
    // exactly (known bug 116).
    void moveFirstEchoBy (juce::Point<int> offset)
    {
        const auto area = getEchoArea();
        // The feedback up to 100 % at most, where the echoes would start to swell: past it is the knob's, or a value typed,
        // not a drag's by accident (one that starts over it can only bring it down).
        const auto highest = std::max (1.0f, drag.getStart (1));
        drag.set (1, offset.y == 0
                         ? drag.getStart (1)
                         : juce::jlimit (0.0f, highest, drag.getStart (1) - (float) offset.y / std::max (1.0f, area.getHeight())));

        if (offset.x == 0)
        {
            drag.set (0, drag.getStart (0));
            return;
        }

        const auto step = (double) std::max (1.0f, getStep());
        auto seconds = 0.001;

        if (draggedSide == 0)
        {
            seconds = std::max (0.001, startSeconds * (step + offset.x) / step);
        }
        else
        {
            // Its first echo, as far along as its time (after the left's, in ping-pong) on the left's scale.
            const auto before = bounces() ? startUnit : 0.0;
            const auto along = (before + startSeconds) / std::max (0.001, startUnit) * step + offset.x;
            seconds = std::max (0.001, along / step * startUnit - before);
        }

        if (isSynced())
        {
            // The note value nearest, at the tempo.
            const auto parameter = timeParameter (draggedSide);
            const auto& choices = getInfo (parameter).choices;
            auto best = 0;
            auto bestDistance = 1.0e9;

            for (size_t i = 0; i < choices.size(); ++i)
            {
                auto values = getBaseValues();
                values[(size_t) parameter] = (float) i;

                if (const auto distance =
                        std::abs (std::log (Delay::getDelaySeconds (values, getTempo(), getBarLength(), draggedSide) / seconds));
                    distance < bestDistance)
                {
                    bestDistance = distance;
                    best = (int) i;
                }
            }

            drag.set (0, (float) best);
        }
        else
        {
            drag.set (0, (float) (seconds * 1000.0));
        }
    }

    void tick() override
    {
        lowCutAttachment->update();
        highCutAttachment->update();

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);
            const auto peak = juce::Decibels::decibelsToGain (scope->getInputPeak().take(), devices::DeviceScope::silentDecibels);
            history.push (peak, juce::Time::getMillisecondCounterHiRes());
            lit = std::max (std::min (1.0f, peak * 2.0f), lit * litFall);
            lit = lit < 1.0e-3f ? 0.0f : lit; // out, rather than ever fainter
        }

        // The echoes, lit by what came in, at what the picture draws of the delay's settings (not the character, the
        // modulation, the duck's amount or the transition, which it doesn't draw), the song's tempo and what its duck takes
        // down now.
        std::vector<float> numbers;

        for (const auto index : { Delay::sync, Delay::note, Delay::time, Delay::link, Delay::noteRight, Delay::timeRight, Delay::feedback,
                                  Delay::pingPong, Delay::freeze, Delay::lowCut, Delay::highCut })
            numbers.push_back (parameters[(size_t) index].isValid() ? parameters[(size_t) index].getValue() : 0.0f);

        numbers.insert (numbers.end(), { (float) getTempo(), (float) getBarLength(), getDuckGain(), lit });
        history.appendTo (numbers);

        if (drawn.changed (std::move (numbers)))
            repaint();
    }

    // How bright an echo is now: what came in its time ago (the peak taken over the time that covers that moment), taken
    // down as it is drawn.
    float getLight (double seconds, float level) const
    {
        const auto newest = history.getTime (history.size() - 1);
        const auto index = newest > 0.0 ? history.findAt (newest - seconds * 1000.0) : -1;

        if (index < 0)
            return 0.0f;

        return std::min (1.0f, history.get (index) * 2.0f) * level;
    }

    // Its editor shown again: what came in while nobody looked doesn't light the echoes.
    void shownAgain() override
    {
        if (auto* scope = device.getScope())
            scope->drain();

        history.clear (0.0f);
        lit = 0.0f;
    }

    void paintEchoes (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getEchoArea();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::s, (float) Space::xs);
        const auto split = isSplit();
        const auto halfHeight = split ? inner.getHeight() * 0.5f - TextSize::small : inner.getHeight() - 2.0f * TextSize::small;
        const auto barWidth = std::max (3.0f, inner.getWidth() / (float) (numEchoes + 1) * 0.35f);
        const auto foot = getFootY();

        if (split)
        {
            g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
            g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());
        }

        const auto fillBar = [&g] (juce::Rectangle<float> bar, juce::Colour colour)
        {
            g.setColour (colour);
            g.fillRoundedRectangle (bar, 2.0f);
        };

        // The sound coming in at full height.
        const auto inBar = juce::Rectangle<float> (getOrigin() - barWidth * 0.5f, foot - halfHeight, barWidth, halfHeight);
        fillBar (inBar, p.textMuted.withAlpha (GraphTint::partOff));

        if (lit > 0.02f)
            fillBar (inBar, p.textMuted.withAlpha (std::min (1.0f, lit)));

        // Then each echo, the feedback down from the one before: the left's up from the line, the right's down from it.
        for (const auto& echo : getDrawnEchoes())
        {
            const auto level = getLevel (echo.pass);
            const auto x = getX (echo.seconds);
            const auto height = std::max (2.0f, std::min (1.0f, level) * halfHeight);
            const auto up = ! split || echo.side == 0;
            const auto bar = juce::Rectangle<float> (x - barWidth * 0.5f, up ? foot - height : foot, barWidth, height);
            fillBar (bar, tint.withAlpha (GraphTint::partCurve + 0.25f * std::min (1.0f, level)));

            // Lit as what played passes through.
            if (const auto light = getLight (echo.seconds, level); light > 0.02f)
                fillBar (bar, tint.withAlpha (std::min (1.0f, light)));

            // Ringed while a press would put it under the pointer.
            if (echo.number == 1 && hovered == (echo.side == 0 ? firstEcho : firstEchoRight))
                paintLitRing (g, p, bar, 2.0f);
        }

        // The times, in note values or milliseconds, the feedback, and Frozen.
        graph::paintReadouts (g, p, area, getReadout(),
                              bounces() ? "Ping-pong: left above, right below" : (split ? "Left above, right below" : ""));
    }

    void paintBand (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getBandArea().toFloat();
        const auto axis = getBandAxis();
        graph::paintBackground (g, p, area);
        graph::paintFrequencyGrid (g, p, axis, false);

        const auto lowX = axis.xFor (parameters[Delay::lowCut].getValue()), highX = axis.xFor (parameters[Delay::highCut].getValue());
        const auto kept = area.withX (lowX).withRight (std::max (lowX + 1.0f, highX)).reduced (0.0f, 6.0f);
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle (kept, 3.0f);

        // Its ends, the one a press takes ringed.
        for (const auto cut : { (int) Delay::lowCut, (int) Delay::highCut })
        {
            const auto x = cut == Delay::lowCut ? lowX : highX;
            const auto end = juce::Rectangle<float> (4.0f, area.getHeight() - 8.0f).withCentre ({ x, area.getCentreY() });
            g.setColour (tint);
            g.fillRoundedRectangle (end, 2.0f);

            if (cut == hovered)
                paintLitRing (g, p, end, 2.0f);
        }

        graph::paintReadout (g, p, area,
                             graph::formatFrequency (parameters[Delay::lowCut].getValue()) + " to "
                                 + graph::formatFrequency (parameters[Delay::highCut].getValue()),
                             juce::Justification::centred);
    }

    graph::FrequencyAxis getBandAxis() const { return { getBandArea().toFloat() }; }
};

//==============================================================================
DelayEditor::DelayEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    auto& shown = keep (std::make_unique<Echoes> (device));
    echoes = &shown;
    addControl ("lowCut", shown.getLowCutKnob());
    addControl ("highCut", shown.getHighCutKnob());

    // The left's note and time, then Link, which the right's beside it under them follow; its names as given, as the part's
    // title would take "Time" off them.
    const auto time =
        makeControlsPart ("Time", { "sync", "note", "time", "transition", "link", "noteRight", "timeRight" }, 4,
                          ParameterControls::Size::compact, { "Sync", "Note", "Time", "Transition", "Link", "Note Right", "Time Right" });
    const auto feedback = makeControlsPart ("Feedback", { "feedback", "pingPong", "freeze" }, 3, ParameterControls::Size::large,
                                            { "Feedback", "Ping-Pong", "Freeze" });
    const auto character = makeControlsPart ("Character", { "character", "modRate", "modDepth" }, 3, ParameterControls::Size::compact,
                                             { "Character", "Mod Rate", "Mod Depth" });
    const auto duck = makeControlsPart ("Duck", { "duck" }, 1, ParameterControls::Size::compact, { "Duck" });
    setParts ({ { { "Echoes", &shown, 300, 196, 120, true, GraphTint::minimumWidth } }, { time, feedback }, { character, duck } });
    refresh();
}

DelayEditor::~DelayEditor() = default;

juce::Component& DelayEditor::getEchoes() const noexcept
{
    return *echoes;
}

juce::Point<float> DelayEditor::getEchoPosition (int n, int side) const
{
    return echoes->getEchoPosition (n, side);
}

float DelayEditor::getEchoLevel (int n, int side) const
{
    return echoes->getEchoLevel (n, side);
}

float DelayEditor::getInputLight() const
{
    return echoes->getInputLight();
}

float DelayEditor::getLightAgo (double seconds) const
{
    return echoes->getLightAgo (seconds);
}

juce::Rectangle<int> DelayEditor::getBandArea() const
{
    return echoes->getBandArea();
}

juce::String DelayEditor::getReadout() const
{
    return echoes->getReadout();
}

void DelayEditor::refresh()
{
    // The right's own time does nothing while Link is on: faint.
    const auto linked = device.getParameter ("link").getValue() > 0.5f;

    for (const auto* id : { "noteRight", "timeRight" })
        if (auto* control = findControl (id))
            control->setAlpha (linked ? SwitchTint::partOff : 1.0f);
}
} // namespace phazite::ui
