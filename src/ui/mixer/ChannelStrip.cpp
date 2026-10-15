// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelStrip.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LiveInput.h"
#include "model/mixer/Mixer.h"
#include "services/Library.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
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
                setPreFader (*aux, ! isPreFader (*aux));

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
                              removeSend (*aux);
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
            setMixedAsBus (*folder, ! isBus (*folder));
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
                              addSendTo (*t, bus);
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
                          addSendToNewReturn (*t);
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

} // namespace phazite::ui
