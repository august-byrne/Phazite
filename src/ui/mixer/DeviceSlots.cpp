// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/arrangement/SongSelection.h"
#include "model/automation/AutomationModel.h"
#include "model/mixer/ContainerModel.h"
#include "model/project/ProjectContext.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int rowAddButtonWidth = 90; // a container's add button, at the end of its row

    // Links a knob to an effect's mix through the model's MixHandle, as ParameterAttachment links one to a parameter: one
    // named undo step per drag or key press.
    class MixAttachment
    {
    public:
        MixAttachment (juce::Slider& s, MixHandle mix)
            : slider (s),
              handle (std::move (mix))
        {
            slider.setRange (0.0, 1.0);
            slider.setDoubleClickReturnValue (true, 1.0);
            slider.onDragStart = [this]
            {
                dragging = true;
                handle.beginGesture();
            };

            slider.onDragEnd = [this]
            {
                dragging = false;
                handle.endGesture();
            };

            slider.onValueChange = [this] { handle.setValue ((float) slider.getValue()); }; // a key press: a step of its own
            update();
        }

        void update()
        {
            if (handle.isValid() && ! dragging)
                slider.setValue (handle.getValue(), juce::dontSendNotification);
        }

    private:
        juce::Slider& slider;
        MixHandle handle;
        bool dragging = false;
    };
} // namespace

//==============================================================================
// A device: its name, which opens its window (right-click for more), an on/off switch, and its key controls.
struct DeviceSlots::Slot : public juce::Component
{
    Slot (DeviceSlots& o, const DeviceSlot& device)
        : owner (o),
          shown (device)
    {
        nameButton.onClickedWith = [this] (const juce::ModifierKeys& mods)
        {
            if (mods.isPopupMenu())
                owner.showSlotMenu (shown);
            else if (mods.isCommandDown() || mods.isShiftDown())
                owner.select (shown, mods);
            else
                showDeviceWindow (owner.context, shown.id);
        };

        power.setButtonText ("On");
        power.setTitle ("On");
        power.setTooltip ("Turns the device on or off");
        power.setClickingTogglesState (true);
        power.onClick = [this] { setDeviceOn (owner.context, shown.id, power.getToggleState()); };

        // An effect's mix first, as FL Studio's mixer slots have it, then its key controls (a container's: its macros).
        if (owner.kind != Kind::instrument && device.hasMix)
        {
            auto* knob = knobs.add (new Knob ("Mix", 1.0));
            knob->textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)) + "% wet"; };
            juce::String tooltip ("Mix: how much of the sound is the effect. At 100% it is the effect alone; lower lets the sound "
                                  "that comes in through as well. Double-click for 100%");

            // Tracktion adds the dry sound back without waiting for the effect, as FL Studio's mix knob does.
            if (device.latencySeconds > 0.0)
                tooltip << ". This effect delays its sound by " << juce::jmax (1, juce::roundToInt (device.latencySeconds * 1000.0))
                        << " ms, so below 100% the two blur together: use the effect's own mix control if it has one";

            knob->setTooltip (tooltip);
            mixAttachment = std::make_unique<MixAttachment> (*knob, getMix (owner.context, device.id));
            addAndMakeVisible (knob);
        }

        for (auto& control : getKeyControls (owner.context, device.id, keyControlsPerDevice))
            addParameterKnob (*this, knobs, attachments, std::move (control));

        addAndMakeVisible (nameButton);
        addAndMakeVisible (power);
        show (device);
    }

    // Its name and switch, as they are now.
    void show (const DeviceSlot& device)
    {
        shown = device;
        nameButton.setButtonText (device.name);
        nameButton.setTitle (device.name);

        const auto what = device.isContainer ? juce::String ("its devices and macros") : device.name + "'s window";
        const auto more =
            owner.kind == Kind::instrument ? juce::String ("replace or remove it") : juce::String ("move, group or remove it");
        nameButton.setTooltip ("Opens " + what + ". Ctrl-click to select it with others; right-click to turn it off, " + more);
        power.setToggleState (device.isOn, juce::dontSendNotification);
        repaint();
    }

    void updateValues()
    {
        for (auto* attachment : attachments)
            attachment->update();

        if (mixAttachment != nullptr)
            mixAttachment->update();
    }

    // A selected device, behind its controls.
    void paint (juce::Graphics& g) override
    {
        if (owner.context.getSongSelection().isSelected (shown.id))
        {
            g.setColour (getPalette (*this).accent.withAlpha (selectionWash));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (slotNameHeight);
        power.setBounds (top.removeFromRight (32));
        nameButton.setBounds (top.withTrimmedRight (2));

        if (knobs.isEmpty())
            return;

        const auto knobWidth = area.getWidth() / knobs.size();

        for (auto* knob : knobs)
            knob->setBounds (area.removeFromLeft (knobWidth).reduced (2, 1));
    }

    DeviceSlots& owner;
    DeviceSlot shown;
    MenuButton nameButton;
    juce::TextButton power;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    std::unique_ptr<MixAttachment> mixAttachment; // an effect's, for its first knob
};

//==============================================================================
DeviceSlots::DeviceSlots (Kind k, ProjectContext& projectContext, CommandBridge& bridge)
    : kind (k),
      context (projectContext),
      commands (bridge)
{
    setTitle (kind == Kind::instrument ? "Instrument" : kind == Kind::effects ? "Effects" : "Devices");
    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);
    updateAddButton();
    context.getSongSelection().addChangeListener (this);
}

DeviceSlots::~DeviceSlots()
{
    context.getSongSelection().removeChangeListener (this);
}

void DeviceSlots::setTrack (TrackID newTrack)
{
    track = newTrack;
    master = false;
    rebuild (getChannelDevices());
}

void DeviceSlots::setMaster()
{
    track = {};
    master = true;
    rebuild (getChannelDevices());
}

void DeviceSlots::setContainer (DeviceID newContainer)
{
    container = newContainer;
    rebuild (getChannelDevices());
}

std::vector<DeviceSlot> DeviceSlots::getChannelDevices() const
{
    if (kind == Kind::containerDevices)
        return getContainerDevices (context, container);

    if (! master && ! track.isValid())
        return {};

    if (kind == Kind::effects)
        return getEffects (context, getEffectsChannel());

    if (const auto instrument = getInstrument (context, track))
        return { *instrument };

    return {};
}

void DeviceSlots::refresh()
{
    // The devices changed (added, removed or moved here or elsewhere, or by undo): new slots.
    const auto devices = getChannelDevices();

    if (getIDs (devices) != shownDevices)
    {
        rebuild (devices);
        return;
    }

    for (size_t i = 0; i < devices.size(); ++i)
        slots[(int) i]->show (devices[i]);

    updateAddButton();
}

void DeviceSlots::refreshNeed()
{
    if (kind == Kind::instrument && slots.isEmpty())
        updateAddButton();
}

void DeviceSlots::updateValues()
{
    for (auto* slot : slots)
        slot->updateValues();
}

void DeviceSlots::rebuild (const std::vector<DeviceSlot>& devices)
{
    slots.clear();
    shownDevices = getIDs (devices);

    for (const auto& device : devices)
        addAndMakeVisible (slots.add (new Slot (*this, device)));

    shownNeed = -1;
    updateAddButton();
    resized();
}

void DeviceSlots::updateAddButton()
{
    if (kind != Kind::instrument)
    {
        addButton.setButtonText ("+ Effect");
        addButton.setTooltip (kind == Kind::containerDevices
                                  ? "Adds an effect after the container's devices: built in or a plug-in"
                                  : "Adds an effect, which changes the sound, such as a reverb: built in or a plug-in");
        addButton.setEnabled (kind == Kind::containerDevices ? container.isValid() : master ? context.hasProject() : track.isValid());
        return;
    }

    // A track has one instrument: the button shows while it has none, and asks for one, tinted, while the track has
    // notes that make no sound without it.
    const auto channel = getChannel (context, track);
    addButton.setVisible (slots.isEmpty());
    addButton.setEnabled (canHoldInstrument (context, track));

    if (const auto need = channel.has_value() && channel->needsInstrument ? 1 : 0; need != shownNeed)
    {
        shownNeed = need;
        const auto& p = getPalette (*this);
        addButton.setButtonText (need == 1 ? "Choose instrument" : "+ Instrument");
        addButton.setTooltip (need == 1 ? "This track has notes, but no instrument to play them, so they make no sound. Choose "
                                          "one: a synth or a sampler"
                                        : "Adds an instrument, which plays the track's notes: a synth or a sampler, built in or a plug-in");
        addButton.setColour (juce::TextButton::buttonColourId,
                             need == 1 ? p.surfaceRaised.interpolatedWith (p.warning, attentionTint) : p.surfaceRaised);
    }
}

void DeviceSlots::lookAndFeelChanged()
{
    shownNeed = -1; // the theme's colors
    updateAddButton();
}

int DeviceSlots::getPreferredHeight() const
{
    if (kind == Kind::containerDevices)
        return slotNameHeight + slotKnobHeight;

    auto height = addButton.isVisible() ? buttonHeight : 0;

    for (auto* slot : slots)
        height += slotNameHeight + (slot->knobs.isEmpty() ? 0 : slotKnobHeight) + Space::xs;

    return height;
}

int DeviceSlots::getPreferredWidth() const
{
    return slots.size() * (rowSlotWidth + Space::xs) + rowAddButtonWidth;
}

void DeviceSlots::resized()
{
    auto area = getLocalBounds();

    // A container's devices: in a row, the add button after them.
    if (kind == Kind::containerDevices)
    {
        for (auto* slot : slots)
        {
            slot->setBounds (area.removeFromLeft (rowSlotWidth));
            area.removeFromLeft (Space::xs);
        }

        addButton.setBounds (area.removeFromLeft (rowAddButtonWidth).removeFromTop (buttonHeight));
        return;
    }

    for (auto* slot : slots)
    {
        slot->setBounds (area.removeFromTop (slotNameHeight + (slot->knobs.isEmpty() ? 0 : slotKnobHeight)));
        area.removeFromTop (Space::xs);
    }

    addButton.setBounds (area.removeFromTop (buttonHeight));
}

void DeviceSlots::showAddMenu()
{
    if (kind == Kind::instrument)
        showInstrumentMenu (context, track, addButton);
    else if (kind == Kind::containerDevices)
        showContainerEffectMenu (context, container, addButton);
    else if (master || track.isValid())
        showEffectMenu (context, getEffectsChannel(), addButton);
}

void DeviceSlots::select (const DeviceSlot& device, const juce::ModifierKeys& mods)
{
    auto& selection = context.getSongSelection();

    if (mods.isCommandDown() && selection.isSelected (device.id))
        selection.remove (device.id);
    else
        selection.add (device.id);
}

void DeviceSlots::changeListenerCallback (juce::ChangeBroadcaster*)
{
    for (auto* slot : slots)
        slot->repaint();
}

void DeviceSlots::showSlotMenu (const DeviceSlot& device)
{
    // The menu acts on the selected devices: this one alone, unless it is one of them.
    if (! context.getSongSelection().isSelected (device.id))
        context.getSongSelection().selectOnly (device.id);

    // Effects move among the channel's effects, or the container's devices.
    const auto index = (int) (std::find (shownDevices.begin(), shownDevices.end(), device.id) - shownDevices.begin());
    const auto id = device.id;
    const juce::Component::SafePointer<DeviceSlots> safeThis (this);

    // Each a model function that is an undo step of its own; the slots show what it did at once.
    const auto act = [safeThis] (std::function<void()> action)
    {
        return [safeThis, action]
        {
            action();

            if (safeThis != nullptr)
                safeThis->refresh();
        };
    };

    auto& projectContext = context;
    juce::PopupMenu menu;
    menu.addItem ("Open Window", [&projectContext, id] { showDeviceWindow (projectContext, id); });
    menu.addItem ("On", true, device.isOn, act ([&projectContext, id, on = device.isOn] { setDeviceOn (projectContext, id, ! on); }));

    // Its parameters, each of which shows its automation lane under the track in the playlist (those with automation
    // ticked), or makes an automation clip for it. A plug-in's own window can't offer this: Automate Last Touched and
    // Create Automation Clip do, for the knob moved last.
    juce::PopupMenu automate, createClip;

    for (const auto& target : getAutomationTargets (projectContext, id))
    {
        automate.addItem (
            juce::PopupMenu::Item (target.name)
                .setTicked (target.automated)
                .setAction ([&projectContext, parameter = target.parameter] { showAutomationLane (projectContext, parameter); }));
        createClip.addItem (target.name, [&projectContext, parameter = target.parameter]
                            { createAutomationClip (getParameter (projectContext, parameter)); });
    }

    menu.addSubMenu ("Automate", automate, automate.getNumItems() > 0);
    menu.addSubMenu ("Create Automation Clip", createClip, createClip.getNumItems() > 0);
    menu.addSeparator();

    if (kind == Kind::instrument)
    {
        menu.addItem ("Replace...",
                      [safeThis]
                      {
                          if (safeThis != nullptr)
                              showInstrumentMenu (safeThis->context, safeThis->track, *safeThis);
                      });
    }
    else
    {
        const auto row = kind == Kind::containerDevices;
        menu.addItem (row ? "Move Left" : "Move Up", index > 0, false,
                      act ([&projectContext, id] { moveEffect (projectContext, id, -1); }));
        menu.addItem (row ? "Move Right" : "Move Down", index < (int) shownDevices.size() - 1, false,
                      act ([&projectContext, id] { moveEffect (projectContext, id, 1); }));
    }

    // The selected devices' commands: a container of the effects, or its devices back; and a preset of this one.
    menu.addSeparator();

    if (kind == Kind::effects)
        addCommandItem (menu, commands, device.isContainer ? "device.ungroup" : "device.group");

    addCommandItem (menu, commands, "device.savePreset");
    menu.addSeparator();
    menu.addItem ("Remove", act ([&projectContext, id] { removeDevice (projectContext, id); }));
    menu.showMenuAsync (juce::PopupMenu::Options());
}
} // namespace phazite::ui
