// SPDX-License-Identifier: AGPL-3.0-or-later
// What the Devices view shows of a container itself, beside its macros (DevicesView.h; docs/design.md, section 9): along its
// top, beside the container's name, how its chains share the sound that comes in (its split) and a button that adds a chain;
// under it, its chains side by side, as the strips of a small mixer, each a card: its one switch for mute and solo, as a
// track's, its name (double-click to rename it) with its menu's
// button (a right-click on its name opens it too), and what part of the sound it plays; its crossover (a band's, but the
// first's), its volume and its pan; an instrument container's chain's keys and velocities, as bars along the keyboard and the
// range of velocities, dragged by their ends; then its devices one under another, as the column stacks a channel's (an
// instrument container's chain's instrument first, or a button that chooses one), each its switch, its name and its mix,
// with a button after them that adds an effect. A device's name clicked shows the device in the chains' place, where it is
// along its top (the owner, 2026-11-09: in the column, a container's chains and devices made it "very busy and cluttered";
// here they are laid out); right-clicked, its menu; dragged, it moves among its chain's devices or into another chain. The
// chains scroll sideways where the dock is narrower than they are, and up and down where it is shorter, under the top line
// and beside the macros, which stay.
#pragma once

#include "model/mixer/ContainerModel.h"
#include "ui/components/Controls.h"
#include "ui/devices/DeviceEditors.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace phazite
{
class SongGesture;
}

namespace phazite::ui
{
class CommandBridge;

// A container's chain's one switch for mute and solo, as a track's (TrackSwitch.h): a click mutes it, a right-click solos
// it alone (and again unsolos it), Ctrl+right-click solos it along with the others soloed. Neither is an undo step.
class ChainSwitch : public juce::Button
{
public:
    explicit ChainSwitch (ProjectContext&);

    void setChain (DeviceID);
    void update(); // shows the chain's mute and solo as they are now

    void clicked (const juce::ModifierKeys&) override;
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    ProjectContext& context;
    DeviceID chain;
    std::optional<MuteSolo> shown;

    using juce::Button::clicked;
};

// A chain's menu, as its card shows it: Rename (rename(), which edits its name where it shows), Duplicate, Move Left and
// Move Right (not in a container split into bands, whose chains are in their bands' order), and Remove (not its container's
// last chain). changed() runs after an edit, so that what shows it shows it at once.
void showChainMenu (ProjectContext&, DeviceID chain, juce::Component& target, std::function<void()> rename, std::function<void()> changed);

// A range of keys or of velocities, an instrument container's chain's, as a bar along its scale, dragged by its ends (each
// drag one undo step): two values of a slider, as JUCE's two-value sliders take them, so that keys and screen readers work
// as on any slider.
class RangeBar : public ValueSlider
{
public:
    enum class Kind
    {
        keys,
        velocities
    };

    RangeBar (ProjectContext&, DeviceID chain, Kind);
    ~RangeBar() override;

    void show (NoteRange); // unless it is being dragged
    void paint (juce::Graphics&) override;

    // How a value of its kind reads: a key's name ("C4", 60), or a velocity's number.
    static juce::String describe (Kind, int value);

private:
    ProjectContext& context;
    const DeviceID chain;
    const Kind kind;
    std::unique_ptr<SongGesture> gesture; // while it is dragged
};

class ContainerEditor : public juce::Component,
                        public FlexibleHeight
{
public:
    ContainerEditor (ProjectContext&, CommandBridge&, DeviceID container);
    ~ContainerEditor() override;

    DeviceID getContainer() const noexcept { return container; }

    // A device's name clicked, with the keys held then: what shows the container shows the device, and selects it. It may
    // take this editor away, as it shows the device in its place.
    std::function<void (const DeviceSlot&, const juce::ModifierKeys&)> onDeviceClicked;

    // After an edit from a menu here, or a drop: what shows the container shows what it did at once.
    std::function<void()> onChanged;

    void refresh();      // as the container is now: a chain's card made again where its devices changed
    void updateValues(); // its knobs and switches, as the display ticker's ticks show them

    // Scrolls a chain's card into view, where the chains are more than the dock shows.
    void showChain (DeviceID chain);

    // A chain's card, and a device's line in its chain's card, for tests; nullptr for none.
    juce::Component* findChain (DeviceID chain) const;
    juce::Component* findDevice (DeviceID device) const;

    int getMinimumHeight() const override;
    int getWidthForHeight (int) const override;
    int getMinimumHeightAtWidth (int) const override;

    void resized() override;

    struct DeviceLine;
    struct ChainCard;

private:
    ProjectContext& context;
    CommandBridge& commands;
    const DeviceID container;
    const bool instrument;
    juce::Label splitLabel { {}, "Split" }, hint;
    juce::ComboBox splitChoice;
    juce::Viewport chainsView; // the cards, scrolled sideways and up and down where they are more than it shows
    juce::Component chainsRow; // what it scrolls: the cards
    juce::OwnedArray<ChainCard> cards;
    juce::TextButton addButton { "+ Chain" };
    std::vector<DeviceID> shownChains;
    std::optional<ContainerSplit> shownSplit;

    int getTopHeight() const;   // its split, + Chain and the hint, beside the container's name
    int getCardsHeight() const; // the tallest card's, which they all are
    int getStripWidth() const;
    void showSplit();
    void layOutCards();
    void showDeviceMenu (const DeviceSlot&, juce::Component& target);
    void changed(); // onChanged, if this is still here
};
} // namespace phazite::ui
