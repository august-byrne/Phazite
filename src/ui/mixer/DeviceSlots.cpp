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

//==============================================================================
// A device: its name, which shows it in the Devices view (right-click for more), an on/off switch, and its key controls.
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
                showDevice (owner.context, owner.commands, shown);
        };

        power.setButtonText ("On");
        power.setTitle ("On");
        power.setTooltip ("Turns the device on or off");
        power.setClickingTogglesState (true);
        power.onClick = [this] { setDeviceOn (owner.context, shown.id, power.getToggleState()); };

        // An effect's mix first, as FL Studio's mixer slots have it, then its key controls (a container's: its macros).
        if (owner.kind != Kind::instrument && device.hasMix)
            addMixKnob (*this, knobs, mixAttachment, owner.context, device);

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

        const auto what = device.opensWindow ? juce::String ("it in the Devices view, and its own window")
                                             : juce::String ("it whole in the Devices view, under the mixer");
        const auto more =
            owner.kind == Kind::instrument ? juce::String ("replace or remove it") : juce::String ("move, group or remove it");
        nameButton.setTooltip (device.frozen
                                   ? "Frozen with its track: unfreeze the track (its snowflake) to change it"
                                   : "Shows " + what + ". Ctrl-click to select it with others; right-click to turn it off, " + more);
        power.setToggleState (device.isOn, juce::dontSendNotification);

        // A frozen device's sound is in its track's file: its controls are off until the track is unfrozen.
        power.setEnabled (! device.frozen);

        for (auto* knob : knobs)
        {
            knob->setEnabled (! device.frozen);
            knob->setAlpha (device.frozen ? FrozenTint::frozenDevice : 1.0f);
        }

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
        power.setBounds (top.removeFromRight (slotPowerWidth));
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
    setTitle (kind == Kind::instrument ? "Instrument" : "Effects");
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
    const auto index = (int) (std::find (shownDevices.begin(), shownDevices.end(), device.id) - shownDevices.begin());
    const juce::Component::SafePointer<DeviceSlots> safeThis (this);

    showDeviceMenu (context, commands, device, kind == Kind::instrument ? DevicePlace::instrument : DevicePlace::effect, track, index,
                    (int) shownDevices.size(), false, *this,
                    [safeThis]
                    {
                        if (safeThis != nullptr)
                            safeThis->refresh();
                    });
}

//==============================================================================
void showDeviceMenu (ProjectContext& context, CommandBridge& commands, const DeviceSlot& device, DevicePlace place, TrackID track,
                     int index, int count, bool inRow, juce::Component& target, std::function<void()> changed)
{
    // The menu acts on the selected devices: this one alone, unless it is one of them.
    if (! context.getSongSelection().isSelected (device.id))
        context.getSongSelection().selectOnly (device.id);

    const auto id = device.id;
    const juce::Component::SafePointer<juce::Component> safeTarget (&target);

    // Each a model function that is an undo step of its own; what shows the device shows what it did at once.
    const auto act = [changed] (std::function<void()> action)
    {
        return [changed, action]
        {
            action();

            if (changed != nullptr)
                changed();
        };
    };

    auto& projectContext = context;
    juce::PopupMenu menu;

    if (device.opensWindow)
        menu.addItem ("Open Window", [&projectContext, id] { showDeviceWindow (projectContext, id); });

    menu.addItem ("On", true, device.isOn, act ([&projectContext, id, on = device.isOn] { setDeviceOn (projectContext, id, ! on); }));

    // Its parameters, each of which shows its automation lane under the track in the playlist (those with automation
    // ticked). A plug-in's own window can't offer this: Automate Last Touched does, for the knob moved last.
    juce::PopupMenu automate;

    for (const auto& automated : getAutomationTargets (projectContext, id))
        automate.addItem (
            juce::PopupMenu::Item (automated.name)
                .setTicked (automated.automated)
                .setAction ([&projectContext, parameter = automated.parameter] { showAutomationLane (projectContext, parameter); }));

    menu.addSubMenu ("Automate", automate, automate.getNumItems() > 0);

    // A device with a sidechain (a compressor that ducks under a kick, a plug-in with a sidechain input): what it listens
    // to, the sound passing through it or another track's.
    if (hasSidechain (projectContext, id))
    {
        juce::PopupMenu sidechain;
        const auto source = getSidechainSource (projectContext, id);
        sidechain.addItem ("Off: the sound passing through it", true, ! source.isValid(),
                           act ([&projectContext, id] { setSidechainSource (projectContext, id, {}); }));
        sidechain.addSeparator();

        for (const auto& choice : getSidechainChoices (projectContext, id))
            sidechain.addItem (choice.name, true, choice.track == source,
                               act ([&projectContext, id, from = choice.track] { setSidechainSource (projectContext, id, from); }));

        menu.addSubMenu ("Sidechain", sidechain);
    }

    menu.addSeparator();

    if (place == DevicePlace::instrument)
    {
        menu.addItem ("Replace...",
                      [&projectContext, track, safeTarget]
                      {
                          if (safeTarget != nullptr)
                              showInstrumentMenu (projectContext, track, *safeTarget);
                      });
    }
    else
    {
        // Effects move among the channel's effects, or the container's devices; the Devices view shows them in a row.
        menu.addItem (inRow ? "Move Left" : "Move Up", index > 0, false,
                      act ([&projectContext, id] { moveEffect (projectContext, id, -1); }));
        menu.addItem (inRow ? "Move Right" : "Move Down", index < count - 1, false,
                      act ([&projectContext, id] { moveEffect (projectContext, id, 1); }));
    }

    // The selected devices' commands: a container of the effects, or its devices back; and a preset of this one.
    menu.addSeparator();

    if (place == DevicePlace::effect)
        addCommandItem (menu, commands, device.isContainer ? "device.ungroup" : "device.group");

    addCommandItem (menu, commands, "device.savePreset");
    menu.addSeparator();
    menu.addItem ("Remove", act ([&projectContext, id] { removeDevice (projectContext, id); }));
    menu.showMenuAsync (juce::PopupMenu::Options());
}
} // namespace phazite::ui
