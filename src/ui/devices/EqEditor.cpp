// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EqEditor.h"
#include "DeviceTint.h"
#include "ParameterControls.h"
#include "devices/dsp/Math.h"
#include "devices/eq/Equalizer.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace phazite::ui
{
namespace
{
    using Eq = devices::Equalizer;
    using Placement = Eq::Placement;

    constexpr double lowestFrequency = 20.0, highestFrequency = 20000.0;
    constexpr double gainRange = 24.0;      // the graph shows the bands' whole range, -24 to +24 dB
    constexpr double analyzerFloor = -90.0; // the analyzer's bottom, in dB; its top is 0 dB
    constexpr double defaultSampleRate = 48000.0;
    constexpr int bandsGroup = 1;              // the band buttons' radio group
    constexpr int graphWidth = 400;            // as wide at any height
    constexpr int bandRows = 2;                // of band buttons
    constexpr float shownGainStep = 0.01f;     // a dynamic band's gain as the curves show it: steps of this many decibels
    constexpr float dashes[] = { 4.0f, 3.0f }; // the side's and the right's curves, dashed

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;
    const auto knobCell = compactKnobSize + Space::l;

    // Beside the graph: the band buttons in two rows, the band's switch and shape, its slope and placement, then its knobs and
    // the output's.
    int getControlsWidth()
    {
        return EqEditor::numKnobs * knobCell;
    }

    int getControlsHeight()
    {
        return bandRows * controlHeight + Space::xs + Space::s + 2 * controlHeight + Space::xs + Space::s + compactKnobSize
               + 2 * labelHeight;
    }

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Eq::info.parameters[(size_t) index];
    }

    int indexOf (int band, Eq::Field field)
    {
        return Eq::getParameterIndex (band, field);
    }

    bool hasGain (int type)
    {
        return type == (int) Eq::BandType::lowShelf || type == (int) Eq::BandType::bell || type == (int) Eq::BandType::highShelf;
    }

    bool isCut (int type)
    {
        return type == (int) Eq::BandType::lowCut || type == (int) Eq::BandType::highCut;
    }

    juce::Colour getBandColour (int band)
    {
        return juce::Colour (partColours[(size_t) band % std::size (partColours)]);
    }

    double toDecibels (double magnitude)
    {
        return 20.0 * std::log10 (std::max (magnitude, 1.0e-6));
    }

    // A band's values, as a list of all of them holds them.
    struct BandValues
    {
        bool on;
        int type;
        Placement placement;
        float gain, dynamic;

        BandValues (const std::vector<float>& values, int band)
            : on (values[(size_t) indexOf (band, Eq::on)] > 0.5f),
              type (juce::roundToInt (values[(size_t) indexOf (band, Eq::type)])),
              placement ((Placement) juce::jlimit (0, 4, juce::roundToInt (values[(size_t) indexOf (band, Eq::placement)]))),
              gain (values[(size_t) indexOf (band, Eq::gain)]),
              dynamic (values[(size_t) indexOf (band, Eq::dynamic)])
        {
        }

        // A bell or a shelf whose gain moves with the sound.
        bool isDynamic() const { return on && hasGain (type) && std::abs (dynamic) > 0.0f; }
    };

    const char* getLetter (Placement placement)
    {
        switch (placement)
        {
            case Placement::stereo:
                return "";
            case Placement::mid:
                return "M";
            case Placement::side:
                return "S";
            case Placement::left:
                return "L";
            case Placement::right:
                return "R";
        }

        return "";
    }

    const char* getWayName (Placement placement)
    {
        switch (placement)
        {
            case Placement::stereo:
                return "Stereo";
            case Placement::mid:
                return "Mid";
            case Placement::side:
                return "Side";
            case Placement::left:
                return "Left";
            case Placement::right:
                return "Right";
        }

        return "";
    }
} // namespace

//==============================================================================
class EqEditor::Graph : public juce::Component
{
public:
    explicit Graph (EqEditor& e)
        : editor (e)
    {
        setTitle ("EQ curve");
        setDescription ("Drag a band's dot to move its frequency and gain, and a dynamic band's handle to change its Dynamic; the "
                        "wheel over a dot changes its Q, a double-click turns it on or off, and a right-click picks its shape and "
                        "placement");

        for (int band = 0; band < Eq::numBands; ++band)
            qs[(size_t) band] = editor.device.getParameter (getInfo (indexOf (band, Eq::q)).id);

        drag.onCancelled = [this] { editor.refresh(); };
    }

    float xFor (double frequency) const
    {
        return (float) (std::log (frequency / lowestFrequency) / std::log (highestFrequency / lowestFrequency) * getWidth());
    }

    double frequencyFor (float x) const
    {
        return lowestFrequency * std::pow (highestFrequency / lowestFrequency, juce::jlimit (0.0, 1.0, (double) x / getWidth()));
    }

    float yForGain (double decibels) const { return (float) ((0.5 - decibels / (2.0 * gainRange)) * getHeight()); }
    double gainFor (float y) const { return (0.5 - (double) y / getHeight()) * 2.0 * gainRange; }

    // A shelf's or bell's dot is at its gain; a cut's or notch's on the line at 0 dB.
    juce::Point<float> getDot (int band) const
    {
        const auto& shown = editor.values;
        const auto type = juce::roundToInt (shown[(size_t) indexOf (band, Eq::type)]);
        const auto gain = hasGain (type) ? (double) shown[(size_t) indexOf (band, Eq::gain)] : 0.0;
        return { xFor (shown[(size_t) indexOf (band, Eq::frequency)]), yForGain (juce::jlimit (-gainRange, gainRange, gain)) };
    }

    // A dynamic band's handle, where its gain goes at its whole Dynamic (kept in the graph); and the mark on its line where
    // its gain is now.
    std::optional<juce::Point<float>> getHandle (int band) const
    {
        const BandValues shown (editor.values, band);

        if (! shown.isDynamic())
            return std::nullopt;

        const auto dot = getDot (band);
        return juce::Point<float> (dot.x, yForGain (juce::jlimit (-gainRange, gainRange, (double) (shown.gain + shown.dynamic))));
    }

    float getMarkY (int band) const { return yForGain (juce::jlimit (-gainRange, gainRange, (double) editor.getShownGain (band))); }

    // The nearest dot within reach, or -1.
    int findBandAt (juce::Point<float> position) const
    {
        auto found = -1;
        auto nearest = GraphTint::dotRadius * 1.5f;

        for (int band = 0; band < Eq::numBands; ++band)
        {
            if (const auto distance = getDot (band).getDistanceFrom (position); distance <= nearest)
            {
                nearest = distance;
                found = band;
            }
        }

        return found;
    }

    // The nearest dynamic band's handle within reach, or -1.
    int findHandleAt (juce::Point<float> position) const
    {
        auto found = -1;
        auto nearest = GraphTint::grabDistance;

        for (int band = 0; band < Eq::numBands; ++band)
        {
            if (const auto handle = getHandle (band); handle.has_value())
            {
                if (const auto distance = handle->getDistanceFrom (position); distance <= nearest)
                {
                    nearest = distance;
                    found = band;
                }
            }
        }

        return found;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        g.setColour (palette.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);

        paintGrid (g, palette);

        // Behind its curve, what comes in, faint, then what the EQ plays, in its colour, each tilted alike.
        const graph::FrequencyAxis frequencies { getLocalBounds().toFloat(), lowestFrequency, highestFrequency };
        const graph::LevelAxis levels { getLocalBounds().toFloat(), (float) analyzerFloor, 0.0f };
        editor.input.paint (g, frequencies, levels, palette.textMuted.withAlpha (GraphTint::spectrumFill),
                            palette.textMuted.withAlpha (GraphTint::spectrumLine * 0.5f));
        const auto tint = getDeviceTint (*this);
        editor.output.paint (g, frequencies, levels, tint.withAlpha (GraphTint::spectrumFill * 1.5f),
                             tint.withAlpha (GraphTint::spectrumLine));

        // The selected band's own curve, filled to 0 dB, and the curve of every band together (or the curves of the ways through
        // it), worked out again only when they change (known bug 130): not at each of the analyzer's 30 repaints a second.
        updateCurves();
        g.setColour (getBandColour (editor.selectedBand).withAlpha (GraphTint::partCurve));
        g.fillPath (bandCurve);
        paintCurves (g, palette, tint);

        paintDynamics (g, palette);
        paintDots (g, palette);
    }

    // How many times its curves were worked out, for the tests.
    int getCurvesWorkedOut() const noexcept { return curvesWorkedOut; }

    // Over a dot, the pointer shows it can be grabbed, and the dot lights up; over a dynamic band's handle, that it moves up
    // and down, and the handle lights up.
    void mouseMove (const juce::MouseEvent& e) override { hover (findTarget (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { hover ({}); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto target = findTarget (e.position);

        if (target.band < 0)
            return;

        const auto band = target.band;
        editor.selectBand (band);

        if (e.mods.isPopupMenu())
        {
            showMenu (band);
            return;
        }

        // A dynamic band's handle: its Dynamic, from where it is set by hand, as one undo step.
        if (target.handle)
        {
            dragDynamic = editor.device.getParameter (getInfo (indexOf (band, Eq::dynamic)).id,
                                                      "Change Band " + juce::String (band + 1) + " Dynamic");
            draggingDynamic = true;
            dragBand = band;
            drag.take ({ &dragDynamic });
            return;
        }

        // Its frequency and, for a shelf or a bell, its gain, from where they are set by hand: one undo step.
        dragFrequency = editor.device.getParameter (getInfo (indexOf (band, Eq::frequency)).id, "Move Band " + juce::String (band + 1));
        dragGain = editor.device.getParameter (getInfo (indexOf (band, Eq::gain)).id);
        draggingGain = hasGain (juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::type)]));
        draggingDynamic = false;
        drag.take (draggingGain ? std::vector<ParameterHandle*> { &dragFrequency, &dragGain }
                                : std::vector<ParameterHandle*> { &dragFrequency });
    }

    // The dot moves as far as the pointer: a dot taken at its edge doesn't jump to the pointer. A handle moves up and down.
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        const auto moved = e.getOffsetFromDragStart().toFloat();

        if (draggingDynamic)
        {
            const auto gain = (double) editor.values[(size_t) indexOf (dragBand, Eq::gain)];
            drag.moveAlong (
                0, moved.y, [this, gain] (float dynamic) { return yForGain (gain + dynamic); },
                [this, gain] (float y) { return (float) juce::jlimit (-gainRange, gainRange, gainFor (y) - gain); });
            editor.refresh();
            return;
        }

        drag.moveAlong (
            0, moved.x, [this] (float hertz) { return xFor (hertz); },
            [this] (float x) { return (float) juce::jlimit (lowestFrequency, highestFrequency, frequencyFor (x)); });

        if (draggingGain)
            drag.moveAlong (
                1, moved.y, [this] (float decibels) { return yForGain (decibels); },
                [this] (float y) { return (float) juce::jlimit (-gainRange, gainRange, gainFor (y)); });

        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag.end();
        draggingDynamic = false;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const auto target = findTarget (e.position); target.band >= 0 && ! target.handle)
        {
            const auto band = target.band;
            auto on = editor.device.getParameter (getInfo (indexOf (band, Eq::on)).id);
            on.setValue (on.getValue() > 0.5f ? 0.0f : 1.0f);
            editor.refresh();
        }
    }

    // Over a dot, the wheel narrows or widens its band, as one undo step until it rests (known bug 118); elsewhere it is the
    // view's, to scroll it.
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const auto target = drag.isTaken() ? Target {} : findTarget (e.position);
        const auto band = target.handle ? -1 : target.band;

        if (band < 0)
        {
            juce::Component::mouseWheelMove (e, wheel);
            return;
        }

        const auto& info = getInfo (indexOf (band, Eq::q));
        auto& q = qs[(size_t) band];
        qWheel.turn (q, devices::constrain (info, q.getBaseValue() * std::pow (2.0f, 2.0f * wheel.deltaY)));
        editor.refresh();
    }

    // The ways whose curves it draws (EqEditor::getCurvesShown()).
    std::vector<Placement> getWays() const
    {
        auto middle = false, sides = false;

        for (int band = 0; band < Eq::numBands; ++band)
        {
            const BandValues shown (editor.values, band);

            if (! shown.on)
                continue;

            middle = middle || shown.placement == Placement::mid || shown.placement == Placement::side;
            sides = sides || shown.placement == Placement::left || shown.placement == Placement::right;
        }

        std::vector<Placement> ways;

        if (middle)
            ways.insert (ways.end(), { Placement::mid, Placement::side });

        if (sides)
            ways.insert (ways.end(), { Placement::left, Placement::right });

        if (ways.empty())
            ways.push_back (Placement::stereo);

        return ways;
    }

private:
    // What a press under the pointer takes: a band's dot, or a dynamic band's handle.
    struct Target
    {
        int band = -1;
        bool handle = false;

        bool operator== (const Target&) const = default;
    };

    EqEditor& editor;
    ParameterHandle dragFrequency, dragGain, dragDynamic;
    std::array<ParameterHandle, Eq::numBands> qs; // each band's, which the wheel turns
    graph::HandleDrag drag { *this };             // after the handles it moves
    graph::WheelGesture qWheel { drag };          // and turns
    bool draggingGain = false, draggingDynamic = false;
    int dragBand = -1;
    Target hovered; // what the pointer is over
    juce::Path bandCurve;
    std::vector<std::pair<Placement, juce::Path>> curves; // each way's, as the bands play now
    std::vector<float> curvesFor;                         // what they were worked out for
    int curvesWorkedOut = 0;

    // The nearer to the press of the dot and the handle within reach of it: a small Dynamic's handle sits within a dot's reach.
    Target findTarget (juce::Point<float> position) const
    {
        const auto dot = findBandAt (position), handle = findHandleAt (position);

        if (dot >= 0 && handle >= 0)
            return getDot (dot).getDistanceFrom (position) <= getHandle (handle)->getDistanceFrom (position) ? Target { dot, false }
                                                                                                             : Target { handle, true };

        if (dot >= 0)
            return { dot, false };

        if (handle >= 0)
            return { handle, true };

        return {};
    }

    void updateCurves()
    {
        const auto sampleRate = editor.getSampleRate();
        const auto width = (float) getWidth();
        const auto& shown = editor.playing;
        auto now = shown;
        now.insert (now.end(), { (float) editor.selectedBand, (float) sampleRate, width, (float) getHeight() });

        if (now == curvesFor)
            return;

        curvesFor = std::move (now);
        ++curvesWorkedOut;
        const auto ways = getWays();
        const auto zero = yForGain (0.0);
        const auto outputGain = (double) devices::dsp::decibelsToGain (shown[(size_t) Eq::outputParameter]);
        bandCurve.clear();
        bandCurve.startNewSubPath (0.0f, zero);
        curves.clear();

        for (const auto way : ways)
            curves.emplace_back (way, juce::Path());

        // Each band's sections designed once, then evaluated at each point.
        std::array<Placement, Eq::numBands> placements {};
        std::array<Eq::BandShape, Eq::numBands> shapes {};

        for (int band = 0; band < Eq::numBands; ++band)
        {
            placements[(size_t) band] = BandValues (shown, band).placement;
            shapes[(size_t) band] = Eq::getBandShape (shown, band, sampleRate);
        }

        for (auto x = 0.0f; x <= width; x += 1.5f)
        {
            const auto frequency = frequencyFor (x);
            std::array<double, 5> through {}; // each way's gain, by its placement's number
            through.fill (outputGain);

            // Each band's magnitude on its own placement, into each way it acts on.
            for (int band = 0; band < Eq::numBands; ++band)
            {
                const auto placement = placements[(size_t) band];
                const auto magnitude = shapes[(size_t) band].getMagnitude (frequency, sampleRate);

                if (band == editor.selectedBand)
                    bandCurve.lineTo (x, yForGain (juce::jlimit (-gainRange, gainRange, toDecibels (magnitude))));

                for (const auto way : ways)
                    if (Eq::actsOn (placement, way))
                        through[(size_t) way] *= magnitude;
            }

            for (auto& [way, curve] : curves)
            {
                const auto y = yForGain (juce::jlimit (-gainRange, gainRange, toDecibels (through[(size_t) way])));

                if (x <= 0.0f)
                    curve.startNewSubPath (x, y);
                else
                    curve.lineTo (x, y);
            }
        }

        bandCurve.lineTo (width, zero);
        bandCurve.closeSubPath();
    }

    // The colour of a way's curve: one curve in the text colour; the middle's and the left's in the device's colour, the side's
    // and the right's in the analyzer's second trace's, dashed.
    static bool isSecond (Placement way) { return way == Placement::side || way == Placement::right; }

    void paintCurves (juce::Graphics& g, const Palette& palette, juce::Colour tint)
    {
        const juce::PathStrokeType stroke (GraphTint::curveWidth);

        for (const auto& [way, curve] : curves)
        {
            if (way == Placement::stereo)
            {
                g.setColour (palette.text);
                g.strokePath (curve, stroke);
                continue;
            }

            g.setColour (isSecond (way) ? palette.textMuted : tint);

            if (isSecond (way))
            {
                juce::Path dashed;
                stroke.createDashedStroke (dashed, curve, dashes, (int) std::size (dashes));
                g.fillPath (dashed);
            }
            else
            {
                g.strokePath (curve, stroke);
            }
        }

        // Which is which, along the top at the right, each name in its curve's colour.
        if (curves.size() < 2)
            return;

        g.setFont (juce::FontOptions (TextSize::small));
        auto key = getLocalBounds().reduced (Space::s, Space::xs).removeFromTop (labelHeight);

        for (auto it = curves.rbegin(); it != curves.rend(); ++it)
        {
            const juce::String name (getWayName (it->first));
            const auto nameWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name)) + 1;
            g.setColour (isSecond (it->first) ? palette.textMuted : tint);
            g.drawText (name, key.removeFromRight (nameWidth), juce::Justification::centredRight);
            key.removeFromRight (Space::s);
        }
    }

    void hover (Target target)
    {
        setMouseCursor (target.band < 0 ? juce::MouseCursor::NormalCursor
                        : target.handle ? juce::MouseCursor::UpDownResizeCursor
                                        : juce::MouseCursor::DraggingHandCursor);

        if (target != hovered)
        {
            hovered = target;
            repaint();
        }
    }

    void paintGrid (juce::Graphics& g, const Palette& palette)
    {
        const auto width = (float) getWidth(), height = (float) getHeight();
        g.setFont (juce::FontOptions (TextSize::small));

        for (const auto frequency : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto x = xFor (frequency);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (x), 0.0f, height);
            g.setColour (palette.textMuted);
            g.drawText (frequency >= 1000.0 ? juce::String ((int) (frequency / 1000.0)) + "k" : juce::String ((int) frequency),
                        juce::Rectangle<float> (x + Space::xs, height - (float) labelHeight, (float) knobSize, (float) labelHeight),
                        juce::Justification::centredLeft);
        }

        for (const auto gain : { -18.0, -12.0, -6.0, 6.0, 12.0, 18.0 })
        {
            const auto y = yForGain (gain);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawHorizontalLine (juce::roundToInt (y), 0.0f, width);
            g.setColour (palette.textMuted);
            g.drawText ((gain > 0.0 ? "+" : "") + juce::String ((int) gain),
                        juce::Rectangle<float> ((float) Space::xs, y - (float) labelHeight, (float) knobSize, (float) labelHeight),
                        juce::Justification::centredLeft);
        }

        g.setColour (palette.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (yForGain (0.0)), 0.0f, width);
    }

    // Each dynamic band's line from its dot to where its gain goes at its whole Dynamic, in its colour, with a hollow handle at
    // its end (ringed in the text colour under the pointer) and a mark across it where its gain is now.
    void paintDynamics (juce::Graphics& g, const Palette& palette)
    {
        for (int band = 0; band < Eq::numBands; ++band)
        {
            const auto handle = getHandle (band);

            if (! handle.has_value())
                continue;

            const auto dot = getDot (band);
            const auto colour = getBandColour (band);
            const auto lit = hovered == Target { band, true } || (draggingDynamic && dragBand == band);
            g.setColour (colour.withAlpha (GraphTint::marker));
            g.drawLine ({ dot, *handle }, GraphTint::handleOutline);

            const auto ring = juce::Rectangle<float> (2.0f * GraphTint::handleRadius, 2.0f * GraphTint::handleRadius).withCentre (*handle);
            g.setColour (palette.background);
            g.fillEllipse (ring);
            g.setColour (lit ? palette.text : colour);
            g.drawEllipse (lit ? ring.expanded (GraphTint::litGap) : ring, lit ? GraphTint::litWidth : GraphTint::handleOutline);

            const auto markY = getMarkY (band);
            g.setColour (palette.text);
            g.drawLine (dot.x - GraphTint::handleRadius, markY, dot.x + GraphTint::handleRadius, markY, GraphTint::litWidth);
        }
    }

    // Each band's dot, filled while it is on, with its number (and its placement's letter), and the selected one outlined in
    // the accent colour.
    void paintDots (juce::Graphics& g, const Palette& palette)
    {
        g.setFont (juce::FontOptions (TextSize::small));

        for (int band = 0; band < Eq::numBands; ++band)
        {
            const auto on = editor.values[(size_t) indexOf (band, Eq::on)] > 0.5f;
            const auto colour = getBandColour (band);
            const auto dot = juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (getDot (band));

            if (on)
            {
                g.setColour (colour);
                g.fillEllipse (dot);
            }

            const auto selected = band == editor.selectedBand, lit = hovered == Target { band, false };
            g.setColour (selected ? palette.accent : (on || lit ? palette.text : palette.textMuted));
            g.drawEllipse (lit ? dot.expanded (1.0f) : dot, selected || lit ? 2.0f : 1.0f);
            g.setColour (on ? getInkFor (colour) : palette.textMuted);
            g.drawFittedText (editor.getDotText (band), dot.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        }
    }

    void showMenu (int band)
    {
        const auto type = juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::type)]);
        const auto slope = juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::slope)]);
        const auto placement = juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::placement)]);
        const auto on = editor.values[(size_t) indexOf (band, Eq::on)] > 0.5f;
        const auto& types = getInfo (indexOf (band, Eq::type)).choices;
        const auto& slopes = getInfo (indexOf (band, Eq::slope)).choices;
        const auto& placements = getInfo (indexOf (band, Eq::placement)).choices;
        constexpr int slopeItems = 100, placementItems = 300, onItem = 200, dynamicItem = 400;

        juce::PopupMenu menu;
        menu.addSectionHeader ("Band " + juce::String (band + 1));

        for (size_t i = 0; i < types.size(); ++i)
            menu.addItem ((int) i + 1, toString (types[i]), true, (int) i == type);

        if (isCut (type))
        {
            menu.addSeparator();

            for (size_t i = 0; i < slopes.size(); ++i)
                menu.addItem (slopeItems + (int) i, toString (slopes[i]), true, (int) i == slope);
        }

        menu.addSeparator();

        for (size_t i = 0; i < placements.size(); ++i)
            menu.addItem (placementItems + (int) i, toString (placements[i]), true, (int) i == placement);

        // A bell or a shelf that isn't dynamic: Make Dynamic, which gives it a handle to drag.
        if (hasGain (type) && ! BandValues (editor.values, band).isDynamic())
        {
            menu.addSeparator();
            menu.addItem (dynamicItem, "Make Dynamic");
        }

        menu.addSeparator();
        menu.addItem (onItem, on ? "Turn Off" : "Turn On");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer<Graph> (this), band, on] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;

                                auto& owner = safe->editor;
                                const auto set = [&owner, band] (Eq::Field field, float value)
                                {
                                    auto parameter = owner.device.getParameter (getInfo (indexOf (band, field)).id);
                                    parameter.setValue (value);
                                };

                                if (result == onItem)
                                    set (Eq::on, on ? 0.0f : 1.0f);
                                else if (result == dynamicItem)
                                    owner.makeDynamic (band);
                                else if (result >= placementItems)
                                    set (Eq::placement, (float) (result - placementItems));
                                else if (result >= slopeItems)
                                    set (Eq::slope, (float) (result - slopeItems));
                                else
                                    set (Eq::type, (float) (result - 1));

                                owner.refresh();
                            });
    }
};

//==============================================================================
EqEditor::EqEditor (const DeviceHandle& handle)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      device (handle)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    for (const auto& parameter : Eq::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    playing = values;
    graph = std::make_unique<Graph> (*this);
    addAndMakeVisible (*graph);

    for (int band = 0; band < Eq::numBands; ++band)
    {
        auto* button = bandButtons.add (new juce::TextButton (juce::String (band + 1)));
        button->setTitle ("Band " + juce::String (band + 1));
        button->setTooltip ("Band " + juce::String (band + 1) + ": shows its controls below");
        button->setRadioGroupId (bandsGroup);
        button->setClickingTogglesState (true);
        button->setColour (juce::TextButton::buttonOnColourId, getBandColour (band));
        button->onClick = [this, band] { selectBand (band); };
        addAndMakeVisible (button);
    }

    onButton.setTitle ("Band On");
    typeBox.setTitle ("Band Shape");
    slopeBox.setTitle ("Slope");
    placementBox.setTitle ("Placement");

    for (auto* control : std::initializer_list<juce::Component*> { &onButton, &typeBox, &slopeBox, &placementBox, &frequencyKnob, &gainKnob,
                                                                   &qKnob, &dynamicKnob, &thresholdKnob, &outputKnob })
        addAndMakeVisible (control);

    // Each knob's name, then what it is set to, where a click types a value.
    for (const auto* name : { "Frequency", "Gain", "Q", "Dynamic", "Threshold", "Output" })
        addLabel (name);

    Knob* const shown[] = { &frequencyKnob, &gainKnob, &qKnob, &dynamicKnob, &thresholdKnob, &outputKnob };

    for (auto* knob : shown)
    {
        auto& value = addLabel ({});
        setThemeColour (value, juce::Label::textColourId, &Palette::textMuted);
        knob->setValueReadout (&value);
    }

    showAsParameter (outputKnob, getInfo (Eq::outputParameter));
    outputAttachment.setParameter (device.getParameter (getInfo (Eq::outputParameter).id));
    outputKnob.setTooltip (describeParameter (getInfo (Eq::outputParameter), device.getParameter (getInfo (Eq::outputParameter).id)));
    applyDeviceTint (*this, device);

    selectBand (selectedBand);
    setSize (getWidthForHeight (0), 2 * Space::xs + std::max (GraphTint::height, getControlsHeight()));
    refresh();
}

int EqEditor::getMinimumHeight() const
{
    return 2 * Space::xs + std::max (GraphTint::minimumHeight, getControlsHeight());
}

int EqEditor::getWidthForHeight (int) const
{
    return 2 * Space::s + graphWidth + Space::m + getControlsWidth();
}

int EqEditor::getMinimumWidth() const
{
    return 2 * Space::s + GraphTint::minimumWidth + Space::m + getControlsWidth();
}

EqEditor::~EqEditor()
{
    if (auto* eq = device.getProcessor<Eq>())
        eq->getOutputTap().setListening (false);

    if (auto* scope = device.getScope())
        scope->setListening (false);
}

juce::Component& EqEditor::getGraph()
{
    return *graph;
}

juce::Point<float> EqEditor::getDotPosition (int band) const
{
    return graph->getDot (band);
}

std::optional<juce::Point<float>> EqEditor::getDynamicHandlePosition (int band) const
{
    return graph->getHandle (band);
}

void EqEditor::makeDynamic (int band)
{
    constexpr float madeDynamic = -6.0f;
    auto dynamic = device.getParameter (getInfo (indexOf (band, Eq::dynamic)).id, "Make Band " + juce::String (band + 1) + " Dynamic");
    dynamic.setValue (madeDynamic);
    refresh();
}

juce::String EqEditor::getDotText (int band) const
{
    return juce::String (band + 1) + getLetter (BandValues (values, band).placement);
}

std::vector<devices::Equalizer::Placement> EqEditor::getCurvesShown() const
{
    return graph->getWays();
}

float EqEditor::getShownGain (int band) const
{
    return playing[(size_t) indexOf (band, Eq::gain)];
}

int EqEditor::getCurvesWorkedOut() const noexcept
{
    return graph->getCurvesWorkedOut();
}

double EqEditor::getSampleRate() const
{
    if (auto* eq = device.getProcessor<Eq>(); eq != nullptr && eq->getSampleRate() > 0.0)
        return eq->getSampleRate();

    return defaultSampleRate;
}

juce::Label& EqEditor::addLabel (const juce::String& text)
{
    auto* label = labels.add (new juce::Label ({}, text));
    label->setJustificationType (juce::Justification::centred);
    label->setFont (juce::FontOptions (TextSize::small));
    addAndMakeVisible (label);
    return *label;
}

void EqEditor::selectBand (int band)
{
    selectedBand = juce::jlimit (0, Eq::numBands - 1, band);
    bandButtons[selectedBand]->setToggleState (true, juce::dontSendNotification);

    const auto handleOf = [this] (Eq::Field field) { return device.getParameter (getInfo (indexOf (selectedBand, field)).id); };

    // Each control says in the hint bar what the band's parameter is, as the grids' controls do.
    for (auto [control, field] : { std::pair<juce::SettableTooltipClient*, Eq::Field> { &onButton, Eq::on },
                                   { &typeBox, Eq::type },
                                   { &slopeBox, Eq::slope },
                                   { &placementBox, Eq::placement },
                                   { &frequencyKnob, Eq::frequency },
                                   { &gainKnob, Eq::gain },
                                   { &qKnob, Eq::q },
                                   { &dynamicKnob, Eq::dynamic },
                                   { &thresholdKnob, Eq::threshold } })
        control->setTooltip (describeParameter (getInfo (indexOf (selectedBand, field)), handleOf (field)));

    onAttachment.setParameter (handleOf (Eq::on));
    typeAttachment.setParameter (handleOf (Eq::type));
    slopeAttachment.setParameter (handleOf (Eq::slope));
    placementAttachment.setParameter (handleOf (Eq::placement));

    for (auto [knob, attachment, field] :
         { std::tuple<Knob*, ParameterAttachment*, Eq::Field> { &frequencyKnob, &frequencyAttachment, Eq::frequency },
           { &gainKnob, &gainAttachment, Eq::gain },
           { &qKnob, &qAttachment, Eq::q },
           { &dynamicKnob, &dynamicAttachment, Eq::dynamic },
           { &thresholdKnob, &thresholdAttachment, Eq::threshold } })
    {
        showAsParameter (*knob, getInfo (indexOf (selectedBand, field)));
        attachment->setParameter (handleOf (field));
    }

    refresh();
}

void EqEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    // The curves show each dynamic band's gain as it plays now (in steps too small to see, so that a gain at rest draws
    // nothing new).
    playing = values;
    auto* eq = device.getProcessor<Eq>();

    for (int band = 0; band < Eq::numBands; ++band)
        if (eq != nullptr && BandValues (values, band).isDynamic())
            playing[(size_t) indexOf (band, Eq::gain)] += std::round (eq->getGainMoved (band) / shownGainStep) * shownGainStep;

    onAttachment.update();
    typeAttachment.update();
    placementAttachment.update();
    frequencyAttachment.update();
    qAttachment.update();
    outputAttachment.update();

    // A cut's slope matters, and a shelf's or bell's gain, Dynamic and Threshold (the Threshold faint while the Dynamic is 0); the
    // other's control is off, switched off once rather than on by its attachment and off again at each tick, which repainted it
    // each time.
    const BandValues selected (values, selectedBand);

    if (isCut (selected.type))
        slopeAttachment.update();
    else
        slopeBox.setEnabled (false);

    if (hasGain (selected.type))
    {
        gainAttachment.update();
        dynamicAttachment.update();
        thresholdAttachment.update();
        thresholdKnob.setAlpha (std::abs (selected.dynamic) > 0.0f ? 1.0f : GraphTint::partOff);
    }
    else
    {
        gainKnob.setEnabled (false);
        dynamicKnob.setEnabled (false);
        thresholdKnob.setEnabled (false);
        thresholdKnob.setAlpha (1.0f);
    }

    for (int band = 0; band < Eq::numBands; ++band)
        bandButtons[band]->setAlpha (values[(size_t) indexOf (band, Eq::on)] > 0.5f ? 1.0f : GraphTint::partOff);

    // What each knob is set to, under its name (none for a knob that doesn't apply).
    Knob* const knobs[] = { &frequencyKnob, &gainKnob, &qKnob, &dynamicKnob, &thresholdKnob, &outputKnob };

    for (int i = 0; i < numKnobs; ++i)
        labels[i + numKnobs]->setText (knobs[i]->isEnabled() ? knobs[i]->getTextFromValue (knobs[i]->getValue()) : juce::String(),
                                       juce::dontSendNotification);

    auto spectraMoved = false;

    if (eq != nullptr)
    {
        output.setSampleRate (getSampleRate());
        output.pull (eq->getOutputTap());
        spectraMoved = output.update();
    }

    // And, fainter behind it, what comes in.
    if (auto* scope = device.getScope())
    {
        scope->setListening (true);
        input.setSampleRate (getSampleRate());

        if (auto* tap = scope->getInput())
            input.pull (*tap);

        spectraMoved = input.update() || spectraMoved;
    }

    // The curves, their dots and the dynamic bands' marks over the spectra, repainted when any of them moves.
    auto numbers = playing;
    numbers.insert (numbers.end(), values.begin(), values.end());
    numbers.insert (numbers.end(), { (float) selectedBand, (float) getSampleRate() });

    if (drawnGraph.changed (std::move (numbers)) || spectraMoved)
        graph->repaint();
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void EqEditor::resized()
{
    // The graph on the left, as tall as the editor; the band's controls beside it. Its edge is less at its top and bottom, as
    // the dock is short (DeviceEditorBase::marginY).
    auto area = getLocalBounds().reduced (Space::s, Space::xs);
    graph->setBounds (area.removeFromLeft (area.getWidth() - getControlsWidth() - Space::m));
    area.removeFromLeft (Space::m);

    // The band buttons, in two rows.
    const auto perRow = Eq::numBands / bandRows;

    for (int row = 0; row < bandRows; ++row)
    {
        auto buttons = area.removeFromTop (controlHeight);
        const auto buttonWidth = (buttons.getWidth() - (perRow - 1) * Space::xs) / perRow;

        for (int i = 0; i < perRow; ++i)
        {
            bandButtons[row * perRow + i]->setBounds (buttons.removeFromLeft (buttonWidth));
            buttons.removeFromLeft (Space::xs);
        }

        if (row + 1 < bandRows)
            area.removeFromTop (Space::xs);
    }

    area.removeFromTop (Space::s);

    // The band's switch and shape, then its slope and its placement side by side.
    auto shapes = area.removeFromTop (controlHeight);
    onButton.setBounds (shapes.removeFromLeft (knobCell));
    typeBox.setBounds (shapes);
    area.removeFromTop (Space::xs);
    auto choices = area.removeFromTop (controlHeight).withTrimmedLeft (knobCell);
    const auto choiceWidth = (choices.getWidth() - Space::xs) / 2;
    slopeBox.setBounds (choices.removeFromLeft (choiceWidth));
    choices.removeFromLeft (Space::xs);
    placementBox.setBounds (choices);
    area.removeFromTop (Space::s);

    // Its knobs, then the output's, each with its name and value.
    Knob* const knobs[] = { &frequencyKnob, &gainKnob, &qKnob, &dynamicKnob, &thresholdKnob, &outputKnob };

    for (int i = 0; i < numKnobs; ++i)
    {
        auto cell = area.removeFromLeft (knobCell).withHeight (compactKnobSize + 2 * labelHeight);
        knobs[i]->setBounds (cell.removeFromTop (compactKnobSize).withSizeKeepingCentre (compactKnobSize, compactKnobSize));
        labels[i]->setBounds (cell.removeFromTop (labelHeight));
        labels[i + numKnobs]->setBounds (cell.removeFromTop (labelHeight));
    }
}
} // namespace phazite::ui
