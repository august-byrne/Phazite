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
// The editor's one panel, in Maximus's layout: the curve and the analysis display over their toolbars, and the selected band's
// controls and the global ones below.
class MultibandEditor::Panel : public juce::Component
{
public:
    explicit Panel (MultibandEditor& e)
        : editor (e)
    {
        setTitle ("Multiband");

        // The curve's toolbar: its menu, Freeze and Snap.
        addAndMakeVisible (tools);

        // The display's toolbar: its traces' switches, its speed, the spectrum, and Monitor or Bands.
        static constexpr const char* traceNames[] = { "In", "Band In", "Out", "Heard", "Gain", "All Gains", "Pre/Post" };
        static constexpr const char* traceTips[] = {
            "Shows what comes into the multiband",
            "Shows what comes into the selected band, after its pre gain",
            "Shows what the selected band puts out",
            "Shows what the selected band's compressor hears",
            "Shows the selected band's gain",
            "Shows every band's gain, each in its colour",
            "Shows the selected band's pre (purple) and post (blue) gains",
        };

        for (int i = 0; i < Analysis::numTraces; ++i)
        {
            auto& button = *traceButtons.add (std::make_unique<juce::TextButton> (traceNames[i]));
            button.setClickingTogglesState (true);
            button.setToggleState (editor.analysis->traces[(size_t) i], juce::dontSendNotification);
            button.setTooltip (traceTips[i]);
            button.onClick = [this, i, &button]
            {
                editor.analysis->traces[(size_t) i] = button.getToggleState();
                editor.analysis->repaint();
            };
            addAndMakeVisible (button);
        }

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

        monitorButton.setToggleState (true, juce::dontSendNotification);
        monitorButton.setTooltip ("Monitor: the last seconds of the selected band");
        bandsButton.setTooltip ("Bands: the bands across the spectrum, with their crossovers and pre gains to drag");
        monitorButton.onClick = [this] { editor.showBands (false); };
        bandsButton.onClick = [this] { editor.showBands (true); };

        // The band tabs, low in red, mid in orange, high in yellow and the master in green, as Maximus colours them.
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            auto& button = *bandButtons.add (std::make_unique<juce::TextButton> (bandNames[band]));
            button.setClickingTogglesState (true);
            button.setRadioGroupId (2);
            button.setColour (juce::TextButton::buttonOnColourId, getBandColour (band).withAlpha (0.6f));
            button.setTooltip (juce::String ("Shows the ")
                               + (band == Multiband::master ? "master band's" : bandNames[band] + juce::String (" band's"))
                               + " curve and controls");
            button.onClick = [this, band] { editor.selectBand (band); };
            addAndMakeVisible (button);
        }
    }

    // Each band's own controls, of which the selected band's show.
    struct BandControls
    {
        ChoiceButtons* mode = nullptr;
        ParameterControls* solo = nullptr; // none for the master
        ParameterControls* stereo = nullptr;
        ParameterControls* gains = nullptr;
        ParameterControls* saturation = nullptr;
        ParameterControls* envelope = nullptr;
        ParameterControls* curves = nullptr;
        ChoiceButtons* detection = nullptr;
        ParameterControls* release2 = nullptr;

        std::vector<juce::Component*> getAll() const
        {
            return { mode, solo, stereo, gains, saturation, envelope, curves, detection, release2 };
        }
    };

    std::array<BandControls, Multiband::numBands> bands;
    ParameterControls *lmhDelay = nullptr, *lmhMix = nullptr, *frequencies = nullptr, *lowCut = nullptr, *midDrives = nullptr;
    ChoiceButtons *lowSlope = nullptr, *highSlope = nullptr;
    juce::OwnedArray<juce::TextButton> bandButtons;

    void addControl (juce::Component* control, bool visible)
    {
        if (control == nullptr)
            return;

        addChildComponent (control);
        control->setVisible (visible);
    }

    void showBand (int shown)
    {
        for (int band = 0; band < Multiband::numBands; ++band)
        {
            for (auto* control : bands[(size_t) band].getAll())
                if (control != nullptr)
                    control->setVisible (band == shown);

            bandButtons[band]->setToggleState (band == shown, juce::dontSendNotification);
        }

        resized();
    }

    void syncViewButtons()
    {
        monitorButton.setToggleState (! editor.analysis->showsBands, juce::dontSendNotification);
        bandsButton.setToggleState (editor.analysis->showsBands, juce::dontSendNotification);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (std::max (200, area.getHeight() / 2));
        area.removeFromTop (Space::s);

        // The curve, square, over its toolbar; the display beside it, over its own.
        auto curveColumn = top.removeFromLeft (top.getHeight() - toolbarHeight);
        auto curveTools = curveColumn.removeFromBottom (toolbarHeight).reduced (0, 2);
        editor.curve->setBounds (curveColumn);
        tools.setBounds (curveTools);

        top.removeFromLeft (Space::s);
        auto displayTools = top.removeFromBottom (toolbarHeight).reduced (0, 2);
        editor.analysis->setBounds (top);
        bandsButton.setBounds (displayTools.removeFromRight (64));
        monitorButton.setBounds (displayTools.removeFromRight (72));
        displayTools.removeFromRight (Space::s);
        spectrumButton.setBounds (displayTools.removeFromRight (72));
        displayTools.removeFromRight (Space::xs);
        auto speedArea = displayTools.removeFromRight (std::min (128, displayTools.getWidth() / 3));
        const auto nameWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (speedName.getFont(), speedName.getText()));
        speedName.setBounds (speedArea.removeFromLeft (nameWidth + Space::s));
        speed.setBounds (speedArea.reduced (Space::xs, 0));
        displayTools.removeFromRight (Space::s);

        const auto traceWidth = std::min (64, displayTools.getWidth() / std::max (1, traceButtons.size()));

        for (auto* button : traceButtons)
            button->setBounds (displayTools.removeFromLeft (traceWidth));

        // Below: the band tabs and Solo; the mode and stereo; the gains and saturation; the envelope; LMH; the crossovers.
        const auto& shown = bands[(size_t) editor.selectedBand];
        auto tabs = area.removeFromLeft (88);

        for (auto* button : bandButtons)
            button->setBounds (tabs.removeFromTop (controlHeight));

        tabs.removeFromTop (Space::s);

        if (shown.solo != nullptr)
            shown.solo->setBounds (tabs.removeFromTop (shown.solo->getPreferredHeight())
                                       .withSizeKeepingCentre (shown.solo->getPreferredWidth(), shown.solo->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto modes = area.removeFromLeft (96);
        shown.mode->setBounds (modes.removeFromTop (shown.mode->getPreferredHeight()));
        modes.removeFromTop (Space::s);
        shown.stereo->setBounds (modes.removeFromTop (shown.stereo->getPreferredHeight())
                                     .withSizeKeepingCentre (shown.stereo->getPreferredWidth(), shown.stereo->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto gains = area.removeFromLeft (shown.gains->getPreferredWidth());
        shown.gains->setBounds (gains.removeFromTop (shown.gains->getPreferredHeight()));
        gains.removeFromTop (Space::s);
        shown.saturation->setBounds (gains.removeFromTop (shown.saturation->getPreferredHeight()));

        // The envelope: its knobs, its curve boxes under them, and Release 2 with Peak or RMS beside it, as wide as its widest row.
        area.removeFromLeft (Space::m);
        const auto releasesWidth = shown.release2->getPreferredWidth() + Space::s + shown.detection->getPreferredWidth();
        auto envelope =
            area.removeFromLeft (std::max ({ shown.envelope->getPreferredWidth(), shown.curves->getPreferredWidth(), releasesWidth }));
        shown.envelope->setBounds (
            envelope.removeFromTop (shown.envelope->getPreferredHeight()).removeFromLeft (shown.envelope->getPreferredWidth()));
        shown.curves->setBounds (
            envelope.removeFromTop (shown.curves->getPreferredHeight()).removeFromLeft (shown.curves->getPreferredWidth()));
        auto releases = envelope.removeFromTop (std::max (shown.release2->getPreferredHeight(), shown.detection->getPreferredHeight()));
        shown.release2->setBounds (
            releases.removeFromLeft (shown.release2->getPreferredWidth()).withHeight (shown.release2->getPreferredHeight()));
        releases.removeFromLeft (Space::s);
        shown.detection->setBounds (
            releases.removeFromLeft (shown.detection->getPreferredWidth()).withHeight (shown.detection->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto lmh = area.removeFromLeft (lmhDelay->getPreferredWidth());
        lmhDelay->setBounds (lmh.removeFromTop (lmhDelay->getPreferredHeight()));
        lmhMix->setBounds (lmh.removeFromTop (lmhMix->getPreferredHeight()));

        area.removeFromLeft (Space::m);
        auto crossovers = area.removeFromLeft (frequencies->getPreferredWidth());
        frequencies->setBounds (crossovers.removeFromTop (frequencies->getPreferredHeight()));
        auto slopes = crossovers.removeFromTop (lowSlope->getPreferredHeight());
        lowSlope->setBounds (slopes.removeFromLeft (slopes.getWidth() / 2).reduced (Space::xs, 0));
        highSlope->setBounds (slopes.reduced (Space::xs, 0));
        crossovers.removeFromTop (Space::xs);
        auto last = crossovers.removeFromTop (lowCut->getPreferredHeight());
        lowCut->setBounds (last.removeFromLeft (lowCut->getPreferredWidth()));
        midDrives->setBounds (last.removeFromLeft (midDrives->getPreferredWidth()));
    }

    // Wide enough for every part, and as tall as its controls need under the graphs.
    static constexpr int preferredWidth = 1000, preferredHeight = 470, minimumHeight = 430;

private:
    MultibandEditor& editor;
    DynamicsCurveTools tools { *editor.curve };
    juce::OwnedArray<juce::TextButton> traceButtons;
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
    auto& shownPanel = keep (std::make_unique<Panel> (*this));
    panel = &shownPanel;
    panel->addAndMakeVisible (shownCurve);
    panel->addAndMakeVisible (shownAnalysis);

    // Each band's controls, by its parameters' IDs, named as Maximus names them.
    const auto id = [] (int band, Multiband::BandField field) { return idOf (Multiband::getBandParameter (band, field)); };

    for (int band = 0; band < Multiband::numBands; ++band)
    {
        auto& shown = panel->bands[(size_t) band];
        const auto visible = band == selectedBand;
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
        shown.mode = &mode;

        if (band != Multiband::master)
            shown.solo = &makeControls ({ id (band, Multiband::solo) }, 1, ParameterControls::Size::compact, {}, { "Solo" });

        shown.stereo = &makeControls ({ id (band, Multiband::stereo) }, 1, ParameterControls::Size::compact, {}, { "Stereo" });
        shown.gains = &makeControls ({ id (band, Multiband::pre), id (band, Multiband::post) }, 2, ParameterControls::Size::compact, {},
                                     { "Pre", "Post" });
        shown.saturation = &makeControls ({ id (band, Multiband::saturation), id (band, Multiband::ceiling) }, 2,
                                          ParameterControls::Size::compact, {}, { "Sat", "Ceiling" });
        shown.envelope = &makeControls ({ id (band, Multiband::attack), id (band, Multiband::release), id (band, Multiband::sustain) }, 3,
                                        ParameterControls::Size::compact, {}, { "Attack", "Release", "Sustain" });
        shown.curves = &makeControls ({ id (band, Multiband::curve), id (band, Multiband::releaseCurve) }, 2,
                                      ParameterControls::Size::compact, {}, { "Curve", "Rel Curve" });
        auto& detection = keep (std::make_unique<ChoiceButtons> (
            device, id (band, Multiband::detection), ChoiceButtons::Direction::down,
            std::vector<juce::String> { "Peak: the compressor hears each peak, held for the sustain: the fastest",
                                        "RMS: the compressor hears the average over the sustain, so transients pass" }));
        addControl (id (band, Multiband::detection), detection);
        shown.detection = &detection;
        shown.release2 = &makeControls ({ id (band, Multiband::release2) }, 1, ParameterControls::Size::compact, {}, { "Release 2" });

        for (auto* control : shown.getAll())
            panel->addControl (control, visible);
    }

    panel->lmhDelay = &makeControls ({ idOf (Multiband::lmhDelay) }, 1, ParameterControls::Size::compact, {}, { "LMH Delay" });
    panel->lmhMix = &makeControls ({ idOf (Multiband::lmhMix) }, 1, ParameterControls::Size::compact, {}, { "LMH Mix" });
    panel->frequencies = &makeControls ({ idOf (Multiband::lowFrequency), idOf (Multiband::highFrequency) }, 2,
                                        ParameterControls::Size::compact, {}, { "Low", "High" });
    panel->lowCut = &makeControls ({ idOf (Multiband::lowCut) }, 1, ParameterControls::Size::compact, {}, { "Low Cut" });
    panel->midDrives = &makeControls ({ idOf (Multiband::midDrivesMaster) }, 1, ParameterControls::Size::compact, {}, { "Mid Drives" });
    auto& lowSlope = keep (std::make_unique<ChoiceButtons> (
        device, idOf (Multiband::lowSlope), ChoiceButtons::Direction::down,
        std::vector<juce::String> { "The low crossover: 12 dB per octave",
                                    "The low crossover: 24 dB per octave, the bands kept further apart" }));
    auto& highSlope = keep (std::make_unique<ChoiceButtons> (
        device, idOf (Multiband::highSlope), ChoiceButtons::Direction::down,
        std::vector<juce::String> { "The high crossover: 12 dB per octave",
                                    "The high crossover: 24 dB per octave, the bands kept further apart" }));
    addControl (idOf (Multiband::lowSlope), lowSlope);
    addControl (idOf (Multiband::highSlope), highSlope);
    panel->lowSlope = &lowSlope;
    panel->highSlope = &highSlope;

    for (auto* control : std::initializer_list<juce::Component*> { panel->lmhDelay, panel->lmhMix, panel->frequencies, panel->lowCut,
                                                                   panel->midDrives, panel->lowSlope, panel->highSlope })
        panel->addControl (control, true);

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

    panel->showBand (selectedBand);
    setParts ({ { { "Multiband", &shownPanel, Panel::preferredWidth, Panel::preferredHeight, Panel::minimumHeight, true } } });
}

MultibandEditor::~MultibandEditor()
{
    if (auto* scope = device.getScope())
        scope->setListening (false);
}

void MultibandEditor::selectBand (int band)
{
    selectedBand = juce::jlimit (0, Multiband::numBands - 1, band);
    panel->showBand (selectedBand);
    curve->setCurve (Multiband::getBandParameter (selectedBand, Multiband::points), getBandColour (selectedBand));
    analysis->repaint();
}

juce::Button& MultibandEditor::getBandButton (int band)
{
    return *panel->bandButtons[band];
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
    panel->syncViewButtons();
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
