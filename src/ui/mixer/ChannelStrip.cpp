// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelStrip.h"
#include "model/presets/PresetModel.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

//==============================================================================
// A send: the return it feeds, its level, and whether it takes the sound before or after the fader.
struct ChannelStrip::SendSlot : public juce::Component
{
    SendSlot (ProjectContext& projectContext, const Send& send)
        : context (projectContext),
          shown (send)
    {
        nameButton.setTooltip ("A send to this return track. Right-click to remove it");
        nameButton.onClicked = [this] (bool isMenuClick)
        {
            if (isMenuClick)
                showMenu();
        };

        level.setTooltip ("How much of this track the send feeds the return. Double-click for 0 dB");
        showAsDecibels (level);
        attachment.setParameter (getSendLevel (context, send.id));

        // The slot shows the switch when the song changes, right after this (ChannelStrip::songChanged()).
        prePost.setTooltip ("Pre: the send takes the sound before the fader, so the fader doesn't change it. Post: after the fader");
        prePost.onClick = [this] { setPreFader (context, shown.id, ! shown.preFader); };

        for (auto* c : std::initializer_list<juce::Component*> { &nameButton, &level, &prePost })
            addAndMakeVisible (c);

        show (send);
    }

    void show (const Send& send)
    {
        shown = send;
        nameButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + send.returnName);
        nameButton.setTitle ("Send to " + send.returnName);
        prePost.setButtonText (send.preFader ? "Pre" : "Post");
    }

    void showMenu()
    {
        juce::PopupMenu menu;
        menu.addItem ("Remove Send", [&projectContext = context, id = shown.id] { removeSend (projectContext, id); });
        menu.showMenuAsync (juce::PopupMenu::Options());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        prePost.setBounds (area.removeFromRight (36));
        level.setBounds (area.removeFromRight (sendHeight));
        nameButton.setBounds (area.withTrimmedRight (2));
    }

    ProjectContext& context;
    Send shown;
    MenuButton nameButton;
    Knob level { "Send level", decibelsToFaderPosition (0.0f) };
    ParameterAttachment attachment { level };
    juce::TextButton prePost;
};

//==============================================================================
ChannelStrip::ChannelStrip (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      instrument (DeviceSlots::Kind::instrument, projectContext, bridge),
      effects (DeviceSlots::Kind::effects, projectContext, bridge),
      muteSolo (projectContext)
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
        if (channel.has_value() && (channel->kind == ChannelKind::bus || channel->kind == ChannelKind::plainFolder))
            setMixedAsBus (context, track, channel->kind != ChannelKind::bus);

        refresh();
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

    context.addListener (this);
    setTrack ({});
    startTimerHz (8);
}

ChannelStrip::~ChannelStrip()
{
    context.removeListener (this);
    sendSlots.clear(); // before slotHolder, which holds them
}

void ChannelStrip::setTrack (TrackID newTrack)
{
    track = newTrack;
    instrument.setTrack (newTrack);
    effects.setTrack (newTrack);
    muteSolo.setTrack (newTrack);
    meter.setSource (getLevel (context, newTrack));
    shownSends.clear();
    sendSlots.clear();
    refresh();
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

void ChannelStrip::songChanged (const SongChanges& changes)
{
    // Shown at the next tick: a fader dragged on another strip changes the song as often as the mouse moves.
    refreshDue = refreshDue || changes.tracks || changes.devices;
    needDue = needDue || changes.clips; // notes that have no instrument to play them
}

void ChannelStrip::refresh()
{
    refreshDue = needDue = false;
    const auto shownColour = channel.has_value() ? channel->colour : juce::Colour();
    channel = getChannel (context, track);

    showChannel();
    attachControls();
    instrument.refresh();
    effects.refresh();
    refreshSends();
    muteSolo.update();
    layOutSlots();

    // The color bar, when the track's color changes (it follows the track's place in the palette unless it has its own).
    if ((channel.has_value() ? channel->colour : juce::Colour()) != shownColour)
        repaint (getLocalBounds().removeFromTop (colourBarHeight));
}

void ChannelStrip::showChannel()
{
    nameLabel.setText (channel.has_value() ? channel->name : juce::String ("No track"), juce::dontSendNotification);
    inputLabel.setText (describeInput(), juce::dontSendNotification);
    outputLabel.setText (describeOutput(), juce::dontSendNotification);
    outputLabel.setTooltip (channel.has_value() && channel->kind != ChannelKind::returnTrack
                                ? "Where the sound goes: to the master, which you hear, or to the bus of the folder the track is in"
                                : "Where the sound goes");
}

void ChannelStrip::attachControls()
{
    // A plain folder has no pan of its own: its VCA only scales its tracks.
    volumeAttachment.setParameter (getVolume (context, track));
    panAttachment.setParameter (getPan (context, track));

    // Instruments go on tracks: folders and returns take effects only. Sends come from tracks and returns.
    const auto kind = channel.has_value() ? channel->kind : ChannelKind::track;
    const auto holdsInstrument = channel.has_value() && kind == ChannelKind::track;
    const auto sends = channel.has_value() && (kind == ChannelKind::track || kind == ChannelKind::returnTrack);
    instrumentHeading.setVisible (holdsInstrument);
    instrument.setVisible (holdsInstrument);
    sendsHeading.setVisible (sends);
    addSendButton.setVisible (sends);

    // A folder's bus switch. A bus with effects stays one: making it plain would take them away.
    const auto folder = channel.has_value() && (kind == ChannelKind::bus || kind == ChannelKind::plainFolder);
    const auto bus = channel.has_value() && kind == ChannelKind::bus, hasEffects = channel.has_value() && channel->hasEffects;
    busButton.setVisible (folder);
    busButton.setToggleState (bus, juce::dontSendNotification);
    busButton.setEnabled (! (bus && hasEffects));
    busButton.setTooltip (! bus ? "Mix as a bus: the folder's tracks then mix together, through its own effects, fader and meter. Now "
                                  "its fader only scales their volumes"
                          : hasEffects
                              ? "A bus: its tracks mix together through its effects. Remove them to make it a plain folder"
                              : "A bus: its tracks mix together through its fader and meter. Click to make it a plain folder, whose "
                                "fader only scales their volumes");
}

void ChannelStrip::refreshSends()
{
    const auto sends = getSends (context, track);

    if (getIDs (sends) == shownSends)
    {
        for (size_t i = 0; i < sends.size(); ++i)
            sendSlots[(int) i]->show (sends[i]);

        return;
    }

    sendSlots.clear();
    shownSends = getIDs (sends);

    for (const auto& send : sends)
        slotHolder.addAndMakeVisible (sendSlots.add (new SendSlot (context, send)));
}

void ChannelStrip::showSendMenu()
{
    if (! channel.has_value() || (channel->kind != ChannelKind::track && channel->kind != ChannelKind::returnTrack))
        return;

    // The returns there are, then new ones: one with an effect, as Studio One makes it in one step, or an empty one.
    juce::PopupMenu menu;
    auto& projectContext = context;
    const auto from = track;
    menu.addSectionHeader ("Return tracks hold effects that tracks share");

    for (const auto& target : getReturns (context))
        if (target.id != from) // a return can't send to itself
            menu.addItem ("To " + target.name, [&projectContext, from, to = target.id] { addSendTo (projectContext, from, to); });

    menu.addSeparator();
    menu.addItem ("To a New Return With an Effect...",
                  [&projectContext, from, safeButton = juce::Component::SafePointer<juce::Component> (&addSendButton)]
                  {
                      if (safeButton != nullptr)
                          showNewReturnMenu (projectContext, from, *safeButton);
                  });

    menu.addItem ("To a New Empty Return", [&projectContext, from] { addSendToNewReturn (projectContext, from); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addSendButton));
}

juce::String ChannelStrip::describeInput() const
{
    if (! channel.has_value())
        return {};

    switch (channel->kind)
    {
        case ChannelKind::bus:
            return "Bus: mixes its tracks";
        case ChannelKind::plainFolder:
            return "Folder: scales its tracks";
        case ChannelKind::returnTrack: // a return says which tracks send to it
            return channel->senders.isEmpty() ? juce::String ("Return: nothing sends to it yet")
                                              : "Return, from " + channel->senders.joinIntoString (", ");
        case ChannelKind::track:
            break;
    }

    return {};
}

juce::String ChannelStrip::describeOutput() const
{
    if (! channel.has_value())
        return {};

    if (channel->kind == ChannelKind::plainFolder)
        return "No sound of its own";

    return "Out: " + (channel->outputBus.isNotEmpty() ? channel->outputBus : juce::String ("Master"));
}

void ChannelStrip::timerCallback()
{
    if (refreshDue)
        refresh();
    else if (std::exchange (needDue, false))
        instrument.refreshNeed();

    volumeAttachment.update();
    panAttachment.update();
    instrument.updateValues();
    effects.updateValues();

    for (auto* slot : sendSlots)
        slot->attachment.update();
}

void ChannelStrip::mouseDown (const juce::MouseEvent&)
{
    if (channel.has_value())
        context.getSongSelection().selectOnly (track);
}

bool ChannelStrip::isInterestedInDragSource (const SourceDetails& details)
{
    return channel.has_value()
           && (getDraggedDevice (details.description).has_value() || getDraggedPreset (details.description).has_value());
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

    if (! channel.has_value())
        return;

    // A device, or a preset: a device's or a container's on this channel, a track's on a new track after it.
    auto placed = false;

    if (const auto device = getDraggedDevice (details.description))
        placed = addDeviceTo (context, track, *device);
    else if (const auto preset = getDraggedPreset (details.description))
        placed = loadPreset (context, *preset, track);

    if (placed)
        markPlaced (details.description);
}

void ChannelStrip::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    auto area = getLocalBounds().reduced (2, 0);
    g.setColour (selected ? p.surfaceRaised.interpolatedWith (p.accent, 0.15f) : p.surfaceRaised);
    g.fillRoundedRectangle (area.toFloat(), cornerRadius);

    // The track's color along the top, as in the playlist.
    if (channel.has_value())
    {
        g.setColour (channel->colour);
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

    if (busButton.isVisible())
        busButton.setBounds (header.removeFromRight (40).reduced (0, 1));

    inputLabel.setBounds (header);

    // At the bottom: the fader with the meter, mute and solo, pan, and where the sound goes. The instrument, effects
    // and sends get what is left above them, and scroll; the fader shrinks, down to its shortest, so that they show
    // whole when they can, as the mixer shares the window's height with the Devices view under it. (The mixer keeps the
    // strip at least getMinimumHeight() tall.)
    layOutSlots();
    const auto slotsWanted = std::max (minSlotsHeight, slotHolder.getHeight());
    const auto faderHeight = getFaderHeight (area.getHeight(), buttonHeight + panKnobSize + outputHeight + 4 * Space::xs + slotsWanted);
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
