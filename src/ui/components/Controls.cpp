// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Controls.h"
#include "ModulatorDrag.h"
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
    constexpr juce::uint32 peakHoldMs = 1000;
    constexpr float meterMoveDb = 0.1f; // the least change of a level that a meter shows

    // A meter falls 30 dB a second.
    constexpr float meterFallDbPerTick = 30.0f / DisplayTicker::ticksPerSecond;

    // The field a value is typed in: a line of small text in a box, at least as wide as a compact knob's place.
    const auto valueFieldHeight = juce::roundToInt (TextSize::small) + 2 * Space::xs;
    constexpr int valueFieldMinimumWidth = compactKnobSize + Space::l;

    // Where a slider's field goes: in a call-out or a dialog that holds the keys while it is up (a modulator's editor), in it,
    // as a field outside it would get no keys; elsewhere in its window's content (a window lays out its own children).
    juce::Component* findValueFieldParent (juce::Component& slider)
    {
        if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal != nullptr && modal->isParentOf (&slider))
            return modal;

        auto* window = slider.getTopLevelComponent();

        if (auto* resizable = dynamic_cast<juce::ResizableWindow*> (window))
            if (auto* content = resizable->getContentComponent(); content != nullptr && content->isParentOf (&slider))
                return content;

        return window != &slider ? window : nullptr;
    }
} // namespace

//==============================================================================
// The field a value is typed in, over its slider or the slider's readout. It follows the slider as the slider moves (a
// scroll), and goes, its text untaken, as the slider hides.
struct ValueSlider::ValueField final : public juce::TextEditor,
                                       private juce::ComponentMovementWatcher
{
    explicit ValueField (ValueSlider& s)
        : juce::ComponentMovementWatcher (&s),
          slider (s)
    {
        setTitle (s.getTitle());
        setDescription ("Type a value and press Enter; Esc leaves it as it was");
        setMultiLine (false);
        setReturnKeyStartsNewLine (false);
        setScrollbarsShown (false);
        setJustification (juce::Justification::centred);
        setIndents (Space::xs, 0);
        setFont (LookAndFeel::getNumberFont (TextSize::small));
        setTooltip (getDescription());
        setPopupMenuEnabled (false); // a menu would take the keys, which ends it: Ctrl+C and Ctrl+V work
    }

    // Opened with the slider's value, which set again unchanged would change nothing, or with what was typed.
    void show (const juce::String& typed)
    {
        shownValue = typed.isEmpty() ? slider.getTextFromValue (slider.getValue()) : juce::String();
        setText (typed.isEmpty() ? shownValue : typed, false);
        place();
        grabKeyboardFocus();

        if (typed.isEmpty())
            selectAll();
        else
            moveCaretToEnd();
    }

    // Over what it covers, as wide as the longest of the slider's texts at least, and inside its parent.
    void place()
    {
        auto* parent = getParentComponent();

        if (parent == nullptr)
            return;

        auto textWidth = 0.0f;

        for (const auto value : { slider.getMinimum(), slider.getMaximum(), slider.getValue() })
            textWidth = juce::jmax (textWidth, juce::GlyphArrangement::getStringWidth (getFont(), slider.getTextFromValue (value)));

        const auto target = slider.getValueFieldTarget (*parent);
        const auto width = juce::jmax (target.getWidth(), (int) std::ceil (textWidth) + 2 * Space::s, valueFieldMinimumWidth);
        const auto height = juce::jlimit (valueFieldHeight, controlHeight, target.getHeight());
        setBounds (juce::Rectangle<int> (width, height).withCentre (target.getCentre()).constrainedWithin (parent->getLocalBounds()));
    }

    // Ends it, once: with what it says (Enter, or the keys gone elsewhere) or without (Esc, or the slider gone), the keys back
    // to the slider after Enter or Esc. It hides at once, and goes a message later, out of what ended it.
    void finish (bool apply, bool keysBack)
    {
        if (std::exchange (finished, true))
            return;

        auto& owner = slider; // this goes with the slider, which setting the value may delete
        const auto text = getText();

        if (keysBack && owner.isShowing())
            owner.grabKeyboardFocus();
        else if (hasKeyboardFocus (true))
            giveAwayKeyboardFocus();

        setVisible (false);

        const juce::Component::SafePointer<ValueSlider> safeSlider (&owner);
        juce::MessageManager::callAsync (
            [safeSlider]
            {
                if (safeSlider != nullptr && safeSlider->valueField != nullptr && safeSlider->valueField->finished)
                    safeSlider->valueField = nullptr;
            });

        // A value that reads back as another (a note's length that its slider reads as a number) stays as it was.
        if (apply && (shownValue.isEmpty() || text != shownValue))
            owner.applyTypedText (text);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::returnKey || key == juce::KeyPress::escapeKey)
        {
            finish (key == juce::KeyPress::returnKey, true);
            return true;
        }

        return juce::TextEditor::keyPressed (key);
    }

    void focusLost (FocusChangeType cause) override
    {
        juce::TextEditor::focusLost (cause);
        finish (true, false);
    }

    ValueSlider& slider;
    bool finished = false;

private:
    juce::String shownValue; // the value's text it opened with, if it did

    using juce::ComponentMovementWatcher::componentMovedOrResized;
    using juce::ComponentMovementWatcher::componentVisibilityChanged;

    void componentMovedOrResized (bool, bool) override
    {
        if (! finished)
            place();
    }

    void componentPeerChanged() override { finish (false, false); }

    void componentVisibilityChanged() override
    {
        if (! slider.isShowing())
            finish (false, false);
    }
};

// A slider's readout's clicks: a click types a value over it, a right-click shows the slider's menu.
struct ValueSlider::ReadoutClicks final : public juce::MouseListener
{
    explicit ReadoutClicks (ValueSlider& s)
        : slider (s)
    {
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && slider.onContextMenu != nullptr && slider.isEnabled())
            slider.onContextMenu();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown())
            slider.showValueField();
    }

    ValueSlider& slider;
};

//==============================================================================
ValueSlider::ValueSlider (SliderStyle style, const juce::String& title, double defaultValue)
    : juce::Slider (style, juce::Slider::NoTextBox)
{
    setTitle (title);
    setName (title);
    setWantsKeyboardFocus (true);

    // The value pops up as the control is dragged or hovered, on the monitor it is on: JUCE has nowhere to put it on a
    // computer without one (a test machine's), so there it doesn't pop up.
    const auto hasMonitor = ! juce::Desktop::getInstance().getDisplays().displays.isEmpty();
    setPopupDisplayEnabled (hasMonitor, hasMonitor, nullptr);
    setDoubleClickReturnValue (true, defaultValue);
    setMouseDragSensitivity (knobDragDistance);

    // Ctrl, Alt and Cmd don't switch to JUCE's velocity-sensitive dragging, which surprises people.
    setVelocityModeParameters (1.0, 1, 0.0, false);
}

ValueSlider::~ValueSlider()
{
    setValueReadout (nullptr);
}

bool ValueSlider::canTypeValue() const
{
    return isEnabled() && isShowing() && ! isTwoValue() && ! isThreeValue();
}

void ValueSlider::showValueField (const juce::String& typed)
{
    if (! canTypeValue())
        return;

    if (getTextBoxPosition() != NoTextBox && isTextBoxEditable())
    {
        typeInTextBox (typed);
        return;
    }

    auto* parent = findValueFieldParent (*this);

    if (parent == nullptr)
        return;

    // One that has finished, hidden, goes now.
    if (valueField == nullptr || valueField->finished)
    {
        valueField = std::make_unique<ValueField> (*this);
        parent->addAndMakeVisible (*valueField);
    }

    valueField->show (typed);
}

juce::TextEditor* ValueSlider::getValueField() const noexcept
{
    return valueField != nullptr && ! valueField->finished ? valueField.get() : nullptr;
}

void ValueSlider::typeInTextBox (const juce::String& typed)
{
    // JUCE's text box, a label, shows its editor with its text selected; a number typed replaces it.
    showTextBox();

    if (typed.isEmpty())
        return;

    for (auto* child : getChildren())
    {
        if (auto* label = dynamic_cast<juce::Label*> (child))
        {
            if (auto* editor = label->getCurrentTextEditor())
            {
                editor->setText (typed, false);
                editor->moveCaretToEnd();
            }
        }
    }
}

void ValueSlider::applyTypedText (const juce::String& text)
{
    // A slider that reads no units reads numbers alone: JUCE would read text without one as 0.
    if (valueFromTextFunction == nullptr && ! text.containsAnyOf ("0123456789"))
        return;

    // As JUCE's own text box sets what is typed in it (Slider::Pimpl::textChanged()): between a drag's start and end, which
    // an attachment makes one undo step.
    const auto value = snapValue (getValueFromText (text), notDragging);

    if (! juce::approximatelyEqual (value, getValue()))
    {
        const ScopedDragNotification drag (*this);
        setValue (value, juce::sendNotificationSync);
    }
}

juce::Rectangle<int> ValueSlider::getValueFieldTarget (const juce::Component& parent) const
{
    if (readout != nullptr && readout->isShowing())
        return parent.getLocalArea (readout.getComponent(), readout->getLocalBounds());

    // A fader's: where its thumb is.
    auto area = getLocalBounds();
    const auto thumb = juce::roundToInt (getPositionOfValue (getValue()));

    if (getSliderStyle() == LinearVertical)
        area = juce::Rectangle<int> (getWidth(), controlHeight).withCentre ({ getWidth() / 2, thumb });
    else if (getSliderStyle() == LinearHorizontal)
        area = juce::Rectangle<int> (valueFieldMinimumWidth, getHeight()).withCentre ({ thumb, getHeight() / 2 });

    return parent.getLocalArea (this, area);
}

void ValueSlider::setValueReadout (juce::Component* newReadout)
{
    if (readout != nullptr && readoutClicks != nullptr)
        readout->removeMouseListener (readoutClicks.get());

    readout = newReadout;

    if (newReadout == nullptr)
        return;

    if (readoutClicks == nullptr)
        readoutClicks = std::make_unique<ReadoutClicks> (*this);

    newReadout->addMouseListener (readoutClicks.get(), false);
    newReadout->setInterceptsMouseClicks (true, false);
    newReadout->setMouseCursor (juce::MouseCursor::IBeamCursor);
}

void ValueSlider::setDragStep (double step)
{
    dragStep = juce::jmax (0.0, step);
}

double ValueSlider::snapValue (double attemptedValue, DragMode dragMode)
{
    if (dragMode == notDragging || dragStep <= 0.0 || fineDrag)
        return attemptedValue;

    const auto steps = std::round ((attemptedValue - getMinimum()) / dragStep);
    return juce::jlimit (getMinimum(), getMaximum(), getMinimum() + steps * dragStep);
}

bool ValueSlider::keyPressed (const juce::KeyPress& key)
{
    // A number typed opens the field with it, as Ableton Live's controls take one; Shift may give the number or its sign on
    // some keyboards. With Ctrl, Alt or Cmd, a key is a shortcut's; Enter is left to what it does where the slider is (a
    // window's default button, the piano roll's Apply).
    const auto mods = key.getModifiers();
    const auto typed = key.getTextCharacter();

    if (canTypeValue() && ! mods.isCtrlDown() && ! mods.isAltDown() && ! mods.isCommandDown()
        && (juce::CharacterFunctions::isDigit (typed) || typed == '-' || typed == '+' || typed == '.'))
    {
        showValueField (juce::String::charToString (typed));
        return true;
    }

    return juce::Slider::keyPressed (key);
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

void Knob::setModulation (std::optional<ModulationRange> newModulation)
{
    const auto same = [] (const std::optional<ModulationRange>& a, const std::optional<ModulationRange>& b)
    {
        if (a.has_value() != b.has_value())
            return false;

        return ! a.has_value()
               || (juce::approximatelyEqual (a->low, b->low) && juce::approximatelyEqual (a->high, b->high)
                   && juce::approximatelyEqual (a->now, b->now) && a->isOn == b->isOn);
    };

    if (! same (modulation, newModulation))
    {
        modulation = newModulation;
        repaint();
    }
}

bool Knob::isOnRing (juce::Point<float> position) const
{
    // The knob's outer edge, as ui::LookAndFeel draws it (drawRotarySlider()), and the ring just outside it.
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    return position.getDistanceFrom (bounds.getCentre()) >= radius - ModulationRing::grabWidth;
}

void Knob::paint (juce::Graphics& g)
{
    ValueSlider::paint (g);

    if (! modulation.has_value() && ! dropping && ! target)
        return;

    const auto& p = getPalette (*this);
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f + ModulationRing::offset;
    const auto centre = bounds.getCentre();
    const auto rotary = getRotaryParameters();
    const auto angleOf = [&rotary] (float proportion)
    { return rotary.startAngleRadians + juce::jlimit (0.0f, 1.0f, proportion) * (rotary.endAngleRadians - rotary.startAngleRadians); };

    // While a modulator is dragged that could modulate it: a faint ring all the way round; under the pointer, a full one over
    // a wash.
    if (dropping || target)
    {
        const auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre);

        if (dropping)
        {
            g.setColour (p.modulation.withAlpha (ModulationRing::dropWash));
            g.fillEllipse (circle);
        }

        g.setColour (p.modulation.withMultipliedAlpha (dropping ? 1.0f : ModulationRing::targetAlpha));
        g.drawEllipse (circle, dropping ? ModulationRing::dropWidth : ModulationRing::width);
    }

    g.setColour (p.modulation);

    // Its modulators switched off: faint, where they would move it, with no dot.
    if (modulation.has_value())
    {
        if (! modulation->isOn)
            g.setColour (p.modulation.withMultipliedAlpha (ModulationRing::offAlpha));

        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, angleOf (modulation->low), angleOf (modulation->high), true);
        g.strokePath (arc, juce::PathStrokeType (ModulationRing::width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (modulation->isOn)
        {
            const auto now = centre.getPointOnCircumference (radius, angleOf (modulation->now));
            g.fillEllipse (juce::Rectangle<float> (ModulationRing::dotSize, ModulationRing::dotSize).withCentre (now));
        }
    }
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    // On the ring, a drag changes the modulation's amount; anywhere else, the knob's value.
    draggingRing = ! e.mods.isPopupMenu() && modulation.has_value() && isOnRing (e.position) && onModulationDragStart != nullptr
                   && onModulationDragStart();

    if (! draggingRing)
        ValueSlider::mouseDown (e);
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingRing)
    {
        ValueSlider::mouseDrag (e);
        return;
    }

    const auto change = (float) -e.getDistanceFromDragStartY() / (float) knobDragDistance * (e.mods.isShiftDown() ? fineDragFactor : 1.0f);

    if (onModulationDrag != nullptr)
        onModulationDrag (change);
}

void Knob::mouseUp (const juce::MouseEvent& e)
{
    if (! draggingRing)
    {
        ValueSlider::mouseUp (e);
        return;
    }

    draggingRing = false;

    if (onModulationDragEnd != nullptr)
        onModulationDragEnd();
}

void Knob::showModulatorTargets (std::optional<ModulatorID> modulator)
{
    // Every knob in every window: a modulator dragged reaches them all, a device's own window too.
    const std::function<void (juce::Component&)> visit = [&modulator, &visit] (juce::Component& component)
    {
        if (auto* knob = dynamic_cast<Knob*> (&component))
        {
            const auto isTarget = modulator.has_value() && knob->canAcceptModulator != nullptr && knob->canAcceptModulator (*modulator);

            if (isTarget != knob->target)
            {
                knob->target = isTarget;
                knob->repaint();
            }
        }

        for (auto* child : component.getChildren())
            visit (*child);
    };

    auto& desktop = juce::Desktop::getInstance();

    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* window = desktop.getComponent (i))
            visit (*window);
}

bool Knob::isInterestedInDragSource (const SourceDetails& details)
{
    const auto modulator = readModulatorDrag (details.description);
    return modulator.has_value() && canAcceptModulator != nullptr && canAcceptModulator (*modulator);
}

void Knob::itemDragEnter (const SourceDetails&)
{
    dropping = true;
    repaint();
}

void Knob::itemDragExit (const SourceDetails&)
{
    dropping = false;
    repaint();
}

void Knob::itemDropped (const SourceDetails& details)
{
    dropping = false;
    repaint();

    if (const auto modulator = readModulatorDrag (details.description); modulator.has_value() && onModulatorDropped != nullptr)
        onModulatorDropped (*modulator);
}

Fader::Fader (Direction direction, const juce::String& title, double defaultValue)
    : ValueSlider (direction == Direction::vertical ? LinearVertical : LinearHorizontal, title, defaultValue)
{
    setSliderSnapsToMousePosition (false);
}

//==============================================================================
FaderScale::FaderScale (const Fader& f)
    : fader (f)
{
    setInterceptsMouseClicks (false, false);
    setAccessible (false); // screen readers read the fader's value
}

void FaderScale::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto right = (float) getWidth();
    const auto textRight = right - (float) (FaderScaleTint::unityTick + FaderScaleTint::gap);
    g.setFont (juce::FontOptions (FaderScaleTint::textSize));

    // Each level where the fader's thumb is at it: a tick towards the fader, on the pixel row there so that it is sharp, and
    // its number.
    const auto mark = [&] (double position, const juce::String& text, bool unity)
    {
        const auto y = std::floor (getLocalPoint (&fader, juce::Point<float> (0.0f, fader.getPositionOfValue (position))).y);
        const auto tick = (float) (unity ? FaderScaleTint::unityTick : FaderScaleTint::tick);
        g.setColour (unity ? p.text : p.textMuted);
        g.fillRect (juce::Rectangle<float> (right - tick, y, tick, 1.0f));
        g.drawText (text, juce::Rectangle<float> (0.0f, y - FaderScaleTint::textSize, textRight, 2.0f * FaderScaleTint::textSize),
                    juce::Justification::centredRight, false);
    };

    for (const auto decibels : FaderScaleTint::marks)
        mark (decibelsToFaderPosition (decibels), (decibels > 0.0f ? "+" : "") + juce::String (juce::roundToInt (decibels)),
              juce::exactlyEqual (decibels, 0.0f));

    mark (0.0, juce::String::fromUTF8 ("-\xe2\x88\x9e"), false); // silence, at the bottom
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
        return ValueSlider::keyPressed (key); // a number typed opens its text box with it

    setValue (juce::jlimit (getMinimum(), getMaximum(), getValue() + steps * step), juce::sendNotificationSync);
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> NumberField::createAccessibilityHandler()
{
    return std::make_unique<NumberFieldAccessibility> (*this);
}

void showAsDecibels (juce::Slider& slider)
{
    // A fader's position, whatever it controls: a slider left with JUCE's range (0 to 10) put 0 dB a tenth of the way along
    // (the browser's preview volume's did, until 2026-11-07).
    slider.setRange (0.0, 1.0);
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

    // A pitch a little off is wrong: dragged, it moves in whole semitones (as Live's Transpose and Serum's Semi do), and in
    // fine steps between them with Shift.
    if (auto* valueSlider = dynamic_cast<ValueSlider*> (&slider))
        valueSlider->setDragStep (parameter.unit == devices::Unit::semitones && ! parameter.step.has_value() ? 1.0 : 0.0);
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
    setThemeColour (button, juce::TextButton::buttonColourId, primary ? &Palette::accent : &Palette::surfaceRaised);
    setThemeColour (button, juce::TextButton::textColourOffId, primary ? &Palette::textOnAccent : &Palette::text);
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

    // A right-click offers to automate the parameter, or modulate it (docs/design.md, section 10).
    if (auto* valueSlider = dynamic_cast<ValueSlider*> (&slider))
        valueSlider->onContextMenu = [this] { showMenu(); };

    // A knob's ring changes the amount of its first modulation, and a modulator dropped on it modulates it.
    if (auto* knob = dynamic_cast<Knob*> (&slider))
    {
        knob->onModulationDragStart = [this]
        {
            const auto modulations = getModulations (handle);
            modulationDrag = modulations.empty() ? nullptr : beginModulationDrag (handle, modulations.front().modulator);
            return modulationDrag != nullptr;
        };

        knob->onModulationDrag = [this] (float change)
        {
            if (modulationDrag != nullptr)
                modulationDrag->setAmount (modulationDrag->getStartAmount() + change);
        };

        knob->onModulationDragEnd = [this] { modulationDrag = nullptr; };

        knob->canAcceptModulator = [this] (ModulatorID id)
        {
            const auto modulators = getModulatorsFor (handle);
            return std::any_of (modulators.begin(), modulators.end(), [id] (const ModulatorInfo& m) { return m.id == id; });
        };

        // One that modulates it already keeps its amount.
        knob->onModulatorDropped = [this] (ModulatorID id)
        {
            const auto modulations = getModulations (handle);

            if (std::none_of (modulations.begin(), modulations.end(), [id] (const Modulation& m) { return m.modulator == id; }))
                modulate (handle, id, defaultModulationAmount);
        };
    }
}

void ParameterAttachment::showMenu()
{
    if (auto menu = getMenu(); menu.getNumItems() > 0)
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slider));
}

juce::PopupMenu ParameterAttachment::getMenu()
{
    juce::PopupMenu menu;

    if (! handle.isValid())
        return menu;

    // The slider and this go together; the menu's choices come later, when either may be gone.
    const juce::Component::SafePointer<juce::Slider> safeSlider (&slider);
    const auto act = [this, safeSlider] (std::function<void (ParameterHandle&)> action)
    {
        return [this, safeSlider, action = std::move (action)]
        {
            if (safeSlider != nullptr)
                action (handle);
        };
    };

    if (const auto name = handle.getName(); name.isNotEmpty())
        menu.addSectionHeader (name);

    // A value typed, as FL Studio's Type in value: the slider's field, over it.
    menu.addItem ("Type a Value...",
                  [safeSlider]
                  {
                      if (auto* valueSlider = dynamic_cast<ValueSlider*> (safeSlider.getComponent()))
                          valueSlider->showValueField();
                  });

    // A send that the first change makes has nothing to automate or modulate yet.
    if (! handle.exists())
        return menu;

    // A device's parameter can be automated; a modulator's, in its editor, can't yet.
    const auto isDevices = handle.getRef().isValid();
    menu.addSeparator();
    menu.addItem ("Automate", isDevices, false, act ([] (ParameterHandle& h) { showAutomationLane (h); }));
    menu.addItem ("Delete Automation", isAutomated (handle), false, act ([] (ParameterHandle& h) { deleteAutomation (h); }));

    // Modulation (docs/design.md, section 10): the modulators of its track, ticked while they modulate it (choosing one
    // adds or takes away its modulation), and a new one of each kind.
    juce::PopupMenu modulateMenu;
    const auto modulations = getModulations (handle);

    for (const auto& modulator : getModulatorsFor (handle))
    {
        const auto id = modulator.id;
        const auto on = std::any_of (modulations.begin(), modulations.end(), [id] (const Modulation& m) { return m.modulator == id; });
        modulateMenu.addItem (juce::PopupMenu::Item (modulator.name)
                                  .setTicked (on)
                                  .setAction (act (
                                      [id, on] (ParameterHandle& h)
                                      {
                                          if (on)
                                              removeModulation (h, id);
                                          else
                                              modulate (h, id, defaultModulationAmount);
                                      })));
    }

    if (modulateMenu.getNumItems() > 0)
        modulateMenu.addSeparator();

    for (const auto type : allModulatorTypes)
        modulateMenu.addItem ("New " + getModulatorTypeName (type),
                              act ([type] (ParameterHandle& h) { modulateWithNew (h, type, defaultModulationAmount); }));

    menu.addSubMenu ("Modulate", modulateMenu, isDevices);
    return menu;
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
    if (const auto info = handle.getInfo())
    {
        showAsParameter (slider, *info);
    }
    else
    {
        slider.textFromValueFunction = [this] (double value) { return handle.getText ((float) value); };
        slider.valueFromTextFunction = [this] (const juce::String& text)
        { return (double) handle.readText (text).value_or ((float) slider.getValue()); };
    }

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

    // A knob shows its value without modulation, and its ring the modulation; a fader shows where the sound is.
    auto* knob = dynamic_cast<Knob*> (&slider);
    const auto modulation = knob != nullptr ? getModulationRange (handle) : std::nullopt;

    if (handle.isValid() && ! handle.isInGesture())
        slider.setValue (modulation.has_value() ? handle.getBaseValue() : handle.getValue(), juce::dontSendNotification);

    if (knob != nullptr)
        knob->setModulation (modulation);
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
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)) // the component, not a Display to copy
{
    setTitle (title);
    setDescription ("The peak level, from -60 to +6 dB. Red means above 0 dB, where the sound clips.");
    levels.fill ((float) silenceDb);
    peaks.fill ((float) silenceDb);
    shownLevels = levels;
    shownPeaks = peaks;
}

LevelMeter::~LevelMeter() = default;

void LevelMeter::setSource (LevelSource newSource)
{
    source = std::move (newSource);
}

void LevelMeter::tick()
{
    source.refresh();
    const bool hasSource = source.isMeasuring();
    const bool mono = hasSource && source.getNumChannels() == 1;
    const auto now = juce::Time::getMillisecondCounter();

    for (size_t ch = 0; ch < (size_t) numChannels; ++ch)
    {
        // Rises at once, falls steadily; a mono source shows on both bars. The peak holds for a second, unless a higher
        // one comes.
        const auto measured = hasSource ? source.getAndClearLevel (mono ? 0 : (int) ch) : (float) silenceDb;
        levels[ch] = juce::jmax (measured, levels[ch] - meterFallDbPerTick, (float) silenceDb);

        if (levels[ch] > peaks[ch] || now - peakTimes[ch] > peakHoldMs)
        {
            peaks[ch] = levels[ch];
            peakTimes[ch] = now;
        }
    }

    // Only what moves what is drawn repaints: levels below the meter's range all look the same, so a silent meter, or
    // one that holds still, repaints nothing (docs/performance-audit.md, section 8).
    const auto moves = [] (float value, float drawn)
    {
        return std::abs (juce::jmax (value, minDb) - juce::jmax (drawn, minDb)) > meterMoveDb || (value > minDb) != (drawn > minDb)
               || (value > 0.0f) != (drawn > 0.0f);
    };

    for (size_t ch = 0; ch < (size_t) numChannels; ++ch)
    {
        if (moves (levels[ch], shownLevels[ch]) || moves (peaks[ch], shownPeaks[ch]))
        {
            shownLevels = levels;
            shownPeaks = peaks;
            repaint();
            return;
        }
    }
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
        const auto level = toProportion (shownLevels[ch]);

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
        if (shownPeaks[ch] > minDb)
        {
            const auto peak = toProportion (shownPeaks[ch]);
            g.setColour (shownPeaks[ch] > 0.0f ? p.danger : p.text);

            if (vertical)
                g.fillRect (bar.getX(), bar.getBottom() - peak * length - 1.0f, bar.getWidth(), 2.0f);
            else
                g.fillRect (bar.getX() + peak * length - 1.0f, bar.getY(), 2.0f, bar.getHeight());
        }
    }
}

juce::String LevelMeter::getLevelText() const
{
    return formatDecibels (juce::jmax (shownPeaks[0], shownPeaks[1]));
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
} // namespace phazite::ui
