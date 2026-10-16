// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/project/ProjectContext.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
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
        nameButton.onClicked = [this] (bool isMenuClick)
        {
            if (isMenuClick)
                owner.showSlotMenu (shown);
            else
                showDeviceWindow (owner.context, shown.id);
        };

        power.setButtonText ("On");
        power.setTitle ("On");
        power.setTooltip ("Turns the device on or off");
        power.setClickingTogglesState (true);
        power.onClick = [this] { setDeviceOn (owner.context, shown.id, power.getToggleState()); };

        // An effect's mix first, as FL Studio's mixer slots have it, then its key controls.
        if (owner.kind == Kind::effects && device.hasMix)
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
        nameButton.setTooltip ("Opens " + device.name + "'s window. Right-click to turn it off, "
                               + (owner.kind == Kind::instrument ? "replace or remove it" : "move or remove it"));
        power.setToggleState (device.isOn, juce::dontSendNotification);
    }

    void updateValues()
    {
        for (auto* attachment : attachments)
            attachment->update();

        if (mixAttachment != nullptr)
            mixAttachment->update();
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
DeviceSlots::DeviceSlots (Kind k, ProjectContext& projectContext)
    : kind (k),
      context (projectContext)
{
    setTitle (kind == Kind::instrument ? "Instrument" : "Effects");
    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);
    updateAddButton();
}

DeviceSlots::~DeviceSlots() = default;

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

std::vector<DeviceSlot> DeviceSlots::getChannelDevices() const
{
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
    if (kind == Kind::effects)
    {
        addButton.setButtonText ("+ Effect");
        addButton.setTooltip ("Adds an effect, which changes the sound, such as a reverb: built in or a plug-in");
        addButton.setEnabled (master ? context.hasProject() : track.isValid());
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
    auto height = addButton.isVisible() ? buttonHeight : 0;

    for (auto* slot : slots)
        height += slotNameHeight + (slot->knobs.isEmpty() ? 0 : slotKnobHeight) + Space::xs;

    return height;
}

void DeviceSlots::resized()
{
    auto area = getLocalBounds();

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
    else if (master || track.isValid())
        showEffectMenu (context, getEffectsChannel(), addButton);
}

void DeviceSlots::showSlotMenu (const DeviceSlot& device)
{
    // Effects move among the channel's effects.
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
        menu.addItem ("Move Up", index > 0, false, act ([&projectContext, id] { moveEffect (projectContext, id, -1); }));
        menu.addItem ("Move Down", index < (int) shownDevices.size() - 1, false,
                      act ([&projectContext, id] { moveEffect (projectContext, id, 1); }));
    }

    menu.addSeparator();
    menu.addItem ("Remove", act ([&projectContext, id] { removeDevice (projectContext, id); }));
    menu.showMenuAsync (juce::PopupMenu::Options());
}
} // namespace phazite::ui
