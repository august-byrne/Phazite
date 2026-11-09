// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContainerEditor.h"
#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/arrangement/SongSelection.h"
#include "model/commands/SongGesture.h"
#include "model/project/ProjectContext.h"
#include "ui/components/PowerSwitch.h"
#include "ui/components/TrackSwitch.h"
#include "ui/components/ValueText.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int cardWidth = 184, cardGap = Space::s, cardPadding = Space::s; // a chain's card
    constexpr int cardHeaderHeight = 24, switchSize = 20, menuButtonSize = 20; // its switch, name and menu's button
    constexpr int partHeight = 16;                                             // what part of the sound it plays
    constexpr int titleHeight = 14, chainKnobSize = 32, knobRowHeight = 36;    // its knobs, each under its title
    constexpr int rangeHeight = 22, rangeLabelWidth = 56;                      // an instrument container's chain's keys and velocities
    constexpr int separatorHeight = Space::s + 1;                              // a line between its parts and its devices
    constexpr int lineHeight = 24, lineGap = 2;                                // a device's line
    constexpr int mixSize = 20;                                                // an effect's mix, a small knob at its line's end
    constexpr int splitLabelWidth = 40, splitWidth = 180, addWidth = 96;       // the split's choice, and + Chain
    constexpr int dragThreshold = 5;     // pixels a press on a name moves before it carries the device
    constexpr float silentAlpha = 0.55f; // a chain that isn't heard, muted or soloed out

    constexpr devices::ParameterInfo keyInfo { .id = "key",
                                               .name = "Key",
                                               .minimum = 0.0f,
                                               .maximum = 127.0f,
                                               .defaultValue = 60.0f,
                                               .unit = devices::Unit::note };

    juce::String describeSwitch (const MuteSolo& state)
    {
        if (state.muted && state.soloed)
            return "Muted and soloed: silent until it is unmuted. Click to unmute it, or right-click to unsolo it";

        if (state.muted)
            return "Muted. Click to play it again, or right-click to solo it";

        if (state.soloed)
            return "Soloed: only the container's soloed chains play. Right-click to unsolo it; Ctrl+right-click another chain to solo "
                   "it too";

        if (state.silencedBySolo)
            return "Silent while other chains are soloed. Right-click to solo this one instead, Ctrl+right-click to solo it too, or "
                   "click to mute it";

        return "Playing. Click to mute this chain, or right-click to solo it (Ctrl+right-click solos it along with others)";
    }
} // namespace

//==============================================================================
ChainSwitch::ChainSwitch (ProjectContext& projectContext)
    : juce::Button ("Mute and solo"),
      context (projectContext)
{
    setMouseClickGrabsKeyboardFocus (false);
}

void ChainSwitch::setChain (DeviceID id)
{
    chain = id;
    shown.reset();
    setTitle ({});
    update();
}

void ChainSwitch::update()
{
    const auto info = getChain (context, chain);
    const auto state =
        info.has_value() ? std::optional (MuteSolo { info->muted, info->soloed, ! info->heard && ! info->muted }) : std::nullopt;

    if (state == shown && getTitle().isNotEmpty())
        return;

    shown = state;
    setEnabled (shown.has_value());

    const auto stateText = ! shown.has_value()     ? juce::String()
                           : shown->muted          ? juce::String (shown->soloed ? "Muted and soloed" : "Muted")
                           : shown->soloed         ? juce::String ("Soloed")
                           : shown->silencedBySolo ? juce::String ("Silent while other chains are soloed")
                                                   : juce::String ("Playing");
    setTitle ("Mute and solo: " + stateText); // what screen readers say
    setTooltip (shown.has_value() ? describeSwitch (*shown) : juce::String());
    repaint();
}

void ChainSwitch::clicked (const juce::ModifierKeys& mods)
{
    if (! shown.has_value())
        return;

    // A click (or Space while it has the keyboard focus) mutes; a right-click solos, alone or, with Ctrl, along with others.
    if (! mods.isPopupMenu())
        toggleChainMute (context, chain);
    else if (mods.isCommandDown())
        toggleChainSolo (context, chain);
    else
        toggleChainSoloAlone (context, chain);

    update();
}

void ChainSwitch::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    if (shown.has_value())
        paintMuteSoloLight (g, *this, *shown, isHighlighted, isDown);
}

//==============================================================================
void showChainMenu (ProjectContext& context, DeviceID chain, juce::Component& target, std::function<void()> rename,
                    std::function<void()> changed)
{
    const auto info = getChain (context, chain);

    if (! info.has_value())
        return;

    // Each a model function that is an undo step of its own; what shows the chain shows what it did at once.
    const auto act = [changed] (std::function<void()> action)
    {
        return [changed, action]
        {
            action();

            if (changed != nullptr)
                changed();
        };
    };

    auto& projectContext = context;
    const auto container = getContainerOfChain (context, chain);
    const auto chains = getChains (context, container);
    const auto index =
        (int) (std::find_if (chains.begin(), chains.end(), [chain] (const ChainInfo& each) { return each.id == chain; }) - chains.begin());

    juce::PopupMenu menu;
    menu.addSectionHeader (info->name);
    menu.addItem ("Rename", rename != nullptr, false, rename != nullptr ? rename : [] {});
    menu.addItem ("Duplicate", canAddChain (context, container), false,
                  act ([&projectContext, chain] { duplicateChain (projectContext, chain); }));

    // A container split into bands keeps its chains in their bands' order. Side by side, they move left and right.
    if (canMoveChains (context, container))
    {
        menu.addItem ("Move Left", index > 0, false, act ([&projectContext, chain] { moveChain (projectContext, chain, -1); }));
        menu.addItem ("Move Right", index < (int) chains.size() - 1, false,
                      act ([&projectContext, chain] { moveChain (projectContext, chain, 1); }));
    }

    menu.addSeparator();
    menu.addItem ("Remove", canRemoveChain (context, chain), false,
                  act ([&projectContext, chain] { removeChain (projectContext, chain); }));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target));
}

//==============================================================================
RangeBar::RangeBar (ProjectContext& projectContext, DeviceID id, Kind k)
    : ValueSlider (juce::Slider::TwoValueHorizontal, k == Kind::keys ? "Keys" : "Velocities", 0.0),
      context (projectContext),
      chain (id),
      kind (k)
{
    const auto range = kind == Kind::keys ? allKeys : allVelocities;
    setRange (range.low, range.high, 1.0);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setMinAndMaxValues (range.low, range.high, juce::dontSendNotification);
    textFromValueFunction = [k] (double value) { return describe (k, (int) std::lround (value)); };
    setTooltip (kind == Kind::keys ? "The keys this chain plays: drag either end. Notes outside them play in the other chains"
                                   : "The velocities this chain plays, from the softest notes to the hardest: drag either end");

    // A drag is one undo step; a key press, one of its own.
    onDragStart = [this]
    { gesture = std::make_unique<SongGesture> (context, kind == Kind::keys ? "Change Key Range" : "Change Velocity Range"); };

    onDragEnd = [this]
    {
        if (gesture != nullptr)
            gesture->commit();

        gesture = nullptr;
    };

    onValueChange = [this]
    {
        const NoteRange chosen { (int) std::lround (getMinValue()), (int) std::lround (getMaxValue()) };

        if (kind == Kind::keys)
            setChainKeys (context, chain, chosen);
        else
            setChainVelocities (context, chain, chosen);
    };
}

RangeBar::~RangeBar() = default;

void RangeBar::show (NoteRange range)
{
    if (gesture == nullptr
        && (! juce::exactlyEqual (getMinValue(), (double) range.low) || ! juce::exactlyEqual (getMaxValue(), (double) range.high)))
    {
        setMinAndMaxValues (range.low, range.high, juce::dontSendNotification);
        repaint();
    }
}

juce::String RangeBar::describe (Kind kind, int value)
{
    return kind == Kind::keys ? formatParameterValue (keyInfo, (float) value) : juce::String (value);
}

void RangeBar::paint (juce::Graphics& g)
{
    // Its scale as a groove, and the range a bar along it, from half a step before its low end to half a step past its high
    // end, with its ends' grips and, where it fits, what it plays.
    const auto& p = getPalette (*this);
    const auto groove = getLocalBounds().toFloat().reduced (0.0f, 3.0f);
    g.setColour (p.surface);
    g.fillRoundedRectangle (groove, cornerRadius);

    const auto positionOf = [this, &groove] (double value)
    { return juce::jlimit (groove.getX(), groove.getRight(), (float) getPositionOfValue (value)); };
    const auto left = positionOf (getMinValue() - 0.5), right = positionOf (getMaxValue() + 0.5);
    const auto bar = juce::Rectangle<float>::leftTopRightBottom (left, groove.getY(), std::max (right, left + 2.0f), groove.getBottom());
    g.setColour (p.accent.withAlpha (isEnabled() ? 0.55f : 0.25f));
    g.fillRoundedRectangle (bar, cornerRadius);
    g.setColour (p.accent);
    g.drawRoundedRectangle (bar.reduced (0.5f), cornerRadius, 1.0f);

    for (const auto x : { bar.getX() + 2.0f, bar.getRight() - 2.0f })
    {
        g.setColour (p.textBright);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, bar.getY() + 3.0f, 2.0f, bar.getHeight() - 6.0f));
    }

    // What it plays: in the bar where it fits, else beside it, where the groove has more room.
    const auto low = (int) std::lround (getMinValue()), high = (int) std::lround (getMaxValue());
    const auto text =
        low == high ? describe (kind, low) : describe (kind, low) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + describe (kind, high);
    const auto font = juce::FontOptions (TextSize::small);
    const auto textWidth = juce::GlyphArrangement::getStringWidth (font, text) + 2.0f * Space::xs;
    g.setFont (font);

    if (textWidth < bar.getWidth())
    {
        g.setColour (p.textBright);
        g.drawText (text, bar, juce::Justification::centred, false);
    }
    else
    {
        const auto before = bar.getX() - groove.getX(), after = groove.getRight() - bar.getRight();
        const auto room = before > after ? groove.withRight (bar.getX()) : groove.withLeft (bar.getRight());
        g.setColour (p.textMuted);
        g.drawText (text, room, juce::Justification::centred, true);
    }

    if (hasKeyboardFocus (false))
    {
        g.setColour (p.focus);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius, 1.0f);
    }
}

//==============================================================================
// A device's line in its chain's card: an effect's switch, its name (click to show it in the chains' place and select it,
// right-click for its menu, drag to move it) and its mix; under the pointer a wash, and while selected the accent's, as the
// column's lines. An instrument container's chain's instrument has no switch (the chain's mutes it), and stays first.
struct ContainerEditor::DeviceLine : public juce::Component
{
    DeviceLine (ContainerEditor& e, const DeviceSlot& device, bool chainInstrument)
        : editor (e),
          isChainInstrument (chainInstrument),
          shown (device)
    {
        setRepaintsOnMouseActivity (true);
        power.setTooltip ("Turns the device on or off");
        power.onClick = [this] { setDeviceOn (editor.context, shown.id, power.getToggleState()); };

        name.onClickedWith = [this] (const juce::ModifierKeys& mods)
        {
            if (std::exchange (dragged, false)) // a drag that moved it, let go over its name
                return;

            // Shown, it takes the chains' place, and this line with them: what that needs is copied first.
            auto& owner = editor;
            const auto device = shown;

            if (mods.isPopupMenu())
            {
                owner.showDeviceMenu (device, name);
            }
            else if (owner.onDeviceClicked != nullptr)
            {
                const auto clicked = owner.onDeviceClicked;
                clicked (device, mods);
            }
        };

        // An effect's switch, and a drag on its name moves it; the chain's instrument has neither.
        if (! isChainInstrument)
        {
            addAndMakeVisible (power);
            name.addMouseListener (this, false);
        }

        if (device.hasMix)
            addMixKnob (*this, mixKnob, mixAttachment, editor.context, device);

        addAndMakeVisible (name);
        show (device);
    }

    void show (const DeviceSlot& device)
    {
        shown = device;
        setTitle (device.name);
        name.setButtonText (device.name);
        name.setTitle (device.name);
        const auto moves = isChainInstrument ? juce::String() : juce::String ("; drag it to move it, into another chain too");
        name.setTooltip (device.frozen        ? "Frozen with its track: unfreeze the track (its snowflake) to change it"
                         : device.opensWindow ? "Click to show its key knobs and open its window; right-click for its menu" + moves
                                              : "Click to show it; right-click for its menu" + moves);
        power.setToggleState (device.isOn, juce::dontSendNotification);
        power.setEnabled (! device.frozen);

        for (auto* knob : mixKnob)
            knob->setEnabled (! device.frozen);

        repaint();
    }

    void updateValues()
    {
        if (mixAttachment != nullptr)
            mixAttachment->update();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto line = getLocalBounds().toFloat();

        if (isMouseOver (true))
        {
            g.setColour (p.hover);
            g.fillRoundedRectangle (line, cornerRadius);
        }

        if (editor.context.getSongSelection().isSelected (shown.id))
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
            g.fillAll (getPalette (*this).surfaceRaised.withAlpha (1.0f - FrozenTint::frozenDevice));
    }

    void resized() override
    {
        auto line = getLocalBounds().reduced (Space::xs, 2);

        if (power.isVisible())
            power.setBounds (line.removeFromLeft (line.getHeight()));

        if (! mixKnob.isEmpty())
        {
            mixKnob.getFirst()->setBounds (line.removeFromRight (mixSize).withSizeKeepingCentre (mixSize, mixSize));
            line.removeFromRight (Space::xs);
        }

        name.setBounds (line);
    }

    // A drag on its name, past a few pixels, carries it to another place in its chain or another (ChainCard::itemDropped()).
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != &name || dragged || ! e.mods.isLeftButtonDown() || shown.frozen
            || e.getDistanceFromDragStart() < dragThreshold)
            return;

        if (auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            dragged = true;
            dragContainer->startDragging (describeEffectDrag (shown.id), &name);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // Let go away from its name, the button hears no click to forget.
        if (e.eventComponent == &name && ! name.getLocalBounds().contains (e.getPosition()))
            dragged = false;
    }

    ContainerEditor& editor;
    const bool isChainInstrument;
    DeviceSlot shown;
    bool dragged = false; // its name dragged since the mouse went down: the click that ends it isn't one
    PowerSwitch power;
    FlatName name;
    juce::OwnedArray<Knob> mixKnob;
    std::unique_ptr<MixAttachment> mixAttachment;
};

//==============================================================================
// A chain's card: its switch, its name and its menu's button; what part of the sound it plays; its knobs (a band's crossover,
// its volume and its pan), each under its title; an instrument container's chain's keys and velocities; then its devices
// (an instrument container's chain's instrument first, or a button that chooses one) and a button that adds an effect. An
// effect dragged onto it goes where the line shows meanwhile. Faint while it isn't heard.
struct ContainerEditor::ChainCard : public juce::Component,
                                    public juce::DragAndDropTarget
{
    ChainCard (ContainerEditor& e, const ChainInfo& info, bool showsParts, bool crossoverColumn)
        : editor (e),
          context (e.context),
          chain (info.id),
          power (e.context),
          showsPart (showsParts),
          showsCrossoverColumn (crossoverColumn)
    {
        power.setChain (chain);
        addAndMakeVisible (power);

        name.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
        name.setMinimumHorizontalScale (1.0f);
        name.setEditable (false, true, false);
        name.setTitle ("Name");
        name.setTooltip ("The chain's name: double-click to rename it, right-click for its menu");
        name.onTextChange = [this] { renameChain (context, chain, name.getText()); };
        name.addMouseListener (this, false);
        addAndMakeVisible (name);

        menuButton.setTooltip ("The chain's menu: rename it, duplicate it, move it or remove it");
        menuButton.onClick = [this] { showMenu (menuButton); };
        addAndMakeVisible (menuButton);

        part.setFont (juce::FontOptions (TextSize::small));
        part.setMinimumHorizontalScale (1.0f);
        part.setInterceptsMouseClicks (false, false);
        addChildComponent (part);

        // A band's crossover (but the first's), its volume and its pan, which macros and automation move as a device's knobs.
        crossover = addParameterKnob (*this, knobs, attachments, getChainCrossover (context, chain));
        volume = addParameterKnob (*this, knobs, attachments, getChainVolume (context, chain));
        pan = addParameterKnob (*this, knobs, attachments, getChainPan (context, chain));

        if (crossover != nullptr)
            crossover->setTooltip ("Where this chain's band starts: below it, the sound goes to the chains before it");

        if (editor.instrument)
        {
            keys = std::make_unique<RangeBar> (context, chain, RangeBar::Kind::keys);
            velocities = std::make_unique<RangeBar> (context, chain, RangeBar::Kind::velocities);
            addAndMakeVisible (*keys);
            addAndMakeVisible (*velocities);

            for (auto* label : { &keysLabel, &velocitiesLabel })
            {
                label->setFont (juce::FontOptions (TextSize::small));
                label->setInterceptsMouseClicks (false, false);
                label->setAccessible (false); // the bars beside them have the names
                addAndMakeVisible (label);
            }

            chooseButton = std::make_unique<juce::TextButton> ("+ Instrument");
            chooseButton->setTitle ("Add instrument");
            chooseButton->setTooltip ("Chooses the instrument this chain plays its notes on: a synth or a sampler, built in or a plug-in");
            chooseButton->onClick = [this] { showChainInstrumentMenu (context, chain, *chooseButton); };
            addChildComponent (*chooseButton);
        }

        addEffectButton.setTitle ("Add effect");
        addEffectButton.setTooltip ("Adds an effect to this chain, after its devices");
        addEffectButton.onClick = [this] { showChainEffectMenu (context, chain, addEffectButton); };
        addAndMakeVisible (addEffectButton);

        for (auto* label : { &part, &keysLabel, &velocitiesLabel })
            setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);

        show (info);
    }

    // Shows it as it is now: its devices' lines made again if they changed, else shown again.
    void show (const ChainInfo& info)
    {
        setTitle (info.name);

        if (! name.isBeingEdited())
            name.setText (info.name, juce::dontSendNotification);

        part.setText (info.part, juce::dontSendNotification);
        part.setVisible (showsPart);
        power.update();

        if (keys != nullptr)
            keys->show (info.keys);

        if (velocities != nullptr)
            velocities->show (info.velocities);

        hasInstrument = info.hasInstrument;

        if (chooseButton != nullptr)
            chooseButton->setVisible (! hasInstrument);

        if (getIDs (info.devices) != order)
        {
            lines.clear();
            order = getIDs (info.devices);

            for (size_t i = 0; i < info.devices.size(); ++i)
                addAndMakeVisible (lines.add (new DeviceLine (editor, info.devices[i], hasInstrument && i == 0)));
        }
        else
        {
            for (size_t i = 0; i < info.devices.size(); ++i)
                lines[(int) i]->show (info.devices[i]);
        }

        if (heard != info.heard)
        {
            heard = info.heard;
            setAlpha (heard ? 1.0f : silentAlpha);
        }

        resized();
    }

    void updateValues()
    {
        for (auto* attachment : attachments)
            attachment->update();

        for (auto* line : lines)
            line->updateValues();

        power.update();
    }

    // As tall as its parts, its devices and its buttons.
    int getPartsHeight() const
    {
        return cardPadding + cardHeaderHeight + (showsPart ? partHeight : 0) + Space::xs + titleHeight + knobRowHeight
               + (keys != nullptr ? Space::xs + 2 * rangeHeight : 0) + separatorHeight;
    }

    int getPreferredHeight() const
    {
        return getPartsHeight() + (chooseButton != nullptr && ! hasInstrument ? controlHeight + lineGap : 0)
               + lines.size() * (lineHeight + lineGap) + controlHeight + cardPadding;
    }

    void showMenu (juce::Component& target)
    {
        const juce::Component::SafePointer<ChainCard> safeThis (this);
        const juce::Component::SafePointer<ContainerEditor> safeEditor (&editor);
        showChainMenu (
            context, chain, target,
            [safeThis]
            {
                if (safeThis != nullptr)
                    safeThis->name.showEditor();
            },
            [safeEditor]
            {
                if (safeEditor != nullptr)
                    safeEditor->changed();
            });
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &name && e.mods.isPopupMenu())
            showMenu (name);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (p.surfaceRaised);
        g.fillRoundedRectangle (bounds, cardCornerRadius);
        g.setColour (p.border);
        g.drawRoundedRectangle (bounds.reduced (0.5f), cardCornerRadius, 1.0f);

        // Over each knob, its title.
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (p.textMuted);

        for (const auto& [knob, text] :
             std::initializer_list<std::pair<Knob*, const char*>> { { crossover, "Crossover" }, { volume, "Volume" }, { pan, "Pan" } })
            if (knob != nullptr)
                g.drawText (text, knob->getBounds().withSizeKeepingCentre (cellWidth, titleHeight).withY (titlesTop),
                            juce::Justification::centred, false);

        // The line between its parts and its devices.
        g.setColour (p.border);
        g.fillRect (cardPadding, devicesTop - separatorHeight / 2 - 1, getWidth() - 2 * cardPadding, 1);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // Where a dragged effect would go: a line between the devices.
        if (dropIndex < 0)
            return;

        const auto y = dropIndex < lines.size() ? lines[dropIndex]->getY() - lineGap / 2
                       : lines.isEmpty()        ? devicesTop
                                                : lines.getLast()->getBottom() + lineGap / 2;
        g.setColour (getPalette (*this).accent);
        g.fillRect (cardPadding, y - 1, getWidth() - 2 * cardPadding, 2);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (cardPadding);
        auto header = area.removeFromTop (cardHeaderHeight);
        power.setBounds (header.removeFromLeft (switchSize).withSizeKeepingCentre (switchSize, switchSize));
        header.removeFromLeft (Space::xs);
        menuButton.setBounds (header.removeFromRight (menuButtonSize).withSizeKeepingCentre (menuButtonSize, menuButtonSize));
        header.removeFromRight (Space::xs);
        name.setBounds (header);

        if (showsPart)
            part.setBounds (area.removeFromTop (partHeight).withTrimmedLeft (switchSize + Space::xs));

        // The knobs in their cells, a crossover's left empty for the first band, so that the cards' knobs line up.
        area.removeFromTop (Space::xs);
        titlesTop = area.getY();
        area.removeFromTop (titleHeight);
        auto knobRow = area.removeFromTop (knobRowHeight);
        const auto cells = crossover != nullptr || showsCrossoverColumn ? 3 : 2;
        cellWidth = knobRow.getWidth() / cells;

        const auto placeKnob = [&knobRow, this] (Knob* knob)
        {
            const auto cell = knobRow.removeFromLeft (cellWidth);

            if (knob != nullptr)
                knob->setBounds (cell.withSizeKeepingCentre (chainKnobSize, chainKnobSize));
        };

        if (cells == 3)
            placeKnob (crossover);

        placeKnob (volume);
        placeKnob (pan);

        // An instrument container's chain's keys and velocities, each beside its name.
        if (keys != nullptr && velocities != nullptr)
        {
            area.removeFromTop (Space::xs);

            for (const auto& [label, bar] :
                 std::initializer_list<std::pair<juce::Label*, RangeBar*>> { { &keysLabel, keys.get() },
                                                                             { &velocitiesLabel, velocities.get() } })
            {
                auto row = area.removeFromTop (rangeHeight);
                label->setBounds (row.removeFromLeft (rangeLabelWidth));
                bar->setBounds (row);
            }
        }

        area.removeFromTop (separatorHeight);
        devicesTop = area.getY();

        if (chooseButton != nullptr && chooseButton->isVisible())
        {
            chooseButton->setBounds (area.removeFromTop (controlHeight).reduced (0, 2));
            area.removeFromTop (lineGap);
        }

        for (auto* line : lines)
        {
            line->setBounds (area.removeFromTop (lineHeight));
            area.removeFromTop (lineGap);
        }

        addEffectButton.setBounds (area.removeFromTop (controlHeight).reduced (0, 2));
    }

    // An effect of its container, dragged by its name, goes before the device at the line under the pointer (after its
    // instrument, in an instrument container's chain).
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        const auto effect = getDraggedEffect (details.description);
        return effect.has_value() && getContainerOf (context, *effect) == editor.container;
    }

    int getDropIndex (juce::Point<int> position) const
    {
        const auto first = hasInstrument ? 1 : 0;

        for (int i = 0; i < lines.size(); ++i)
            if (position.y < lines[i]->getY() + lineHeight / 2)
                return std::max (i, first);

        return lines.size();
    }

    void itemDragMove (const SourceDetails& details) override
    {
        if (const auto index = getDropIndex (details.localPosition); index != dropIndex)
        {
            dropIndex = index;
            repaint();
        }
    }

    void itemDragExit (const SourceDetails&) override
    {
        dropIndex = -1;
        repaint();
    }

    void itemDropped (const SourceDetails& details) override
    {
        const auto index = getDropIndex (details.localPosition);
        dropIndex = -1;
        repaint();

        if (const auto effect = getDraggedEffect (details.description))
        {
            moveEffectToChain (context, *effect, chain, index < lines.size() ? index : -1);
            editor.changed(); // at once, before the song's change comes
        }
    }

    ContainerEditor& editor;
    ProjectContext& context;
    const DeviceID chain;
    ChainSwitch power;
    juce::Label name, part;
    IconButton menuButton { "Chain's menu", Icon::moreActions };
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    Knob* crossover = nullptr;
    Knob* volume = nullptr;
    Knob* pan = nullptr;
    std::unique_ptr<RangeBar> keys, velocities; // an instrument container's chain's
    juce::Label keysLabel { {}, "Keys" }, velocitiesLabel { {}, "Velocity" };
    std::unique_ptr<juce::TextButton> chooseButton; // an instrument container's chain's, while it has no instrument
    juce::OwnedArray<DeviceLine> lines;
    std::vector<DeviceID> order;
    juce::TextButton addEffectButton { "+ Effect" };
    const bool showsPart;            // its container splits the sound into parts, which its chains play
    const bool showsCrossoverColumn; // the first band's, in a container split into bands
    bool hasInstrument = false;
    bool heard = true;
    int titlesTop = 0, cellWidth = 0, devicesTop = 0;
    int dropIndex = -1;
};

//==============================================================================
ContainerEditor::ContainerEditor (ProjectContext& projectContext, CommandBridge& bridge, DeviceID id)
    : context (projectContext),
      commands (bridge),
      container (id),
      instrument (isInstrumentContainer (projectContext, id))
{
    setTitle ("Chains");

    // How the chains share what comes in: a container of effects'; an instrument container's play notes.
    if (! instrument)
    {
        for (const auto split : { ContainerSplit::layers, ContainerSplit::bands, ContainerSplit::midSide, ContainerSplit::leftRight })
            splitChoice.addItem (getSplitName (split), (int) split + 1);

        splitChoice.setTitle ("Split");
        splitChoice.setTooltip ("What part of the sound coming in each chain plays: all of it (layers, which add up), a band of it, "
                                "low to high, its middle and its side, or its left and its right. Mid and Side, and Left and Right, "
                                "are for two chains");
        splitChoice.onChange = [this]
        {
            if (const auto chosen = splitChoice.getSelectedId(); chosen > 0)
                setContainerSplit (context, container, (ContainerSplit) (chosen - 1));
        };
        splitLabel.setFont (juce::FontOptions (TextSize::small));
        splitLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (splitLabel);
        addAndMakeVisible (splitChoice);
    }

    hint.setFont (juce::FontOptions (TextSize::small));
    hint.setJustificationType (juce::Justification::centredLeft);
    hint.setMinimumHorizontalScale (1.0f);
    hint.setText (instrument ? "Each chain plays its instrument on the notes in its keys and velocities: + Instrument, in a chain, "
                               "chooses one."
                             : "Its chains play side by side and add up: + Effect, in a chain, adds an effect to it.",
                  juce::dontSendNotification);
    addChildComponent (hint);

    addButton.setTitle ("Add chain");
    addButton.setTooltip (instrument ? "Adds a chain, for another instrument layered with the others, or played on other keys"
                                     : "Adds a chain, which plays beside the others, with its own devices, volume, pan, mute and solo");
    addButton.onClick = [this] { addChain (context, container); };
    addAndMakeVisible (addButton);

    chainsRow.setTitle ("Chains side by side");
    chainsView.setTitle ("Chains"); // what screen readers say as the keys go to them, to scroll them
    chainsView.setViewedComponent (&chainsRow, false);
    chainsView.setScrollBarsShown (true, true);
    addAndMakeVisible (chainsView);

    for (auto* label : { &splitLabel, &hint })
        setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);

    refresh();
}

ContainerEditor::~ContainerEditor()
{
    chainsView.setViewedComponent (nullptr, false);
}

void ContainerEditor::refresh()
{
    const auto chains = getChains (context, container);
    const auto split = getContainerSplit (context, container);

    if (getIDs (chains) != shownChains || split != shownSplit)
    {
        // Every card keeps a line for its part where the sound is split into parts, and a cell for the bands' crossovers.
        cards.clear();
        const auto showsParts = std::any_of (chains.begin(), chains.end(), [] (const ChainInfo& chain) { return chain.part.isNotEmpty(); });
        const auto bands = split == ContainerSplit::bands && chains.size() > 1;

        for (const auto& chain : chains)
            chainsRow.addAndMakeVisible (cards.add (new ChainCard (*this, chain, showsParts, bands)));

        shownChains = getIDs (chains);
        shownSplit = split;
    }
    else
    {
        for (size_t i = 0; i < chains.size(); ++i)
            cards[(int) i]->show (chains[i]);
    }

    showSplit();
    addButton.setEnabled (canAddChain (context, container));
    hint.setVisible (std::all_of (chains.begin(), chains.end(), [] (const ChainInfo& chain) { return chain.devices.empty(); }));
    resized();
    repaint();
}

void ContainerEditor::showSplit()
{
    if (instrument)
        return;

    for (const auto split : { ContainerSplit::layers, ContainerSplit::bands, ContainerSplit::midSide, ContainerSplit::leftRight })
        splitChoice.setItemEnabled ((int) split + 1, canSplit (context, container, split));

    splitChoice.setSelectedId ((int) getContainerSplit (context, container) + 1, juce::dontSendNotification);
}

void ContainerEditor::updateValues()
{
    for (auto* card : cards)
        card->updateValues();
}

void ContainerEditor::showChain (DeviceID chain)
{
    if (auto* card = dynamic_cast<ChainCard*> (findChain (chain)))
    {
        auto x = chainsView.getViewPositionX();

        if (card->getRight() > x + chainsView.getViewWidth())
            x = card->getRight() - chainsView.getViewWidth();

        if (card->getX() < x)
            x = card->getX();

        chainsView.setViewPosition (std::max (0, x), 0);
    }
}

juce::Component* ContainerEditor::findChain (DeviceID chain) const
{
    for (auto* card : cards)
        if (card->chain == chain)
            return card;

    return nullptr;
}

juce::Component* ContainerEditor::findDevice (DeviceID device) const
{
    for (auto* card : cards)
        for (auto* line : card->lines)
            if (line->shown.id == device)
                return line;

    return nullptr;
}

void ContainerEditor::showDeviceMenu (const DeviceSlot& device, juce::Component& target)
{
    // Its place among its chain's effects, which follow its instrument in an instrument container's, for the menu's moves.
    auto index = 0, count = 1;

    if (const auto chain = getChain (context, getChainOf (context, device.id)))
    {
        std::vector<DeviceID> effects;

        for (size_t i = chain->hasInstrument ? 1 : 0; i < chain->devices.size(); ++i)
            effects.push_back (chain->devices[i].id);

        index = (int) (std::find (effects.begin(), effects.end(), device.id) - effects.begin());
        count = (int) effects.size();
    }

    const juce::Component::SafePointer<ContainerEditor> safeThis (this);
    ui::showDeviceMenu (context, commands, device, DevicePlace::inContainer, getDeviceChannel (context, container).value_or (TrackID()),
                        index, count, false, target,
                        [safeThis]
                        {
                            if (safeThis != nullptr)
                                safeThis->changed();
                        });
}

void ContainerEditor::changed()
{
    if (onChanged != nullptr)
    {
        const auto callback = onChanged; // what it shows may take this editor away
        callback();
    }
}

int ContainerEditor::getTopHeight() const
{
    return controlHeight + Space::s;
}

int ContainerEditor::getCardsHeight() const
{
    auto height = controlHeight;

    for (auto* card : cards)
        height = std::max (height, card->getPreferredHeight());

    return height;
}

int ContainerEditor::getStripWidth() const
{
    return std::max (0, cards.size() * (cardWidth + cardGap) - cardGap);
}

int ContainerEditor::getMinimumHeight() const
{
    // Its top line, and room for its chains' cards to show their parts at least: their devices scroll up and down, within it,
    // where the dock is shorter than they are, as they scroll sideways where it is narrower.
    auto parts = controlHeight;

    for (auto* card : cards)
        parts = std::max (parts, card->getPartsHeight() + lineHeight + cardPadding);

    return getTopHeight() + std::min (parts, getCardsHeight());
}

int ContainerEditor::getWidthForHeight (int) const
{
    return getStripWidth();
}

int ContainerEditor::getMinimumHeightAtWidth (int width) const
{
    // Where the chains are wider than it, their scroll bar under them.
    return getMinimumHeight() + (getStripWidth() > width ? chainsView.getScrollBarThickness() : 0);
}

void ContainerEditor::resized()
{
    // Along its top, beside the container's name: a container of effects' split, + Chain, and while its chains are empty, what
    // to do with them.
    auto area = getLocalBounds();
    auto top = area.removeFromTop (controlHeight);
    area.removeFromTop (Space::s);

    if (! instrument)
    {
        splitLabel.setBounds (top.removeFromLeft (splitLabelWidth));
        top.removeFromLeft (Space::xs);
        splitChoice.setBounds (top.removeFromLeft (splitWidth).reduced (0, 2));
        top.removeFromLeft (Space::m);
    }

    addButton.setBounds (top.removeFromLeft (addWidth).reduced (0, 2));
    top.removeFromLeft (Space::m);
    hint.setBounds (top);

    chainsView.setBounds (area);
    layOutCards();
}

void ContainerEditor::layOutCards()
{
    // The cards side by side, each as tall as the tallest, or as the room there; each scroll bar takes room from the other way.
    const auto width = getStripWidth(), tallest = getCardsHeight();
    const auto thickness = chainsView.getScrollBarThickness();
    const auto roomWidth = chainsView.getWidth(), roomHeight = chainsView.getHeight();
    const auto scrollsDown = tallest > roomHeight || (width > roomWidth && tallest > roomHeight - thickness);
    const auto scrollsAcross = width > roomWidth - (scrollsDown ? thickness : 0);
    const auto height = std::max (tallest, roomHeight - (scrollsAcross ? thickness : 0));
    auto x = 0;

    for (auto* card : cards)
    {
        card->setBounds (x, 0, cardWidth, height);
        x += cardWidth + cardGap;
    }

    chainsRow.setSize (width, height);
}
} // namespace phazite::ui
