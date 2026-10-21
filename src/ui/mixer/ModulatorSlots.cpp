// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModulatorSlots.h"
#include "StripParts.h"
#include "model/automation/AutomationModel.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int outputMeterWidth = 28;

    // A modulator's output now: a bar from the middle (bipolar) or from the left, faint while it is switched off.
    struct Meter : public juce::Component
    {
        Meter()
        {
            setTitle ("Output");
            setInterceptsMouseClicks (false, false);
        }

        void show (float newValue, bool isBipolar, bool isOn)
        {
            if (! juce::approximatelyEqual (value, newValue) || bipolar != isBipolar || on != isOn)
            {
                value = newValue;
                bipolar = isBipolar;
                on = isOn;
                repaint();
            }
        }

        void paint (juce::Graphics& g) override
        {
            const auto& p = getPalette (*this);
            const auto area = getLocalBounds().toFloat().reduced (1.0f, 6.0f);
            g.setColour (p.surfaceRaised);
            g.fillRoundedRectangle (area, cornerRadius);

            const auto origin = bipolar ? area.getCentreX() : area.getX();
            const auto end = bipolar ? area.getCentreX() + value * area.getWidth() / 2.0f : area.getX() + value * area.getWidth();
            g.setColour (p.modulation.withMultipliedAlpha (on ? 1.0f : ModulationRing::offAlpha));
            g.fillRect (
                juce::Rectangle<float>::leftTopRightBottom (std::min (origin, end), area.getY(), std::max (origin, end), area.getBottom()));
        }

        float value = 0.0f;
        bool bipolar = false, on = true;
    };
} // namespace

//==============================================================================
struct ModulatorSlots::Slot : public juce::Component
{
    Slot (ModulatorSlots& o, const ModulatorInfo& info)
        : owner (o),
          shown (info),
          grip (info.id, "Drag onto a knob of this track's devices, or its volume, pan or sends, to modulate it")
    {
        nameButton.onClicked = [this] (bool isMenuClick)
        {
            if (isMenuClick)
                owner.showSlotMenu (shown);
            else
                owner.openEditor (shown.id);
        };

        nameButton.setButtonText (info.name);
        nameButton.setTitle (info.name);
        nameButton.setTooltip ("Opens " + info.name + "'s editor. Right-click to "
                               + (info.type == ModulatorType::macro ? "remove it" : "switch it off or remove it"));
        addAndMakeVisible (grip);
        addAndMakeVisible (nameButton);

        // A macro is a knob: what it modulates follows it.
        if (info.type == ModulatorType::macro)
        {
            const auto parameters = getModulatorParameters (owner.context, info.id);
            macroKnob = std::make_unique<Knob> (info.name, 0.0);
            macroAttachment = std::make_unique<ParameterAttachment> (*macroKnob);

            if (! parameters.empty())
                macroAttachment->setParameter (getModulatorParameter (owner.context, info.id, parameters.front().id));

            addAndMakeVisible (*macroKnob);
        }
        else
        {
            // Its switch: off, it moves nothing, to hear the sound without it.
            power.setButtonText ("On");
            power.setTitle ("On");
            power.setTooltip ("Turns " + info.name + " on or off, to hear the sound with and without it");
            power.setClickingTogglesState (true);
            power.setToggleState (info.isOn, juce::dontSendNotification);
            power.onClick = [this] { setModulatorOn (owner.context, shown.id, power.getToggleState()); };
            addAndMakeVisible (power);
            addAndMakeVisible (meter);
        }
    }

    void updateValues()
    {
        if (macroAttachment != nullptr)
        {
            macroAttachment->update();
            return;
        }

        const auto on = isModulatorOn (owner.context, shown.id);
        power.setToggleState (on, juce::dontSendNotification);
        meter.show (getModulatorValue (owner.context, shown.id), isBipolar (owner.context, shown.id), on);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        grip.setBounds (area.removeFromLeft (ModulatorGrip::width));
        auto right = area.removeFromRight (outputMeterWidth);

        if (macroKnob != nullptr)
        {
            macroKnob->setBounds (right.reduced (2, 0));
        }
        else
        {
            meter.setBounds (right);
            power.setBounds (area.removeFromRight (slotPowerWidth));
        }

        nameButton.setBounds (area.withTrimmedRight (2));
    }

    ModulatorSlots& owner;
    ModulatorInfo shown;
    ModulatorGrip grip;
    MenuButton nameButton;
    juce::TextButton power; // a modulator's, not a macro's
    Meter meter;
    std::unique_ptr<Knob> macroKnob;
    std::unique_ptr<ParameterAttachment> macroAttachment;
};

//==============================================================================
ModulatorSlots::ModulatorSlots (ProjectContext& projectContext)
    : context (projectContext)
{
    setTitle ("Modulators");
    addButton.setTooltip ("Adds a modulator, which moves the parameters you drag it onto as the song plays (an LFO, an envelope "
                          "follower, a step sequencer, a random source, a breakpoint envelope or a note follower), or a macro, "
                          "a knob that moves several at once");
    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);
}

ModulatorSlots::~ModulatorSlots() = default;

void ModulatorSlots::setTrack (TrackID newTrack)
{
    track = newTrack;
    addButton.setEnabled (track.isValid());
    rebuild (getModulators (context, track));
}

void ModulatorSlots::refresh()
{
    const auto modulators = getModulators (context, track);
    std::vector<ModulatorID> ids;

    for (const auto& modulator : modulators)
        ids.push_back (modulator.id);

    // Added, removed or renamed (their places among their kind): new slots.
    const auto sameNames = std::equal (modulators.begin(), modulators.end(), slots.begin(), slots.end(),
                                       [] (const ModulatorInfo& m, const Slot* slot) { return m.name == slot->shown.name; });

    if (ids != shown || ! sameNames)
        rebuild (modulators);
}

void ModulatorSlots::updateValues()
{
    for (auto* slot : slots)
        slot->updateValues();
}

void ModulatorSlots::rebuild (const std::vector<ModulatorInfo>& modulators)
{
    slots.clear();
    shown.clear();

    for (const auto& modulator : modulators)
    {
        addAndMakeVisible (slots.add (new Slot (*this, modulator)));
        shown.push_back (modulator.id);
    }

    resized();
}

int ModulatorSlots::getPreferredHeight() const
{
    return slots.size() * (slotNameHeight + Space::xs) + buttonHeight;
}

void ModulatorSlots::resized()
{
    auto area = getLocalBounds();

    for (auto* slot : slots)
    {
        slot->setBounds (area.removeFromTop (slotNameHeight));
        area.removeFromTop (Space::xs);
    }

    addButton.setBounds (area.removeFromTop (buttonHeight));
}

void ModulatorSlots::openEditor (ModulatorID id)
{
    if (onOpenEditor != nullptr)
    {
        onOpenEditor (id);
        return;
    }

    for (auto* slot : slots)
    {
        if (slot->shown.id == id)
        {
            juce::CallOutBox::launchAsynchronously (std::make_unique<ModulatorEditor> (context, id), slot->getScreenBounds(), nullptr);
            return;
        }
    }
}

void ModulatorSlots::showAddMenu()
{
    if (! track.isValid())
        return;

    auto& projectContext = context;
    const auto id = track;
    const juce::Component::SafePointer<ModulatorSlots> safeThis (this);
    juce::PopupMenu menu;

    for (const auto type : allModulatorTypes)
    {
        if (type == ModulatorType::macro)
            menu.addSeparator();

        menu.addItem (getModulatorTypeName (type),
                      [&projectContext, id, type, safeThis]
                      {
                          const auto added = addModulator (projectContext, id, type);

                          // A new modulator's editor opens, to set it up; a macro shows its knob in its slot.
                          if (safeThis != nullptr && added.isValid())
                          {
                              safeThis->refresh();

                              if (type != ModulatorType::macro)
                                  safeThis->openEditor (added);
                          }
                      });
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton));
}

void ModulatorSlots::showSlotMenu (const ModulatorInfo& modulator)
{
    auto& projectContext = context;
    const auto id = modulator.id;
    juce::PopupMenu menu;
    menu.addItem ("Open Editor", [this, id] { openEditor (id); });

    // What it modulates, each of which a click stops modulating.
    juce::PopupMenu modulates;

    for (const auto& modulation : getModulations (context, id))
    {
        const auto target = getAutomationTarget (context, modulation.parameter);
        const auto name = target.has_value() ? (target->deviceName == "Mixer" ? target->name : target->deviceName + ": " + target->name)
                                             : juce::String ("Parameter");
        modulates.addItem ("Stop Modulating " + name,
                           [&projectContext, id, parameter = modulation.parameter] { removeModulation (projectContext, id, parameter); });
    }

    menu.addSubMenu ("Modulates", modulates, modulates.getNumItems() > 0);

    if (modulator.type != ModulatorType::macro)
    {
        const auto on = isModulatorOn (context, id);
        menu.addItem ("On", true, on, [&projectContext, id, on] { setModulatorOn (projectContext, id, ! on); });
    }

    menu.addSeparator();
    menu.addItem ("Remove", [&projectContext, id] { removeModulator (projectContext, id); });
    menu.showMenuAsync (juce::PopupMenu::Options());
}
} // namespace phazite::ui
