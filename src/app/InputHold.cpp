// SPDX-License-Identifier: AGPL-3.0-or-later
#include "InputHold.h"

#include <vector>

namespace phazite
{
// Modal, in its window if it has one (an invisible speck, which takes the keys there), else nowhere on the screen. Input
// tried meanwhile is let go quietly; keys go to passKey, which lets through the few that change nothing, and their changes
// (a key let go) to keysChanged, which the window's key listeners would have heard.
struct InputHold::Holder final : juce::Component
{
    Holder (std::function<bool (const juce::KeyPress&)> pass, std::function<void (bool)> changed)
        : passKey (std::move (pass)),
          keysChanged (std::move (changed))
    {
        setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (true);
        setAccessible (false); // nothing to say: a progress window speaks for a wait that lasts
    }

    void inputAttemptWhenModal() override {}

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (passKey != nullptr)
            passKey (key);

        return true; // the others are held
    }

    // Taken here, as JUCE stops at the component with the keys; heard by what passKey runs too, so that a key it ran for and
    // let go is pressed again, not taken for one held.
    bool keyStateChanged (bool isKeyDown) override
    {
        if (keysChanged != nullptr)
            keysChanged (isKeyDown);

        return true;
    }

    const std::function<bool (const juce::KeyPress&)> passKey;
    const std::function<void (bool)> keysChanged;
};

namespace
{
    // Whether the keyboard is in a text field, whose edit the hold mustn't end by taking the focus (TypedField commits as it
    // loses the focus).
    bool isTyping()
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        return dynamic_cast<juce::TextEditor*> (focused) != nullptr;
    }
} // namespace

InputHold::InputHold() = default;

InputHold::~InputHold()
{
    if (holds > 0)
    {
        holds = 1;
        releaseInput();
    }
}

void InputHold::setWindow (juce::Component* windowToHold, std::function<bool (const juce::KeyPress&)> pass,
                           std::function<void (bool)> changed)
{
    window = windowToHold;
    passKey = std::move (pass);
    keysChanged = std::move (changed);
}

void InputHold::holdInput()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (holds++ > 0)
        return;

    focusBefore = juce::Component::getCurrentlyFocusedComponent();

    // In its window, to take the keys there; not while a text field has them, whose edit stays open, and whose keys (Space
    // among them) then do nothing: they reach the holder, as the field is under it, and go no further.
    const auto typing = isTyping();
    const auto takesKeys = window != nullptr && window->isShowing() && ! typing;
    holder = std::make_unique<Holder> (typing ? nullptr : passKey, keysChanged);

    if (takesKeys)
    {
        holder->setBounds (0, 0, 1, 1);
        window->addAndMakeVisible (*holder);
    }

    holder->enterModalState (takesKeys, nullptr, false);
}

void InputHold::releaseInput()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (holds == 0 || --holds > 0)
        return;

    const auto hadKeys = holder->hasKeyboardFocus (true);
    holder->exitModalState (0);
    holder = nullptr;

    // The focus back where it was, unless something took it meanwhile (a swap started as a keep lands, which gives the
    // browser's tree the keys).
    if (auto* focused = focusBefore.getComponent();
        focused != nullptr && focused->isShowing() && (hadKeys || juce::Component::getCurrentlyFocusedComponent() == nullptr))
        focused->grabKeyboardFocus();

    // Keys let go while it held reached no window: each hears now that its keys changed, and asks which are still down.
    std::vector<juce::Component::SafePointer<juce::Component>> windows;

    for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
        windows.emplace_back (&juce::ComponentPeer::getPeer (i)->getComponent());

    for (const auto& each : windows)
        if (auto* shown = each.getComponent())
            if (auto* peer = shown->getPeer())
                peer->handleKeyUpOrDown (false);
}

bool InputHold::isHold (const juce::Component* component)
{
    return dynamic_cast<const Holder*> (component) != nullptr;
}
} // namespace phazite
