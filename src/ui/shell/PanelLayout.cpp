// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PanelLayout.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr std::array<Panel, 4> allPanels { Panel::left, Panel::song, Panel::editor, Panel::inspector };
} // namespace

juce::String getLeftViewKey (LeftView view)
{
    switch (view)
    {
        case LeftView::project:
            return "project";
        case LeftView::versions:
            return "versions";
        case LeftView::browser:
        default:
            return "browser";
    }
}

std::optional<LeftView> findLeftViewByKey (const juce::String& key)
{
    for (const auto view : { LeftView::browser, LeftView::project, LeftView::versions })
        if (getLeftViewKey (view) == key)
            return view;

    return std::nullopt;
}

juce::String getPanelKey (Panel panel)
{
    switch (panel)
    {
        case Panel::left:
            return "leftPanel";
        case Panel::song:
            return "song";
        case Panel::editor:
            return "editor";
        case Panel::inspector:
        default:
            return "inspector";
    }
}

std::optional<Panel> findPanelByKey (const juce::String& key)
{
    for (const auto panel : allPanels)
        if (getPanelKey (panel) == key)
            return panel;

    return std::nullopt;
}

juce::String getPanelCommandID (const char* verb, Panel panel)
{
    const auto key = getPanelKey (panel);
    return "view." + juce::String (verb) + key.substring (0, 1).toUpperCase() + key.substring (1);
}

juce::String getPanelName (Panel panel)
{
    switch (panel)
    {
        case Panel::left:
            return "Left Panel";
        case Panel::song:
            return "Upper Panel"; // by default the playlist and the mixer (Panels.h: ViewPlaces)
        case Panel::editor:
            return "Lower Panel"; // by default the selected clip's editor and the selected track's devices
        case Panel::inspector:
        default:
            return "Inspector";
    }
}

juce::String describePanel (Panel panel)
{
    switch (panel)
    {
        case Panel::left:
            return "the left panel";
        case Panel::song:
            return "the upper panel";
        case Panel::editor:
            return "the lower panel";
        case Panel::inspector:
        default:
            return "the inspector";
    }
}

bool PanelLayout::isShown (Panel panel) const noexcept
{
    return shown[indexOf (panel)];
}

void PanelLayout::show (Panel panel)
{
    // Seeing another panel brings back the others too.
    if (maximized.has_value() && *maximized != panel)
        restore();

    shown[indexOf (panel)] = true;
}

bool PanelLayout::canHide (Panel panel) const noexcept
{
    return panel != Panel::song || ! isDetached (Panel::editor);
}

void PanelLayout::hide (Panel panel)
{
    if (! canHide (panel))
        return;

    if (isMaximized (panel))
        restore();

    shown[indexOf (panel)] = false;

    // The centre shows one of them at least.
    if (panel == Panel::song)
        shown[indexOf (Panel::editor)] = true;
    else if (panel == Panel::editor && ! isShown (Panel::song))
        shown[indexOf (Panel::song)] = true;
}

void PanelLayout::toggle (Panel panel)
{
    // One shown but out of sight behind a maximized panel comes back with the others.
    if (takesRoom (panel))
        hide (panel);
    else
        show (panel);
}

void PanelLayout::maximize (Panel panel)
{
    if (! canMaximize (panel))
        return;

    shown[indexOf (panel)] = true;
    maximized = panel;
}

void PanelLayout::toggleMaximized (Panel panel)
{
    if (isMaximized (panel))
        restore();
    else
        maximize (panel);
}

void PanelLayout::setDetached (Panel panel, bool isAway)
{
    if (panel == Panel::song)
        return;

    detached[indexOf (panel)] = isAway;

    // It shows in its window, and so here when it comes back; one that filled the window gives the room back.
    if (isAway)
        shown[indexOf (panel)] = true;

    if (isAway && isMaximized (panel))
        restore();

    // The centre keeps the song's views while the editor dock is away.
    if (isAway && panel == Panel::editor)
        shown[indexOf (Panel::song)] = true;
}

bool PanelLayout::isDetached (Panel panel) const noexcept
{
    return detached[indexOf (panel)];
}

bool PanelLayout::takesRoom (Panel panel) const noexcept
{
    return isShown (panel) && ! isDetached (panel) && (! maximized.has_value() || *maximized == panel);
}

PanelLayout::SideWidths PanelLayout::getSideWidths (int width) const
{
    SideWidths widths;
    const auto centreShown = takesRoom (Panel::song) || takesRoom (Panel::editor);

    if (! centreShown) // a side panel maximized
    {
        widths.left = takesRoom (Panel::left) ? width : 0;
        widths.inspector = takesRoom (Panel::inspector) ? width : 0;
        return widths;
    }

    widths.left = takesRoom (Panel::left) ? leftWidth : 0;
    widths.inspector = takesRoom (Panel::inspector) ? inspectorWidth : 0;

    // Each gives way in proportion to what it has above its least.
    const auto dividers = (widths.left > 0 ? Shell::dividerSize : 0) + (widths.inspector > 0 ? Shell::dividerSize : 0);
    const auto over = widths.left + widths.inspector + dividers + Shell::minimumCentreWidth - width;
    const auto spareBrowser = widths.left > 0 ? juce::jmax (0, widths.left - Shell::minimumLeftPanelWidth) : 0;
    const auto spareInspector = widths.inspector > 0 ? juce::jmax (0, widths.inspector - Shell::minimumInspectorWidth) : 0;

    if (over > 0 && spareBrowser + spareInspector > 0)
    {
        const auto fromBrowser =
            juce::jmin (spareBrowser, juce::roundToInt (over * spareBrowser / (double) (spareBrowser + spareInspector)));
        widths.left -= fromBrowser;
        widths.inspector -= juce::jmin (spareInspector, over - fromBrowser);
    }

    return widths;
}

int PanelLayout::getEditorHeight (int centreHeight) const
{
    const auto most = juce::jmax (Shell::minimumCentreHeight, centreHeight - Shell::dividerSize - Shell::minimumCentreHeight);
    return juce::jlimit (Shell::minimumCentreHeight, most, juce::roundToInt (centreHeight * editorShare));
}

juce::String PanelLayout::getToggleName (Panel panel) const
{
    return (takesRoom (panel) ? "Hide the " : "Show the ") + getPanelName (panel);
}

juce::String PanelLayout::getMaximizeName (Panel panel) const
{
    return isMaximized (panel) ? juce::String ("Restore the Panels") : juce::String();
}

juce::String PanelLayout::getMaximizeInUseName() const
{
    return maximized.has_value() ? juce::String ("Restore the Panels") : juce::String();
}

juce::String PanelLayout::toText() const
{
    juce::StringArray shownKeys;

    for (const auto panel : allPanels)
        if (isShown (panel))
            shownKeys.add (getPanelKey (panel));

    juce::String text ("shown=" + shownKeys.joinIntoString (","));

    if (maximized.has_value())
        text << " maximized=" << getPanelKey (*maximized);

    text << " left=" << leftWidth << " leftView=" << getLeftViewKey (leftView) << " inspector=" << inspectorWidth
         << " editor=" << juce::String (editorShare, 3) << " upperSplit=" << juce::String (rowShares[0], 3)
         << " lowerSplit=" << juce::String (rowShares[1], 3);
    return text;
}

void PanelLayout::setFromText (const juce::String& text)
{
    for (const auto& item : juce::StringArray::fromTokens (text, " ", ""))
    {
        const auto key = item.upToFirstOccurrenceOf ("=", false, false);
        const auto value = item.fromFirstOccurrenceOf ("=", false, false);

        if (key == "shown")
        {
            std::array<bool, 4> read {};

            for (const auto& name : juce::StringArray::fromTokens (value, ",", ""))
                if (const auto panel = findPanelByKey (name))
                    read[indexOf (*panel)] = true;

            // The centre shows one of them at least.
            if (read[indexOf (Panel::song)] || read[indexOf (Panel::editor)])
                shown = read;
        }
        else if (key == "maximized")
        {
            if (const auto panel = findPanelByKey (value))
                maximized = panel;
        }
        else if (key == "left" && value.getIntValue() > 0)
        {
            leftWidth = juce::jlimit (Shell::minimumLeftPanelWidth, Shell::maximumLeftPanelWidth, value.getIntValue());
        }
        else if (key == "leftView")
        {
            leftView = findLeftViewByKey (value).value_or (leftView);
        }
        else if (key == "inspector" && value.getIntValue() > 0)
        {
            inspectorWidth = juce::jlimit (Shell::minimumInspectorWidth, Shell::maximumInspectorWidth, value.getIntValue());
        }
        else if (key == "editor")
        {
            if (const auto share = value.getDoubleValue(); share >= Shell::minimumEditorShare && share <= Shell::maximumEditorShare)
                editorShare = share;
        }
        else if (key == "upperSplit" || key == "lowerSplit")
        {
            if (const auto share = value.getDoubleValue(); share > 0.0 && share < 1.0)
                rowShares[key == "upperSplit" ? 0 : 1] = share;
        }
    }

    // One maximized shows.
    if (maximized.has_value())
        shown[indexOf (*maximized)] = true;
}

//==============================================================================
PanelButtons::PanelButtons (CommandBridge& bridge, Panel shownPanel)
    : commands (bridge),
      panel (shownPanel),
      maximizeButton ("Maximize", Icon::maximizePanel),
      detachButton ("Its own window", Icon::detach),
      hideButton ("Hide", Icon::close) // VS Code's X, which hides its panel
{
    setTitle ("Panel buttons");
    maximizeButton.onClick = [this] { commands.invoke (getPanelCommandID ("maximize", panel)); };
    detachButton.onClick = [this] { commands.invoke (getPanelCommandID ("detach", panel)); };
    hideButton.onClick = [this] { commands.invoke (getPanelCommandID ("toggle", panel)); };

    for (auto* button : getButtons())
    {
        button->setQuiet (true);
        button->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (button);
    }

    refreshTooltips();
}

juce::Array<IconButton*> PanelButtons::getButtons()
{
    if (panel != Panel::editor)
        return { &hideButton }; // the inspector's X

    return { &maximizeButton, &detachButton, &hideButton };
}

void PanelButtons::update (const PanelLayout& layout)
{
    hideButton.setEnabled (layout.canToggle (panel)); // the playlist stays while the editor dock is in its own window

    if (layout.isMaximized (panel) == maximized)
        return;

    maximized = layout.isMaximized (panel);
    maximizeButton.setIcon (maximized ? Icon::restorePanels : Icon::maximizePanel);
    maximizeButton.setTitle (maximized ? "Restore the panels" : "Maximize");
    refreshTooltips();
}

void PanelButtons::refreshTooltips()
{
    const auto name = describePanel (panel);
    maximizeButton.setTooltip (withShortcut (commands,
                                             maximized ? juce::String ("Restore the panels: the others come back as they were")
                                                       : "Maximize " + name
                                                             + ": it fills the window, and the other panels hide until you "
                                                               "restore them",
                                             getPanelCommandID ("maximize", panel)));
    detachButton.setTooltip (withShortcut (commands,
                                           "Moves " + name
                                               + " to a window of its own, for example on a second monitor. Close that "
                                                 "window to put it back",
                                           getPanelCommandID ("detach", panel)));
    hideButton.setTooltip (withShortcut (commands, "Hides " + name + ". Its switch at the top of the window shows it again",
                                         getPanelCommandID ("toggle", panel)));
}

int PanelButtons::getPreferredWidth() const noexcept
{
    const auto buttons = panel == Panel::editor ? Shell::panelCornerButtons : 1;
    return Shell::getPanelCornerWidth (buttons) - Space::xs / 2; // its margin from the panel's edge aside
}

void PanelButtons::resized()
{
    auto area = getLocalBounds();

    for (auto* button : getButtons())
    {
        if (! button->isVisible())
            continue;

        button->setBounds (area.removeFromLeft (Shell::panelButtonSize));
        area.removeFromLeft (Space::xs / 2);
    }
}

//==============================================================================
PanelDivider::PanelDivider (const juce::String& title, bool isBetweenColumns)
    : betweenColumns (isBetweenColumns)
{
    setTitle (title);
    setRepaintsOnMouseActivity (true);
    setMouseCursor (betweenColumns ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
}

void PanelDivider::paint (juce::Graphics& g)
{
    // The window's frame shows through, between the cards, until the pointer is on it: then a line in the focus colour, as
    // VS Code's sashes light.
    if (isMouseOverOrDragging())
        g.fillAll (getPalette (*this).focus.withAlpha (Shell::dividerLit));
}

void PanelDivider::mouseDown (const juce::MouseEvent&)
{
    if (onDragStart != nullptr)
        onDragStart();
}

void PanelDivider::mouseDrag (const juce::MouseEvent& e)
{
    if (onDrag != nullptr)
        onDrag (betweenColumns ? e.getScreenX() - e.getMouseDownScreenX() : e.getScreenY() - e.getMouseDownScreenY()); // it moves meanwhile
}

void PanelDivider::mouseUp (const juce::MouseEvent&)
{
    repaint();

    if (onDragEnd != nullptr)
        onDragEnd();
}

void PanelDivider::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick != nullptr)
        onDoubleClick();
}

//==============================================================================
PanelCards::PanelCards()
{
    setInterceptsMouseClicks (false, false);
    setAccessible (false); // what it draws is the panels' edges
}

void PanelCards::setCards (const juce::Array<juce::Rectangle<int>>& newCards)
{
    if (newCards == cards)
        return;

    cards = newCards;
    corners.clear();
    edges.clear();

    for (const auto& card : cards)
    {
        if (card.isEmpty())
            continue;

        // The corners: what lies between the card's rectangle and its rounded shape.
        const auto area = card.toFloat();
        corners.addRectangle (area);
        corners.addRoundedRectangle (area, cardCornerRadius);

        juce::Path edge;
        edge.addRoundedRectangle (area.reduced (0.5f), cardCornerRadius);
        juce::PathStrokeType (1.0f).createStrokedPath (edge, edge);
        edges.addPath (edge);
    }

    corners.setUsingNonZeroWinding (false);
    repaint();
}

void PanelCards::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.setColour (p.surface);
    g.fillPath (corners);
    g.setColour (p.border);
    g.fillPath (edges);
}
} // namespace phazite::ui
