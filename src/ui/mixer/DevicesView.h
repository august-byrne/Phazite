// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (docs/design.md, section 9; roadmap steps 2.5 and 2.17), the editor dock's Instrument and Effects tabs,
// which it shows as two pages: the selected track's instrument, apart, and its effects, left to right as the sound goes
// through them, as Ableton Live's Device View and Bitwig's Device panel show theirs. First, on both, a column with the
// channel's name and its modulators and macros (the one whose name is clicked opens its editor beside them). The
// Instrument page shows the instrument as wide as the dock, under a header in the track's colour, its editor wrapping into
// rows where it is narrower and scrolling up and down, or a button that chooses one. The Effects page shows each effect as
// a card: folded, its header (an on/off switch, a chevron, its name: click to select it, right-click for its menu) over its
// key knobs; one at a time opened to its whole editor (or a container's macros and devices, cards again), so that a long
// chain stays short; a plug-in's card keeps its key knobs, its controls being in its own window. A button after them adds
// an effect.
#pragma once

#include "DeviceSlots.h"
#include "ModulatorSlots.h"
#include "StripParts.h"
#include "model/project/ProjectContext.h"

#include <memory>
#include <optional>
#include <set>

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

    // The page shown: the instrument, or the effects (the editor dock's tab chooses it); and the one a device is on.
    enum class Page
    {
        instrument,
        effects
    };

    void showPage (Page);
    Page getPage() const noexcept { return page; }
    Page getPageOf (DeviceID) const;

    // Whether an effect's card is open, its whole editor showing; opening one folds the others beside it (in its channel,
    // or in its container). Remembered for the session.
    bool isOpen (DeviceID) const;
    void setOpen (DeviceID, bool);

    void paint (juce::Graphics&) override;
    void resized() override;

    struct Panel;
    struct Row;

private:
    CommandBridge& commands;
    ProjectContext& context;
    std::optional<TrackID> channel;
    Page page = Page::effects;
    std::set<DeviceID> openCards; // one at most among a channel's effects, and one among each container's
    juce::Label noInstrument;     // why a channel has none, on the Instrument page

    juce::Label nameLabel, kindLabel; // the channel's
    StripHeading modulatorsHeading { "Modulators" };
    ModulatorSlots modulators;
    juce::Viewport modulatorsView;
    std::unique_ptr<juce::Component> modulatorEditor; // the one opened beside the modulators, or nothing
    juce::Viewport rowView;                           // the devices, sideways
    std::unique_ptr<Row> instrumentRow, effectsRow;   // each page's, kept while the channel shows, so that a page keeps its state
    Row* getShownRow() const noexcept { return page == Page::instrument ? instrumentRow.get() : effectsRow.get(); }

    void followSelection(); // shows the channel the selection points to
    void setChannel (std::optional<TrackID>);
    void showWhyNoInstrument();
    void showChannelName();
    void toggleModulatorEditor (ModulatorID);
    void layOutRow();

    void projectChanged() override;
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
    void timerCallback() override;                                   // the knobs' values and the modulators' meters
};
} // namespace phazite::ui
