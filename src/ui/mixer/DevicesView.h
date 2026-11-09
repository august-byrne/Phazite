// SPDX-License-Identifier: AGPL-3.0-or-later
// The Devices view (docs/design.md, section 9; roadmap steps 2.5 and 2.17), the editor dock's Instrument and Effects tabs,
// which it shows as two pages of the selected track's devices. Down its left side, a column with the channel's name, then
// its devices, then its modulators' and macros' chips (a click on one pops its editor out over the window, beside it). On
// the Instrument page the column holds the instrument's name, its Presets and its window's button (the owner, 2026-11-06:
// they took the whole width as a header), or a button that chooses one; on the Effects page, the effects one under
// another, as FL Studio's mixer stacks its slots, scrolled up and down when they are many (the owner, 2026-11-06: in a row,
// the more there were, the less room each had): each an on/off switch, its name (click to show it, right-click for its
// menu, drag to move it) and its mix; the one that shows, its Presets, its window's button and its sidechain under them;
// a container's chains under it, indented, each its devices under its line (while there are more than one), with a button
// after them that adds an effect, then a button that adds a chain; an instrument container's on the Instrument page. Beside
// the column, the whole rest of the dock: the device that shows, whole (one of Phazite's own's editor; a container's
// macros, beside the editor of the device inside it that shows, or beside its chains for the container itself:
// ContainerEditor.h; a plug-in's key knobs, its own controls being in its window), its editor's graphs narrowing, then its
// parts in pages where the dock is narrower, so that it shows without scrolling.
#pragma once

#include "DeviceSlots.h"
#include "ModulatorSlots.h"
#include "StripParts.h"
#include "model/project/ProjectContext.h"
#include "ui/components/SectionHeader.h"

#include <map>
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

    // Shows a device's channel (MixerModel.h: getDeviceChannel()) on the device's page, the device beside the column. Until
    // another device or track is chosen, it follows the selection: the channel of the device selected, else the selected
    // track's, or the track live MIDI plays (getInspectedChannel()).
    void showDevice (DeviceID);

    // The channel shown: a track, or the master (an empty ID); nothing without a project.
    std::optional<TrackID> getChannel() const noexcept { return channel; }

    // A device's slot in the column, or nullptr if it isn't shown there; for tests.
    juce::Component* findPanel (DeviceID) const;

    // What shows the device beside the column (an editor, a container's macros, a plug-in's key knobs), or nullptr.
    juce::Component* getShownBody() const;

    // The page shown: the instrument, or the effects (the editor dock's tab chooses it); and the one a device is on.
    enum class Page
    {
        instrument,
        effects
    };

    void showPage (Page);
    Page getPage() const noexcept { return page; }
    Page getPageOf (DeviceID) const;

    // Whether a device shows beside the column, whole, on the page shown: one does at a time, in its channel (one in a
    // container, beside the container's macros; the container itself, its chains beside them), remembered for the session,
    // an effect on the Effects page, the instrument or a device of its instrument container on the Instrument page. Shown, it
    // opens the containers it is in; and none shows with false (the instrument, then, on the Instrument page).
    bool isOpen (DeviceID) const;
    void setOpen (DeviceID, bool);

    // A device's Presets button, under its name while it shows (an instrument's always), was clicked: shows its presets' menu
    // over the button (the app's browser does: Browser::showDevicePresetMenu()).
    std::function<void (DeviceID, const juce::String& deviceName, juce::Component& target)> onShowPresets;

    // A drum sampler's Swap, in its editor, was clicked for a pad: the app's browser swaps its sound (Browser::startSwap()).
    std::function<void (DeviceID, int pad)> onSwap;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override; // another theme: its colours in it

    // While a macro is dragged from a container here, the knobs it could modulate light up (Knob::showModulatorTargets()).
    void dragOperationStarted (const juce::DragAndDropTarget::SourceDetails&) override;
    void dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&) override;

    struct Slot;
    struct DeviceList;
    struct ChainHeader;
    struct ContainerChains;
    struct Column;
    struct ContainerFrame;

private:
    CommandBridge& commands;
    ProjectContext& context;
    std::optional<TrackID> channel;
    Page page = Page::effects;
    std::map<TrackID, DeviceID> shownEffects;         // the effect each channel shows, remembered for the session
    std::map<TrackID, DeviceID> shownInstrumentParts; // the device of each track's instrument container that shows, likewise
    std::set<DeviceID> foldedContainers;              // whose devices' slots are hidden under them

    juce::Label nameLabel, kindLabel; // the channel's
    juce::Viewport columnView;        // the column's devices and modulators, scrolled up and down when they don't fit
    std::unique_ptr<Column> column;
    juce::Viewport bodyView; // the device that shows, beside the column
    std::unique_ptr<juce::Component> body;
    DeviceID bodyDevice;       // whose it is
    bool bodyInWindow = false; // what it showed: that the device is in its window,
    bool bodyFrozen = false;   // that it is frozen
    juce::Label hint;          // with nothing to show: why, or what to do

    DeviceID getShownDevice() const;     // the instrument (or a device of its instrument container), or the effect that shows
    void rememberShown (DeviceID, Page); // the device that shows on a page, for the channel shown
    void followSelection();              // shows the channel the selection points to
    void setChannel (std::optional<TrackID>);
    void refresh();  // the column's slots, and what shows beside it, as the song is now
    void showBody(); // what shows the shown device, made again if that changed
    void showChannelName();
    void layOutBody();
    void showMenu (Slot&);
    void popOut (const DeviceSlot&);
    void select (const DeviceSlot&, const juce::ModifierKeys&);
    void showInstrumentButton();
    bool windowsChanged() const; // a device's window closed by itself since it showed

    void projectChanged() override;
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed
    void timerCallback() override;                                   // the knobs' values and the windows
};
} // namespace phazite::ui
