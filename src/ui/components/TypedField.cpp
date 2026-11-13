// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TypedField.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>
#include <utility>

namespace phazite::ui
{
TypedField::TypedField (juce::Component& c)
    : juce::ComponentMovementWatcher (&c),
      owner (c)
{
    setTitle (c.getTitle());
    setDescription ("Type a value and press Enter; Esc leaves it as it was");
    setMultiLine (false);
    setReturnKeyStartsNewLine (false);
    setScrollbarsShown (false);
    setJustification (juce::Justification::centred);
    setIndents (Space::xs, 0);
    setFont (LookAndFeel::getNumberFont (TextSize::small));
    setTooltip (getDescription());
    setPopupMenuEnabled (false); // a menu would take the keys, which ends it: Ctrl+C and Ctrl+V work
}

int TypedField::getMinimumWidth() noexcept
{
    return compactKnobSize + Space::l;
}

int TypedField::getLineHeight() noexcept
{
    return juce::roundToInt (TextSize::small) + 2 * Space::xs;
}

juce::Component* TypedField::findParent (juce::Component& c)
{
    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal != nullptr && modal->isParentOf (&c))
        return modal;

    auto* window = c.getTopLevelComponent();

    if (auto* resizable = dynamic_cast<juce::ResizableWindow*> (window))
        if (auto* content = resizable->getContentComponent(); content != nullptr && content->isParentOf (&c))
            return content;

    return window != &c ? window : nullptr;
}

void TypedField::show (const juce::String& valueText, const juce::String& typed)
{
    // Opened with the value, which set again unchanged would change nothing, or with what was typed.
    shownValue = typed.isEmpty() ? valueText : juce::String();
    setText (typed.isEmpty() ? valueText : typed, false);
    place();
    grabKeyboardFocus();

    if (typed.isEmpty())
        selectAll();
    else
        moveCaretToEnd();
}

void TypedField::place()
{
    auto* parent = getParentComponent();

    if (parent == nullptr || getTarget == nullptr)
        return;

    auto textWidth = juce::GlyphArrangement::getStringWidth (getFont(), getText());

    if (getTexts != nullptr)
        for (const auto& text : getTexts())
            textWidth = juce::jmax (textWidth, juce::GlyphArrangement::getStringWidth (getFont(), text));

    const auto target = getTarget (*parent);
    const auto width = juce::jmax (target.getWidth(), (int) std::ceil (textWidth) + 2 * Space::s, getMinimumWidth());
    const auto height = juce::jlimit (getLineHeight(), controlHeight, target.getHeight());
    setBounds (juce::Rectangle<int> (width, height).withCentre (target.getCentre()).constrainedWithin (parent->getLocalBounds()));
}

void TypedField::finish (bool apply, bool keysBack)
{
    if (std::exchange (finished, true))
        return;

    const auto text = getText();

    if (keysBack && owner.isShowing())
        owner.grabKeyboardFocus();
    else if (hasKeyboardFocus (true))
        giveAwayKeyboardFocus();

    setVisible (false);

    if (onFinished != nullptr)
        onFinished();

    // Last, as what it applies may delete its owner and with it this. A value that reads back as another (a note's length
    // that its slider reads as a number) stays as it was.
    if (apply && (shownValue.isEmpty() || text != shownValue))
        if (const auto take = onApply; take != nullptr)
            take (text);
}

bool TypedField::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::escapeKey)
    {
        finish (key == juce::KeyPress::returnKey, true);
        return true;
    }

    return juce::TextEditor::keyPressed (key);
}

void TypedField::focusLost (FocusChangeType cause)
{
    juce::TextEditor::focusLost (cause);
    finish (true, false);
}

void TypedField::componentMovedOrResized (bool, bool)
{
    if (! finished)
        place();
}

void TypedField::componentPeerChanged()
{
    finish (false, false);
}

void TypedField::componentVisibilityChanged()
{
    if (! owner.isShowing())
        finish (false, false);
}
} // namespace phazite::ui
