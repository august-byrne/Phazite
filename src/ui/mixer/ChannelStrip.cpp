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
// A send to a return: its name, its level, and whether it takes the sound before or after the fader. Every return has
// one on every strip that can send to it, off until its knob is first turned, which makes the send (question 13).
struct ChannelStrip::SendSlot : public juce::Component
{
    SendSlot (ProjectContext& projectContext, TrackID fromTrack, const SendTarget& target)
        : context (projectContext),
          from (fromTrack)
    {
        nameButton.setTooltip ("Turn the knob to send some of this track to this return. Right-click to remove the send");
        nameButton.onClicked = [this] (bool isMenuClick)
        {
            if (isMenuClick)
                showMenu();
        };

        level.setTooltip ("How much of this track the send feeds the return: off until you turn it. Double-click for 0 dB");
        showAsDecibels (level);
        attachment.setParameter (getSendLevelTo (context, from, target.returnTrack));

        // The slot shows the switch when the song changes, right after this (ChannelStrip::songChanged()).
        prePost.setTooltip ("Pre: the send takes the sound before the fader, so the fader doesn't change it. Post: after the fader");
        prePost.onClick = [this]
        {
            if (shown.send.has_value())
                setPreFader (context, shown.send->id, ! shown.send->preFader);
        };

        for (auto* c : std::initializer_list<juce::Component*> { &nameButton, &level, &prePost })
            addAndMakeVisible (c);

        show (target);
    }

    void show (const SendTarget& target)
    {
        shown = target;
        nameButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + target.name);
        nameButton.setTitle (target.name + " return");
        level.setTitle ("Send to " + target.name);
        prePost.setVisible (target.send.has_value()); // a send that isn't there has nothing to switch
        prePost.setButtonText (target.send.has_value() && target.send->preFader ? "Pre" : "Post");
    }

    void showMenu()
    {
        if (! shown.send.has_value())
            return;

        juce::PopupMenu menu;
        menu.addItem ("Remove Send", [&projectContext = context, id = shown.send->id] { removeSend (projectContext, id); });
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
    const TrackID from;
    SendTarget shown;
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

    for (auto* label : { &inputLabel })
    {
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (TextSize::small));
        label->setMinimumHorizontalScale (0.8f);
        label->setInterceptsMouseClicks (false, false);
    }

    outputButton.setTitle ("Output");
    outputButton.onClick = [this] { showOutputMenu (context, commands, track, outputButton); };

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

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &inputLabel, &busButton, &slotView, &outputButton, &meter, &volume,
                                                             &pan, &muteSolo, &cpuBar })
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
    shownReturns.clear();
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
    outputButton.setButtonText (strip::describeOutput (channel));

    // A track or a bus chooses where its sound goes; a return goes to the master, and a plain folder has no sound.
    const auto chooses = channel.has_value() && channel->kind != ChannelKind::plainFolder && canChooseOutput (context, track);
    outputButton.setEnabled (chooses);
    outputButton.setTooltip (chooses ? "Where the sound goes: to the master, which you hear, or into a bus, which the track goes "
                                       "into as a folder. Click to choose"
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
    // A slot for every return the channel can send to (tracks and returns send; folders don't).
    const auto kind = channel.has_value() ? channel->kind : ChannelKind::track;
    const auto sends = channel.has_value() && (kind == ChannelKind::track || kind == ChannelKind::returnTrack);
    const auto targets = sends ? getSendTargets (context, track) : std::vector<SendTarget>();
    std::vector<TrackID> returns;

    for (const auto& target : targets)
        returns.push_back (target.returnTrack);

    if (returns == shownReturns)
    {
        for (size_t i = 0; i < targets.size(); ++i)
            sendSlots[(int) i]->show (targets[i]);

        return;
    }

    sendSlots.clear();
    shownReturns = returns;

    for (const auto& target : targets)
        slotHolder.addAndMakeVisible (sendSlots.add (new SendSlot (context, track, target)));
}

void ChannelStrip::showSendMenu()
{
    if (! channel.has_value() || (channel->kind != ChannelKind::track && channel->kind != ChannelKind::returnTrack))
        return;

    // The returns there are have their knobs above: a new one, with an effect, as Studio One makes it in one step, or empty.
    juce::PopupMenu menu;
    auto& projectContext = context;
    const auto from = track;
    menu.addSectionHeader ("A new return track, for an effect that tracks share");
    menu.addItem ("With an Effect...",
                  [&projectContext, from, safeButton = juce::Component::SafePointer<juce::Component> (&addSendButton)]
                  {
                      if (safeButton != nullptr)
                          showNewReturnMenu (projectContext, from, *safeButton);
                  });

    menu.addItem ("Empty", [&projectContext, from] { addSendToNewReturn (projectContext, from); });
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

void ChannelStrip::timerCallback()
{
    if (! isShowing()) // what is due waits until it shows (docs/performance-audit.md, U5)
        return;

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

    cpuBar.setLoad (getCpuLoad (context, track));
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

    // Along the bottom, how much CPU the track's devices take.
    cpuBar.setBounds (area.removeFromBottom (CpuBar::height).reduced (Space::xs, 0));
    area.removeFromBottom (Space::xs);
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
    outputButton.setBounds (area.removeFromBottom (outputHeight));
    area.removeFromBottom (Space::xs);

    slotView.setBounds (area);
    layOutSlots();
}

int ChannelStrip::getMinimumHeight()
{
    // As resized() lays it out: the header, a heading and a button, where the sound goes, pan, mute and solo, the
    // shortest fader, and the CPU bar.
    return 2 * Space::xs + headerHeight + minSlotsHeight + outputHeight + panKnobSize + buttonHeight + minFaderHeight + 4 * Space::xs
           + CpuBar::height + Space::xs;
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
