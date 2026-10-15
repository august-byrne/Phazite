// SPDX-License-Identifier: AGPL-3.0-or-later
// A channel's controls, as the mixer and the inspector show them (docs/design.md, section 8), top to bottom in the
// order the sound goes through them, as in Logic Pro and Studio One: the track's name and what feeds it, its
// instrument, its effects (each with a few key controls), its sends, where its sound goes, pan, mute, solo, fader
// and meter. The master's strip has its effects, fader and meter, and the loudness meter.
#pragma once

#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"

#include <vector>

namespace phazite
{
class ProjectContext;
} // namespace phazite

namespace phazite::ui
{
// A small heading over a group of a strip's controls, such as "Effects", in the theme's muted text color. Screen
// readers skip it: the group it heads has the same name.
class StripHeading : public juce::Component
{
public:
    explicit StripHeading (const juce::String& text);
    void paint (juce::Graphics&) override;

private:
    juce::String text;
};

//==============================================================================
// A channel's devices of one kind, each as a slot: its name (click to open its window, right-click for more), an
// on/off switch and its key controls, and a button that adds one. A track has an instrument, which turns its
// notes into sound, and effects, which change the sound; folders, returns and the master have effects only.
class DeviceSlots : public juce::Component
{
public:
    enum class Kind
    {
        instrument, // a track's instrument, or a button that chooses one
        effects
    };

    explicit DeviceSlots (Kind);
    ~DeviceSlots() override;

    // The track whose devices to show, or with nullptr the master's of the Edit.
    void setChannel (te::Track*, te::Edit*);

    // Rebuilds the slots if the devices changed, and shows the controls' values. Call it from a timer.
    void update();

    int getPreferredHeight() const;
    void resized() override;
    void lookAndFeelChanged() override; // the add button's color

private:
    struct Slot;

    const Kind kind;
    te::SafeSelectable<te::Track> track;
    te::SafeSelectable<te::Edit> edit;
    juce::OwnedArray<Slot> slots;
    juce::TextButton addButton;
    std::vector<te::EditItemID> shownDevices; // the devices the slots show
    int shownNeed = -1;                       // whether the add button asks for an instrument, as it shows now

    juce::Array<te::Plugin*> getChannelDevices() const;
    void rebuild();
    void updateAddButton();
    void showAddMenu();
    void showSlotMenu (te::Plugin&);
};

//==============================================================================
class ChannelStrip : public juce::Component,
                     public juce::DragAndDropTarget,
                     private juce::Timer
{
public:
    static constexpr int width = 128;

    // How tall the strip must be for its parts not to overlap; the mixer scrolls when it is shorter.
    static int getMinimumHeight();

    ChannelStrip (te::Engine&, CommandBridge&, te::SelectionManager&);
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

//==============================================================================
class MasterStrip : public juce::Component,
                    public juce::DragAndDropTarget,
                    private juce::Timer
{
public:
    static constexpr int width = 156;
    static int getMinimumHeight(); // as ChannelStrip's

    MasterStrip();

    void setEdit (te::Edit*);

    void paint (juce::Graphics&) override;
    void resized() override;

    // An effect dragged from the browser goes onto the master.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }
    void itemDropped (const SourceDetails&) override;

private:
    te::SafeSelectable<te::Edit> edit;
    bool dropping = false;

    void setDropping (bool);
    StripHeading effectsHeading { "Effects" };
    DeviceSlots effects { DeviceSlots::Kind::effects };
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
    Inspector (te::Engine&, CommandBridge&, ProjectContext&);
    ~Inspector() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ProjectContext& projectContext;
    te::SelectionManager& selection; // the project's
    ChannelStrip strip;
    juce::Viewport stripView; // scrolls the strip up and down when the panel is shorter than it needs

    void showTrack();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { showTrack(); } // the selection changed
    void timerCallback() override { showTrack(); }                                   // or live MIDI moved
};
} // namespace phazite::ui
