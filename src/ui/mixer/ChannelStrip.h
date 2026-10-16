// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's controls, as the mixer and the inspector show them (docs/design.md, section 8), top to bottom in the
// order the sound goes through them, as in Logic Pro and Studio One: the track's name and what feeds it, its
// instrument, its effects (each with a few key controls), its sends, where its sound goes, pan, mute, solo, fader
// and meter.
#pragma once

#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"

#include <vector>

namespace phazite::ui
{
class ChannelStrip : public juce::Component,
                     public juce::DragAndDropTarget,
                     private juce::Timer
{
public:
    static constexpr int width = 128;

    // How tall the strip must be for its parts not to overlap; the mixer scrolls when it is shorter.
    static int getMinimumHeight();

    ChannelStrip (te::Engine&, CommandBridge&, ProjectContext&);
    ~ChannelStrip() override;

    // The track to show: an audio track, a folder (a bus or a plain one) or a return; nullptr for none. The
    // strip notices when the track is deleted, and when its devices or sends change.
    void setTrack (te::Track*);
    te::Track* getTrack() const { return track.get(); }

    void setSelected (bool);
    void setRelated (bool); // its sound reaches the selected channel, or the selected channel's reaches it
    bool isRelated() const noexcept { return related; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // selects the track

    // A device dragged from the browser goes onto this channel (phazite::addDeviceTo()).
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    struct SendSlot;

    te::Engine& engine;
    CommandBridge& commands;
    te::SelectionManager& selection;
    te::SafeSelectable<te::Track> track;
    bool selected = false, related = false, dropping = false;
    juce::Colour shownColour; // the track's color, as the bar along the top shows it

    juce::Label nameLabel, inputLabel, outputLabel; // the name, what the channel is and what feeds it, and where it goes
    juce::TextButton busButton { "Bus" };           // a folder's: whether it mixes its tracks, or only scales them
    StripHeading instrumentHeading { "Instrument" }, effectsHeading { "Effects" }, sendsHeading { "Sends" };
    DeviceSlots instrument { DeviceSlots::Kind::instrument }, effects { DeviceSlots::Kind::effects };
    juce::OwnedArray<SendSlot> sendSlots;
    juce::TextButton addSendButton { "+ Send" };
    std::vector<te::EditItemID> shownSends; // the sends the slots show
    juce::Component slotHolder;             // the instrument, effects and sends, scrolling when they don't fit
    juce::Viewport slotView;

    LevelMeter meter { "Level" };
    Fader volume { Fader::Direction::vertical, "Volume", te::decibelsToVolumeFaderPosition (0.0f) };
    Knob pan { "Pan", 0.0 };
    MuteSoloButtons muteSolo;
    ParameterAttachment volumeAttachment { volume, "Change Volume" }, panAttachment { pan, "Change Pan" };

    void attachControls();
    void rebuildSends();
    void layOutSlots();
    void showSendMenu();
    juce::String describeInput() const;
    juce::String describeOutput() const;
    void timerCallback() override; // shows changes made elsewhere: names, devices, undo, automation
};

} // namespace phazite::ui
