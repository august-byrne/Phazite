// SPDX-License-Identifier: AGPL-3.0-or-later
// A burst of the mouse wheel on a control, as one gesture: one undo step and, for a value a device's latency follows, one new
// plan, however many notches it has (known bug 201 until 2026-11-18), as a drag of the control is one. A knob or a fader (ValueSlider) and
// a graph's handle (graph::WheelGesture) turn through it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace phazite::ui
{
// The first notch begins the burst (its owner's begin(): a parameter's gesture); each notch after it joins it; and it ends
// (its owner's end) when the wheel has rested for half a second, or at once at:
//   - a mouse press anywhere, and the pointer leaving what it turns (before a press elsewhere can begin a gesture of its own,
//     which one in progress would have joined);
//   - a turn of the wheel on another control, which begins a burst of its own;
//   - what it turns hidden, or destroyed with this;
//   - endAll(), which its owner calls where a key reaches the control, the control loses the keyboard focus or a drag
//     starts on it, and the window's shortcuts call before they run a command (an Undo then takes the burst back whole).
// One is open at a time, in the whole app. Message thread only.
class WheelBurst : private juce::Timer,
                   private juce::ComponentListener,
                   private juce::MouseListener
{
public:
    explicit WheelBurst (juce::Component& turned); // what the wheel turns, which must outlive this (its owner's member)
    ~WheelBurst() override;                        // ends it, if it is open

    // A notch: begins the burst if this one isn't open, ending the one that is first (onBegin then runs, and onEnd will end
    // it); then it rests for half a second before it ends.
    void turn (const std::function<void()>& onBegin, std::function<void()> onEnd);

    bool isOpen() const noexcept { return open == this; }
    void end();

    // Ends the burst that is open, if one is. Tests call it in place of waiting for the wheel to rest.
    static void endAll();
    static bool isAnyOpen() noexcept { return open != nullptr; }

    static constexpr int restMilliseconds = 500;

private:
    struct Presses; // the desktop's presses, heard while a burst is open

    juce::Component& turned;
    std::function<void()> ending;
    static inline WheelBurst* open = nullptr;

    void listen (bool shouldListen);
    void timerCallback() override { end(); }
    void componentVisibilityChanged (juce::Component&) override;
    void componentBeingDeleted (juce::Component&) override { end(); }
    void mouseExit (const juce::MouseEvent&) override;

    JUCE_DECLARE_NON_COPYABLE (WheelBurst)
};
} // namespace phazite::ui
