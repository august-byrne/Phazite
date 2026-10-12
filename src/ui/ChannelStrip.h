// SPDX-License-Identifier: AGPL-3.0-or-later
// One track's mixer controls, and the inspector that shows them.
#pragma once

#include "Controls.h"

namespace phazite::ui
{
//==============================================================================
// A track's name, level meter, volume fader, pan knob, mute and solo. The mixer (roadmap step 1.4)
// will show one for each track.
class ChannelStrip : public juce::Component,
                     private juce::Timer
{
public:
    ChannelStrip();

    // The track to control, or nullptr. The strip notices when the track is deleted.
    void setTrack (te::AudioTrack*);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    te::SafeSelectable<te::AudioTrack> track;

    juce::Label nameLabel;
    LevelMeter meter { "Track level" };
    Fader volume { Fader::Direction::vertical, "Volume", te::decibelsToVolumeFaderPosition (0.0f) };
    Knob pan { "Pan", 0.0 };
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    ParameterAttachment volumeAttachment { volume, "Change Volume" }, panAttachment { pan, "Change Pan" };

    void timerCallback() override; // shows changes made elsewhere, such as undo, automation or a new name
    void setMuteOrSolo (bool mute, bool on);
};

//==============================================================================
// The panel on the right. Until tracks can be selected (roadmap step 1.1), it shows the channel of
// the track that live MIDI plays, or else the first track.
class Inspector : public juce::Component,
                  private juce::Timer
{
public:
    explicit Inspector (std::function<te::Edit*()> getEdit);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::function<te::Edit*()> getEdit;
    ChannelStrip strip;

    void timerCallback() override; // follows the track
};
} // namespace phazite::ui
