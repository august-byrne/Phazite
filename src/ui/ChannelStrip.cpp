// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelStrip.h"
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "model/LiveInput.h"
#include "model/LoudnessMeterPlugin.h"
#include "model/Mixer.h"
#include "model/Tracks.h"

#include <map>

namespace phazite::ui
{
namespace
{
    constexpr int slotNameHeight = 22, slotKnobHeight = 30, sendHeight = 26, buttonHeight = 24;
    constexpr int keyControlsPerDevice = 2;
    constexpr int panKnobSize = 44, meterWidth = 10, headerHeight = 46, colourBarHeight = 4;
    constexpr int minFaderHeight = 72, minSlotsHeight = buttonHeight;
    constexpr int loudnessHeight = 4 * 16 + buttonHeight + Space::xs; // the master's readout: four lines and the Reset button

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
        nameButton.setTooltip ("Opens " + device.getName() + "'s window. Right-click to turn it off, move or remove it");
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
};

//==============================================================================
DeviceSlots::DeviceSlots (te::Engine& e, CommandRegistry& registry)
    : engine (e),
      commands (registry)
{
    setTitle ("Devices");
    addButton.setTooltip ("Adds a device: an instrument or an effect, built in or a plug-in");
    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);
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
        return getDevices (*t);

    if (auto* e = edit.get())
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
}

void DeviceSlots::rebuild()
{
    slots.clear();
    const auto channelDevices = getChannelDevices();
    shownDevices = getIDs (channelDevices);

    for (auto* device : channelDevices)
        addAndMakeVisible (slots.add (new Slot (*this, *device)));

    addButton.setEnabled (track.get() != nullptr || edit.get() != nullptr);
    resized();
}

int DeviceSlots::getPreferredHeight() const
{
    auto height = buttonHeight;

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
    auto* e = track.get() != nullptr ? &track->edit : edit.get();

    if (e == nullptr)
        return;

    // Built-in devices, then the plug-ins found by scanning, by maker. Only tracks play instruments: folders and
    // the master take effects.
    const auto types = getDeviceTypes (engine);
    const auto canHoldInstruments = dynamic_cast<te::AudioTrack*> (track.get()) != nullptr;
    juce::PopupMenu instruments, effects;
    std::map<juce::String, juce::PopupMenu> plugins;

    for (size_t i = 0; i < types.size(); ++i)
    {
        const auto& type = types[i];
        const auto id = (int) i + 1;

        if (te::PluginManager::isBuiltInPlugin (type.description))
            (type.isInstrument ? instruments : effects).addItem (id, type.name);
        else
            plugins[type.description.manufacturerName.isNotEmpty() ? type.description.manufacturerName : juce::String ("Other")].addItem (
                id, type.name + (type.isInstrument ? " (instrument)" : ""), ! type.isInstrument || canHoldInstruments);
    }

    juce::PopupMenu menu;
    menu.addSubMenu ("Instruments", instruments, canHoldInstruments);
    menu.addSubMenu ("Effects", effects);

    if (plugins.empty())
    {
        menu.addItem ("No plug-ins yet: Options > Scan for Plug-ins finds them", false, false, [] {});
    }
    else
    {
        menu.addSectionHeader ("Plug-ins");

        for (auto& [maker, list] : plugins)
            menu.addSubMenu (maker, list);
    }

    const te::SafeSelectable<te::Track> safeTrack (track.get());
    const te::SafeSelectable<te::Edit> safeEdit (e);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton),
                        [safeThis = juce::Component::SafePointer<DeviceSlots> (this), safeTrack, safeEdit, types] (int result)
                        {
                            auto* chosenEdit = safeEdit.get();

                            if (safeThis == nullptr || chosenEdit == nullptr || result <= 0 || result > (int) types.size()) // 0: dismissed
                                return;

                            const auto& type = types[(size_t) result - 1];
                            chosenEdit->getUndoManager().beginNewTransaction ("Add " + type.name);
                            auto device = createDevice (*chosenEdit, type);

                            if (auto* t = safeTrack.get())
                                insertDevice (*t, device);
                            else
                                insertMasterDevice (*chosenEdit, device);

                            safeThis->update();
                        });
}

void DeviceSlots::showSlotMenu (te::Plugin& device)
{
    const auto devices = getChannelDevices();
    const auto index = devices.indexOf (&device);
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
    menu.addItem ("Move Up", index > 0, false, act ("Move Device", [index] (te::Plugin& d) { moveDevice (d, index - 1); }));
    menu.addItem ("Move Down", index < devices.size() - 1, false,
                  act ("Move Device", [index] (te::Plugin& d) { moveDevice (d, index + 1); }));
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
        nameButton.setTooltip ("A send to this return. Right-click to remove it");
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
        nameButton.setButtonText (name);
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
ChannelStrip::ChannelStrip (te::Engine& e, CommandRegistry& registry, te::SelectionManager& selectionManager)
    : engine (e),
      commands (registry),
      selection (selectionManager),
      devices (e, registry)
{
    setTitle ("Channel");

    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    nameLabel.setInterceptsMouseClicks (false, false); // a click on the name selects the track
    outputLabel.setJustificationType (juce::Justification::centred);
    outputLabel.setFont (juce::FontOptions (TextSize::small));
    outputLabel.setInterceptsMouseClicks (false, false);

    showAsDecibels (volume);
    showAsPan (pan);
    volume.setTooltip ("The track's volume. Double-click for 0 dB; drag with Shift held for fine steps");
    pan.setTooltip ("Where the track sits between the left and right speakers. Double-click to center it");

    muteButton.setTitle ("Mute");
    muteButton.setTooltip ("Mute: silences this track");
    soloButton.setTitle ("Solo");
    soloButton.setTooltip ("Solo: plays only this track, and any others that are soloed");

    for (auto* button : { &muteButton, &soloButton })
        button->setClickingTogglesState (true);

    muteButton.onClick = [this] { setMuteOrSolo (true, muteButton.getToggleState()); };
    soloButton.onClick = [this] { setMuteOrSolo (false, soloButton.getToggleState()); };

    addSendButton.setTooltip ("Sends some of this track to a return track, for an effect such as a reverb");
    addSendButton.onClick = [this] { showSendMenu(); };

    slotView.setViewedComponent (&slotHolder, false);
    slotView.setScrollBarsShown (true, false);
    slotView.setScrollBarThickness (6);
    slotHolder.addAndMakeVisible (devices);
    slotHolder.addAndMakeVisible (addSendButton);

    for (auto* c :
         std::initializer_list<juce::Component*> { &nameLabel, &outputLabel, &slotView, &meter, &volume, &pan, &muteButton, &soloButton })
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
    devices.setChannel (newTrack, nullptr);
    shownSends.clear();
    rebuildSends();
    attachControls();
    timerCallback();
}

void ChannelStrip::setSelected (bool isSelected)
{
    if (selected != isSelected)
    {
        selected = isSelected;
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

    // Sends come from tracks; folders route through their tracks.
    addSendButton.setVisible (dynamic_cast<te::AudioTrack*> (t) != nullptr);
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

    juce::PopupMenu menu;
    const te::SafeSelectable<te::AudioTrack> safeTrack (audio);

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
    menu.addItem ("To a New Return Track",
                  [safeTrack]
                  {
                      if (auto* t = safeTrack.get())
                      {
                          auto& edit = t->edit;
                          edit.getUndoManager().beginNewTransaction ("Add Send");

                          if (auto* created = addReturnTrack (edit, "Return " + juce::String (getReturnTracks (edit).size() + 1), nullptr))
                              addSend (*t, getReturnBus (*created), false);
                      }
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addSendButton));
}

juce::String ChannelStrip::describeOutput() const
{
    auto* t = track.get();

    if (t == nullptr)
        return {};

    if (auto* folder = dynamic_cast<te::FolderTrack*> (t); folder != nullptr && ! isBus (*folder))
        return "Folder: scales its tracks";

    // A track goes into the nearest bus it is in, or else to the master.
    for (auto* parent = t->getParentFolderTrack(); parent != nullptr; parent = parent->getParentFolderTrack())
        if (isBus (*parent))
            return juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + parent->getName();

    const auto prefix = getReturnBus (*t) >= 0 ? juce::String ("Return ") : (isBus (*t) ? juce::String ("Bus ") : juce::String());
    return prefix + juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 Master"));
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
    outputLabel.setText (describeOutput(), juce::dontSendNotification);
    volumeAttachment.update();
    panAttachment.update();

    muteButton.setEnabled (t != nullptr);
    soloButton.setEnabled (t != nullptr);
    muteButton.setToggleState (t != nullptr && t->isMuted (false), juce::dontSendNotification);
    soloButton.setToggleState (t != nullptr && t->isSolo (false), juce::dontSendNotification);

    devices.update();
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

void ChannelStrip::setMuteOrSolo (bool mute, bool on)
{
    // Tracktion keeps mute and solo out of the undo history, as many DAWs do.
    if (auto* t = track.get())
    {
        if (mute)
            t->setMute (on);
        else
            t->setSolo (on);
    }
}

void ChannelStrip::mouseDown (const juce::MouseEvent&)
{
    if (auto* t = track.get())
        selection.selectOnly (t);
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

    if (selected)
    {
        g.setColour (p.accent);
        g.drawRoundedRectangle (getLocalBounds().reduced (2, 0).toFloat().reduced (0.5f), cornerRadius, 1.0f);
    }
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (Space::xs + 2, Space::xs);
    area.removeFromTop (colourBarHeight);

    auto header = area.removeFromTop (headerHeight - colourBarHeight);
    nameLabel.setBounds (header.removeFromTop (controlHeight - 4));
    outputLabel.setBounds (header);

    // At the bottom: the fader with the meter, then mute and solo, and pan. The device and send slots get what is
    // left above them, and scroll. (The mixer keeps the strip at least getMinimumHeight() tall.)
    const auto faderHeight = getFaderHeight (area.getHeight(), buttonHeight + panKnobSize + 3 * Space::xs + minSlotsHeight);
    auto faderArea = area.removeFromBottom (faderHeight).withSizeKeepingCentre (std::min (area.getWidth(), 56), faderHeight);
    meter.setBounds (faderArea.removeFromRight (meterWidth));
    volume.setBounds (faderArea);
    area.removeFromBottom (Space::xs);

    auto buttons = area.removeFromBottom (buttonHeight);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (Space::xs / 2, 0));
    soloButton.setBounds (buttons.reduced (Space::xs / 2, 0));
    area.removeFromBottom (Space::xs);
    pan.setBounds (area.removeFromBottom (panKnobSize).withSizeKeepingCentre (panKnobSize, panKnobSize));
    area.removeFromBottom (Space::xs);

    slotView.setBounds (area);
    layOutSlots();
}

int ChannelStrip::getMinimumHeight()
{
    // As resized() lays it out: the header, a row of slots, pan, mute and solo, and the shortest fader.
    return 2 * Space::xs + headerHeight + minSlotsHeight + panKnobSize + buttonHeight + minFaderHeight + 3 * Space::xs;
}

void ChannelStrip::layOutSlots()
{
    const auto slotWidth = std::max (1, slotView.getWidth() - slotView.getScrollBarThickness());
    auto y = 0;

    devices.setBounds (0, y, slotWidth, devices.getPreferredHeight());
    y += devices.getHeight() + Space::s;

    for (auto* slot : sendSlots)
    {
        slot->setBounds (0, y, slotWidth, sendHeight);
        y += sendHeight + Space::xs;
    }

    addSendButton.setBounds (0, y, slotWidth, buttonHeight);
    y += addSendButton.isVisible() ? buttonHeight : 0;
    slotHolder.setSize (slotWidth, y);
}

//==============================================================================
MasterStrip::MasterStrip (te::Engine& e, CommandRegistry& registry)
    : devices (e, registry)
{
    setTitle ("Master");
    showAsDecibels (volume);
    volume.setTooltip ("The volume of everything Phazite plays. Double-click for 0 dB");

    loudness.setFont (LookAndFeel::getNumberFont (TextSize::small));
    loudness.setJustificationType (juce::Justification::topLeft);
    loudness.setTitle ("Loudness");
    loudness.setTooltip ("Loudness, measured as streaming services do (EBU R128): momentary, over the last 3 seconds, and integrated since "
                         "the reset, with the true peak. It measures the master's devices, before the master volume");

    resetButton.setTooltip ("Starts the integrated loudness and the true peak again");
    resetButton.onClick = [this]
    {
        if (auto* e2 = edit.get())
            if (auto* meterPlugin = findLoudnessMeter (*e2))
                meterPlugin->resetReadings();
    };

    deviceView.setViewedComponent (&devices, false);
    deviceView.setScrollBarsShown (true, false);
    deviceView.setScrollBarThickness (6);

    for (auto* c : std::initializer_list<juce::Component*> { &deviceView, &meter, &volume, &loudness, &resetButton })
        addAndMakeVisible (c);

    startTimerHz (5);
}

void MasterStrip::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    devices.setChannel (nullptr, newEdit);

    const auto volumePlugin = newEdit != nullptr ? newEdit->getMasterVolumePlugin() : nullptr;
    volumeAttachment.setParameter (volumePlugin.get(), volumePlugin != nullptr ? volumePlugin->volParam->paramID : juce::String());
    timerCallback();
}

void MasterStrip::timerCallback()
{
    auto* e = edit.get();
    volumeAttachment.update();
    devices.update();
    devices.setSize (devices.getWidth(), devices.getPreferredHeight());

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
}

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    area.removeFromTop (controlHeight);

    auto readout = area.removeFromBottom (loudnessHeight);
    resetButton.setBounds (readout.removeFromBottom (buttonHeight).removeFromLeft (64));
    loudness.setBounds (readout);
    area.removeFromBottom (Space::s);

    // The fader, and above it the devices, which scroll. (The mixer keeps the strip at least getMinimumHeight() tall.)
    const auto faderHeight = getFaderHeight (area.getHeight(), Space::xs + minSlotsHeight);
    auto faderArea = area.removeFromBottom (faderHeight).withSizeKeepingCentre (std::min (area.getWidth(), 56), faderHeight);
    meter.setBounds (faderArea.removeFromRight (meterWidth));
    volume.setBounds (faderArea);
    area.removeFromBottom (Space::xs);

    deviceView.setBounds (area);
    devices.setSize (std::max (1, area.getWidth() - deviceView.getScrollBarThickness()), devices.getPreferredHeight());
}

int MasterStrip::getMinimumHeight()
{
    // As resized() lays it out: the name, a row of device slots, the shortest fader and the loudness readout.
    return 2 * Space::s + controlHeight + minSlotsHeight + Space::xs + minFaderHeight + Space::s + loudnessHeight;
}

//==============================================================================
Inspector::Inspector (te::Engine& engine, CommandRegistry& registry, std::function<te::Edit*()> editGetter,
                      te::SelectionManager& selectionManager)
    : getEdit (std::move (editGetter)),
      selection (selectionManager),
      strip (engine, registry, selectionManager)
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

    if (auto* edit = getEdit())
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
    auto area = getLocalBounds().reduced (Space::m);
    area.removeFromTop (24 + Space::s);
    // Up to 640 pixels tall, but no shorter than its parts need: then it scrolls.
    const auto height = std::min (area.getHeight(), 640);
    const auto scrolls = height < ChannelStrip::getMinimumHeight();
    stripView.setBounds (area.removeFromLeft (ChannelStrip::width + (scrolls ? stripView.getScrollBarThickness() : 0)).withHeight (height));
    strip.setSize (ChannelStrip::width, std::max (height, ChannelStrip::getMinimumHeight()));
}
} // namespace phazite::ui
