// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LessonCard.h"
#include "ui/components/Controls.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    const auto headerHeight = juce::roundToInt (TextSize::title) + Space::xs;
    const auto progressHeight = juce::roundToInt (TextSize::small) + Space::xs;
    constexpr int kindWidth = 56;
    constexpr int buttonWidth = 80;

    juce::FontOptions getTextFont()
    {
        return juce::FontOptions (TextSize::title);
    }
} // namespace

//==============================================================================
LessonCard::Ring::Ring()
{
    setInterceptsMouseClicks (false, false);
    setAccessible (false);
}

juce::Rectangle<int> LessonCard::Ring::getPaintedArea() const
{
    if (! target.has_value())
        return {};

    return target->expanded ((int) std::ceil (LessonTint::ringGap + LessonTint::ringWidth + LessonTint::pulseSpread));
}

void LessonCard::Ring::show (std::optional<juce::Rectangle<int>> newTarget, float newPulse)
{
    if (newTarget == target && juce::approximatelyEqual (newPulse, pulse))
        return;

    repaint (getPaintedArea());
    target = newTarget;
    pulse = newPulse;
    repaint (getPaintedArea());
}

void LessonCard::Ring::paint (juce::Graphics& g)
{
    if (! target.has_value())
        return;

    const auto accent = getPalette (*this).accent;
    const auto area = target->toFloat().expanded (LessonTint::ringGap);

    // As a step starts, rings that spread out from the control and fade, a few times; then the ring alone, still.
    if (pulse < 1.0f)
    {
        const auto phase = std::fmod (pulse * (float) LessonTint::pulses, 1.0f);
        g.setColour (accent.withAlpha (1.0f - phase));
        g.drawRoundedRectangle (area.expanded (phase * LessonTint::pulseSpread), cornerRadius + phase * LessonTint::pulseSpread,
                                LessonTint::ringWidth);
    }

    g.setColour (accent);
    g.drawRoundedRectangle (area, cornerRadius, LessonTint::ringWidth);
}

//==============================================================================
LessonCard::LessonCard (Lessons& l, std::function<juce::Component*(const juce::String&)> find)
    : lessons (l),
      findTarget (std::move (find))
{
    setTitle ("Lesson");
    kind.setText ("Lesson", juce::dontSendNotification);
    kind.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    name.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    progress.setFont (juce::FontOptions (TextSize::small));
    text.setFont (getTextFont());
    text.setJustificationType (juce::Justification::topLeft);
    text.setMinimumHorizontalScale (1.0f);

    backButton.setTooltip ("Goes back to the step before");
    backButton.onClick = [this] { lessons.back(); };
    nextButton.onClick = [this] { lessons.next(); };
    closeButton.setTooltip ("Stops the lesson. Help > Lessons starts it again");
    closeButton.onClick = [this] { lessons.stop(); };

    for (auto* label : { &kind, &name, &progress, &text })
        label->setInterceptsMouseClicks (false, false);

    for (auto* c : std::initializer_list<juce::Component*> { &kind, &name, &progress, &text, &backButton, &nextButton, &closeButton })
        addAndMakeVisible (c);

    setVisible (false);
}

LessonCard::~LessonCard() = default;

void LessonCard::update()
{
    const auto* lesson = lessons.getLesson();
    setVisible (lesson != nullptr);
    ring.setVisible (lesson != nullptr);

    if (lesson == nullptr)
    {
        stopTimer();
        ring.show ({}, 1.0f);
        shownStep.clear();
        return;
    }

    const auto& p = getPalette (*this);
    const auto finished = lessons.isFinished();
    const auto waits = lessons.waitsForNext();
    kind.setColour (juce::Label::textColourId, p.focus); // text in the focus colour, which reads on the card (4.5:1)
    progress.setColour (juce::Label::textColourId, p.textMuted);
    name.setText (lesson->name, juce::dontSendNotification);
    progress.setText (finished ? juce::String ("Done")
                               : "Step " + juce::String (lessons.getStep() + 1) + " of " + juce::String ((int) lesson->steps.size()),
                      juce::dontSendNotification);
    text.setText (lessons.getText(), juce::dontSendNotification);
    setDescription (lessons.getHint()); // what screen readers say of the card
    backButton.setVisible (! finished && lessons.getStep() > 0);
    nextButton.setButtonText (finished ? "Done" : waits ? "Next" : "Skip");
    nextButton.setTooltip (finished ? "Ends the lesson"
                           : waits  ? "Goes on to the next step"
                                    : "Goes on to the next step, leaving this one as it is");
    setPrimary (nextButton, finished || waits);

    // A new step: the ring around its control pulses again.
    if (const auto step = juce::String (lesson->id) + "/" + juce::String (lessons.getStep()); step != shownStep)
    {
        shownStep = step;
        pulseStart = juce::Time::getMillisecondCounterHiRes();
    }

    if (auto* parent = getParentComponent())
        parent->resized(); // which places the card, as tall as its text

    resized();
    repaint();
    updateRing();
    startTimerHz (LessonTint::pulseFramesPerSecond);
}

void LessonCard::updateRing()
{
    std::optional<juce::Rectangle<int>> area;
    const auto target = lessons.getTarget();

    if (target.isNotEmpty() && findTarget != nullptr && ring.getParentComponent() != nullptr)
        if (auto* control = findTarget (target))
            area = ring.getLocalArea (control, control->getLocalBounds());

    const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - pulseStart) / 1000.0;
    ring.show (area, (float) juce::jmin (1.0, elapsed / LessonTint::pulseSeconds));
}

void LessonCard::timerCallback()
{
    // A step that the song shows done goes on (which calls update() again); and the ring follows its control.
    lessons.update();

    if (! isVisible())
        return;

    updateRing();

    // Quick while the ring pulses, then a few times a second: nothing repaints while nothing moves.
    const auto pulsing = (juce::Time::getMillisecondCounterHiRes() - pulseStart) / 1000.0 < LessonTint::pulseSeconds;
    const auto rate = pulsing ? LessonTint::pulseFramesPerSecond : LessonTint::checksPerSecond;

    if (getTimerInterval() != 1000 / rate)
        startTimerHz (rate);
}

int LessonCard::getHeightFor (int width) const
{
    // The text wrapped at the card's width, with half a line to spare, as a label breaks lines a little differently.
    const auto textWidth = width - 2 * (LessonTint::shadow + Space::m);
    juce::AttributedString attributed (lessons.getText());
    attributed.setFont (juce::Font (getTextFont()));
    attributed.setWordWrap (juce::AttributedString::byWord);
    juce::TextLayout layout;
    layout.createLayout (attributed, (float) juce::jmax (1, textWidth));
    const auto textHeight = (int) std::ceil (layout.getHeight() + TextSize::title / 2.0f);

    return 2 * (LessonTint::shadow + Space::m) + headerHeight + progressHeight + Space::s + textHeight + Space::s + controlHeight;
}

void LessonCard::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto card = getLocalBounds().reduced (LessonTint::shadow);

    // Raised over the window, in an accent outline: it isn't a part of what is under it.
    juce::DropShadow (juce::Colours::black.withAlpha (LessonTint::shadowAlpha), LessonTint::shadow, {}).drawForRectangle (g, card);
    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (card.toFloat(), LessonTint::corner);
    g.setColour (p.accent);
    g.drawRoundedRectangle (card.toFloat().reduced (LessonTint::outline / 2.0f), LessonTint::corner, LessonTint::outline);

    // How far the lesson has come: a dot for each step, filled up to this one.
    if (const auto* lesson = lessons.getLesson())
    {
        const auto steps = (int) lesson->steps.size();
        const auto row = progress.getBounds().toFloat();
        constexpr auto size = LessonTint::dot, gap = LessonTint::dot;

        for (int i = 0; i < steps; ++i)
        {
            const juce::Rectangle<float> dot (row.getRight() - (float) (steps - i) * (size + gap) + gap, row.getCentreY() - size / 2.0f,
                                              size, size);
            g.setColour (i <= lessons.getStep() ? p.accent : p.textMuted);

            if (i <= lessons.getStep())
                g.fillEllipse (dot);
            else
                g.drawEllipse (dot.reduced (0.5f), 1.0f);
        }
    }
}

void LessonCard::resized()
{
    auto area = getLocalBounds().reduced (LessonTint::shadow).reduced (Space::m);
    auto header = area.removeFromTop (headerHeight);
    closeButton.setBounds (header.removeFromRight (headerHeight));
    kind.setBounds (header.removeFromLeft (kindWidth));
    name.setBounds (header);
    progress.setBounds (area.removeFromTop (progressHeight));
    area.removeFromTop (Space::s);

    auto footer = area.removeFromBottom (controlHeight);
    nextButton.setBounds (footer.removeFromRight (buttonWidth));
    backButton.setBounds (footer.removeFromLeft (buttonWidth));
    area.removeFromBottom (Space::s);
    text.setBounds (area);
}
} // namespace phazite::ui
