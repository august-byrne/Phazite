// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FilterBar.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>
#include <utility>

namespace phazite::ui
{
namespace
{
    using Filter = FilterBar::Filter;
    constexpr std::array<Filter, 4> allFilters { Filter::type, Filter::loops, Filter::tempo, Filter::key };

    juce::String getFilterName (Filter filter)
    {
        switch (filter)
        {
            case Filter::type:
                return "Type";
            case Filter::loops:
                return "Loop";
            case Filter::tempo:
                return "Tempo";
            case Filter::key:
                return "Key";
        }

        return {};
    }

    juce::String getTooltipFor (Filter filter)
    {
        switch (filter)
        {
            case Filter::type:
                return "Only sounds of a type: kicks, snares, bass..., as the words in their names and folders say";
            case Filter::loops:
                return "Only loops, or only one-shots";
            case Filter::tempo:
                return "Only sounds at a tempo, as their names or loop data say";
            case Filter::key:
                return "Only sounds in a key, or tuned to a note, as their names or loop data say";
        }

        return {};
    }

    // What a filter is set to, as its chip says it; nothing while it isn't set.
    juce::String getValueText (const BrowserFilters& filters, Filter filter)
    {
        switch (filter)
        {
            case Filter::type:
                return filters.type;
            case Filter::loops:
                return filters.loops.has_value() ? juce::String (*filters.loops ? "Loops" : "One-shots") : juce::String();
            case Filter::tempo:
                return filters.tempo.has_value() ? juce::String (*filters.tempo) + " BPM" : juce::String();
            case Filter::key:
                return filters.key.has_value() ? filters.key->describe() : juce::String();
        }

        return {};
    }

    BrowserFilters without (BrowserFilters filters, Filter filter)
    {
        switch (filter)
        {
            case Filter::type:
                filters.type.clear();
                break;
            case Filter::loops:
                filters.loops.reset();
                break;
            case Filter::tempo:
                filters.tempo.reset();
                break;
            case Filter::key:
                filters.key.reset();
                break;
        }

        return filters;
    }
} // namespace

//==============================================================================
// A filter's chip: its name while it isn't set, its value and an x while it is (no chevron, so that the four fit the side
// panel's width in a row: the pill and its tooltip say it opens a menu). A click on the x takes the value away; anywhere
// else, Space or Enter open its menu; Delete or Backspace take its value away.
class FilterBar::Chip : public juce::Button
{
public:
    Chip (FilterBar& b, Filter f)
        : juce::Button (getFilterName (f)),
          bar (b),
          filter (f)
    {
        setTooltip (getTooltipFor (filter));
        setWantsKeyboardFocus (true);
        refresh();
    }

    juce::String getValue() const { return getValueText (bar.filters, filter); }
    bool isSet() const { return getValue().isNotEmpty(); }
    juce::String getText() const { return isSet() ? getValue() : getFilterName (filter); }

    void refresh()
    {
        setTitle (isSet() ? getFilterName (filter) + " filter: " + getValue() : getFilterName (filter) + " filter");
        repaint();
    }

    // Its text whole, rounded up and a pixel to spare (a pixel short, it would end in an ellipsis), and its x while it is set.
    int getPreferredWidth() const
    {
        const juce::Font font { juce::FontOptions (TextSize::small) };
        const auto textWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, getText())) + 1;
        return 2 * FilterChipTint::textInset + textWidth + (isSet() ? FilterChipTint::gap + FilterChipTint::iconSize : 0);
    }

    // What a click takes the value away on, while it is set: its x, and the inset after it.
    juce::Rectangle<int> getClearArea() const
    {
        return getLocalBounds().removeFromRight (FilterChipTint::iconSize + FilterChipTint::textInset + FilterChipTint::gap / 2);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto& p = getPalette (*this);
        const auto area = getLocalBounds().toFloat().reduced (0.5f);
        const auto set = isSet();
        const auto corner = std::min (FilterChipTint::corner, area.getHeight() / 2.0f);

        if (set)
        {
            g.setColour (p.accent.withAlpha (FilterChipTint::setFill));
            g.fillRoundedRectangle (area, corner);
        }

        if (highlighted || down)
        {
            g.setColour (p.hover);
            g.fillRoundedRectangle (area, corner);
        }

        g.setColour (hasKeyboardFocus (false) ? p.focus : set ? p.accent : p.controlBorder);
        g.drawRoundedRectangle (area, corner, 1.0f);

        auto inside = getLocalBounds().reduced (FilterChipTint::textInset, 0);
        const auto ink = set ? p.textBright : p.text;

        if (set)
        {
            const auto iconArea = inside.removeFromRight (FilterChipTint::iconSize).toFloat();
            inside.removeFromRight (FilterChipTint::gap);
            cross.draw (g, iconArea.withSizeKeepingCentre ((float) FilterChipTint::iconSize, (float) FilterChipTint::iconSize), ink);
        }

        g.setColour (ink);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (getText(), inside, juce::Justification::centredLeft, true);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressedOnClear = isSet() && getClearArea().contains (e.getPosition());
        juce::Button::mouseDown (e);
    }

    using juce::Button::clicked; // the overload with the modifiers, which calls this one

    void clicked() override
    {
        if (std::exchange (pressedOnClear, false))
            bar.change (without (bar.filters, filter));
        else
            bar.showMenu (filter);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (isSet() && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
        {
            bar.change (without (bar.filters, filter));
            return true;
        }

        return juce::Button::keyPressed (key);
    }

private:
    FilterBar& bar;
    const Filter filter;
    bool pressedOnClear = false;
    TintedIcon cross { Icon::close };
};

//==============================================================================
FilterBar::FilterBar()
{
    setTitle ("Filters");

    for (const auto filter : allFilters)
    {
        auto& chip = chips[(size_t) filter];
        chip = std::make_unique<Chip> (*this, filter);
        addAndMakeVisible (*chip);
    }

    clearButton.setTitle ("Clear the filters and the search");
    clearButton.setTooltip ("Takes every filter away, and the search's words");
    clearButton.onClick = [this]
    {
        if (onClear != nullptr)
            onClear();
    };
    addChildComponent (clearButton);
}

FilterBar::~FilterBar() = default;

void FilterBar::show (const BrowserFilters& shown, bool hasWords)
{
    filters = shown;
    searchHasWords = hasWords;

    for (auto& chip : chips)
        chip->refresh();

    clearButton.setVisible (! filters.isEmpty() || searchHasWords);
    resized();
}

juce::Button& FilterBar::getChip (Filter filter)
{
    return *chips[(size_t) filter];
}

juce::Rectangle<int> FilterBar::getClearArea (Filter filter)
{
    return chips[(size_t) filter]->getClearArea();
}

void FilterBar::change (const BrowserFilters& changed)
{
    if (changed == filters)
        return;

    show (changed, searchHasWords);

    if (onChange != nullptr)
        onChange (filters);
}

juce::PopupMenu FilterBar::getMenu (Filter filter)
{
    juce::PopupMenu menu;
    const auto options = getOptions != nullptr ? getOptions() : FilterOptions();
    const juce::Component::SafePointer<FilterBar> safeThis (this);
    auto setIsOffered = false;

    // Each value, with how many have it; the one set ticked, which chosen again takes it away.
    const auto add = [&] (const juce::String& text, int count, bool isSet, BrowserFilters chosen)
    {
        juce::PopupMenu::Item item (text);
        item.isTicked = isSet;
        item.shortcutKeyDescription = juce::String (count);
        item.action = [safeThis, next = isSet ? without (filters, filter) : chosen]
        {
            if (safeThis != nullptr)
                safeThis->change (next);
        };
        menu.addItem (std::move (item));
        setIsOffered = setIsOffered || isSet;
    };

    switch (filter)
    {
        case Filter::type:
            for (const auto& [type, count] : options.types)
            {
                auto chosen = filters;
                chosen.type = type;
                add (type, count, type == filters.type, chosen);
            }

            break;
        case Filter::loops:
            for (const auto loops : { true, false })
            {
                if (const auto count = loops ? options.loops : options.oneShots; count > 0)
                {
                    auto chosen = filters;
                    chosen.loops = loops;
                    add (loops ? "Loops" : "One-shots", count, filters.loops == loops, chosen);
                }
            }

            break;
        case Filter::tempo:
            for (const auto& [tempo, count] : options.tempos)
            {
                auto chosen = filters;
                chosen.tempo = tempo;
                add (juce::String (tempo) + " BPM", count, filters.tempo == tempo, chosen);
            }

            break;
        case Filter::key:
            for (const auto& [key, count] : options.keys)
            {
                auto chosen = filters;
                chosen.key = key;
                add (key.describe(), count, filters.key == key, chosen);
            }

            break;
    }

    // The value set, when what shows now has none of it (the search's words changed since), to take away.
    if (const auto value = getValueText (filters, filter); value.isNotEmpty() && ! setIsOffered)
        add (value, 0, true, without (filters, filter));

    if (menu.getNumItems() == 0)
        menu.addItem (juce::PopupMenu::Item ("None among what shows").setEnabled (false));

    return menu;
}

void FilterBar::showMenu (Filter filter)
{
    getMenu (filter).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (chips[(size_t) filter].get()));
}

std::vector<juce::Rectangle<int>> FilterBar::layOut (int width) const
{
    std::vector<int> widths;

    for (const auto& chip : chips)
        widths.push_back (chip->getPreferredWidth());

    // Clear, in the font text buttons have, with room at its ends as theirs.
    if (clearButton.isVisible())
    {
        const juce::Font font { juce::FontOptions (TextSize::body) };
        widths.push_back (2 * Space::s + (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, clearButton.getButtonText())) + 1);
    }

    // In rows, as many in each as fit.
    std::vector<juce::Rectangle<int>> bounds;
    auto x = 0, y = 0;

    for (const auto preferred : widths)
    {
        const auto w = std::min (preferred, std::max (1, width));

        if (x > 0 && x + w > width)
        {
            x = 0;
            y += FilterChipTint::height + FilterChipTint::gap;
        }

        bounds.emplace_back (x, y, w, FilterChipTint::height);
        x += w + FilterChipTint::gap;
    }

    return bounds;
}

int FilterBar::getHeightForWidth (int width) const
{
    auto bottom = 0;

    for (const auto& area : layOut (width))
        bottom = std::max (bottom, area.getBottom());

    return bottom;
}

void FilterBar::resized()
{
    const auto bounds = layOut (getWidth());

    for (size_t i = 0; i < chips.size(); ++i)
        chips[i]->setBounds (bounds[i]);

    if (clearButton.isVisible())
        clearButton.setBounds (bounds.back());
}
} // namespace phazite::ui
