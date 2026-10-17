// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContainerEditor.h"
#include "StripParts.h"
#include "model/mixer/ContainerModel.h"
#include "model/modulation/ModulationModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int macroColumns = 4, macroWidth = 72, macroKnobSize = 44, macroNameHeight = 18;
    constexpr int macroHeight = macroKnobSize + macroNameHeight;
} // namespace

//==============================================================================
// A macro: its knob, its name, and a grip to drag it by onto a knob of the container's devices.
struct ContainerEditor::Macro : public juce::Component
{
    Macro (ContainerEditor& o, const ModulatorInfo& info)
        : owner (o),
          id (info.id),
          knob (info.name, 0.0),
          grip (info.id, "Drag onto a knob of this container's devices: the macro then turns it too")
    {
        if (const auto parameters = getModulatorParameters (owner.context, id); ! parameters.empty())
            attachment.setParameter (getModulatorParameter (owner.context, id, parameters.front().id));

        knob.setTooltip ("Turns what it modulates in the container: drag its grip onto a knob of the container's devices, or "
                         "right-click a knob > Modulate");
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (TextSize::small));
        label.setEditable (false, true, false);
        label.setTooltip ("Double-click to rename it");
        label.onTextChange = [this] { renameMacro (owner.context, id, label.getText()); };
        show (info.name);

        addAndMakeVisible (grip);
        addAndMakeVisible (knob);
        addAndMakeVisible (label);
    }

    void show (const juce::String& macroName)
    {
        if (! label.isBeingEdited())
            label.setText (macroName, juce::dontSendNotification);

        knob.setTitle (macroName);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromBottom (macroNameHeight));
        grip.setBounds (area.removeFromLeft (ModulatorGrip::width));
        knob.setBounds (area.withSizeKeepingCentre (macroKnobSize, macroKnobSize));
    }

    ContainerEditor& owner;
    const ModulatorID id;
    Knob knob;
    ParameterAttachment attachment { knob };
    ModulatorGrip grip;
    juce::Label label;
};

//==============================================================================
ContainerEditor::ContainerEditor (ProjectContext& projectContext, CommandBridge& commands, DeviceID id)
    : context (projectContext),
      container (id),
      devices (DeviceSlots::Kind::containerDevices, projectContext, commands)
{
    setTitle ("Container");
    name.setFont (juce::FontOptions (TextSize::title));
    name.setEditable (false, true, false);
    name.setTooltip ("The container's name: double-click to rename it");
    name.onTextChange = [this] { renameContainer (context, container, name.getText()); };
    addAndMakeVisible (name);

    for (const auto& macro : getContainerMacros (context, container))
        addAndMakeVisible (macros.add (new Macro (*this, macro)));

    devices.setContainer (container);
    addAndMakeVisible (devices);
    showNames();
    updateSize();
    context.addListener (this);
    startTimerHz (10);
}

ContainerEditor::~ContainerEditor()
{
    context.removeListener (this);
}

Knob& ContainerEditor::getMacroKnob (int index)
{
    return macros[index]->knob;
}

juce::Label& ContainerEditor::getMacroName (int index)
{
    return macros[index]->label;
}

juce::Component& ContainerEditor::getMacroGrip (int index)
{
    return macros[index]->grip;
}

void ContainerEditor::showNames()
{
    if (! name.isBeingEdited())
        name.setText (getContainerName (context, container), juce::dontSendNotification);

    const auto infos = getContainerMacros (context, container);

    for (size_t i = 0; i < infos.size() && (int) i < macros.size(); ++i)
        macros[(int) i]->show (infos[i].name);
}

void ContainerEditor::songChanged (const SongChanges& changes)
{
    if (changes.devices)
    {
        devices.refresh();
        updateSize();
    }

    if (changes.devices || changes.modulation)
        showNames();
}

void ContainerEditor::timerCallback()
{
    devices.updateValues();

    for (auto* macro : macros)
        macro->attachment.update();
}

void ContainerEditor::updateSize()
{
    // The macros on the left, in rows of four; the devices in a row on the right, as wide as they need.
    const auto rows = (macros.size() + macroColumns - 1) / macroColumns;
    const auto macrosHeight = rows * macroHeight + std::max (0, rows - 1) * Space::s;
    const auto width =
        2 * Space::l + macroColumns * macroWidth + Space::l + std::max (devices.getPreferredWidth(), DeviceSlots::rowSlotWidth);
    const auto height = 2 * Space::l + controlHeight + Space::s + std::max (macrosHeight, devices.getPreferredHeight());
    setSize (width, height);
}

void ContainerEditor::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    // The macros' panel, behind them.
    if (! macros.isEmpty())
    {
        g.setColour (p.surfaceRaised);
        g.fillRoundedRectangle (macros.getFirst()->getBounds().getUnion (macros.getLast()->getBounds()).expanded (Space::xs).toFloat(),
                                cornerRadius);
    }
}

void ContainerEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::l);
    name.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);

    const auto left = area.removeFromLeft (macroColumns * macroWidth);
    area.removeFromLeft (Space::l);

    for (int i = 0; i < macros.size(); ++i)
        macros[i]->setBounds (left.getX() + (i % macroColumns) * macroWidth, left.getY() + (i / macroColumns) * (macroHeight + Space::s),
                              macroWidth, macroHeight);

    devices.setBounds (area.removeFromTop (devices.getPreferredHeight()));
}
} // namespace phazite::ui
