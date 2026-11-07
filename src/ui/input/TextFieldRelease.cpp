// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TextFieldRelease.h"

namespace phazite::ui
{
TextFieldRelease::TextFieldRelease()
{
    auto& desktop = juce::Desktop::getInstance();
    desktop.addGlobalMouseListener (this);
    desktop.addFocusChangeListener (this);
    field = dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent());
}

TextFieldRelease::~TextFieldRelease()
{
    auto& desktop = juce::Desktop::getInstance();
    desktop.removeFocusChangeListener (this);
    desktop.removeGlobalMouseListener (this);
}

void TextFieldRelease::mouseDown (const juce::MouseEvent& e)
{
    // JUCE has given the keys to what the press landed on already, if it takes them; this hears the press after that.
    auto* focused = dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent());
    auto* pressed = e.eventComponent;

    if (focused == nullptr || pressed == nullptr || pressed == focused || focused->isParentOf (pressed))
        return;

    // While a menu, a call-out or a dialog is up: only a press in the one the field is in lets it go (its menu's items,
    // over it, are the field's; a press outside its dialog does nothing).
    if (auto* modal = juce::Component::getCurrentlyModalComponent())
        if (modal->isParentOf (focused) == pressed->isCurrentlyBlockedByAnotherModalComponent())
            return;

    clearSelection (*focused);
    focused->giveAwayKeyboardFocus();
}

void TextFieldRelease::globalFocusChanged (juce::Component* focused)
{
    // The keys left the app (its window went behind another app's), or a press let the field go and took its selection:
    // the field keeps what it shows.
    if (focused == nullptr)
        return;

    if (auto* left = field.getComponent(); left != nullptr && left != focused)
        clearSelection (*left);

    field = dynamic_cast<juce::TextEditor*> (focused);
}

void clearSelection (juce::TextEditor& field)
{
    if (! field.getHighlightedRegion().isEmpty())
        field.setCaretPosition (field.getCaretPosition());
}
} // namespace phazite::ui
