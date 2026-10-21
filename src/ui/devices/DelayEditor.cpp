// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DelayEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/delay/Delay.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Delay;

    constexpr int numEchoes = 8;                                   // the echoes shown after the sound coming in
    constexpr float litFall = 0.85f;                               // of a hit's light, kept per refresh
    constexpr int historyTicks = 4 * DeviceTicker::ticksPerSecond; // what came in, for the echoes' light

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
                            private DeviceTicker::Display
{
public:
    explicit Echoes (const DeviceHandle& d)
        : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          history (historyTicks, 0.0f)
    {
        setTitle ("Echoes");
        setDescription ("The sound coming in, then its echoes, fading as the feedback takes them down. Drag to put the first echo "
                        "under the pointer: sideways for the time, up or down for the feedback. Under them, the band the echoes "
                        "keep: drag its ends.");

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

    // The time shown, in seconds, at the tempo the delay last played at.
    double getSeconds() const { return Delay::getDelaySeconds (getValues(), getTempo()); }

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
        draggingBand = getBandArea().toFloat().contains (e.position);

        if (draggingBand)
        {
            const auto axis = getBandAxis();
            const auto lowX = axis.xFor (parameters[Delay::lowCut].getValue()), highX = axis.xFor (parameters[Delay::highCut].getValue());
            draggedCut = std::abs (e.position.x - lowX) <= std::abs (e.position.x - highX) ? Delay::lowCut : Delay::highCut;
            parameters[(size_t) draggedCut].beginGesture();
        }
        else
        {
            // The time, and the feedback: one undo step, named after the first.
            parameters[(size_t) timeParameter()].beginGesture();
            parameters[Delay::feedback].beginGesture();
        }

        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingBand)
        {
            auto& cut = parameters[(size_t) draggedCut];
            const auto range = cut.getRange();
            cut.setValue (juce::jlimit (range.start, range.end, (float) getBandAxis().frequencyAt (e.position.x)));
        }
        else
        {
            setFirstEchoAt (e.position);
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (draggingBand)
        {
            parameters[(size_t) draggedCut].endGesture();
        }
        else
        {
            parameters[Delay::feedback].endGesture();
            parameters[(size_t) timeParameter()].endGesture();
        }
    }

private:
    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the delay's order
    std::unique_ptr<Knob> lowCutKnob, highCutKnob;
    std::unique_ptr<ParameterAttachment> lowCutAttachment, highCutAttachment;
    graph::LevelHistory history; // what came in, a peak a tick, as a gain
    float lit = 0.0f;            // the light of what is coming in now
    bool draggingBand = false;
    int draggedCut = Delay::lowCut;

    std::vector<float> getValues() const
    {
        std::vector<float> values;

        for (size_t i = 0; i < parameters.size(); ++i)
            values.push_back (parameters[i].isValid() ? parameters[i].getValue() : Delay::info.parameters[i].defaultValue);

        return values;
    }

    double getTempo() const
    {
        auto* delay = device.getProcessor<Delay>();
        return delay != nullptr ? std::max (1.0f, delay->getShownTempo().get()) : 120.0;
    }

    bool isSynced() const { return parameters[Delay::sync].getValue() > 0.5f; }
    bool bounces() const { return parameters[Delay::pingPong].getValue() > 0.5f; }
    int timeParameter() const { return isSynced() ? Delay::time : Delay::milliseconds; }

    graph::FrequencyAxis getBandAxis() const { return { getBandArea().toFloat() }; }

    // The first echo where the pointer is: its time from how far along, its feedback from how high.
    void setFirstEchoAt (juce::Point<float> position)
    {
        const auto first = getEchoPosition (1).x, origin = getEchoPosition (0).x;
        const auto seconds = std::max (0.001, getSeconds() * (double) ((position.x - origin) / std::max (1.0f, first - origin)));

        if (isSynced())
        {
            // The note value nearest, at the tempo.
            const auto& choices = getInfo (Delay::time).choices;
            auto best = 0;
            auto bestDistance = 1.0e9;

            for (size_t i = 0; i < choices.size(); ++i)
            {
                auto values = getValues();
                values[Delay::time] = (float) i;

                if (const auto distance = std::abs (std::log (Delay::getDelaySeconds (values, getTempo()) / seconds));
                    distance < bestDistance)
                {
                    bestDistance = distance;
                    best = (int) i;
                }
            }

            parameters[Delay::time].setValue ((float) best);
        }
        else
        {
            const auto range = parameters[Delay::milliseconds].getRange();
            parameters[Delay::milliseconds].setValue (juce::jlimit (range.start, range.end, (float) (seconds * 1000.0)));
        }

        const auto area = getEchoArea();
        const auto feedbackRange = parameters[Delay::feedback].getRange();
        const auto height = juce::jlimit (0.0f, 1.0f, (area.getBottom() - position.y) / std::max (1.0f, area.getHeight()));
        parameters[Delay::feedback].setValue (juce::jlimit (feedbackRange.start, feedbackRange.end, height));
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
        }

        repaint();
    }

    // How bright echo n is now: what came in its time ago, taken down by the feedback.
    float getLight (int n, float level) const
    {
        if (n == 0)
            return lit;

        const auto ticksAgo = juce::roundToInt (getSeconds() * n * DeviceTicker::ticksPerSecond);

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

        graph::paintReadout (g, p, area, time + "  Feedback " + juce::String (juce::roundToInt (feedback * 100.0f)) + "%",
                             juce::Justification::topLeft);
        graph::paintReadout (g, p, area, bounces() ? "Ping-pong: left above, right below" : "", juce::Justification::topRight);
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
        g.setColour (tint);

        for (const auto x : { lowX, highX })
            g.fillRoundedRectangle (juce::Rectangle<float> (4.0f, area.getHeight() - 8.0f).withCentre ({ x, area.getCentreY() }), 2.0f);

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
    setParts ({ { { "Echoes", &shown, 300, 196, 120, true } }, { time, feedback } });
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
