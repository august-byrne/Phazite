// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (docs/design.md, section 9; roadmap step 2.5), the editor dock's second tab: the selected track's
// devices in a row, left to right as the sound goes through them, each shown whole, as Ableton Live's Device View and
// Bitwig's Device panel show theirs. First a column with the channel's name and its modulators and macros (the one
// whose name is clicked opens its editor beside them); then its instrument and its effects, each a panel as tall as
// the view: a header with an on/off switch, its name (click to select it, right-click for its menu) and, for a plug-in,
// a button that opens its own window; and under it, one of Phazite's own devices' editor, a container's macros and its
// devices, or a plug-in's key knobs. Buttons after them add an instrument or an effect.
#pragma once

#include "DeviceSlots.h"
#include "ModulatorSlots.h"
#include "StripParts.h"
#include "model/project/ProjectContext.h"

#include <memory>
#include <optional>

namespace phazite::ui
{
class CommandBridge;
class IconButton;

class DevicesView : public juce::Component,
                    public juce::DragAndDropContainer,
                    private ProjectContext::Listener,
                    private juce::ChangeListener,
                    private juce::Timer
{
public:
    DevicesView (CommandBridge&, ProjectContext&);
    ~DevicesView() override;

    // Shows a device's channel (MixerModel.h: getDeviceChannel()), with the device in view. Until another device or
    // track is chosen, it follows the selection: the channel of the device selected, else the selected track's, or the
    // track live MIDI plays (getInspectedChannel()).
    void showDevice (DeviceID);

    // The channel shown: a track, or the master (an empty ID); nothing without a project.
    std::optional<TrackID> getChannel() const noexcept { return channel; }

    // A device's panel, or nullptr if it isn't shown; for tests and to scroll to it.
    juce::Component* findPanel (DeviceID) const;

    void paint (juce::Graphics&) override;
    void resized() override;

    struct Panel;
    struct Row;

private:
    CommandBridge& commands;
    ProjectContext& context;
    std::optional<TrackID> channel;

    juce::Label nameLabel, kindLabel; // the channel's
    StripHeading modulatorsHeading { "Modulators" };
    ModulatorSlots modulators;
    juce::Viewport modulatorsView;
    std::unique_ptr<juce::Component> modulatorEditor; // the one opened beside the modulators, or nothing
    juce::Viewport rowView;                           // the devices, sideways
    std::unique_ptr<Row> row;

    void followSelection(); // shows the channel the selection points to
    void setChannel (std::optional<TrackID>);
    void showChannelName();
    void toggleModulatorEditor (ModulatorID);
    void layOutRow();

    void projectChanged() override;
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
    void timerCallback() override;                                   // the knobs' values and the modulators' meters
};
} // namespace phazite::ui
