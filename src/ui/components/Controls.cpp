// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Controls.h"
#include "ValueText.h"
#include "model/automation/AutomationModel.h"
#include "model/mixer/MixerModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr float fineDragFactor = 0.2f; // Shift-drag moves a fifth as far
    constexpr int knobDragDistance = 200;  // pixels from one end of a knob's range to the other
    constexpr int meterRefreshRateHz = 30;
    constexpr float meterFallDbPerTick = 1.0f; // 30 dB a second
    constexpr juce::uint32 peakHoldMs = 1000;

    juce::String getMuteTooltip (bool silencedBySolo)
    {
        return silencedBySolo ? "Silent while other tracks are soloed. Click to mute it anyway, or right-click to solo it alone"
                              : "Mute: silences this track. Right-click to solo it alone, as in FL Studio";
    }
} // namespace

//==============================================================================
ValueSlider::ValueSlider (SliderStyle style, const juce::String& title, double defaultValue)
    : juce::Slider (style, juce::Slider::NoTextBox)
{
    setTitle (title);
    setName (title);
    setWantsKeyboardFocus (true);
    setPopupDisplayEnabled (true, true, nullptr);
    setDoubleClickReturnValue (true, defaultValue);
    setMouseDragSensitivity (knobDragDistance);

    // Ctrl, Alt and Cmd don't switch to JUCE's velocity-sensitive dragging, which surprises people.
    setVelocityModeParameters (1.0, 1, 0.0, false);
}

void ValueSlider::mouseDown (const juce::MouseEvent& e)
{
    showedMenu = e.mods.isPopupMenu() && onContextMenu != nullptr;

    if (showedMenu)
    {
        onContextMenu();
        return;
    }

    fineDrag = e.mods.isShiftDown();
    juce::Slider::mouseDown (e);
}

void ValueSlider::mouseUp (const juce::MouseEvent& e)
{
    if (! showedMenu)
        juce::Slider::mouseUp (e);
}

void ValueSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (showedMenu)
        return;

    if (fineDrag)
        juce::Slider::mouseDrag (e.withNewPosition (e.mouseDownPosition + (e.position - e.mouseDownPosition) * fineDragFactor));
    else
        juce::Slider::mouseDrag (e);
}

void ValueSlider::resized()
{
    juce::Slider::resized();

    // A fader's thumb follows the mouse: a drag over the fader's length covers its whole range.
    if (getSliderStyle() == LinearVertical || getSliderStyle() == LinearHorizontal)
    {
        const auto length = isVertical() ? getHeight() : getWidth();
        setMouseDragSensitivity (juce::jmax (1, length - 2 * getLookAndFeel().getSliderThumbRadius (*this)));
    }
}

//==============================================================================
Knob::Knob (const juce::String& title, double defaultValue)
    : ValueSlider (RotaryHorizontalVerticalDrag, title, defaultValue)
{
}

Fader::Fader (Direction direction, const juce::String& title, double defaultValue)
    : ValueSlider (direction == Direction::vertical ? LinearVertical : LinearHorizontal, title, defaultValue)
{
    setSliderSnapsToMousePosition (false);
}

//==============================================================================
namespace
{
    // What screen readers read and set for a number field: its value with its units, stepped as the arrow keys step it
    // (JUCE's keys take the step from here).
    class NumberFieldAccessibility : public juce::AccessibilityHandler
    {
    public:
        explicit NumberFieldAccessibility (NumberField& field)
            : juce::AccessibilityHandler (field, juce::AccessibilityRole::slider, juce::AccessibilityActions {},
                                          juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (field) })
        {
        }

    private:
        struct Value : public juce::AccessibilityValueInterface
        {
            explicit Value (NumberField& f)
                : field (f)
            {
            }

            bool isReadOnly() const override { return ! field.isEnabled(); }
            double getCurrentValue() const override { return field.getValue(); }
            juce::String getCurrentValueAsString() const override { return field.getTextFromValue (field.getValue()); }
            void setValue (double newValue) override { field.setValue (newValue, juce::sendNotificationSync); }
            void setValueAsString (const juce::String& text) override { setValue (field.getValueFromText (text)); }
            AccessibleValueRange getRange() const override { return { { field.getMinimum(), field.getMaximum() }, field.getStep() }; }

            NumberField& field;
        };
    };
} // namespace

NumberField::NumberField (const juce::String& title, double minimum, double maximum, double stepToUse, double defaultValue)
    : ValueSlider (LinearBarVertical, title, defaultValue),
      step (stepToUse)
{
    // The text box covers a bar slider, and a click on it without a drag types in it.
    setTextBoxStyle (TextBoxLeft, false, 0, 0);
    setRange (minimum, maximum, step / 100.0);
    setValue (defaultValue, juce::dontSendNotification);
    setSliderSnapsToMousePosition (false);
    setMouseDragSensitivity (juce::roundToInt ((maximum - minimum) / step) * pixelsPerStep);
    setDoubleClickReturnValue (false, defaultValue); // a click types instead
    setPopupDisplayEnabled (false, false, nullptr);  // the field shows its value itself
}

double NumberField::snapValue (double attemptedValue, DragMode dragMode)
{
    if (dragMode == notDragging)
        return attemptedValue; // typed, or stepped by a key or the wheel

    const auto steps = std::round ((attemptedValue - getMinimum()) / step);
    return juce::jlimit (getMinimum(), getMaximum(), getMinimum() + steps * step);
}

void NumberField::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isEnabled() || e.mods.isAnyMouseButtonDown())
        return;

    // A step a notch, from a mouse wheel's notches or a trackpad's smaller movements added up.
    const auto amount = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    wheelNotches += (wheel.isReversed ? -amount : amount) / wheelNotch;
    const auto steps = (int) wheelNotches;
    wheelNotches -= steps;

    if (steps != 0)
        setValue (juce::jlimit (getMinimum(), getMaximum(), getValue() + steps * step), juce::sendNotificationSync);
}

bool NumberField::keyPressed (const juce::KeyPress& key)
{
    const auto steps = (key == juce::KeyPress::upKey || key == juce::KeyPress::rightKey)    ? 1
                       : (key == juce::KeyPress::downKey || key == juce::KeyPress::leftKey) ? -1
                                                                                            : 0;

    if (steps == 0 || key.getModifiers().isAnyModifierKeyDown())
        return juce::Slider::keyPressed (key);

    setValue (juce::jlimit (getMinimum(), getMaximum(), getValue() + steps * step), juce::sendNotificationSync);
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> NumberField::createAccessibilityHandler()
{
    return std::make_unique<NumberFieldAccessibility> (*this);
}

void showAsDecibels (juce::Slider& slider)
{
    slider.textFromValueFunction = [] (double position) { return formatDecibels (faderPositionToDecibels ((float) position)); };
    slider.valueFromTextFunction = [&slider] (const juce::String& text)
    {
        if (const auto dB = parseDecibels (text))
            return (double) decibelsToFaderPosition ((float) *dB);

        return slider.getValue();
    };
}

void showAsPan (juce::Slider& slider)
{
    slider.textFromValueFunction = [] (double pan) { return formatPan (pan); };
    slider.valueFromTextFunction = [&slider] (const juce::String& text) { return parsePan (text).value_or (slider.getValue()); };
}

void showAsParameter (juce::Slider& slider, const devices::ParameterInfo& parameter)
{
    slider.textFromValueFunction = [parameter] (double value) { return formatParameterValue (parameter, (float) value); };
    slider.valueFromTextFunction = [parameter, &slider] (const juce::String& text)
    { return (double) parseParameterValue (parameter, text).value_or ((float) slider.getValue()); };
}

void showAsLoudness (juce::Slider& slider)
{
    slider.textFromValueFunction = [] (double lufs) { return formatLoudness (lufs); };
    slider.valueFromTextFunction = [&slider] (const juce::String& text) { return parseLoudness (text).value_or (slider.getValue()); };
    slider.updateText();
}

void showAsTruePeak (juce::Slider& slider)
{
    slider.textFromValueFunction = [] (double dBTP) { return formatTruePeak (dBTP); };
    slider.valueFromTextFunction = [&slider] (const juce::String& text) { return parseTruePeak (text).value_or (slider.getValue()); };
    slider.updateText();
}

void setPrimary (juce::TextButton& button, bool primary)
{
    const auto& p = getPalette (button);
    button.setColour (juce::TextButton::buttonColourId, primary ? p.accent : p.surfaceRaised);
    button.setColour (juce::TextButton::textColourOffId, primary ? p.textOnAccent : p.text);
}

//==============================================================================
ParameterAttachment::ParameterAttachment (juce::Slider& s)
    : slider (s)
{
    slider.setEnabled (false);
    slider.onDragStart = [this] { handle.beginGesture(); };
    slider.onDragEnd = [this] { handle.endGesture(); };

    // During a drag it moves the gesture's value; a key press or a double-click is a step of its own.
    slider.onValueChange = [this] { handle.setValue ((float) slider.getValue()); };

    // A right-click offers to automate the parameter (docs/design.md, section 10).
    if (auto* valueSlider = dynamic_cast<ValueSlider*> (&slider))
        valueSlider->onContextMenu = [this] { showMenu(); };
}

void ParameterAttachment::showMenu()
{
    if (! handle.isValid())
        return;

    // The slider and this go together; the menu's choices come later, when either may be gone.
    const juce::Component::SafePointer<juce::Slider> safeSlider (&slider);
    juce::PopupMenu menu;
    menu.addSectionHeader (handle.getName());
    menu.addItem ("Automate",
                  [this, safeSlider]
                  {
                      if (safeSlider != nullptr)
                          showAutomationLane (handle);
                  });
    menu.addItem ("Create Automation Clip",
                  [this, safeSlider]
                  {
                      if (safeSlider != nullptr)
                          createAutomationClip (handle);
                  });
    menu.addItem ("Clear Automation", isAutomated (handle), false,
                  [this, safeSlider]
                  {
                      if (safeSlider != nullptr)
                          clearAutomation (handle);
                  });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slider));
}

void ParameterAttachment::setParameter (ParameterHandle newHandle)
{
    if (handle.refersToSameParameter (newHandle))
    {
        update();
        return;
    }

    // Without a parameter while the range changes, so that a value the slider clamps reaches none.
    handle = ParameterHandle();
    showRange (newHandle);
    handle = std::move (newHandle);
    update();
}

void ParameterAttachment::showDeviceText()
{
    slider.textFromValueFunction = [this] (double value) { return handle.getText ((float) value); };
    slider.updateText();
}

void ParameterAttachment::showRange (const ParameterHandle& shown)
{
    if (! shown.isValid())
        return;

    const auto range = shown.getRange();
    slider.setNormalisableRange ({ range.start, range.end, range.interval, range.skew });

    if (const auto defaultValue = shown.getDefaultValue())
        slider.setDoubleClickReturnValue (true, *defaultValue);
}

void ParameterAttachment::update()
{
    slider.setEnabled (handle.isValid());

    if (handle.isValid() && ! handle.isInGesture())
        slider.setValue (handle.getValue(), juce::dontSendNotification);
}

//==============================================================================
ChoiceAttachment::ChoiceAttachment (juce::ComboBox& b)
    : box (b)
{
    box.setEnabled (false);
    box.onChange = [this]
    {
        if (handle.isValid() && box.getSelectedItemIndex() >= 0)
            handle.setValue (minimum + (float) box.getSelectedItemIndex());
    };
}

void ChoiceAttachment::setParameter (ParameterHandle newHandle)
{
    handle = ParameterHandle(); // without a parameter while the items change
    box.clear (juce::dontSendNotification);
    minimum = 0.0f;

    if (const auto info = newHandle.getInfo())
    {
        minimum = info->minimum;

        for (size_t i = 0; i < info->choices.size(); ++i)
            box.addItem (juce::String (info->choices[i].data(), info->choices[i].size()), (int) i + 1);
    }

    handle = std::move (newHandle);
    update();
}

void ChoiceAttachment::update()
{
    box.setEnabled (handle.isValid());

    if (handle.isValid())
        box.setSelectedItemIndex (juce::roundToInt (handle.getValue() - minimum), juce::dontSendNotification);
}

//==============================================================================
ToggleAttachment::ToggleAttachment (juce::Button& b)
    : button (b)
{
    button.setEnabled (false);
    button.setClickingTogglesState (true);
    button.onClick = [this]
    {
        if (handle.isValid())
            handle.setValue (button.getToggleState() ? 1.0f : 0.0f);
    };
}

void ToggleAttachment::setParameter (ParameterHandle newHandle)
{
    handle = std::move (newHandle);
    update();
}

void ToggleAttachment::update()
{
    button.setEnabled (handle.isValid());

    if (handle.isValid())
        button.setToggleState (handle.getValue() > 0.5f, juce::dontSendNotification);
}

//==============================================================================
LevelMeter::LevelMeter (const juce::String& title)
{
    setTitle (title);
    setDescription ("The peak level, from -60 to +6 dB. Red means above 0 dB, where the sound clips.");
    levels.fill ((float) silenceDb);
    peaks.fill ((float) silenceDb);
    startTimerHz (meterRefreshRateHz);
}

LevelMeter::~LevelMeter() = default;

void LevelMeter::setSource (LevelSource newSource)
{
    source = std::move (newSource);
}

void LevelMeter::timerCallback()
{
    source.refresh();
    const bool hasSource = source.isMeasuring();
    const bool mono = hasSource && source.getNumChannels() == 1;
    const auto now = juce::Time::getMillisecondCounter();
    bool changed = false;

    for (size_t ch = 0; ch < (size_t) numChannels; ++ch)
    {
        // Rises at once, falls steadily; a mono source shows on both bars.
        const auto measured = hasSource ? source.getAndClearLevel (mono ? 0 : (int) ch) : (float) silenceDb;
        const auto level = juce::jmax (measured, levels[ch] - meterFallDbPerTick, (float) silenceDb);

        if (level >= peaks[ch] || now - peakTimes[ch] > peakHoldMs)
        {
            peaks[ch] = level;
            peakTimes[ch] = now;
            changed = true;
        }

        // Only changes that move what is drawn count (levels below the meter's range all look the same).
        if (std::abs (juce::jmax (level, minDb) - juce::jmax (levels[ch], minDb)) > 0.1f)
            changed = true;

        levels[ch] = level;
    }

    if (changed)
        repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    auto area = getLocalBounds().toFloat();

    g.setColour (p.background);
    g.fillRoundedRectangle (area, cornerRadius);

    const bool vertical = getHeight() > getWidth();
    area.reduce (1.0f, 1.0f);
    const auto gap = 1.0f;
    const auto barThickness = ((vertical ? area.getWidth() : area.getHeight()) - gap) / numChannels;

    const auto toProportion = [] (float dB) { return juce::jlimit (0.0f, 1.0f, (dB - minDb) / (maxDb - minDb)); };
    const auto zeroDb = toProportion (0.0f);

    for (size_t ch = 0; ch < (size_t) numChannels; ++ch)
    {
        auto bar = vertical ? area.withWidth (barThickness).withX (area.getX() + (float) ch * (barThickness + gap))
                            : area.withHeight (barThickness).withY (area.getY() + (float) ch * (barThickness + gap));

        const auto length = vertical ? bar.getHeight() : bar.getWidth();
        const auto level = toProportion (levels[ch]);

        // The bar up to the level: the accent, then the danger color above 0 dB.
        const auto drawSegment = [&] (float from, float to, juce::Colour colour)
        {
            if (to <= from)
                return;

            g.setColour (colour);
            g.fillRect (vertical ? juce::Rectangle<float> (bar.getX(), bar.getBottom() - to * length, bar.getWidth(), (to - from) * length)
                                 : juce::Rectangle<float> (bar.getX() + from * length, bar.getY(), (to - from) * length, bar.getHeight()));
        };

        drawSegment (0.0f, juce::jmin (level, zeroDb), p.accent);
        drawSegment (zeroDb, level, p.danger);

        // The recent peak, as a line.
        if (peaks[ch] > minDb)
        {
            const auto peak = toProportion (peaks[ch]);
            g.setColour (peaks[ch] > 0.0f ? p.danger : p.text);

            if (vertical)
                g.fillRect (bar.getX(), bar.getBottom() - peak * length - 1.0f, bar.getWidth(), 2.0f);
            else
                g.fillRect (bar.getX() + peak * length - 1.0f, bar.getY(), 2.0f, bar.getHeight());
        }
    }
}

juce::String LevelMeter::getLevelText() const
{
    return formatDecibels (juce::jmax (peaks[0], peaks[1]));
}

std::unique_ptr<juce::AccessibilityHandler> LevelMeter::createAccessibilityHandler()
{
    // Screen readers read the meter as a value that can't be changed: its recent peak.
    class Value final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit Value (const LevelMeter& m)
            : meter (m)
        {
        }

        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return meter.getLevelText(); }
        void setValueAsString (const juce::String&) override {}

    private:
        const LevelMeter& meter;
    };

    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::progressBar, juce::AccessibilityActions(),
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

//==============================================================================
ValueMeter::ValueMeter (const juce::String& title, float lowestValue, float highestValue, Fill direction)
    : lowest (lowestValue),
      highest (highestValue),
      fill (direction),
      value (lowestValue)
{
    setTitle (title);
}

void ValueMeter::update (float reached)
{
    const auto shown = juce::jlimit (lowest, highest, std::max (reached, value - fallPerUpdate));

    if (! juce::exactlyEqual (shown, value))
    {
        value = shown;
        repaint();
    }
}

void ValueMeter::setMark (std::optional<float> newMark)
{
    const auto same = newMark.has_value() == mark.has_value() && (! mark.has_value() || juce::exactlyEqual (*newMark, *mark));

    if (! same)
    {
        mark = newMark;
        repaint();
    }
}

void ValueMeter::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto area = getLocalBounds().toFloat();
    g.setColour (p.background);
    g.fillRoundedRectangle (area, cornerRadius);

    const auto proportion = (value - lowest) / (highest - lowest);
    const auto length = area.getHeight() * proportion;

    if (fill == Fill::fromBottom)
    {
        g.setColour (p.accent);
        g.fillRect (area.withTop (area.getBottom() - length));
    }
    else
    {
        g.setColour (p.warning);
        g.fillRect (area.withHeight (length));
    }

    if (mark.has_value())
    {
        const auto markProportion = juce::jlimit (0.0f, 1.0f, (*mark - lowest) / (highest - lowest));
        const auto y = fill == Fill::fromBottom ? area.getBottom() - area.getHeight() * markProportion
                                                : area.getY() + area.getHeight() * markProportion;
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.fillRect (area.withY (y - GraphTint::curveWidth / 2.0f).withHeight (GraphTint::curveWidth));
    }
}

juce::String ValueMeter::getValueText() const
{
    return formatDecibels (fill == Fill::fromTop ? -value : value);
}

std::unique_ptr<juce::AccessibilityHandler> ValueMeter::createAccessibilityHandler()
{
    // Screen readers read the meter as a value that can't be changed.
    class Value final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit Value (const ValueMeter& m)
            : meter (m)
        {
        }

        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override { return meter.getValueText(); }
        void setValueAsString (const juce::String&) override {}

    private:
        const ValueMeter& meter;
    };

    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::progressBar, juce::AccessibilityActions(),
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

//==============================================================================
// A button that shows the track's state rather than toggling itself, and tells a right-click (which JUCE's
// buttons report as clicks too) from a click by the keys it passes on. Half lit is a state between off and on.
struct MuteSoloButtons::Button : public juce::TextButton
{
    Button (const juce::String& buttonText, const juce::String& title)
        : juce::TextButton (buttonText)
    {
        setTitle (title);
    }

    using juce::TextButton::clicked;

    void clicked (const juce::ModifierKeys& mods) override
    {
        if (onClickWith != nullptr)
            onClickWith (mods);
    }

    void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
    {
        auto& lf = getLookAndFeel();
        const auto off = findColour (buttonColourId), on = findColour (buttonOnColourId);
        lf.drawButtonBackground (g, *this, getToggleState() ? on : (halfLit ? off.interpolatedWith (on, halfLitAmount) : off),
                                 isHighlighted, isDown);
        lf.drawButtonText (g, *this, isHighlighted, isDown);
    }

    static constexpr float halfLitAmount = 0.4f;
    std::function<void (const juce::ModifierKeys&)> onClickWith;
    bool halfLit = false;
};

MuteSoloButtons::MuteSoloButtons (ProjectContext& projectContext)
    : context (projectContext),
      mute (std::make_unique<Button> ("M", "Mute")),
      solo (std::make_unique<Button> ("S", "Solo"))
{
    mute->setTooltip (getMuteTooltip (false));
    solo->setTooltip ("Solo: plays only the soloed tracks, and leaves the mute switches as they are. Ctrl+click solos this track alone");

    mute->onClickWith = [this] (const juce::ModifierKeys& mods)
    {
        if (mods.isPopupMenu())
            toggleSoloAlone (context, track);
        else
            toggleMute (context, track);

        update();
    };

    solo->onClickWith = [this] (const juce::ModifierKeys& mods)
    {
        if (mods.isCommandDown())
            toggleSoloAlone (context, track);
        else
            toggleSolo (context, track);

        update();
    };

    addAndMakeVisible (*mute);
    addAndMakeVisible (*solo);
    update();
}

MuteSoloButtons::~MuteSoloButtons() = default;

void MuteSoloButtons::setTrack (TrackID newTrack)
{
    track = newTrack;
    update();
}

void MuteSoloButtons::update()
{
    const auto state = getMuteSolo (context, track);
    const auto silenced = state.has_value() && state->silencedBySolo;

    mute->setEnabled (state.has_value());
    solo->setEnabled (state.has_value());
    mute->setToggleState (state.has_value() && state->muted, juce::dontSendNotification);
    solo->setToggleState (state.has_value() && state->soloed, juce::dontSendNotification);

    if (mute->halfLit != silenced)
    {
        mute->halfLit = silenced;
        mute->setTooltip (getMuteTooltip (silenced));
        mute->repaint();
    }
}

void MuteSoloButtons::resized()
{
    // Two buttons, a small gap between them.
    auto area = getLocalBounds();
    const auto width = (area.getWidth() - Space::xs / 2) / 2;
    mute->setBounds (area.removeFromLeft (width));
    solo->setBounds (area.removeFromRight (width));
}
} // namespace phazite::ui
