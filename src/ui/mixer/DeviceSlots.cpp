// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/mixer/Mixer.h"
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
        MixAttachment (juce::Slider& s, te::Plugin& device)
            : slider (s),
              handle (device)
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
    Slot (DeviceSlots& o, te::Plugin& device)
        : owner (o),
          plugin (&device)
    {
        nameButton.setButtonText (device.getName());
        nameButton.setTitle (device.getName());
        nameButton.setTooltip ("Opens " + device.getName() + "'s window. Right-click to turn it off, "
                               + (owner.kind == Kind::instrument ? "replace or remove it" : "move or remove it"));
        nameButton.onClicked = [this] (bool isMenuClick)
        {
            if (auto* d = plugin.get())
            {
                if (isMenuClick)
                    owner.showSlotMenu (*d);
                else
                    d->showWindowExplicitly();
            }
        };

        power.setButtonText ("On");
        power.setTitle ("On");
        power.setTooltip ("Turns the device on or off");
        power.setClickingTogglesState (true);
        power.onClick = [this]
        {
            if (auto* d = plugin.get())
                setDeviceOn (*d, power.getToggleState());
        };

        // An effect's mix first, as FL Studio's mixer slots have it, then its key controls.
        if (owner.kind == Kind::effects && hasMix (device))
        {
            auto* knob = knobs.add (new Knob ("Mix", 1.0));
            knob->textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)) + "% wet"; };
            juce::String tooltip ("Mix: how much of the sound is the effect. At 100% it is the effect alone; lower lets the sound "
                                  "that comes in through as well. Double-click for 100%");

            // Tracktion adds the dry sound back without waiting for the effect, as FL Studio's mix knob does.
            if (const auto latency = device.getLatencySeconds(); latency > 0.0)
                tooltip << ". This effect delays its sound by " << juce::jmax (1, juce::roundToInt (latency * 1000.0))
                        << " ms, so below 100% the two blur together: use the effect's own mix control if it has one";

            knob->setTooltip (tooltip);
            mixAttachment = std::make_unique<MixAttachment> (*knob, device);
            addAndMakeVisible (knob);
        }

        for (const auto& id : getKeyControls (device, keyControlsPerDevice))
            addParameterKnob (*this, knobs, attachments, device, id);

        addAndMakeVisible (nameButton);
        addAndMakeVisible (power);
        update();
    }

    void update()
    {
        if (auto* d = plugin.get())
            power.setToggleState (d->isEnabled(), juce::dontSendNotification);

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
    te::SafeSelectable<te::Plugin> plugin;
    MenuButton nameButton;
    juce::TextButton power;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    std::unique_ptr<MixAttachment> mixAttachment; // an effect's, for its first knob
};

//==============================================================================
DeviceSlots::DeviceSlots (Kind k)
    : kind (k)
{
    setTitle (kind == Kind::instrument ? "Instrument" : "Effects");
    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);
    updateAddButton();
}

DeviceSlots::~DeviceSlots() = default;

void DeviceSlots::setChannel (te::Track* newTrack, te::Edit* newEdit)
{
    track = newTrack;
    edit = newEdit;
    shownDevices.clear();
    rebuild();
}

juce::Array<te::Plugin*> DeviceSlots::getChannelDevices() const
{
    if (auto* t = track.get())
    {
        if (kind == Kind::effects)
            return getEffects (*t);

        if (auto* instrumentDevice = getInstrument (*t))
            return { instrumentDevice };

        return {};
    }

    if (auto* e = edit.get(); e != nullptr && kind == Kind::effects)
        return getMasterDevices (*e);

    return {};
}

void DeviceSlots::update()
{
    // The devices changed (added, removed or moved here or elsewhere, or by undo): new slots.
    if (getIDs (getChannelDevices()) != shownDevices)
    {
        rebuild();
        return;
    }

    for (auto* slot : slots)
        slot->update();

    updateAddButton();
}

void DeviceSlots::rebuild()
{
    slots.clear();
    const auto channelDevices = getChannelDevices();
    shownDevices = getIDs (channelDevices);

    for (auto* device : channelDevices)
        addAndMakeVisible (slots.add (new Slot (*this, *device)));

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
        addButton.setEnabled (track.get() != nullptr || edit.get() != nullptr);
        return;
    }

    // A track has one instrument: the button shows while it has none, and asks for one, tinted, while the track has
    // notes that make no sound without it.
    auto* t = track.get();
    addButton.setVisible (slots.isEmpty());
    addButton.setEnabled (dynamic_cast<te::AudioTrack*> (t) != nullptr);

    if (const auto need = t != nullptr && needsInstrument (*t) ? 1 : 0; need != shownNeed)
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
    {
        if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track.get()))
            showInstrumentMenu (*audioTrack, addButton);
    }
    else if (auto* e = track.get() != nullptr ? &track->edit : edit.get())
    {
        showEffectMenu (*e, track.get(), addButton);
    }
}

void DeviceSlots::showSlotMenu (te::Plugin& device)
{
    // Effects move among the channel's effects, which follow its instrument among all its devices.
    const auto shown = getChannelDevices();
    const auto index = shown.indexOf (&device);
    const auto all = track.get() != nullptr ? getDevices (*track.get()) : (edit.get() != nullptr ? getMasterDevices (*edit.get()) : shown);
    const auto position = all.indexOf (&device);
    const te::SafeSelectable<te::Plugin> safeDevice (&device);
    const juce::Component::SafePointer<DeviceSlots> safeThis (this);

    // Each a model function that is an undo step of its own.
    const auto act = [safeDevice, safeThis] (std::function<void (te::Plugin&)> action)
    {
        return [safeDevice, safeThis, action]
        {
            if (auto* d = safeDevice.get())
                action (*d);

            if (safeThis != nullptr)
                safeThis->update();
        };
    };

    juce::PopupMenu menu;
    menu.addItem ("Open Window",
                  [safeDevice]
                  {
                      if (auto* d = safeDevice.get())
                          d->showWindowExplicitly();
                  });
    menu.addItem ("On", true, device.isEnabled(), act ([] (te::Plugin& d) { setDeviceOn (d, ! d.isEnabled()); }));
    menu.addSeparator();

    if (kind == Kind::instrument)
    {
        menu.addItem ("Replace...",
                      [safeThis]
                      {
                          if (safeThis != nullptr)
                              if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (safeThis->track.get()))
                                  showInstrumentMenu (*audioTrack, *safeThis);
                      });
    }
    else
    {
        menu.addItem ("Move Up", index > 0, false, act ([position] (te::Plugin& d) { moveDevice (d, position - 1); }));
        menu.addItem ("Move Down", index < shown.size() - 1, false, act ([position] (te::Plugin& d) { moveDevice (d, position + 1); }));
    }

    menu.addSeparator();
    menu.addItem ("Remove", act ([] (te::Plugin& d) { removeDevice (d); }));
    menu.showMenuAsync (juce::PopupMenu::Options());
}
} // namespace phazite::ui
