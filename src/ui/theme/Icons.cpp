// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Icons.h"
#include "LookAndFeel.h"

#include <PhaziteData.h>

namespace phazite::ui
{
namespace
{
    constexpr float iconSize = 16.0f;

    const char* getFileName (Icon icon)
    {
        switch (icon)
        {
            case Icon::play:
                return "play.svg";
            case Icon::stop:
                return "square.svg";
            case Icon::record:
                return "circle.svg";
            case Icon::capture:
                return "clock-arrow-down.svg";
            case Icon::keyboard:
                return "keyboard-music.svg";
            case Icon::search:
                return "search.svg";
            case Icon::detach:
                return "app-window.svg";
            case Icon::plus:
                return "plus.svg";
            case Icon::magnet:
                return "magnet.svg";
            case Icon::loop:
                return "repeat.svg";
            case Icon::metronome:
                return "metronome.svg";
            case Icon::expanded:
                return "chevron-down.svg";
            case Icon::folded:
                return "chevron-right.svg";
            case Icon::collapseAll:
                return "chevrons-down-up.svg";
            case Icon::expandAll:
                return "chevrons-up-down.svg";
            case Icon::smartTool:
                return "mouse-pointer-2.svg";
            case Icon::draw:
                return "pencil.svg";
            case Icon::paint:
                return "paintbrush.svg";
            case Icon::slice:
                return "scissors.svg";
            case Icon::slip:
                return "move-horizontal.svg";
            case Icon::mute:
                return "volume-x.svg";
            case Icon::erase:
                return "eraser.svg";
            case Icon::select:
                return "square-dashed-mouse-pointer.svg";
            case Icon::zoom:
                return "zoom-in.svg";
            case Icon::folder:
                return "folder.svg";
            case Icon::openFolder:
                return "folder-open.svg";
            case Icon::addFolder:
                return "folder-plus.svg";
            case Icon::returnTrack:
                return "corner-down-left.svg";
            case Icon::favorite:
                return "star.svg";
            case Icon::recent:
                return "clock.svg";
            case Icon::sound:
                return "audio-lines.svg";
            case Icon::midi:
                return "music.svg";
            case Icon::instrument:
                return "piano.svg";
            case Icon::effect:
                return "sliders-horizontal.svg";
            case Icon::preview:
                return "headphones.svg";
            case Icon::download:
                return "download.svg";
            case Icon::close:
                return "x.svg";
            case Icon::frozen:
                return "snowflake.svg";
            case Icon::lowLatency:
                return "zap.svg";
            case Icon::maximizePanel:
                return "maximize-2.svg";
            case Icon::restorePanels:
                return "minimize-2.svg";
            case Icon::hidePanelLeft:
                return "panel-left-close.svg";
            case Icon::hidePanelTop:
                return "panel-top-close.svg";
            case Icon::hidePanelBottom:
                return "panel-bottom-close.svg";
            case Icon::hidePanelRight:
                return "panel-right-close.svg";
            case Icon::library:
                return "library.svg";
            case Icon::versions:
                return "git-branch.svg";
            case Icon::account:
                return "circle-user-round.svg";
            case Icon::settings:
                return "settings.svg";
            case Icon::check:
                return "check.svg";
            case Icon::moreActions:
                return "ellipsis.svg";
            case Icon::song:
                return "file-music.svg";
            case Icon::minus:
                return "minus.svg";
            case Icon::menu:
                return "menu.svg";
            case Icon::undo:
                return "undo-2.svg";
            case Icon::redo:
                return "redo-2.svg";
            case Icon::layout:
                return "layout-dashboard.svg";
            case Icon::minimiseWindow:
                return "minus.svg";
            case Icon::maximiseWindow:
                return "square.svg";
            case Icon::restoreWindow:
                return "copy.svg";
        }

        return "";
    }
} // namespace

std::unique_ptr<juce::Drawable> createIcon (Icon icon, bool filled)
{
    const juce::String fileName (getFileName (icon));

    for (int i = 0; i < PhaziteData::namedResourceListSize; ++i)
    {
        if (fileName != PhaziteData::originalFilenames[i])
            continue;

        int size = 0;

        // Lucide draws in "currentColor": read as black, the colour that the icons' tints replace.
        if (const auto* data = PhaziteData::getNamedResource (PhaziteData::namedResourceList[i], size))
        {
            auto text = juce::String::fromUTF8 (data, size).replace ("currentColor", "#000000");

            if (filled)
                text = text.replace ("fill=\"none\"", "fill=\"#000000\"");

            return juce::Drawable::createFromSVGString (text);
        }
    }

    return {};
}

//==============================================================================
TintedIcon::TintedIcon (Icon icon)
    : drawable (createIcon (icon))
{
}

void TintedIcon::draw (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour newColour)
{
    if (drawable == nullptr)
        return;

    if (newColour != colour)
    {
        drawable->replaceColour (colour, newColour);
        colour = newColour;
    }

    drawable->drawWithin (g, area, juce::RectanglePlacement::centred, 1.0f);
}

//==============================================================================
IconButton::IconButton (const juce::String& title, Icon i)
    : juce::Button ({}),
      icon (i),
      drawable (createIcon (i))
{
    setName (title);
    setTitle (title);
}

void IconButton::setIcon (Icon newIcon)
{
    if (icon == newIcon && drawable != nullptr)
        return;

    icon = newIcon;
    drawable = createIcon (newIcon);
    drawableColour = juce::Colours::black;
    repaint();
}

void IconButton::setQuiet (bool shouldBeQuiet)
{
    quiet = shouldBeQuiet;
    repaint();
}

void IconButton::setAccented (bool shouldBeAccented)
{
    accented = shouldBeAccented;
    repaint();
}

void IconButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    const auto& p = getPalette (*this);
    const bool on = getToggleState() || accented;

    if (quiet && ! on)
    {
        // As VS Code's toolbars' buttons: no border, the hover wash under the pointer, the focus outline with the keyboard.
        const auto bounds = getLocalBounds().toFloat();

        if (isEnabled() && (isHighlighted || isDown))
        {
            g.setColour (isDown ? p.selected : p.hover);
            g.fillRoundedRectangle (bounds, cornerRadius);
        }

        if (hasKeyboardFocus (false))
        {
            g.setColour (p.focus);
            g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);
        }
    }
    else
    {
        getLookAndFeel().drawButtonBackground (
            g, *this, findColour (on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId), isHighlighted, isDown);
    }

    const auto colour = (on ? p.textOnAccent : p.text).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f);
    const auto label = getButtonText();
    const auto font = juce::Font (juce::FontOptions (TextSize::body));
    const auto gap = label.isEmpty() ? 0.0f : (float) Space::s;
    const auto textWidth = label.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (font, label);

    // The icon and the text, centered together. A button too small for the usual margins gets smaller ones.
    const auto margin = juce::jlimit (0.0f, (float) Space::s, ((float) getWidth() - iconSize) / 2.0f);
    const auto area = getLocalBounds().toFloat().reduced (margin, 0.0f);
    auto content = area.withSizeKeepingCentre (juce::jmin (area.getWidth(), iconSize + gap + textWidth), area.getHeight());

    if (drawable != nullptr)
    {
        if (drawableColour != colour)
        {
            drawable->replaceColour (drawableColour, colour);
            drawableColour = colour;
        }

        drawable->drawWithin (g, content.removeFromLeft (iconSize).withSizeKeepingCentre (iconSize, iconSize),
                              juce::RectanglePlacement::centred, 1.0f);
    }

    if (label.isNotEmpty())
    {
        content.removeFromLeft (gap);
        g.setColour (colour);
        g.setFont (font);
        g.drawText (label, content, juce::Justification::centredLeft, true);
    }
}
} // namespace phazite::ui
