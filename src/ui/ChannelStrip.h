// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's controls, as the mixer and the inspector show them (docs/design.md, section 8): the track's name
// and where it goes, its devices with a few key controls each, its sends, pan, mute, solo, fader and meter.
// The master's strip has its devices, fader and meter, and the loudness meter.
#pragma once

#include "Controls.h"

#include <vector>

namespace phazite::ui
{
// The devices of a channel, each as a slot: its name (click to open its window, right-click for more), an
// on/off switch and its key controls. Shared by track and master strips.
class DeviceSlots : public juce::Component
{
public:
    DeviceSlots (te::Engine&, CommandRegistry&);
    ~DeviceSlots() override;

    // The track whose devices to show, or with nullptr the master's of the Edit.
    void setChannel (te::Track*, te::Edit*);

    // Rebuilds the slots if the devices changed, and shows the controls' values. Call it from a timer.
    void update();

    int getPreferredHeight() const;
    void resized() override;

private:
    struct Slot;

    te::Engine& engine;
    CommandRegistry& commands;
    te::SafeSelectable<te::Track> track;
    te::SafeSelectable<te::Edit> edit;
    juce::OwnedArray<Slot> slots;
    juce::TextButton addButton { "+ Device" };
    std::vector<te::EditItemID> shownDevices; // the devices the slots show

    juce::Array<te::Plugin*> getChannelDevices() const;
    void rebuild();
    void showAddMenu();
    void showSlotMenu (te::Plugin&);
};

//==============================================================================
class ChannelStrip : public juce::Component,
                     private juce::Timer
{
public:
    static constexpr int width = 120;

    // How tall the strip must be for its parts not to overlap; the mixer scrolls when it is shorter.
    static int getMinimumHeight();

    ChannelStrip (te::Engine&, CommandRegistry&, te::SelectionManager&);
    ~ChannelStrip() override;

    // The track to show: an audio track, a folder (a bus or a plain one) or a return; nullptr for none. The
    // strip notices when the track is deleted, and when its devices or sends change.
    void setTrack (te::Track*);
    te::Track* getTrack() const { return track.get(); }

    void setSelected (bool);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // selects the track

private:
    struct SendSlot;

    te::Engine& engine;
    CommandRegistry& commands;
    te::SelectionManager& selection;
    te::SafeSelectable<te::Track> track;
    bool selected = false;
    juce::Colour shownColour; // the track's color, as the bar along the top shows it

    juce::Label nameLabel, outputLabel;
    DeviceSlots devices;
    juce::OwnedArray<SendSlot> sendSlots;
    juce::TextButton addSendButton { "+ Send" };
    std::vector<te::EditItemID> shownSends; // the sends the slots show
    juce::Component slotHolder;             // devices and sends, scrolling when they don't fit
    juce::Viewport slotView;

    LevelMeter meter { "Level" };
    Fader volume { Fader::Direction::vertical, "Volume", te::decibelsToVolumeFaderPosition (0.0f) };
    Knob pan { "Pan", 0.0 };
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    ParameterAttachment volumeAttachment { volume, "Change Volume" }, panAttachment { pan, "Change Pan" };

    void attachControls();
    void rebuildSends();
    void layOutSlots();
    void showSendMenu();
    juce::String describeOutput() const;
    void timerCallback() override; // shows changes made elsewhere: names, devices, undo, automation
    void setMuteOrSolo (bool mute, bool on);
};

//==============================================================================
class MasterStrip : public juce::Component,
                    private juce::Timer
{
public:
    static constexpr int width = 156;
    static int getMinimumHeight(); // as ChannelStrip's

    MasterStrip (te::Engine&, CommandRegistry&);

    void setEdit (te::Edit*);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    te::SafeSelectable<te::Edit> edit;
    DeviceSlots devices;
    juce::Viewport deviceView;
    LevelMeter meter { "Master level" };
    Fader volume { Fader::Direction::vertical, "Master volume", te::decibelsToVolumeFaderPosition (0.0f) };
    ParameterAttachment volumeAttachment { volume, "Change Master Volume" };
    juce::Label loudness;
    juce::TextButton resetButton { "Reset" };

    void timerCallback() override; // the loudness readout, the meter and the fader
};

//==============================================================================
// The panel on the right. It shows the channel of the selected track; with none selected, that of the
// track live MIDI plays, or else the first track.
class Inspector : public juce::Component,
                  private juce::ChangeListener,
                  private juce::Timer
{
public:
    Inspector (te::Engine&, CommandRegistry&, std::function<te::Edit*()> getEdit, te::SelectionManager&);
    ~Inspector() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::function<te::Edit*()> getEdit;
    te::SelectionManager& selection;
    ChannelStrip strip;
    juce::Viewport stripView; // scrolls the strip up and down when the panel is shorter than it needs

    void showTrack();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { showTrack(); } // the selection changed
    void timerCallback() override { showTrack(); }                                   // or live MIDI moved
};
} // namespace phazite::ui
