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
#include "ui/input/CommandBridge.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int columnWidth = 200; // the channel's name and its modulators
    constexpr int powerWidth = 36, windowButtonWidth = 28;
    constexpr int keyKnobColumns = 4, maxKeyKnobs = 8; // a plug-in's key knobs
    constexpr int labelHeight = 18;                    // a knob's name under it
    constexpr int addButtonWidth = 110;                // "+ Effect", "Choose instrument"
    constexpr int macroColumns = 4, macroWidth = 72, macroKnobSize = 44;
    constexpr int macroHeight = macroKnobSize + labelHeight;

    //==============================================================================
    // A plug-in's, or one of Tracktion's built-in devices', key knobs: its mix first, as a strip's slot has it, then the
    // knobs the model offers, each named; its own controls are in its window.
    struct KeyKnobs : public juce::Component
    {
        KeyKnobs (ProjectContext& context, const DeviceSlot& device)
        {
            if (device.hasMix)
                addMixKnob (*this, knobs, mixAttachment, context, device);

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

            if (mixAttachment != nullptr)
                mixAttachment->update();
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
        std::unique_ptr<MixAttachment> mixAttachment;
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
// Devices in a row, left to right as the sound goes through them: a channel's (its instrument first, or a button that
// adds one, then its effects and a button that adds one), or a container's.
struct DevicesView::Row : public juce::Component
{
    Row (DevicesView&, TrackID channelTrack, DeviceID container); // the master's with an empty track and container
    ~Row() override;

    void refresh(); // new panels if the devices changed, else their names and switches
    void updateValues();
    int getPreferredWidth() const;
    void resized() override;

    std::vector<DeviceSlot> getDevices() const;
    void showAddMenu();

    DevicesView& view;
    const TrackID track;
    const DeviceID container;
    juce::OwnedArray<Panel> panels;
    std::vector<DeviceID> shown;
    juce::TextButton instrumentButton, addButton { "+ Effect" };
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
// A device: its header (an on/off switch, its name, and a plug-in's window button) over what shows it, scrolled up and
// down when the view is shorter than it.
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

        if (device.opensWindow)
        {
            windowButton = std::make_unique<IconButton> ("Open its window", Icon::detach);
            windowButton->setTooltip ("Opens the plug-in's own window, with all its controls");
            windowButton->onClick = [this] { showDeviceWindow (view.context, shown.id); };
            addAndMakeVisible (*windowButton);
        }

        // What shows it: one of Phazite's own devices' editor, a container's macros and devices, or a plug-in's key knobs.
        if (device.isContainer)
            body = std::make_unique<ContainerBody> (view, view.context, device);
        else if (! device.opensWindow)
            body = createDeviceEditor (getDeviceHandle (view.context, device.id));

        if (body == nullptr)
            body = std::make_unique<KeyKnobs> (view.context, device);

        bodyView.setViewedComponent (body.get(), false);
        bodyView.setScrollBarsShown (true, false);
        addAndMakeVisible (bodyView);
        addAndMakeVisible (power);
        addAndMakeVisible (nameButton);
        show (device);
    }

    void show (const DeviceSlot& device)
    {
        shown = device;
        setTitle (device.name);
        nameButton.setButtonText (device.name);
        nameButton.setTitle (device.name);
        nameButton.setTooltip (device.opensWindow ? "Click to select it, and open its window; right-click for its menu"
                                                  : "Click to select it; right-click for its menu");
        power.setToggleState (device.isOn, juce::dontSendNotification);

        if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            containerBody->refresh();

        repaint();
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

        if (shown.opensWindow)
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
        if (auto* knobs = dynamic_cast<KeyKnobs*> (body.get()))
            knobs->updateValues();
        else if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            containerBody->updateValues();
    }

    // As wide as what shows it, and a scroll bar if it is taller than the view.
    int getBodyWidth() const
    {
        if (auto* containerBody = dynamic_cast<ContainerBody*> (body.get()))
            return containerBody->getPreferredWidth();

        return body->getWidth();
    }

    int getPreferredWidth (int height) const
    {
        const auto bodyHeight = height - controlHeight;
        const auto scrolls = dynamic_cast<ContainerBody*> (body.get()) == nullptr && getBodyHeight (bodyHeight) > bodyHeight;
        return getBodyWidth() + (scrolls ? bodyView.getScrollBarThickness() : 0);
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

        if (windowButton != nullptr)
            windowButton->setBounds (header.removeFromRight (windowButtonWidth));

        nameButton.setBounds (header);
        bodyView.setBounds (area);

        const auto bodyHeight = getBodyHeight (area.getHeight());
        body->setSize (getBodyWidth(), bodyHeight);
    }

    DevicesView& view;
    const DevicePlace place;
    const TrackID track;
    DeviceSlot shown;
    juce::TextButton power;
    MenuButton nameButton;
    std::unique_ptr<IconButton> windowButton; // a plug-in's
    std::unique_ptr<juce::Component> body;
    juce::Viewport bodyView;
};

//==============================================================================
DevicesView::Row::Row (DevicesView& v, TrackID channelTrack, DeviceID containerID)
    : view (v),
      track (channelTrack),
      container (containerID)
{
    setTitle (container.isValid() ? "Container's devices" : "Devices");
    instrumentButton.onClick = [this] { showInstrumentMenu (view.context, track, instrumentButton); };
    addButton.onClick = [this] { showAddMenu(); };
    addButton.setTooltip (container.isValid() ? "Adds an effect after the container's devices: built in or a plug-in"
                                              : "Adds an effect, which changes the sound, such as a reverb: built in or a plug-in");
    addAndMakeVisible (instrumentButton);
    addAndMakeVisible (addButton);
    refresh();
}

DevicesView::Row::~Row() = default;

std::vector<DeviceSlot> DevicesView::Row::getDevices() const
{
    if (container.isValid())
        return getContainerDevices (view.context, container);

    auto devices = getEffects (view.context, track);

    if (const auto instrument = track.isValid() ? getInstrument (view.context, track) : std::nullopt)
        devices.insert (devices.begin(), *instrument);

    return devices;
}

void DevicesView::Row::refresh()
{
    const auto devices = getDevices();

    if (getIDs (devices) != shown)
    {
        panels.clear();
        shown = getIDs (devices);

        for (const auto& device : devices)
        {
            const auto place = container.isValid() ? DevicePlace::inContainer
                               : track.isValid() && getInstrument (view.context, track).has_value() && device.id == devices.front().id
                                   ? DevicePlace::instrument
                                   : DevicePlace::effect;
            addAndMakeVisible (panels.add (new Panel (view, device, place, track)));
        }
    }
    else
    {
        for (size_t i = 0; i < devices.size(); ++i)
            panels[(int) i]->show (devices[i]);
    }

    // A track without an instrument has a button for one first, which asks for one while its notes make no sound.
    const auto trackChannel = track.isValid() ? phazite::getChannel (view.context, track) : std::nullopt;
    const auto needsButton = ! container.isValid() && track.isValid() && canHoldInstrument (view.context, track)
                             && ! getInstrument (view.context, track).has_value();
    const auto needs = trackChannel.has_value() && trackChannel->needsInstrument;
    const auto& p = getPalette (view);
    instrumentButton.setVisible (needsButton);
    instrumentButton.setButtonText (needs ? "Choose instrument" : "+ Instrument");
    instrumentButton.setTooltip (needs ? "This track has notes, but no instrument to play them, so they make no sound. Choose one: a "
                                         "synth or a sampler"
                                       : "Adds an instrument, which plays the track's notes: a synth or a sampler, built in or a plug-in");
    instrumentButton.setColour (juce::TextButton::buttonColourId,
                                needs ? p.surfaceRaised.interpolatedWith (p.warning, attentionTint) : p.surfaceRaised);
    addButton.setEnabled (view.context.hasProject());
    resized();
}

void DevicesView::Row::updateValues()
{
    for (auto* panel : panels)
        panel->updateValues();
}

int DevicesView::Row::getPreferredWidth() const
{
    auto width = Space::s;

    if (instrumentButton.isVisible())
        width += addButtonWidth + Space::s;

    for (auto* panel : panels)
        width += panel->getPreferredWidth (getHeight()) + Space::s;

    return width + addButtonWidth + Space::s;
}

void DevicesView::Row::resized()
{
    auto area = getLocalBounds().reduced (0, Space::s).withTrimmedLeft (Space::s);

    if (instrumentButton.isVisible())
    {
        instrumentButton.setBounds (area.removeFromLeft (addButtonWidth).removeFromTop (controlHeight));
        area.removeFromLeft (Space::s);
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

    return row != nullptr ? search (*row) : nullptr;
}

void DevicesView::showDevice (DeviceID id)
{
    if (const auto deviceChannel = getDeviceChannel (context, id))
        setChannel (deviceChannel);

    if (auto* panel = findPanel (id))
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
    if (newChannel == channel && (row != nullptr) == newChannel.has_value())
        return;

    channel = newChannel;
    modulatorEditor = nullptr;
    rowView.setViewedComponent (nullptr, false);
    row = nullptr;

    if (channel.has_value())
    {
        row = std::make_unique<Row> (*this, *channel, DeviceID());
        rowView.setViewedComponent (row.get(), false);
    }

    // The master's modulators are its track's.
    modulators.setTrack (! channel.has_value() ? TrackID() : channel->isValid() ? *channel : getMasterTrack (context));
    showChannelName();
    resized();
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
    layOutRow();
}

void DevicesView::layOutRow()
{
    if (row == nullptr)
        return;

    const auto height = rowView.getHeight() - rowView.getScrollBarThickness();
    row->setSize (row->getWidth(), height); // the panels' widths depend on the height
    row->setSize (std::max (row->getPreferredWidth(), rowView.getWidth()), height);
}

void DevicesView::projectChanged()
{
    channel.reset();
    row = nullptr;
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
        if (row != nullptr)
            row->refresh();

        layOutRow();
    }

    if (changes.modulation || changes.devices)
        modulators.refresh();
}

void DevicesView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    followSelection();

    // The panels show which devices are selected, those in containers too.
    if (row != nullptr)
        row->repaint();
}

void DevicesView::timerCallback()
{
    if (! isShowing())
        return;

    if (row != nullptr)
        row->updateValues();

    modulators.updateValues();
}
} // namespace phazite::ui
