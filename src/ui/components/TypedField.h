// SPDX-License-Identifier: AGPL-3.0-or-later
// A field floated over a control to type its value in exactly (the owner, 2026-11-09: "a tiny bit off is totally wrong"), as
// Ableton Live and FL Studio open one over a knob, and Ableton Live's Edit Value and FL Studio's Type in value... over an
// automation point (2026-11-13). It opens with the value's text, all of it selected, or with what was typed, the caret after
// it; Enter, or a click elsewhere, takes what it says (onApply), and Esc leaves the value as it was, as does its own text set
// again. It belongs to a component, which it follows as that moves (a scroll), and which it leaves, its text untaken, as that
// hides; its owner places it (getTarget) and deletes it a message after it finished (onFinished), out of what ended it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace phazite::ui
{
class TypedField final : public juce::TextEditor,
                         private juce::ComponentMovementWatcher
{
public:
    explicit TypedField (juce::Component& owner);

    // What it covers, in its parent's coordinates; the texts it must be wide enough for (a slider's least, most and value);
    // what it says when it finishes with it taken; and that it finished, hidden, which its owner deletes a message later.
    std::function<juce::Rectangle<int> (const juce::Component& parent)> getTarget;
    std::function<juce::StringArray()> getTexts;
    std::function<void (const juce::String&)> onApply;
    std::function<void()> onFinished;

    // Opens it with the value's text, all of it selected, or, with typed, what was typed. Put it in its parent first
    // (findParent()).
    void show (const juce::String& valueText, const juce::String& typed = {});

    // Ends it, once: with what it says (Enter, or the keys gone elsewhere) or without (Esc, or its owner gone), the keys back
    // to its owner after Enter or Esc.
    void finish (bool apply, bool keysBack);
    bool isFinished() const noexcept { return finished; }

    // Where a field over a component goes: in a call-out or a dialog that holds the keys while it is up (a modulator's editor),
    // in it, as a field outside it would get no keys; elsewhere in its window's content (a window lays out its own children).
    static juce::Component* findParent (juce::Component& owner);

    // At least this wide, as a compact knob's place, and this tall: a line of small text in a box.
    static int getMinimumWidth() noexcept;
    static int getLineHeight() noexcept;

    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override;

private:
    juce::Component& owner;
    bool finished = false;
    juce::String shownValue; // the value's text it opened with, if it did

    void place(); // over its target, as wide as its texts, inside its parent

    using juce::ComponentMovementWatcher::componentMovedOrResized;
    using juce::ComponentMovementWatcher::componentVisibilityChanged;
    void componentMovedOrResized (bool, bool) override;
    void componentPeerChanged() override;
    void componentVisibilityChanged() override;

    JUCE_DECLARE_NON_COPYABLE (TypedField)
};
} // namespace phazite::ui
