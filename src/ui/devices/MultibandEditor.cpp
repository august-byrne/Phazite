// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MultibandEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "DynamicsCurve.h"
#include "devices/dynamics/Multiband.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Multiband;

    constexpr const char* bandNames[] = { "Low", "Mid", "High", "Master" };
    constexpr float gridDecibels[] = { -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 3.0f, 6.0f, 9.0f };
    constexpr int toolbarHeight = 26;
    constexpr int historySize = 1024;       // the monitor's columns, at most
    constexpr double bandsShownMs = 1500.0; // Bands, brought up while a crossover or a pre gain moves under Monitor

    juce::Colour getBandColour (int band)
    {
        return juce::Colour (partColours[(size_t) band]);
    }

    std::string_view idOf (int index)
    {
        return Multiband::info.parameters[(size_t) index].id;
    }

    float axisOf (float decibels)
    {
        return devices::curves::axisForDecibels (decibels);
    }
} // namespace

//==============================================================================
// The analysis display: Monitor or Bands, as Maximus's.
class MultibandEditor::Analysis : public juce::Component
{
public:
    explicit Analysis (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Analysis");
        setDescription ("Monitor: the last seconds of the selected band, what comes in, what goes out, what its compressor hears and "
                        "its gain, scrolling; a click pauses it. Bands: the three bands across the spectrum, as high as their pre "
                        "gains: drag a band's edge to move its crossover, a band up or down for its pre gain.");
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
    bool showsBands = false, paused = false, showsSpectrum = true;
    double bandsUntil = 0.0;      // while a crossover or a pre gain moves under Monitor, Bands shows until then
    bool wasBandsShowing = false; // at the last refresh, so that Monitor comes back when the moment is over
    float speed = 0.5f;           // from 0 (slowest) to 1
    graph::SpectrumTrace spectrum;

    bool isBandsShowing() const { return showsBands || juce::Time::getMillisecondCounterHiRes() < bandsUntil; }

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

        if (sameInARow < historySize && ! isBandsShowing())
            repaint();
    }

    juce::Rectangle<float> getArea() const { return getLocalBounds().toFloat().reduced (1.0f); }

    float yForPlace (float place) const { return getArea().getBottom() - juce::jlimit (0.0f, 1.0f, place) * getArea().getHeight(); }
    float yForDecibels (float decibels) const { return yForPlace (axisOf (decibels)); }

    graph::FrequencyAxis getFrequencies() const { return { getArea() }; }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        graph::paintBackground (g, p, getLocalBounds().toFloat());

        if (isBandsShowing())
            paintBands (g, p);
        else
            paintMonitor (g, p);

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

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isBandsShowing())
        {
            paused = ! paused; // Monitor: a click pauses it, another goes on
            repaint();
            return;
        }

        // Bands: a crossover's edge near the pointer, or the band under it for its pre gain.
        const auto axis = getFrequencies();
        dragged.reset();

        for (const auto global : { Multiband::lowFrequency, Multiband::highFrequency })
            if (std::abs (axis.xFor (editor.values[(size_t) global]) - e.position.x) <= GraphTint::grabDistance)
                dragged = (int) global;

        if (! dragged.has_value())
        {
            const auto frequency = axis.frequencyAt (e.position.x);
            const auto band = frequency < editor.values[(size_t) Multiband::lowFrequency]    ? Multiband::low
                              : frequency < editor.values[(size_t) Multiband::highFrequency] ? Multiband::mid
                                                                                             : Multiband::high;
            dragged = Multiband::getBandParameter (band, Multiband::pre);
        }

        handle = editor.device.getParameter (idOf (*dragged));
        handle.beginGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragged.has_value())
            return;

        if (*dragged == Multiband::lowFrequency || *dragged == Multiband::highFrequency)
        {
            handle.setValue (
                devices::constrain (Multiband::info.parameters[(size_t) *dragged], (float) getFrequencies().frequencyAt (e.position.x)));
        }
        else
        {
            // The band's level under the pointer: its pre gain, on the curves' scale.
            const auto place = juce::jlimit (0.0f, 1.0f, (getArea().getBottom() - e.position.y) / getArea().getHeight());
            const auto decibels = toDecibels (devices::curves::fromAxis (place));
            handle.setValue (devices::constrain (Multiband::info.parameters[(size_t) *dragged], decibels));
        }

        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (handle.isInGesture())
            handle.endGesture();

        dragged.reset();
    }

private:
    MultibandEditor& editor;
    std::vector<Column> columns;
    int newest = 0, sameInARow = historySize; // how many columns in a row came in alike: at first, all of them
    std::optional<int> dragged;               // a crossover's or a band's pre gain's parameter
    ParameterHandle handle;

    static float toDecibels (float gain) { return gain <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (gain); }

    void paintMonitor (juce::Graphics& g, const Palette& p)
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

        if (traces[heard])
        {
            g.setColour (p.modulation);
            g.strokePath (trace ([b] (const Column& c) { return c[b + 2]; }, false), juce::PathStrokeType (1.5f));
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

    void paintBands (juce::Graphics& g, const Palette& p)
    {
        const auto area = getArea();
        const auto axis = getFrequencies();
        const auto low = editor.values[(size_t) Multiband::lowFrequency], high = editor.values[(size_t) Multiband::highFrequency];
        const std::array<std::pair<double, double>, 3> spans { std::pair { 20.0, (double) low }, std::pair { (double) low, (double) high },
                                                               std::pair { (double) high, 20000.0 } };

        // Each band as high as its pre gain, in its colour; faint while it is off or muted.
        for (int band = 0; band < 3; ++band)
        {
            const auto mode = juce::roundToInt (editor.values[(size_t) Multiband::getBandParameter (band, Multiband::mode)]);
            const auto quiet = mode == (int) Multiband::Mode::muted || mode == (int) Multiband::Mode::off;
            const auto left = axis.xFor (spans[(size_t) band].first), right = axis.xFor (spans[(size_t) band].second);
            const auto top = yForDecibels (editor.values[(size_t) Multiband::getBandParameter (band, Multiband::pre)]);
            const auto fill = juce::Rectangle<float>::leftTopRightBottom (left, top, right, area.getBottom());
            g.setColour (getBandColour (band).withAlpha (quiet ? GraphTint::spectrumFill : GraphTint::partCurve));
            g.fillRect (fill);
            g.setColour (getBandColour (band));
            g.drawHorizontalLine (juce::roundToInt (top), left, right);
        }

        // What it plays, over them.
        if (showsSpectrum)
            spectrum.paint (g, axis, { area, -90.0f, 6.0f }, p.text.withAlpha (GraphTint::spectrumFill),
                            p.text.withAlpha (GraphTint::spectrumLine));

        // The crossovers, which a drag moves.
        g.setColour (p.text.withAlpha (GraphTint::marker));

        for (const auto frequency : { low, high })
            g.drawVerticalLine (juce::roundToInt (axis.xFor (frequency)), area.getY(), area.getBottom());

        graph::paintFrequencyGrid (g, p, axis, true);
    }
};

//==============================================================================
namespace
{
    // The traces Monitor shows, each switched in its menu, in Analysis::Trace's order.
    constexpr const char* traceNames[] = { "In", "Band In", "Out", "Heard", "Gain", "All Gains", "Pre/Post" };

    constexpr int bandTabWidth = 72;                    // the band tabs' column
    constexpr int curveWidth = 220, displayWidth = 400; // the curve's and the display's, where there is room,
    constexpr int curveLeast = 180, displayLeast = 340; // and the least they narrow to
    constexpr int graphHeight = 220, graphLeast = 150;  // both

    // A control's size as it wants it: a ParameterControls' grid, or a ChoiceButtons' buttons.
    juce::Point<int> getWantedSize (const juce::Component& control)
    {
        if (auto* controls = dynamic_cast<const ParameterControls*> (&control))
            return { controls->getPreferredWidth(), controls->getPreferredHeight() };

        if (auto* buttons = dynamic_cast<const ChoiceButtons*> (&control))
            return { buttons->getPreferredWidth(), buttons->getPreferredHeight() };

        return { control.getWidth(), control.getHeight() };
    }
} // namespace

// What a part shows of the selected band: each band's controls one beside another (or one under another), only the selected
// band's showing; or controls that every band shares.
class MultibandEditor::BandPart : public juce::Component
{
public:
    explicit BandPart (bool controlsAcross)
        : across (controlsAcross)
    {
    }

    // A band's control; with no band, every band's.
    void add (juce::Component& control, std::optional<int> band = {})
    {
        for (int b = 0; b < Multiband::numBands; ++b)
            if (! band.has_value() || *band == b)
                controls[(size_t) b].push_back (&control);

        addChildComponent (control);
    }

    void show (int band)
    {
        shown = band;

        for (int b = 0; b < Multiband::numBands; ++b)
            if (b != band)
                for (auto* control : controls[(size_t) b])
                    control->setVisible (false);

        for (auto* control : controls[(size_t) band])
            control->setVisible (true);

        resized();
    }

    // The widest and tallest band's, so that the part keeps its size as another band shows.
    juce::Point<int> getPreferredSize() const
    {
        juce::Point<int> size;

        for (const auto& band : controls)
        {
            juce::Point<int> each;

            for (auto* control : band)
            {
                const auto wanted = getWantedSize (*control);
                each = across ? juce::Point<int> (each.x + (each.x > 0 ? gap : 0) + wanted.x, std::max (each.y, wanted.y))
                              : juce::Point<int> (std::max (each.x, wanted.x), each.y + (each.y > 0 ? gap : 0) + wanted.y);
            }

            size = { std::max (size.x, each.x), std::max (size.y, each.y) };
        }

        return size;
    }

    void resized() override
    {
        auto area = getLocalBounds();

        for (auto* control : controls[(size_t) shown])
        {
            const auto wanted = getWantedSize (*control);

            if (across)
            {
                control->setBounds (area.removeFromLeft (wanted.x).withHeight (wanted.y));
                area.removeFromLeft (gap);
            }
            else
            {
                control->setBounds (area.removeFromTop (wanted.y)); // as wide as the part
                area.removeFromTop (gap);
            }
        }
    }

private:
    static constexpr int gap = Space::s;
    const bool across;
    std::array<std::vector<juce::Component*>, Multiband::numBands> controls;
    int shown = 0;
};

//==============================================================================
// The band tabs, one under another, low in red, mid in orange, high in yellow and the master in green, as Maximus colours
// them; and under them the selected band's Solo (the master has none). It leads every page of the editor, as what picks the
// band the other parts show.
class MultibandEditor::BandsPart : public juce::Component
{
public:
    explicit BandsPart (MultibandEditor& editor)
    {
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            auto& button = *buttons.add (std::make_unique<juce::TextButton> (bandNames[band]));
            button.setClickingTogglesState (true);
            button.setRadioGroupId (2);
            button.setColour (juce::TextButton::buttonOnColourId, getBandColour (band).withAlpha (0.6f));
            button.setTooltip (juce::String ("Shows the ")
                               + (band == Multiband::master ? "master band's" : bandNames[band] + juce::String (" band's"))
                               + " curve and controls");
            button.onClick = [&editor, band] { editor.selectBand (band); };
            addAndMakeVisible (button);
        }

        addAndMakeVisible (solos);
    }

    void show (int band)
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == band, juce::dontSendNotification);

        solos.show (band);
    }

    int getPreferredHeight() const
    {
        return Multiband::numBands * (controlHeight + Space::xs) - Space::xs + Space::s + solos.getPreferredSize().y;
    }

    void resized() override
    {
        auto area = getLocalBounds();

        for (auto* button : buttons)
        {
            button->setBounds (area.removeFromTop (controlHeight));
            area.removeFromTop (Space::xs);
        }

        area.removeFromTop (Space::s - Space::xs);
        const auto solo = solos.getPreferredSize();
        solos.setBounds (area.removeFromTop (solo.y).withSizeKeepingCentre (std::min (area.getWidth(), solo.x), solo.y));
    }

    juce::OwnedArray<juce::TextButton> buttons;
    BandPart solos { false };
};

//==============================================================================
// The selected band's curve, square, over its toolbar: its menu, Freeze and Snap.
class MultibandEditor::CurvePart : public juce::Component
{
public:
    explicit CurvePart (DynamicsCurve& c)
        : curve (c),
          tools (c)
    {
        addAndMakeVisible (curve);
        addAndMakeVisible (tools);
    }

    int getLeastWidth() const { return std::max (curveLeast, tools.getPreferredWidth()); }

    void resized() override
    {
        auto area = getLocalBounds();
        tools.setBounds (area.removeFromBottom (toolbarHeight).reduced (0, 2));

        // Square, its levels in and out on one scale, as Maximus draws it.
        const auto side = std::min (area.getWidth(), area.getHeight());
        curve.setBounds (area.withSizeKeepingCentre (side, side));
    }

    DynamicsCurve& curve;
    DynamicsCurveTools tools;
};

//==============================================================================
// The analysis display, over its toolbar: the menu of its traces, the spectrum, Monitor or Bands, and Monitor's speed.
class MultibandEditor::DisplayPart : public juce::Component
{
public:
    explicit DisplayPart (MultibandEditor& e)
        : editor (e)
    {
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

        spectrumButton.setClickingTogglesState (true);
        spectrumButton.setToggleState (true, juce::dontSendNotification);
        spectrumButton.setTooltip ("Shows the spectrum of what the multiband plays over Bands");
        spectrumButton.onClick = [this] { editor.analysis->showsSpectrum = spectrumButton.getToggleState(); };
        addAndMakeVisible (spectrumButton);

        for (auto* button : { &monitorButton, &bandsButton })
        {
            button->setClickingTogglesState (true);
            button->setRadioGroupId (1);
            addAndMakeVisible (button);
        }

        monitorButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        bandsButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        monitorButton.setToggleState (true, juce::dontSendNotification);
        monitorButton.setTooltip ("Monitor: the last seconds of the selected band");
        bandsButton.setTooltip ("Bands: the bands across the spectrum, with their crossovers and pre gains to drag");
        monitorButton.onClick = [this] { editor.showBands (false); };
        bandsButton.onClick = [this] { editor.showBands (true); };
    }

    void syncViewButtons()
    {
        monitorButton.setToggleState (! editor.analysis->showsBands, juce::dontSendNotification);
        bandsButton.setToggleState (editor.analysis->showsBands, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto tools = area.removeFromBottom (toolbarHeight).reduced (0, 2);
        editor.analysis->setBounds (area);

        tracesButton.setBounds (tools.removeFromLeft (tracesWidth));
        tools.removeFromLeft (Space::xs);
        spectrumButton.setBounds (tools.removeFromLeft (spectrumWidth));
        tools.removeFromLeft (Space::s);
        monitorButton.setBounds (tools.removeFromLeft (viewWidth));
        bandsButton.setBounds (tools.removeFromLeft (viewWidth));
        tools.removeFromLeft (Space::s);

        // The speed in what is left, named where there is room.
        const auto nameWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (speedName.getFont(), speedName.getText()));
        speedName.setVisible (tools.getWidth() >= nameWidth + Space::s + minimumSpeedWidth);

        if (speedName.isVisible())
            speedName.setBounds (tools.removeFromLeft (nameWidth + Space::s));

        speed.setBounds (tools.reduced (Space::xs, 0));
    }

    // Its toolbar's least: every button, and a little of the speed.
    static constexpr int tracesWidth = 72, spectrumWidth = 72, viewWidth = 68, minimumSpeedWidth = 48;

private:
    MultibandEditor& editor;
    juce::TextButton tracesButton { "Traces" };
    juce::Slider speed;
    juce::Label speedName { {}, "Speed" };
    juce::TextButton spectrumButton { "Spectrum" }, monitorButton { "Monitor" }, bandsButton { "Bands" };
};

//==============================================================================
MultibandEditor::MultibandEditor (const DeviceHandle& handle)
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

    auto& shownCurve = keep (std::make_unique<DynamicsCurve> (device, values));
    curve = &shownCurve;
    curve->setCurve (Multiband::getBandParameter (selectedBand, Multiband::points), getBandColour (selectedBand));
    curve->onEdit = [this] { refresh(); };
    auto& shownAnalysis = keep (std::make_unique<Analysis> (*this));
    analysis = &shownAnalysis;

    auto& curvePart = keep (std::make_unique<CurvePart> (shownCurve));
    auto& displayPart = keep (std::make_unique<DisplayPart> (*this));
    display = &displayPart;
    auto& tabs = keep (std::make_unique<BandsPart> (*this));
    bandTabs = &tabs;
    auto& modePart = keep (std::make_unique<BandPart> (false));
    auto& gainPart = keep (std::make_unique<BandPart> (true));
    auto& saturationPart = keep (std::make_unique<BandPart> (true));
    auto& envelopePart = keep (std::make_unique<BandPart> (true));
    auto& curvesPart = keep (std::make_unique<BandPart> (true));
    auto& crossoversPart = keep (std::make_unique<BandPart> (true));
    bandParts = { &modePart, &gainPart, &saturationPart, &envelopePart, &curvesPart };

    // Each band's controls, by its parameters' IDs, named as Maximus names them.
    const auto id = [] (int band, Multiband::BandField field) { return idOf (Multiband::getBandParameter (band, field)); };
    constexpr auto compact = ParameterControls::Size::compact;

    for (int band = 0; band < Multiband::numBands; ++band)
    {
        auto& mode = keep (std::make_unique<ChoiceButtons> (device, id (band, Multiband::mode), ChoiceButtons::Direction::down,
                                                            std::vector<juce::String> {
                                                                "The band is on",
                                                                "The band's compressor is off; its gains, saturation and stereo still work",
                                                                "The band is silent",
                                                                band == Multiband::mid || band == Multiband::master
                                                                    ? "The band passes as it comes in"
                                                                    : "The band is off: the mid band takes its room",
                                                            }));
        addControl (id (band, Multiband::mode), mode);
        modePart.add (mode, band);
        modePart.add (makeControls ({ id (band, Multiband::stereo) }, 1, compact, {}, { "Stereo" }), band);

        if (band != Multiband::master)
            tabs.solos.add (makeControls ({ id (band, Multiband::solo) }, 1, compact, {}, { "Solo" }), band);

        gainPart.add (makeControls ({ id (band, Multiband::pre), id (band, Multiband::post) }, 2, compact, {}, { "Pre", "Post" }), band);
        saturationPart.add (
            makeControls ({ id (band, Multiband::saturation), id (band, Multiband::ceiling) }, 2, compact, {}, { "Sat", "Ceiling" }), band);
        envelopePart.add (makeControls ({ id (band, Multiband::attack), id (band, Multiband::release), id (band, Multiband::sustain),
                                          id (band, Multiband::release2) },
                                        4, compact, {}, { "Attack", "Release", "Sustain", "Release 2" }),
                          band);
        curvesPart.add (
            makeControls ({ id (band, Multiband::curve), id (band, Multiband::releaseCurve) }, 2, compact, {}, { "Curve", "Rel Curve" }),
            band);
        auto& detection = keep (std::make_unique<ChoiceButtons> (
            device, id (band, Multiband::detection), ChoiceButtons::Direction::down,
            std::vector<juce::String> { "Peak: the compressor hears each peak, held for the sustain: the fastest",
                                        "RMS: the compressor hears the average over the sustain, so transients pass" }));
        addControl (id (band, Multiband::detection), detection);
        curvesPart.add (detection, band);
    }

    // The global controls: the crossovers with their slopes; LMH Delay and Mix, the low cut and the mid band driving the master.
    crossoversPart.add (
        makeControls ({ idOf (Multiband::lowFrequency), idOf (Multiband::highFrequency) }, 2, compact, {}, { "Low", "High" }));

    const auto addSlope = [this, &crossoversPart] (int index, const juce::String& which)
    {
        auto& slope = keep (std::make_unique<ChoiceButtons> (
            device, idOf (index), ChoiceButtons::Direction::down,
            std::vector<juce::String> { "The " + which + " crossover: 12 dB per octave",
                                        "The " + which + " crossover: 24 dB per octave, the bands kept further apart" }));
        addControl (idOf (index), slope);
        crossoversPart.add (slope);
    };

    addSlope (Multiband::lowSlope, "low");
    addSlope (Multiband::highSlope, "high");

    auto& global =
        makeControls ({ idOf (Multiband::lmhDelay), idOf (Multiband::lmhMix), idOf (Multiband::lowCut), idOf (Multiband::midDrivesMaster) },
                      4, compact, {}, { "LMH Delay", "LMH Mix", "Low Cut", "Mid Drives" });

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

    showBandControls();
    crossoversPart.show (0);

    // Its parts: the band tabs, on every page in the dock; the band's curve, mode, gains and saturation; Monitor; the band's
    // envelope; the crossovers. In a window of its own, or a tall dock, the curve, the band's controls and Monitor across,
    // the rest under them, as Maximus lays them out.
    const auto part = [] (const juce::String& title, BandPart& shown)
    {
        const auto size = shown.getPreferredSize();
        return DeviceLayout::Part { title, &shown, size.x, size.y, size.y };
    };

    auto modeColumn = part ("Mode", modePart);
    modeColumn.width = std::max (modeColumn.width, bandTabWidth);
    const auto tabsHeight = tabs.getPreferredHeight();
    setParts ({ { { "Band", &tabs, bandTabWidth, tabsHeight, tabsHeight } },
                { { "Curve", &curvePart, std::max (curveWidth, curvePart.getLeastWidth()), graphHeight, graphLeast, true,
                    curvePart.getLeastWidth() } },
                { modeColumn },
                { part ("Gain", gainPart), part ("Saturation", saturationPart) },
                { { "Monitor", &displayPart, displayWidth, graphHeight, graphLeast, true, displayLeast } },
                { part ("Envelope", envelopePart), part ("Curves", curvesPart) },
                { part ("Crossovers", crossoversPart),
                  { "LMH", &global, global.getPreferredWidth(), global.getPreferredHeight(), global.getPreferredHeight() } } });
    getLayout().setPinned (0);
}

MultibandEditor::~MultibandEditor()
{
    if (auto* scope = device.getScope())
        scope->setListening (false);
}

void MultibandEditor::showBandControls()
{
    bandTabs->show (selectedBand);

    for (auto* part : bandParts)
        part->show (selectedBand);
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
    showBandControls();
    curve->setCurve (Multiband::getBandParameter (selectedBand, Multiband::points), getBandColour (selectedBand));
    analysis->repaint();
}

juce::Button& MultibandEditor::getBandButton (int band)
{
    return *bandTabs->buttons[band];
}

DynamicsCurve& MultibandEditor::getCurve() const noexcept
{
    return *curve;
}

juce::Component& MultibandEditor::getDisplay() const noexcept
{
    return *analysis;
}

bool MultibandEditor::isShowingBands() const
{
    return analysis->isBandsShowing();
}

void MultibandEditor::showBands (bool bands)
{
    analysis->showsBands = bands;
    analysis->bandsUntil = 0.0;
    display->syncViewButtons();
    analysis->repaint();
}

float MultibandEditor::getBandsX (double frequency) const
{
    return analysis->getFrequencies().xFor (frequency);
}

float MultibandEditor::getBandsY (float decibels) const
{
    return analysis->yForDecibels (decibels);
}

void MultibandEditor::refresh()
{
    // A crossover or a pre gain changed under Monitor brings Bands up for a moment, as in Maximus.
    const auto moved = [this] (int index)
    {
        return parameters[(size_t) index].isValid()
               && ! juce::approximatelyEqual (values[(size_t) index], parameters[(size_t) index].getValue());
    };
    const auto bandsMoved = moved (Multiband::lowFrequency) || moved (Multiband::highFrequency)
                            || moved (Multiband::getBandParameter (Multiband::low, Multiband::pre))
                            || moved (Multiband::getBandParameter (Multiband::mid, Multiband::pre))
                            || moved (Multiband::getBandParameter (Multiband::high, Multiband::pre));

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    if (bandsMoved && ! analysis->showsBands)
        analysis->bandsUntil = juce::Time::getMillisecondCounterHiRes() + bandsShownMs;

    if (const auto bands = analysis->isBandsShowing(); bands != analysis->wasBandsShowing)
    {
        analysis->wasBandsShowing = bands;
        analysis->repaint();
    }

    // The meters into Monitor's newest column, and what the curve's dot rides at.
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
    }

    if (auto* scope = device.getScope())
    {
        scope->setListening (analysis->isBandsShowing() && analysis->showsSpectrum);
        analysis->spectrum.setSampleRate (scope->getSampleRate());

        if (auto* output = scope->getOutput())
            analysis->spectrum.pull (*output);

        if (analysis->spectrum.update() && analysis->isBandsShowing())
            analysis->repaint();
    }

    curve->setHeardLevel (shownHeard[(size_t) selectedBand]);
    auto numbers = values;
    numbers.push_back (shownHeard[(size_t) selectedBand]);

    if (drawnCurve.changed (std::move (numbers)))
    {
        curve->repaint();
        analysis->repaint();
    }
}
} // namespace phazite::ui
