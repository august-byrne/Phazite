// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelStrip.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LiveInput.h"
#include "model/engine/LoudnessMeterPlugin.h"
#include "model/mixer/Mixer.h"
#include "model/project/ProjectContext.h"
#include "services/Library.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int slotNameHeight = 22, slotKnobHeight = 30, sendHeight = 26, buttonHeight = 24, headingHeight = 16;
    constexpr int keyControlsPerDevice = 2;
    constexpr int panKnobSize = 44, meterWidth = 10, headerHeight = 46, colourBarHeight = 4, outputHeight = 18;
    constexpr int minFaderHeight = 72, minSlotsHeight = headingHeight + buttonHeight;
    constexpr int loudnessHeight = 4 * 16 + buttonHeight + Space::xs; // the master's readout: four lines and the Reset button
    constexpr float relatedOutlineAlpha = 0.5f;                       // a channel the selected one feeds, or is fed by

    // The fader's height: half of what there is, at least the minimum, but leaving the rest of the strip what it needs.
    int getFaderHeight (int available, int forTheRest)
    {
        return juce::jlimit (minFaderHeight, std::max (minFaderHeight, available - forTheRest), std::max (120, available / 2));
    }

    // A button that tells a right-click (which opens a menu) from a click.
    struct MenuButton : public juce::TextButton
    {
        using juce::TextButton::clicked;
        std::function<void (bool isMenuClick)> onClicked;

        void clicked (const juce::ModifierKeys& mods) override
        {
            if (onClicked != nullptr)
                onClicked (mods.isPopupMenu());
        }
    };

    // What a channel's slots show, so that a timer can tell when it changes.
    template <typename Item>
    std::vector<te::EditItemID> getIDs (const juce::Array<Item*>& items)
    {
        std::vector<te::EditItemID> ids;

        for (auto* item : items)
            ids.push_back (item->itemID);

        return ids;
    }

    // A knob for a parameter of a plug-in, attached to it.
    Knob* addParameterKnob (juce::Component& parent, juce::OwnedArray<Knob>& knobs, juce::OwnedArray<ParameterAttachment>& attachments,
                            te::Plugin& plugin, const juce::String& parameterID)
    {
        auto parameter = plugin.getAutomatableParameterByID (parameterID);

        if (parameter == nullptr)
            return nullptr;

        auto* knob = knobs.add (new Knob (parameter->getParameterName(), parameter->getDefaultValue().value_or (0.0f)));
        knob->textFromValueFunction = [parameter] (double value) { return parameter->valueToString ((float) value); };
        knob->setTooltip (parameter->getParameterName());
        attachments.add (new ParameterAttachment (*knob, "Change " + parameter->getParameterName()))->setParameter (&plugin, parameterID);
        parent.addAndMakeVisible (knob);
        return knob;
    }

    // Links a knob to an effect's mix (phazite::setMix()), as ParameterAttachment links one to a parameter: one named undo
    // step per drag or key press.
    class MixAttachment
    {
    public:
        MixAttachment (juce::Slider& s, te::Plugin& device)
            : slider (s),
              plugin (&device)
        {
            slider.setRange (0.0, 1.0);
            slider.setDoubleClickReturnValue (true, 1.0);

            slider.onDragStart = [this]
            {
                dragging = true;
                beginUndoStep();
            };

            slider.onDragEnd = [this] { dragging = false; };

            slider.onValueChange = [this]
            {
                if (auto* d = plugin.get())
                {
                    if (! dragging)
                        beginUndoStep(); // a key press or a double-click: one undo step each

                    setMix (*d, (float) slider.getValue());
                }
            };

            update();
        }

        void update()
        {
            if (auto* d = plugin.get(); d != nullptr && ! dragging)
                slider.setValue (getMix (*d), juce::dontSendNotification);
        }

    private:
        juce::Slider& slider;
        te::SafeSelectable<te::Plugin> plugin;
        bool dragging = false;

        void beginUndoStep()
        {
            if (auto* d = plugin.get())
            {
                // As ParameterAttachment does, the levels are stored first, without an undo step, so that undoing the
                // first change plays the old mix.
                for (auto* parameter : getMixParameters (*d))
                    parameter->setParameter (parameter->getCurrentValue(), juce::sendNotification);

                d->edit.getUndoManager().beginNewTransaction ("Change Mix");
            }
        }
    };

    // The tracks that send to a return's bus.
    juce::StringArray getSenderNames (const te::Edit& edit, int bus)
    {
        juce::StringArray names;

        for (auto* track : te::getAudioTracks (edit))
            for (auto* send : getSends (*track))
                if (send->getBusNumber() == bus)
                    names.addIfNotAlreadyThere (track->getName());

        return names;
    }
} // namespace

//==============================================================================
StripHeading::StripHeading (const juce::String& headingText)
    : text (headingText)
{
    setAccessible (false);
    setInterceptsMouseClicks (false, false);
}

void StripHeading::paint (juce::Graphics& g)
{
    g.setColour (getPalette (*this).textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (text, getLocalBounds(), juce::Justification::centredLeft, true);
}

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
            {
                d->edit.getUndoManager().beginNewTransaction (power.getToggleState() ? "Turn Device On" : "Turn Device Off");
                d->setEnabled (power.getToggleState());
            }
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

    const auto act = [safeDevice, safeThis] (const juce::String& undoName, std::function<void (te::Plugin&)> action)
    {
        return [safeDevice, safeThis, undoName, action]
        {
            if (auto* d = safeDevice.get())
            {
                d->edit.getUndoManager().beginNewTransaction (undoName);
                action (*d);
            }

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
    menu.addItem ("On", true, device.isEnabled(),
                  act (device.isEnabled() ? "Turn Device Off" : "Turn Device On", [] (te::Plugin& d) { d.setEnabled (! d.isEnabled()); }));
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
        menu.addItem ("Move Up", index > 0, false, act ("Move Device", [position] (te::Plugin& d) { moveDevice (d, position - 1); }));
        menu.addItem ("Move Down", index < shown.size() - 1, false,
                      act ("Move Device", [position] (te::Plugin& d) { moveDevice (d, position + 1); }));
    }

    menu.addSeparator();
    menu.addItem ("Remove", act ("Remove " + device.getName(), [] (te::Plugin& d) { removeDevice (d); }));
    menu.showMenuAsync (juce::PopupMenu::Options());
}

//==============================================================================
// A send: the return it feeds, its level, and whether it takes the sound before or after the fader.
struct ChannelStrip::SendSlot : public juce::Component
{
    explicit SendSlot (te::AuxSendPlugin& s)
        : send (&s)
    {
        nameButton.setTooltip ("A send to this return track. Right-click to remove it");
        nameButton.onClicked = [this] (bool isMenuClick)
        {
            if (isMenuClick)
                showMenu();
        };

        level.setTooltip ("How much of this track the send feeds the return. Double-click for 0 dB");
        showAsDecibels (level);
        attachment.setParameter (&s, s.gain->paramID);

        prePost.setTooltip ("Pre: the send takes the sound before the fader, so the fader doesn't change it. Post: after the fader");
        prePost.onClick = [this]
        {
            if (auto* aux = send.get())
            {
                aux->edit.getUndoManager().beginNewTransaction ("Change Send");
                setPreFader (*aux, ! isPreFader (*aux));
            }

            update();
        };

        for (auto* c : std::initializer_list<juce::Component*> { &nameButton, &level, &prePost })
            addAndMakeVisible (c);

        update();
    }

    void update()
    {
        auto* aux = send.get();

        if (aux == nullptr)
            return;

        const auto* target = findReturnTrack (aux->edit, aux->getBusNumber());
        const auto name = target != nullptr ? target->getName() : aux->getBusName();
        nameButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + name);
        nameButton.setTitle ("Send to " + name);
        prePost.setButtonText (isPreFader (*aux) ? "Pre" : "Post");
        attachment.update();
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addItem ("Remove Send",
                      [safeSend = te::SafeSelectable<te::AuxSendPlugin> (send.get())]
                      {
                          if (auto* aux = safeSend.get())
                          {
                              aux->edit.getUndoManager().beginNewTransaction ("Remove Send");
                              aux->deleteFromParent();
                          }
                      });
        menu.showMenuAsync (juce::PopupMenu::Options());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        prePost.setBounds (area.removeFromRight (36));
        level.setBounds (area.removeFromRight (sendHeight));
        nameButton.setBounds (area.withTrimmedRight (2));
    }

    te::SafeSelectable<te::AuxSendPlugin> send;
    MenuButton nameButton;
    Knob level { "Send level", te::decibelsToVolumeFaderPosition (0.0f) };
    ParameterAttachment attachment { level, "Change Send Level" };
    juce::TextButton prePost;
};

//==============================================================================
ChannelStrip::ChannelStrip (te::Engine& e, CommandBridge& bridge, te::SelectionManager& selectionManager)
    : engine (e),
      commands (bridge),
      selection (selectionManager)
{
    setTitle ("Channel");

    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    nameLabel.setInterceptsMouseClicks (false, false); // a click on the name selects the track

    for (auto* label : { &inputLabel, &outputLabel })
    {
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        label->setMinimumHorizontalScale (0.8f);
        label->setInterceptsMouseClicks (false, false);
    }

    outputLabel.setTitle ("Output");

    busButton.setTitle ("Mix as a bus");
    busButton.onClick = [this]
    {
        if (auto* folder = dynamic_cast<te::FolderTrack*> (track.get()))
        {
            const auto bus = ! isBus (*folder);
            folder->edit.getUndoManager().beginNewTransaction (bus ? "Mix Folder as Bus" : "Make Folder Plain");
            setMixedAsBus (*folder, bus);
        }

        timerCallback();
    };

    showAsDecibels (volume);
    showAsPan (pan);
    volume.setTooltip ("The track's volume. Double-click for 0 dB; drag with Shift held for fine steps");
    pan.setTooltip ("Where the track sits between the left and right speakers. Double-click to center it");

    addSendButton.setTooltip ("Sends some of this track to a return track, which holds an effect that several tracks share, "
                              "such as a reverb");
    addSendButton.onClick = [this] { showSendMenu(); };

    slotView.setViewedComponent (&slotHolder, false);
    slotView.setScrollBarsShown (true, false);
    slotView.setScrollBarThickness (6);

    for (auto* c : std::initializer_list<juce::Component*> { &instrumentHeading, &instrument, &effectsHeading, &effects, &sendsHeading,
                                                             &addSendButton })
        slotHolder.addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &inputLabel, &busButton, &slotView, &outputLabel, &meter, &volume,
                                                             &pan, &muteSolo })
        addAndMakeVisible (c);

    setTrack (nullptr);
    startTimerHz (8);
}

ChannelStrip::~ChannelStrip()
{
    sendSlots.clear(); // before slotHolder, which holds them
}

void ChannelStrip::setTrack (te::Track* newTrack)
{
    track = newTrack;
    instrument.setChannel (newTrack, nullptr);
    effects.setChannel (newTrack, nullptr);
    muteSolo.setTrack (newTrack);
    shownSends.clear();
    rebuildSends();
    attachControls();
    timerCallback();
    resized(); // a folder's header has the bus switch
}

void ChannelStrip::setSelected (bool isSelected)
{
    if (selected != isSelected)
    {
        selected = isSelected;
        repaint();
    }
}

void ChannelStrip::setRelated (bool isRelated)
{
    if (related != isRelated)
    {
        related = isRelated;
        repaint();
    }
}

void ChannelStrip::attachControls()
{
    auto* t = track.get();
    auto* volumeDevice = t != nullptr ? getVolumeDevice (*t) : nullptr;
    volumeAttachment.setParameter (volumeDevice, t != nullptr ? getVolumeParameterID (*t) : juce::String());

    // A plain folder has no pan of its own: its VCA only scales its tracks.
    auto* panDevice = dynamic_cast<te::VolumeAndPanPlugin*> (volumeDevice);
    panAttachment.setParameter (panDevice, panDevice != nullptr ? panDevice->panParam->paramID : juce::String());
    meter.setSource (t != nullptr ? getLevelMeasurer (*t) : nullptr);

    // Instruments go on tracks: folders and returns take effects only. Sends come from tracks.
    auto* audioTrack = dynamic_cast<te::AudioTrack*> (t);
    const auto canHoldInstrument = audioTrack != nullptr && getReturnBus (*audioTrack) < 0;
    instrumentHeading.setVisible (canHoldInstrument);
    instrument.setVisible (canHoldInstrument);
    sendsHeading.setVisible (audioTrack != nullptr);
    addSendButton.setVisible (audioTrack != nullptr);

    // A folder's bus switch. A bus with effects stays one: making it plain would take them away.
    auto* folder = dynamic_cast<te::FolderTrack*> (t);
    const auto bus = folder != nullptr && isBus (*folder), hasEffects = folder != nullptr && ! getEffects (*folder).isEmpty();
    busButton.setVisible (folder != nullptr);
    busButton.setToggleState (bus, juce::dontSendNotification);
    busButton.setEnabled (! (bus && hasEffects));
    busButton.setTooltip (! bus ? "Mix as a bus: the folder's tracks then mix together, through its own effects, fader and meter. Now "
                                  "its fader only scales their volumes"
                          : hasEffects
                              ? "A bus: its tracks mix together through its effects. Remove them to make it a plain folder"
                              : "A bus: its tracks mix together through its fader and meter. Click to make it a plain folder, whose "
                                "fader only scales their volumes");
}

void ChannelStrip::rebuildSends()
{
    sendSlots.clear();
    const auto sends = track.get() != nullptr ? getSends (*track.get()) : juce::Array<te::AuxSendPlugin*>();
    shownSends = getIDs (sends);

    for (auto* send : sends)
        slotHolder.addAndMakeVisible (sendSlots.add (new SendSlot (*send)));

    layOutSlots();
}

void ChannelStrip::showSendMenu()
{
    auto* audio = dynamic_cast<te::AudioTrack*> (track.get());

    if (audio == nullptr)
        return;

    // The returns there are, then new ones: one with an effect, as Studio One makes it in one step, or an empty one.
    juce::PopupMenu menu;
    const te::SafeSelectable<te::AudioTrack> safeTrack (audio);
    menu.addSectionHeader ("Return tracks hold effects that tracks share");

    for (auto* target : getReturnTracks (audio->edit))
    {
        const auto bus = getReturnBus (*target);

        if (target == audio)
            continue; // a return can't send to itself

        menu.addItem ("To " + target->getName(),
                      [safeTrack, bus]
                      {
                          if (auto* t = safeTrack.get())
                          {
                              t->edit.getUndoManager().beginNewTransaction ("Add Send");
                              addSend (*t, bus, false);
                          }
                      });
    }

    menu.addSeparator();
    menu.addItem ("To a New Return With an Effect...",
                  [safeTrack, safeButton = juce::Component::SafePointer<juce::Component> (&addSendButton)]
                  {
                      if (auto* t = safeTrack.get(); t != nullptr && safeButton != nullptr)
                          showNewReturnMenu (*t, *safeButton);
                  });

    menu.addItem ("To a New Empty Return",
                  [safeTrack]
                  {
                      if (auto* t = safeTrack.get())
                      {
                          auto& edit = t->edit;
                          edit.getUndoManager().beginNewTransaction ("Add Send");

                          if (auto* created = addNewReturnTrack (edit, nullptr))
                              addSend (*t, getReturnBus (*created), false);
                      }
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addSendButton));
}

juce::String ChannelStrip::describeInput() const
{
    auto* t = track.get();

    if (t == nullptr)
        return {};

    if (auto* folder = dynamic_cast<te::FolderTrack*> (t))
        return isBus (*folder) ? "Bus: mixes its tracks" : "Folder: scales its tracks";

    // A return says which tracks send to it.
    if (const auto bus = getReturnBus (*t); bus >= 0)
    {
        const auto senders = getSenderNames (t->edit, bus);
        return senders.isEmpty() ? juce::String ("Return: nothing sends to it yet") : "Return, from " + senders.joinIntoString (", ");
    }

    return {};
}

juce::String ChannelStrip::describeOutput() const
{
    auto* t = track.get();

    if (t == nullptr)
        return {};

    if (auto* folder = dynamic_cast<te::FolderTrack*> (t); folder != nullptr && ! isBus (*folder))
        return "No sound of its own";

    // A track goes into the nearest bus it is in, or else to the master.
    for (auto* parent = t->getParentFolderTrack(); parent != nullptr; parent = parent->getParentFolderTrack())
        if (isBus (*parent))
            return "Out: " + parent->getName();

    return "Out: Master";
}

void ChannelStrip::timerCallback()
{
    auto* t = track.get();

    // A deleted track's plug-ins can live on in the Edit's plug-in cache, so let go of them here.
    if (t == nullptr)
    {
        volumeAttachment.setParameter (nullptr, {});
        panAttachment.setParameter (nullptr, {});
        meter.setSource (nullptr);
    }
    else
    {
        attachControls(); // a folder may have become a bus, or plain again
    }

    nameLabel.setText (t != nullptr ? t->getName() : juce::String ("No track"), juce::dontSendNotification);
    inputLabel.setText (describeInput(), juce::dontSendNotification);
    outputLabel.setText (describeOutput(), juce::dontSendNotification);
    outputLabel.setTooltip (t != nullptr && getReturnBus (*t) < 0
                                ? "Where the sound goes: to the master, which you hear, or to the bus of the folder the track is in"
                                : "Where the sound goes");
    volumeAttachment.update();
    panAttachment.update();
    muteSolo.update();

    instrument.update();
    effects.update();
    layOutSlots(); // the devices may have changed

    if (getIDs (t != nullptr ? getSends (*t) : juce::Array<te::AuxSendPlugin*>()) != shownSends)
        rebuildSends();
    else
        for (auto* slot : sendSlots)
            slot->update();

    // The color bar, when the track's color changes (it follows the track's place in the palette unless it has its own).
    if (const auto colour = t != nullptr ? getTrackColour (*t, getPlaylistTracks (t->edit).indexOf (t)) : juce::Colour();
        colour != shownColour)
    {
        shownColour = colour;
        repaint (getLocalBounds().removeFromTop (colourBarHeight));
    }
}

void ChannelStrip::mouseDown (const juce::MouseEvent&)
{
    if (auto* t = track.get())
        selection.selectOnly (t);
}

bool ChannelStrip::isInterestedInDragSource (const SourceDetails& details)
{
    return track.get() != nullptr && getDraggedDevice (details.description).has_value();
}

void ChannelStrip::itemDragEnter (const SourceDetails&)
{
    dropping = true;
    repaint();
}

void ChannelStrip::itemDragExit (const SourceDetails&)
{
    dropping = false;
    repaint();
}

void ChannelStrip::itemDropped (const SourceDetails& details)
{
    itemDragExit (details);

    if (auto* t = track.get())
        if (const auto device = getDraggedDevice (details.description))
            if (addDeviceTo (t->edit, t, *device, &selection) != nullptr)
                markPlaced (details.description);
}

void ChannelStrip::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    auto area = getLocalBounds().reduced (2, 0);
    g.setColour (selected ? p.surfaceRaised.interpolatedWith (p.accent, 0.15f) : p.surfaceRaised);
    g.fillRoundedRectangle (area.toFloat(), cornerRadius);

    // The track's color along the top, as in the playlist.
    if (track.get() != nullptr)
    {
        g.setColour (shownColour);
        g.fillRect (area.removeFromTop (colourBarHeight));
    }

    // Selected, or related to the selected channel by a send or a bus: then fainter, so the routing shows. Thicker while a
    // device from the browser is dragged over it.
    if (selected || related || dropping)
    {
        g.setColour (p.accent.withAlpha (selected || dropping ? 1.0f : relatedOutlineAlpha));
        g.drawRoundedRectangle (getLocalBounds().reduced (2, 0).toFloat().reduced (0.5f), cornerRadius, dropping ? 2.0f : 1.0f);
    }
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (Space::xs + 2, Space::xs);
    area.removeFromTop (colourBarHeight);

    auto header = area.removeFromTop (headerHeight - colourBarHeight);
    nameLabel.setBounds (header.removeFromTop (controlHeight - 4));

    if (dynamic_cast<te::FolderTrack*> (track.get()) != nullptr)
        busButton.setBounds (header.removeFromRight (40).reduced (0, 1));

    inputLabel.setBounds (header);

    // At the bottom: the fader with the meter, mute and solo, pan, and where the sound goes. The instrument, effects
    // and sends get what is left above them, and scroll. (The mixer keeps the strip at least getMinimumHeight() tall.)
    const auto faderHeight = getFaderHeight (area.getHeight(), buttonHeight + panKnobSize + outputHeight + 4 * Space::xs + minSlotsHeight);
    auto faderArea = area.removeFromBottom (faderHeight).withSizeKeepingCentre (std::min (area.getWidth(), 56), faderHeight);
    meter.setBounds (faderArea.removeFromRight (meterWidth));
    volume.setBounds (faderArea);
    area.removeFromBottom (Space::xs);

    muteSolo.setBounds (area.removeFromBottom (buttonHeight));
    area.removeFromBottom (Space::xs);
    pan.setBounds (area.removeFromBottom (panKnobSize).withSizeKeepingCentre (panKnobSize, panKnobSize));
    area.removeFromBottom (Space::xs);
    outputLabel.setBounds (area.removeFromBottom (outputHeight));
    area.removeFromBottom (Space::xs);

    slotView.setBounds (area);
    layOutSlots();
}

int ChannelStrip::getMinimumHeight()
{
    // As resized() lays it out: the header, a heading and a button, where the sound goes, pan, mute and solo, and the
    // shortest fader.
    return 2 * Space::xs + headerHeight + minSlotsHeight + outputHeight + panKnobSize + buttonHeight + minFaderHeight + 4 * Space::xs;
}

void ChannelStrip::layOutSlots()
{
    // Top to bottom as the sound goes: the instrument (on tracks), the effects, then the sends (from tracks).
    const auto slotWidth = std::max (1, slotView.getWidth() - slotView.getScrollBarThickness());
    auto y = 0;

    const auto place = [&y, slotWidth] (juce::Component& component, int height, int gapAfter)
    {
        component.setBounds (0, y, slotWidth, height);
        y += height + gapAfter;
    };

    if (instrument.isVisible())
    {
        place (instrumentHeading, headingHeight, 0);
        place (instrument, instrument.getPreferredHeight(), Space::s);
    }

    place (effectsHeading, headingHeight, 0);
    place (effects, effects.getPreferredHeight(), Space::s);

    if (addSendButton.isVisible())
    {
        place (sendsHeading, headingHeight, 0);

        for (auto* slot : sendSlots)
            place (*slot, sendHeight, Space::xs);

        place (addSendButton, buttonHeight, 0);
    }

    slotHolder.setSize (slotWidth, y);
}

//==============================================================================
MasterStrip::MasterStrip()
{
    setTitle ("Master");
    showAsDecibels (volume);
    volume.setTooltip ("The volume of everything Phazite plays. Double-click for 0 dB");

    loudness.setFont (LookAndFeel::getNumberFont (TextSize::small));
    loudness.setJustificationType (juce::Justification::topLeft);
    loudness.setTitle ("Loudness");
    loudness.setTooltip ("Loudness, measured as streaming services do (EBU R128): momentary, over the last 3 seconds, and integrated since "
                         "the reset, with the true peak. It measures the master's effects, before the master volume");

    resetButton.setTooltip ("Starts the integrated loudness and the true peak again");
    resetButton.onClick = [this]
    {
        if (auto* e2 = edit.get())
            if (auto* meterPlugin = findLoudnessMeter (*e2))
                meterPlugin->resetReadings();
    };

    deviceView.setViewedComponent (&effects, false);
    deviceView.setScrollBarsShown (true, false);
    deviceView.setScrollBarThickness (6);

    for (auto* c : std::initializer_list<juce::Component*> { &effectsHeading, &deviceView, &meter, &volume, &loudness, &resetButton })
        addAndMakeVisible (c);

    startTimerHz (5);
}

void MasterStrip::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    effects.setChannel (nullptr, newEdit);

    const auto volumePlugin = newEdit != nullptr ? newEdit->getMasterVolumePlugin() : nullptr;
    volumeAttachment.setParameter (volumePlugin.get(), volumePlugin != nullptr ? volumePlugin->volParam->paramID : juce::String());
    timerCallback();
}

void MasterStrip::timerCallback()
{
    auto* e = edit.get();
    volumeAttachment.update();
    effects.update();
    effects.setSize (effects.getWidth(), effects.getPreferredHeight());

    auto* context = e != nullptr ? e->getCurrentPlaybackContext() : nullptr;
    meter.setSource (context != nullptr ? &context->masterLevels : nullptr);

    // LUFS to one decimal, or a dash until enough has been measured.
    juce::String text;

    if (auto* meterPlugin = e != nullptr ? findLoudnessMeter (*e) : nullptr)
    {
        const auto readings = meterPlugin->getReadings();
        const auto format = [] (bool valid, float value) { return valid ? juce::String (value, 1) : juce::String ("-"); };
        text << "M  " << format (readings.momentaryValid, readings.momentaryLufs) << " LUFS\n"
             << "S  " << format (readings.shortTermValid, readings.shortTermLufs) << " LUFS\n"
             << "I  " << format (readings.integratedValid, readings.integratedLufs) << " LUFS\n"
             << "TP " << format (readings.truePeakDb > te::LoudnessMeter::silenceFloorDb, readings.truePeakDb) << " dB";
    }

    loudness.setText (text, juce::dontSendNotification);
}

void MasterStrip::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (0, 0, 1, getHeight()); // between the tracks' strips and this
    g.setColour (p.text);
    g.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    g.drawText ("Master", getLocalBounds().reduced (Space::s).removeFromTop (controlHeight), juce::Justification::centred);

    // While an effect from the browser is dragged over it.
    if (dropping)
    {
        g.setColour (p.accent);
        g.drawRect (getLocalBounds(), 2);
    }
}

bool MasterStrip::isInterestedInDragSource (const SourceDetails& details)
{
    const auto device = getDraggedDevice (details.description);
    return edit.get() != nullptr && device.has_value() && ! device->isInstrument;
}

void MasterStrip::setDropping (bool isDropping)
{
    dropping = isDropping;
    repaint();
}

void MasterStrip::itemDropped (const SourceDetails& details)
{
    setDropping (false);

    if (auto* e = edit.get())
        if (const auto device = getDraggedDevice (details.description))
            if (addDeviceTo (*e, nullptr, *device, nullptr) != nullptr)
                markPlaced (details.description);
}

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    area.removeFromTop (controlHeight);

    auto readout = area.removeFromBottom (loudnessHeight);
    resetButton.setBounds (readout.removeFromBottom (buttonHeight).removeFromLeft (64));
    loudness.setBounds (readout);
    area.removeFromBottom (Space::s);

    // The fader, and above it the effects, which scroll. (The mixer keeps the strip at least getMinimumHeight() tall.)
    const auto faderHeight = getFaderHeight (area.getHeight(), Space::xs + minSlotsHeight);
    auto faderArea = area.removeFromBottom (faderHeight).withSizeKeepingCentre (std::min (area.getWidth(), 56), faderHeight);
    meter.setBounds (faderArea.removeFromRight (meterWidth));
    volume.setBounds (faderArea);
    area.removeFromBottom (Space::xs);

    effectsHeading.setBounds (area.removeFromTop (headingHeight));
    deviceView.setBounds (area);
    effects.setSize (std::max (1, area.getWidth() - deviceView.getScrollBarThickness()), effects.getPreferredHeight());
}

int MasterStrip::getMinimumHeight()
{
    // As resized() lays it out: the name, a heading and a button, the shortest fader and the loudness readout.
    return 2 * Space::s + controlHeight + minSlotsHeight + Space::xs + minFaderHeight + Space::s + loudnessHeight;
}

//==============================================================================
Inspector::Inspector (te::Engine& engine, CommandBridge& bridge, ProjectContext& context)
    : projectContext (context),
      selection (context.getSelection()),
      strip (engine, bridge, context.getSelection())
{
    setTitle ("Inspector");
    setDescription ("The channel of the selected track.");
    stripView.setViewedComponent (&strip, false);
    stripView.setScrollBarsShown (true, false);
    addAndMakeVisible (stripView);
    selection.addChangeListener (this);
    startTimerHz (2);
}

Inspector::~Inspector()
{
    selection.removeChangeListener (this);
}

void Inspector::showTrack()
{
    te::Track* track = nullptr;

    if (auto* edit = projectContext.getEdit())
    {
        track = getSelectedTracks (selection).getFirst();

        if (track == nullptr)
            track = getSelectedAudioTrack (selection);

        if (track == nullptr)
            track = getLiveMidiTrack (*edit);

        if (track == nullptr)
            track = getPlaylistTracks (*edit).getFirst();
    }

    if (strip.getTrack() != track)
        strip.setTrack (track);
}

void Inspector::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    g.setColour (p.text);
    g.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    g.drawText (getTitle(), getLocalBounds().reduced (Space::m).removeFromTop (24), juce::Justification::centredLeft);
}

void Inspector::resized()
{
    // The strip fills the panel, but is no shorter than its parts need (then it scrolls) and no narrower than the
    // mixer's strips.
    auto area = getLocalBounds().reduced (Space::m);
    area.removeFromTop (24 + Space::s);
    const auto scrolls = area.getHeight() < ChannelStrip::getMinimumHeight();
    stripView.setBounds (area);
    strip.setSize (std::max (ChannelStrip::width, area.getWidth() - (scrolls ? stripView.getScrollBarThickness() : 0)),
                   std::max (area.getHeight(), ChannelStrip::getMinimumHeight()));
}
} // namespace phazite::ui
