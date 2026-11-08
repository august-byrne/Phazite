// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FmEditor.h"
#include "DeviceTint.h"
#include "devices/dsp/Envelopes.h"
#include "devices/fm/FmSynth.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::FmSynth;
    using Field = FmSynth::Field;

    // The operators' part: the algorithm down its left, the operators' columns beside it, their heads over the groups' tabs
    // and the group shown.
    constexpr int algorithmWidth = 128, algorithmGap = Space::m, columnGap = Space::xs;
    constexpr int stepHeight = 20, soundHeight = 26, timbreGap = Space::xs;
    constexpr int headerHeight = 20, envelopeHeight = 34, stripHeight = 19, rowGap = 3;
    constexpr int tabHeight = 20, tabGap = 4, groupGap = 3, groupRowGap = 4;
    constexpr int bendWidth = 32; // a stage's bend, beside its time

    std::string_view getID (int parameter)
    {
        return FmSynth::info.parameters[(size_t) parameter].id;
    }

    std::string_view getID (int op, Field field)
    {
        return getID (FmSynth::getParameterIndex (op, field));
    }

    juce::String getParameterName (int parameter)
    {
        const auto name = FmSynth::info.parameters[(size_t) parameter].name;
        return { name.data(), name.size() };
    }

    // What each algorithm is good for, as the eight say it under their pictures.
    constexpr const char* algorithmUses[] = {
        "Each bends the next: one rich, bright tone for basses, leads and growls",
        "Two bend a stack: deep basses, as the TX81Z's Lately Bass",
        "A chain and one more bend one: mallets, plucks and clavs",
        "Three bend one: rich basses and brass",
        "Two pairs, each a modulator over a carrier: electric pianos and bells",
        "A stack beside one alone: a growl over a clean sub",
        "One bends three: brass, strings and organs",
        "Four heard side by side, none bent: organs and additive tones",
    };

    static_assert (std::size (algorithmUses) == (size_t) FmSynth::numAlgorithms);

    juce::String getAlgorithmName (int index)
    {
        const auto name = FmSynth::info.parameters[(size_t) FmSynth::algorithm].choices[(size_t) index];
        return { name.data(), name.size() };
    }

    // A carrier (heard) in the device's colour, a modulator in modulation's.
    juce::Colour getRoleColour (const juce::Component& component, bool carrier)
    {
        return carrier ? getDeviceTint (component) : getPalette (component).modulation;
    }

    // "Heard", or what an operator modulates, the highest first: "→ 3 2 1".
    juce::String getTargetsText (int algorithm, int op)
    {
        if (FmSynth::isCarrier (algorithm, op))
            return "Heard";

        auto text = juce::String::fromUTF8 ("\xe2\x86\x92");
        const auto targets = FmSynth::getTargets (algorithm, op);

        for (int target = FmSynth::numOperators - 1; target >= 0; --target)
            if ((targets >> target) & 1)
                text << " " << (target + 1);

        return text;
    }

    // What an operator modulates, in words: "operator 1", "operators 3, 2 and 1".
    juce::String describeTargets (int algorithm, int op)
    {
        juce::StringArray numbers;
        const auto targets = FmSynth::getTargets (algorithm, op);

        for (int target = FmSynth::numOperators - 1; target >= 0; --target)
            if ((targets >> target) & 1)
                numbers.add (juce::String (target + 1));

        if (numbers.size() == 1)
            return "operator " + numbers[0];

        const auto last = numbers[numbers.size() - 1];
        numbers.remove (numbers.size() - 1);
        return "operators " + numbers.joinIntoString (", ") + " and " + last;
    }

    // Where each operator sits in an algorithm's picture, from 0 to 1 across and up: the carriers along the bottom, the
    // highest number on the left; each modulator a row above the highest of what it modulates, over the middle of what it
    // modulates, so that a pair's modulator stands over its carrier and a stack stands straight, and the operators of a row
    // at least `apart` from each other, in their order.
    std::array<juce::Point<float>, FmSynth::numOperators> placeOperators (const FmSynth::Algorithm& algorithm, float apart)
    {
        constexpr int count = FmSynth::numOperators;
        std::array<int, count> rows {};

        for (int op = 0; op < count; ++op)
        {
            auto row = 0;

            for (int below = 0; below < op; ++below)
                if ((algorithm.modulatedBy[(size_t) below] >> op) & 1)
                    row = std::max (row, rows[(size_t) below] + 1);

            rows[(size_t) op] = row;
        }

        const auto highest = *std::max_element (rows.begin(), rows.end());
        std::array<juce::Point<float>, count> places {};

        for (int row = 0; row <= highest; ++row)
        {
            std::vector<int> inRow;

            for (int op = count - 1; op >= 0; --op)
                if (rows[(size_t) op] == row)
                    inRow.push_back (op);

            std::vector<float> xs;

            for (size_t n = 0; n < inRow.size(); ++n)
            {
                if (row == 0)
                {
                    xs.push_back (((float) n + 0.5f) / (float) inRow.size());
                    continue;
                }

                auto sum = 0.0f;
                auto targets = 0;

                for (int below = 0; below < count; ++below)
                {
                    if ((algorithm.modulatedBy[(size_t) below] >> inRow[n]) & 1)
                    {
                        sum += places[(size_t) below].x;
                        ++targets;
                    }
                }

                xs.push_back (targets > 0 ? sum / (float) targets : 0.5f);
            }

            // Those that would overlap spread out evenly around the middle of where they would be, in their order, until none
            // do; then the row stays inside.
            for (int pass = 0; pass < count; ++pass)
            {
                auto moved = false;

                for (size_t first = 0; first < xs.size();)
                {
                    auto last = first;

                    while (last + 1 < xs.size() && xs[last + 1] - xs[last] < apart - 1.0e-4f)
                        ++last;

                    if (last > first)
                    {
                        auto middle = 0.0f;

                        for (auto n = first; n <= last; ++n)
                            middle += xs[n] / (float) (last - first + 1);

                        for (auto n = first; n <= last; ++n)
                            xs[n] = middle + ((float) (n - first) - (float) (last - first) * 0.5f) * apart;

                        moved = true;
                    }

                    first = last + 1;
                }

                if (! moved)
                    break;
            }

            const auto shift = std::max (0.0f, -xs.front()) - std::max (0.0f, xs.back() - 1.0f);

            for (size_t n = 0; n < inRow.size(); ++n)
                places[(size_t) inRow[n]] = { std::clamp (xs[n] + shift, 0.0f, 1.0f), highest > 0 ? (float) row / (float) highest : 0.0f };
        }

        return places;
    }

    // How an algorithm's picture shows each operator: lit (from 0 to 1) as its envelope moves, switched on, fed back (from
    // -1 to 1), and under the pointer.
    struct OperatorLook
    {
        float lit = 0.0f;
        bool on = true;
        float feedback = 0.0f;
        bool hovered = false;
    };

    using Looks = std::array<OperatorLook, FmSynth::numOperators>;

    // An algorithm's picture: each operator a box with its number, the carriers in the device's colour along the bottom and
    // the modulators in modulation's above what they modulate, each modulator's line down into what it modulates, the
    // carriers' lines into the one along the bottom, which goes down out of the picture (what is heard), and a loop at the
    // side of each operator fed back (dashed while it is fed back toward a square). Returns each operator's box.
    std::array<juce::Rectangle<float>, FmSynth::numOperators> paintAlgorithm (juce::Graphics& g, const juce::Component& component,
                                                                              juce::Rectangle<float> area, int index, const Looks& looks,
                                                                              float boxWidth)
    {
        constexpr int count = FmSynth::numOperators;
        const auto& p = getPalette (component);
        const auto& shape = FmSynth::getAlgorithm (index);
        const auto boxHeight = std::round (boxWidth * 0.75f);
        const auto loop = std::round (boxWidth * 0.3f);
        const auto inner = area.withTrimmedTop (boxHeight * 0.5f + loop + 2.0f)
                               .withTrimmedBottom (boxHeight * 0.5f + boxHeight * 0.7f)
                               .withTrimmedLeft (boxWidth * 0.5f + 1.0f)
                               .withTrimmedRight (boxWidth * 0.5f + loop + 2.0f);
        const auto places = placeOperators (shape, (boxWidth + boxWidth * 0.35f + loop) / std::max (1.0f, inner.getWidth()));
        std::array<juce::Rectangle<float>, count> boxes {};

        for (int op = 0; op < count; ++op)
            boxes[(size_t) op] = juce::Rectangle<float> (boxWidth, boxHeight)
                                     .withCentre ({ std::round (inner.getX() + places[(size_t) op].x * inner.getWidth()),
                                                    std::round (inner.getBottom() - places[(size_t) op].y * inner.getHeight()) });

        // Each modulator's line into what it modulates: down from its box, across, and down into the box's top.
        const auto lineWidth = std::max (1.0f, boxWidth / 14.0f);
        g.setColour (p.textMuted);

        for (int op = 0; op < count; ++op)
        {
            for (int from = op + 1; from < count; ++from)
            {
                if (((shape.modulatedBy[(size_t) op] >> from) & 1) == 0)
                    continue;

                const auto start = boxes[(size_t) from].getCentre().withY (boxes[(size_t) from].getBottom());
                const auto end = boxes[(size_t) op].getCentre().withY (boxes[(size_t) op].getY());
                const auto middle = std::round ((start.y + end.y) * 0.5f);
                juce::Path line;
                line.startNewSubPath (start);
                line.lineTo (start.x, middle);
                line.lineTo (end.x, middle);
                line.lineTo (end);
                g.strokePath (line, juce::PathStrokeType (lineWidth));
            }
        }

        // The carriers' lines into the one along the bottom, and down from its middle out of the picture: what is heard.
        const auto tint = getDeviceTint (component);
        const auto bottom = std::round (inner.getBottom() + boxHeight * 0.5f + boxHeight * 0.45f);
        auto leftmost = area.getRight(), rightmost = area.getX();

        g.setColour (tint);

        for (int op = 0; op < count; ++op)
        {
            if (! FmSynth::isCarrier (index, op))
                continue;

            const auto x = boxes[(size_t) op].getCentreX();
            leftmost = std::min (leftmost, x);
            rightmost = std::max (rightmost, x);
            g.drawLine (x, boxes[(size_t) op].getBottom(), x, bottom, lineWidth);
        }

        const auto out = std::round ((leftmost + rightmost) * 0.5f);
        g.drawLine (leftmost - lineWidth * 0.5f, bottom, rightmost + lineWidth * 0.5f, bottom, lineWidth);
        g.drawLine (out, bottom, out, area.getBottom(), lineWidth);

        // Each operator's feedback, a loop out of its right side and back into its top, as thick as it is deep.
        for (int op = 0; op < count; ++op)
        {
            const auto& look = looks[(size_t) op];

            if (std::abs (look.feedback) < 0.005f)
                continue;

            const auto& box = boxes[(size_t) op];
            juce::Path path;
            const auto enter = box.getRight() - box.getWidth() * 0.25f;
            path.startNewSubPath (box.getRight(), box.getCentreY());
            path.lineTo (box.getRight() + loop, box.getCentreY());
            path.lineTo (box.getRight() + loop, box.getY() - loop);
            path.lineTo (enter, box.getY() - loop);
            path.lineTo (enter, box.getY());
            const juce::PathStrokeType stroke (lineWidth * (1.0f + 1.2f * std::abs (look.feedback)));
            g.setColour (getRoleColour (component, FmSynth::isCarrier (index, op)));

            if (look.feedback < 0.0f)
            {
                juce::Path dashed;
                const float dashes[] = { 2.0f, 2.0f };
                stroke.createDashedStroke (dashed, path, dashes, 2);
                g.fillPath (dashed);
            }
            else
            {
                g.strokePath (path, stroke);
            }
        }

        // The boxes: filled in their roles' colours, brighter as they are lit; hollow while off; ringed under the pointer.
        const auto font = juce::Font (juce::FontOptions (std::min (TextSize::small, boxHeight - 2.0f))).boldened();

        for (int op = 0; op < count; ++op)
        {
            const auto& look = looks[(size_t) op];
            const auto& box = boxes[(size_t) op];
            const auto colour = getRoleColour (component, FmSynth::isCarrier (index, op));
            const auto radius = std::min (cornerRadius, boxHeight * 0.25f);

            if (look.on)
            {
                const auto fill = p.background.interpolatedWith (colour, 0.45f + 0.55f * std::clamp (look.lit, 0.0f, 1.0f));
                g.setColour (fill);
                g.fillRoundedRectangle (box, radius);
                g.setColour (colour);
                g.drawRoundedRectangle (box.reduced (0.5f), radius, 1.0f);
                g.setColour (getInkFor (fill));
            }
            else
            {
                g.setColour (p.background);
                g.fillRoundedRectangle (box, radius);
                g.setColour (colour.withAlpha (0.6f));
                g.drawRoundedRectangle (box.reduced (0.5f), radius, 1.0f);
                g.setColour (p.textMuted);
            }

            g.setFont (font);
            g.drawText (juce::String (op + 1), box, juce::Justification::centred, false);

            if (look.hovered)
            {
                g.setColour (p.focus);
                g.drawRoundedRectangle (box.expanded (2.0f), radius + 2.0f, 1.5f);
            }
        }

        return boxes;
    }

    //==========================================================================
    // A parameter as a strip of its own: dragged up and down or left and right as a knob is (a fifth as far with Shift), back
    // to where it started with a double-click, stepped by the arrow keys; its name at its left and its value at its right,
    // filled to its value in a colour where it is an amount (from the middle for one that goes both ways), or for a stage's
    // bend, the stage drawn bent. A right-click offers to automate or modulate it. The editor's ticker shows changes made
    // elsewhere (update()).
    class ParameterStrip : public ValueSlider
    {
    public:
        enum class Look
        {
            field,
            amount,
            bend
        };

        ParameterStrip (const DeviceHandle& device, int parameter, const juce::String& shownName, Look shownAs)
            : ValueSlider (RotaryHorizontalVerticalDrag, getParameterName (parameter),
                           FmSynth::info.parameters[(size_t) parameter].defaultValue),
              caption (shownName),
              look (shownAs)
        {
            const auto& info = FmSynth::info.parameters[(size_t) parameter];
            setTextBoxStyle (NoTextBox, false, 0, 0);
            setPopupDisplayEnabled (false, false, nullptr);
            setMouseDragSensitivity (200);
            showAsParameter (*this, info);
            attachment.setParameter (device.getParameter (info.id));
            bipolar = info.minimum < 0.0f && info.maximum > 0.0f;
        }

        // The fill's colour, for an amount; and for a bend, whether its stage rises (an attack) or falls.
        std::function<juce::Colour()> fillColour;
        bool rises = true;

        void update() { attachment.update(); }

        void paint (juce::Graphics& g) override
        {
            const auto& p = getPalette (*this);
            const auto box = getLocalBounds().toFloat().reduced (0.5f);
            const auto under = isMouseOverOrDragging();
            const auto shown = isEnabled() ? 1.0f : 0.5f;

            g.setColour (p.surface);
            g.fillRoundedRectangle (box, cornerRadius);

            if (look == Look::amount)
            {
                const auto at = (float) valueToProportionOfLength (getValue());
                const auto from = bipolar ? (float) valueToProportionOfLength (0.0) : 0.0f;
                const auto left = std::min (at, from), right = std::max (at, from);
                juce::Path shape;
                shape.addRoundedRectangle (box, cornerRadius);
                juce::Graphics::ScopedSaveState clipped (g);
                g.reduceClipRegion (shape);
                g.setColour ((fillColour != nullptr ? fillColour() : getDeviceTint (*this)).withMultipliedAlpha (0.45f * shown));
                g.fillRect (box.withX (box.getX() + left * box.getWidth()).withWidth (std::max (1.0f, (right - left) * box.getWidth())));
            }
            else if (look == Look::bend)
            {
                // The stage across the strip, bent as it plays.
                const auto area = box.reduced (6.0f, 4.0f);
                const auto curve = (float) getValue();
                juce::Path stage;

                for (int i = 0; i <= 16; ++i)
                {
                    const auto fraction = (float) i / 16.0f;
                    const auto shape = devices::dsp::Adsr::getShape (curve, fraction);
                    const juce::Point<float> at { area.getX() + fraction * area.getWidth(),
                                                  area.getBottom() - (rises ? shape : 1.0f - shape) * area.getHeight() };

                    if (i == 0)
                        stage.startNewSubPath (at);
                    else
                        stage.lineTo (at);
                }

                g.setColour (getDeviceTint (*this).withMultipliedAlpha (shown));
                g.strokePath (stage, juce::PathStrokeType (1.5f));
            }

            g.setColour (hasKeyboardFocus (false) ? p.focus : under ? p.textMuted : p.controlBorder);
            g.drawRoundedRectangle (box, cornerRadius, 1.0f);

            if (look == Look::bend)
                return;

            const auto text = box.reduced (5.0f, 0.0f);
            const auto value = getTextFromValue (getValue());
            g.setFont (juce::FontOptions (TextSize::small));

            if (caption.isNotEmpty())
            {
                g.setColour (p.textMuted.withMultipliedAlpha (shown));
                g.drawText (caption, text, juce::Justification::centredLeft, false);
            }

            g.setColour (p.text.withMultipliedAlpha (shown));
            g.drawText (value, text, caption.isNotEmpty() ? juce::Justification::centredRight : juce::Justification::centred, false);
        }

        void mouseEnter (const juce::MouseEvent& e) override
        {
            ValueSlider::mouseEnter (e);
            repaint();
        }

        void mouseExit (const juce::MouseEvent& e) override
        {
            ValueSlider::mouseExit (e);
            repaint();
        }

    private:
        const juce::String caption;
        const Look look;
        bool bipolar = false;
        ParameterAttachment attachment { *this };
    };

    //==========================================================================
    // An operator's wave: four buttons, each drawing its wave, the one played lit; a click plays another, as one undo step.
    class WaveChoice : public juce::Component
    {
    public:
        WaveChoice (const DeviceHandle& device, int op)
            : wave (device.getParameter (getID (op, Field::wave)))
        {
            setTitle (getParameterName (FmSynth::getParameterIndex (op, Field::wave)));
            setFocusContainerType (FocusContainerType::focusContainer);

            static constexpr const char* tooltips[] = {
                "Sine: the pure tone that FM bends into every other; the DX7 had nothing else",
                "Triangle: softer and hollow, odd harmonics only, falling away fast",
                "Saw: every harmonic, bright and buzzing, band-limited so that it doesn't alias",
                "Square: odd harmonics only, hollow and reedy",
            };

            const auto& choices = FmSynth::info.parameters[(size_t) FmSynth::getParameterIndex (op, Field::wave)].choices;

            for (int i = 0; i < (int) choices.size(); ++i)
            {
                auto* button = buttons.add (new WaveButton (i));
                button->setTitle (juce::String (choices[(size_t) i].data(), choices[(size_t) i].size()));
                button->setTooltip (tooltips[i]);
                button->onClick = [this, i]
                {
                    if (juce::roundToInt (wave.getValue()) != i)
                        wave.setValue ((float) i);

                    update();
                };
                addAndMakeVisible (button);
            }

            update();
        }

        void update()
        {
            const auto played = juce::roundToInt (wave.getValue());

            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setToggleState (i == played, juce::dontSendNotification);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            const auto width = (area.getWidth() - (buttons.size() - 1) * 2) / std::max (1, buttons.size());

            for (auto* button : buttons)
            {
                button->setBounds (button == buttons.getLast() ? area : area.removeFromLeft (width));
                area.removeFromLeft (2);
            }
        }

    private:
        class WaveButton : public juce::Button
        {
        public:
            explicit WaveButton (int shownWave)
                : juce::Button ({}),
                  shape ((FmSynth::Wave) shownWave)
            {
            }

            void paintButton (juce::Graphics& g, bool over, bool down) override
            {
                const auto& p = getPalette (*this);
                const auto box = getLocalBounds().toFloat().reduced (0.5f);
                const auto on = getToggleState();
                g.setColour (on ? p.selected : over || down ? p.hover : p.surface);
                g.fillRoundedRectangle (box, cornerRadius);
                g.setColour (on ? getDeviceTint (*this) : hasKeyboardFocus (false) ? p.focus : p.controlBorder);
                g.drawRoundedRectangle (box, cornerRadius, 1.0f);

                const auto area = box.reduced (4.0f, 5.0f);
                juce::Path line;

                for (int i = 0; i <= 24; ++i)
                {
                    const auto place = (float) i / 24.0f;
                    const auto value = shape == FmSynth::Wave::sine       ? std::sin (juce::MathConstants<float>::twoPi * place)
                                       : shape == FmSynth::Wave::triangle ? (place < 0.25f   ? 4.0f * place
                                                                             : place < 0.75f ? 2.0f - 4.0f * place
                                                                                             : 4.0f * place - 4.0f)
                                       : shape == FmSynth::Wave::saw      ? (i == 24 ? -1.0f : 1.0f - 2.0f * place)
                                                                          : (place < 0.5f ? 1.0f : -1.0f);
                    const juce::Point<float> at { area.getX() + place * area.getWidth(),
                                                  area.getCentreY() - value * area.getHeight() * 0.5f };

                    if (i == 0)
                        line.startNewSubPath (at);
                    else
                        line.lineTo (at);
                }

                g.setColour (on ? p.textBright : over ? p.text : p.textMuted);
                g.strokePath (line, juce::PathStrokeType (1.25f));
            }

        private:
            const FmSynth::Wave shape;
        };

        ParameterHandle wave;
        juce::OwnedArray<WaveButton> buttons;
    };

    // A caption over what follows it in a group (the feedback's strip).
    class GroupCaption : public juce::Component
    {
    public:
        explicit GroupCaption (const juce::String& shown)
            : text (shown)
        {
            setInterceptsMouseClicks (false, false);
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (getPalette (*this).textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (text, getLocalBounds(), juce::Justification::centredLeft, true);
        }

    private:
        const juce::String text;
    };
} // namespace

//==============================================================================
// The tabs over the operators' groups, as the editor's page tabs are: a pill behind the group that shows and under the
// pointer.
class FmEditor::GroupTab : public juce::Button
{
public:
    GroupTab (FmEditor& e, Group shownGroup, const juce::String& name, const juce::String& tooltip)
        : juce::Button (name),
          editor (e),
          group (shownGroup)
    {
        setTitle (name);
        setTooltip (tooltip);
        setRadioGroupId (0x464d0001);
        onClick = [this] { editor.showGroup (group); };
    }

    int getIdealWidth() const
    {
        return juce::roundToInt (juce::GlyphArrangement::getStringWidth (juce::Font (juce::FontOptions (TextSize::small)), getButtonText()))
               + 2 * Space::m;
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& p = getPalette (*this);
        const auto front = getToggleState();
        const auto pill = getLocalBounds().toFloat();

        if (front || over || down)
        {
            g.setColour (front ? p.selectedInactive : p.hover);
            g.fillRoundedRectangle (pill, cornerRadius);
        }

        if (hasKeyboardFocus (false))
        {
            g.setColour (p.focus);
            g.drawRoundedRectangle (pill.reduced (0.5f), cornerRadius, 1.0f);
        }

        g.setColour (front ? p.textBright : over ? p.text : p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (getButtonText(), pill, juce::Justification::centred, false);
    }

private:
    FmEditor& editor;
    const Group group;
};

//==============================================================================
// The algorithm: its name between the arrows that step through the eight, its picture, and under it the harmonics the
// operators make together for a note at middle C as it strikes, or as the newest note plays (FmSynth::draw()), as FM is
// taught: the more modulation, the more harmonics and the higher they reach, where the ratios put them. A click shows all
// eight; the arrow keys step through them.
class FmEditor::AlgorithmView : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    explicit AlgorithmView (FmEditor& e)
        : editor (e),
          algorithm (e.getDevice().getParameter (getID (FmSynth::algorithm))),
          drawing (std::make_unique<FmSynth::Drawing>())
    {
        setTitle (getParameterName (FmSynth::algorithm));
        setWantsKeyboardFocus (true);

        for (const auto& parameter : FmSynth::info.parameters)
            parameters.push_back (e.getDevice().getParameter (parameter.id));

        for (auto* step : { &previous, &next })
        {
            step->setWantsKeyboardFocus (false);
            addAndMakeVisible (*step);
        }

        previous.onClick = [this] { stepBy (-1); };
        next.onClick = [this] { stepBy (1); };
        refresh();
    }

    int getChosen() const { return std::clamp (juce::roundToInt (algorithm.getValue()), 0, FmSynth::numAlgorithms - 1); }

    juce::Button& getStep (bool forward) { return forward ? next : previous; }

    // The operator whose box is under the pointer, or -1.
    int getBoxUnderPointer() const noexcept { return boxUnderPointer; }

    void stepBy (int steps)
    {
        algorithm.setValue ((float) ((getChosen() + steps + FmSynth::numAlgorithms) % FmSynth::numAlgorithms));
        refresh();
    }

    void refresh()
    {
        // The operators' envelopes as the newest note plays them, in steps the eye sees, or at their peaks while none plays.
        auto* synth = editor.getDevice().getProcessor<FmSynth>();
        const auto playing = synth != nullptr && synth->getActiveVoices() > 0;
        std::array<float, FmSynth::numOperators> envelopes {};
        std::vector<float> values;
        values.reserve (parameters.size());

        for (const auto& each : parameters)
            values.push_back (each.getValue());

        auto numbers = values;

        for (size_t op = 0; op < envelopes.size(); ++op)
        {
            envelopes[op] = playing ? std::round (synth->getShown().envelopes[op].get() * 50.0f) / 50.0f : 1.0f;
            numbers.push_back (envelopes[op]);
        }

        numbers.push_back (hasKeyboardFocus (false) ? 1.0f : 0.0f);
        numbers.push_back ((float) editor.getHoveredOperator());
        numbers.push_back (isMouseOver (true) ? 1.0f : 0.0f);

        if (! drawn.changed (std::move (numbers)))
            return;

        FmSynth::draw (values, envelopes, *drawing);
        lit = playing ? envelopes : std::array<float, FmSynth::numOperators> {};
        measureHarmonics();

        // What a screen reader reads of it: the algorithm it plays, and what it is good for.
        const auto chosen = getChosen();
        setDescription ("Algorithm " + juce::String (chosen + 1) + " of 8, " + getAlgorithmName (chosen) + ": " + algorithmUses[chosen]
                        + ". Click to see all eight, or step through them with the arrow keys.");
        setTooltip (juce::String (algorithmUses[chosen])
                    + ". Click for all eight. Under it, the harmonics the operators make together as a note strikes (or as the "
                      "newest note plays): the more modulation, the more of them and the higher they reach");
        repaint();
    }

    // Each half harmonic's level in what is heard, over its two cycles (Goertzel's), against the loudest.
    void measureHarmonics()
    {
        std::array<double, shownHalves> levels {};
        auto loudest = 0.0;

        for (int half = 1; half <= shownHalves; ++half)
        {
            const auto coefficient = 2.0 * std::cos (juce::MathConstants<double>::twoPi * half / FmSynth::drawnLength);
            auto s1 = 0.0, s2 = 0.0;

            for (const auto sample : drawing->heard)
            {
                const auto s0 = (double) sample + coefficient * s1 - s2;
                s2 = s1;
                s1 = s0;
            }

            levels[(size_t) half - 1] = std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2));
            loudest = std::max (loudest, levels[(size_t) half - 1]);
        }

        for (size_t half = 0; half < spectrum.size(); ++half)
            spectrum[half] = loudest > 1.0e-9 && levels[half] > 1.0e-12 ? (float) (20.0 * std::log10 (levels[half] / loudest)) : -200.0f;
    }

    void resized() override
    {
        auto row = getLocalBounds().removeFromTop (stepHeight);
        previous.setBounds (row.removeFromLeft (stepHeight));
        next.setBounds (row.removeFromRight (stepHeight));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto chosen = getChosen();
        auto area = getLocalBounds().toFloat();
        const auto title = area.removeFromTop ((float) stepHeight).reduced ((float) stepHeight, 0.0f);

        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (isMouseOver (true) && ! previous.isMouseOver() && ! next.isMouseOver() ? p.textBright : p.text);
        g.drawText (getAlgorithmName (chosen), title, juce::Justification::centred, true);

        area.removeFromTop (2.0f);
        graph::paintBackground (g, p, area);

        if (hasKeyboardFocus (false))
        {
            g.setColour (p.focus);
            g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.5f);
        }

        // The harmonics along the bottom, under what comes out of the picture: a bar for each up to the 16th, as high as it
        // is loud against the loudest, over 48 dB; the halves between them (an operator at half a ratio) narrower.
        const auto bars = area.removeFromBottom ((float) soundHeight).reduced (6.0f, 3.0f);
        const auto step = bars.getWidth() / (float) shownHalves;
        g.setColour (getDeviceTint (*this));

        for (size_t half = 0; half < spectrum.size(); ++half)
        {
            const auto whole = half % 2 == 1; // the bars go up in half harmonics, from the first half
            const auto height = std::clamp ((spectrum[half] + spectrumRange) / spectrumRange, 0.0f, 1.0f) * bars.getHeight();
            const auto width = whole ? step - 1.0f : std::max (1.0f, step * 0.5f);
            const auto x = bars.getX() + ((float) half + 0.5f) * step - width * 0.5f;

            if (height > 0.5f)
                g.fillRect (juce::Rectangle<float> (x, bars.getBottom() - height, width, height));
        }

        // The algorithm's picture over it.
        Looks looks;

        for (int op = 0; op < FmSynth::numOperators; ++op)
        {
            looks[(size_t) op].lit = lit[(size_t) op];
            looks[(size_t) op].on = parameters[(size_t) FmSynth::getParameterIndex (op, Field::on)].getValue() > 0.5f;
            looks[(size_t) op].feedback = parameters[(size_t) FmSynth::getParameterIndex (op, Field::feedback)].getValue();
            looks[(size_t) op].hovered = editor.getHoveredOperator() == op;
        }

        boxes = paintAlgorithm (g, *this, area.reduced (4.0f, 0.0f).withTrimmedTop (2.0f), chosen, looks, 20.0f);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto under = -1;

        for (int op = 0; op < FmSynth::numOperators; ++op)
            if (boxes[(size_t) op].expanded (2.0f).contains (e.position))
                under = op;

        if (under != boxUnderPointer)
        {
            boxUnderPointer = under;
            editor.refresh();
        }
    }

    // Its name lit while the pointer is over it (but for its arrows), as what a click opens.
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }

    void mouseExit (const juce::MouseEvent&) override
    {
        boxUnderPointer = -1;
        editor.refresh();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        grabKeyboardFocus();
        editor.showAlgorithmGrid();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto steps = key == juce::KeyPress::leftKey || key == juce::KeyPress::upKey      ? -1
                           : key == juce::KeyPress::rightKey || key == juce::KeyPress::downKey ? 1
                                                                                               : 0;

        if (steps == 0)
            return false;

        stepBy (steps);
        return true;
    }

    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

private:
    // An arrow beside the algorithm's name, which steps to the one before or after it.
    class StepButton : public juce::Button
    {
    public:
        explicit StepButton (bool forward)
            : juce::Button (forward ? "Next algorithm" : "Previous algorithm"),
              right (forward)
        {
            setTitle (getName());
            setTooltip (forward ? "Plays the next algorithm" : "Plays the algorithm before");
        }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto& p = getPalette (*this);
            const auto area = getLocalBounds().toFloat();

            if (over || down)
            {
                g.setColour (p.hover);
                g.fillRoundedRectangle (area, cornerRadius);
            }

            const auto centre = area.getCentre();
            const auto size = 3.5f;
            juce::Path chevron;
            chevron.startNewSubPath (centre.x + (right ? -size * 0.5f : size * 0.5f), centre.y - size);
            chevron.lineTo (centre.x + (right ? size * 0.5f : -size * 0.5f), centre.y);
            chevron.lineTo (centre.x + (right ? -size * 0.5f : size * 0.5f), centre.y + size);
            g.setColour (over ? p.textBright : p.text);
            g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

    private:
        const bool right;
    };

    FmEditor& editor;
    ParameterHandle algorithm;
    std::vector<ParameterHandle> parameters;
    StepButton previous { false }, next { true };
    std::unique_ptr<FmSynth::Drawing> drawing;
    static constexpr int shownHalves = 32;        // the harmonics shown, in halves, up to the 16th
    static constexpr float spectrumRange = 48.0f; // decibels under the loudest
    std::array<float, shownHalves> spectrum {};   // each half harmonic's level, in decibels against the loudest
    std::array<float, FmSynth::numOperators> lit {};
    std::array<juce::Rectangle<float>, FmSynth::numOperators> boxes {};
    int boxUnderPointer = -1;
    DrawnNumbers drawn;
};

//==============================================================================
// All eight algorithms, in two rows of four over the window, each with its number, its name, its picture and what it is good
// for, the one played lit; a click plays one, as one undo step, and closes them.
class FmEditor::AlgorithmGrid : public juce::Component
{
public:
    static constexpr int cellWidth = 148, cellHeight = 164, columns = 4;

    explicit AlgorithmGrid (FmEditor& e)
        : editor (&e),
          algorithm (e.getDevice().getParameter (getID (FmSynth::algorithm)))
    {
        setTitle ("Algorithms");
        setDescription ("The eight algorithms: which operators modulate which, and which are heard. Click one to play it.");

        for (int op = 0; op < FmSynth::numOperators; ++op)
        {
            looks[(size_t) op].on = e.getDevice().getParameter (getID (op, Field::on)).getValue() > 0.5f;
            looks[(size_t) op].feedback = e.getDevice().getParameter (getID (op, Field::feedback)).getValue();
        }

        setSize (columns * cellWidth, (FmSynth::numAlgorithms / columns) * cellHeight);
    }

    juce::Rectangle<int> getCell (int index) const
    {
        return { (index % columns) * cellWidth, (index / columns) * cellHeight, cellWidth, cellHeight };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto chosen = juce::roundToInt (algorithm.getValue());

        for (int index = 0; index < FmSynth::numAlgorithms; ++index)
        {
            const auto cell = getCell (index).toFloat().reduced (3.0f);

            if (index == chosen || index == hovered)
            {
                g.setColour (index == chosen ? p.selected : p.hover);
                g.fillRoundedRectangle (cell, cornerRadius);
            }

            if (index == chosen)
            {
                g.setColour (getDeviceTint (*this));
                g.drawRoundedRectangle (cell.reduced (0.5f), cornerRadius, 1.5f);
            }

            auto inside = cell.reduced (8.0f, 6.0f);
            const auto title = inside.removeFromTop (16.0f);
            g.setFont (juce::FontOptions (TextSize::small));
            g.setColour (p.textMuted);
            g.drawText (juce::String (index + 1), title, juce::Justification::centredLeft, false);
            g.setColour (index == chosen ? p.textBright : p.text);
            g.drawText (getAlgorithmName (index), title.withTrimmedLeft (14.0f), juce::Justification::centredLeft, true);

            const auto uses = inside.removeFromBottom (44.0f);
            g.setColour (p.textMuted);
            g.drawFittedText (algorithmUses[index], uses.toNearestInt(), juce::Justification::topLeft, 3, 1.0f);
            paintAlgorithm (g, *this, inside.reduced (10.0f, 2.0f), index, looks, 16.0f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (findCell (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto index = findCell (e.getPosition());

        if (index < 0 || e.mods.isPopupMenu())
            return;

        if (index != juce::roundToInt (algorithm.getValue()))
            algorithm.setValue ((float) index);

        if (editor != nullptr)
            editor->refresh();

        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

private:
    juce::Component::SafePointer<FmEditor> editor;
    ParameterHandle algorithm;
    Looks looks;
    int hovered = -1;

    int findCell (juce::Point<int> position) const
    {
        for (int index = 0; index < FmSynth::numAlgorithms; ++index)
            if (getCell (index).contains (position))
                return index;

        return -1;
    }

    void setHovered (int index)
    {
        if (index != hovered)
        {
            hovered = index;
            repaint();
        }
    }
};

//==============================================================================
// An operator's column: its switch, its number in its role's colour and what it does in the algorithm, its envelope, its
// level and its ratio and fine (or its frequency, while fixed); and under the tabs, its group of controls shown. It shows
// faint while the operator is off.
class FmEditor::OperatorColumn : public juce::Component
{
public:
    OperatorColumn (FmEditor& e, int index)
        : op (index),
          fixed (e.getDevice().getParameter (getID (index, Field::fixed))),
          on (e.getDevice().getParameter (getID (index, Field::on)))
    {
        const auto& handle = e.getDevice();
        const auto make = [this, &handle, &e] (Field field, const juce::String& caption, ParameterStrip::Look look) -> ParameterStrip&
        {
            auto& strip = e.keep (std::make_unique<ParameterStrip> (handle, FmSynth::getParameterIndex (op, field), caption, look));
            e.addControl (getID (op, field), strip);
            strips.push_back (&strip);
            return strip;
        };

        // The head: its switch and its header, then what its switch shows faint while it is off.
        powerSwitch = &e.makePartSwitch (getID (op, Field::on), content);
        addAndMakeVisible (*powerSwitch);
        addAndMakeVisible (header);
        addAndMakeVisible (content);

        auto shown = [&e, index]
        {
            auto* synth = e.getDevice().getProcessor<FmSynth>();
            return synth != nullptr ? synth->getShown().envelopes[(size_t) index].get() : 0.0f;
        };

        envelope = &e.keep (std::make_unique<EnvelopeView> (
            "Operator " + juce::String (op + 1) + " envelope", handle.getParameter (getID (op, Field::attack)),
            handle.getParameter (getID (op, Field::decay)), handle.getParameter (getID (op, Field::sustain)),
            handle.getParameter (getID (op, Field::release)), shown));
        envelope->setCurves (handle.getParameter (getID (op, Field::attackCurve)), handle.getParameter (getID (op, Field::decayCurve)),
                             handle.getParameter (getID (op, Field::releaseCurve)));
        content.addAndMakeVisible (*envelope);

        level = &make (Field::level, "Level", ParameterStrip::Look::amount);
        level->fillColour = [this] { return getRoleColour (*level, FmSynth::isCarrier (std::max (0, shownAlgorithm), op)); };
        ratio = &make (Field::ratio, "Ratio", ParameterStrip::Look::field);
        fine = &make (Field::fine, {}, ParameterStrip::Look::field);
        frequency = &make (Field::frequency, "Hz", ParameterStrip::Look::field);
        ratio->textFromValueFunction = [] (double value)
        { return juce::String::fromUTF8 ("\xc3\x97") + juce::String (value, juce::exactlyEqual (value, std::round (value)) ? 0 : 1); };
        fine->textFromValueFunction = [] (double value)
        { return (value > 0.0 ? "+." : ".") + juce::String (juce::roundToInt (value * 100.0)).paddedLeft ('0', 2); };
        frequency->textFromValueFunction = [] (double value)
        { return value >= 1000.0 ? juce::String (value / 1000.0, 2) + "k" : juce::String (value, value < 100.0 ? 1 : 0); };

        for (auto* strip : { level, ratio, fine, frequency })
            content.addAndMakeVisible (*strip);

        // The groups: the envelope's times, sustain and bends; the wave and the feedback; Fixed and the detune; velocity and
        // key scaling.
        attack = &make (Field::attack, "A", ParameterStrip::Look::field);
        decay = &make (Field::decay, "D", ParameterStrip::Look::field);
        sustain = &make (Field::sustain, "S", ParameterStrip::Look::field);
        release = &make (Field::release, "R", ParameterStrip::Look::field);
        attackBend = &make (Field::attackCurve, {}, ParameterStrip::Look::bend);
        decayBend = &make (Field::decayCurve, {}, ParameterStrip::Look::bend);
        releaseBend = &make (Field::releaseCurve, {}, ParameterStrip::Look::bend);
        decayBend->rises = releaseBend->rises = false;
        feedback = &make (Field::feedback, {}, ParameterStrip::Look::amount);
        feedback->textFromValueFunction = [] (double value)
        {
            const auto percent = juce::String (juce::roundToInt (std::abs (value) * 100.0)) + "%";
            return value > 0.005 ? "Saw " + percent : value < -0.005 ? "Square " + percent : juce::String ("No feedback");
        };
        detune = &make (Field::detune, "Detune", ParameterStrip::Look::field);
        velocity = &make (Field::velocity, "Velocity", ParameterStrip::Look::amount);
        keyScale = &make (Field::keyScale, "Key", ParameterStrip::Look::amount);

        wave = &e.keep (std::make_unique<WaveChoice> (handle, op));
        e.addControl (getID (op, Field::wave), *wave);
        fixedSwitch = &e.keep (std::make_unique<ParameterSwitch> (
            handle, getID (op, Field::fixed), "Fixed",
            "Plays the operator at a fixed frequency, whatever the note, rather than at a ratio of its pitch: for drums, noise and "
            "metallic sounds"));
        e.addControl (getID (op, Field::fixed), *fixedSwitch);
        feedbackCaption = &e.keep (std::make_unique<GroupCaption> ("Feedback"));

        groups[(size_t) Group::envelope].addAndMakeVisible (*attack);
        groups[(size_t) Group::envelope].addAndMakeVisible (*attackBend);
        groups[(size_t) Group::envelope].addAndMakeVisible (*decay);
        groups[(size_t) Group::envelope].addAndMakeVisible (*decayBend);
        groups[(size_t) Group::envelope].addAndMakeVisible (*sustain);
        groups[(size_t) Group::envelope].addAndMakeVisible (*release);
        groups[(size_t) Group::envelope].addAndMakeVisible (*releaseBend);
        groups[(size_t) Group::tone].addAndMakeVisible (*wave);
        groups[(size_t) Group::tone].addAndMakeVisible (*feedbackCaption);
        groups[(size_t) Group::tone].addAndMakeVisible (*feedback);
        groups[(size_t) Group::pitch].addAndMakeVisible (*fixedSwitch);
        groups[(size_t) Group::pitch].addAndMakeVisible (*detune);
        groups[(size_t) Group::scaling].addAndMakeVisible (*velocity);
        groups[(size_t) Group::scaling].addAndMakeVisible (*keyScale);

        for (auto& group : groups)
        {
            group.setInterceptsMouseClicks (false, true);
            groupArea.addChildComponent (group);
        }

        groupArea.setInterceptsMouseClicks (false, true);
        setTitle ("Operator " + juce::String (op + 1));
    }

    // The column's group, under the tabs, which the operators' part places and shows faint with the column.
    juce::Component& getGroupArea() noexcept { return groupArea; }

    void showGroup (Group shown)
    {
        for (size_t g = 0; g < groups.size(); ++g)
            groups[g].setVisible (g == (size_t) shown);
    }

    juce::String getRoleText() const { return roleText; }

    // Its role in the algorithm played, its controls' values, and whether it is lit as the pointer is over it or its box.
    void refresh (int algorithm, bool lit)
    {
        for (auto* strip : strips)
            strip->update();

        wave->update();

        const auto isFixed = fixed.getValue() > 0.5f;
        ratio->setVisible (! isFixed);
        fine->setVisible (! isFixed);
        frequency->setVisible (isFixed);
        const auto alpha = on.getValue() > 0.5f ? 1.0f : SwitchTint::partOff;
        header.setAlpha (alpha);
        groupArea.setAlpha (alpha);

        const auto carrier = FmSynth::isCarrier (algorithm, op);

        if (algorithm != shownAlgorithm || lit != shownLit)
        {
            // Its role and what its controls' tips say only change with the algorithm.
            if (algorithm != shownAlgorithm)
            {
                roleText = getTargetsText (algorithm, op);
                describe (algorithm, carrier);
                level->repaint();
            }

            shownAlgorithm = algorithm;
            shownLit = lit;
            header.show (op, roleText, carrier, lit);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (headerHeight);
        powerSwitch->setBounds (top.removeFromLeft (SwitchTint::size).withSizeKeepingCentre (SwitchTint::size, SwitchTint::size));
        top.removeFromLeft (Space::xs);
        header.setBounds (top);
        area.removeFromTop (rowGap);
        content.setBounds (area);
        auto inside = content.getLocalBounds();
        envelope->setBounds (inside.removeFromTop (envelopeHeight));
        inside.removeFromTop (rowGap);
        level->setBounds (inside.removeFromTop (stripHeight));
        inside.removeFromTop (rowGap);
        auto tune = inside.removeFromTop (stripHeight);
        frequency->setBounds (tune);
        fine->setBounds (tune.removeFromRight (bendWidth));
        tune.removeFromRight (rowGap);
        ratio->setBounds (tune);
    }

    // The group's area, laid out as the operators' part gives it.
    void layOutGroups()
    {
        const auto area = groupArea.getLocalBounds();

        for (auto& group : groups)
            group.setBounds (area);

        const auto rowAt = [&area] (int row) { return area.withY (row * (stripHeight + groupRowGap)).withHeight (stripHeight); };
        const auto withBend = [] (juce::Rectangle<int> row, ParameterStrip& time, ParameterStrip* bend)
        {
            const auto right = row.removeFromRight (bendWidth);

            if (bend != nullptr)
                bend->setBounds (right);

            row.removeFromRight (rowGap);
            time.setBounds (row);
        };

        withBend (rowAt (0), *attack, attackBend);
        withBend (rowAt (1), *decay, decayBend);
        withBend (rowAt (2), *sustain, nullptr);
        withBend (rowAt (3), *release, releaseBend);

        auto tone = area;
        wave->setBounds (tone.removeFromTop (stripHeight + 3));
        tone.removeFromTop (groupRowGap);
        feedbackCaption->setBounds (tone.removeFromTop (14));
        feedback->setBounds (tone.removeFromTop (stripHeight));

        auto pitch = area;
        fixedSwitch->setBounds (pitch.removeFromTop (stripHeight + 3));
        pitch.removeFromTop (groupRowGap);
        detune->setBounds (pitch.removeFromTop (stripHeight));

        velocity->setBounds (rowAt (0));
        keyScale->setBounds (rowAt (1));
    }

private:
    // The column's head: its number in its role's colour, lit while the pointer is over it or its box, and what it does.
    class Header : public juce::Component,
                   public juce::SettableTooltipClient
    {
    public:
        Header() { setInterceptsMouseClicks (false, false); }

        void show (int shownOp, const juce::String& shownRole, bool isCarrier, bool isLit)
        {
            op = shownOp;
            role = shownRole;
            carrier = isCarrier;
            lit = isLit;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& p = getPalette (*this);
            auto area = getLocalBounds().toFloat();
            const auto badge = area.removeFromLeft (20.0f).withSizeKeepingCentre (20.0f, 16.0f);
            const auto colour = getRoleColour (*this, carrier);
            const auto fill = p.background.interpolatedWith (colour, 0.6f);
            g.setColour (fill);
            g.fillRoundedRectangle (badge, cornerRadius);

            if (lit)
            {
                g.setColour (p.focus);
                g.drawRoundedRectangle (badge.expanded (1.0f), cornerRadius + 1.0f, 1.5f);
            }

            g.setColour (getInkFor (fill));
            g.setFont (juce::Font (juce::FontOptions (TextSize::small)).boldened());
            g.drawText (juce::String (op + 1), badge, juce::Justification::centred, false);
            area.removeFromLeft (5.0f);
            g.setColour (carrier ? p.text : p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (role, area, juce::Justification::centredLeft, true);
        }

    private:
        int op = 0;
        juce::String role;
        bool carrier = false, lit = false;
    };

    const int op;
    ParameterHandle fixed, on;
    juce::Component content, groupArea;
    std::array<juce::Component, numGroups> groups;
    Header header;
    juce::Component* powerSwitch = nullptr;
    EnvelopeView* envelope = nullptr;
    ParameterStrip *level = nullptr, *ratio = nullptr, *fine = nullptr, *frequency = nullptr;
    ParameterStrip *attack = nullptr, *decay = nullptr, *sustain = nullptr, *release = nullptr;
    ParameterStrip *attackBend = nullptr, *decayBend = nullptr, *releaseBend = nullptr;
    ParameterStrip *feedback = nullptr, *detune = nullptr, *velocity = nullptr, *keyScale = nullptr;
    WaveChoice* wave = nullptr;
    juce::Component* fixedSwitch = nullptr;
    juce::Component* feedbackCaption = nullptr;
    std::vector<ParameterStrip*> strips;
    juce::String roleText;
    int shownAlgorithm = -1;
    bool shownLit = false;

    // What its controls do in its role, as their tooltips (and the hint bar) say it.
    void describe (int algorithm, bool carrier)
    {
        const auto number = juce::String (op + 1);
        const auto bends = carrier ? juce::String() : describeTargets (algorithm, op);
        const auto drag = juce::String (". Drag up or down, double-click for where it started, or right-click to automate or modulate it");

        header.setTooltip (carrier
                               ? "Operator " + number + " is a carrier: you hear it. Its level is how loud it plays, its ratio its pitch"
                               : "Operator " + number + " is a modulator: you don't hear it, it bends the wave of " + bends
                                     + ". Its level is how bright that sounds, its ratio which overtones it adds");
        level->setTooltip ((carrier ? "How loud operator " + number + " plays"
                                    : "How strongly operator " + number + " bends " + bends + ": the higher, the brighter")
                           + drag);
        ratio->setTooltip (
            (carrier ? "Operator " + number + "'s pitch, a multiple of the note's: 1 plays the note, 2 an octave up, 0.5 an octave down"
                     : "Which overtones operator " + number + " adds to " + bends
                           + ": 1 gives a saw-like tone, 2 a hollow, square-like one, higher ones brighter")
            + drag);
        fine->setTooltip ("Moves operator " + number + "'s ratio between its steps, out of tune with the note: bells, metal and chimes"
                          + drag);
        frequency->setTooltip ("Operator " + number + "'s fixed frequency, the same for every note" + drag);
        feedback->setTooltip ("Feeds operator " + number
                              + "'s wave back into itself: to the right toward a saw (every harmonic), to the left toward a square (odd "
                                "harmonics only), each brightest at 50 % (the DX7's feedback 6); further, rougher, toward noise at 100 % "
                                "(its 7)"
                              + drag);
        detune->setTooltip ("Pulls operator " + number + " a few cents off its ratio: two operators a little apart beat, as a chorus does"
                            + drag);
        velocity->setTooltip ("How much a harder note makes operator " + number + (carrier ? " louder" : " bend harder, brighter") + drag);
        keyScale->setTooltip ("How much operator " + number
                              + " falls each octave up from middle C (rises below it), so that high notes don't turn harsh; below 0, the "
                                "other way"
                              + drag);
        attack->setTooltip ("How long operator " + number + " takes to reach its peak" + drag);
        decay->setTooltip ("How long it then takes to fall to its sustain" + drag);
        sustain->setTooltip ("Where it stays while the key is held" + drag);
        release->setTooltip ("How long it takes to fall silent after the key is let go" + drag);

        for (auto* bend : { attackBend, decayBend, releaseBend })
            bend->setTooltip ("How the stage beside it bends: to the left fast at first, to the right slow at first. The envelope's "
                              "handles bend it too"
                              + drag);
    }
};

//==============================================================================
// The operators' part: the algorithm and Timbre down its left, and beside them the operators' columns, their groups' tabs and
// their groups.
class FmEditor::OperatorsPart : public juce::Component
{
public:
    OperatorsPart (AlgorithmView& view, ParameterControls& timbreControls, std::array<OperatorColumn*, numOperators> shownColumns,
                   std::array<GroupTab*, numGroups> shownTabs)
        : algorithmView (view),
          timbre (timbreControls),
          columns (shownColumns),
          tabs (shownTabs)
    {
        addAndMakeVisible (algorithmView);
        addAndMakeVisible (timbre);

        for (auto* column : columns)
        {
            addAndMakeVisible (column);
            addAndMakeVisible (column->getGroupArea());
        }

        for (auto* tab : tabs)
            addAndMakeVisible (tab);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (getPalette (*this).border);
        g.fillRect (rule.withSizeKeepingCentre (rule.getWidth(), 1));
    }

    static int getColumnWidth() { return 96; }

    static int getPreferredWidth()
    {
        return algorithmWidth + algorithmGap + numOperators * getColumnWidth() + (numOperators - 1) * columnGap;
    }

    static int getHeadHeight() { return headerHeight + rowGap + envelopeHeight + rowGap + stripHeight + rowGap + stripHeight; }
    static int getGroupHeight() { return 4 * stripHeight + 3 * groupRowGap; }
    static int getPreferredHeight() { return getHeadHeight() + tabGap + tabHeight + groupGap + getGroupHeight(); }

    void resized() override
    {
        auto area = getLocalBounds();
        auto left = area.removeFromLeft (algorithmWidth);
        const auto timbreHeight = timbre.getPreferredHeight();
        timbre.setBounds (left.removeFromBottom (timbreHeight).withSizeKeepingCentre (timbre.getPreferredWidth(), timbreHeight));
        left.removeFromBottom (timbreGap);
        algorithmView.setBounds (left);
        area.removeFromLeft (algorithmGap);

        const auto heads = area.removeFromTop (getHeadHeight());
        area.removeFromTop (tabGap);
        auto tabRow = area.removeFromTop (tabHeight);
        area.removeFromTop (groupGap);
        const auto groups = area.withHeight (std::min (area.getHeight(), getGroupHeight()));

        for (int op = 0; op < numOperators; ++op)
        {
            const auto x = heads.getX() + op * (getColumnWidth() + columnGap);
            columns[(size_t) op]->setBounds (x, heads.getY(), getColumnWidth(), heads.getHeight());
            columns[(size_t) op]->getGroupArea().setBounds (x, groups.getY(), getColumnWidth(), groups.getHeight());
            columns[(size_t) op]->layOutGroups();
        }

        // The tabs as wide as their names, from the left, so that none sits over one column alone as if it were that
        // operator's; a line on from them to the right, over the groups of all four.
        for (auto* tab : tabs)
        {
            tab->setBounds (tabRow.removeFromLeft (tab->getIdealWidth()));
            tabRow.removeFromLeft (2);
        }

        rule = tabRow.withTrimmedLeft (Space::s);
    }

private:
    AlgorithmView& algorithmView;
    ParameterControls& timbre;
    std::array<OperatorColumn*, numOperators> columns;
    std::array<GroupTab*, numGroups> tabs;
    juce::Rectangle<int> rule;
};

//==============================================================================
// Which operator's column or group the pointer is over, from their mouse events and their children's: the operator to light
// in the algorithm's picture.
class FmEditor::HoverWatcher : public juce::MouseListener
{
public:
    explicit HoverWatcher (FmEditor& e)
        : editor (e)
    {
    }

    void mouseEnter (const juce::MouseEvent& e) override { show (e); }
    void mouseMove (const juce::MouseEvent& e) override { show (e); }

    void mouseExit (const juce::MouseEvent& e) override
    {
        // Out of a control into another of the same column keeps it.
        const auto op = findOperator (e);

        if (op >= 0 && op == editor.hoveredColumn && ! isInside (op, e))
            editor.hoveredColumn = -1;
    }

private:
    FmEditor& editor;

    int findOperator (const juce::MouseEvent& e) const
    {
        for (int op = 0; op < numOperators; ++op)
        {
            auto& column = *editor.columns[(size_t) op];

            if (e.eventComponent == &column || column.isParentOf (e.eventComponent) || e.eventComponent == &column.getGroupArea()
                || column.getGroupArea().isParentOf (e.eventComponent))
                return op;
        }

        return -1;
    }

    bool isInside (int op, const juce::MouseEvent& e) const
    {
        auto& column = *editor.columns[(size_t) op];
        return column.getLocalBounds().toFloat().contains (e.getEventRelativeTo (&column).position)
               || column.getGroupArea().getLocalBounds().toFloat().contains (e.getEventRelativeTo (&column.getGroupArea()).position);
    }

    void show (const juce::MouseEvent& e)
    {
        if (const auto op = findOperator (e); op >= 0)
            editor.hoveredColumn = op;
    }
};

//==============================================================================
FmEditor::FmEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    const auto parameter = [this] (int index) { return device.getParameter (getID (index)); };
    const auto shown = [this]() -> const FmSynth::Shown*
    {
        auto* synth = device.getProcessor<FmSynth>();
        return synth != nullptr ? &synth->getShown() : nullptr;
    };

    // The operators: the algorithm and Timbre, the four columns, and their groups' tabs.
    auto& view = keep (std::make_unique<AlgorithmView> (*this));
    algorithmView = &view;
    addControl (getID (FmSynth::algorithm), view);
    auto& timbre = makeControls ({ getID (FmSynth::timbre) }, 1);

    for (int op = 0; op < numOperators; ++op)
        columns[(size_t) op] = &keep (std::make_unique<OperatorColumn> (*this, op));

    hoverWatcher = std::make_unique<HoverWatcher> (*this);

    for (auto* column : columns)
    {
        column->addMouseListener (hoverWatcher.get(), true);
        column->getGroupArea().addMouseListener (hoverWatcher.get(), true);
    }

    groupTabs = { &keep (std::make_unique<GroupTab> (*this, Group::envelope, "Envelope",
                                                     "Shows the operators' envelopes: the times, the sustain and how each stage bends")),
                  &keep (std::make_unique<GroupTab> (*this, Group::tone, "Tone", "Shows the operators' waves and feedback")),
                  &keep (std::make_unique<GroupTab> (*this, Group::pitch, "Pitch",
                                                     "Shows whether the operators play at a fixed frequency, and their detune")),
                  &keep (std::make_unique<GroupTab> (*this, Group::scaling, "Scaling",
                                                     "Shows how much velocity and the key change the operators' levels")) };

    auto& part = keep (std::make_unique<OperatorsPart> (view, timbre, columns, groupTabs));
    operators = &part;
    const DeviceLayout::Part operatorsPart {
        "Operators", &part, OperatorsPart::getPreferredWidth(), OperatorsPart::getPreferredHeight(), OperatorsPart::getPreferredHeight(),
        false
    };

    // How it plays: its pitch envelope and the envelopes' Key Time, then its voices, Phase and Spread.
    const auto pitch = makeControlsPart ("Pitch", { "pitchAmount", "pitchDecay", "keyTime" }, 3, ParameterControls::Size::compact,
                                         { "Amount", "Decay", "Key Time" });
    const auto voice = makeControlsPart ("Voice", { "voiceMode", "glide", "bendRange", "phase", "spread", "level" }, 3,
                                         ParameterControls::Size::compact, { "Mode", "Glide", "Bend", "Phase", "Spread", "Level" });

    // The filter and the LFO, each switched on at its title.
    auto all = std::make_shared<std::vector<ParameterHandle>>();

    for (const auto& each : FmSynth::info.parameters)
        all->push_back (device.getParameter (each.id));

    auto& filterView = keep (std::make_unique<FilterView> (
        "Filter response", parameter (FmSynth::cutoff), parameter (FmSynth::resonance),
        [all]
        {
            std::vector<float> values;

            for (const auto& each : *all)
                values.push_back (each.getValue());

            return values;
        },
        [this] (const std::vector<float>& values, double cutoffHertz, double frequency)
        {
            auto* scope = device.getScope();
            return FmSynth::getFilterMagnitude (values, cutoffHertz, frequency, scope != nullptr ? scope->getSampleRate() : 48000.0);
        },
        [shown] { return shown() != nullptr ? shown()->cutoff.get() : 0.0f; }));
    auto filter = makePicturePart ("Filter", filterView, 70, 40, { "filterType", "cutoff", "resonance", "filterSlope", "keyTracking" }, 3,
                                   ParameterControls::Size::compact, { "Type", "Cutoff", "Resonance", "Slope", "Key" });
    filter.titleSwitch = &makePartSwitch ("filterOn", *filter.content);

    auto lfoShape = std::make_shared<ParameterHandle> (parameter (FmSynth::lfoShape));
    auto& lfoView = keep (std::make_unique<LfoView> (
        "LFO wave", [lfoShape] (float phaseAt) { return getDrawnLfoValue (juce::roundToInt (lfoShape->getValue()), phaseAt); },
        [shown] { return shown() != nullptr ? shown()->lfoPhase.get() : -1.0f; }));
    lfoView.setControls (parameter (FmSynth::lfoDepth), parameter (FmSynth::lfoRate));
    auto lfo = makePicturePart ("LFO", lfoView, 56, 32, { "lfoShape", "lfoRate", "lfoTarget", "lfoDepth" }, 2,
                                ParameterControls::Size::compact, { "Shape", "Rate", "Target", "Depth" });
    lfo.titleSwitch = &makePartSwitch ("lfoOn", *lfo.content);

    // The operators alone on the first page of the default dock, as wide as the dock; then how it plays, the filter and the LFO.
    setParts ({ { operatorsPart }, { pitch, voice }, { filter }, { lfo } });
    showGroup (Group::envelope);
    refresh();
}

FmEditor::~FmEditor()
{
    if (auto* box = gridBox.getComponent())
        box->dismiss();

    for (auto* column : columns)
    {
        column->removeMouseListener (hoverWatcher.get());
        column->getGroupArea().removeMouseListener (hoverWatcher.get());
    }
}

juce::Component& FmEditor::getAlgorithmView() const
{
    return *algorithmView;
}

juce::Button& FmEditor::getAlgorithmStep (bool next) const
{
    return algorithmView->getStep (next);
}

juce::Component* FmEditor::getAlgorithmGrid() const
{
    auto* box = gridBox.getComponent();
    return box != nullptr && box->isVisible() ? box->getChildComponent (0) : nullptr;
}

void FmEditor::showAlgorithmGrid()
{
    // Its own again closes it, as a click on the picture closes it (the call-out, which takes the click, closes itself then).
    if (auto* open = gridBox.getComponent())
    {
        open->dismiss();
        gridBox = nullptr;
        return;
    }

    // Over the window, as its other pop-ups are (Popups.h), beside the picture.
    auto* window = getTopLevelComponent();
    const auto anchor = window->getLocalArea (algorithmView, algorithmView->getLocalBounds());
    auto& box = juce::CallOutBox::launchAsynchronously (std::make_unique<AlgorithmGrid> (*this), anchor, window);

    if (findWindowForPopups (*this) != nullptr)
        FrostedGlass::putBehind (box, FrostedGlass::Shape::outlined, false);

    gridBox = &box;
}

juce::Button& FmEditor::getGroupTab (Group group) const
{
    return *groupTabs[(size_t) group];
}

void FmEditor::showGroup (Group group)
{
    shownGroup = group;

    for (size_t g = 0; g < groupTabs.size(); ++g)
        groupTabs[g]->setToggleState (g == (size_t) group, juce::dontSendNotification);

    for (auto* column : columns)
        column->showGroup (group);
}

juce::Component& FmEditor::getOperatorColumn (int op) const
{
    return *columns[(size_t) std::clamp (op, 0, numOperators - 1)];
}

juce::String FmEditor::getRoleText (int op) const
{
    return columns[(size_t) std::clamp (op, 0, numOperators - 1)]->getRoleText();
}

void FmEditor::setHovered (int op)
{
    hovered = op;
}

void FmEditor::refresh()
{
    // The operator under the pointer: its box, or else its column or its group.
    const auto box = algorithmView->getBoxUnderPointer();
    setHovered (box >= 0 ? box : hoveredColumn);

    const auto algorithm = algorithmView->getChosen();

    for (int op = 0; op < numOperators; ++op)
        columns[(size_t) op]->refresh (algorithm, op == hovered);

    algorithmView->refresh();
}
} // namespace phazite::ui
