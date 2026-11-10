// SPDX-License-Identifier: AGPL-3.0-or-later
// A text field lets the keys go when the mouse is pressed elsewhere, and shows no selection once they have gone
// (src/ui/input/TextFieldRelease.h), driven as the user would.
#include "UiTestHelpers.h"
#include "ui/input/TextFieldRelease.h"

namespace
{
// What a press can land on besides a text field: what doesn't take the keys (a panel's background, a button) and what does
// (the playlist).
struct Panel : public juce::Component
{
    Panel()
    {
        field.setText ("Bass");
        taker.setWantsKeyboardFocus (true);

        for (auto* c : { static_cast<juce::Component*> (&field), &background, &taker })
            addAndMakeVisible (c);
    }

    void resized() override
    {
        field.setBounds (10, 10, 100, 24);
        background.setBounds (10, 50, 100, 24);
        taker.setBounds (10, 90, 100, 24);
    }

    juce::TextEditor field;
    juce::Component background, taker;
};

// A menu or a dialog, modal over the window while it is up; a press outside it, which it ignores, doesn't beep.
struct Modal : public juce::Component
{
    void inputAttemptWhenModal() override {}
};
} // namespace

TEST_CASE ("A press away from a text field lets the keys go, and a field the keys left shows no selection")
{
    Panel panel;
    phazite::test::TestWindow window (panel, 200, 200);
    phazite::ui::TextFieldRelease release;
    phazite::test::Mouse mouse (panel);

    // Clicked, the field has the keys; its text selected, as typing over it starts.
    const auto clickField = [&]
    {
        mouse.click (panel.field.getBounds().getCentre());
        panel.field.selectAll();
        phazite::test::dispatchPendingMessages();
        REQUIRE (panel.field.hasKeyboardFocus (false));
        REQUIRE_FALSE (panel.field.getHighlightedRegion().isEmpty());
    };

    // A press on what doesn't take the keys, which JUCE leaves in the field: the field lets them go, so that they reach the
    // window's shortcuts, and shows no selection; its text stays.
    clickField();
    mouse.click (panel.background.getBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);
    CHECK (panel.field.getHighlightedRegion().isEmpty());
    CHECK (panel.field.getText() == juce::String ("Bass"));

    // A press on what takes the keys: they go there, and the field shows no selection either.
    clickField();
    mouse.click (panel.taker.getBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (panel.taker.hasKeyboardFocus (false));
    CHECK (panel.field.getHighlightedRegion().isEmpty());

    // A press in the field keeps them there.
    clickField();
    mouse.click (panel.field.getBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (panel.field.hasKeyboardFocus (false));

    // While a menu is up over the field, a press in the menu (its Copy, say) is the field's: it keeps the keys and its
    // selection.
    clickField();
    Modal menu;
    panel.addAndMakeVisible (menu);
    menu.setBounds (120, 10, 60, 60);
    menu.enterModalState (false);
    mouse.click (menu.getBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (panel.field.hasKeyboardFocus (false));
    CHECK_FALSE (panel.field.getHighlightedRegion().isEmpty());
    menu.exitModalState (0);
    phazite::test::dispatchPendingMessages();
    panel.removeChildComponent (&menu);

    // A field in a dialog: a press on the dialog lets the field go; one outside it, which the dialog ignores, doesn't.
    Modal dialog;
    juce::TextEditor dialogField;
    panel.addAndMakeVisible (dialog);
    dialog.setBounds (120, 10, 70, 120);
    dialog.addAndMakeVisible (dialogField);
    dialogField.setBounds (5, 5, 60, 24);
    dialog.enterModalState (false);
    const auto clickDialogField = [&]
    {
        phazite::test::Mouse dialogMouse (dialog);
        dialogMouse.click (dialogField.getBounds().getCentre());
        phazite::test::dispatchPendingMessages();
        REQUIRE (dialogField.hasKeyboardFocus (false));
    };

    clickDialogField();
    mouse.click (panel.background.getBounds().getCentre()); // outside the dialog
    phazite::test::dispatchPendingMessages();
    CHECK (dialogField.hasKeyboardFocus (false));
    mouse.click (dialog.getBounds().getCentre()); // on the dialog, under its field
    phazite::test::dispatchPendingMessages();
    CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);
    dialog.exitModalState (0);
    phazite::test::dispatchPendingMessages();
    panel.removeChildComponent (&dialog);
}

TEST_CASE ("A press away from a knob that has the keys lets them go too, so that the number keys reach the shortcuts again")
{
    // The owner, 2026-11-09: a knob that has the keys takes a number typed as its value (ui::ValueSlider), and the tools'
    // shortcuts are numbers.
    Panel panel;
    juce::Slider knob (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
    knob.setWantsKeyboardFocus (true);
    panel.addAndMakeVisible (knob);
    knob.setBounds (120, 10, 40, 40);
    phazite::test::TestWindow window (panel, 200, 200);
    phazite::ui::TextFieldRelease release;
    phazite::test::Mouse mouse (panel);

    mouse.click (knob.getBounds().getCentre());
    REQUIRE (knob.hasKeyboardFocus (false));
    mouse.click (panel.background.getBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);

    // A press on it keeps them; one on what takes them gives them there.
    mouse.click (knob.getBounds().getCentre());
    mouse.click (knob.getBounds().getCentre());
    CHECK (knob.hasKeyboardFocus (false));
    mouse.click (panel.taker.getBounds().getCentre());
    CHECK (panel.taker.hasKeyboardFocus (false));
}
