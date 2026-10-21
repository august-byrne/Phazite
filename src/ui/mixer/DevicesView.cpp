// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DevicesView.h"
#include "StripParts.h"
#include "model/arrangement/SongSelection.h"
#include "model/automation/AutomationModel.h"
#include "model/mixer/ContainerModel.h"
#include "model/modulation/ModulationModel.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/devices/ModulatorEditor.h"
#include "ui/devices/SidechainMenus.h"
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int columnWidth = 200; // the channel's name and its modulators
    constexpr int powerWidth = 36, windowButtonWidth = 28, sidechainWidth = 104;
    constexpr int keyKnobColumns = 4, maxKeyKnobs = 8; // a plug-in's key knobs
    constexpr int labelHeight = 18;                    // a knob's name under it
    constexpr int addButtonWidth = 110;                // "+ Effect", "Choose instrument"
    constexpr int macroColumns = 4, macroWidth = 72, macroKnobSize = 44;
    constexpr int macroHeight = macroKnobSize + labelHeight;
    constexpr int foldedWidth = 184, foldButtonWidth = 22; // an effect's card, folded
    constexpr int foldedKnobs = 2;                         // its key knobs then, beside its mix in its header

    //==============================================================================
    // A device's key knobs, which the model offers, each named (its mix is in the header): a plug-in's, whose own controls
    // are in its window, or a folded card's, compact.
    struct KeyKnobs : public juce::Component
    {
        KeyKnobs (ProjectContext& context, const DeviceSlot& device, int maximum = maxKeyKnobs, int columnsShown = keyKnobColumns,
                  int sizeShown = knobSize)
            : columns (columnsShown),
              size (sizeShown)
        {
            for (auto& control : getKeyControls (context, device.id, maximum))
                addParameterKnob (*this, knobs, attachments, std::move (control));

            for (auto* knob : knobs)
            {
                auto* name = names.add (new juce::Label ({}, knob->getTitle()));
                name->setJustificationType (juce::Justification::centred);
                name->setFont (juce::FontOptions (TextSize::small));
                name->setInterceptsMouseClicks (false, false);
                addAndMakeVisible (name);
            }

            const auto rows = std::max (1, (knobs.size() + columns - 1) / columns);
            setSize (columns * (size + Space::l) + Space::l, rows * (size + labelHeight + Space::s) + Space::l);
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
                const auto x = Space::l + (i % columns) * (size + Space::l);
                const auto y = Space::l + (i / columns) * (size + labelHeight + Space::s);
                knobs[i]->setBounds (x, y, size, size);
                names[i]->setBounds (x - Space::s, y + size, size + 2 * Space::s, labelHeight);
            }
        }

        const int columns, size;
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
              grip (info.id, "Drag onto a knob of this container's devices: the macro then turns it too")
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
} // namespace

//==============================================================================
// A channel's instrument (or a button that chooses one), on the Instrument page; or devices in a row, left to right as the
// sound goes through them, folded but one: a channel's effects and a button that adds one, or a container's.
struct DevicesView::Row : public juce::Component
{
    // The master's with an empty track and container.
    Row (DevicesView&, TrackID channelTrack, DeviceID container, Page = Page::effects);
    ~Row() override;

    void refresh(); // new panels if the devices changed, else their names and switches
    void updateValues();
    void setOpen (DeviceID, bool); // that card open, the others folded
    int getPreferredWidth() const;
    void resized() override;

    std::vector<DeviceSlot> getDevices() const;
    void showAddMenu();

    DevicesView& view;
    const TrackID track;
    const DeviceID container;
    const Page page;
    juce::OwnedArray<Panel> panels;
    std::vector<DeviceID> shown;
    bool refreshed = false; // once: then a device that appears is one just added, and opens
    juce::TextButton instrumentButton, addButton { "+ Effect" };
    juce::Label instrumentHint; // beside the button that chooses one
};

//==============================================================================
// A container's name, mix and eight macros on the left, as Ableton's racks show theirs, and its devices in a row.
struct ContainerBody : public juce::Component
{
    ContainerBody (DevicesView&, ProjectContext&, const DeviceSlot&);
    ~ContainerBody() override;

    void refresh(); // its name, macros' names and devices
    void updateValues();
    int getPreferredWidth() const;
    void paint (juce::Graphics&) override;
    void resized() override;

    ProjectContext& context;
    const DeviceID container;
    juce::Label name;
    juce::OwnedArray<Knob> mixKnob; // its mix, a dry/wet blend that Tracktion delays to match its devices
    std::unique_ptr<MixAttachment> mixAttachment;
    juce::OwnedArray<Macro> macros;
    std::unique_ptr<DevicesView::Row> devices;
};

//==============================================================================
// A device: its header (an on/off switch, a chevron that folds or opens an effect's card, its name, what its sidechain
// listens to if it has one, its mix and a plug-in's window button) over what shows it: folded, its key knobs; open, the
// whole of it, scrolled up and down when the view is shorter than it. An instrument's header is in its track's colour.
struct DevicesView::Panel : public juce::Component
{
    Panel (DevicesView& v, const DeviceSlot& device, DevicePlace p, TrackID channelTrack)
        : view (v),
          place (p),
          track (channelTrack),
          shown (device)
    {
        setTitle (device.name);
        power.setButtonText ("On");
        power.setTitle ("On");
        power.setTooltip ("Turns the device on or off");
        power.setClickingTogglesState (true);
        power.onClick = [this] { setDeviceOn (view.context, shown.id, power.getToggleState()); };

        nameButton.onClickedWith = [this] (const juce::ModifierKeys& mods)
        {
            if (mods.isPopupMenu())
                showMenu();
            else
                select (mods);
        };

        // An effect's card folds to its key knobs and opens to the whole of it; a plug-in's controls are in its window.
        if (place != DevicePlace::instrument && ! device.opensWindow)
        {
            foldButton = std::make_unique<IconButton> ("Open", Icon::folded);
            foldButton->setQuiet (true);
            foldButton->onClick = [this] { view.setOpen (shown.id, folded); };
            addAndMakeVisible (*foldButton);
        }

        // Every effect's mix, in one place: its header, as FL Studio's mixer slots have it (a container's is beside its macros).
        if (device.hasMix && ! device.isContainer)
            addMixKnob (*this, mixKnob, mixAttachment, view.context, device);

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
            addAndMakeVisible (*sidechainButton);
        }

        if (device.opensWindow)
        {
            windowButton = std::make_unique<IconButton> ("Open its window", Icon::detach);
            windowButton->setTooltip ("Opens the plug-in's own window, with all its controls");
            windowButton->onClick = [this] { showDeviceWindow (view.context, shown.id); };
            addAndMakeVisible (*windowButton);
        }

        // One of Phazite's own devices, in a window of its own when the user wants: its card folds meanwhile, one editor at a
        // time (app/PluginWindow.h).
        if (device.popsOut)
        {
            windowButton = std::make_unique<IconButton> ("Open in a window", Icon::detach);
            windowButton->setClickingTogglesState (false);
            windowButton->onClick = [this] { popOut(); };
            addAndMakeVisible (*windowButton);
        }

        bodyView.setScrollBarsShown (true, false);
        addAndMakeVisible (bodyView);
        addAndMakeVisible (power);
        addAndMakeVisible (nameButton);
        setFolded (device.windowOpen || (foldButton != nullptr && ! view.isOpen (device.id)), true); // in its window: folded from the start
        show (device);
    }

    bool isFoldable() const noexcept { return foldButton != nullptr; }

    // Folds the card to its key knobs, or opens it to the whole of it; an instrument folds only while its window shows it.
    void setFolded (bool shouldFold, bool force = false)
    {
        if (! isFoldable() && ! shown.windowOpen)
            shouldFold = false;

        if (shouldFold == folded && ! force)
            return;

        folded = shouldFold;
        bodyView.setViewedComponent (nullptr, false);
        body = nullptr;

        // What shows it: folded, its key knobs; open, one of Phazite's own devices' editor or a container's macros and
        // devices; a plug-in, its key knobs.
        if (folded)
            body = std::make_unique<KeyKnobs> (view.context, shown, foldedKnobs, foldedKnobs, compactKnobSize);
        else if (shown.isContainer)
            body = std::make_unique<ContainerBody> (view, view.context, shown);
        else if (! shown.opensWindow)
            body = createDeviceEditor (getDeviceHandle (view.context, shown.id));

        if (body == nullptr)
            body = std::make_unique<KeyKnobs> (view.context, shown);

        bodyView.setViewedComponent (body.get(), false);

        if (foldButton != nullptr)
        {
            foldButton->setIcon (folded ? Icon::folded : Icon::expanded);
            foldButton->setTitle (folded ? "Open" : "Fold");
            foldButton->setTooltip (folded ? "Opens it, to all its controls (and folds the one that was open)"
                                           : "Folds it to its key knobs");
        }

        resized();
    }

    void show (const DeviceSlot& device)
    {
        shown = device;
        setTitle (device.name);
        nameButton.setButtonText (device.name);
        nameButton.setTitle (device.name);
        nameButton.setTooltip (device.frozen        ? "Frozen with its track: unfreeze the track (its snowflake) to change it"
                               : device.opensWindow ? "Click to select it, and open its window; right-click for its menu"
                                                    : "Click to select it; right-click for its menu");
        power.setToggleState (device.isOn, juce::dontSendNotification);

        if (sidechainButton != nullptr)
            sidechainButton->show (device.sidechain);

        // In its window: folded here, until the window closes.
        if (device.popsOut && windowButton != nullptr)
        {
            windowButton->setToggleState (device.windowOpen, juce::dontSendNotification);
            windowButton->setTooltip (device.windowOpen ? "It is in its own window: click to bring the window to the front"
                                                        : "Shows it in a window of its own, out of the dock: here it shows its key knobs "
                                                          "meanwhile");

            // One editor at a time: its key knobs here while its window shows it; an instrument whole again after.
            if (device.windowOpen)
                setFolded (true);
            else if (! isFoldable())
                setFolded (false);

            if (foldButton != nullptr)
                foldButton->setVisible (! device.windowOpen);
        }

        // A frozen device's sound is in its track's file: its controls are off until the track is unfrozen.
        for (auto* control : std::initializer_list<juce::Component*> { &power, &bodyView, windowButton.get(), sidechainButton.get() })
            if (control != nullptr)
                control->setEnabled (! device.frozen);

        bodyView.setAlpha (device.frozen ? FrozenTint::frozenDevice : 1.0f);

        if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            containerBody->refresh();

        repaint();
    }

    // Out into its window, or the window to the front; its card, if it was the open one, folds once the window shows it.
    void popOut()
    {
        showDeviceWindow (view.context, shown.id);

        if (isDeviceWindowOpen (view.context, shown.id) && view.isOpen (shown.id))
            view.setOpen (shown.id, false);

        view.songChanged ({ .devices = true }); // its card shows that it is in its window
    }

    // Whether its window, or a window of a device in it, has closed since it showed (they open only from popOut()).
    bool windowsChanged() const
    {
        if (shown.windowOpen && ! isDeviceWindowOpen (view.context, shown.id))
            return true;

        if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            for (auto* panel : containerBody->devices->panels)
                if (panel->windowsChanged())
                    return true;

        return false;
    }

    void select (const juce::ModifierKeys& mods)
    {
        auto& selection = view.context.getSongSelection();

        if (mods.isCommandDown() || mods.isShiftDown())
        {
            if (mods.isCommandDown() && selection.isSelected (shown.id))
                selection.remove (shown.id);
            else
                selection.add (shown.id);

            return;
        }

        selection.selectOnly (shown.id);

        // A plug-in's window, or one of Phazite's own devices' that shows it now, comes to the front.
        if (shown.opensWindow || shown.windowOpen)
            showDeviceWindow (view.context, shown.id);
    }

    void showMenu()
    {
        auto* parentRow = dynamic_cast<Row*> (getParentComponent());

        if (parentRow == nullptr)
            return;

        const auto& order = parentRow->shown;
        const auto index = (int) (std::find (order.begin(), order.end(), shown.id) - order.begin());
        const juce::Component::SafePointer<DevicesView> safeView (&view);
        showDeviceMenu (view.context, view.commands, shown, place, track, index, (int) order.size(), true, nameButton,
                        [safeView]
                        {
                            if (safeView != nullptr)
                                safeView->songChanged ({ .devices = true });
                        });
    }

    void updateValues()
    {
        if (mixAttachment != nullptr)
            mixAttachment->update();

        if (auto* knobs = dynamic_cast<KeyKnobs*> (body.get()))
            knobs->updateValues();
        else if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            containerBody->updateValues();
    }

    // As wide as what shows it at that height (an editor that reflows is wider when shorter), and a scroll bar if it is
    // taller than the view; a folded card, its own width.
    int getBodyWidth (int bodyHeight) const
    {
        if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            return containerBody->getPreferredWidth();

        if (auto* flexible = dynamic_cast<const FlexibleHeight*> (body.get()))
            return flexible->getWidthForHeight (getBodyHeight (bodyHeight));

        return body->getWidth();
    }

    int getPreferredWidth (int height) const
    {
        if (folded)
            return foldedWidth;

        const auto bodyHeight = height - controlHeight;
        const auto scrolls = dynamic_cast<ContainerBody*> (body.get()) == nullptr && getBodyHeight (bodyHeight) > bodyHeight;
        return getBodyWidth (bodyHeight) + (scrolls ? bodyView.getScrollBarThickness() : 0);
    }

    // An editor that can fit less height grows to fill the view (DeviceEditors.h: FlexibleHeight); others keep theirs.
    int getBodyHeight (int available) const
    {
        if (dynamic_cast<ContainerBody*> (body.get()) != nullptr)
            return available;

        if (auto* flexible = dynamic_cast<const FlexibleHeight*> (body.get()))
            return std::max (flexible->getMinimumHeight(), available);

        return body->getHeight();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat();
        g.setColour (p.surfaceRaised);
        g.fillRoundedRectangle (area, cornerRadius);

        // An instrument's header in its track's colour: the instrument apart from the effects.
        if (place == DevicePlace::instrument)
            if (const auto owner = phazite::getChannel (view.context, track))
            {
                g.setColour (owner->colour.withAlpha (TrackTint::header));
                g.fillRoundedRectangle (area.withHeight ((float) controlHeight), cornerRadius);
            }

        // A selected device stands out, as a selected slot does in the mixer.
        if (view.context.getSongSelection().isSelected (shown.id))
        {
            g.setColour (p.accent.withAlpha (selectionWash));
            g.fillRoundedRectangle (area.withHeight ((float) controlHeight), cornerRadius);
            g.setColour (p.accent);
            g.drawRoundedRectangle (area.reduced (0.5f), cornerRadius, 1.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (controlHeight).reduced (Space::xs, 2);
        power.setBounds (header.removeFromLeft (powerWidth));
        header.removeFromLeft (Space::xs);

        if (foldButton != nullptr)
            foldButton->setBounds (header.removeFromLeft (foldButtonWidth));

        if (windowButton != nullptr)
            windowButton->setBounds (header.removeFromRight (windowButtonWidth));

        if (! mixKnob.isEmpty())
            mixKnob.getFirst()->setBounds (header.removeFromRight (header.getHeight()));

        if (sidechainButton != nullptr)
            sidechainButton->setBounds (
                header.removeFromRight (std::min (sidechainWidth, header.getWidth() / 2)).withTrimmedRight (Space::xs));

        nameButton.setBounds (header);
        bodyView.setBounds (area);

        if (body == nullptr)
            return;

        // The instrument, as wide as the dock: an editor that wraps its parts into rows where it is narrower, as tall as
        // that takes, scrolled up and down.
        if (place == DevicePlace::instrument)
        {
            const auto width = area.getWidth() - bodyView.getScrollBarThickness();

            if (auto* flexible = dynamic_cast<const FlexibleHeight*> (body.get()))
                body->setSize (width, std::max ({ flexible->getHeightForWidth (width), flexible->getMinimumHeight(), area.getHeight() }));

            return;
        }

        const auto bodyHeight = getBodyHeight (area.getHeight());
        body->setSize (folded ? area.getWidth() : getBodyWidth (area.getHeight()), bodyHeight);
    }

    DevicesView& view;
    const DevicePlace place;
    const TrackID track;
    DeviceSlot shown;
    bool folded = false;
    juce::TextButton power;
    MenuButton nameButton;
    juce::OwnedArray<Knob> mixKnob; // an effect's
    std::unique_ptr<MixAttachment> mixAttachment;
    std::unique_ptr<IconButton> foldButton;           // an effect's that opens: one of Phazite's own, or a container
    std::unique_ptr<IconButton> windowButton;         // a plug-in's, or one of Phazite's own that pops out
    std::unique_ptr<SidechainButton> sidechainButton; // a device's that has a sidechain
    std::unique_ptr<juce::Component> body;
    juce::Viewport bodyView;
};

//==============================================================================
DevicesView::Row::Row (DevicesView& v, TrackID channelTrack, DeviceID containerID, Page shownPage)
    : view (v),
      track (channelTrack),
      container (containerID),
      page (containerID.isValid() ? Page::effects : shownPage)
{
    setTitle (container.isValid() ? "Container's devices" : page == Page::instrument ? "Instrument" : "Effects");
    instrumentButton.onClick = [this] { showInstrumentMenu (view.context, track, instrumentButton); };
    addButton.onClick = [this] { showAddMenu(); };
    addButton.setTooltip (container.isValid() ? "Adds an effect after the container's devices: built in or a plug-in"
                                              : "Adds an effect, which changes the sound, such as a reverb: built in or a plug-in");
    instrumentHint.setFont (juce::FontOptions (TextSize::small));
    instrumentHint.setText ("An instrument plays the track's notes: a synth or a sampler, built in or a plug-in",
                            juce::dontSendNotification);
    addChildComponent (instrumentButton);
    addChildComponent (instrumentHint);
    addChildComponent (addButton);
    refresh();
}

DevicesView::Row::~Row() = default;

std::vector<DeviceSlot> DevicesView::Row::getDevices() const
{
    if (container.isValid())
        return getContainerDevices (view.context, container);

    if (page == Page::effects)
        return getEffects (view.context, track);

    if (const auto instrument = track.isValid() ? getInstrument (view.context, track) : std::nullopt)
        return { *instrument };

    return {};
}

void DevicesView::Row::refresh()
{
    const auto devices = getDevices();

    if (getIDs (devices) != shown)
    {
        // An effect that wasn't here before, once the row has shown its devices, was just added: it opens (one of Phazite's
        // own, or a container: a plug-in's controls are in its window).
        std::optional<DeviceID> added;

        if (refreshed && page == Page::effects)
            for (const auto& device : devices)
                if (std::find (shown.begin(), shown.end(), device.id) == shown.end() && ! device.opensWindow)
                    added = device.id;

        panels.clear();
        shown = getIDs (devices);
        const auto place = container.isValid()        ? DevicePlace::inContainer
                           : page == Page::instrument ? DevicePlace::instrument
                                                      : DevicePlace::effect;

        for (const auto& device : devices)
            addAndMakeVisible (panels.add (new Panel (view, device, place, track)));

        if (added.has_value())
            view.setOpen (*added, true);
    }
    else
    {
        for (size_t i = 0; i < devices.size(); ++i)
            panels[(int) i]->show (devices[i]);
    }

    refreshed = true;

    // On the Instrument page, a track without one has a button that chooses one, which asks for one while its notes make
    // no sound; the Effects page ends with a button that adds an effect.
    const auto trackChannel = track.isValid() ? phazite::getChannel (view.context, track) : std::nullopt;
    const auto needsButton = page == Page::instrument && track.isValid() && canHoldInstrument (view.context, track) && devices.empty();
    const auto needs = trackChannel.has_value() && trackChannel->needsInstrument;
    const auto& p = getPalette (view);
    instrumentButton.setVisible (needsButton);
    instrumentHint.setVisible (needsButton);
    instrumentButton.setButtonText (needs ? "Choose instrument" : "+ Instrument");
    instrumentButton.setTooltip (needs ? "This track has notes, but no instrument to play them, so they make no sound. Choose one: a "
                                         "synth or a sampler"
                                       : "Adds an instrument, which plays the track's notes: a synth or a sampler, built in or a plug-in");
    instrumentButton.setColour (juce::TextButton::buttonColourId,
                                needs ? p.surfaceRaised.interpolatedWith (p.warning, attentionTint) : p.surfaceRaised);
    addButton.setVisible (page == Page::effects);
    addButton.setEnabled (view.context.hasProject());
    resized();
}

void DevicesView::Row::updateValues()
{
    for (auto* panel : panels)
        panel->updateValues();
}

void DevicesView::Row::setOpen (DeviceID id, bool open)
{
    for (auto* panel : panels)
        if (panel->isFoldable())
            panel->setFolded (! (open && panel->shown.id == id));

    resized();
}

int DevicesView::Row::getPreferredWidth() const
{
    auto width = Space::s;

    for (auto* panel : panels)
        width += panel->getPreferredWidth (getHeight()) + Space::s;

    return width + (addButton.isVisible() ? addButtonWidth + Space::s : 0);
}

void DevicesView::Row::resized()
{
    auto area = getLocalBounds().reduced (0, Space::s).withTrimmedLeft (Space::s);

    // The instrument: the whole page.
    if (page == Page::instrument)
    {
        area = area.withTrimmedRight (Space::s);

        if (instrumentButton.isVisible())
        {
            auto line = area.removeFromTop (controlHeight);
            instrumentButton.setBounds (line.removeFromLeft (addButtonWidth));
            line.removeFromLeft (Space::s);
            instrumentHint.setBounds (line);
        }

        for (auto* panel : panels)
            panel->setBounds (area);

        return;
    }

    for (auto* panel : panels)
    {
        panel->setBounds (area.removeFromLeft (panel->getPreferredWidth (area.getHeight())));
        area.removeFromLeft (Space::s);
    }

    addButton.setBounds (area.removeFromLeft (addButtonWidth).removeFromTop (controlHeight));
}

void DevicesView::Row::showAddMenu()
{
    if (container.isValid())
        showContainerEffectMenu (view.context, container, addButton);
    else
        showEffectMenu (view.context, track, addButton);
}

//==============================================================================
ContainerBody::ContainerBody (DevicesView& view, ProjectContext& projectContext, const DeviceSlot& device)
    : context (projectContext),
      container (device.id)
{
    setTitle ("Container");
    name.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    name.setEditable (false, true, false);
    name.setTooltip ("The container's name: double-click to rename it");
    name.onTextChange = [this] { renameContainer (context, container, name.getText()); };
    addAndMakeVisible (name);

    if (device.hasMix)
        addMixKnob (*this, mixKnob, mixAttachment, context, device);

    for (const auto& macro : getContainerMacros (context, container))
        addAndMakeVisible (macros.add (new Macro (context, macro)));

    devices = std::make_unique<DevicesView::Row> (view, TrackID(), container);
    addAndMakeVisible (*devices);
    refresh();
}

ContainerBody::~ContainerBody() = default;

void ContainerBody::refresh()
{
    if (! name.isBeingEdited())
        name.setText (getContainerName (context, container), juce::dontSendNotification);

    const auto infos = getContainerMacros (context, container);

    for (size_t i = 0; i < infos.size() && (int) i < macros.size(); ++i)
        macros[(int) i]->show (infos[i].name);

    devices->refresh();
}

void ContainerBody::updateValues()
{
    if (mixAttachment != nullptr)
        mixAttachment->update();

    for (auto* macro : macros)
        macro->attachment.update();

    devices->updateValues();
}

int ContainerBody::getPreferredWidth() const
{
    return Space::s + macroColumns * macroWidth + Space::s + devices->getPreferredWidth();
}

void ContainerBody::paint (juce::Graphics& g)
{
    // The macros' panel, behind them.
    if (! macros.isEmpty())
    {
        g.setColour (getPalette (*this).surface);
        g.fillRoundedRectangle (macros.getFirst()->getBounds().getUnion (macros.getLast()->getBounds()).expanded (Space::xs).toFloat(),
                                cornerRadius);
    }
}

void ContainerBody::resized()
{
    auto area = getLocalBounds();
    auto left = area.removeFromLeft (Space::s + macroColumns * macroWidth + Space::s).reduced (Space::s, Space::s);
    auto top = left.removeFromTop (controlHeight);

    if (! mixKnob.isEmpty())
        mixKnob.getFirst()->setBounds (top.removeFromRight (controlHeight));

    name.setBounds (top);
    left.removeFromTop (Space::s);

    for (int i = 0; i < macros.size(); ++i)
        macros[i]->setBounds (left.getX() + (i % macroColumns) * macroWidth, left.getY() + (i / macroColumns) * (macroHeight + Space::s),
                              macroWidth, macroHeight);

    devices->setBounds (area);
}

//==============================================================================
DevicesView::DevicesView (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      modulators (projectContext)
{
    setTitle ("Devices");
    nameLabel.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    kindLabel.setFont (juce::FontOptions (TextSize::small));

    // A modulator's editor opens beside the list, where there is room for it next to the knobs it is dragged onto.
    modulators.onOpenEditor = [this] (ModulatorID id) { toggleModulatorEditor (id); };
    modulatorsView.setViewedComponent (&modulators, false);
    modulatorsView.setScrollBarsShown (true, false);
    rowView.setScrollBarsShown (false, true);

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &kindLabel, &modulatorsHeading, &modulatorsView, &rowView })
        addAndMakeVisible (c);

    noInstrument.setFont (juce::FontOptions (TextSize::body));
    noInstrument.setJustificationType (juce::Justification::topLeft);
    addChildComponent (noInstrument);

    context.addListener (this);
    context.getSongSelection().addChangeListener (this);
    followSelection();
    startTimerHz (15);
}

DevicesView::~DevicesView()
{
    context.getSongSelection().removeChangeListener (this);
    context.removeListener (this);
}

juce::Component* DevicesView::findPanel (DeviceID id) const
{
    const std::function<juce::Component*(const Row&)> search = [&search, id] (const Row& in) -> juce::Component*
    {
        for (auto* panel : in.panels)
        {
            if (panel->shown.id == id)
                return panel;

            if (auto* containerBody = dynamic_cast<ContainerBody*> (panel->body.get()))
                if (auto* found = search (*containerBody->devices))
                    return found;
        }

        return nullptr;
    };

    auto* shown = getShownRow();
    return shown != nullptr ? search (*shown) : nullptr;
}

DevicesView::Page DevicesView::getPageOf (DeviceID id) const
{
    if (const auto deviceChannel = getDeviceChannel (context, id); deviceChannel.has_value() && deviceChannel->isValid())
        if (const auto instrument = getInstrument (context, *deviceChannel); instrument.has_value() && instrument->id == id)
            return Page::instrument;

    return Page::effects;
}

void DevicesView::showPage (Page newPage)
{
    if (newPage == page)
        return;

    page = newPage;
    rowView.setScrollBarsShown (false, page == Page::effects);
    rowView.setViewedComponent (getShownRow(), false);
    showWhyNoInstrument();
    layOutRow();
}

bool DevicesView::isOpen (DeviceID id) const
{
    return openCards.count (id) > 0;
}

void DevicesView::setOpen (DeviceID id, bool open)
{
    // In its window, it opens there: one editor at a time.
    if (open && isDeviceWindowOpen (context, id))
    {
        showDeviceWindow (context, id);
        return;
    }

    // The row that holds it, a channel's or a container's: its other cards fold.
    const std::function<Row*(Row&)> search = [&search, id] (Row& in) -> Row*
    {
        for (auto* panel : in.panels)
        {
            if (panel->shown.id == id)
                return &in;

            if (auto* containerBody = dynamic_cast<ContainerBody*> (panel->body.get()))
                if (auto* found = search (*containerBody->devices))
                    return found;
        }

        return nullptr;
    };

    auto* holder = effectsRow != nullptr ? search (*effectsRow) : nullptr;

    // A plug-in's card doesn't open (its controls are in its window): the open one stays.
    if (holder != nullptr)
        for (auto* panel : holder->panels)
            if (panel->shown.id == id && ! panel->isFoldable())
                return;

    if (holder != nullptr)
        for (auto* panel : holder->panels)
            openCards.erase (panel->shown.id);

    if (open)
        openCards.insert (id);
    else
        openCards.erase (id);

    if (holder != nullptr)
        holder->setOpen (id, open);

    layOutRow();
}

void DevicesView::showDevice (DeviceID id)
{
    if (const auto deviceChannel = getDeviceChannel (context, id))
        setChannel (deviceChannel);

    // On its page; an effect's card open, and its container's, if it is in one.
    showPage (getPageOf (id));

    if (page == Page::effects)
    {
        if (const auto inside = getContainerOf (context, id); inside.isValid())
            setOpen (inside, true);

        setOpen (id, true);
    }

    if (auto* panel = findPanel (id); panel != nullptr && page == Page::effects)
    {
        // Into view, sideways, with as much of it as fits.
        const auto bounds = rowView.getViewedComponent()->getLocalArea (panel, panel->getLocalBounds());
        auto x = rowView.getViewPositionX();

        if (bounds.getRight() > x + rowView.getViewWidth())
            x = bounds.getRight() - rowView.getViewWidth();

        if (bounds.getX() < x)
            x = bounds.getX();

        rowView.setViewPosition (std::max (0, x), 0);
    }
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
    if (newChannel == channel && (effectsRow != nullptr) == newChannel.has_value())
        return;

    channel = newChannel;
    modulatorEditor = nullptr;
    rowView.setViewedComponent (nullptr, false);
    instrumentRow = nullptr;
    effectsRow = nullptr;

    // Both pages, so that each keeps its state (its scroll, its editors' tabs) while the other shows.
    if (channel.has_value())
    {
        instrumentRow = std::make_unique<Row> (*this, *channel, DeviceID(), Page::instrument);
        effectsRow = std::make_unique<Row> (*this, *channel, DeviceID(), Page::effects);
        rowView.setViewedComponent (getShownRow(), false);
    }

    showWhyNoInstrument();

    // The master's modulators are its track's.
    modulators.setTrack (! channel.has_value() ? TrackID() : channel->isValid() ? *channel : getMasterTrack (context));
    showChannelName();
    resized();
}

void DevicesView::showWhyNoInstrument()
{
    // On the Instrument page, a channel that can't hold one says why, and where its effects are.
    const auto shown = channel.has_value() && channel->isValid() ? phazite::getChannel (context, *channel) : std::nullopt;
    const auto why = ! channel.has_value() || page != Page::instrument ? juce::String()
                     : ! channel->isValid() ? juce::String ("The master has no instrument: every track's sound comes to it.")
                     : canHoldInstrument (context, *channel) ? juce::String()
                     : shown.has_value() && shown->kind == ChannelKind::returnTrack
                         ? juce::String ("A return has no instrument: its sound comes from the tracks that send to it.")
                         : juce::String ("A folder has no instrument: its sound comes from its tracks.");
    noInstrument.setText (why.isEmpty() ? why : why + " Its effects are in the Effects tab.", juce::dontSendNotification);
    noInstrument.setVisible (why.isNotEmpty());
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
        kindLabel.setText ("Where every track's sound goes", juce::dontSendNotification);
        return;
    }

    const auto shown = phazite::getChannel (context, *channel);
    nameLabel.setText (shown.has_value() ? shown->name : juce::String(), juce::dontSendNotification);

    const auto kind = ! shown.has_value()                       ? juce::String()
                      : shown->kind == ChannelKind::bus         ? juce::String ("Folder: mixes its tracks")
                      : shown->kind == ChannelKind::plainFolder ? juce::String ("Folder")
                      : shown->kind == ChannelKind::returnTrack ? juce::String ("Return")
                                                                : juce::String ("Track");
    kindLabel.setText (kind, juce::dontSendNotification);
    repaint();
}

void DevicesView::toggleModulatorEditor (ModulatorID id)
{
    auto* shownEditor = dynamic_cast<ModulatorEditor*> (modulatorEditor.get());
    const auto wasShown = shownEditor != nullptr && shownEditor->getModulator() == id;
    modulatorEditor = nullptr;

    if (! wasShown)
    {
        modulatorEditor = std::make_unique<ModulatorEditor> (context, id);
        addAndMakeVisible (*modulatorEditor);
    }

    resized();
}

void DevicesView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    // The channel's color, along its column, as a track's header has it.
    if (channel.has_value() && channel->isValid())
        if (const auto shown = phazite::getChannel (context, *channel))
        {
            g.setColour (shown->colour);
            g.fillRect (0, 0, colourBarHeight, getHeight());
        }

    g.setColour (p.border);
    g.fillRect (columnWidth - 1, 0, 1, getHeight());
}

void DevicesView::resized()
{
    auto area = getLocalBounds();
    auto column = area.removeFromLeft (columnWidth).reduced (Space::s);
    column.removeFromLeft (colourBarHeight);
    nameLabel.setBounds (column.removeFromTop (controlHeight));
    kindLabel.setBounds (column.removeFromTop (labelHeight));
    column.removeFromTop (Space::s);
    modulatorsHeading.setBounds (column.removeFromTop (headingHeight));
    modulatorsView.setBounds (column);
    modulators.setSize (column.getWidth() - modulatorsView.getScrollBarThickness(), modulators.getPreferredHeight());

    if (modulatorEditor != nullptr)
        modulatorEditor->setTopLeftPosition (
            area.removeFromLeft (modulatorEditor->getWidth() + Space::s).getTopLeft().translated (Space::s, Space::s));

    rowView.setBounds (area);
    noInstrument.setBounds (area.reduced (Space::m));
    layOutRow();
}

void DevicesView::layOutRow()
{
    auto* row = getShownRow();

    if (row == nullptr)
        return;

    // The instrument: the whole page, which its panel scrolls up and down.
    if (page == Page::instrument)
    {
        row->setSize (rowView.getWidth(), rowView.getHeight());
        row->resized();
        return;
    }

    // The effects: as wide as their cards, one open (a container's card as wide as its own cards); laid out again even when
    // that width is the same, as a card inside a container that opens or folds changes the container's.
    const auto height = rowView.getHeight() - rowView.getScrollBarThickness();
    row->setSize (row->getWidth(), height); // the panels' widths depend on the height
    row->setSize (std::max (row->getPreferredWidth(), rowView.getWidth()), height);
    row->resized();
}

void DevicesView::projectChanged()
{
    openCards.clear(); // another project's devices can have the same IDs
    channel.reset();
    rowView.setViewedComponent (nullptr, false);
    instrumentRow = nullptr;
    effectsRow = nullptr;
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

    if (changes.devices || changes.clips)
    {
        for (auto* row : { instrumentRow.get(), effectsRow.get() })
            if (row != nullptr)
                row->refresh();

        showWhyNoInstrument();
        layOutRow();
    }

    if (changes.modulation || changes.devices)
        modulators.refresh();
}

void DevicesView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    followSelection();

    // The panels show which devices are selected, those in containers too.
    if (auto* row = getShownRow())
        row->repaint();
}

void DevicesView::timerCallback()
{
    if (! isShowing())
        return;

    if (auto* row = getShownRow())
    {
        row->updateValues();

        // A device's window opened or closed (its close button, Tracktion): its card shows it.
        if (std::any_of (row->panels.begin(), row->panels.end(), [] (const Panel* panel) { return panel->windowsChanged(); }))
        {
            row->refresh();
            layOutRow();
        }
    }

    modulators.updateValues();
}
} // namespace phazite::ui
