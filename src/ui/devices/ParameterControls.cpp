// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ParameterControls.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    std::unique_ptr<juce::Label> makeLabel (const juce::String& text)
    {
        auto label = std::make_unique<juce::Label> (juce::String(), text);
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        label->setMinimumHorizontalScale (0.8f);
        label->setInterceptsMouseClicks (false, false);
        return label;
    }

    // What a parameter's control says in the hint bar: what it ranges over and where it starts, and for a knob how to work it.
    juce::String describe (const devices::ParameterInfo& parameter, const ParameterHandle& handle)
    {
        const auto name = toString (parameter.name);

        if (parameter.unit == devices::Unit::choice)
        {
            juce::StringArray choices;

            for (const auto& choice : parameter.choices)
                choices.add (toString (choice));

            const auto first = (int) std::lround (parameter.defaultValue - parameter.minimum);
            return name + ": " + choices.joinIntoString (", ")
                   + (juce::isPositiveAndBelow (first, choices.size()) ? ", " + choices[first] + " at first" : juce::String());
        }

        if (parameter.unit == devices::Unit::toggle)
            return name + ": on or off, " + (parameter.defaultValue > 0.5f ? "on" : "off") + " at first";

        return name + ": from " + handle.getText (parameter.minimum) + " to " + handle.getText (parameter.maximum) + ", "
               + handle.getText (parameter.defaultValue)
               + " at first. Drag up or down, double-click for where it started, or right-click to automate or modulate it";
    }
} // namespace

juce::String getShownName (const juce::String& name, const juce::String& partTitle)
{
    if (partTitle.isNotEmpty() && name.startsWith (partTitle + " "))
        return name.substring (partTitle.length() + 1);

    const auto first = partTitle.upToFirstOccurrenceOf (" ", false, false);

    if (first.isNotEmpty() && name.startsWith (first + " "))
        return name.fromFirstOccurrenceOf (" ", false, false);

    return name;
}

//==============================================================================
struct ParameterControls::Control
{
    std::string_view id;
    std::unique_ptr<Knob> knob;
    std::unique_ptr<juce::ComboBox> box;
    std::unique_ptr<juce::ToggleButton> toggle; // for a parameter that is on or off
    std::unique_ptr<ParameterAttachment> knobAttachment;
    std::unique_ptr<ChoiceAttachment> boxAttachment;
    std::unique_ptr<ToggleAttachment> toggleAttachment;
    std::unique_ptr<juce::Label> name, value;
};

int ParameterControls::getCellWidth (Size size)
{
    return (size == Size::large ? knobSize : compactKnobSize) + Space::l;
}

int ParameterControls::getCellHeight (Size size)
{
    return (size == Size::large ? knobSize : compactKnobSize) + 2 * labelHeight;
}

ParameterControls::ParameterControls (const DeviceHandle& device, std::vector<std::string_view> parameterIDs, int numColumns, Size s,
                                      const juce::String& dropFromNames, const std::vector<juce::String>& shownNames)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      columns (numColumns > 0 ? numColumns : std::max (1, (int) parameterIDs.size())),
      size (s)
{
    const auto* info = device.getInfo();

    for (size_t n = 0; n < parameterIDs.size(); ++n)
    {
        const auto id = parameterIDs[n];
        const auto index = info != nullptr ? devices::findParameter (*info, id) : -1;

        if (index < 0)
        {
            jassertfalse; // the device has no such parameter
            continue;
        }

        const auto& parameter = info->parameters[(size_t) index];
        const auto fullName = toString (parameter.name);
        auto handle = device.getParameter (parameter.id);
        const auto help = describe (parameter, handle);
        auto control = std::make_unique<Control>();
        control->id = parameter.id;
        control->name =
            makeLabel (n < shownNames.size() && shownNames[n].isNotEmpty() ? shownNames[n] : getShownName (fullName, dropFromNames));
        addAndMakeVisible (*control->name);

        if (parameter.unit == devices::Unit::choice)
        {
            control->box = std::make_unique<juce::ComboBox>();
            control->box->setTitle (fullName);
            control->boxAttachment = std::make_unique<ChoiceAttachment> (*control->box);
            control->boxAttachment->setParameter (std::move (handle));
            control->box->setTooltip (help);
            addAndMakeVisible (*control->box);
        }
        else if (parameter.unit == devices::Unit::toggle)
        {
            control->toggle = std::make_unique<juce::ToggleButton>();
            control->toggle->setTitle (fullName);
            control->toggleAttachment = std::make_unique<ToggleAttachment> (*control->toggle);
            control->toggleAttachment->setParameter (std::move (handle));
            control->toggle->setTooltip (help);
            addAndMakeVisible (*control->toggle);
        }
        else
        {
            control->knob = std::make_unique<Knob> (fullName, parameter.defaultValue);
            showAsParameter (*control->knob, parameter);
            control->knobAttachment = std::make_unique<ParameterAttachment> (*control->knob);
            control->knobAttachment->setParameter (std::move (handle));
            control->knob->setTooltip (help);
            control->value = makeLabel ({});
            control->value->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
            addAndMakeVisible (*control->knob);
            addAndMakeVisible (*control->value);
        }

        controls.push_back (std::move (control));
    }

    // Each column as wide as its widest control.
    std::vector<int> widths ((size_t) columns, 0);

    for (size_t i = 0; i < controls.size(); ++i)
        widths[i % (size_t) columns] = std::max (widths[i % (size_t) columns], getWidthOf (*controls[i]));

    for (const auto width : widths)
        preferredWidth += width;

    const auto rows = ((int) controls.size() + columns - 1) / columns;
    preferredHeight = rows * getCellHeight (size);
    setSize (preferredWidth, preferredHeight);
    refresh();
}

ParameterControls::~ParameterControls() = default;

int ParameterControls::getWidthOf (const Control& control) const
{
    // A list wide enough for its choices' names.
    return control.box != nullptr ? getCellWidth (Size::compact) * 3 / 2 + Space::s : getCellWidth (size);
}

juce::Component* ParameterControls::findControl (std::string_view parameterID) const
{
    for (const auto& control : controls)
    {
        if (control->id == parameterID)
        {
            if (control->knob != nullptr)
                return control->knob.get();

            if (control->toggle != nullptr)
                return control->toggle.get();

            return control->box.get();
        }
    }

    return nullptr;
}

void ParameterControls::refresh()
{
    for (const auto& control : controls)
    {
        if (control->knobAttachment != nullptr)
        {
            control->knobAttachment->update();
            control->value->setText (control->knob->getTextFromValue (control->knob->getValue()), juce::dontSendNotification);
        }

        if (control->boxAttachment != nullptr)
            control->boxAttachment->update();

        if (control->toggleAttachment != nullptr)
            control->toggleAttachment->update();
    }
}

void ParameterControls::resized()
{
    const auto knob = size == Size::large ? knobSize : compactKnobSize;
    const auto cellHeight = getCellHeight (size);
    std::vector<int> widths ((size_t) columns, 0);

    for (size_t i = 0; i < controls.size(); ++i)
        widths[i % (size_t) columns] = std::max (widths[i % (size_t) columns], getWidthOf (*controls[i]));

    // Spread across the width given, if wider than the grid.
    const auto spare = std::max (0, getWidth() - preferredWidth) / std::max (1, columns);

    for (size_t i = 0; i < controls.size(); ++i)
    {
        auto& control = *controls[i];
        const auto column = (int) (i % (size_t) columns), row = (int) (i / (size_t) columns);
        auto x = 0;

        for (int c = 0; c < column; ++c)
            x += widths[(size_t) c] + spare;

        auto cell = juce::Rectangle<int> (x, row * cellHeight, widths[(size_t) column] + spare, cellHeight);
        auto top = cell.removeFromTop (knob);

        if (control.knob != nullptr)
            control.knob->setBounds (top.withSizeKeepingCentre (knob, knob));
        else if (control.toggle != nullptr)
            control.toggle->setBounds (top.withSizeKeepingCentre (controlHeight, controlHeight));
        else
            control.box->setBounds (top.withSizeKeepingCentre (cell.getWidth() - Space::s, controlHeight));

        control.name->setBounds (cell.removeFromTop (labelHeight));

        if (control.value != nullptr)
            control.value->setBounds (cell.removeFromTop (labelHeight));
    }
}
} // namespace phazite::ui
