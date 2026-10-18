// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DevicePanel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int refreshRateHz = 10;

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;
    const auto cellWidth = knobSize + Space::l;
    const auto choiceWidth = cellWidth + cellWidth / 2; // a list, wide enough for its choices' names
    const auto cellHeight = knobSize + 2 * labelHeight; // the control, its name and its value
    const auto rowHeight = labelHeight + cellHeight;    // a row of groups: their titles, then their controls

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    // A control's name within its group: without the group's first word in front.
    juce::String getShownName (const juce::String& name, const juce::String& groupTitle)
    {
        const auto first = groupTitle.upToFirstOccurrenceOf (" ", false, false);

        if (first.isNotEmpty() && name.startsWith (first + " "))
            return name.fromFirstOccurrenceOf (" ", false, false);

        return name;
    }

    std::unique_ptr<juce::Label> makeLabel (const juce::String& text)
    {
        auto label = std::make_unique<juce::Label> (juce::String(), text);
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        return label;
    }
} // namespace

DevicePanel::DevicePanel (const DeviceHandle& device, const std::vector<std::vector<Group>>& rows)
{
    const auto* info = device.getInfo();
    auto widest = 0;

    for (const auto& row : rows)
    {
        auto& placedRow = groups.emplace_back();
        auto rowWidth = 0;

        for (const auto& group : row)
        {
            PlacedGroup placed;
            placed.title = makeLabel (group.title);
            placed.title->setJustificationType (juce::Justification::centredLeft);
            placed.title->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
            addAndMakeVisible (*placed.title);
            placed.first = (int) controls.size();

            for (const auto id : group.parameterIDs)
            {
                const auto index = info != nullptr ? devices::findParameter (*info, id) : -1;

                if (index < 0)
                {
                    jassertfalse; // the device has no such parameter
                    continue;
                }

                const auto& parameter = info->parameters[(size_t) index];
                const auto fullName = toString (parameter.name);
                auto control = std::make_unique<Control>();
                control->id = parameter.id;
                control->name = makeLabel (getShownName (fullName, group.title));
                addAndMakeVisible (*control->name);

                if (parameter.unit == devices::Unit::choice)
                {
                    control->box = std::make_unique<juce::ComboBox>();
                    control->box->setTitle (fullName);
                    control->boxAttachment = std::make_unique<ChoiceAttachment> (*control->box);
                    control->boxAttachment->setParameter (device.getParameter (parameter.id));
                    addAndMakeVisible (*control->box);
                }
                else if (parameter.unit == devices::Unit::toggle)
                {
                    control->toggle = std::make_unique<juce::ToggleButton>();
                    control->toggle->setTitle (fullName);
                    control->toggleAttachment = std::make_unique<ToggleAttachment> (*control->toggle);
                    control->toggleAttachment->setParameter (device.getParameter (parameter.id));
                    addAndMakeVisible (*control->toggle);
                }
                else
                {
                    control->knob = std::make_unique<Knob> (fullName, parameter.defaultValue);
                    showAsParameter (*control->knob, parameter);
                    control->knobAttachment = std::make_unique<ParameterAttachment> (*control->knob);
                    control->knobAttachment->setParameter (device.getParameter (parameter.id));
                    control->value = makeLabel ({});
                    control->value->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
                    addAndMakeVisible (*control->knob);
                    addAndMakeVisible (*control->value);
                }

                placed.width += control->box != nullptr ? choiceWidth : cellWidth;
                controls.push_back (std::move (control));
                ++placed.count;
            }

            rowWidth += (rowWidth > 0 ? Space::xl : 0) + placed.width;
            placedRow.push_back (std::move (placed));
        }

        widest = std::max (widest, rowWidth);
    }

    const auto numRows = (int) groups.size();
    setSize (2 * Space::l + widest, 2 * Space::l + numRows * (labelHeight + cellHeight) + std::max (0, numRows - 1) * Space::m);
    refresh();
    startTimerHz (refreshRateHz);
}

DevicePanel::~DevicePanel()
{
    stopTimer();
}

juce::Component* DevicePanel::findControl (std::string_view parameterID) const
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

void DevicePanel::refresh()
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

void DevicePanel::paint (juce::Graphics& g)
{
    const auto& palette = getPalette (*this);
    g.fillAll (palette.surface);

    // A frame around each group's controls.
    g.setColour (palette.border);

    for (const auto& row : groups)
    {
        for (const auto& group : row)
        {
            if (group.count == 0)
                continue;

            const auto first = controls[(size_t) group.first].get();
            const auto last = controls[(size_t) (group.first + group.count - 1)].get();
            const auto top = first->name->getBounds().getUnion (last->name->getBounds());
            const auto area = juce::Rectangle<int>::leftTopRightBottom (top.getX(), group.title->getBottom(), top.getRight(),
                                                                        top.getBottom() + labelHeight);
            g.drawRoundedRectangle (area.toFloat().expanded ((float) Space::xs / 2.0f), cornerRadius, 1.0f);
        }
    }
}

void DevicePanel::resized()
{
    auto y = Space::l;

    for (const auto& row : flow (getHeight()))
    {
        auto x = Space::l;

        for (const auto* group : row)
        {
            group->title->setBounds (x, y, group->width, labelHeight);
            auto left = x;

            for (int i = 0; i < group->count; ++i)
            {
                auto& control = *controls[(size_t) (group->first + i)];
                const auto width = control.box != nullptr ? choiceWidth : cellWidth;
                auto cell = juce::Rectangle<int> (left, y + labelHeight, width, cellHeight);
                left += width;
                auto top = cell.removeFromTop (knobSize);

                if (control.knob != nullptr)
                    control.knob->setBounds (top.withSizeKeepingCentre (knobSize, knobSize));
                else if (control.toggle != nullptr)
                    control.toggle->setBounds (top.withSizeKeepingCentre (controlHeight, controlHeight));
                else
                    control.box->setBounds (top.withSizeKeepingCentre (width - Space::s, controlHeight));

                control.name->setBounds (cell.removeFromTop (labelHeight));

                if (control.value != nullptr)
                    control.value->setBounds (cell.removeFromTop (labelHeight));
            }

            x += group->width + Space::xl;
        }

        y += rowHeight + Space::m;
    }
}

int DevicePanel::getRowWidth (const std::vector<const PlacedGroup*>& row)
{
    auto width = 0;

    for (const auto* group : row)
        width += (width > 0 ? Space::xl : 0) + group->width;

    return width;
}

std::vector<std::vector<const DevicePanel::PlacedGroup*>> DevicePanel::flow (int height) const
{
    // The rows as given while they fit; with fewer, the groups in their order, each row about as wide as the others.
    const auto fit = std::max (1, (height - 2 * Space::l + Space::m) / (rowHeight + Space::m));
    std::vector<std::vector<const PlacedGroup*>> rows;

    if (fit >= (int) groups.size())
    {
        for (const auto& row : groups)
        {
            auto& placed = rows.emplace_back();

            for (const auto& group : row)
                placed.push_back (&group);
        }

        return rows;
    }

    auto total = 0;

    for (const auto& row : groups)
        for (const auto& group : row)
            total += group.width + Space::xl;

    const auto target = total / fit;
    auto width = 0;
    rows.emplace_back();

    for (const auto& row : groups)
    {
        for (const auto& group : row)
        {
            if (! rows.back().empty() && width + group.width > target && (int) rows.size() < fit)
            {
                rows.emplace_back();
                width = 0;
            }

            rows.back().push_back (&group);
            width += group.width + Space::xl;
        }
    }

    return rows;
}

int DevicePanel::getMinimumHeight() const
{
    return 2 * Space::l + rowHeight;
}

int DevicePanel::getWidthForHeight (int height) const
{
    auto widest = 0;

    for (const auto& row : flow (height))
        widest = std::max (widest, getRowWidth (row));

    return 2 * Space::l + widest;
}
} // namespace phazite::ui
