// SPDX-License-Identifier: AGPL-3.0-or-later
// A text field lets the keys go when the mouse is pressed anywhere else, as text fields do in other apps, so that they go
// back to the window's shortcuts (Space plays again); and a field the keys left shows no selection (the owner's request,
// 2026-11-07). JUCE leaves the keys in a field when a press lands on what doesn't take them (a panel's background, a
// button, a knob), and draws a field's selection at half strength once the keys have gone.
//
// A press in a menu or a call-out over the field (its own menu's Cut, Copy and Paste) is the field's, and one outside the
// dialog a field is in changes nothing, as the dialog ignores it. A field whose window went behind another app's keeps its
// selection: the keys come back to it with the window.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class TextFieldRelease : private juce::MouseListener,
                         private juce::FocusChangeListener
{
public:
    TextFieldRelease(); // hears every press and every move of the keys in the app, until it goes
    ~TextFieldRelease() override;

private:
    juce::Component::SafePointer<juce::TextEditor> field; // the field that has the keys, or had them last

    void mouseDown (const juce::MouseEvent&) override;
    void globalFocusChanged (juce::Component* focused) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TextFieldRelease)
};

// Takes a field's selection away, leaving its caret where it was, as a field shows once the keys have gone.
void clearSelection (juce::TextEditor&);
} // namespace phazite::ui
