// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AgeEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "GraphDot.h"
#include "devices/age/Age.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Age;

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
    constexpr float hubSize = 0.3f, spokes = 3.0f, flange = 0.92f, spokeReach = 0.8f; // as parts of a reel's radius; its spokes
    constexpr float reelsWide = 0.25f, reelsTall = 0.36f; // a reel's radius, at most, as parts of the picture's width and height
    constexpr float headBelow = 1.6f;                     // how far under a reel's middle the head is, as a part of its radius
    constexpr float headWidth = 2.0f * GraphTint::handleRadius, headHeight = (float) Space::xs + GraphTint::curveWidth; // its head
    constexpr float brokenFall = 0.8f;                        // of a dropout's break in the tape's line, kept per refresh
    constexpr float gapWidth = 2.0f * GraphTint::dotRadius;   // the break's width, at its widest
    constexpr int bandSwitchSize = DeviceLayout::titleHeight; // the Band's switch, as a part's title's is

    // What a press takes, besides the band's ends (by their parameters): the wow, or nothing.
    constexpr int wowTarget = -1, nothing = -2;

    const juce::String recordAbout ("The record turning at its speed, as far off its centre as its wow swings it, with a speck where each "
                                    "crackle comes; beside it, the pitch it played over the last three seconds; under them, the band "
                                    "the player keeps. Drag up or down for the wow and turn the wheel for its rate; drag the band's "
                                    "ends for its low and high cut.");
    const juce::String tapeAbout ("The tape's two reels turning at the wow's rate, the tape running between them over the head, its line "
                                  "breaking where a dropout comes; beside them, the pitch it played over the last three seconds; under "
                                  "them, the band the machine keeps. Drag up or down for the wow and turn the wheel for its rate; drag "
                                  "the band's ends for its low and high cut.");

    // Each medium's page: what the tab says it is.
    const std::vector<juce::String> mediumTips {
        "Record: a turntable's warp and wow, the groove's distortion of the loud highs, and its surface's hiss and crackle",
        "Tape: a tape machine's drift and flutter, its saturation and head bump, and its hiss and dropouts",
    };

    // What the hint bar says of what a press takes.
    juce::String describe (int target, bool tape)
    {
        if (target == wowTarget)
            return tape ? "Drag up or down for the wow: how far the tape's pitch drifts. The wheel turns how fast it drifts."
                        : "Drag up or down for the wow: how far the record's pitch swings. The wheel turns how fast it swings.";

        if (target == Age::lowCut)
            return "Drag sideways to move the band's low end, the low cut: nothing below it is kept.";

        if (target == Age::highCut)
            return "Drag sideways to move the band's high end, the high cut: nothing above it is kept.";

        return tape ? tapeAbout : recordAbout;
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Age::info.parameters[(size_t) index];
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
        return formatParameterValue (getInfo (Age::wow), cents);
    }

    juce::Rectangle<float> circle (juce::Point<float> at, float radius)
    {
        return juce::Rectangle<float> (2.0f * radius, 2.0f * radius).withCentre (at);
    }

    // The band's knobs, which the Band's switch shows faint while it is off, over the strip they sit in: a press between them
    // goes to the picture under it.
    struct BandControls : public juce::Component
    {
        BandControls() { setInterceptsMouseClicks (false, true); }
    };
} // namespace

//==============================================================================
class AgeEditor::Picture : public juce::Component,
                           private DisplayTicker::Display
{
public:
    explicit Picture (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          wow (device.getParameter (getInfo (Age::wow).id)),
          rate (device.getParameter (getInfo (Age::wowRate).id)),
          history (historyLength, 0.0f),
          pulled (512)
    {
        for (const auto& parameter : Age::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        addAndMakeVisible (bandControls);

        for (auto [knob, attachment, index] : { std::tuple { &lowCutKnob, &lowCutAttachment, (int) Age::lowCut },
                                                std::tuple { &highCutKnob, &highCutAttachment, (int) Age::highCut } })
        {
            *knob = makeKnob (index);
            *attachment = std::make_unique<ParameterAttachment> (**knob);
            (*attachment)->setParameter (device.getParameter (getInfo (index).id));
            (*knob)->setTooltip (describeParameter (getInfo (index), device.getParameter (getInfo (index).id)));
            bandControls.addAndMakeVisible (**knob);
        }

        readValues();
        showMedium();

        drag.onCancelled = [this]
        {
            readValues();
            repaint();
        };

        // What came before the editor opened isn't shown.
        if (auto* age = device.getProcessor<Age>())
        {
            age->getSurfaceNoise().takeClicks();
            age->getSurfaceNoise().getLargest().take();
            age->getTapeWear().takeDropouts();
        }
    }

    ~Picture() override
    {
        if (auto* age = device.getProcessor<Age>())
            age->getPitchTap().setListening (false);
    }

    Knob& getLowCutKnob() const { return *lowCutKnob; }
    Knob& getHighCutKnob() const { return *highCutKnob; }
    juce::Component& getBandControls() noexcept { return bandControls; }

    // The Band's switch, which the editor makes: placed in the band's strip, beside the low cut's knob.
    void setBandSwitch (juce::Component& bandSwitch)
    {
        bandSwitchShown = &bandSwitch;
        addAndMakeVisible (bandSwitch);
        resized();
    }

    float getTurn() const noexcept { return shownTurn; }
    float getPitch() const noexcept { return shownPitch; }
    int getNumReadings() const noexcept { return readingsTaken; }
    int getNumSpecks() const noexcept { return (int) specks.size(); }
    bool isBroken() const noexcept { return broken > 0.0f; }
    bool showsTape() const noexcept { return values[Age::medium] > 0.5f; }

    juce::Rectangle<int> getBandArea() const
    {
        return getLocalBounds()
            .removeFromBottom (compactKnobSize)
            .withTrimmedLeft (compactKnobSize + bandSwitchSize + 2 * Space::xs)
            .withTrimmedRight (compactKnobSize + Space::xs);
    }

    juce::Rectangle<float> getUpperArea() const
    {
        return getLocalBounds().toFloat().withTrimmedBottom ((float) (compactKnobSize + Space::xs));
    }

    juce::Rectangle<float> getMediumArea() const
    {
        const auto upper = getUpperArea();
        return upper.withWidth (std::min (upper.getHeight() * (showsTape() ? 1.4f : 1.0f), upper.getWidth() * 0.5f));
    }

    juce::Rectangle<float> getPitchArea() const { return getUpperArea().withTrimmedLeft (getMediumArea().getWidth() + (float) Space::xs); }

    // The record's middle as it is drawn now, off the area's middle by its wow, and its radius.
    std::pair<juce::Point<float>, float> getDisc() const
    {
        const auto area = getMediumArea().reduced ((float) Space::xs);
        const auto radius = std::max (1.0f, std::min (area.getWidth(), area.getHeight()) * 0.5f);
        const auto off = radius * mostWobble * std::sqrt (juce::jlimit (0.0f, 1.0f, values[Age::wow] / getInfo (Age::wow).maximum));
        const auto angle = twoPi * shownTurn;
        return { area.getCentre() + juce::Point<float> (std::cos (angle), std::sin (angle)) * off, radius };
    }

    // The tape's two reels: their middles and their radius.
    struct Reels
    {
        juce::Point<float> left, right;
        float radius;
    };

    Reels getReels() const
    {
        const auto area = getMediumArea().reduced ((float) Space::xs);
        const auto radius = std::max (1.0f, std::min (area.getWidth() * reelsWide, area.getHeight() * reelsTall));
        const auto y = area.getY() + radius;
        return { { area.getX() + radius, y }, { area.getRight() - radius, y }, radius };
    }

    juce::Point<float> getCentre() const
    {
        if (! showsTape())
            return getDisc().first;

        const auto reels = getReels();
        return (reels.left + reels.right) * 0.5f;
    }

    void resized() override
    {
        // The band's strip: its switch, then the low cut's knob, the band, and the high cut's knob.
        auto strip = getLocalBounds().removeFromBottom (compactKnobSize);
        const auto switchArea = strip.removeFromLeft (bandSwitchSize);
        strip.removeFromLeft (Space::xs);
        bandControls.setBounds (strip);
        auto inStrip = bandControls.getLocalBounds();
        lowCutKnob->setBounds (inStrip.removeFromLeft (compactKnobSize));
        highCutKnob->setBounds (inStrip.removeFromRight (compactKnobSize));

        if (bandSwitchShown != nullptr)
            bandSwitchShown->setBounds (switchArea.withSizeKeepingCentre (bandSwitchSize, bandSwitchSize));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);

        if (showsTape())
            paintTape (g, p, tint);
        else
            paintRecord (g, p, tint);

        paintPitch (g, p, tint);
        paintBand (g, p, tint);
    }

    // On the record, the reels or the pitch, a drag up or down is the wow, as one undo step; on the band, it moves the nearer of
    // its ends.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        // Each from its value set by hand, the press changing nothing (graph::HandleDrag).
        const auto target = findTarget (e.position);
        draggingBand = target != wowTarget;

        if (draggingBand)
            draggedCut = target;

        drag.take ({ draggingBand ? &parameters[(size_t) draggedCut] : &wow });
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isTaken())
            return;

        if (draggingBand)
        {
            drag.moveAlong (
                0, (float) e.getDistanceFromDragStartX(), [this] (float hertz) { return getBandAxis().xFor (hertz); },
                [this] (float x) { return (float) getBandAxis().frequencyAt (x); });
        }
        else
        {
            const auto range = wow.getRange();
            const auto moved =
                -(float) e.getDistanceFromDragStartY() / std::max (1.0f, getUpperArea().getHeight()) * (range.end - range.start);
            drag.set (0, drag.getStart (0) + moved);
        }

        readValues();
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        drag.end();
        draggingBand = false;
        hover (getLocalBounds().toFloat().contains (e.position) ? findTarget (e.position) : nothing);
    }

    // Over the medium and its pitch, the pointer moves up and down and the medium lights up; over the band, it moves sideways
    // and the band's nearer end, which a press takes, lights up.
    void mouseMove (const juce::MouseEvent& e) override { hover (findTarget (e.position)); }
    void mouseExit (const juce::MouseEvent&) override { hover (nothing); }

    // The wheel turns the wow's rate: the record's speed, or the tape's drift's.
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& details) override
    {
        if (juce::approximatelyEqual (details.deltaY, 0.0f))
            return;

        const auto range = rate.getRange();
        const auto along = range.convertTo0to1 (rate.getBaseValue()) + (details.deltaY > 0.0f ? wheelStep : -wheelStep);
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
    std::vector<ParameterHandle> parameters; // every parameter, in Age's order
    std::vector<float> values;
    ParameterHandle wow, rate;
    BandControls bandControls;
    std::unique_ptr<Knob> lowCutKnob, highCutKnob;
    std::unique_ptr<ParameterAttachment> lowCutAttachment, highCutAttachment;
    juce::Component* bandSwitchShown = nullptr;
    graph::HandleDrag drag { *this };          // after the handles it moves
    graph::WheelGesture wheelGesture { drag }; // and turns
    graph::LevelHistory history;               // the pitch's readings, in cents, oldest first
    std::vector<float> pulled;
    std::vector<Speck> specks;
    int readingsTaken = 0, specksMade = 0;
    float shownTurn = 0.0f, shownPitch = 0.0f, lit = 0.0f, broken = 0.0f;
    bool draggingBand = false;
    int draggedCut = Age::lowCut;
    int hovered = nothing; // what a press under the pointer takes, lit: the wow or one of the band's ends
    DrawnNumbers drawn;

    void readValues()
    {
        for (size_t i = 0; i < parameters.size(); ++i)
            if (parameters[i].isValid())
                values[i] = parameters[i].getValue();
    }

    // Its title and its hint for the medium it draws.
    void showMedium()
    {
        setTitle (showsTape() ? "Tape" : "Record");
        setDescription (describe (hovered, showsTape()));
    }

    graph::FrequencyAxis getBandAxis() const { return { getBandArea().toFloat() }; }

    // In the band, its nearer end, by its parameter; elsewhere, the wow.
    int findTarget (juce::Point<float> position) const
    {
        if (! getBandArea().toFloat().contains (position))
            return wowTarget;

        const auto axis = getBandAxis();
        const auto lowX = axis.xFor (values[Age::lowCut]), highX = axis.xFor (values[Age::highCut]);
        return std::abs (position.x - lowX) <= std::abs (position.x - highX) ? Age::lowCut : Age::highCut;
    }

    void hover (int target)
    {
        setMouseCursor (target == wowTarget ? juce::MouseCursor::UpDownResizeCursor
                        : target == nothing ? juce::MouseCursor::NormalCursor
                                            : juce::MouseCursor::LeftRightResizeCursor);
        setDescription (describe (target, showsTape()));

        if (target != hovered)
        {
            hovered = target;
            repaint();
        }
    }

    // The pitch's scale, either way from the middle: a little more than the wow and the flutter reach, 5 cents at least.
    float getPitchScale() const
    {
        const auto reach = 1.25f * values[Age::wow] + 2.5f * values[Age::flutter];
        return std::max (5.0f, std::ceil (reach * 1.2f));
    }

    void tick() override
    {
        lowCutAttachment->update();
        highCutAttachment->update();
        const auto wasTape = showsTape();
        readValues();

        if (showsTape() != wasTape)
            showMedium();

        if (auto* age = device.getProcessor<Age>())
        {
            auto& tap = age->getPitchTap();
            tap.setListening (true);

            for (int n; (n = tap.pull (pulled.data(), (int) pulled.size())) > 0;)
            {
                for (int i = 0; i < n; ++i)
                    history.push (pulled[(size_t) i]);

                readingsTaken = std::min (historyLength, readingsTaken + n);
            }

            shownTurn = age->getTurn().get();
            shownPitch = age->getPitch().get();

            // A speck for each crackle that came, under the stylus, at most a few a refresh: the largest as large as it was.
            auto& noise = age->getSurfaceNoise();
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

            // The tape's line breaks as a dropout comes, as deep as it dips now, and closes again.
            auto& wear = age->getTapeWear();
            const auto dropouts = wear.takeDropouts();
            broken = std::max ({ dropouts > 0 ? 0.5f : 0.0f, wear.getDip().get(), broken * brokenFall });
            broken = broken < 0.02f ? 0.0f : broken;
        }

        for (auto& speck : specks)
            speck.age += 1.0 / DisplayTicker::ticksPerSecond;

        specks.erase (std::remove_if (specks.begin(), specks.end(), [] (const Speck& s) { return s.age >= speckSeconds; }), specks.end());

        if (specks.size() > (size_t) mostSpecks)
            specks.erase (specks.begin(), specks.begin() + (std::ptrdiff_t) (specks.size() - (size_t) mostSpecks));

        auto numbers = values;
        numbers.insert (numbers.end(), { shownTurn, shownPitch, lit, broken, (float) specks.size() });
        history.appendTo (numbers);

        for (const auto& speck : specks)
            numbers.insert (numbers.end(), { speck.angle, speck.reach, speck.size, (float) speck.age });

        if (drawn.changed (std::move (numbers)))
            repaint();
    }

    void paintRecord (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getMediumArea();
        graph::paintBackground (g, p, area);
        const auto [centre, radius] = getDisc();
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
        const auto still = getMediumArea().reduced ((float) Space::xs).getCentre();
        const auto stylus = still + juce::Point<float> (std::cos (stylusAngle), std::sin (stylusAngle)) * (stylusReach * radius);
        const juce::Point<float> pivot { area.getRight() - (float) Space::s, area.getY() + (float) Space::s };
        g.setColour (p.textMuted);
        g.drawLine ({ pivot, stylus }, GraphTint::curveWidth);
        g.setColour (lit > 0.0f ? tint.interpolatedWith (p.text, 0.5f * lit) : tint);
        g.fillEllipse (circle (stylus, GraphTint::handleRadius));

        graph::paintReadout (g, p, area, juce::String (juce::roundToInt (values[Age::wowRate] * 60.0f)) + " rpm",
                             juce::Justification::bottomLeft);
    }

    // A reel: its flange, its hub, and its spokes, turned as far as the tape has run.
    static void paintReel (juce::Graphics& g, const Palette& p, juce::Colour tint, juce::Point<float> centre, float radius, float angle)
    {
        g.setColour (p.surfaceRaised);
        g.fillEllipse (circle (centre, radius));
        g.setColour (p.text.withAlpha (GraphTint::grid));
        g.drawEllipse (circle (centre, radius * flange), grooveLine);
        g.setColour (tint.withAlpha (GraphTint::partCurve));
        g.fillEllipse (circle (centre, hubSize * radius));

        g.setColour (tint);

        for (int spoke = 0; spoke < (int) spokes; ++spoke)
        {
            const auto at = angle + twoPi * (float) spoke / spokes;
            const auto towards = juce::Point<float> (std::cos (at), std::sin (at));
            g.drawLine ({ centre + towards * (hubSize * radius), centre + towards * (spokeReach * radius) }, GraphTint::curveWidth);
        }

        g.setColour (p.background);
        g.fillEllipse (circle (centre, spindleRadius));
    }

    void paintTape (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getMediumArea();
        graph::paintBackground (g, p, area);
        const auto reels = getReels();
        const auto angle = twoPi * shownTurn; // both reels turn as the tape runs from the left one to the right one

        paintReel (g, p, tint, reels.left, reels.radius, angle);
        paintReel (g, p, tint, reels.right, reels.radius, angle);

        // The tape: from under the left reel down to the head, over it, and up to the right reel; broken at the head for a
        // moment as a dropout comes, the gap as wide as it dips.
        const auto inner = area.reduced ((float) Space::xs);
        const juce::Point<float> head { inner.getCentreX(),
                                        std::min (inner.getBottom() - headHeight, reels.left.y + reels.radius * headBelow) };
        const juce::Point<float> fromLeft { reels.left.x, reels.left.y + reels.radius },
            toRight { reels.right.x, reels.right.y + reels.radius };
        const auto gap = broken * gapWidth;
        g.setColour (tint);

        if (gap > 0.0f)
        {
            const auto beforeHead = head.translated (-0.5f * gap, 0.0f), afterHead = head.translated (0.5f * gap, 0.0f);
            g.drawLine ({ fromLeft, beforeHead }, GraphTint::curveWidth);
            g.drawLine ({ afterHead, toRight }, GraphTint::curveWidth);
        }
        else
        {
            g.drawLine ({ fromLeft, head }, GraphTint::curveWidth);
            g.drawLine ({ head, toRight }, GraphTint::curveWidth);
        }

        // The head, under the tape.
        g.setColour (p.textMuted);
        g.fillRect (juce::Rectangle<float> (headWidth, headHeight)
                        .withCentre (head.translated (0.0f, 0.5f * (headHeight + GraphTint::curveWidth))));

        // Ringed while a drag would change its wow.
        if (hovered == wowTarget)
        {
            paintLitRing (g, p, reels.left, reels.radius);
            paintLitRing (g, p, reels.right, reels.radius);
        }

        graph::paintReadout (g, p, area, formatParameterValue (getInfo (Age::wowRate), values[Age::wowRate]),
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

        // Faint while the Band is off, as a part that is off shows.
        const auto bandTint = values[Age::band] > 0.5f ? tint : tint.withMultipliedAlpha (SwitchTint::partOff);
        const auto lowX = axis.xFor (values[Age::lowCut]), highX = axis.xFor (values[Age::highCut]);
        const auto kept = area.withX (lowX).withRight (std::max (lowX + 1.0f, highX)).reduced (0.0f, 6.0f);
        g.setColour (bandTint.withMultipliedAlpha (GraphTint::partCurve));
        g.fillRoundedRectangle (kept, 3.0f);

        // Its ends, the one a press takes ringed.
        for (const auto cut : { (int) Age::lowCut, (int) Age::highCut })
        {
            const auto x = cut == Age::lowCut ? lowX : highX;
            const auto end = juce::Rectangle<float> (4.0f, area.getHeight() - 8.0f).withCentre ({ x, area.getCentreY() });
            g.setColour (bandTint);
            g.fillRoundedRectangle (end, 2.0f);

            if (cut == hovered)
                paintLitRing (g, p, end, 2.0f);
        }

        graph::paintReadout (g, p, area,
                             graph::formatFrequency (values[Age::lowCut]) + " to " + graph::formatFrequency (values[Age::highCut]),
                             juce::Justification::centred);
    }
};

//==============================================================================
AgeEditor::AgeEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Picture> (device));
    picture = &shown;
    addControl ("lowCut", shown.getLowCutKnob());
    addControl ("highCut", shown.getHighCutKnob());
    shown.setBandSwitch (makePartSwitch ("band", shown.getBandControls()));

    // A part with its switch at its title's right end.
    const auto switched = [this] (DeviceLayout::Part part, std::string_view switchID)
    {
        part.titleSwitch = &makePartSwitch (switchID, *part.content);
        return part;
    };

    // What the two media share: the picture's place, the Wobble and the Stereo, the same components in both.
    const auto wobblePart = switched (makeControlsPart ("Wobble", { "wow", "wowRate", "flutter" }, 3), "wobble");
    const auto stereoPart = switched (makeControlsPart ("Stereo", { "width", "bassMono" }, 2), "stereo");

    // The record: its crackle, and the groove's distortion.
    const DeviceLayout::Part record { "Record", &shown, 320, 196, 136, true, GraphTint::minimumWidth };
    const auto recordNoise = switched (makeControlsPart ("Noise", { "hiss", "crackle", "density" }, 3), "noise");
    const auto recordDrive = switched (makeControlsPart ("Drive", { "distortion" }, 1), "drive");

    // The tape: its dropouts, its saturation and its head bump.
    const DeviceLayout::Part tape { "Tape", &shown, 320, 196, 136, true, GraphTint::minimumWidth };
    const auto tapeNoise = switched (makeControlsPart ("Noise", { "hiss", "dropouts" }, 2), "noise");
    const auto tapeDrive = switched (makeControlsPart ("Drive", { "distortion", "headBump" }, 2), "drive");

    setModes (Age::info.parameters[Age::medium].id,
              { { { record }, { wobblePart, recordNoise }, { recordDrive, stereoPart } },
                { { tape }, { wobblePart, tapeNoise }, { tapeDrive, stereoPart } } },
              mediumTips);
}

AgeEditor::~AgeEditor() = default;

juce::Component& AgeEditor::getRecord() const noexcept
{
    return *picture;
}

juce::Point<float> AgeEditor::getRecordCentre() const
{
    return picture->getCentre();
}

juce::Rectangle<int> AgeEditor::getBandArea() const
{
    return picture->getBandArea();
}

float AgeEditor::getShownTurn() const
{
    return picture->getTurn();
}

float AgeEditor::getShownPitch() const
{
    return picture->getPitch();
}

int AgeEditor::getNumReadings() const
{
    return picture->getNumReadings();
}

int AgeEditor::getNumSpecks() const
{
    return picture->getNumSpecks();
}

bool AgeEditor::isTapeBroken() const
{
    return picture->isBroken();
}

bool AgeEditor::showsTape() const
{
    return picture->showsTape();
}
} // namespace phazite::ui
