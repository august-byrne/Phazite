// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelStrip.h"
#include "LookAndFeel.h"
#include "model/LiveInput.h"

namespace phazite::ui
{
namespace
{
    constexpr int stripWidth = 104;
    constexpr int knobSize = 52;
    constexpr int meterWidth = 10;
} // namespace

//==============================================================================
ChannelStrip::ChannelStrip()
{
    setTitle ("Channel");

    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));

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

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &meter, &volume, &pan, &muteButton, &soloButton })
        addAndMakeVisible (c);

    timerCallback(); // "No track" until setTrack()
    startTimerHz (4);
}

void ChannelStrip::setTrack (te::AudioTrack* newTrack)
{
    if (track == newTrack)
        return;

    track = newTrack;

    auto* volumePlugin = newTrack != nullptr ? newTrack->getVolumePlugin() : nullptr;
    volumeAttachment.setParameter (volumePlugin, volumePlugin != nullptr ? volumePlugin->volParam->paramID : juce::String());
    panAttachment.setParameter (volumePlugin, volumePlugin != nullptr ? volumePlugin->panParam->paramID : juce::String());

    auto* meterPlugin = newTrack != nullptr ? newTrack->getLevelMeterPlugin() : nullptr;
    meter.setSource (meterPlugin != nullptr ? &meterPlugin->measurer : nullptr);

    timerCallback();
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

    nameLabel.setText (t != nullptr ? t->getName() : juce::String ("No track"), juce::dontSendNotification);
    volumeAttachment.update();
    panAttachment.update();

    muteButton.setEnabled (t != nullptr);
    soloButton.setEnabled (t != nullptr);
    muteButton.setToggleState (t != nullptr && t->isMuted (false), juce::dontSendNotification);
    soloButton.setToggleState (t != nullptr && t->isSolo (false), juce::dontSendNotification);
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

void ChannelStrip::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced (Space::s);

    nameLabel.setBounds (area.removeFromTop (controlHeight));
    pan.setBounds (area.removeFromTop (knobSize).withSizeKeepingCentre (knobSize, knobSize));
    area.removeFromTop (Space::s);

    auto buttons = area.removeFromTop (controlHeight);
    muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (Space::xs / 2, 0));
    soloButton.setBounds (buttons.reduced (Space::xs / 2, 0));
    area.removeFromTop (Space::s);

    // The fader and the meter side by side, filling the rest.
    auto faderArea = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), 56), area.getHeight());
    meter.setBounds (faderArea.removeFromRight (meterWidth));
    volume.setBounds (faderArea);
}

//==============================================================================
Inspector::Inspector (std::function<te::Edit*()> editGetter, te::SelectionManager& selectionManager)
    : getEdit (std::move (editGetter)),
      selection (selectionManager)
{
    setTitle ("Inspector");
    setDescription ("The settings of the selected track.");
    addAndMakeVisible (strip);
    selection.addChangeListener (this);
    startTimerHz (2);
}

Inspector::~Inspector()
{
    selection.removeChangeListener (this);
}

void Inspector::showTrack()
{
    te::AudioTrack* track = nullptr;

    if (auto* edit = getEdit())
    {
        track = getSelectedAudioTrack (selection);

        if (track == nullptr)
            track = getLiveMidiTrack (*edit);

        if (track == nullptr)
            if (const auto tracks = te::getAudioTracks (*edit); ! tracks.isEmpty())
                track = tracks.getFirst();
    }

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
    strip.setBounds (area.removeFromLeft (stripWidth).withHeight (juce::jmin (area.getHeight(), 420)));
}
} // namespace phazite::ui
