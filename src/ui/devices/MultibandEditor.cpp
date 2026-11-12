// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MultibandEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "SidechainMenus.h"
#include "devices/dynamics/Multiband.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Multiband;

    constexpr const char* bandNames[] = { "Low", "Mid", "High", "Master" };
    constexpr float gridDecibels[] = { -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 3.0f, 6.0f, 9.0f };
    constexpr int historySize = 1024; // the monitor's columns, at most

    // Its layout puts all of it on one page in the default dock's 552 by 268 pixels (the owner, 2026-11-10): the bands' part
    // and the band's side by side, their knobs in cells a little narrower than other editors', their rows of buttons a little
    // lower.
    constexpr int rowHeight = controlHeight - 6;                      // 22: the band's tabs, its tools, mode and switches
    constexpr int cellLeast = compactKnobSize + 10;                   // 50: a compact knob, its name and value squeezed
    constexpr int gridLeast = 4 * cellLeast;                          // 200: four cells across, the bands' and the band's
    constexpr int gridMost = 4 * (compactKnobSize + Space::l);        // 224: as wide as other editors' cells
    constexpr int curveLeast = 116, curveMost = 240, curveWide = 160; // the band's curve, square
    constexpr int monitorLeast = 220, monitorWidth = 300;             // Monitor, where it shows beside the band's knobs
    constexpr int sidechainWidth = 3 * (compactKnobSize + Space::l);  // 168: the Sidechain part, where it shows beside them
    constexpr int bandsWidth = 300;                                   // the bands' part, where there is room
    constexpr int bandsGraphLeast = 94, bandsGraphHeight = 152;       // its graph
    constexpr int slopeWidth = 44, slopeHeight = 20;                  // a crossover's slope, in the graph's top corner
    constexpr int columnGap = Space::m;                               // between the band's knobs, Monitor and the Sidechain part
    constexpr int leftRowsHeight = 4 * rowHeight + 3 * Space::xs;     // the band's tools, mode (two rows) and switches
    constexpr int listenWidth = 64;

    juce::Colour getBandColour (int band)
    {
        return juce::Colour (partColours[(size_t) band]);
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Multiband::info.parameters[(size_t) index];
    }

    std::string_view idOf (int index)
    {
        return getInfo (index).id;
    }

    std::string_view idOf (int band, Multiband::BandField field)
    {
        return idOf (Multiband::getBandParameter (band, field));
    }

    float axisOf (float decibels)
    {
        return devices::curves::axisForDecibels (decibels);
    }

    int getLabelHeight()
    {
        return juce::roundToInt (TextSize::small) + Space::xs; // a control's name, as ParameterControls shows it
    }

    // A control with its name under it, as a knob has: Peak or RMS.
    class NamedControl : public juce::Component
    {
    public:
        NamedControl (juce::Component& c, const juce::String& name)
            : content (c)
        {
            label.setText (name, juce::dontSendNotification);
            label.setFont (juce::FontOptions (TextSize::small));
            label.setJustificationType (juce::Justification::centred);
            label.setMinimumHorizontalScale (0.8f);
            label.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (content);
            addAndMakeVisible (label);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            label.setBounds (area.removeFromBottom (getLabelHeight()));
            area.removeFromBottom (Space::xs);
            content.setBounds (area.reduced (Space::xs, 0));
        }

    private:
        juce::Component& content;
        juce::Label label;
    };
} // namespace

//==============================================================================
// Monitor's picture, as Maximus's: the last seconds of the selected band, scrolling, paused by a click.
class MultibandEditor::Analysis : public juce::Component
{
public:
    explicit Analysis (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Monitor");
        setDescription ("The last seconds of the selected band: what comes in, what goes out, what its compressor hears (the "
                        "sidechain, in its track's colour and named, while the band's Sidechain is on) and its gain, scrolling; a "
                        "click pauses it.");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);

        // Silence at first, so that a history that stays silent is the same all along and needn't repaint.
        Column silence {};
        silence.fill (-120.0f);

        for (int band = 0; band < Multiband::numBands; ++band)
            silence[(size_t) band * 4 + 3] = 0.0f; // no reduction

        columns.assign (historySize, silence);
    }

    // The traces Monitor shows, each by its switch.
    enum Trace
    {
        mainInput,
        bandInput,
        bandOutput,
        heard,
        gain,
        allGains,
        levels,
        numTraces
    };

    std::array<bool, numTraces> traces { true, false, true, true, true, false, true };
    bool paused = false;
    float speed = 0.5f; // from 0 (slowest) to 1

    // A column: each band's level in, out, heard and gain, then what came into the device, in decibels.
    using Column = std::array<float, 4 * Multiband::numBands + 1>;

    void push (const Column& column)
    {
        if (paused)
            return;

        // Repainted while what shows changes: a history all alike, silence, say, looks the same scrolled.
        sameInARow = column == columns[(size_t) newest] ? std::min (sameInARow + 1, historySize) : 0;
        newest = (newest + 1) % historySize;
        columns[(size_t) newest] = column;

        if (sameInARow < historySize)
            repaint();
    }

    // The track the selected band hears through its Key, which its heard trace shows in its colour and names; none while it
    // hears itself (or what comes in, with no track picked).
    void setKey (const juce::String& name, juce::Colour colour)
    {
        if (name == keyName && colour == keyColour)
            return;

        keyName = name;
        keyColour = colour;
        repaint();
    }

    juce::String getKeyName() const { return keyName; }

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (1.0f); }
    float yForDecibels (float decibels) const
    {
        return getArea().getBottom() - juce::jlimit (0.0f, 1.0f, axisOf (decibels)) * getArea().getHeight();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        graph::paintBackground (g, p, getLocalBounds().toFloat());
        paintTraces (g, p);

        // The levels' scale down the right, as on the curve.
        const auto area = getArea();
        g.setFont (juce::FontOptions (TextSize::small));

        for (const auto decibels : gridDecibels)
        {
            const auto y = yForDecibels (decibels);
            g.setColour (p.text.withAlpha (GraphTint::grid));
            g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
            g.setColour (p.textMuted);
            g.drawText (juce::String ((int) decibels), juce::Rectangle<float> (area.getRight() - 28.0f, y - 7.0f, 24.0f, 14.0f),
                        juce::Justification::centredRight);
        }

        const auto zero = yForDecibels (0.0f);
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (zero), area.getX(), area.getRight());
        g.setColour (p.textMuted);
        g.drawText ("0", juce::Rectangle<float> (area.getRight() - 28.0f, zero - 7.0f, 24.0f, 14.0f), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        paused = ! paused; // a click pauses it, another goes on
        repaint();
    }

private:
    MultibandEditor& editor;
    std::vector<Column> columns;
    int newest = 0, sameInARow = historySize; // how many columns in a row came in alike: at first, all of them
    juce::String keyName;
    juce::Colour keyColour;

    void paintTraces (juce::Graphics& g, const Palette& p)
    {
        const auto area = getArea();
        const auto band = editor.selectedBand;
        const auto colour = getBandColour (band);
        const auto step = 1.0f + 3.0f * speed; // pixels a column takes: faster, wider
        const auto count = std::min (historySize, (int) (area.getWidth() / step) + 1);

        // A trace's path, newest at the right, from a column's value.
        const auto trace = [&] (auto valueOf, bool filled)
        {
            juce::Path path;

            for (int i = 0; i < count; ++i)
            {
                const auto& column = columns[(size_t) ((newest - i + historySize) % historySize)];
                const auto x = area.getRight() - (float) i * step;
                const auto y = yForDecibels (valueOf (column));

                if (i == 0)
                    path.startNewSubPath (x, filled ? area.getBottom() : y);

                path.lineTo (x, y);
            }

            if (filled)
            {
                path.lineTo (area.getRight() - (float) (count - 1) * step, area.getBottom());
                path.closeSubPath();
            }

            return path;
        };

        const auto b = (size_t) band * 4;

        if (traces[mainInput])
        {
            g.setColour (p.text.withAlpha (GraphTint::spectrumFill));
            g.fillPath (trace ([] (const Column& c) { return c.back(); }, true));
        }

        if (traces[bandInput])
        {
            g.setColour (colour.withAlpha (GraphTint::spectrumFill));
            g.fillPath (trace ([b] (const Column& c) { return c[b]; }, true));
        }

        if (traces[bandOutput])
        {
            g.setColour (colour.withAlpha (GraphTint::partCurve * 2.0f));
            g.fillPath (trace ([b] (const Column& c) { return c[b + 1]; }, true));
        }

        // What its compressor hears: the key, in its track's colour and named, while it hears one.
        if (traces[heard])
        {
            const auto heardOf = [b] (const Column& c) { return c[b + 2]; };

            if (keyName.isNotEmpty())
            {
                g.setColour (keyColour.withAlpha (GraphTint::partCurve));
                g.fillPath (trace (heardOf, true));
                g.setColour (keyColour);
                g.strokePath (trace (heardOf, false), juce::PathStrokeType (1.5f));
                g.setFont (juce::FontOptions (TextSize::small));
                g.drawText (keyName, area.reduced ((float) Space::xs).toNearestInt(), juce::Justification::bottomLeft, true);
            }
            else
            {
                g.setColour (p.modulation);
                g.strokePath (trace (heardOf, false), juce::PathStrokeType (1.5f));
            }
        }

        if (traces[allGains])
        {
            for (int other = 0; other < Multiband::numBands; ++other)
            {
                const auto o = (size_t) other * 4;
                g.setColour (getBandColour (other));
                g.strokePath (trace ([o] (const Column& c) { return -c[o + 3]; }, false), juce::PathStrokeType (1.0f));
            }
        }

        if (traces[gain])
        {
            g.setColour (p.text);
            g.strokePath (trace ([b] (const Column& c) { return -c[b + 3]; }, false), juce::PathStrokeType (1.5f));
        }

        // The band's pre and post gains, as level marks.
        if (traces[levels])
        {
            const auto pre = editor.values[(size_t) Multiband::getBandParameter (band, Multiband::pre)];
            const auto post = editor.values[(size_t) Multiband::getBandParameter (band, Multiband::post)];
            g.setColour (getBandColour (6)); // purple
            g.drawHorizontalLine (juce::roundToInt (yForDecibels (pre)), area.getX(), area.getRight());
            g.setColour (getBandColour (5)); // blue
            g.drawHorizontalLine (juce::roundToInt (yForDecibels (post)), area.getX(), area.getRight());
        }

        if (paused)
        {
            g.setColour (p.textMuted);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText ("Paused", area.reduced (Space::s).toNearestInt(), juce::Justification::topLeft);
        }
    }
};

//==============================================================================
namespace
{
    // The traces Monitor shows, each switched in its menu, in Analysis::Trace's order.
    constexpr const char* traceNames[] = { "In", "Band In", "Out", "Heard", "Gain", "All Gains", "Pre/Post" };
} // namespace

// Monitor, over its tools: the menu of its traces, and its speed.
class MultibandEditor::MonitorPart : public juce::Component
{
public:
    explicit MonitorPart (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Monitor");
        addAndMakeVisible (*editor.analysis);

        tracesButton.setTooltip ("Which traces Monitor shows: what comes into the multiband (In), what comes into the selected "
                                 "band after its pre gain (Band In), what it puts out (Out), what its compressor hears (Heard), "
                                 "its gain, every band's gain, and its pre (purple) and post (blue) gains");
        tracesButton.onClick = [this]
        { editor.getTracesMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&tracesButton)); };
        addAndMakeVisible (tracesButton);

        speed.setSliderStyle (juce::Slider::LinearHorizontal);
        speed.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        speed.setRange (0.0, 1.0);
        speed.setValue (editor.analysis->speed, juce::dontSendNotification);
        speed.setTitle ("Speed");
        speed.setTooltip ("How fast Monitor scrolls: slower to the left, faster to the right");
        speed.onValueChange = [this] { editor.analysis->speed = (float) speed.getValue(); };
        addAndMakeVisible (speed);
        speedName.setFont (juce::FontOptions (TextSize::small));
        speedName.setJustificationType (juce::Justification::centredRight);
        speedName.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (speedName);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto tools = area.removeFromBottom (rowHeight);
        area.removeFromBottom (Space::xs);
        editor.analysis->setBounds (area);

        tracesButton.setBounds (tools.removeFromLeft (std::min (tools.getWidth() / 2, tracesWidth)));
        tools.removeFromLeft (Space::s);

        // The speed in what is left, named where there is room.
        const auto nameWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (speedName.getFont(), speedName.getText()));
        speedName.setVisible (tools.getWidth() >= nameWidth + Space::s + minimumSpeedWidth);

        if (speedName.isVisible())
            speedName.setBounds (tools.removeFromLeft (nameWidth + Space::s));

        speed.setBounds (tools.reduced (Space::xs, 0));
    }

private:
    static constexpr int tracesWidth = 72, minimumSpeedWidth = 48;

    MultibandEditor& editor;
    juce::TextButton tracesButton { "Traces" };
    juce::Slider speed;
    juce::Label speedName { {}, "Speed" };
};

//==============================================================================
// The bands across the spectrum, as Maximus's Bands view and FabFilter Pro-MB's display show them: each as high as its level
// (its Post), between the crossovers, with the spectrum of what the multiband plays over them and the low cut's edge at the
// left; the crossovers' slopes in the top corners. A crossover or the low cut's edge drags left or right, a band up or down
// for its level, each one undo step; a click on a band shows it, a double-click puts what is under the pointer back where it
// started. Under the pointer, what a drag moves lights up, the pointer shows which way it goes, and the tooltip, which the hint
// bar shows, says what it does (the owner, 2026-11-10: "it should be obvious that you are able to do this gesture").
class MultibandEditor::BandsGraph : public juce::Component,
                                    public juce::TooltipClient
{
public:
    BandsGraph (MultibandEditor& e, ChoiceButtons& low, ChoiceButtons& high)
        : editor (e),
          lowSlope (low),
          highSlope (high)
    {
        setTitle ("Bands");
        setDescription ("The low, mid and high bands across the spectrum, each as high as its level, with the spectrum of what the "
                        "multiband plays over them: drag a crossover, or the low cut's edge at the left, left or right; drag a band up "
                        "or down for its level; click a band to show its curve and controls; double-click to put one back where it "
                        "started. The crossovers' slopes are in its top corners.");
        addAndMakeVisible (lowSlope);
        addAndMakeVisible (highSlope);

        const auto& post = getInfo (Multiband::getBandParameter (Multiband::low, Multiband::post));
        lowestLevel = post.minimum;
        highestLevel = post.maximum;
    }

    graph::SpectrumTrace spectrum;

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (1.0f); }
    graph::FrequencyAxis getFrequencies() const { return { getArea() }; }

    // A band's level, from the lowest at the bottom to the highest at the top, 0 dB in the middle.
    float yForLevel (float decibels) const
    {
        const auto area = getArea();
        return area.getBottom() - juce::jlimit (0.0f, 1.0f, (decibels - lowestLevel) / (highestLevel - lowestLevel)) * area.getHeight();
    }

    juce::String getTooltip() override
    {
        const auto target = dragged.has_value() ? dragged : hovered;

        if (! target.has_value())
            return {};

        const auto value = valueOf (getParameter (*target));

        switch (target->kind)
        {
            case Target::Kind::lowCut:
                return "The low cut, " + graph::formatFrequency (value)
                       + ", under which the multiband takes the sound away: drag its edge left or right; double-click it to put it back";

            case Target::Kind::crossover:
                return juce::String ("The crossover between the ") + (target->index == 0 ? "low and mid" : "mid and high") + " bands, "
                       + graph::formatFrequency (value) + ": drag it left or right; its slope is in the "
                       + (target->index == 0 ? "left" : "right") + " corner above; double-click it to put it back";

            case Target::Kind::level:
                break;
        }

        return "The " + juce::String (bandNames[target->index]).toLowerCase() + " band's level, " + graph::formatDecibels (value)
               + " (its Post, after its compressor): drag it up or down; click it to show its curve and controls; double-click it for "
                 "0 dB";
    }

    void resized() override
    {
        // The low crossover's slope at the left, the high one's at the right.
        const auto area = getArea().toNearestInt().reduced (Space::xs);
        lowSlope.setBounds (area.getX(), area.getY(), slopeWidth, slopeHeight);
        highSlope.setBounds (area.getRight() - slopeWidth, area.getY(), slopeWidth, slopeHeight);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto area = getArea();
        const auto axis = getFrequencies();
        graph::paintBackground (g, p, getLocalBounds().toFloat());

        // The levels' lines: 0 dB, and every 12 dB.
        for (auto level = lowestLevel + 12.0f; level < highestLevel; level += 12.0f)
        {
            g.setColour (p.text.withAlpha (juce::approximatelyEqual (level, 0.0f) ? GraphTint::zeroLine : GraphTint::grid));
            g.drawHorizontalLine (juce::roundToInt (yForLevel (level)), area.getX(), area.getRight());
        }

        graph::paintFrequencyGrid (g, p, axis, true);

        // What the low cut takes away, faint, from the left.
        const auto cutX = getLowCutX();

        if (cutX > area.getX())
        {
            g.setColour (p.text.withAlpha (GraphTint::grid));
            g.fillRect (area.withRight (cutX));
        }

        // Each band as high as its level, in its colour, the band shown stronger and one that doesn't sound (muted, off, or
        // soloed out) faint; its top, which a drag moves, lit under the pointer, as wide as a lit line in the text colour.
        auto soloed = false;

        for (int band = Multiband::low; band <= Multiband::high; ++band)
            soloed = soloed || (valueOf (Multiband::getSoloParameter (band)) > 0.5f && ! isOff (band));

        for (int band = Multiband::low; band <= Multiband::high; ++band)
        {
            const auto [left, right] = getSpan (band);
            const auto mode = juce::roundToInt (valueOf (Multiband::getBandParameter (band, Multiband::mode)));
            const auto silent =
                mode == (int) Multiband::Mode::muted || isOff (band) || (soloed && valueOf (Multiband::getSoloParameter (band)) <= 0.5f);
            const auto top = yForLevel (valueOf (Multiband::getBandParameter (band, Multiband::post)));
            const auto colour = getBandColour (band);
            g.setColour (colour.withAlpha (silent                        ? GraphTint::spectrumFill
                                           : band == editor.selectedBand ? GraphTint::partCurve * 1.6f
                                                                         : GraphTint::partCurve));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (left, top, right, area.getBottom()));

            const auto lit = isHot ({ Target::Kind::level, band });
            const auto thickness = lit ? GraphTint::litWidth : GraphTint::handleOutline;
            g.setColour (lit ? p.text : silent ? colour.withAlpha (GraphTint::partOff) : colour);
            g.fillRect (juce::Rectangle<float> (left, top - thickness / 2.0f, right - left, thickness));
        }

        // What it plays, over them.
        spectrum.paint (g, axis, { area, -90.0f, 6.0f }, p.text.withAlpha (GraphTint::spectrumFill),
                        p.text.withAlpha (GraphTint::spectrumLine));

        // The low cut's edge and the crossovers, which drags move: lit under the pointer, as wide as a lit line in the text
        // colour.
        const auto paintEdge = [&] (float x, bool lit)
        {
            const auto width = lit ? GraphTint::litWidth : 1.0f;
            g.setColour (lit ? p.text : p.text.withAlpha (GraphTint::marker));
            g.fillRect (juce::Rectangle<float> (x - width / 2.0f, area.getY(), width, area.getHeight()));
        };

        const auto lowCutHot = isHot ({ Target::Kind::lowCut, 0 });

        if (cutX > area.getX() + 0.5f || lowCutHot)
            paintEdge (cutX, lowCutHot);

        paintEdge (getSpan (Multiband::low).second, isHot ({ Target::Kind::crossover, 0 }));
        paintEdge (getSpan (Multiband::mid).second, isHot ({ Target::Kind::crossover, 1 }));

        // What the pointer is over, between the slopes.
        if (const auto target = dragged.has_value() ? dragged : hovered)
            graph::paintReadout (
                g, p, area.withTrimmedLeft ((float) (slopeWidth + Space::xs)).withTrimmedRight ((float) (slopeWidth + Space::xs)),
                describe (*target), juce::Justification::centredTop);
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (findTarget (e.position)); }

    void mouseExit (const juce::MouseEvent&) override { setHovered ({}); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        moved = false;
        dragged = e.mods.isPopupMenu() ? std::optional<Target>() : findTarget (e.position);

        if (dragged.has_value())
        {
            handle = editor.device.getParameter (idOf (getParameter (*dragged)));
            startValue = handle.getValue();
            repaint();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // A press becomes a drag once it moves a little, so that a click shows its band and changes nothing.
        if (! dragged.has_value() || ! e.mouseWasDraggedSinceMouseDown())
            return;

        if (! moved)
        {
            moved = true;
            handle.beginGesture();
        }

        const auto& info = getInfo (getParameter (*dragged));

        if (dragged->kind == Target::Kind::level)
        {
            // Up or down from where it was, as far as the pointer moved.
            const auto perPixel = (highestLevel - lowestLevel) / std::max (1.0f, getArea().getHeight());
            handle.setValue (devices::constrain (info, startValue - (float) e.getDistanceFromDragStartY() * perPixel));
        }
        else
        {
            handle.setValue (devices::constrain (info, (float) getFrequencies().frequencyAt (e.position.x)));
        }

        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const auto target = dragged;

        if (handle.isInGesture())
            handle.endGesture();

        dragged.reset();

        // A click on a band shows its curve and controls.
        if (target.has_value() && target->kind == Target::Kind::level && ! moved)
            editor.selectBand (target->index);

        moved = false;
        setHovered (findTarget (e.position));
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Back where it started, as a knob's double-click puts it.
        if (const auto target = findTarget (e.position))
        {
            const auto& info = getInfo (getParameter (*target));
            editor.device.getParameter (info.id).setValue (info.defaultValue);
            editor.refresh();
        }
    }

private:
    // What a press takes: the low cut's edge, a crossover (0, the low one) or a band's level.
    struct Target
    {
        enum class Kind
        {
            lowCut,
            crossover,
            level
        };

        Kind kind = Kind::level;
        int index = 0;

        bool operator== (const Target&) const = default;
    };

    MultibandEditor& editor;
    ChoiceButtons &lowSlope, &highSlope;
    float lowestLevel = -24.0f, highestLevel = 24.0f;
    std::optional<Target> hovered, dragged;
    ParameterHandle handle; // what a drag moves
    float startValue = 0.0f;
    bool moved = false; // the press has become a drag

    float valueOf (int index) const { return editor.values[(size_t) index]; }

    bool isOff (int band) const
    {
        return juce::roundToInt (valueOf (Multiband::getBandParameter (band, Multiband::mode))) == (int) Multiband::Mode::off;
    }

    int getParameter (Target target) const
    {
        switch (target.kind)
        {
            case Target::Kind::lowCut:
                return Multiband::lowCut;

            case Target::Kind::crossover:
                return target.index == 0 ? Multiband::lowFrequency : Multiband::highFrequency;

            case Target::Kind::level:
                break;
        }

        return Multiband::getBandParameter (target.index, Multiband::post);
    }

    bool isHot (Target target) const { return dragged.has_value() ? *dragged == target : hovered == target; }

    // A band's edges, at the crossovers, the high one never left of the low one, as the multiband splits them.
    std::pair<float, float> getSpan (int band) const
    {
        const auto area = getArea();
        const auto axis = getFrequencies();
        const auto within = [&area] (float x) { return juce::jlimit (area.getX(), area.getRight(), x); };
        const auto low = within (axis.xFor (valueOf (Multiband::lowFrequency)));
        const auto high = std::max (low, within (axis.xFor (valueOf (Multiband::highFrequency))));

        if (band == Multiband::low)
            return { area.getX(), low };

        return band == Multiband::mid ? std::pair { low, high } : std::pair { high, area.getRight() };
    }

    float getLowCutX() const
    {
        return juce::jlimit (getArea().getX(), getArea().getRight(), getFrequencies().xFor (valueOf (Multiband::lowCut)));
    }

    // Under the pointer: a crossover's line near it, the nearest; then the low cut's edge; else the band it is over.
    std::optional<Target> findTarget (juce::Point<float> position) const
    {
        if (! getArea().contains (position))
            return {};

        std::optional<Target> nearest;
        auto nearestDistance = GraphTint::grabDistance;

        for (int crossover = 0; crossover < 2; ++crossover)
        {
            if (const auto distance = std::abs (getSpan (crossover).second - position.x); distance <= nearestDistance)
            {
                nearest = Target { Target::Kind::crossover, crossover };
                nearestDistance = distance;
            }
        }

        if (nearest.has_value())
            return nearest;

        if (std::abs (getLowCutX() - position.x) <= GraphTint::grabDistance)
            return Target { Target::Kind::lowCut, 0 };

        for (int band = Multiband::low; band <= Multiband::high; ++band)
            if (const auto [left, right] = getSpan (band); position.x >= left && position.x <= right)
                return Target { Target::Kind::level, band };

        return {};
    }

    // The pointer at each move (so that it is never one left from before), showing which way a drag there goes; what is under
    // it lit.
    void setHovered (std::optional<Target> target)
    {
        setMouseCursor (! target.has_value()                  ? juce::MouseCursor::NormalCursor
                        : target->kind == Target::Kind::level ? juce::MouseCursor::UpDownResizeCursor
                                                              : juce::MouseCursor::LeftRightResizeCursor);

        if (target != hovered)
        {
            hovered = target;
            repaint();
        }
    }

    // What the pointer is over, with its value: "Low/Mid 120 Hz", "Mid +2.0 dB".
    juce::String describe (Target target) const
    {
        const auto value = valueOf (getParameter (target));

        switch (target.kind)
        {
            case Target::Kind::lowCut:
                return "Low Cut " + graph::formatFrequency (value);

            case Target::Kind::crossover:
                return juce::String (target.index == 0 ? "Low/Mid " : "Mid/High ") + graph::formatFrequency (value);

            case Target::Kind::level:
                break;
        }

        return juce::String (bandNames[target.index]) + " " + graph::formatDecibels (value);
    }
};

//==============================================================================
// The bands' part: the graph, over its knobs, always together.
class MultibandEditor::BandsPart : public juce::Component
{
public:
    BandsPart (BandsGraph& g, ParameterControls& k)
        : picture (g),
          knobs (k)
    {
        addAndMakeVisible (picture);
        addAndMakeVisible (knobs);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        knobs.setBounds (area.removeFromBottom (knobs.getPreferredHeight()));
        area.removeFromBottom (Space::xs);
        picture.setBounds (area);
    }

private:
    BandsGraph& picture;
    ParameterControls& knobs;
};

//==============================================================================
// The Sidechain part: the key's picker at its top (with a project's context), its cuts and Listen, and which bands hear it.
// Listen plays the key, as the bands hear it through its cuts, instead of the sound, while it is on and shows
// (Multiband::listenToKey(), which the refresh asks for again and again), as the compressor's does.
class MultibandEditor::SidechainPart : public juce::Component
{
public:
    SidechainPart (MultibandEditor& editor, ParameterControls& c, ProjectContext* context, DeviceID id)
        : cuts (c)
    {
        setTitle ("Sidechain");

        if (context != nullptr && id.isValid())
        {
            picker = std::make_unique<SidechainButton> (*context, id);
            picker->onChange = [&editor] { editor.refresh(); };
            addAndMakeVisible (*picker);
        }

        listen.setTitle ("Listen");
        listen.setTooltip ("Plays the sidechain, as the bands whose Sidechain is on hear it through its cuts, instead of the sound, "
                           "to hear what makes them duck");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (cuts);
        addAndMakeVisible (listen);

        listeners.setFont (juce::FontOptions (TextSize::small));
        listeners.setJustificationType (juce::Justification::topLeft);
        listeners.setInterceptsMouseClicks (false, false);
        setThemeColour (listeners, juce::Label::textColourId, &Palette::textMuted);
        addAndMakeVisible (listeners);
    }

    void showSource (const SidechainChoice& source)
    {
        if (picker != nullptr)
            picker->show (source);
    }

    void showListeners (const juce::String& text) { listeners.setText (text, juce::dontSendNotification); }

    void resized() override
    {
        auto area = getLocalBounds();

        if (picker != nullptr)
        {
            picker->setBounds (area.removeFromTop (controlHeight));
            area.removeFromTop (Space::xs);
        }

        // Listen beside the cuts where there is room, else under them.
        const auto cutsWidth = cuts.getPreferredWidth(), cutsHeight = cuts.getPreferredHeight();

        if (area.getWidth() >= cutsWidth + Space::s + listenWidth)
        {
            auto row = area.removeFromTop (cutsHeight);
            cuts.setBounds (row.removeFromLeft (cutsWidth));
            row.removeFromLeft (Space::s);
            listen.setBounds (row.removeFromLeft (listenWidth).withSizeKeepingCentre (listenWidth, controlHeight));
        }
        else
        {
            cuts.setBounds (area.removeFromTop (cutsHeight).withSizeKeepingCentre (std::min (area.getWidth(), cutsWidth), cutsHeight));
            area.removeFromTop (Space::xs);
            listen.setBounds (area.removeFromTop (controlHeight));
        }

        area.removeFromTop (Space::xs);
        listeners.setBounds (area.removeFromTop (std::min (area.getHeight(), 3 * getLabelHeight())));
    }

    std::unique_ptr<SidechainButton> picker;
    juce::TextButton listen { "Listen" };

private:
    ParameterControls& cuts;
    juce::Label listeners;
};

//==============================================================================
// The band's part: the band tabs (and the Sidechain's, where that part doesn't show beside); down its left, the band's curve,
// square, under its tools (and Monitor's switch, where Monitor doesn't show beside), then its mode and its switches; its knobs
// beside them, or Monitor in their place while its switch is on; Monitor beside them where there is room, then the Sidechain
// part. Only the selected band's controls show.
class MultibandEditor::BandPanel : public juce::Component
{
public:
    // A band's controls.
    struct Band
    {
        ChoiceButtons* mode = nullptr;
        std::array<juce::TextButton*, 2> switches {}; // Solo and Key; the master's Key and Mid Drives
        ParameterControls* knobs = nullptr;           // Pre, Stereo, Sat and Ceiling; Attack, Release, Sustain and Release 2
        ParameterControls* boxes = nullptr;           // the curve boxes
        juce::Component* detection = nullptr;         // Peak or RMS, named
        ParameterControls* post = nullptr;            // the master's

        std::vector<juce::Component*> getAll() const
        {
            std::vector<juce::Component*> all { mode, switches[0], switches[1], knobs, boxes, detection };

            if (post != nullptr)
                all.push_back (post);

            return all;
        }
    };

    // Its widths: the least, the dock's; and where there is room, Monitor and the Sidechain part beside the band.
    static constexpr int leastWidth = curveLeast + Space::s + gridLeast;
    static constexpr int preferredWidth = curveWide + Space::s + gridMost + columnGap + monitorWidth + columnGap + sidechainWidth;

    static int getPreferredHeight()
    {
        return rowHeight + Space::xs + 3 * ParameterControls::getCellHeight (ParameterControls::Size::compact);
    }

    BandPanel (MultibandEditor& e, DynamicsCurve& c, MonitorPart& m, SidechainPart& s)
        : editor (e),
          curve (c),
          tools (c),
          monitor (m),
          sidechain (s)
    {
        setTitle ("Band");

        for (int band = 0; band < Multiband::numBands; ++band)
        {
            auto& tab = *tabs.add (std::make_unique<juce::TextButton> (bandNames[band]));
            tab.setClickingTogglesState (true);
            tab.setRadioGroupId (tabGroup);
            tab.setColour (juce::TextButton::buttonOnColourId, getBandColour (band).withAlpha (0.6f));
            tab.setConnectedEdges ((band > 0 ? juce::Button::ConnectedOnLeft : 0)
                                   | (band + 1 < Multiband::numBands ? juce::Button::ConnectedOnRight : 0));
            tab.setTooltip (juce::String ("Shows the ")
                            + (band == Multiband::master ? "master band's" : bandNames[band] + juce::String (" band's"))
                            + " curve and controls");
            tab.onClick = [this, band] { editor.selectBand (band); };
            addAndMakeVisible (tab);
        }

        sidechainTab.setClickingTogglesState (true);
        sidechainTab.setRadioGroupId (tabGroup);
        sidechainTab.setTooltip ("Shows the Sidechain part in place of the band: the track the bands whose Sidechain is on hear, its "
                                 "cuts and Listen");
        sidechainTab.onClick = [this] { showSidechain (true); };
        addChildComponent (sidechainTab);

        monitorButton.setClickingTogglesState (true);
        monitorButton.setTooltip ("Monitor: shows the band's last seconds in place of its knobs: what comes in, what goes out, what its "
                                  "compressor hears and its gain");
        monitorButton.onClick = [this] { showMonitor (monitorButton.getToggleState()); };
        addChildComponent (monitorButton);

        addAndMakeVisible (curve);
        addAndMakeVisible (tools);
        addChildComponent (monitor);
        addChildComponent (sidechain);
    }

    void addBand (int band, const Band& added)
    {
        bands[(size_t) band] = added;

        for (auto* control : added.getAll())
            addChildComponent (control);
    }

    void show (int band)
    {
        selected = band;
        showingSidechain = false;
        resized();
    }

    void showSidechain (bool shown)
    {
        showingSidechain = shown;
        resized();
    }

    void showMonitor (bool shown)
    {
        showingMonitor = shown;
        monitorButton.setToggleState (shown, juce::dontSendNotification);
        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        monitorBeside = area.getWidth() >= leastWidth + columnGap + monitorLeast;
        sidechainBeside = area.getWidth() >= leastWidth + columnGap + monitorLeast + columnGap + sidechainWidth;
        const auto sidechainShown = sidechainBeside || showingSidechain;
        const auto bandShown = sidechainBeside || ! showingSidechain;

        layOutTabs (area.removeFromTop (rowHeight));
        area.removeFromTop (Space::xs);

        // The Sidechain part, at the right where there is room, else in the band's place by its tab.
        sidechain.setVisible (sidechainShown);

        if (sidechainBeside)
        {
            sidechain.setBounds (area.removeFromRight (sidechainWidth));
            area.removeFromRight (columnGap);
        }
        else if (sidechainShown)
        {
            sidechain.setBounds (area);
        }

        // The curve as large as the height and the width leave it, narrowed first, then the knobs, to their least.
        const auto besideWidth = monitorBeside ? columnGap + monitorLeast : 0;
        auto curveSide = juce::jlimit (curveLeast, curveMost, area.getHeight() - leftRowsHeight);
        auto gridWidth = gridMost;
        auto over = curveSide + Space::s + gridWidth + besideWidth - area.getWidth();

        if (over > 0)
        {
            const auto fromCurve = std::min (over, curveSide - curveLeast);
            curveSide -= fromCurve;
            over -= fromCurve;
        }

        if (over > 0)
            gridWidth = std::max (gridLeast, gridWidth - over);

        if (! monitorBeside)
            gridWidth = std::max (0, area.getWidth() - curveSide - Space::s); // the knobs, or Monitor, take the rest

        auto left = area.removeFromLeft (curveSide);
        area.removeFromLeft (Space::s);
        auto gridArea = area.removeFromLeft (gridWidth);
        area.removeFromLeft (columnGap);

        // The curve's tools, and Monitor's switch where Monitor can't show beside the knobs (Freeze and Snap then in the
        // curve's menu).
        auto toolbar = left.removeFromTop (rowHeight);
        left.removeFromTop (Space::xs);
        tools.setCompact (! monitorBeside);
        tools.setBounds (toolbar.removeFromLeft (std::min (toolbar.getWidth(), tools.getPreferredWidth())));
        monitorButton.setVisible (bandShown && ! monitorBeside);

        if (! monitorBeside)
        {
            toolbar.removeFromLeft (Space::xs);
            monitorButton.setBounds (
                toolbar.removeFromRight (std::min (toolbar.getWidth(), monitorButton.getBestWidthForHeight (rowHeight))));
        }

        curve.setBounds (left.removeFromTop (curveSide)); // square
        left.removeFromTop (Space::xs);
        const auto modeArea = left.removeFromTop (2 * rowHeight);
        left.removeFromTop (Space::xs);
        const auto switchesArea = left.removeFromTop (rowHeight);
        curve.setVisible (bandShown);
        tools.setVisible (bandShown);

        // Monitor beside the knobs, or in their place while its switch is on.
        const auto monitorShown = bandShown && (monitorBeside || showingMonitor);
        const auto gridShown = bandShown && (monitorBeside || ! showingMonitor);
        monitor.setVisible (monitorShown);
        monitor.setBounds (monitorBeside ? area : gridArea);

        for (int b = 0; b < Multiband::numBands; ++b)
        {
            const auto& band = bands[(size_t) b];

            if (band.mode == nullptr) // not added yet
                continue;

            const auto isSelected = b == selected;

            for (auto* control : band.getAll())
                control->setVisible (bandShown && isSelected);

            for (auto* control : std::initializer_list<juce::Component*> { band.knobs, band.boxes, band.detection, band.post })
                if (control != nullptr)
                    control->setVisible (gridShown && isSelected);

            if (! isSelected)
                continue;

            band.mode->setBounds (modeArea);
            layOutSwitches (switchesArea, band.switches);

            // The knobs in two rows, then the curve boxes, Peak or RMS and the master's Post.
            const auto cellHeight = ParameterControls::getCellHeight (ParameterControls::Size::compact);
            const auto cell = gridArea.getWidth() / 4;
            auto rows = gridArea;
            band.knobs->setBounds (rows.removeFromTop (2 * cellHeight));
            auto last = rows.removeFromTop (cellHeight);
            band.boxes->setBounds (last.removeFromLeft (2 * cell));
            band.detection->setBounds (last.removeFromLeft (cell));

            if (band.post != nullptr)
                band.post->setBounds (last.removeFromLeft (cell));
        }
    }

    juce::OwnedArray<juce::TextButton> tabs;
    juce::TextButton sidechainTab { "Sidechain" }, monitorButton { "Monitor" };

private:
    static constexpr int tabGroup = 1;

    MultibandEditor& editor;
    DynamicsCurve& curve;
    DynamicsCurveTools tools;
    MonitorPart& monitor;
    SidechainPart& sidechain;
    std::array<Band, Multiband::numBands> bands {};
    int selected = Multiband::master;
    bool showingSidechain = false, showingMonitor = false; // by their tab and switch, where they don't show beside
    bool sidechainBeside = false, monitorBeside = false;

    // The band tabs, joined, each as wide as its name, then the Sidechain's after a gap where it has one: all narrowed alike
    // where the row is narrower.
    void layOutTabs (juce::Rectangle<int> row)
    {
        sidechainTab.setVisible (! sidechainBeside);
        std::vector<juce::TextButton*> shown (tabs.begin(), tabs.end());

        if (! sidechainBeside)
            shown.push_back (&sidechainTab);

        std::vector<int> widths;
        auto total = 0;

        for (auto* tab : shown)
        {
            widths.push_back (tab->getBestWidthForHeight (row.getHeight()));
            total += widths.back();
        }

        const auto gap = sidechainBeside ? 0 : Space::s;
        const auto room = std::max (1, row.getWidth() - gap);

        for (size_t i = 0; i < shown.size(); ++i)
        {
            if (shown[i] == &sidechainTab)
                row.removeFromLeft (gap);

            shown[i]->setBounds (row.removeFromLeft (total > room ? widths[i] * room / total : widths[i]));
        }

        const auto sidechainTabOn = showingSidechain && ! sidechainBeside;

        for (int band = 0; band < tabs.size(); ++band)
            tabs[band]->setToggleState (! sidechainTabOn && band == selected, juce::dontSendNotification);

        sidechainTab.setToggleState (sidechainTabOn, juce::dontSendNotification);
    }

    // A band's two switches, sharing the row as their texts' widths do.
    static void layOutSwitches (juce::Rectangle<int> row, const std::array<juce::TextButton*, 2>& switches)
    {
        const auto first = switches[0]->getBestWidthForHeight (row.getHeight());
        const auto second = switches[1]->getBestWidthForHeight (row.getHeight());
        const auto room = row.getWidth() - Space::xs;
        switches[0]->setBounds (row.removeFromLeft (room * first / std::max (1, first + second)));
        row.removeFromLeft (Space::xs);
        switches[1]->setBounds (row);
    }
};

//==============================================================================
MultibandEditor::MultibandEditor (const DeviceHandle& handle, ProjectContext* context, DeviceID id)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    for (const auto& parameter : Multiband::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    constexpr auto compact = ParameterControls::Size::compact;

    auto& shownCurve = keep (std::make_unique<DynamicsCurve> (device, values));
    curve = &shownCurve;
    curve->setCurve (Multiband::getBandParameter (selectedBand, Multiband::points), getBandColour (selectedBand));
    curve->onEdit = [this] { refresh(); };
    analysis = &keep (std::make_unique<Analysis> (*this));
    auto& monitorPart = keep (std::make_unique<MonitorPart> (*this));

    // The bands' part: the graph, the crossovers' slopes in its top corners, over its knobs: the low cut, the crossovers and
    // LMH Delay; the bands' levels and LMH Mix.
    const auto makeSlope = [this] (int index, const juce::String& which) -> ChoiceButtons&
    {
        auto& slope = keep (std::make_unique<ChoiceButtons> (
            device, idOf (index), ChoiceButtons::Direction::across,
            std::vector<juce::String> { "The " + which + " crossover's slope: 12 dB per octave, gentler",
                                        "The " + which + " crossover's slope: 24 dB per octave, the bands kept further apart" },
            std::vector<juce::String> { "12", "24" }));
        addControl (idOf (index), slope);
        return slope;
    };

    auto& shownGraph =
        keep (std::make_unique<BandsGraph> (*this, makeSlope (Multiband::lowSlope, "low"), makeSlope (Multiband::highSlope, "high")));
    bandsGraph = &shownGraph;
    auto& bandKnobs =
        makeControls ({ idOf (Multiband::lowCut), idOf (Multiband::lowFrequency), idOf (Multiband::highFrequency),
                        idOf (Multiband::lmhDelay), idOf (Multiband::low, Multiband::post), idOf (Multiband::mid, Multiband::post),
                        idOf (Multiband::high, Multiband::post), idOf (Multiband::lmhMix) },
                      4, compact, {}, { "Low Cut", "Low Freq", "High Freq", "LMH Delay", "Low", "Mid", "High", "LMH Mix" });
    auto& bandsPart = keep (std::make_unique<BandsPart> (shownGraph, bandKnobs));

    // The Sidechain part.
    auto& cuts =
        makeControls ({ idOf (Multiband::keyLowCut), idOf (Multiband::keyHighCut) }, 2, compact, sidechainLabel, { "Low Cut", "High Cut" });
    sidechain = &keep (std::make_unique<SidechainPart> (*this, cuts, context, id));

    // The band's part, and each band's controls in it, by their parameters' IDs, named as Maximus names them.
    auto& panel = keep (std::make_unique<BandPanel> (*this, shownCurve, monitorPart, *sidechain));
    bandPanel = &panel;

    const auto makeSwitch = [this] (std::string_view parameterID, const juce::String& text,
                                    const juce::String& tooltip) -> juce::TextButton&
    {
        auto& made = keep (std::make_unique<ParameterSwitch> (device, parameterID, text, tooltip));
        addControl (parameterID, made);
        return made;
    };

    const juce::String keyTip ("Sidechain: the band's compressor hears the sidechain, through its cuts, in place of the band, so "
                               "that it ducks under another track (with no track picked, under what comes in)");

    for (int band = 0; band < Multiband::numBands; ++band)
    {
        BandPanel::Band shown;
        auto& mode = keep (std::make_unique<ChoiceButtons> (device, idOf (band, Multiband::mode), ChoiceButtons::Direction::twoAcross,
                                                            std::vector<juce::String> {
                                                                "The band is on",
                                                                "The band's compressor is off; its gains, saturation and stereo still work",
                                                                "The band is silent",
                                                                band == Multiband::mid || band == Multiband::master
                                                                    ? "The band passes as it comes in"
                                                                    : "The band is off: the mid band takes its room",
                                                            }));
        addControl (idOf (band, Multiband::mode), mode);
        shown.mode = &mode;

        if (band == Multiband::master)
            shown.switches = { &makeSwitch (idOf (band, Multiband::key), sidechainLabel, keyTip),
                               &makeSwitch (idOf (Multiband::midDrivesMaster), "Mid Drives",
                                            "Mid Drives Master: the master's compressor hears the mid band in place of what it "
                                            "plays, the low and high bands' compression off meanwhile, as Maximus's Master Mid "
                                            "mode ducks, gates and de-esses") };
        else
            shown.switches = { &makeSwitch (idOf (Multiband::getSoloParameter (band)), "Solo",
                                            "Solo: only the soloed bands play, and the master band after them"),
                               &makeSwitch (idOf (band, Multiband::key), sidechainLabel, keyTip) };

        shown.knobs = &makeControls ({ idOf (band, Multiband::pre), idOf (band, Multiband::stereo), idOf (band, Multiband::saturation),
                                       idOf (band, Multiband::ceiling), idOf (band, Multiband::attack), idOf (band, Multiband::release),
                                       idOf (band, Multiband::sustain), idOf (band, Multiband::release2) },
                                     4, compact, {}, { "Pre", "Stereo", "Sat", "Ceiling", "Attack", "Release", "Sustain", "Release 2" });
        shown.boxes = &makeControls ({ idOf (band, Multiband::curve), idOf (band, Multiband::releaseCurve) }, 2, compact, {},
                                     { "Curve", "Rel Curve" });

        auto& detection = keep (std::make_unique<ChoiceButtons> (
            device, idOf (band, Multiband::detection), ChoiceButtons::Direction::down,
            std::vector<juce::String> { "Peak: the compressor hears each peak, held for the sustain: the fastest",
                                        "RMS: the compressor hears the average over the sustain, so transients pass" }));
        addControl (idOf (band, Multiband::detection), detection);
        shown.detection = &keep (std::make_unique<NamedControl> (detection, "Detection"));

        if (band == Multiband::master)
            shown.post = &makeControls ({ idOf (band, Multiband::post) }, 1, compact, {}, { "Post" });

        panel.addBand (band, shown);
    }

    // The meters' first readings would show what played before the editor opened.
    if (auto* multiband = device.getProcessor<Multiband>())
    {
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            multiband->getInputMeter (band).take();
            multiband->getOutputMeter (band).take();
            multiband->getHeardMeter (band).take();
            multiband->getReductionMeter (band).take();
        }

        multiband->getMainInputMeter().take();
    }

    // The two parts side by side, each whole: in the default dock as narrow as they go, the bands' graph 200 pixels wide and
    // the band's curve 116; wider, the graph and the curve grow, and Monitor and the Sidechain part come beside the band.
    const auto knobsHeight = bandKnobs.getPreferredHeight();
    setParts ({ { { "Bands", &bandsPart, bandsWidth, bandsGraphHeight + Space::xs + knobsHeight, bandsGraphLeast + Space::xs + knobsHeight,
                    true, gridLeast } },
                { { "Band", &panel, BandPanel::preferredWidth, BandPanel::getPreferredHeight(), BandPanel::getPreferredHeight(), true,
                    BandPanel::leastWidth } } });
    selectBand (selectedBand);
}

MultibandEditor::~MultibandEditor()
{
    if (auto* scope = device.getScope())
        scope->setListening (false);
}

juce::PopupMenu MultibandEditor::getTracesMenu()
{
    juce::PopupMenu menu;
    const juce::Component::SafePointer<MultibandEditor> safeThis (this);

    for (int i = 0; i < Analysis::numTraces; ++i)
        menu.addItem (traceNames[i], true, analysis->traces[(size_t) i],
                      [safeThis, i]
                      {
                          if (safeThis != nullptr)
                          {
                              auto& shown = safeThis->analysis->traces[(size_t) i];
                              shown = ! shown;
                              safeThis->analysis->repaint();
                          }
                      });

    return menu;
}

void MultibandEditor::selectBand (int band)
{
    selectedBand = juce::jlimit (0, Multiband::numBands - 1, band);
    bandPanel->show (selectedBand);
    curve->setCurve (Multiband::getBandParameter (selectedBand, Multiband::points), getBandColour (selectedBand));
    analysis->repaint();
    bandsGraph->repaint();
}

juce::Button& MultibandEditor::getBandButton (int band)
{
    return *bandPanel->tabs[band];
}

DynamicsCurve& MultibandEditor::getCurve() const noexcept
{
    return *curve;
}

juce::Component& MultibandEditor::getBandsGraph() const noexcept
{
    return *bandsGraph;
}

float MultibandEditor::getBandsX (double frequency) const
{
    return bandsGraph->getFrequencies().xFor (frequency);
}

float MultibandEditor::getBandsY (float decibels) const
{
    return bandsGraph->yForLevel (decibels);
}

juce::Component& MultibandEditor::getMonitor() const noexcept
{
    return *analysis;
}

juce::Button& MultibandEditor::getMonitorButton()
{
    return bandPanel->monitorButton;
}

bool MultibandEditor::isMonitorShowing() const
{
    return analysis->isShowing();
}

juce::String MultibandEditor::getMonitorKeyName() const
{
    return analysis->getKeyName();
}

juce::Component& MultibandEditor::getSidechainPart() const noexcept
{
    return *sidechain;
}

juce::Button& MultibandEditor::getSidechainTab()
{
    return bandPanel->sidechainTab;
}

bool MultibandEditor::isSidechainShowing() const
{
    return sidechain->isShowing();
}

SidechainButton* MultibandEditor::getSidechainButton() const noexcept
{
    return sidechain->picker.get();
}

juce::Button& MultibandEditor::getListenButton()
{
    return sidechain->listen;
}

juce::String MultibandEditor::describeListeners() const
{
    juce::StringArray bands;

    for (int band = 0; band < Multiband::numBands; ++band)
        if (values[(size_t) Multiband::getBandParameter (band, Multiband::key)] > 0.5f)
            bands.add (juce::String (bandNames[band]).toLowerCase());

    if (bands.isEmpty())
        return "No band hears it yet: turn on a band's Sidechain, under its mode";

    const auto last = bands[bands.size() - 1];
    bands.remove (bands.size() - 1);

    if (bands.isEmpty())
        return "Heard by the " + last + " band, in place of its own sound";

    return "Heard by the " + bands.joinIntoString (", ") + " and " + last + " bands, in place of their own sound";
}

void MultibandEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    // The meters into Monitor's newest column, and what the curve's light reaches; Listen, while it is on and shows.
    if (auto* multiband = device.getProcessor<Multiband>())
    {
        Analysis::Column column {};

        for (int band = 0; band < Multiband::numBands; ++band)
        {
            const auto b = (size_t) band * 4;
            column[b] = multiband->getInputMeter (band).take();
            column[b + 1] = multiband->getOutputMeter (band).take();
            column[b + 2] = multiband->getHeardMeter (band).take();
            const auto reduction = multiband->getReductionMeter (band).take();
            column[b + 3] = reduction <= -120.0f ? 0.0f : reduction;
            shownHeard[(size_t) band] = column[b + 2];
        }

        column.back() = multiband->getMainInputMeter().take();
        analysis->push (column);

        if (sidechain->listen.getToggleState() && sidechain->listen.isShowing())
            multiband->listenToKey();
    }

    // The spectrum of what it plays, over the bands.
    if (auto* scope = device.getScope())
    {
        scope->setListening (true);
        bandsGraph->spectrum.setSampleRate (scope->getSampleRate());

        if (auto* output = scope->getOutput())
            bandsGraph->spectrum.pull (*output);

        if (bandsGraph->spectrum.update())
            bandsGraph->repaint();
    }

    // The key's track, on its picker, and in Monitor while the selected band hears it; which bands hear it.
    const auto source = device.getSidechainSource();

    if (source.track != shownSource.track || source.name != shownSource.name || source.colour != shownSource.colour)
    {
        shownSource = source;
        sidechain->showSource (source);
    }

    sidechain->showListeners (describeListeners());
    const auto hearsKey = values[(size_t) Multiband::getBandParameter (selectedBand, Multiband::key)] > 0.5f;
    analysis->setKey (hearsKey && source.track.isValid() ? source.name : juce::String(), source.colour);

    // The curve, lit up to the level heard, the bands and Monitor's level marks, repainted when they move.
    curve->setHeardLevel (shownHeard[(size_t) selectedBand]);
    auto numbers = values;
    numbers.push_back (shownHeard[(size_t) selectedBand]);

    if (drawnValues.changed (std::move (numbers)))
    {
        curve->repaint();
        bandsGraph->repaint();
        analysis->repaint();
    }
}
} // namespace phazite::ui
