// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SaturatorEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "GraphDot.h"
#include "devices/saturation/Saturator.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Saturator;

    constexpr int scopeLength = 2048;   // samples kept for the wave
    constexpr float reachFall = 0.05f;  // of the reach, per refresh, as it falls back
    constexpr float driveRange = 36.0f; // decibels a drag the curve's height turns the drive

    const devices::ParameterInfo& getInfo (int index)
    {
        return Saturator::info.parameters[(size_t) index];
    }

    // The curve as it plays, for an input from -extent to extent, as a path across an area.
    juce::Path makeShape (Saturator::Shape shape, float driveDecibels, float colour, juce::Rectangle<float> area, float extent)
    {
        return graph::makeCurve (area,
                                 [&] (float x)
                                 {
                                     const auto in = ((x - area.getX()) / area.getWidth() * 2.0f - 1.0f) * extent;
                                     const auto out = juce::jlimit (-1.2f, 1.2f, Saturator::playCurve (shape, in, driveDecibels, colour));
                                     return area.getCentreY() - out / 1.2f * area.getHeight() * 0.5f;
                                 });
    }
} // namespace

//==============================================================================
class SaturatorEditor::CurveView : public juce::Component,
                                   private DisplayTicker::Display
{
public:
    explicit CurveView (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          drive (device.getParameter (getInfo (Saturator::drive).id)),
          shape (device.getParameter (getInfo (Saturator::shape).id)),
          colour (device.getParameter (getInfo (Saturator::colour).id)),
          inputs ((size_t) scopeLength, 0.0f),
          outputs ((size_t) scopeLength, 0.0f),
          lefts ((size_t) scopeLength),
          rights ((size_t) scopeLength)
    {
        setTitle ("Saturation");
        setDescription ("The curve the sound is driven into, lit as far as the sound reaches, and the wave coming in and going out. "
                        "Drag up or down anywhere on them to turn the drive.");
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        drag.onCancelled = [this] { repaint(); };
    }

    ~CurveView() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    float getReach() const noexcept { return reach; }

    juce::Rectangle<float> getCurveArea() const
    {
        const auto area = getLocalBounds().toFloat();
        return area.withWidth (std::min (area.getHeight(), area.getWidth() * 0.5f));
    }

    juce::Rectangle<float> getWaveArea() const { return getLocalBounds().toFloat().withTrimmedLeft (getCurveArea().getWidth() + Space::s); }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        paintCurve (g, p, tint);
        paintWave (g, p, tint);
    }

    // A drag up or down turns the drive from where it is set by hand, as one undo step (graph::HandleDrag).
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            drag.take ({ &drive });
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        const auto moved = -(float) e.getDistanceFromDragStartY() / std::max (1.0f, (float) getHeight()) * driveRange;
        drag.set (0, drag.getStart (0) + moved);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { drag.end(); }

    // Under the pointer, what a drag bends lights up: the whole curve.
    void mouseEnter (const juce::MouseEvent&) override { setHovered (true); }
    void mouseMove (const juce::MouseEvent&) override { setHovered (true); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (false); }

private:
    DeviceHandle device;
    ParameterHandle drive, shape, colour;
    graph::HandleDrag drag { *this }; // after the handle it moves
    float reach = 0.0f;
    bool hovered = false;
    std::vector<float> inputs, outputs; // the newest samples, oldest first
    std::vector<float> lefts, rights;   // pulled from the taps
    DrawnNumbers drawn;

    Saturator::Shape getShape() const { return (Saturator::Shape) juce::roundToInt (shape.getValue()); }
    float getDriveGain() const { return juce::Decibels::decibelsToGain (drive.getValue()); }

    void setHovered (bool isHovered)
    {
        if (isHovered != hovered)
        {
            hovered = isHovered;
            repaint();
        }
    }

    // Takes a tap's newest samples into a store of the last ones, both channels averaged.
    void pull (devices::StereoTap& tap, std::vector<float>& store)
    {
        for (int n; (n = tap.pull (lefts.data(), rights.data(), (int) lefts.size())) > 0;)
        {
            const auto kept = std::min (n, scopeLength);
            std::move (store.begin() + kept, store.end(), store.begin());

            for (int i = 0; i < kept; ++i)
                store[(size_t) (scopeLength - kept + i)] = 0.5f * (lefts[(size_t) (n - kept + i)] + rights[(size_t) (n - kept + i)]);
        }
    }

    void tick() override
    {
        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            if (auto* in = scope->getInput())
                pull (*in, inputs);

            if (auto* out = scope->getOutput())
                pull (*out, outputs);

            // How far the loudest coming in reaches into the curve once driven: up at once, falling back slowly, and gone once
            // it is too low to light any.
            const auto peak = juce::Decibels::decibelsToGain (scope->getInputPeak().take(), devices::DeviceScope::silentDecibels);
            reach = std::max (peak * getDriveGain(), reach * (1.0f - reachFall));
            reach = reach < 1.0e-3f ? 0.0f : reach;
        }

        auto* saturator = device.getProcessor<Saturator>();
        std::vector<float> numbers { drive.getValue(),
                                     shape.getValue(),
                                     colour.getValue(),
                                     reach,
                                     (float) (saturator != nullptr ? saturator->getLatencySamples() : 0),
                                     (float) (device.getScope() != nullptr ? device.getScope()->getSampleRate() : 48000.0) };
        numbers.insert (numbers.end(), inputs.begin(), inputs.end());
        numbers.insert (numbers.end(), outputs.begin(), outputs.end());

        if (drawn.changed (std::move (numbers)))
            repaint();
    }

    void paintCurve (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getCurveArea();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::xs);

        // Its axes, and in equals out.
        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());
        g.drawVerticalLine (juce::roundToInt (inner.getCentreX()), inner.getY(), inner.getBottom());

        // The curve for an input up to full scale (faint, but under the pointer or dragged), then lit as far as the sound
        // reaches.
        constexpr float extent = 1.0f;
        const auto driveGain = getDriveGain();
        g.setColour (hovered || drive.isInGesture() ? tint : tint.withAlpha (GraphTint::partOff));
        const auto curve = makeShape (getShape(), drive.getValue(), colour.getValue(), inner, extent);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        if (const auto lit = juce::jlimit (0.0f, extent, reach / std::max (driveGain, 1.0e-3f)); lit > 0.01f)
        {
            g.saveState();
            g.reduceClipRegion (inner.withSizeKeepingCentre (inner.getWidth() * lit / extent, inner.getHeight()).toNearestInt());
            g.setColour (tint);
            g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth + 1.0f));
            g.restoreState();
        }

        graph::paintReadout (g, p, area, "Drive " + graph::formatDecibels (drive.getValue()), juce::Justification::topLeft);
    }

    void paintWave (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getWaveArea();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::xs);
        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());

        // About 10 ms, from where the wave coming in last rose through zero, so that it stands still.
        const auto shown = std::min (
            scopeLength / 2, juce::roundToInt (0.01 * (device.getScope() != nullptr ? device.getScope()->getSampleRate() : 48000.0)));
        auto start = scopeLength - shown;

        for (int i = scopeLength - shown - 1; i > 0; --i)
        {
            if (inputs[(size_t) i - 1] < 0.0f && inputs[(size_t) i] >= 0.0f)
            {
                start = i;
                break;
            }
        }

        // What goes out comes later by the saturator's latency.
        auto* saturator = device.getProcessor<Saturator>();
        const auto latency = saturator != nullptr ? saturator->getLatencySamples() : 0;

        const auto trace = [&inner, shown] (const std::vector<float>& samples, int from)
        {
            juce::Path path;

            for (int i = 0; i < shown; ++i)
            {
                const auto index = juce::jlimit (0, scopeLength - 1, from + i);
                const juce::Point<float> point {
                    inner.getX() + (float) i / (float) (shown - 1) * inner.getWidth(),
                    inner.getCentreY() - juce::jlimit (-1.2f, 1.2f, samples[(size_t) index]) / 1.2f * inner.getHeight() * 0.5f
                };

                if (i == 0)
                    path.startNewSubPath (point);
                else
                    path.lineTo (point);
            }

            return path;
        };

        g.setColour (p.textMuted.withAlpha (GraphTint::marker));
        g.strokePath (trace (inputs, start), juce::PathStrokeType (1.0f));
        g.setColour (tint);
        g.strokePath (trace (outputs, std::min (start + latency, scopeLength - shown)), juce::PathStrokeType (GraphTint::curveWidth));
    }
};

//==============================================================================
// One button for each shape, drawn with its curve: the shape chosen lit.
class SaturatorEditor::ShapeButtons : public juce::Component,
                                      private DisplayTicker::Display
{
public:
    explicit ShapeButtons (const DeviceHandle& device)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          shape (device.getParameter (getInfo (Saturator::shape).id))
    {
        setTitle ("Shape");
        const auto& choices = getInfo (Saturator::shape).choices;

        for (size_t i = 0; i < choices.size(); ++i)
        {
            auto* button = buttons.add (new ShapeButton (juce::String (choices[i].data(), choices[i].size()), (Saturator::Shape) i));
            button->setRadioGroupId (1);
            button->setClickingTogglesState (false);
            button->onClick = [this, i]
            {
                shape.setValue ((float) i);
                tick();
            };
            addAndMakeVisible (button);
        }

        tick();
    }

    juce::Button& getButton (int index) const { return *buttons[index]; }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto width = area.getWidth() / std::max (1, buttons.size());

        for (auto* button : buttons)
            button->setBounds (area.removeFromLeft (width).reduced (1, 0));
    }

    void tick() override
    {
        const auto chosen = juce::roundToInt (shape.getValue());

        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == chosen, juce::dontSendNotification);
    }

private:
    struct ShapeButton : public juce::Button
    {
        ShapeButton (const juce::String& name, Saturator::Shape s)
            : juce::Button (name),
              curve (s)
        {
            setTitle (name);
            setTooltip (name + ": " + describe (s));
        }

        static juce::String describe (Saturator::Shape s)
        {
            switch (s)
            {
                case Saturator::Shape::tape:
                    return "rounds off early and gently, warm and wide";
                case Saturator::Shape::tube:
                    return "lopsided, adding even harmonics: rich";
                case Saturator::Shape::hard:
                    return "clips the tops off: gritty";
                case Saturator::Shape::fold:
                    return "folds the tops back: a synthetic edge";
                case Saturator::Shape::crush:
                    return "rounds the sound to steps, fewer bits the harder it is driven: lo-fi grit";
                case Saturator::Shape::soft:
                    break;
            }

            return "rounds the tops off smoothly";
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto& p = getPalette (*this);
            const auto tint = getDeviceTint (*this);
            const auto area = getLocalBounds().toFloat();
            g.setColour (getToggleState() ? tint.withAlpha (GraphTint::partCurve) : p.surfaceRaised.brighter (highlighted ? 0.1f : 0.0f));
            g.fillRoundedRectangle (area, cornerRadius);

            if (getToggleState() || down)
            {
                g.setColour (tint);
                g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);
            }

            auto inner = area.reduced (4.0f);
            const auto label = inner.removeFromBottom (TextSize::small);
            g.setColour (getToggleState() ? tint : p.textMuted);
            // Driven as far as shows its bend, Crush as far as shows its steps.
            g.strokePath (makeShape (curve, curve == Saturator::Shape::crush ? 24.0f : 8.0f, 0.0f, inner, 1.0f),
                          juce::PathStrokeType (1.5f));
            g.setColour (p.text);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (getName(), label, juce::Justification::centred, false);

            if (hasKeyboardFocus (false))
            {
                g.setColour (p.accent);
                g.drawRoundedRectangle (area.reduced (1.0f), cornerRadius, 1.5f);
            }
        }

        const Saturator::Shape curve;
    };

    ParameterHandle shape;
    juce::OwnedArray<ShapeButton> buttons;
};

//==============================================================================
// The Band: the spectrum of what comes in (pale) and of what goes out (in the device's colour), with the band lit between
// From and To as it plays (To an octave above From at least), its edges drawn as bars. A press takes the nearer edge, which
// the drag moves sideways from where it is drawn, as one undo step (graph::HandleDrag): Escape takes it back. From pushes
// To up ahead of it, so that the knobs show what plays.
class SaturatorEditor::BandView : public juce::Component,
                                  private DisplayTicker::Display
{
public:
    explicit BandView (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d)
    {
        setTitle ("Band");
        setDescription (about);

        for (const auto& parameter : Saturator::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        readValues();
        drag.onCancelled = [this]
        {
            readValues();
            repaint();
        };
    }

    ~BandView() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    graph::FrequencyAxis getAxis() const { return { getLocalBounds().toFloat().reduced (1.0f) }; }

    float getEdgeX (bool high) const
    {
        const auto band = Saturator::getBand (values);
        return getAxis().xFor (high ? band.to : band.from);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto axis = getAxis();
        graph::paintBackground (g, p, area);
        graph::paintFrequencyGrid (g, p, axis, true);

        // The band, lit, under the spectra.
        const auto lowX = getEdgeX (false), highX = getEdgeX (true);
        g.setColour (tint.withAlpha (GraphTint::spectrumFill));
        g.fillRect (area.withX (lowX).withRight (std::max (lowX + 1.0f, highX)));

        // What comes in, faint; what goes out, in its colour.
        const graph::LevelAxis levels { area, -90.0f, 6.0f };
        input.paint (g, axis, levels, p.textMuted.withAlpha (GraphTint::spectrumFill), p.textMuted.withAlpha (GraphTint::spectrumLine));
        output.paint (g, axis, levels, tint.withAlpha (GraphTint::spectrumFill), tint.withAlpha (GraphTint::spectrumLine));

        // Its edges, the one a press takes ringed.
        for (const auto high : { false, true })
        {
            const auto end = juce::Rectangle<float> (edgeWidth, area.getHeight() - 2.0f * (float) Space::xs)
                                 .withCentre ({ high ? highX : lowX, area.getCentreY() });
            g.setColour (tint);
            g.fillRoundedRectangle (end, cornerRadius);

            if (hovered == (high ? Saturator::bandHigh : Saturator::bandLow))
                paintLitRing (g, p, end, cornerRadius);
        }

        const auto band = Saturator::getBand (values);
        graph::paintReadout (g, p, area, graph::formatFrequency (band.from) + " to " + graph::formatFrequency (band.to),
                             juce::Justification::centredTop);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        // The edge pressed alone: taking the other too would begin its gesture, which on a track in Touch punches it in while
        // the song plays, though it may never move. Of the other, its value as the press finds it.
        dragged = findEdge (e.position);
        const auto other = dragged == Saturator::bandLow ? Saturator::bandHigh : Saturator::bandLow;
        otherAtPress = parameters[(size_t) other].getBaseValue();
        pushed = false;
        drag.take ({ &parameters[(size_t) dragged] });
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        // To stays an octave above From, as the saturator plays it: so a drag of To starts from where its edge is drawn
        // (above its knob's value, if that is set under the octave) and stops an octave above From, and a drag of From
        // pushes To up ahead of it, back as From comes down again; after either, the knobs show what plays.
        const auto moved = (float) e.getDistanceFromDragStartX();
        const auto axis = getAxis();
        auto& toEdge = parameters[(size_t) Saturator::bandHigh];
        const auto toRange = toEdge.getRange();

        if (dragged == Saturator::bandLow)
        {
            drag.moveAlong (
                0, moved, [&axis] (float hertz) { return axis.xFor (hertz); }, [&axis] (float x) { return (float) axis.frequencyAt (x); });

            // To is written only as From pushes it or lets it back (while its lane plays, its value follows the lane, which
            // the drag mustn't write into its step). Set outside a gesture of its own, the change joins From's step, which
            // Escape takes back whole, and begins no recording of To.
            const auto least = 2.0f * parameters[(size_t) Saturator::bandLow].getBaseValue();

            if (least > otherAtPress)
            {
                if (const auto pushing = juce::jlimit (toRange.start, toRange.end, least);
                    ! pushed || ! juce::exactlyEqual (pushing, pushedTo))
                {
                    toEdge.setValue (pushing);
                    pushedTo = pushing;
                    pushed = true;
                }
            }
            else if (pushed)
            {
                toEdge.setValue (otherAtPress);
                pushed = false;
            }
        }
        else
        {
            const auto least = 2.0f * otherAtPress;
            const auto shown = juce::jlimit (toRange.start, toRange.end, std::max (drag.getStart (0), least));
            const auto to = juce::exactlyEqual (moved, 0.0f) ? shown : (float) axis.frequencyAt (axis.xFor (shown) + moved);
            drag.set (0, std::max (to, least));
        }

        readValues();
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        drag.end();
        hover (getLocalBounds().toFloat().contains (e.position) ? findEdge (e.position) : nothing);
    }

    // Over the picture, the pointer that moves sideways, and the nearer edge lit, which a press takes.
    void mouseMove (const juce::MouseEvent& e) override { hover (findEdge (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { hover (nothing); }

private:
    static constexpr int nothing = -1;
    static constexpr float edgeWidth = 4.0f;
    static constexpr const char* about = "The spectrum of what comes in, pale, and of what goes out, with the band lit between From "
                                         "and To: only the band goes into the curve. Drag either edge sideways.";

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the saturator's order
    std::vector<float> values;
    graph::HandleDrag drag { *this }; // after the handles it moves
    graph::SpectrumTrace input, output;
    int dragged = Saturator::bandLow;
    float otherAtPress = 0.0f; // the other edge's value as the press found it
    bool pushed = false;       // From has pushed To in this drag, to pushedTo
    float pushedTo = 0.0f;
    int hovered = nothing;
    DrawnNumbers drawn;

    void readValues()
    {
        for (size_t i = 0; i < parameters.size(); ++i)
            if (parameters[i].isValid())
                values[i] = parameters[i].getValue();
    }

    // The edge nearer the pointer, by its parameter.
    int findEdge (juce::Point<float> position) const
    {
        return std::abs (position.x - getEdgeX (false)) <= std::abs (position.x - getEdgeX (true)) ? Saturator::bandLow
                                                                                                   : Saturator::bandHigh;
    }

    void hover (int edge)
    {
        setMouseCursor (edge == nothing ? juce::MouseCursor::NormalCursor : juce::MouseCursor::LeftRightResizeCursor);

        if (edge != hovered)
        {
            hovered = edge;
            repaint();
        }
    }

    void tick() override
    {
        readValues();
        auto spectraMoved = false;

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            for (auto* trace : { &input, &output })
                trace->setSampleRate (scope->getSampleRate());

            if (auto* tap = scope->getInput())
                input.pull (*tap);

            if (auto* tap = scope->getOutput())
                output.pull (*tap);

            spectraMoved = input.update();
            spectraMoved = output.update() || spectraMoved;
        }

        if (drawn.changed (std::vector<float> (values)) || spectraMoved)
            repaint();
    }
};

//==============================================================================
// The Band's part: its picture, growing with what the column has to spare, over From, To and Listen. While Listen is on and
// the editor shows, the saturator plays the band alone (Saturator::listenToBand(), which the editor asks for again at each
// refresh); hidden, or with the editor gone, the sound comes back within half a second.
class SaturatorEditor::BandPart : public juce::Component
{
public:
    BandPart (BandView& view, ParameterControls& c)
        : picture (view),
          controls (c)
    {
        listen.setTitle ("Listen");
        listen.setTooltip ("Plays the band alone, as it goes out, without the rest of the sound, to set it by ear");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (picture);
        addAndMakeVisible (controls);
        addAndMakeVisible (listen);
    }

    int getPreferredWidth() const { return std::max (pictureWidth, controls.getPreferredWidth() + Space::xs + listenWidth); }
    int getControlsHeight() const { return controls.getPreferredHeight(); }

    void resized() override
    {
        auto area = getLocalBounds();
        auto row = area.removeFromBottom (controls.getPreferredHeight());
        area.removeFromBottom (Space::xs);
        picture.setBounds (area);
        controls.setBounds (row.removeFromLeft (controls.getPreferredWidth()));
        row.removeFromLeft (Space::xs);
        listen.setBounds (row.withSizeKeepingCentre (std::min (row.getWidth(), listenWidth), controlHeight));
    }

    BandView& picture;
    ParameterControls& controls;
    juce::TextButton listen { "Listen" };

    static constexpr int pictureWidth = 260, listenWidth = 64;
};

//==============================================================================
SaturatorEditor::SaturatorEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    auto& shown = keep (std::make_unique<CurveView> (device));
    display = &shown;
    auto& buttons = keep (std::make_unique<ShapeButtons> (device));
    shapes = &buttons;
    addControl ("shape", buttons);

    const auto driveControl = makeControlsPart ("Drive", { "drive" }, 1, ParameterControls::Size::large);
    const auto colour = makeControlsPart ("Colour", { "colour", "tone", "output", "autoGain" }, 4);
    constexpr int displayWidth = 300;

    // The Band, switched at its title's right end, faint while it is off.
    auto& view = keep (std::make_unique<BandView> (device));
    bandView = &view;
    auto& edges = makeControls ({ "bandLow", "bandHigh" }, 2);

    if (auto* to = dynamic_cast<juce::SettableTooltipClient*> (edges.findControl ("bandHigh")))
        to->setTooltip (to->getTooltip() + ". It plays an octave above From at least, whatever it is set to");
    auto& part = keep (std::make_unique<BandPart> (view, edges));
    bandPart = &part;
    constexpr int pictureHeight = 90, pictureLeast = 48;
    DeviceLayout::Part band { "Band",
                              &part,
                              part.getPreferredWidth(),
                              pictureHeight + Space::xs + part.getControlsHeight(),
                              pictureLeast + Space::xs + part.getControlsHeight(),
                              true };
    band.titleSwitch = &makePartSwitch ("band", part);

    const auto quality = makeControlsPart ("Quality", { "quality" }, 1);

    setParts ({ { { "Saturation", &shown, displayWidth, 140, 80, true }, { "Shape", &buttons, displayWidth, 48, 48 } },
                { driveControl, colour },
                { band, quality } });
}

SaturatorEditor::~SaturatorEditor() = default;

juce::Component& SaturatorEditor::getCurve() const noexcept
{
    return *display;
}

juce::Button& SaturatorEditor::getShapeButton (int shape) const
{
    return shapes->getButton (shape);
}

float SaturatorEditor::getShownReach() const
{
    return display->getReach();
}

juce::Component& SaturatorEditor::getBandView() const noexcept
{
    return *bandView;
}

float SaturatorEditor::getEdgeX (bool high) const
{
    return bandView->getEdgeX (high);
}

juce::Button& SaturatorEditor::getListenButton() const noexcept
{
    return bandPart->listen;
}

// Listen: the band alone, for as long as the editor shows and asks.
void SaturatorEditor::tick()
{
    if (bandPart->listen.getToggleState())
        if (auto* saturator = device.getProcessor<Saturator>())
            saturator->listenToBand();
}
} // namespace phazite::ui
