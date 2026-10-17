// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's controls, as the mixer and the inspector show them (docs/design.md, section 8), top to bottom in the
// order the sound goes through them, as in Logic Pro and Studio One: the track's name and what feeds it, its
// instrument, its effects (each with a few key controls), its sends, where its sound goes, pan, mute, solo, fader
// and meter.
#pragma once

#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"

#include <optional>
#include <vector>

namespace phazite::ui
{
class ChannelStrip : public juce::Component,
                     public juce::DragAndDropTarget,
                     private ProjectContext::Listener,
                     private juce::Timer
{
public:
    static constexpr int width = 128;

    // How tall the strip must be for its parts not to overlap; the mixer scrolls when it is shorter.
    static int getMinimumHeight();

    ChannelStrip (CommandBridge&, ProjectContext&);
    ~ChannelStrip() override;

    // The channel to show (model/mixer/MixerModel.h): a track, a folder (a bus or a plain one) or a return; an empty ID
    // for none. The strip then shows what changes in it, and nothing once it is deleted. Set it again for a channel of
    // another project, even one with the same ID.
    void setTrack (TrackID);
    TrackID getTrack() const noexcept { return track; }

    void setSelected (bool);
    void setRelated (bool); // its sound reaches the selected channel, or the selected channel's reaches it
    bool isRelated() const noexcept { return related; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // selects the track

    // A device or a preset dragged from the browser goes onto this channel (phazite::addDeviceTo(), loadPreset()).
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    struct SendSlot;

    CommandBridge& commands;
    ProjectContext& context;
    TrackID track;
    std::optional<Channel> channel; // the channel as it was when the song last changed; nothing once it is gone
    bool selected = false, related = false, dropping = false;
    bool refreshDue = false, needDue = false; // the song changed: shown at the next tick (refresh(), refreshNeed())

    juce::Label nameLabel, inputLabel, outputLabel; // the name, what the channel is and what feeds it, and where it goes
    juce::TextButton busButton { "Bus" };           // a folder's: whether it mixes its tracks, or only scales them
    StripHeading instrumentHeading { "Instrument" }, effectsHeading { "Effects" }, sendsHeading { "Sends" };
    DeviceSlots instrument, effects;
    juce::OwnedArray<SendSlot> sendSlots;
    juce::TextButton addSendButton { "+ Send" };
    std::vector<DeviceID> shownSends; // the sends the slots show
    juce::Component slotHolder;       // the instrument, effects and sends, scrolling when they don't fit
    juce::Viewport slotView;

    LevelMeter meter { "Level" };
    Fader volume { Fader::Direction::vertical, "Volume", decibelsToFaderPosition (0.0f) };
    Knob pan { "Pan", 0.0 };
    MuteSoloButtons muteSolo;
    ParameterAttachment volumeAttachment { volume }, panAttachment { pan };

    void refresh(); // shows the channel as it is now: its name, kind, devices and sends
    void showChannel();
    void attachControls();
    void refreshSends();
    void layOutSlots();
    void showSendMenu();
    juce::String describeInput() const;
    juce::String describeOutput() const;

    void projectChanged() override {} // the mixer or the inspector gives it a channel of the new project
    void songChanged (const SongChanges&) override;
    void timerCallback() override; // the song's changes, and the controls' values (undo, automation, devices' windows)
};

} // namespace phazite::ui
