// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WheelBurst.h"

#include <utility>

namespace phazite::ui
{
// A press anywhere in the app ends the burst open: the desktop's listeners hear every press, after what was pressed.
struct WheelBurst::Presses final : public juce::MouseListener
{
    void mouseDown (const juce::MouseEvent&) override { WheelBurst::endAll(); }

    static Presses& get()
    {
        static Presses presses;
        return presses;
    }
};

WheelBurst::WheelBurst (juce::Component& turnedComponent)
    : turned (turnedComponent)
{
}

WheelBurst::~WheelBurst()
{
    end();
}

void WheelBurst::turn (const std::function<void()>& onBegin, std::function<void()> onEnd)
{
    if (! isOpen())
    {
        endAll(); // a turn on another control is a step of its own
        open = this;
        ending = std::move (onEnd);
        listen (true);

        if (onBegin != nullptr)
            onBegin();
    }

    startTimer (restMilliseconds);
}

void WheelBurst::end()
{
    if (! isOpen())
        return;

    stopTimer();
    listen (false);
    open = nullptr;

    // Taken out first: what it runs may turn the wheel again, or end every burst.
    if (const auto finish = std::exchange (ending, nullptr))
        finish();
}

void WheelBurst::endAll()
{
    if (open != nullptr)
        open->end();
}

void WheelBurst::listen (bool shouldListen)
{
    if (shouldListen)
    {
        juce::Desktop::getInstance().addGlobalMouseListener (&Presses::get());
        turned.addComponentListener (this);
        turned.addMouseListener (this, true);
    }
    else
    {
        juce::Desktop::getInstance().removeGlobalMouseListener (&Presses::get());
        turned.removeComponentListener (this);
        turned.removeMouseListener (this);
    }
}

void WheelBurst::componentVisibilityChanged (juce::Component& component)
{
    if (! component.isVisible())
        end();
}

void WheelBurst::mouseExit (const juce::MouseEvent& e)
{
    // Out of what it turns, not into one of its children.
    if (! turned.getLocalBounds().contains (e.getEventRelativeTo (&turned).getPosition()))
        end();
}
} // namespace phazite::ui
