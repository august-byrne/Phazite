// SPDX-License-Identifier: AGPL-3.0-or-later
// The FM synth's editor (devices/fm/FmSynth.h; roadmap step 4.7; docs/design.md, section 9), laid out as the FM synths whose
// editors are praised lay theirs out (B156): the algorithm played, drawn large, and every operator at once, as Korg's opsix
// and Yamaha's reface DX show theirs, one group of their controls at a time.
// - Down its left, the algorithm: its name between arrows that step through the eight, and its picture, each operator a box
//   with its number, the carriers (heard) in the device's colour along the line to what comes out, the modulators in
//   modulation's colour over what they modulate, each fed back with a loop at its side; the boxes glow with the newest note's
//   envelopes. A click on it shows all eight, each with what it is good for, to pick one (one undo step); the arrow keys
//   step through them. Under it, the harmonics the operators make together as a note strikes (or as the newest note plays),
//   so that what each change does shows; and Timbre.
// - Beside it, the four operators in columns, each headed by its switch, its number in its role's colour and what it does in
//   the algorithm ("Heard", or what it modulates: "→ 1"), then its envelope, whose points drag and whose stages bend, its
//   level and its ratio and fine (or its frequency, while fixed). Under them, tabs for the rest of their controls, all four
//   at once: Envelope (times, sustain and bends), Tone (wave, feedback and where in its wave it starts each note, faint
//   while the phases run free), Pitch (Fixed and detune) and Scaling (velocity and key). An operator's column shows faint
//   while it is off; the one under the pointer lights its box.
// Then Pitch over Voice, the Filter, switched on at its title, with its envelope beside it as the synth's editor has it
// (faint while the filter is off), and the LFO, switched on at its title. A dock narrower than them shows them in pages.
#pragma once

#include "DeviceEditorBase.h"
#include "SynthParts.h"

#include <array>

namespace phazite::ui
{
class FmEditor : public DeviceEditorBase,
                 private DisplayTicker::Display
{
public:
    static constexpr int numOperators = 4;

    explicit FmEditor (const DeviceHandle&);
    ~FmEditor() override;

    // The algorithm's picture (a click shows all eight, the arrow keys step through them), the arrows beside its name, and
    // the eight while they show (nullptr otherwise).
    juce::Component& getAlgorithmView() const;
    juce::Button& getAlgorithmStep (bool next) const;
    juce::Component* getAlgorithmGrid() const;
    void showAlgorithmGrid();

    // The groups of the operators' controls under their tabs, and the one shown (the envelopes' at first).
    enum class Group
    {
        envelope,
        tone,
        pitch,
        scaling
    };

    static constexpr int numGroups = 4;

    juce::Button& getGroupTab (Group) const;
    Group getShownGroup() const noexcept { return shownGroup; }
    void showGroup (Group);

    // An operator's column, and what it does in the algorithm as its column says it ("Heard", "→ 1").
    juce::Component& getOperatorColumn (int op) const;
    juce::String getRoleText (int op) const;

    // The operator whose box is lit as the pointer is over it or its column, or -1.
    int getHoveredOperator() const noexcept { return hovered; }

    // Shows what changed elsewhere (undo, automation, the algorithm, the operators' roles); the ticker does it.
    void refresh();

private:
    class AlgorithmView;
    class AlgorithmGrid;
    class OperatorColumn;
    class OperatorsPart;
    class GroupTab;
    class HoverWatcher;

    AlgorithmView* algorithmView = nullptr;
    OperatorsPart* operators = nullptr;
    std::array<OperatorColumn*, numOperators> columns {};
    std::array<GroupTab*, numGroups> groupTabs {};
    std::unique_ptr<HoverWatcher> hoverWatcher; // the operator whose column or group the pointer is over
    juce::Component::SafePointer<juce::CallOutBox> gridBox;
    juce::Component* filterEnvelopePart = nullptr; // faint while the filter is off
    ParameterHandle filterOn;
    Group shownGroup = Group::envelope;
    int hovered = -1, hoveredColumn = -1;

    void setHovered (int op);
    void tick() override { refresh(); }
};
} // namespace phazite::ui
