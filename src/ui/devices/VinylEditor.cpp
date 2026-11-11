// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VinylEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "GraphDot.h"
#include "devices/vinyl/Vinyl.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Vinyl;

    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr int historyLength = 300;    // the pitch's readings shown: three seconds of them
    constexpr int mostSpecks = 48;        // crackles shown on the record at once
    constexpr double speckSeconds = 1.8;  // how long one shows, fading
    constexpr int mostSpecksATick = 6;    // of the crackles that came since the last refresh, at most so many specks
    constexpr float stylusAngle = -0.55f; // where the stylus rests on the record, in radians from the right, up being negative
    constexpr float stylusReach = 0.72f;  // and how far out, as a part of the record's radius
    constexpr float labelSize = 0.34f, innerGroove = 0.42f, outerGroove = 0.97f;   // as parts of the record's radius
    constexpr float grooveSpacing = 3.0f, grooveLine = 0.6f, spindleRadius = 2.0f; // in pixels
    constexpr float mostWobble = 0.08f; // how far off its centre the deepest wow draws the record, as a part of its radius
    constexpr float wheelStep = 0.04f;  // of the rate's travel a notch of the wheel turns it
    constexpr float speckSize = 2.2f;   // the largest crackle's speck's radius is 1.6 times this
    constexpr float litFall = 0.75f;    // of the stylus's light, kept per refresh

    // What a press takes, besides the band's ends (by their parameters): the wow, or nothing.
    constexpr int wowTarget = -1, nothing = -2;

    const juce::String about ("The record turning at its speed, as far off its centre as its wow swings it, with a speck where each "
                              "crackle comes; beside it, the pitch it played over the last three seconds; under them, the band the "
                              "player keeps. Drag up or down for the wow and turn the wheel for its rate; drag the band's ends for "
                              "its low and high cut.");

    // What the hint bar says of what a press takes.
    juce::String describe (int target)
    {
        if (target == wowTarget)
            return "Drag up or down for the wow: how far the record's pitch swings. The wheel turns how fast it swings.";

        if (target == Vinyl::lowCut)
            return "Drag sideways to move the band's low end, the low cut: the player keeps nothing below it.";

        if (target == Vinyl::highCut)
            return "Drag sideways to move the band's high end, the high cut: the player keeps nothing above it.";

        return about;
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Vinyl::info.parameters[(size_t) index];
    }

    std::unique_ptr<Knob> makeKnob (int index)
    {
        const auto& parameter = getInfo (index);
        auto knob = std::make_unique<Knob> (juce::String (parameter.name.data(), parameter.name.size()), parameter.defaultValue);
        showAsParameter (*knob, parameter);
        return knob;
    }

    // "+3.2 ct", "-12 ct", "0 ct".
    juce::String formatCents (float cents)
    {
        return formatParameterValue (getInfo (Vinyl::wow), cents);
    }
} // namespace

//==============================================================================
class VinylEditor::Record : public juce::Component,
                            private DisplayTicker::Display
{
public:
    explicit Record (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          wow (device.getParameter (getInfo (Vinyl::wow).id)),
          rate (device.getParameter (getInfo (Vinyl::wowRate).id)),
          history (historyLength, 0.0f),
          pulled (512)
    {
        setTitle ("Record");
        setDescription (about);

        for (const auto& parameter : Vinyl::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        for (auto [knob, attachment, index] : { std::tuple { &lowCutKnob, &lowCutAttachment, (int) Vinyl::lowCut },
                                                std::tuple { &highCutKnob, &highCutAttachment, (int) Vinyl::highCut } })
        {
            *knob = makeKnob (index);
            *attachment = std::make_unique<ParameterAttachment> (**knob);
            (*attachment)->setParameter (device.getParameter (getInfo (index).id));
            addAndMakeVisible (**knob);
        }

        readValues();

        // What came before the editor opened isn't shown.
        if (auto* vinyl = device.getProcessor<Vinyl>())
        {
            vinyl->getSurfaceNoise().takeClicks();
            vinyl->getSurfaceNoise().getLargest().take();
        }
    }

    ~Record() override
    {
        if (auto* vinyl = device.getProcessor<Vinyl>())
            vinyl->getPitchTap().setListening (false);
    }

    Knob& getLowCutKnob() const { return *lowCutKnob; }
    Knob& getHighCutKnob() const { return *highCutKnob; }

    float getTurn() const noexcept { return shownTurn; }
    float getPitch() const noexcept { return shownPitch; }
    int getNumReadings() const noexcept { return readingsTaken; }
    int getNumSpecks() const noexcept { return (int) specks.size(); }

    juce::Rectangle<int> getBandArea() const
    {
        return getLocalBounds()
            .removeFromBottom (compactKnobSize)
            .withTrimmedLeft (compactKnobSize + Space::xs)
            .withTrimmedRight (compactKnobSize + Space::xs);
    }

    juce::Rectangle<float> getUpperArea() const
    {
        return getLocalBounds().toFloat().withTrimmedBottom ((float) (compactKnobSize + Space::xs));
    }

    juce::Rectangle<float> getDiscArea() const
    {
        const auto upper = getUpperArea();
        return upper.withWidth (std::min (upper.getHeight(), upper.getWidth() * 0.5f));
    }

    juce::Rectangle<float> getPitchArea() const { return getUpperArea().withTrimmedLeft (getDiscArea().getWidth() + (float) Space::xs); }

    // The record's middle as it is drawn now, off the area's middle by its wow, and its radius.
    std::pair<juce::Point<float>, float> getDisc() const
    {
        const auto area = getDiscArea().reduced ((float) Space::xs);
        const auto radius = std::max (1.0f, std::min (area.getWidth(), area.getHeight()) * 0.5f);
        const auto off = radius * mostWobble * std::sqrt (juce::jlimit (0.0f, 1.0f, values[Vinyl::wow] / getInfo (Vinyl::wow).maximum));
        const auto angle = twoPi * shownTurn;
        return { area.getCentre() + juce::Point<float> (std::cos (angle), std::sin (angle)) * off, radius };
    }

    void resized() override
    {
        auto strip = getLocalBounds().removeFromBottom (compactKnobSize);
        lowCutKnob->setBounds (strip.removeFromLeft (compactKnobSize));
        highCutKnob->setBounds (strip.removeFromRight (compactKnobSize));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        paintRecord (g, p, tint);
        paintPitch (g, p, tint);
        paintBand (g, p, tint);
    }

    // On the record or its pitch, a drag up or down is the wow, as one undo step; on the band, it moves the nearer of its ends.
    void mouseDown (const juce::MouseEvent& e) override
    {
        wheelGesture.end(); // a step of its own, not the wheel's

        if (e.mods.isPopupMenu())
            return;

        const auto target = findTarget (e.position);
        draggingBand = target != wowTarget;

        if (draggingBand)
        {
            draggedCut = target;
            parameters[(size_t) draggedCut].beginGesture();
            mouseDrag (e);
            return;
        }

        wow.beginGesture();
        wowAtStart = wow.getValue();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingBand)
        {
            auto& cut = parameters[(size_t) draggedCut];

            if (cut.isInGesture())
            {
                const auto range = cut.getRange();
                cut.setValue (juce::jlimit (range.start, range.end, (float) getBandAxis().frequencyAt (e.position.x)));
            }
        }
        else if (wow.isInGesture())
        {
            const auto range = wow.getRange();
            const auto moved =
                -(float) e.getDistanceFromDragStartY() / std::max (1.0f, getUpperArea().getHeight()) * (range.end - range.start);
            wow.setValue (juce::jlimit (range.start, range.end, wowAtStart + moved));
        }

        readValues();
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (draggingBand && parameters[(size_t) draggedCut].isInGesture())
            parameters[(size_t) draggedCut].endGesture();

        if (wow.isInGesture())
            wow.endGesture();

        draggingBand = false;
        hover (getLocalBounds().toFloat().contains (e.position) ? findTarget (e.position) : nothing);
    }

    // Over the record and its pitch, the pointer moves up and down and the record lights up; over the band, it moves sideways
    // and the band's nearer end, which a press takes, lights up.
    void mouseMove (const juce::MouseEvent& e) override { hover (findTarget (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { hover (nothing); }

    // The wheel turns the record's speed: the wow's rate.
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& details) override
    {
        if (juce::approximatelyEqual (details.deltaY, 0.0f))
            return;

        const auto range = rate.getRange();
        const auto along = range.convertTo0to1 (rate.getValue()) + (details.deltaY > 0.0f ? wheelStep : -wheelStep);
        wheelGesture.turn (rate, range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, along)));
        readValues();
        repaint();
    }

private:
    struct Speck
    {
        float angle; // on the record, from where it turns from
        float reach; // as a part of its radius
        float size;  // 0 to 1
        double age;  // seconds
    };

    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the vinyl's order
    std::vector<float> values;
    ParameterHandle wow, rate;
    std::unique_ptr<Knob> lowCutKnob, highCutKnob;
    std::unique_ptr<ParameterAttachment> lowCutAttachment, highCutAttachment;
    graph::WheelGesture wheelGesture; // after the handles it turns
    graph::LevelHistory history;      // the pitch's readings, in cents, oldest first
    std::vector<float> pulled;
    std::vector<Speck> specks;
    int readingsTaken = 0, specksMade = 0;
    float shownTurn = 0.0f, shownPitch = 0.0f, lit = 0.0f, wowAtStart = 0.0f;
    bool draggingBand = false;
    int draggedCut = Vinyl::lowCut;
    int hovered = nothing; // what a press under the pointer takes, lit: the wow or one of the band's ends
    DrawnNumbers drawn;

    void readValues()
    {
        for (size_t i = 0; i < parameters.size(); ++i)
            if (parameters[i].isValid())
                values[i] = parameters[i].getValue();
    }

    graph::FrequencyAxis getBandAxis() const { return { getBandArea().toFloat() }; }

    // In the band, its nearer end, by its parameter; elsewhere, the wow.
    int findTarget (juce::Point<float> position) const
    {
        if (! getBandArea().toFloat().contains (position))
            return wowTarget;

        const auto axis = getBandAxis();
        const auto lowX = axis.xFor (values[Vinyl::lowCut]), highX = axis.xFor (values[Vinyl::highCut]);
        return std::abs (position.x - lowX) <= std::abs (position.x - highX) ? Vinyl::lowCut : Vinyl::highCut;
    }

    void hover (int target)
    {
        setMouseCursor (target == wowTarget ? juce::MouseCursor::UpDownResizeCursor
                        : target == nothing ? juce::MouseCursor::NormalCursor
                                            : juce::MouseCursor::LeftRightResizeCursor);
        setDescription (describe (target));

        if (target != hovered)
        {
            hovered = target;
            repaint();
        }
    }

    // The pitch's scale, either way from the middle: a little more than the wow and the flutter reach, 5 cents at least.
    float getPitchScale() const
    {
        const auto reach = 1.25f * values[Vinyl::wow] + 2.5f * values[Vinyl::flutter];
        return std::max (5.0f, std::ceil (reach * 1.2f));
    }

    void tick() override
    {
        lowCutAttachment->update();
        highCutAttachment->update();
        readValues();

        if (auto* vinyl = device.getProcessor<Vinyl>())
        {
            auto& tap = vinyl->getPitchTap();
            tap.setListening (true);

            for (int n; (n = tap.pull (pulled.data(), (int) pulled.size())) > 0;)
            {
                for (int i = 0; i < n; ++i)
                    history.push (pulled[(size_t) i]);

                readingsTaken = std::min (historyLength, readingsTaken + n);
            }

            shownTurn = vinyl->getTurn().get();
            shownPitch = vinyl->getPitch().get();

            // A speck for each crackle that came, under the stylus, at most a few a refresh: the largest as large as it was.
            auto& noise = vinyl->getSurfaceNoise();
            const auto clicks = noise.takeClicks();
            const auto largest = noise.getLargest().take();

            for (int i = 0; i < std::min (clicks, mostSpecksATick); ++i)
            {
                ++specksMade;
                const auto reach = stylusReach + 0.05f * std::sin ((float) specksMade * 2.3999632f);
                specks.push_back ({ stylusAngle - twoPi * shownTurn, reach, i == 0 ? largest : 0.3f * largest, 0.0 });
            }

            lit = clicks > 0 ? 1.0f : lit * litFall;
            lit = lit < 0.01f ? 0.0f : lit;
        }

        for (auto& speck : specks)
            speck.age += 1.0 / DisplayTicker::ticksPerSecond;

        specks.erase (std::remove_if (specks.begin(), specks.end(), [] (const Speck& s) { return s.age >= speckSeconds; }), specks.end());

        if (specks.size() > (size_t) mostSpecks)
            specks.erase (specks.begin(), specks.begin() + (std::ptrdiff_t) (specks.size() - (size_t) mostSpecks));

        auto numbers = values;
        numbers.insert (numbers.end(), { shownTurn, shownPitch, lit, (float) specks.size() });
        history.appendTo (numbers);

        for (const auto& speck : specks)
            numbers.insert (numbers.end(), { speck.angle, speck.reach, speck.size, (float) speck.age });

        if (drawn.changed (std::move (numbers)))
            repaint();
    }

    void paintRecord (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getDiscArea();
        graph::paintBackground (g, p, area);
        const auto [centre, radius] = getDisc();
        const auto circle = [] (juce::Point<float> at, float r) { return juce::Rectangle<float> (2.0f * r, 2.0f * r).withCentre (at); };
        const auto angle = twoPi * shownTurn; // clockwise, as a record turns seen from above

        // The record, its grooves, its label with a mark that turns with it, and the spindle's hole.
        g.setColour (p.surfaceRaised);
        g.fillEllipse (circle (centre, radius));
        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (auto r = innerGroove * radius; r < outerGroove * radius; r += grooveSpacing)
            g.drawEllipse (circle (centre, r), grooveLine);

        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillEllipse (circle (centre, labelSize * radius));
        g.setColour (tint);
        g.drawLine ({ centre, centre + juce::Point<float> (std::cos (angle), std::sin (angle)) * (labelSize * radius) },
                    GraphTint::curveWidth);
        g.setColour (p.background);
        g.fillEllipse (circle (centre, spindleRadius));

        // Ringed while a drag would change its wow.
        if (hovered == wowTarget)
            paintLitRing (g, p, centre, radius);

        // The crackles' specks, turning away with the record and fading.
        for (const auto& speck : specks)
        {
            const auto at =
                centre + juce::Point<float> (std::cos (angle + speck.angle), std::sin (angle + speck.angle)) * (speck.reach * radius);
            const auto fade = (float) (1.0 - speck.age / speckSeconds);
            g.setColour (p.text.withAlpha (juce::jlimit (0.0f, 1.0f, fade)));
            g.fillEllipse (circle (at, speckSize * (0.6f + speck.size)));
        }

        // The arm, from beyond the record's top right to the stylus, which stays where it is as the record swings under it; lit as
        // a crackle comes.
        const auto still = getDiscArea().reduced ((float) Space::xs).getCentre();
        const auto stylus = still + juce::Point<float> (std::cos (stylusAngle), std::sin (stylusAngle)) * (stylusReach * radius);
        const juce::Point<float> pivot { area.getRight() - (float) Space::s, area.getY() + (float) Space::s };
        g.setColour (p.textMuted);
        g.drawLine ({ pivot, stylus }, GraphTint::curveWidth);
        g.setColour (lit > 0.0f ? tint.interpolatedWith (p.text, 0.5f * lit) : tint);
        g.fillEllipse (circle (stylus, GraphTint::handleRadius));

        graph::paintReadout (g, p, area, juce::String (juce::roundToInt (values[Vinyl::wowRate] * 60.0f)) + " rpm",
                             juce::Justification::bottomLeft);
    }

    void paintPitch (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getPitchArea();

        if (area.getWidth() < 4.0f)
            return;

        graph::paintBackground (g, p, area);
        const auto inner = area.reduced ((float) Space::xs, (float) Space::s);
        const auto scale = getPitchScale();
        const auto yFor = [inner, scale] (float cents)
        { return inner.getCentreY() - juce::jlimit (-1.0f, 1.0f, cents / scale) * inner.getHeight() * 0.5f; };

        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (yFor (0.0f)), inner.getX(), inner.getRight());
        g.setColour (p.text.withAlpha (GraphTint::grid));

        for (const auto edge : { -scale, scale })
            g.drawHorizontalLine (juce::roundToInt (yFor (edge)), inner.getX(), inner.getRight());

        g.setColour (tint);
        g.strokePath (history.makeLine (inner, yFor), juce::PathStrokeType (GraphTint::curveWidth));

        graph::paintReadouts (g, p, area, "Pitch " + formatCents (shownPitch),
                              juce::String::fromUTF8 ("\xc2\xb1") + formatCents (scale).trimCharactersAtStart ("+"));
    }

    void paintBand (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getBandArea().toFloat();
        const auto axis = getBandAxis();
        graph::paintBackground (g, p, area);
        graph::paintFrequencyGrid (g, p, axis, false);

        const auto lowX = axis.xFor (values[Vinyl::lowCut]), highX = axis.xFor (values[Vinyl::highCut]);
        const auto kept = area.withX (lowX).withRight (std::max (lowX + 1.0f, highX)).reduced (0.0f, 6.0f);
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle (kept, 3.0f);

        // Its ends, the one a press takes ringed.
        for (const auto cut : { (int) Vinyl::lowCut, (int) Vinyl::highCut })
        {
            const auto x = cut == Vinyl::lowCut ? lowX : highX;
            const auto end = juce::Rectangle<float> (4.0f, area.getHeight() - 8.0f).withCentre ({ x, area.getCentreY() });
            g.setColour (tint);
            g.fillRoundedRectangle (end, 2.0f);

            if (cut == hovered)
                paintLitRing (g, p, end, 2.0f);
        }

        graph::paintReadout (g, p, area,
                             graph::formatFrequency (values[Vinyl::lowCut]) + " to " + graph::formatFrequency (values[Vinyl::highCut]),
                             juce::Justification::centred);
    }
};

//==============================================================================
VinylEditor::VinylEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Record> (device));
    record = &shown;
    addControl ("lowCut", shown.getLowCutKnob());
    addControl ("highCut", shown.getHighCutKnob());

    const auto wowPart = makeControlsPart ("Wow", { "wow", "wowRate", "flutter" }, 3);
    const auto surface = makeControlsPart ("Noise", { "hiss", "crackle", "density" }, 3);
    const auto groove = makeControlsPart ("Groove", { "distortion", "width", "bassMono" }, 3);
    setParts ({ { { "Record", &shown, 320, 196, 136, true, GraphTint::minimumWidth } }, { wowPart, surface }, { groove } });
}

VinylEditor::~VinylEditor() = default;

juce::Component& VinylEditor::getRecord() const noexcept
{
    return *record;
}

juce::Point<float> VinylEditor::getRecordCentre() const
{
    return record->getDisc().first;
}

juce::Rectangle<int> VinylEditor::getBandArea() const
{
    return record->getBandArea();
}

float VinylEditor::getShownTurn() const
{
    return record->getTurn();
}

float VinylEditor::getShownPitch() const
{
    return record->getPitch();
}

int VinylEditor::getNumReadings() const
{
    return record->getNumReadings();
}

int VinylEditor::getNumSpecks() const
{
    return record->getNumSpecks();
}
} // namespace phazite::ui
