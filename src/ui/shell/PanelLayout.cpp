// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PanelLayout.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr std::array<Panel, 4> allPanels { Panel::browser, Panel::song, Panel::editor, Panel::inspector };

    Icon getHideIcon (Panel panel)
    {
        switch (panel)
        {
            case Panel::browser:
                return Icon::hidePanelLeft;
            case Panel::song:
                return Icon::hidePanelTop;
            case Panel::editor:
                return Icon::hidePanelBottom;
            case Panel::inspector:
            default:
                return Icon::hidePanelRight;
        }
    }
} // namespace

juce::String getPanelKey (Panel panel)
{
    switch (panel)
    {
        case Panel::browser:
            return "browser";
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
        case Panel::browser:
            return "Browser";
        case Panel::song:
            return "Playlist and Mixer";
        case Panel::editor:
            return "Editor";
        case Panel::inspector:
        default:
            return "Inspector";
    }
}

juce::String describePanel (Panel panel)
{
    switch (panel)
    {
        case Panel::browser:
            return "the browser";
        case Panel::song:
            return "the playlist and mixer";
        case Panel::editor:
            return "the editor";
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
        widths.browser = takesRoom (Panel::browser) ? width : 0;
        widths.inspector = takesRoom (Panel::inspector) ? width : 0;
        return widths;
    }

    widths.browser = takesRoom (Panel::browser) ? browserWidth : 0;
    widths.inspector = takesRoom (Panel::inspector) ? inspectorWidth : 0;

    // Each gives way in proportion to what it has above its least.
    const auto dividers = (widths.browser > 0 ? Shell::dividerSize : 0) + (widths.inspector > 0 ? Shell::dividerSize : 0);
    const auto over = widths.browser + widths.inspector + dividers + Shell::minimumCentreWidth - width;
    const auto spareBrowser = widths.browser > 0 ? juce::jmax (0, widths.browser - Shell::minimumBrowserWidth) : 0;
    const auto spareInspector = widths.inspector > 0 ? juce::jmax (0, widths.inspector - Shell::minimumInspectorWidth) : 0;

    if (over > 0 && spareBrowser + spareInspector > 0)
    {
        const auto fromBrowser =
            juce::jmin (spareBrowser, juce::roundToInt (over * spareBrowser / (double) (spareBrowser + spareInspector)));
        widths.browser -= fromBrowser;
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

    text << " browser=" << browserWidth << " inspector=" << inspectorWidth << " editor=" << juce::String (editorShare, 3);
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
        else if (key == "browser" && value.getIntValue() > 0)
        {
            browserWidth = juce::jlimit (Shell::minimumBrowserWidth, Shell::maximumBrowserWidth, value.getIntValue());
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
      hideButton ("Hide", getHideIcon (shownPanel))
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

    detachButton.setVisible (panel != Panel::song);
    refreshTooltips();
}

juce::Array<IconButton*> PanelButtons::getButtons()
{
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
    hideButton.setTooltip (
        withShortcut (commands, "Hides " + name + ". The View menu shows it again", getPanelCommandID ("toggle", panel)));
}

int PanelButtons::getPreferredWidth() const noexcept
{
    return Shell::getPanelCornerWidth (panel == Panel::song ? 2 : 3) - Space::xs / 2; // its margin from the panel's edge aside
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
    // The shell's border colour shows through, until the pointer is on it.
    if (isMouseOverOrDragging())
        g.fillAll (getPalette (*this).accent.withAlpha (Shell::dividerLit));
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
} // namespace phazite::ui
