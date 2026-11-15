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

    constexpr int numEchoes = 8;                                    // the echoes shown after the sound coming in
    constexpr float litFall = 0.85f;                                // of a hit's light, kept per refresh
    constexpr int historyTicks = 4 * DisplayTicker::ticksPerSecond; // what came in, for the echoes' light
    constexpr int firstEcho = -1, nothing = -2;                     // what a press takes, besides the band's ends (by their parameters)

    const juce::String about ("The sound coming in, then its echoes, fading as the feedback takes them down. Drag anywhere on them "
                              "to move the first echo: sideways for the time, up or down for the feedback. Under them, the band the "
                              "echoes keep: drag its ends.");

    // What the hint bar says of what a press takes.
    juce::String describe (int target)
    {
        if (target == firstEcho)
            return "Drag to move the first echo, from where it is: sideways for the time, up or down for the feedback.";

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

    // The time shown, in seconds, at the tempo the delay last played at, in the song's bar there.
    double getSeconds() const { return Delay::getDelaySeconds (getValues(), getTempo(), getBarLength()); }

    // Echo n's place (0 is the sound coming in), its bar's foot on the middle line, or on the bottom when echoes don't bounce.
    juce::Point<float> getEchoPosition (int n) const
    {
        const auto area = getEchoArea().reduced ((float) Space::s, 0.0f);
        const auto x =
            area.getX() + (float) n / (float) (numEchoes + 1) * area.getWidth() + area.getWidth() / (float) (2 * (numEchoes + 1));
        return { x, bounces() ? area.getCentreY() : area.getBottom() - (float) TextSize::small - 2.0f };
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
        draggingBand = target != firstEcho;

        // A band's end, or the first echo (its time and its feedback, as one undo step named after the first), from where they
        // are set by hand: the press changes nothing (graph::HandleDrag).
        if (draggingBand)
        {
            draggedCut = target;
            drag.take ({ &parameters[(size_t) draggedCut] });
        }
        else
        {
            drag.take ({ &parameters[(size_t) timeParameter()], &parameters[Delay::feedback] });
            startSeconds = Delay::getDelaySeconds (getBaseValues(), getTempo(), getBarLength());
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

    // Over the echoes, the grabbing hand, and the first echo lit, which a drag there moves; over the band, the pointer
    // that moves sideways, and its nearer end lit, which a press takes.
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
    int draggedCut = Delay::lowCut;
    int hovered = nothing;     // what a press under the pointer takes, lit: the first echo or one of the band's ends
    double startSeconds = 0.0; // the first echo's time, set by hand, as the press took it

    // In the band, its nearer end, by its parameter; elsewhere, the first echo.
    int findTarget (juce::Point<float> position) const
    {
        if (! getBandArea().toFloat().contains (position))
            return firstEcho;

        const auto axis = getBandAxis();
        const auto lowX = axis.xFor (parameters[Delay::lowCut].getValue()), highX = axis.xFor (parameters[Delay::highCut].getValue());
        return std::abs (position.x - lowX) <= std::abs (position.x - highX) ? Delay::lowCut : Delay::highCut;
    }

    void hover (int target)
    {
        setMouseCursor (target == firstEcho ? juce::MouseCursor::DraggingHandCursor
                        : target == nothing ? juce::MouseCursor::NormalCursor
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

    bool isSynced() const { return parameters[Delay::sync].getValue() > 0.5f; }
    bool bounces() const { return parameters[Delay::pingPong].getValue() > 0.5f; }
    int timeParameter() const { return isSynced() ? Delay::time : Delay::milliseconds; }

    graph::FrequencyAxis getBandAxis() const { return { getBandArea().toFloat() }; }

    // The first echo moved as far as the pointer from where it was set by hand as the press took it: its time in proportion to
    // how far along from the sound coming in it is taken (echo 1 is drawn in one place, whatever its time), its feedback by how
    // far up. Each move works from the press's values, not from what the last one wrote, and an axis the pointer hasn't moved
    // along keeps its value exactly (known bug 116).
    void moveFirstEchoBy (juce::Point<int> offset)
    {
        const auto area = getEchoArea();
        drag.set (1, offset.y == 0 ? drag.getStart (1)
                                   : juce::jlimit (0.0f, 1.0f, drag.getStart (1) - (float) offset.y / std::max (1.0f, area.getHeight())));

        if (offset.x == 0)
        {
            drag.set (0, drag.getStart (0));
            return;
        }

        const auto first = getEchoPosition (1).x, origin = getEchoPosition (0).x;
        const auto seconds =
            std::max (0.001, startSeconds * (double) ((first + (float) offset.x - origin) / std::max (1.0f, first - origin)));

        if (isSynced())
        {
            // The note value nearest, at the tempo.
            const auto& choices = getInfo (Delay::time).choices;
            auto best = 0;
            auto bestDistance = 1.0e9;

            for (size_t i = 0; i < choices.size(); ++i)
            {
                auto values = getBaseValues();
                values[Delay::time] = (float) i;

                if (const auto distance = std::abs (std::log (Delay::getDelaySeconds (values, getTempo(), getBarLength()) / seconds));
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
            history.push (peak);
            lit = std::max (std::min (1.0f, peak * 2.0f), lit * litFall);
            lit = lit < 1.0e-3f ? 0.0f : lit; // out, rather than ever fainter
        }

        // The echoes, lit by what came in, at the delay's settings and the song's tempo.
        auto numbers = getValues();
        numbers.insert (numbers.end(), { (float) getTempo(), lit });
        history.appendTo (numbers);

        if (drawn.changed (std::move (numbers)))
            repaint();
    }

    // How bright echo n is now: what came in its time ago, taken down by the feedback.
    float getLight (int n, float level) const
    {
        if (n == 0)
            return lit;

        const auto ticksAgo = juce::roundToInt (getSeconds() * n * DisplayTicker::ticksPerSecond);

        if (ticksAgo >= history.size())
            return 0.0f;

        return std::min (1.0f, history.get (history.size() - 1 - ticksAgo) * 2.0f) * level;
    }

    void paintEchoes (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getEchoArea();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::s, (float) Space::xs);
        const auto halfHeight = bounces() ? inner.getHeight() * 0.5f - TextSize::small : inner.getHeight() - 2.0f * TextSize::small;
        const auto feedback = parameters[Delay::feedback].getValue();
        const auto barWidth = std::max (3.0f, inner.getWidth() / (float) (numEchoes + 1) * 0.35f);

        if (bounces())
        {
            g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
            g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());
        }

        for (int n = 0; n <= numEchoes; ++n)
        {
            // The sound coming in at full height, then each echo the feedback down from the one before.
            const auto level = n == 0 ? 1.0f : std::pow (feedback, (float) (n - 1));
            const auto foot = getEchoPosition (n);
            const auto height = std::max (2.0f, level * halfHeight);
            const auto up = ! bounces() || n % 2 == 1 || n == 0;
            const auto bar = juce::Rectangle<float> (foot.x - barWidth * 0.5f, up ? foot.y - height : foot.y, barWidth, height);
            const auto colour = n == 0 ? p.textMuted : tint;
            g.setColour (colour.withAlpha (n == 0 ? GraphTint::partOff : GraphTint::partCurve + 0.25f * level));
            g.fillRoundedRectangle (bar, 2.0f);

            // Lit as what played passes through.
            if (const auto light = getLight (n, level); light > 0.02f)
            {
                g.setColour (colour.withAlpha (std::min (1.0f, light)));
                g.fillRoundedRectangle (bar, 2.0f);
            }

            // Ringed while a press would put it under the pointer.
            if (n == 1 && hovered == firstEcho)
                paintLitRing (g, p, bar, 2.0f);
        }

        // The time, in note values or milliseconds, and the feedback.
        const auto seconds = getSeconds();
        auto time = juce::String (juce::roundToInt (seconds * 1000.0)) + " ms";

        if (isSynced())
        {
            const auto index =
                juce::jlimit (0, (int) getInfo (Delay::time).choices.size() - 1, juce::roundToInt (parameters[Delay::time].getValue()));
            const auto name = getInfo (Delay::time).choices[(size_t) index];
            time = juce::String (name.data(), name.size()) + "  " + time;
        }

        graph::paintReadouts (g, p, area, time + "  Feedback " + juce::String (juce::roundToInt (feedback * 100.0f)) + "%",
                              bounces() ? "Ping-pong: left above, right below" : "");
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
};

//==============================================================================
DelayEditor::DelayEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Echoes> (device));
    echoes = &shown;
    addControl ("lowCut", shown.getLowCutKnob());
    addControl ("highCut", shown.getHighCutKnob());

    const auto time = makeControlsPart ("Time", { "sync", "time", "milliseconds", "pingPong" }, 4, ParameterControls::Size::compact,
                                        { "Sync", "Note", "Time", "Ping-Pong" });
    const auto feedback = makeControlsPart ("Feedback", { "feedback" }, 1, ParameterControls::Size::large);
    setParts ({ { { "Echoes", &shown, 300, 196, 120, true, GraphTint::minimumWidth } }, { time, feedback } });
}

DelayEditor::~DelayEditor() = default;

juce::Component& DelayEditor::getEchoes() const noexcept
{
    return *echoes;
}

juce::Point<float> DelayEditor::getEchoPosition (int n) const
{
    return echoes->getEchoPosition (n);
}

juce::Rectangle<int> DelayEditor::getBandArea() const
{
    return echoes->getBandArea();
}
} // namespace phazite::ui
