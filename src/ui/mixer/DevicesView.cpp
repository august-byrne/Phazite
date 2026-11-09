// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DevicesView.h"
#include "ContainerEditor.h"
#include "StripParts.h"
#include "model/arrangement/SongSelection.h"
#include "model/automation/AutomationModel.h"
#include "model/mixer/ContainerModel.h"
#include "model/modulation/ModulationModel.h"
#include "ui/components/ModulatorDrag.h"
#include "ui/components/PowerSwitch.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/devices/DrumSamplerEditor.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <functional>
#include <limits>

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int columnWidth = 168;            // the channel's name, its devices and its modulators
    constexpr int slotHeight = 24, slotGap = 2; // a device's line in the column
    constexpr int extrasHeight = 26;            // under it while it shows: its Presets, its window's button and its sidechain
    constexpr int indent = Space::m;            // a container's devices under it
    constexpr int mixSize = 20;                 // an effect's mix, a small knob at its line's end
    constexpr int presetsWidth = 60, windowButtonWidth = 24, foldWidth = 16;
    constexpr int keyKnobColumns = 4, maxKeyKnobs = 8; // a plug-in's key knobs
    constexpr int labelHeight = 18;                    // a knob's name under it
    constexpr int nameHeight = 20, kindHeight = 16;    // the channel's name and kind, atop the column
    constexpr int areaMargin = Space::xs;              // around what shows beside the column, as the dock is short
    constexpr int macroWidth = 64, macroKnobSize = 36; // a container's macros
    constexpr int macroHeight = macroKnobSize + labelHeight;
    constexpr int dragThreshold = 5;          // pixels a press on a name moves before it carries the device
    constexpr int chainHeaderHeight = 20;     // a container's chain's line, over its devices
    constexpr float chainSilentAlpha = 0.55f; // a chain that isn't heard, muted or soloed out

    //==============================================================================
    // A plug-in's key knobs, which the model offers, each named (its mix is in its slot): its own controls are in its window.
    struct KeyKnobs : public juce::Component
    {
        KeyKnobs (ProjectContext& context, const DeviceSlot& device)
        {
            for (auto& control : getKeyControls (context, device.id, maxKeyKnobs))
                addParameterKnob (*this, knobs, attachments, std::move (control));

            for (auto* knob : knobs)
            {
                auto* name = names.add (new juce::Label ({}, knob->getTitle()));
                name->setJustificationType (juce::Justification::centred);
                name->setFont (juce::FontOptions (TextSize::small));
                name->setInterceptsMouseClicks (false, false);
                addAndMakeVisible (name);
            }

            const auto rows = std::max (1, (knobs.size() + keyKnobColumns - 1) / keyKnobColumns);
            setSize (keyKnobColumns * (knobSize + Space::l) + Space::l, rows * (knobSize + labelHeight + Space::s) + Space::l);
        }

        void updateValues()
        {
            for (auto* attachment : attachments)
                attachment->update();
        }

        void resized() override
        {
            for (int i = 0; i < knobs.size(); ++i)
            {
                const auto x = Space::l + (i % keyKnobColumns) * (knobSize + Space::l);
                const auto y = Space::l + (i / keyKnobColumns) * (knobSize + labelHeight + Space::s);
                knobs[i]->setBounds (x, y, knobSize, knobSize);
                names[i]->setBounds (x - Space::s, y + knobSize, knobSize + 2 * Space::s, labelHeight);
            }
        }

        juce::OwnedArray<Knob> knobs;
        juce::OwnedArray<ParameterAttachment> attachments;
        juce::OwnedArray<juce::Label> names;
    };

    //==============================================================================
    // A container's macro: its knob, its name (double-click to rename it), and a grip to drag it by onto a knob of the
    // container's devices.
    struct Macro : public juce::Component
    {
        Macro (ProjectContext& c, const ModulatorInfo& info)
            : context (c),
              id (info.id),
              knob (info.name, 0.0),
              grip (info.id, info.name,
                    "Drag onto a knob of this container's devices: the macro then turns it too. The knobs it can turn light up")
        {
            if (const auto parameters = getModulatorParameters (context, id); ! parameters.empty())
                attachment.setParameter (getModulatorParameter (context, id, parameters.front().id));

            knob.setTooltip ("Turns what it modulates in the container: drag its grip onto a knob of the container's devices, or "
                             "right-click a knob > Modulate");
            label.setJustificationType (juce::Justification::centred);
            label.setFont (juce::FontOptions (TextSize::small));
            label.setEditable (false, true, false);
            label.setTooltip ("Double-click to rename it");
            label.onTextChange = [this] { renameMacro (context, id, label.getText()); };
            show (info.name);

            addAndMakeVisible (grip);
            addAndMakeVisible (knob);
            addAndMakeVisible (label);
        }

        void show (const juce::String& macroName)
        {
            if (! label.isBeingEdited())
                label.setText (macroName, juce::dontSendNotification);

            knob.setTitle (macroName);
            grip.chipText = macroName;
        }

        void paintOverChildren (juce::Graphics& g) override
        {
            if (grip.isDragging())
                outlineDragSource (g, *this);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            label.setBounds (area.removeFromBottom (labelHeight));
            grip.setBounds (area.removeFromLeft (ModulatorGrip::width));
            knob.setBounds (area.withSizeKeepingCentre (macroKnobSize, macroKnobSize));
        }

        ProjectContext& context;
        const ModulatorID id;
        Knob knob;
        ParameterAttachment attachment { knob };
        ModulatorGrip grip;
        juce::Label label;
    };

    //==============================================================================
    // A device's name in its line, flat, as a list's row is: its line shows it under the pointer and while it shows.
    struct FlatName : public MenuButton
    {
        void paintButton (juce::Graphics& g, bool, bool) override
        {
            const auto& p = getPalette (*this);

            if (hasKeyboardFocus (false))
            {
                g.setColour (p.focus);
                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius, 1.0f);
            }

            g.setColour ((getToggleState() ? p.textBright : p.text).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
            g.setFont (juce::FontOptions (TextSize::body));
            g.drawText (getButtonText(), getLocalBounds().withTrimmedLeft (Space::xs), juce::Justification::centredLeft, true);
        }
    };

    //==============================================================================
    // A word in place of a device beside the column: why nothing shows, or what to do.
    std::unique_ptr<juce::Label> makeNote (const juce::String& text)
    {
        auto note = std::make_unique<juce::Label> (text, text);
        note->setJustificationType (juce::Justification::topLeft);
        note->setFont (juce::FontOptions (TextSize::body));
        note->setSize (360, 60);
        return note;
    }
} // namespace

//==============================================================================
// A list of devices in the column, one under another as FL Studio's mixer stacks its slots: a channel's effects, with a
// button after them that adds one; or a container's chain's devices, under the container's line, indented, with its own
// button (and an instrument container's chain without an instrument, a button that chooses one, first). A device's name
// dragged moves it among its neighbours, or into another chain of its container, a line showing where meanwhile.
struct DevicesView::DeviceList : public juce::Component,
                                 public juce::DragAndDropTarget
{
    DeviceList (DevicesView&, TrackID channelTrack, DeviceID container, DeviceID chain);

    void refresh(); // slots for the devices as they are now: new ones if they changed, else shown again
    void updateValues();
    int getPreferredHeight() const;
    void resized() override;

    std::vector<DeviceSlot> getDevices() const;
    Slot* findSlot (DeviceID) const; // here or in a container here

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;
    void paintOverChildren (juce::Graphics&) override;
    int getDropIndex (juce::Point<int>) const; // the slot a drop there goes before

    DevicesView& view;
    const TrackID track;
    const DeviceID container, chain; // a container's chain's, or none for a channel's effects
    juce::OwnedArray<Slot> slots;
    std::vector<DeviceID> order;
    bool refreshed = false; // once: then a device that appears is one just added, and shows
    juce::TextButton addButton { "+ Effect" }, instrumentButton { "+ Instrument" };
    int dropIndex = -1;
};

//==============================================================================
// A container's chain's line in the column, over its devices, while the container has more than one: its switch for mute
// and solo, as a track's, and its name (click to show the container's chains beside the column, double-click to rename it,
// right-click for its menu), with what part of the sound it plays in its tooltip (the column is narrow; the chains beside it
// show it).
struct DevicesView::ChainHeader : public juce::Component
{
    ChainHeader (DevicesView&, DeviceID container, const ChainInfo&);

    void show (const ChainInfo&);
    void mouseDown (const juce::MouseEvent&) override;
    void paint (juce::Graphics&) override;
    void resized() override;

    DevicesView& view;
    const DeviceID container, chain;
    ChainSwitch power;
    juce::Label name;
};

//==============================================================================
// A container's chains in the column, under its line, indented: each chain's devices, under its line while there are more
// chains than one (ChainHeader); then a button that adds a chain.
struct DevicesView::ContainerChains : public juce::Component
{
    ContainerChains (DevicesView&, TrackID channelTrack, DeviceID container);

    void refresh(); // lists and lines for the chains as they are now: new ones if they changed, else shown again
    void updateValues();
    int getPreferredHeight() const;
    void resized() override;
    Slot* findSlot (DeviceID) const;

    DevicesView& view;
    const TrackID track;
    const DeviceID container;
    juce::OwnedArray<ChainHeader> headers; // while there are more chains than one
    juce::OwnedArray<DeviceList> lists;
    std::vector<DeviceID> shown;
    juce::TextButton addButton { "+ Chain" };
};

//==============================================================================
// A device's line in the column: an effect's switch, a container's chevron, its name (click to show it beside the column
// and select it, right-click for its menu, drag to move it) and its mix; while it shows, lit, with its Presets, its window's
// button and its sidechain under them. An instrument's has no switch (its track's mutes it) and shows its extras always, as
// it is alone. A container's devices follow under it, indented, until its chevron folds them.
struct DevicesView::Slot : public juce::Component
{
    Slot (DevicesView& v, const DeviceSlot& device, DevicePlace p, TrackID channelTrack)
        : view (v),
          place (p),
          track (channelTrack),
          shown (device)
    {
        setTitle (device.name);
        setRepaintsOnMouseActivity (true);
        power.setTooltip ("Turns the device on or off");
        power.onClick = [this] { setDeviceOn (view.context, shown.id, power.getToggleState()); };

        nameButton.onClickedWith = [this] (const juce::ModifierKeys& mods)
        {
            if (std::exchange (dragged, false)) // a drag that moved it, let go over its name
                return;

            if (mods.isPopupMenu())
                view.showMenu (*this);
            else
                view.select (shown, mods);
        };

        // An effect's switch, and a drag on its name moves it; an instrument has neither (a track's switch mutes it, or its
        // chain's, an instrument container's chain's instrument, which stays first in its chain).
        if (place != DevicePlace::instrument && ! device.isInstrument)
        {
            addAndMakeVisible (power);
            nameButton.addMouseListener (this, false);
        }

        // Every device's mix, in one place: its line, as FL Studio's mixer slots have it.
        if (device.hasMix)
            addMixKnob (*this, mixKnob, mixAttachment, view.context, device);

        // A container's chevron, which folds its devices' lines under it.
        if (device.isContainer)
        {
            foldButton = std::make_unique<IconButton> ("Fold its devices", Icon::expanded);
            foldButton->setQuiet (true);
            foldButton->onClick = [this]
            {
                if (view.foldedContainers.erase (shown.id) == 0)
                    view.foldedContainers.insert (shown.id);

                view.refresh();
            };
            addAndMakeVisible (*foldButton);
            inside = std::make_unique<ContainerChains> (view, track, device.id);
            addAndMakeVisible (*inside);
        }

        // Its presets, Phazite's and the user's, and Save Preset... (a container's load as new ones, from the browser).
        if (! device.isContainer)
        {
            presetsButton = std::make_unique<juce::TextButton> ("Presets");
            presetsButton->setTitle ("Presets");
            presetsButton->setTooltip ("Phazite's presets for it, by kind of sound, and yours: the one you pick takes the place of its "
                                       "settings. And Save Preset...");
            presetsButton->onClick = [this]
            {
                if (view.onShowPresets != nullptr)
                    view.onShowPresets (shown.id, shown.name, *presetsButton);
            };
            addChildComponent (*presetsButton);
        }

        // What its sidechain listens to: the one place to choose it (ui/devices/SidechainMenus.h).
        if (device.hasSidechain)
        {
            sidechainButton = std::make_unique<SidechainButton> (view.context, device.id);
            const juce::Component::SafePointer<DevicesView> safeView (&view);
            sidechainButton->onChange = [safeView]
            {
                if (safeView != nullptr)
                    safeView->songChanged ({ .devices = true });
            };
            addChildComponent (*sidechainButton);
        }

        // A plug-in's own window; or one of Phazite's own in a window of its own when the user wants, which then shows it:
        // beside the column, a word says so meanwhile (app/PluginWindow.h).
        if (device.opensWindow)
        {
            windowButton = std::make_unique<IconButton> ("Open its window", Icon::detach);
            windowButton->setTooltip ("Opens the plug-in's own window, with all its controls");
            windowButton->onClick = [this] { showDeviceWindow (view.context, shown.id); };
        }
        else if (device.popsOut)
        {
            windowButton = std::make_unique<IconButton> ("Open in a window", Icon::detach);
            windowButton->setClickingTogglesState (false);
            windowButton->onClick = [this] { view.popOut (shown); };
        }

        if (windowButton != nullptr)
            addChildComponent (*windowButton);

        addAndMakeVisible (nameButton);
        show (device);
    }

    // Shows it as it is now.
    void show (const DeviceSlot& device)
    {
        shown = device;
        setTitle (device.name);
        nameButton.setButtonText (device.name);
        nameButton.setTitle (device.name);
        const auto moves = place == DevicePlace::instrument ? juce::String() : juce::String ("; drag it to move it");
        nameButton.setTooltip (device.frozen        ? "Frozen with its track: unfreeze the track (its snowflake) to change it"
                               : device.opensWindow ? "Click to show its key knobs and open its window; right-click for its menu" + moves
                                                    : "Click to show it; right-click for its menu" + moves);
        power.setToggleState (device.isOn, juce::dontSendNotification);

        if (sidechainButton != nullptr)
            sidechainButton->show (device.sidechain);

        if (windowButton != nullptr && device.popsOut)
        {
            windowButton->setToggleState (device.windowOpen, juce::dontSendNotification);
            windowButton->setTooltip (device.windowOpen ? "It is in its own window: click to bring the window to the front"
                                                        : "Shows it in a window of its own, out of the dock");
        }

        if (foldButton != nullptr)
        {
            const auto folded = isFolded();
            foldButton->setIcon (folded ? Icon::folded : Icon::expanded);
            foldButton->setTitle (folded ? "Show its devices" : "Fold its devices");
            foldButton->setTooltip (folded ? "Shows the container's devices under it" : "Folds the container's devices under it");
        }

        // A frozen device's sound is in its track's file: its controls are off until the track is unfrozen.
        for (auto* control :
             std::initializer_list<juce::Component*> { &power, windowButton.get(), sidechainButton.get(), presetsButton.get() })
            if (control != nullptr)
                control->setEnabled (! device.frozen);

        for (auto* knob : mixKnob)
            knob->setEnabled (! device.frozen);

        if (inside != nullptr)
            inside->refresh();

        repaint();
    }

    bool isShownBeside() const { return view.getShownDevice() == shown.id; }
    bool hasExtras() const { return (place == DevicePlace::instrument || isShownBeside()) && ! shown.isContainer; } // a container has none
    bool isFolded() const { return view.foldedContainers.count (shown.id) > 0; }

    int getPreferredHeight() const
    {
        return slotHeight + (hasExtras() ? extrasHeight : 0)
               + (inside != nullptr && ! isFolded() ? slotGap + inside->getPreferredHeight() : 0);
    }

    void updateValues()
    {
        if (mixAttachment != nullptr)
            mixAttachment->update();

        if (inside != nullptr)
            inside->updateValues();
    }

    void paint (juce::Graphics& g) override
    {
        // Its line: lit while it shows, a wash under the pointer; while it is selected, washed in the accent as a mixer slot is
        // (DeviceSlots), and outlined in it, as a list's row, so that effects Ctrl- or Shift-clicked to group stand out as much
        // as the one that shows (the owner, 2026-11-08).
        const auto& p = getPalette (*this);
        const auto line = getLocalBounds().removeFromTop (slotHeight + (hasExtras() ? extrasHeight : 0)).toFloat();

        if (isShownBeside())
        {
            g.setColour (p.selectedInactive);
            g.fillRoundedRectangle (line, cornerRadius);
        }
        else if (isMouseOver (true))
        {
            g.setColour (p.hover);
            g.fillRoundedRectangle (line, cornerRadius);
        }

        if (view.context.getSongSelection().isSelected (shown.id))
        {
            g.setColour (p.accent.withAlpha (selectionWash));
            g.fillRoundedRectangle (line, cornerRadius);
            g.setColour (p.accent);
            g.drawRoundedRectangle (line.reduced (0.5f), cornerRadius, 1.0f);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // A frozen device shows faint.
        if (shown.frozen)
            g.fillAll (getPalette (*this).surface.withAlpha (1.0f - FrozenTint::frozenDevice));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto line = area.removeFromTop (slotHeight).reduced (Space::xs, 2);
        nameButton.setToggleState (isShownBeside(), juce::dontSendNotification);

        if (foldButton != nullptr)
            foldButton->setBounds (line.removeFromLeft (foldWidth));

        if (power.isVisible())
            power.setBounds (line.removeFromLeft (line.getHeight()));

        if (! mixKnob.isEmpty())
        {
            mixKnob.getFirst()->setBounds (line.removeFromRight (mixSize).withSizeKeepingCentre (mixSize, mixSize));
            line.removeFromRight (Space::xs);
        }

        nameButton.setBounds (line);

        // Under it while it shows: its Presets, its window's button and its sidechain, in that order.
        const auto extras = hasExtras();

        for (auto* control : std::initializer_list<juce::Component*> { presetsButton.get(), windowButton.get(), sidechainButton.get() })
            if (control != nullptr)
                control->setVisible (extras);

        if (extras)
        {
            auto row = area.removeFromTop (extrasHeight).reduced (Space::xs, 1).withTrimmedBottom (Space::xs);

            if (presetsButton != nullptr)
            {
                presetsButton->setBounds (row.removeFromLeft (presetsWidth));
                row.removeFromLeft (Space::xs);
            }

            if (windowButton != nullptr)
            {
                windowButton->setBounds (row.removeFromLeft (windowButtonWidth));
                row.removeFromLeft (Space::xs);
            }

            if (sidechainButton != nullptr)
                sidechainButton->setBounds (row);
        }

        // A container's devices under it, indented, unless they are folded.
        if (inside != nullptr)
        {
            inside->setVisible (! isFolded());
            area.removeFromTop (slotGap);
            inside->setBounds (area.withTrimmedLeft (indent).withHeight (inside->getPreferredHeight()));
        }
    }

    // A drag on its name, past a few pixels, carries it to another place among its neighbours, or into another chain of its
    // container (DeviceList::itemDropped()).
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != &nameButton || dragged || ! e.mods.isLeftButtonDown() || shown.frozen
            || e.getDistanceFromDragStart() < dragThreshold)
            return;

        if (auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragged = true;
            dragContainer->startDragging (describeEffectDrag (shown.id), &nameButton);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Let go away from its name, the button hears no click to forget.
        if (e.eventComponent == &nameButton && ! nameButton.getLocalBounds().contains (e.getPosition()))
            dragged = false;
    }

    DevicesView& view;
    const DevicePlace place;
    const TrackID track;
    DeviceSlot shown;
    bool dragged = false; // its name dragged since the mouse went down: the click that ends it isn't one
    PowerSwitch power;    // an effect's
    FlatName nameButton;
    juce::OwnedArray<Knob> mixKnob;
    std::unique_ptr<MixAttachment> mixAttachment;
    std::unique_ptr<IconButton> foldButton;           // a container's
    std::unique_ptr<juce::TextButton> presetsButton;  // a device's, not a container's
    std::unique_ptr<IconButton> windowButton;         // a plug-in's, or one of Phazite's own that pops out
    std::unique_ptr<SidechainButton> sidechainButton; // a device's that has a sidechain
    std::unique_ptr<ContainerChains> inside;          // a container's chains, with their devices
};

//==============================================================================
DevicesView::DeviceList::DeviceList (DevicesView& v, TrackID channelTrack, DeviceID containerID, DeviceID chainID)
    : view (v),
      track (channelTrack),
      container (containerID),
      chain (chainID)
{
    setTitle (chain.isValid() ? "Chain's devices" : "Effects");
    addButton.onClick = [this]
    {
        if (chain.isValid())
            showChainEffectMenu (view.context, chain, addButton);
        else
            showEffectMenu (view.context, track, addButton);
    };
    addButton.setTitle ("Add effect");
    addButton.setTooltip (chain.isValid() ? "Adds an effect after the chain's devices: built in or a plug-in"
                                          : "Adds an effect, which changes the sound, such as a reverb: built in or a plug-in");
    addAndMakeVisible (addButton);

    // An instrument container's chain without an instrument: a button that chooses one, first.
    instrumentButton.setTitle ("Add instrument");
    instrumentButton.setTooltip ("Chooses this chain's instrument, which plays the notes in its keys and velocities: a synth or a "
                                 "sampler, built in or a plug-in");
    instrumentButton.onClick = [this] { showChainInstrumentMenu (view.context, chain, instrumentButton); };
    addChildComponent (instrumentButton);
    refresh();
}

std::vector<DeviceSlot> DevicesView::DeviceList::getDevices() const
{
    if (! chain.isValid())
        return getEffects (view.context, track);

    const auto info = getChain (view.context, chain);
    return info.has_value() ? info->devices : std::vector<DeviceSlot>();
}

void DevicesView::DeviceList::refresh()
{
    const auto devices = getDevices();

    if (getIDs (devices) != order)
    {
        // A device that wasn't here before, once the list has shown its devices, was just added: it shows (one of Phazite's
        // own, or a container: a plug-in's controls are in its window).
        std::optional<DeviceID> added;

        if (refreshed)
            for (const auto& device : devices)
                if (std::find (order.begin(), order.end(), device.id) == order.end() && ! device.opensWindow)
                    added = device.id;

        slots.clear();
        order = getIDs (devices);

        for (const auto& device : devices)
            addAndMakeVisible (
                slots.add (new Slot (view, device, chain.isValid() ? DevicePlace::inContainer : DevicePlace::effect, track)));

        // On its page: an instrument container's chains' are on the Instrument page, the others on the Effects page.
        if (added.has_value())
            view.rememberShown (*added,
                                chain.isValid() && isInstrumentContainer (view.context, container) ? Page::instrument : Page::effects);
    }
    else
    {
        for (size_t i = 0; i < devices.size(); ++i)
            slots[(int) i]->show (devices[i]);
    }

    refreshed = true;
    addButton.setEnabled (view.context.hasProject());
    instrumentButton.setVisible (
        chain.isValid() && isInstrumentContainer (view.context, container)
        && std::none_of (devices.begin(), devices.end(), [] (const DeviceSlot& device) { return device.isInstrument; }));
    resized();
}

void DevicesView::DeviceList::updateValues()
{
    for (auto* slot : slots)
        slot->updateValues();
}

DevicesView::Slot* DevicesView::DeviceList::findSlot (DeviceID id) const
{
    for (auto* slot : slots)
    {
        if (slot->shown.id == id)
            return slot;

        if (slot->inside != nullptr)
            if (auto* found = slot->inside->findSlot (id))
                return found;
    }

    return nullptr;
}

int DevicesView::DeviceList::getPreferredHeight() const
{
    auto height = instrumentButton.isVisible() ? controlHeight + slotGap : 0;

    for (auto* slot : slots)
        height += slot->getPreferredHeight() + slotGap;

    return height + controlHeight;
}

void DevicesView::DeviceList::resized()
{
    auto area = getLocalBounds();

    if (instrumentButton.isVisible())
    {
        instrumentButton.setBounds (area.removeFromTop (controlHeight).reduced (0, 2));
        area.removeFromTop (slotGap);
    }

    for (auto* slot : slots)
    {
        slot->setBounds (area.removeFromTop (slot->getPreferredHeight()));
        slot->resized(); // as tall as before, perhaps, but showing its extras or not now
        area.removeFromTop (slotGap);
    }

    addButton.setBounds (area.removeFromTop (controlHeight).reduced (0, 2));
}

bool DevicesView::DeviceList::isInterestedInDragSource (const SourceDetails& details)
{
    // One of its own, or an effect of another chain of its container.
    const auto effect = getDraggedEffect (details.description);

    if (! effect.has_value())
        return false;

    return std::find (order.begin(), order.end(), *effect) != order.end()
           || (chain.isValid() && getContainerOf (view.context, *effect) == container);
}

int DevicesView::DeviceList::getDropIndex (juce::Point<int> position) const
{
    for (int i = 0; i < slots.size(); ++i)
        if (position.y < slots[i]->getY() + slotHeight / 2)
            return i;

    return slots.size();
}

void DevicesView::DeviceList::itemDragMove (const SourceDetails& details)
{
    if (const auto index = getDropIndex (details.localPosition); index != dropIndex)
    {
        dropIndex = index;
        repaint();
    }
}

void DevicesView::DeviceList::itemDragExit (const SourceDetails&)
{
    dropIndex = -1;
    repaint();
}

void DevicesView::DeviceList::itemDropped (const SourceDetails& details)
{
    const auto index = getDropIndex (details.localPosition);
    dropIndex = -1;
    repaint();

    const auto effect = getDraggedEffect (details.description);

    if (! effect.has_value())
        return;

    // Among its neighbours, or from another chain of the container into this one, before the device there.
    if (std::find (order.begin(), order.end(), *effect) == order.end())
        moveEffectToChain (view.context, *effect, chain, index < (int) order.size() ? index : -1);
    else if (const auto places = getPlacesMoved (order, *effect, index); places != 0)
        moveEffect (view.context, *effect, places);
    else
        return;

    view.songChanged ({ .devices = true }); // at once, before the song's change comes
}

void DevicesView::DeviceList::paintOverChildren (juce::Graphics& g)
{
    // Where a dragged effect would go: a line between the slots (in an empty chain, along its top).
    if (dropIndex < 0)
        return;

    const auto y = slots.isEmpty()            ? slotGap / 2
                   : dropIndex < slots.size() ? slots[dropIndex]->getY() - slotGap / 2
                                              : slots.getLast()->getBottom() + slotGap / 2;
    g.setColour (getPalette (*this).accent);
    g.fillRect (0, std::max (1, y) - 1, getWidth(), 2);
}

//==============================================================================
DevicesView::ChainHeader::ChainHeader (DevicesView& v, DeviceID containerID, const ChainInfo& info)
    : view (v),
      container (containerID),
      chain (info.id),
      power (v.context)
{
    setTitle (info.name);
    power.setChain (chain);
    addAndMakeVisible (power);

    name.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
    name.setMinimumHorizontalScale (1.0f);
    name.setEditable (false, true, false);
    name.onTextChange = [this] { renameChain (view.context, chain, name.getText()); };
    name.addMouseListener (this, false);
    addAndMakeVisible (name);
    show (info);
}

void DevicesView::ChainHeader::show (const ChainInfo& info)
{
    setTitle (info.name);

    if (! name.isBeingEdited())
        name.setText (info.name, juce::dontSendNotification);

    name.setTitle (info.name);
    name.setTooltip ((info.part.isNotEmpty() ? info.part + ". " : juce::String())
                     + "A chain of the container's, which plays beside its others: click to show them all beside, double-click "
                       "to rename it, right-click for its menu");
    power.update();
    setAlpha (info.heard ? 1.0f : chainSilentAlpha);
}

void DevicesView::ChainHeader::mouseDown (const juce::MouseEvent& e)
{
    if (e.eventComponent != &name)
        return;

    // A right-click: its menu; a click: the container's chains, beside the column.
    if (e.mods.isPopupMenu())
    {
        const juce::Component::SafePointer<ChainHeader> safeThis (this);
        const juce::Component::SafePointer<DevicesView> safeView (&view);
        showChainMenu (
            view.context, chain, name,
            [safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->name.showEditor();
            },
            [safeView]
            {
                if (safeView != nullptr)
                    safeView->songChanged ({ .devices = true });
            });
        return;
    }

    view.context.getSongSelection().selectOnly (container);
    view.setOpen (container, true);
}

void DevicesView::ChainHeader::paint (juce::Graphics& g)
{
    // A line along its top, which sets it off from the chain above.
    g.setColour (getPalette (*this).border);
    g.fillRect (0, 0, getWidth(), 1);
}

void DevicesView::ChainHeader::resized()
{
    auto area = getLocalBounds().withTrimmedTop (1);
    power.setBounds (area.removeFromLeft (area.getHeight()));
    area.removeFromLeft (Space::xs);
    name.setBounds (area);
}

//==============================================================================
DevicesView::ContainerChains::ContainerChains (DevicesView& v, TrackID channelTrack, DeviceID containerID)
    : view (v),
      track (channelTrack),
      container (containerID)
{
    setTitle ("Container's chains");
    addButton.setTitle ("Add chain");
    addButton.setTooltip ("Adds a chain to the container, which plays beside its others, with its own devices, volume, pan, mute "
                          "and solo");
    addButton.onClick = [this]
    {
        addChain (view.context, container);
        view.songChanged ({ .devices = true }); // at once, before the song's change comes
    };
    addAndMakeVisible (addButton);
    refresh();
}

void DevicesView::ContainerChains::refresh()
{
    // A line over each chain's devices while there are more chains than one: one chain is the container's devices alone.
    const auto chains = getChains (view.context, container);

    if (getIDs (chains) != shown)
    {
        headers.clear();
        lists.clear();
        shown = getIDs (chains);

        for (const auto& chain : chains)
        {
            if (chains.size() > 1)
                addAndMakeVisible (headers.add (new ChainHeader (view, container, chain)));

            addAndMakeVisible (lists.add (new DeviceList (view, track, container, chain.id)));
        }
    }
    else
    {
        for (size_t i = 0; i < chains.size(); ++i)
        {
            if ((int) i < headers.size())
                headers[(int) i]->show (chains[i]);

            lists[(int) i]->refresh();
        }
    }

    addButton.setEnabled (canAddChain (view.context, container));
    resized();
}

void DevicesView::ContainerChains::updateValues()
{
    for (auto* list : lists)
        list->updateValues();
}

DevicesView::Slot* DevicesView::ContainerChains::findSlot (DeviceID id) const
{
    for (auto* list : lists)
        if (auto* found = list->findSlot (id))
            return found;

    return nullptr;
}

int DevicesView::ContainerChains::getPreferredHeight() const
{
    auto height = headers.size() * (chainHeaderHeight + slotGap);

    for (auto* list : lists)
        height += list->getPreferredHeight() + slotGap;

    return height + controlHeight;
}

void DevicesView::ContainerChains::resized()
{
    auto area = getLocalBounds();

    for (int i = 0; i < lists.size(); ++i)
    {
        if (i < headers.size())
        {
            headers[i]->setBounds (area.removeFromTop (chainHeaderHeight));
            area.removeFromTop (slotGap);
        }

        lists[i]->setBounds (area.removeFromTop (lists[i]->getPreferredHeight()));
        lists[i]->resized();
        area.removeFromTop (slotGap);
    }

    addButton.setBounds (area.removeFromTop (controlHeight).reduced (0, 2));
}

//==============================================================================
// The column's devices and modulators, which scroll up and down together when they don't fit: under a heading that folds
// them, the instrument (or a button that chooses one) or the effects; then the modulators and macros.
struct DevicesView::Column : public juce::Component
{
    explicit Column (DevicesView& v)
        : view (v),
          modulators (v.context)
    {
        setTitle ("Devices and modulators");

        for (auto* header : { &devicesHeader, &modulatorsHeader })
        {
            header->setToggleState (true, juce::dontSendNotification);
            header->onClick = [this] { view.resized(); };
            addAndMakeVisible (header);
        }

        modulatorsHeader.setTooltip ("Folds the modulators and macros, or shows them again");

        instrumentButton.onClick = [this]
        {
            if (view.channel.has_value())
                showInstrumentMenu (view.context, *view.channel, instrumentButton);
        };
        addChildComponent (instrumentButton);
        addAndMakeVisible (modulators);
    }

    // For the channel and the page, as they are now.
    void refresh()
    {
        const auto onInstrumentPage = view.page == Page::instrument;
        const auto channelTrack = view.channel.value_or (TrackID());

        // The instrument: its slot, made again for another one.
        const auto instrumentNow =
            view.channel.has_value() && channelTrack.isValid() ? getInstrument (view.context, channelTrack) : std::nullopt;

        if (! instrumentNow.has_value())
            instrument = nullptr;
        else if (instrument == nullptr || instrument->shown.id != instrumentNow->id)
            addAndMakeVisible (*(instrument = std::make_unique<Slot> (view, *instrumentNow, DevicePlace::instrument, channelTrack)));
        else
            instrument->show (*instrumentNow);

        // The effects, kept while the channel shows.
        if (! view.channel.has_value())
            effects = nullptr;
        else if (effects == nullptr || effects->track != channelTrack)
            addAndMakeVisible (*(effects = std::make_unique<DeviceList> (view, channelTrack, DeviceID(), DeviceID())));
        else
            effects->refresh();

        view.showInstrumentButton();

        devicesHeader.set (onInstrumentPage ? "Instrument" : "Effects", {},
                           onInstrumentPage || effects == nullptr ? 0 : (int) effects->order.size());
        devicesHeader.setTitle (onInstrumentPage ? "Instrument" : "Effects");
        devicesHeader.setTooltip (onInstrumentPage ? "Folds the instrument, or shows it again" : "Folds the effects, or shows them again");
        modulatorsHeader.set ("Modulators", {}, (int) getModulators (view.context, getModulatorTrack()).size());
        resized();
    }

    TrackID getModulatorTrack() const
    {
        // The master's modulators are its track's.
        return ! view.channel.has_value() ? TrackID() : view.channel->isValid() ? *view.channel : getMasterTrack (view.context);
    }

    juce::Component* getDevices() const
    {
        if (view.page == Page::instrument)
            return instrument != nullptr          ? static_cast<juce::Component*> (instrument.get())
                   : instrumentButton.isVisible() ? const_cast<juce::TextButton*> (&instrumentButton)
                                                  : nullptr;

        return effects.get();
    }

    int getDevicesHeight() const
    {
        if (view.page == Page::instrument)
            return instrument != nullptr ? instrument->getPreferredHeight() : instrumentButton.isVisible() ? controlHeight : 0;

        return effects != nullptr ? effects->getPreferredHeight() : 0;
    }

    int getPreferredHeight() const
    {
        auto height = devicesHeader.getPreferredHeight();

        if (devicesHeader.getToggleState())
            height += getDevicesHeight();

        height += Space::s + modulatorsHeader.getPreferredHeight();

        if (modulatorsHeader.getToggleState())
            height += modulators.getPreferredHeight();

        return height;
    }

    void updateValues()
    {
        if (instrument != nullptr && view.page == Page::instrument)
            instrument->updateValues();

        if (effects != nullptr && view.page == Page::effects)
            effects->updateValues();

        modulators.updateValues();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        devicesHeader.setBounds (area.removeFromTop (devicesHeader.getPreferredHeight()));
        auto* devices = getDevices();

        for (auto* each : std::initializer_list<juce::Component*> { instrument.get(), effects.get() })
            if (each != nullptr && each != devices)
                each->setVisible (false);

        if (devices != nullptr)
        {
            devices->setVisible (devicesHeader.getToggleState());

            if (devices->isVisible())
            {
                devices->setBounds (area.removeFromTop (getDevicesHeight()));
                devices->resized();
            }
        }

        area.removeFromTop (Space::s);
        modulatorsHeader.setBounds (area.removeFromTop (modulatorsHeader.getPreferredHeight()));
        modulators.setVisible (modulatorsHeader.getToggleState());
        modulators.setBounds (area.removeFromTop (modulators.isVisible() ? modulators.getPreferredHeight() : 0));
    }

    DevicesView& view;
    SectionHeader devicesHeader { "Effects", SectionHeader::Style::group };
    SectionHeader modulatorsHeader { "Modulators", SectionHeader::Style::group };
    std::unique_ptr<Slot> instrument;    // the channel's, on the Instrument page
    std::unique_ptr<DeviceList> effects; // its effects, on the Effects page
    juce::TextButton instrumentButton;
    ModulatorSlots modulators;
};

//==============================================================================
// What shows a container, or a device in one, beside the column: the container's name (double-click to rename it) and its
// eight macros, which a grip drags onto a knob of its devices, as Ableton's racks show theirs; beside them, the device of it
// that shows, or for the container itself, a word on where its devices are.
struct DevicesView::ContainerFrame : public juce::Component
{
    ContainerFrame (ProjectContext& c, DeviceID id, std::unique_ptr<juce::Component> shownInside)
        : context (c),
          container (id),
          inner (std::move (shownInside))
    {
        setTitle ("Container");
        name.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
        name.setEditable (false, true, false);
        name.setTooltip ("The container's name: double-click to rename it");
        name.onTextChange = [this] { renameContainer (context, container, name.getText()); };
        addAndMakeVisible (name);

        for (const auto& macro : getContainerMacros (context, container))
            addAndMakeVisible (macros.add (new Macro (context, macro)));

        if (inner != nullptr)
            addAndMakeVisible (*inner);

        refresh();
    }

    void refresh()
    {
        if (! name.isBeingEdited())
            name.setText (getContainerName (context, container), juce::dontSendNotification);

        const auto infos = getContainerMacros (context, container);

        for (size_t i = 0; i < infos.size() && (int) i < macros.size(); ++i)
            macros[(int) i]->show (infos[i].name);

        // The container itself: its chains, as they are now.
        if (auto* chains = dynamic_cast<ContainerEditor*> (inner.get()))
            chains->refresh();
    }

    void updateValues()
    {
        for (auto* macro : macros)
            macro->attachment.update();

        if (auto* knobs = dynamic_cast<KeyKnobs*> (inner.get()))
            knobs->updateValues();
        else if (auto* chains = dynamic_cast<ContainerEditor*> (inner.get()))
            chains->updateValues();
    }

    bool showsWord() const { return dynamic_cast<juce::Label*> (inner.get()) != nullptr; }

    // The macros in two columns beside a device of it, in four beside the word for the container alone.
    int getMacroColumns() const { return showsWord() ? 4 : 2; }
    int getMacrosWidth() const { return 2 * Space::s + getMacroColumns() * macroWidth; }

    void paint (juce::Graphics& g) override
    {
        // The macros' panel, behind them.
        g.setColour (getPalette (*this).surfaceRaised);
        g.fillRoundedRectangle (getLocalBounds().removeFromLeft (getMacrosWidth()).toFloat().reduced (1.0f), cardCornerRadius);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto left = area.removeFromLeft (getMacrosWidth()).reduced (Space::s);
        name.setBounds (left.removeFromTop (controlHeight));
        left.removeFromTop (Space::xs);
        const auto columns = getMacroColumns();

        for (int i = 0; i < macros.size(); ++i)
            macros[i]->setBounds (left.getX() + (i % columns) * macroWidth, left.getY() + (i / columns) * (macroHeight + Space::xs),
                                  macroWidth, macroHeight);

        // The device beside them, as tall as it needs at its width, as one shows beside the column alone.
        if (inner == nullptr)
            return;

        area.removeFromLeft (Space::s);

        if (auto* flexible = dynamic_cast<const FlexibleHeight*> (inner.get()))
            inner->setBounds (area.withHeight (std::max (area.getHeight(), flexible->getMinimumHeightAtWidth (area.getWidth()))));
        else if (showsWord())
            inner->setBounds (area.reduced (Space::s).withHeight (std::min (area.getHeight(), 80)));
        else
            inner->setTopLeftPosition (area.getTopLeft());
    }

    // As tall as the device needs at the width it has, or the macros.
    int getNeededHeight (int width) const
    {
        const auto rows = (macros.size() + getMacroColumns() - 1) / getMacroColumns();
        auto needed = 2 * Space::s + controlHeight + Space::xs + rows * (macroHeight + Space::xs);

        if (auto* flexible = dynamic_cast<const FlexibleHeight*> (inner.get()))
            needed = std::max (needed, flexible->getMinimumHeightAtWidth (width - getMacrosWidth() - Space::s));
        else if (inner != nullptr && ! showsWord())
            needed = std::max (needed, inner->getHeight());

        return needed;
    }

    ProjectContext& context;
    const DeviceID container;
    juce::Label name;
    juce::OwnedArray<Macro> macros;
    std::unique_ptr<juce::Component> inner;
};

//==============================================================================
DevicesView::DevicesView (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Devices");
    setWantsKeyboardFocus (true); // F6 and Shift+F6 give it the keys (and a screen reader its name), and Tab its controls
    nameLabel.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale (1.0f); // a long name ends in "...", whole in its tooltip
    kindLabel.setFont (juce::FontOptions (TextSize::small));
    kindLabel.setMinimumHorizontalScale (1.0f);

    column = std::make_unique<Column> (*this);
    columnView.setTitle ("Devices and modulators in the column"); // what screen readers say as the keys go to them, to scroll them
    columnView.setViewedComponent (column.get(), false);
    columnView.setScrollBarsShown (true, false);
    bodyView.setTitle ("Controls");
    bodyView.setScrollBarsShown (true, false);

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &kindLabel, &columnView, &bodyView })
        addAndMakeVisible (c);

    hint.setFont (juce::FontOptions (TextSize::body));
    hint.setJustificationType (juce::Justification::topLeft);
    addChildComponent (hint);
    lookAndFeelChanged();

    context.addListener (this);
    context.getSongSelection().addChangeListener (this);
    followSelection();
    startTimerHz (15);
}

DevicesView::~DevicesView()
{
    context.getSongSelection().removeChangeListener (this);
    context.removeListener (this);
    bodyView.setViewedComponent (nullptr, false);
    body = nullptr;
    columnView.setViewedComponent (nullptr, false);
    column = nullptr;
}

void DevicesView::lookAndFeelChanged()
{
    const auto& p = getPalette (*this);
    kindLabel.setColour (juce::Label::textColourId, p.textMuted);
    hint.setColour (juce::Label::textColourId, p.textMuted);

    if (auto* note = dynamic_cast<juce::Label*> (body.get()))
        note->setColour (juce::Label::textColourId, p.textMuted);

    if (column != nullptr)
        showInstrumentButton(); // its tint, while the track asks for an instrument
}

juce::Component* DevicesView::findPanel (DeviceID id) const
{
    // On the Instrument page, the instrument, or a device of its instrument container.
    if (page == Page::instrument)
    {
        if (column->instrument == nullptr)
            return nullptr;

        if (column->instrument->shown.id == id)
            return column->instrument.get();

        return column->instrument->inside != nullptr ? column->instrument->inside->findSlot (id) : nullptr;
    }

    return column->effects != nullptr ? column->effects->findSlot (id) : nullptr;
}

juce::Component* DevicesView::getShownBody() const
{
    // A device of a container, beside the container's macros; or the container itself, its macros and its chains.
    if (auto* frame = dynamic_cast<ContainerFrame*> (body.get()))
        return frame->inner != nullptr && ! frame->showsWord() && dynamic_cast<ContainerEditor*> (frame->inner.get()) == nullptr
                   ? frame->inner.get()
                   : frame;

    return body.get();
}

DevicesView::Page DevicesView::getPageOf (DeviceID id) const
{
    // A track's instrument, and the devices of its instrument container.
    if (const auto deviceChannel = getDeviceChannel (context, id); deviceChannel.has_value() && deviceChannel->isValid())
        if (const auto instrument = getInstrument (context, *deviceChannel);
            instrument.has_value() && (instrument->id == id || getContainerOf (context, id) == instrument->id))
            return Page::instrument;

    return Page::effects;
}

DeviceID DevicesView::getShownDevice() const
{
    if (! channel.has_value())
        return {};

    // The instrument; or a device of its instrument container, remembered, while it is still there.
    if (page == Page::instrument)
    {
        if (column->instrument == nullptr)
            return {};

        if (const auto found = shownInstrumentParts.find (*channel); found != shownInstrumentParts.end() && found->second.isValid()
                                                                     && column->instrument->inside != nullptr
                                                                     && column->instrument->inside->findSlot (found->second) != nullptr)
            return found->second;

        return column->instrument->shown.id;
    }

    // The effect remembered, while it is still there; else the first that isn't a plug-in (a plug-in shows when chosen).
    if (column->effects == nullptr)
        return {};

    if (const auto found = shownEffects.find (*channel); found != shownEffects.end())
        return found->second.isValid() && column->effects->findSlot (found->second) != nullptr ? found->second : DeviceID();

    for (auto* slot : column->effects->slots)
        if (! slot->shown.opensWindow)
            return slot->shown.id;

    return {};
}

void DevicesView::showPage (Page newPage)
{
    if (newPage == page)
        return;

    page = newPage;
    refresh();
}

bool DevicesView::isOpen (DeviceID id) const
{
    if (! id.isValid())
        return false;

    // The container of the device that shows is open too, its macros beside it.
    const auto shown = getShownDevice();
    return shown == id || (shown.isValid() && getContainerOf (context, shown) == id);
}

void DevicesView::rememberShown (DeviceID id, Page onPage)
{
    if (channel.has_value())
        (onPage == Page::instrument ? shownInstrumentParts : shownEffects)[*channel] = id;
}

void DevicesView::setOpen (DeviceID id, bool open)
{
    if (! channel.has_value())
        return;

    // In its window, it opens there: one editor at a time.
    if (open && isDeviceWindowOpen (context, id))
    {
        showDeviceWindow (context, id);
        return;
    }

    if (open)
    {
        rememberShown (id, page);

        for (auto inside = getContainerOf (context, id); inside.isValid(); inside = getContainerOf (context, inside))
            foldedContainers.erase (inside);
    }
    else if (isOpen (id))
    {
        rememberShown ({}, page);
    }

    refresh();
}

void DevicesView::showDevice (DeviceID id)
{
    if (const auto deviceChannel = getDeviceChannel (context, id))
        setChannel (deviceChannel);

    // On its page, beside the column; and its slot in view in the column.
    showPage (getPageOf (id));
    setOpen (id, true);

    if (auto* slot = findPanel (id))
    {
        const auto bounds = column->getLocalArea (slot, slot->getLocalBounds());
        auto y = columnView.getViewPositionY();

        if (bounds.getBottom() > y + columnView.getViewHeight())
            y = bounds.getBottom() - columnView.getViewHeight();

        if (bounds.getY() < y)
            y = bounds.getY();

        columnView.setViewPosition (0, std::max (0, y));
    }
}

void DevicesView::select (const DeviceSlot& device, const juce::ModifierKeys& mods)
{
    auto& selection = context.getSongSelection();

    if (mods.isCommandDown() || mods.isShiftDown())
    {
        if (mods.isCommandDown() && selection.isSelected (device.id))
            selection.remove (device.id);
        else
            selection.add (device.id);

        return;
    }

    selection.selectOnly (device.id);

    // It shows beside the column (an instrument, or a device of an instrument container, on the Instrument page); a plug-in's
    // window, or one of Phazite's own devices' that shows it now, comes to the front.
    setOpen (device.id, true);

    if (device.opensWindow || device.windowOpen)
        showDeviceWindow (context, device.id);
}

void DevicesView::popOut (const DeviceSlot& device)
{
    // Out into its window, or the window to the front; beside the column, a word says so meanwhile.
    showDeviceWindow (context, device.id);
    songChanged ({ .devices = true });
}

void DevicesView::showMenu (Slot& slot)
{
    // Its place among its neighbours, for the menu's moves.
    auto index = 0, count = 1;

    if (auto* list = dynamic_cast<DeviceList*> (slot.getParentComponent()))
    {
        const auto& order = list->order;
        index = (int) (std::find (order.begin(), order.end(), slot.shown.id) - order.begin());
        count = (int) order.size();
    }

    const juce::Component::SafePointer<DevicesView> safeThis (this);
    showDeviceMenu (context, commands, slot.shown, slot.place, slot.track, index, count, false, slot.nameButton,
                    [safeThis]
                    {
                        if (safeThis != nullptr)
                            safeThis->songChanged ({ .devices = true });
                    });
}

void DevicesView::showInstrumentButton()
{
    // On the Instrument page, a track without one has a button that chooses one, which asks for one while its notes make no
    // sound.
    auto& button = column->instrumentButton;
    const auto trackChannel = channel.has_value() && channel->isValid() ? phazite::getChannel (context, *channel) : std::nullopt;
    const auto needsButton = page == Page::instrument && trackChannel.has_value() && canHoldInstrument (context, *channel)
                             && ! getInstrument (context, *channel).has_value();
    const auto needs = trackChannel.has_value() && trackChannel->needsInstrument;
    const auto& p = getPalette (*this);
    button.setVisible (needsButton);
    button.setButtonText (needs ? "Choose instrument" : "+ Instrument");
    button.setTooltip (needs ? "This track has notes, but no instrument to play them, so they make no sound. Choose one: a synth or a "
                               "sampler"
                             : "Adds an instrument, which plays the track's notes: a synth or a sampler, built in or a plug-in");
    button.setColour (juce::TextButton::buttonColourId, needs ? p.surfaceRaised.interpolatedWith (p.warning, attentionTint)
                                                              : juce::Colours::transparentBlack); // clear, as other buttons
}

void DevicesView::followSelection()
{
    if (! context.hasProject())
    {
        setChannel ({});
        return;
    }

    // The channel of the device selected, else the selected track's (or live MIDI's: getInspectedChannel()).
    if (const auto devices = context.getSongSelection().getDevices(); ! devices.empty())
    {
        if (const auto deviceChannel = getDeviceChannel (context, devices.front()))
        {
            setChannel (deviceChannel);
            return;
        }
    }

    setChannel (getInspectedChannel (context));
}

void DevicesView::setChannel (std::optional<TrackID> newChannel)
{
    if (newChannel == channel && (column->effects != nullptr) == newChannel.has_value())
        return;

    channel = newChannel;
    column->instrument = nullptr;
    column->effects = nullptr;

    // The master's modulators are its track's.
    column->modulators.setTrack (column->getModulatorTrack());
    showChannelName();
    refresh();
    columnView.setViewPosition (0, 0);
}

void DevicesView::refresh()
{
    column->refresh();
    showBody();
    resized();
    repaint();
}

void DevicesView::showBody()
{
    const auto id = getShownDevice();
    auto* slot = id.isValid() ? static_cast<Slot*> (findPanel (id)) : nullptr;
    const auto* device = slot != nullptr ? &slot->shown : nullptr;
    const auto inWindow = device != nullptr && device->windowOpen;
    const auto frozen = device != nullptr && device->frozen;

    // The same device as it was: only its container's names, which may have changed.
    if (body != nullptr && id == bodyDevice && inWindow == bodyInWindow && frozen == bodyFrozen)
    {
        if (auto* frame = dynamic_cast<ContainerFrame*> (body.get()))
            frame->refresh();

        layOutBody();
        return;
    }

    bodyView.setViewedComponent (nullptr, false);
    body = nullptr;
    bodyDevice = id;
    bodyInWindow = inWindow;
    bodyFrozen = frozen;

    if (device != nullptr)
    {
        // What shows it: one of Phazite's own devices' editor (in its window, a word that says so), a plug-in's key knobs;
        // with its container's macros beside it, if it is in one; a container alone, its chains beside its macros, with a
        // word on where their devices are while they have none.
        std::unique_ptr<juce::Component> shown;
        const auto& p = getPalette (*this);

        if (device->isContainer)
            shown = std::make_unique<ContainerEditor> (context, device->id);
        else if (device->windowOpen)
            shown = makeNote ("It shows in a window of its own: its button at the left brings the window to the front.");
        else if (! device->opensWindow)
            shown = createDeviceEditor (getDeviceHandle (context, device->id));

        // A drum sampler's Swap tries the browser's sounds on a pad (onSwap: the app's browser).
        if (auto* drums = dynamic_cast<DrumSamplerEditor*> (shown.get()); drums != nullptr && onSwap != nullptr)
            drums->onSwap = [this, id = device->id] (int pad)
            {
                if (onSwap != nullptr)
                    onSwap (id, pad);
            };

        if (shown == nullptr)
            shown = std::make_unique<KeyKnobs> (context, *device);

        if (auto* note = dynamic_cast<juce::Label*> (shown.get()))
            note->setColour (juce::Label::textColourId, p.textMuted);

        const auto container = device->isContainer ? device->id : getContainerOf (context, device->id);
        body = container.isValid() ? std::make_unique<ContainerFrame> (context, container, std::move (shown)) : std::move (shown);
        body->setEnabled (! device->frozen);
        body->setAlpha (device->frozen ? FrozenTint::frozenDevice : 1.0f);
    }

    bodyView.setViewedComponent (body.get(), false);

    // Nothing to show: why, or what to do.
    const auto trackChannel = channel.has_value() && channel->isValid() ? phazite::getChannel (context, *channel) : std::nullopt;
    const auto why =
        body != nullptr || ! channel.has_value() ? juce::String()
        : page == Page::effects
            ? juce::String ("Effects change the track's sound, as a reverb or a compressor does: add one with + Effect, at the left. A "
                            "click on one shows it here.")
        : ! channel->isValid() ? juce::String ("The master has no instrument: every track's sound comes to it. Its effects are in the "
                                               "Effects tab.")
        : canHoldInstrument (context, *channel)
            ? juce::String ("An instrument plays the track's notes: a synth or a sampler, built in or a plug-in. Choose one at the left.")
        : trackChannel.has_value() && trackChannel->kind == ChannelKind::returnTrack
            ? juce::String ("A return has no instrument: its sound comes from the tracks that send to it. Its effects are in the "
                            "Effects tab.")
            : juce::String ("A folder has no instrument: its sound comes from its tracks. Its effects are in the Effects tab.");
    hint.setText (why, juce::dontSendNotification);
    hint.setVisible (why.isNotEmpty());
    layOutBody();
}

void DevicesView::layOutBody()
{
    if (body == nullptr)
        return;

    // As wide as the room beside the column and as tall, or as tall as its least at that width (its editor in pages where it is
    // narrower), scrolled up and down only where the dock is shorter than that.
    const auto width = bodyView.getWidth(), height = bodyView.getHeight();
    const auto thickness = bodyView.getScrollBarThickness();

    if (auto* frame = dynamic_cast<ContainerFrame*> (body.get()))
    {
        const auto needed = frame->getNeededHeight (width);
        const auto w = width - (needed > height ? thickness : 0);
        frame->setSize (w, std::max (height, frame->getNeededHeight (w)));
        frame->resized();
    }
    else if (auto* flexible = dynamic_cast<const FlexibleHeight*> (body.get()))
    {
        const auto w = width - (flexible->getMinimumHeightAtWidth (width) > height ? thickness : 0);
        body->setSize (w, std::max (height, flexible->getMinimumHeightAtWidth (w)));
    }
    else if (dynamic_cast<juce::Label*> (body.get()) != nullptr)
    {
        body->setSize (width, std::min (height, 80));
    }
}

void DevicesView::showChannelName()
{
    if (! channel.has_value())
    {
        nameLabel.setText ({}, juce::dontSendNotification);
        kindLabel.setText ("No project open", juce::dontSendNotification);
        return;
    }

    if (! channel->isValid())
    {
        nameLabel.setText ("Master", juce::dontSendNotification);
        nameLabel.setTooltip ("Master");
        kindLabel.setText ("Where every track's sound goes", juce::dontSendNotification);
        return;
    }

    const auto shown = phazite::getChannel (context, *channel);
    nameLabel.setText (shown.has_value() ? shown->name : juce::String(), juce::dontSendNotification);
    nameLabel.setTooltip (nameLabel.getText()); // whole, where it ends in "..."

    const auto kind = ! shown.has_value()                       ? juce::String()
                      : shown->kind == ChannelKind::bus         ? juce::String ("Folder: mixes its tracks")
                      : shown->kind == ChannelKind::plainFolder ? juce::String ("Folder")
                      : shown->kind == ChannelKind::returnTrack ? juce::String ("Return")
                                                                : juce::String ("Track");
    kindLabel.setText (kind, juce::dontSendNotification);
    repaint();
}

bool DevicesView::windowsChanged() const
{
    // A device's window, or one of a container's chains' devices', closed since it showed (they open only from popOut()).
    std::function<bool (const DeviceList&)> search;

    const auto searchInside = [&search] (const Slot& slot)
    {
        if (slot.inside != nullptr)
            for (auto* list : slot.inside->lists)
                if (search (*list))
                    return true;

        return false;
    };

    search = [&searchInside, this] (const DeviceList& list)
    {
        for (auto* slot : list.slots)
            if ((slot->shown.windowOpen && ! isDeviceWindowOpen (context, slot->shown.id)) || searchInside (*slot))
                return true;

        return false;
    };

    if (auto* instrument = column->instrument.get();
        instrument != nullptr
        && ((instrument->shown.windowOpen && ! isDeviceWindowOpen (context, instrument->shown.id)) || searchInside (*instrument)))
        return true;

    return column->effects != nullptr && search (*column->effects);
}

void DevicesView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    // The channel's color, along the column, as a track's header has it; and the line between the column and the rest.
    if (channel.has_value() && channel->isValid())
        if (const auto shown = phazite::getChannel (context, *channel))
        {
            g.setColour (shown->colour);
            g.fillRect (0, 0, colourBarHeight, getHeight());
        }

    g.setColour (p.border);
    g.fillRect (columnWidth - 1, 0, 1, getHeight());
}

void DevicesView::dragOperationStarted (const juce::DragAndDropTarget::SourceDetails& details)
{
    Knob::showModulatorTargets (readModulatorDrag (details.description));
}

void DevicesView::dragOperationEnded (const juce::DragAndDropTarget::SourceDetails&)
{
    Knob::showModulatorTargets ({});
}

void DevicesView::resized()
{
    // The column: the channel's name and kind, then its devices and modulators, scrolled only when they are more than it holds.
    auto area = getLocalBounds();
    auto side = area.removeFromLeft (columnWidth).reduced (Space::s);
    side.removeFromLeft (colourBarHeight);
    nameLabel.setBounds (side.removeFromTop (nameHeight));
    kindLabel.setBounds (side.removeFromTop (kindHeight));
    side.removeFromTop (Space::xs);
    columnView.setBounds (side);
    const auto scrolls = column->getPreferredHeight() > side.getHeight();
    column->setSize (side.getWidth() - (scrolls ? columnView.getScrollBarThickness() : 0), column->getPreferredHeight());
    column->resized();

    // Beside it, the device that shows, or a word in its place: its editor has its own edge at its sides.
    area.reduce (0, areaMargin);
    bodyView.setBounds (area);
    hint.setBounds (area.reduced (Space::s));
    layOutBody();
}

void DevicesView::projectChanged()
{
    shownEffects.clear(); // another project's devices can have the same IDs
    shownInstrumentParts.clear();
    foldedContainers.clear();
    channel.reset();
    bodyView.setViewedComponent (nullptr, false);
    body = nullptr;
    bodyDevice = {};
    column->instrument = nullptr;
    column->effects = nullptr;
    followSelection();
}

void DevicesView::songChanged (const SongChanges& changes)
{
    if (changes.tracks)
    {
        // The channel shown may be gone: follow the selection, which no longer has it.
        if (channel.has_value() && channel->isValid() && ! phazite::getChannel (context, *channel).has_value())
        {
            channel.reset();
            followSelection();
            return;
        }

        showChannelName();
    }

    if (changes.devices || changes.clips || changes.modulation)
    {
        if (changes.modulation || changes.devices)
            column->modulators.refresh(); // and the editor of one that is gone (removed, or its adding undone) closes with its chip

        refresh();
    }
}

void DevicesView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    followSelection();

    // The slots show which devices are selected, those in containers too.
    const std::function<void (juce::Component&)> repaintSlots = [&repaintSlots] (juce::Component& parent)
    {
        for (auto* child : parent.getChildren())
        {
            if (dynamic_cast<Slot*> (child) != nullptr)
                child->repaint();

            if (dynamic_cast<Slot*> (child) != nullptr || dynamic_cast<DeviceList*> (child) != nullptr
                || dynamic_cast<ContainerChains*> (child) != nullptr)
                repaintSlots (*child);
        }
    };

    repaintSlots (*column);
}

void DevicesView::timerCallback()
{
    if (! isShowing())
        return;

    column->updateValues();

    if (auto* knobs = dynamic_cast<KeyKnobs*> (body.get()))
        knobs->updateValues();
    else if (auto* frame = dynamic_cast<ContainerFrame*> (body.get()))
        frame->updateValues();

    // A device's window closed (by its own close button, say): its slot shows it, and its editor beside the column.
    if (windowsChanged())
        refresh();
}
} // namespace phazite::ui
