// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Controls.h"
#include "LookAndFeel.h"
#include "model/ValueText.h"

namespace phazite::ui
{
namespace
{
    constexpr float fineDragFactor = 0.2f; // Shift-drag moves a fifth as far
    constexpr int knobDragDistance = 200;  // pixels from one end of a knob's range to the other
    constexpr int meterRefreshRateHz = 30;
    constexpr float meterFallDbPerTick = 1.0f; // 30 dB a second
    constexpr juce::uint32 peakHoldMs = 1000;

    // Tracktion stores a parameter's value in the project only once it changes, and undoing that
    // first change removes it again. phazite::undo() then couldn't play the old value without an undo
    // step of its own, which would cost the redo history. So the value is stored first, without an
    // undo step: with the value unchanged, setParameter() only writes it, past the undo manager.
    void storeValueWithoutUndo (te::AutomatableParameter& parameter)
    {
        parameter.setParameter (parameter.getCurrentValue(), juce::sendNotification);
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
    fineDrag = e.mods.isShiftDown();
    juce::Slider::mouseDown (e);
}

void ValueSlider::mouseDrag (const juce::MouseEvent& e)
{
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

void showAsDecibels (juce::Slider& slider)
{
    slider.textFromValueFunction = [] (double position) { return formatDecibels (te::volumeFaderPositionToDB ((float) position)); };
    slider.valueFromTextFunction = [&slider] (const juce::String& text)
    {
        if (const auto dB = parseDecibels (text))
            return (double) te::decibelsToVolumeFaderPosition ((float) *dB);

        return slider.getValue();
    };
}

void showAsPan (juce::Slider& slider)
{
    slider.textFromValueFunction = [] (double pan) { return formatPan (pan); };
    slider.valueFromTextFunction = [&slider] (const juce::String& text) { return parsePan (text).value_or (slider.getValue()); };
}

//==============================================================================
ParameterAttachment::ParameterAttachment (juce::Slider& s, juce::String name)
    : slider (s),
      undoName (std::move (name))
{
    slider.setEnabled (false);

    slider.onDragStart = [this]
    {
        if (auto parameter = getParameter())
        {
            dragging = true;
            storeValueWithoutUndo (*parameter);
            parameter->getEdit().getUndoManager().beginNewTransaction (undoName);
            parameter->parameterChangeGestureBegin(); // for automation recording
        }
    };

    slider.onDragEnd = [this]
    {
        if (auto parameter = getParameter(); parameter != nullptr && dragging)
            parameter->parameterChangeGestureEnd();

        dragging = false;
    };

    slider.onValueChange = [this]
    {
        if (auto parameter = getParameter())
        {
            if (! dragging) // a key press or a double-click: one undo step each
            {
                storeValueWithoutUndo (*parameter);
                parameter->getEdit().getUndoManager().beginNewTransaction (undoName);
            }

            parameter->setParameter ((float) slider.getValue(), juce::sendNotification);
        }
    };
}

void ParameterAttachment::setParameter (te::Plugin* newPlugin, const juce::String& newParameterID)
{
    if (plugin == newPlugin && parameterID == newParameterID)
        return;

    plugin = nullptr; // so that setting the range below can't change the parameter
    dragging = false;

    if (auto parameter = newPlugin != nullptr ? newPlugin->getAutomatableParameterByID (newParameterID) : nullptr)
    {
        const auto range = parameter->getValueRange();
        slider.setRange (range.getStart(), range.getEnd());

        if (const auto defaultValue = parameter->getDefaultValue())
            slider.setDoubleClickReturnValue (true, *defaultValue);
    }

    plugin = newPlugin;
    parameterID = newParameterID;
    update();
}

void ParameterAttachment::update()
{
    const auto parameter = getParameter();
    slider.setEnabled (parameter != nullptr);

    if (parameter != nullptr && ! dragging)
        slider.setValue (parameter->getCurrentValue(), juce::dontSendNotification);
}

te::AutomatableParameter::Ptr ParameterAttachment::getParameter() const
{
    if (auto* p = plugin.get())
        return p->getAutomatableParameterByID (parameterID);

    return {};
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

LevelMeter::~LevelMeter()
{
    detach();
}

void LevelMeter::setSource (te::LevelMeasurer* newSource)
{
    if (source.get() == newSource)
        return;

    detach();
    source = newSource;

    if (newSource != nullptr)
        newSource->addClient (client);
}

void LevelMeter::detach()
{
    if (auto* oldSource = source.get())
        oldSource->removeClient (client);

    source = nullptr;
    client.reset();
}

void LevelMeter::timerCallback()
{
    const bool hasSource = source.get() != nullptr;
    const bool mono = hasSource && client.getNumChannelsUsed() == 1;
    const auto now = juce::Time::getMillisecondCounter();
    bool changed = false;

    for (size_t ch = 0; ch < (size_t) numChannels; ++ch)
    {
        // Rises at once, falls steadily; a mono source shows on both bars.
        const auto measured = hasSource ? client.getAndClearAudioLevel (mono ? 0 : (int) ch).dB : (float) silenceDb;
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
} // namespace phazite::ui
