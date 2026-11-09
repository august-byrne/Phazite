// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ContainerEditor.h"
#include "StripParts.h"
#include "model/commands/SongGesture.h"
#include "ui/components/TrackSwitch.h"
#include "ui/components/ValueText.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
using namespace strip;

namespace
{
    constexpr int rowHeight = 40, rowGap = Space::xs;       // a chain's row
    constexpr int switchWidth = 20, nameWidth = 132;        // its switch and its name, with its part under it
    constexpr int chainKnobSize = 32, knobColumnWidth = 60; // its crossover, volume and pan, each under its column's title
    constexpr int keysMinimum = 220, velocitiesMinimum = 120, rulerHeight = 14;
    constexpr int splitLabelWidth = 40, splitWidth = 180, addWidth = 96; // the split's choice, and + Chain
    constexpr int effectsWidth = switchWidth + nameWidth + 3 * knobColumnWidth + 2 * Space::s;
    constexpr int instrumentsWidth = switchWidth + nameWidth + 2 * knobColumnWidth + keysMinimum + velocitiesMinimum + 4 * Space::s;
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

    // A container split into bands keeps its chains in their bands' order.
    if (canMoveChains (context, container))
    {
        menu.addItem ("Move Up", index > 0, false, act ([&projectContext, chain] { moveChain (projectContext, chain, -1); }));
        menu.addItem ("Move Down", index < (int) chains.size() - 1, false,
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
    const auto groove = getLocalBounds().toFloat().reduced (0.0f, 6.0f);
    g.setColour (p.surfaceRaised);
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

    const auto low = (int) std::lround (getMinValue()), high = (int) std::lround (getMaxValue());
    const auto text =
        low == high ? describe (kind, low) : describe (kind, low) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + describe (kind, high);
    const auto font = juce::FontOptions (TextSize::small);

    if (juce::GlyphArrangement::getStringWidth (font, text) + 2.0f * Space::xs < bar.getWidth())
    {
        g.setColour (p.textBright);
        g.setFont (font);
        g.drawText (text, bar, juce::Justification::centred, false);
    }

    if (hasKeyboardFocus (false))
    {
        g.setColour (p.focus);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), cornerRadius, 1.0f);
    }
}

//==============================================================================
// A chain's row: its switch, its name over what part of the sound it plays, its knobs (a band's crossover, its volume and its
// pan) and an instrument container's chain's keys and velocities. Faint while it isn't heard.
struct ContainerEditor::Row : public juce::Component
{
    Row (ProjectContext& projectContext, const ChainInfo& info, bool instrumentContainer)
        : context (projectContext),
          chain (info.id),
          power (projectContext)
    {
        setTitle (info.name);
        power.setChain (chain);
        addAndMakeVisible (power);

        name.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
        name.setMinimumHorizontalScale (1.0f);
        name.setEditable (false, true, false);
        name.setTooltip ("The chain's name: double-click to rename it, right-click for its menu");
        name.onTextChange = [this] { renameChain (context, chain, name.getText()); };
        name.addMouseListener (this, false);
        part.setFont (juce::FontOptions (TextSize::small));
        part.setMinimumHorizontalScale (1.0f);
        part.setInterceptsMouseClicks (false, false);

        for (auto* label : { &name, &part })
            addAndMakeVisible (label);

        // A band's crossover (but the first's), its volume and its pan, which macros and automation move as a device's knobs.
        crossover = addParameterKnob (*this, knobs, attachments, getChainCrossover (context, chain));
        volume = addParameterKnob (*this, knobs, attachments, getChainVolume (context, chain));
        pan = addParameterKnob (*this, knobs, attachments, getChainPan (context, chain));

        if (crossover != nullptr)
            crossover->setTooltip ("Where this chain's band starts: below it, the sound goes to the chains before it");

        if (instrumentContainer)
        {
            keys = std::make_unique<RangeBar> (context, chain, RangeBar::Kind::keys);
            velocities = std::make_unique<RangeBar> (context, chain, RangeBar::Kind::velocities);
            addAndMakeVisible (*keys);
            addAndMakeVisible (*velocities);
        }

        setThemeColour (part, juce::Label::textColourId, &Palette::textMuted);
        show (info);
    }

    void show (const ChainInfo& info)
    {
        setTitle (info.name);

        if (! name.isBeingEdited())
            name.setText (info.name, juce::dontSendNotification);

        name.setTitle (info.name);
        part.setText (info.part, juce::dontSendNotification);
        part.setVisible (info.part.isNotEmpty());
        power.update();

        if (keys != nullptr)
            keys->show (info.keys);

        if (velocities != nullptr)
            velocities->show (info.velocities);

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

        power.update();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent != &name || ! e.mods.isPopupMenu())
            return;

        const juce::Component::SafePointer<Row> safeThis (this);
        showChainMenu (context, chain, name,
                       [safeThis]
                       {
                           if (safeThis != nullptr)
                               safeThis->name.showEditor();
                       },
                       {});
    }

    void resized() override
    {
        auto area = getLocalBounds();
        power.setBounds (area.removeFromLeft (switchWidth).withSizeKeepingCentre (switchWidth, switchWidth));
        area.removeFromLeft (Space::xs);
        auto names = area.removeFromLeft (nameWidth);

        if (part.isVisible())
        {
            name.setBounds (names.removeFromTop (names.getHeight() / 2 + 2));
            part.setBounds (names);
        }
        else
        {
            name.setBounds (names);
        }

        // The knobs in their columns, a crossover's left empty for the first band, so that each column lines up.
        const auto placeKnob = [&area] (Knob* knob)
        {
            const auto cell = area.removeFromLeft (knobColumnWidth);

            if (knob != nullptr)
                knob->setBounds (cell.withSizeKeepingCentre (chainKnobSize, chainKnobSize));
        };

        if (crossover != nullptr || showsCrossoverColumn)
            placeKnob (crossover);

        placeKnob (volume);
        placeKnob (pan);

        if (keys != nullptr && velocities != nullptr)
        {
            area.removeFromLeft (Space::s);
            const auto velocitiesWidth = std::max (velocitiesMinimum, area.getWidth() / 3);
            auto bars = area;
            velocities->setBounds (bars.removeFromRight (velocitiesWidth));
            bars.removeFromRight (Space::s);
            keys->setBounds (bars);
        }
    }

    ProjectContext& context;
    const DeviceID chain;
    ChainSwitch power;
    juce::Label name, part;
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<ParameterAttachment> attachments;
    Knob* crossover = nullptr;
    Knob* volume = nullptr;
    Knob* pan = nullptr;
    std::unique_ptr<RangeBar> keys, velocities; // an instrument container's chain's
    bool heard = true;
    bool showsCrossoverColumn = false; // the first band's row, in a container split into bands
};

//==============================================================================
ContainerEditor::ContainerEditor (ProjectContext& projectContext, DeviceID id)
    : context (projectContext),
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
    hint.setText (instrument ? "Each chain plays its instrument on the notes in its keys and velocities: + Instrument, under a chain "
                               "at the left, chooses one."
                             : "Its chains play side by side and add up: + Effect, under a chain at the left, adds an effect to it.",
                  juce::dontSendNotification);
    addChildComponent (hint);

    addButton.setTitle ("Add chain");
    addButton.setTooltip (instrument ? "Adds a chain, for another instrument layered with the others, or played on other keys"
                                     : "Adds a chain, which plays beside the others, with its own devices, volume, pan, mute and solo");
    addButton.onClick = [this] { addChain (context, container); };
    addAndMakeVisible (addButton);

    for (auto* label : { &splitLabel, &hint })
        setThemeColour (*label, juce::Label::textColourId, &Palette::textMuted);

    refresh();
}

ContainerEditor::~ContainerEditor() = default;

void ContainerEditor::refresh()
{
    const auto chains = getChains (context, container);
    const auto split = getContainerSplit (context, container);

    if (getIDs (chains) != shownChains || split != shownSplit)
    {
        rows.clear();

        for (const auto& chain : chains)
            addAndMakeVisible (rows.add (new Row (context, chain, instrument)));

        shownChains = getIDs (chains);
        shownSplit = split;

        // Every row keeps a column for the bands' crossovers, the first band's empty.
        const auto bands = split == ContainerSplit::bands && chains.size() > 1;

        for (auto* row : rows)
            row->showsCrossoverColumn = bands;
    }
    else
    {
        for (size_t i = 0; i < chains.size(); ++i)
            rows[(int) i]->show (chains[i]);
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
    for (auto* row : rows)
        row->updateValues();
}

juce::Component* ContainerEditor::findRow (DeviceID chain) const
{
    for (auto* row : rows)
        if (row->chain == chain)
            return row;

    return nullptr;
}

int ContainerEditor::getMinimumHeight() const
{
    return controlHeight + Space::s + rulerHeight + rows.size() * (rowHeight + rowGap) + Space::xs + controlHeight
           + (hint.isVisible() ? controlHeight : 0) + Space::s;
}

int ContainerEditor::getWidthForHeight (int) const
{
    return instrument ? instrumentsWidth : effectsWidth;
}

int ContainerEditor::getMinimumHeightAtWidth (int) const
{
    return getMinimumHeight();
}

void ContainerEditor::paint (juce::Graphics& g)
{
    if (rows.isEmpty())
        return;

    // Over the rows, each column's title: a band's crossover, the volume, the pan and an instrument container's velocities.
    const auto& p = getPalette (*this);
    const auto top = rows.getFirst()->getY() - rowGap - rulerHeight;
    g.setFont (juce::FontOptions (TextSize::small));
    g.setColour (p.textMuted);

    const auto title = [&] (const juce::Component* part, const char* text)
    {
        if (part == nullptr)
            return;

        const auto bounds = getLocalArea (part->getParentComponent(), part->getBounds());
        g.drawText (text, juce::Rectangle<int> (bounds.getCentreX() - knobColumnWidth / 2, top, knobColumnWidth, rulerHeight - 2),
                    juce::Justification::centred, false);
    };

    const Knob* crossover = nullptr;

    for (auto* row : rows)
        crossover = crossover != nullptr ? crossover : row->crossover;

    title (crossover, "Crossover");
    title (rows.getFirst()->volume, "Volume");
    title (rows.getFirst()->pan, "Pan");

    // An instrument container's keyboard, along its chains' keys: a mark at each C, named every two octaves.
    if (! instrument || rows.getFirst()->keys == nullptr)
        return;

    const auto* keys = rows.getFirst()->keys.get();
    const auto* velocities = rows.getFirst()->velocities.get();
    g.drawText ("Velocity",
                getLocalArea (velocities->getParentComponent(), velocities->getBounds()).withY (top).withHeight (rulerHeight - 2),
                juce::Justification::centred, false);
    const auto bounds = getLocalArea (keys, keys->getLocalBounds());

    for (int key = 0; key <= 120; key += 12)
    {
        const auto x = (float) bounds.getX() + (float) keys->getPositionOfValue (key);
        g.setColour (p.border);
        g.fillRect (juce::Rectangle<float> (x, (float) top + rulerHeight - 4.0f, 1.0f, 4.0f));

        if (key % 24 == 0)
        {
            g.setColour (p.textMuted);
            g.drawText (RangeBar::describe (RangeBar::Kind::keys, key),
                        juce::Rectangle<float> (x - 20.0f, (float) top, 40.0f, rulerHeight - 3.0f), juce::Justification::centred, false);
        }
    }
}

void ContainerEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::s, 0);
    area.removeFromTop (Space::s);
    auto top = area.removeFromTop (controlHeight);

    if (instrument)
    {
        hint.setBounds (top);
    }
    else
    {
        splitLabel.setBounds (top.removeFromLeft (splitLabelWidth));
        top.removeFromLeft (Space::xs);
        splitChoice.setBounds (top.removeFromLeft (splitWidth));
    }

    // The columns' titles (and an instrument container's keyboard) over the rows.
    area.removeFromTop (rulerHeight);

    for (auto* row : rows)
    {
        area.removeFromTop (rowGap);
        row->setBounds (area.removeFromTop (rowHeight));
    }

    area.removeFromTop (Space::xs);
    auto bottom = area.removeFromTop (controlHeight);
    addButton.setBounds (bottom.removeFromLeft (addWidth));

    if (! instrument && hint.isVisible())
        hint.setBounds (area.removeFromTop (controlHeight));
}
} // namespace phazite::ui
