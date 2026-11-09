// SPDX-License-Identifier: AGPL-3.0-or-later
// What the Devices view shows of a container itself, beside its macros (DevicesView.h; docs/design.md, section 9): how its
// chains share the sound that comes in (its split), then its chains as the rows of a small mixer, as Ableton Live's chain
// list shows a rack's: each chain's one switch for mute and solo, as a track's; its name (double-click to rename it,
// right-click for its menu) and what part of the sound it plays; its crossover (a band's, but the first's), its volume and
// its pan; an instrument container's chain's keys and velocities as bars along one keyboard and one range of velocities,
// dragged by their ends; and a button that adds a chain. The chains' devices are in the column at the left, under them.
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

// A chain's menu, as its name shows it here and in the Devices view's column: Rename (rename(), which edits its name where
// it shows), Duplicate, Move Up and Move Down (not in a container split into bands, whose chains are in their bands'
// order), and Remove (not its container's last chain). changed() runs after an edit, so that what shows it shows it at once.
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
    ContainerEditor (ProjectContext&, DeviceID container);
    ~ContainerEditor() override;

    DeviceID getContainer() const noexcept { return container; }

    void refresh();      // as the container is now: rows made again for chains that changed
    void updateValues(); // its knobs and switches, as the display ticker's ticks show them

    // A chain's row, for tests; nullptr for none.
    juce::Component* findRow (DeviceID chain) const;

    int getMinimumHeight() const override;
    int getWidthForHeight (int) const override;
    int getMinimumHeightAtWidth (int) const override;

    void paint (juce::Graphics&) override;
    void resized() override;

    struct Row;

private:
    ProjectContext& context;
    const DeviceID container;
    const bool instrument;
    juce::Label splitLabel { {}, "Split" }, hint;
    juce::ComboBox splitChoice;
    juce::OwnedArray<Row> rows;
    std::vector<DeviceID> shownChains;
    std::optional<ContainerSplit> shownSplit;
    juce::TextButton addButton { "+ Chain" };

    void showSplit();
};
} // namespace phazite::ui
