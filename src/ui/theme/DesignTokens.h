// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's design tokens: the named colours, text sizes and spacings that every component uses
// (docs/design.md, "Design language"). Change the look here rather than in components.
//
// The dark and light themes are VS Code's 2026 Dark and 2026 Light, and the sizes those of its Modern UI (docs/vscode-look.md,
// where each value comes from): VS Code's source is under the MIT licence, whose notice ships next to the app
// (licenses/VSCode.txt). The high contrast theme is Phazite's own.
#pragma once

#include <juce_graphics/juce_graphics.h>

#include <optional>

namespace phazite::ui
{
enum class ThemeName
{
    dark,
    light,
    highContrast
};

// Each with the colour of VS Code's it is (in the dark theme).
struct Palette
{
    juce::Colour background;       // the song's views, as VS Code's editor: the playlist, the piano roll, the mixer (editor)
    juce::Colour surface;          // the window's frame and the side panels, the browser and the inspector (sideBar, titleBar)
    juce::Colour surfaceRaised;    // what pops up, fields and rulers (menu, editorWidget)
    juce::Colour border;           // the panels' edges and the lines between parts, decorative (surface.border, widget.border)
    juce::Colour controlBorder;    // around a field, a list to pick from and a button that isn't filled (input.border)
    juce::Colour text;             // (foreground)
    juce::Colour textMuted;        // secondary text, still readable (descriptionForeground)
    juce::Colour textBright;       // on a selected row, and the activity bar's view that shows (list.activeSelectionForeground)
    juce::Colour accent;           // the main button and what is switched on (button.background)
    juce::Colour textOnAccent;     // (button.foreground)
    juce::Colour focus;            // the keyboard focus's outline, and a menu's highlighted item (focusBorder, menu.selectionBorder)
    juce::Colour hover;            // a wash under the pointer, translucent (list.hoverBackground)
    juce::Colour selected;         // a wash behind what is selected, translucent (list.activeSelectionBackground)
    juce::Colour selectedInactive; // the tab that shows, and the activity bar's view (list.inactiveSelectionBackground)
    juce::Colour scrollThumb;      // a scroll bar's thumb, translucent; lighter under the pointer (scrollbarSlider.background)
    juce::Colour indentGuide;      // a tree's line down each level, under the selection's (tree.indentGuidesStroke)
    juce::Colour warning;          // (list.warningForeground)
    juce::Colour danger;           // (errorForeground)
    juce::Colour loop;             // where playback loops, in the rulers
    juce::Colour modulation;       // what modulation does to a control: the ring around a knob, a modulator's output
    juce::Colour frozen;           // a frozen track's frost, over its clips and its snowflake (FrozenTint)
    juce::Colour boost;            // a level brought up, as upward compression does (danger: one turned down), in devices' graphs
    juce::Colour playing;          // a track's switch while the track plays, lit as FL Studio's mute switch is (TrackSwitch.h)
    juce::Colour soloed;           // a soloed track's switch, in the yellow most DAWs light solo with
};

inline Palette getPalette (ThemeName theme)
{
    using juce::Colour;

    switch (theme)
    {
        case ThemeName::light:
            return { .background = Colour (0xffffffff),
                     .surface = Colour (0xfffafafd),
                     .surfaceRaised = Colour (0xfff0f0f3),
                     .border = Colour (0xffe4e5e6),
                     .controlBorder = Colour (0xffd8d8d8),
                     .text = Colour (0xff202020),
                     .textMuted = Colour (0xff606060),
                     .textBright = Colour (0xff202020),
                     .accent = Colour (0xff0069cc),
                     .textOnAccent = Colour (0xffffffff),
                     .focus = Colour (0xff0069cc),
                     .hover = Colour (0x14000000),
                     .selected = Colour (0x25000000),
                     .selectedInactive = Colour (0xffe7e7e8),
                     .scrollThumb = Colour (0xc0646464),
                     .indentGuide = Colour (0xffa9a9a9),
                     .warning = Colour (0xff895503),
                     .danger = Colour (0xffad0707),
                     .loop = Colour (0xff1a7f5a),
                     .modulation = Colour (0xff7b3fd4),
                     .frozen = Colour (0xff2b8fd6),
                     .boost = Colour (0xff1e7d3c),
                     .playing = Colour (0xff1f8a43),
                     .soloed = Colour (0xffb07d00) };

        case ThemeName::highContrast:
            return { .background = Colour (0xff000000),
                     .surface = Colour (0xff000000),
                     .surfaceRaised = Colour (0xff1a1a1a),
                     .border = Colour (0xffffffff),
                     .controlBorder = Colour (0xffffffff),
                     .text = Colour (0xffffffff),
                     .textMuted = Colour (0xffe6e6e6),
                     .textBright = Colour (0xffffffff),
                     .accent = Colour (0xffffd400),
                     .textOnAccent = Colour (0xff000000),
                     .focus = Colour (0xffffd400),
                     .hover = Colour (0x33ffffff),
                     .selected = Colour (0x4dffffff),
                     .selectedInactive = Colour (0xff333333),
                     .scrollThumb = Colour (0x99ffffff),
                     .indentGuide = Colour (0xffe6e6e6),
                     .warning = Colour (0xffffb000),
                     .danger = Colour (0xffff6b6b),
                     .loop = Colour (0xff00ff9c),
                     .modulation = Colour (0xffff5cff),
                     .frozen = Colour (0xff00e5ff),
                     .boost = Colour (0xff3dff7a),
                     .playing = Colour (0xff3dff7a),
                     .soloed = Colour (0xffffe14d) };

        case ThemeName::dark:
        default:
            return { .background = Colour (0xff121314),
                     .surface = Colour (0xff191a1b),
                     .surfaceRaised = Colour (0xff202122),
                     .border = Colour (0xff2a2b2c),
                     .controlBorder = Colour (0xff333536),
                     .text = Colour (0xffbfbfbf),
                     .textMuted = Colour (0xff8c8c8c),
                     .textBright = Colour (0xffededed),
                     .accent = Colour (0xff297aa0),
                     .textOnAccent = Colour (0xffffffff),
                     .focus = Colour (0xff3994bc),
                     .hover = Colour (0x14ffffff),
                     .selected = Colour (0x22ffffff),
                     .selectedInactive = Colour (0xff2c2d2e),
                     .scrollThumb = Colour (0x85a8a9aa),
                     .indentGuide = Colour (0xff585858),
                     .warning = Colour (0xffe5ba7d),
                     .danger = Colour (0xfff48771),
                     .loop = Colour (0xff3fbf8f),
                     .modulation = Colour (0xffb48cff),
                     .frozen = Colour (0xffa8dcff),
                     .boost = Colour (0xff6fdc8c),
                     .playing = Colour (0xff4fd47c),
                     .soloed = Colour (0xfff7c948) };
    }
}

// Text sizes at 100 % scale (JUCE font heights, in pixels), as VS Code's: its body text (fontSize.body1), its labels, menus and
// status bar (fontSize.label1) and its badges' counts (fontSize.label3). Two weights, regular and semibold (Inter's
// SemiBold, what a bold font is), as VS Code's.
namespace TextSize
{
    inline constexpr float small = 12.0f;
    inline constexpr float body = 13.0f;
    inline constexpr float title = 16.0f;
    inline constexpr float heading = 20.0f;
    inline constexpr float badge = 10.0f; // a count in a badge, such as the activity bar's
} // namespace TextSize

// Spacing, on a 4-pixel grid.
namespace Space
{
    inline constexpr int xs = 4;
    inline constexpr int s = 8;
    inline constexpr int m = 12;
    inline constexpr int l = 16;
    inline constexpr int xl = 24;
} // namespace Space

inline constexpr int controlHeight = 28;

// Corners, as VS Code's: a control's, a button's, a field's, a row's or a tab's (cornerRadius.small); a menu's items
// (cornerRadius.medium); and the panels' cards and what pops up, menus, lists and tooltips (cornerRadius.large).
inline constexpr float cornerRadius = 4.0f;
inline constexpr float itemCornerRadius = 6.0f;
inline constexpr float cardCornerRadius = 8.0f;

// A scroll bar, as VS Code's Modern UI's: so wide, its thumb rounded and no track behind it; the thumb's colour
// (Palette::scrollThumb) this much more opaque under the pointer and while it is dragged.
namespace ScrollTint
{
    inline constexpr int width = 8;
    inline constexpr float hoverAlpha = 1.08f, dragAlpha = 1.17f;

    // One over what it scrolls, as the playlist's rows' is (as FL Studio's): thinner, its thumb this much fainter, over a
    // track in the text colour this opaque.
    inline constexpr int overlayWidth = 6;
    inline constexpr float overlayThumb = 0.6f, overlayTrack = 0.06f;
} // namespace ScrollTint

// The playlist's overview of the whole song (ui/playlist/PlaylistParts.h: Playlist::Overview): its clips this opaque, muted
// ones this, under the see-through thumb of the part in view, in the text colour this opaque, brighter under the pointer, and
// its edge.
namespace OverviewTint
{
    inline constexpr float clip = 0.85f, muted = 0.3f;
    inline constexpr float thumb = 0.12f, thumbLit = 0.2f, thumbEdge = 0.3f;
} // namespace OverviewTint

// A list's rows, as VS Code's: so tall, and their hover and selection washes inset this far from the list's sides, rounded
// (cornerRadius).
namespace ListRow
{
    inline constexpr int height = 22;
    inline constexpr float inset = 4.0f;
    inline constexpr float inactiveGuide = 0.4f; // a tree's other indent guides, shown while the pointer is over it (an alpha)
} // namespace ListRow

// The views of a side panel (the browser, Source Control), as VS Code's Modern UI lays out its side bar's views (docs/
// vscode-look.md): the title along the top (ui/components/ViewTitle.h), so tall, its name this far from the panel's edge, its
// actions so big, so far apart and the last so far from the edge; a section's header, as a pane's (the Graph's), so tall, its
// chevron and its title this far in, under a line inset this far at each end; a tree's rows: the first level's chevron this
// far in, each level this much further, a chevron so big, and the text (or an icon, then the text) this far after a chevron
// or an icon, what a heading is about this far after its name; an action on a row or a heading this wide, and a count's
// badge so tall (ui/components/SectionHeader.h); and the fields and buttons at the top (the commit's message and Commit, the
// search), this far from the panel's sides, in rows so tall, themselves so tall.
namespace SideBar
{
    inline constexpr int titleHeight = 32, titleInset = 12, titlePadding = 4;
    inline constexpr int actionSize = 22, actionGap = 4;
    inline constexpr int paneHeaderHeight = 28, paneChevron = 10, paneText = 28, paneLineInset = 8;
    inline constexpr int rowChevron = 12, indent = 8, chevronSize = 16, gap = 6, detailGap = 10;
    inline constexpr int rowActionWidth = 22;
    inline constexpr float badgeHeight = 16.0f;
    inline constexpr int fieldLeft = 12, fieldRight = 8;
    inline constexpr int inputRowHeight = 34, buttonRowHeight = 36, controlHeight = 26;
} // namespace SideBar

// A view area's tabs (ui/shell/Panels.h), as VS Code's panels' in its Modern UI: pills so tall in the middle of the bar, so far
// apart, their names this far from their ends. A device editor's pages' tabs (ui/devices/DeviceLayout.h) are the same, smaller.
namespace TabTint
{
    inline constexpr float pillHeight = 24.0f, pillInset = 2.0f;
    inline constexpr int textInset = 10;
    inline constexpr float pagePillHeight = 20.0f;
    inline constexpr int pageTextInset = 8;
} // namespace TabTint

// A shortcut's keys as VS Code's command search shows them (drawKeyCaps()): each in a little box with these corners, so tall,
// its text so big and so far from its ends, the boxes so far apart; filled in the muted text colour this opaque, outlined in
// the control border.
namespace KeyCap
{
    inline constexpr float corner = 3.0f, height = 18.0f, textSize = 11.0f, padding = 5.0f, gap = 3.0f;
    inline constexpr float fill = 0.17f;
} // namespace KeyCap

// The main window's bars and panels (ui/shell/AppShell.h, PanelLayout.h, TitleBar.h, StatusBar.h).
namespace Shell
{
    inline constexpr int titleBarHeight = 35;     // the menus, Undo and Redo, the command search and the panels' switches
    inline constexpr int transportBarHeight = 44; // play, record, the tempo and the master level
    inline constexpr int tempoStretchWidth = 320; // what a tempo change stretches, under the tempo field as it changes
    inline constexpr int tempoStretchSeconds = 4; // how long that stays once the tempo stops changing

    // Whether the project is saved, the selection, the hints, the audio output...: as VS Code's status bar, its items so
    // tall in the middle of it, each lit under the pointer in the hover wash, rounded.
    inline constexpr int statusBarHeight = 28, statusItemHeight = 22;

    // As VS Code's Modern UI lays out its parts: each panel a card, rounded (cardCornerRadius) and outlined in the border
    // colour, so far apart and from the window's sides and foot, on the window's frame (the surface).
    inline constexpr int cardGap = 4;

    // The title bar's parts: the window's buttons, as wide as Windows 11's, with their glyphs, Close red under the pointer as
    // VS Code's (in every theme); the command search, as wide as it can be in the middle and as tall as VS Code's command
    // center; the project's name at its widest; the
    // panels' switches, a little window with the panel's part filled while it shows; and how faint the bar's text goes while
    // another window is in front. The window's edges resize it within this band.
    inline constexpr int windowButtonWidth = 46;
    inline constexpr float windowGlyphSize = 10.0f;
    inline constexpr juce::uint32 closeButtonColour = 0xe6e81123, closeButtonText = 0xffffffff;
    inline constexpr int searchMinimumWidth = 240, searchMaximumWidth = 520, searchIconSize = 14, searchHeight = 22;
    inline constexpr int projectNameWidth = 200;
    inline constexpr float panelSwitchWidth = 16.0f, panelSwitchHeight = 12.0f, panelSwitchCorner = 2.0f;
    inline constexpr float panelSwitchSideShare = 0.35f, panelSwitchBottomShare = 0.4f; // of the little window, the panel's part
    inline constexpr float inactiveAlpha = 0.6f;

    // Phazite's logo at the title bar's left end, as VS Code's is at its: in a slot as wide as the bar is tall, so big in its
    // middle. And the window's icon, where the system takes it from the window (Linux), so many pixels across.
    inline constexpr int logoSlotWidth = 35, logoSize = 16;
    inline constexpr int windowIconSize = 128;

    inline constexpr int resizeBand = 5; // along the top and the title bar's sides
    inline constexpr int edgeInset = 8;  // the panels keep out of the band along a restored window's sides, which resizes it

    // The status bar's CPU in the warning colour from here, in percent: the sound is close to breaking up.
    inline constexpr int busyCpuPercent = 80;

    // The status bar's bar of how far clips being stretched in the background have come; and the least room its items leave
    // the hints, which drop the least needed items rather than shrink below it.
    inline constexpr int statusProgressWidth = 80, statusProgressHeight = 6;
    inline constexpr int statusHintsMinimumWidth = 240;

    inline constexpr int dividerSize = cardGap; // between panels, dragged to size them
    inline constexpr float dividerLit = 0.5f;   // the focus colour's alpha on a divider under the pointer

    // A divider dragged until its panel would be smaller than this part of the least it shrinks to closes the panel, as
    // VS Code's sashes close its side bar and panel (at half their least); dragged back before the drag ends, it opens again.
    inline constexpr double dividerSnap = 0.5;

    // The buttons in the lower panel's top right corner: maximize, a window of its own and hide; and what they take of it,
    // with their margins, so that what is there leaves them room.
    inline constexpr int panelButtonSize = 26, panelCornerButtons = 3;

    constexpr int getPanelCornerWidth (int buttons)
    {
        return buttons * panelButtonSize + buttons * (Space::xs / 2);
    }

    inline constexpr int minimumLeftPanelWidth = 160, maximumLeftPanelWidth = 480, leftPanelWidth = 240; // the browser or the versions

    // The activity bar along the window's left edge (ui/shell/ActivityBar.h), as VS Code's Modern UI's: its width, its items'
    // height and the room between them, their icons, and the rounded box behind the icon of the view that shows and under
    // the pointer; and the badge on the Source Control icon with how many changes the song has since its last commit, up to
    // the most it counts ("99+" past it).
    inline constexpr int activityBarWidth = 44, activityItemHeight = 36, activityItemGap = 8, activityIconSize = 24;
    inline constexpr float activityBoxSize = 32.0f;
    inline constexpr float activityBadgeHeight = 16.0f;
    inline constexpr int activityBadgeMost = 99;
    inline constexpr int minimumInspectorWidth = 180, maximumInspectorWidth = 420, inspectorWidth = 260;
    inline constexpr int minimumCentreWidth = 300, minimumCentreHeight = 120; // the song's views, and the editor dock
    inline constexpr int minimumAreaWidth = 240; // an area of views beside another in a row of the centre (ui/shell/Panels.h)
    inline constexpr double minimumEditorShare = 0.1, maximumEditorShare = 0.85, editorShare = 0.45; // of the centre's height
    inline constexpr double mixEditorShare = 0.4; // in the Mix layout, which gives the mixer more
} // namespace Shell

// Each kind of built-in device's colour (ui/devices/DeviceTint.h), so that each has a character of its own and its kind shows at
// a glance: the dynamics warm, the EQ and the filter cyan, the saturator red, the delay and the reverb blue, the phaser and
// the flanger green, the stereo expander teal and the vocoder pink; instruments keep the accent. Each is at least 3:1
// against the surface in every theme, and apart from the modulation colour, which rings their knobs.
enum class DeviceFamily
{
    instrument,
    dynamics, // compressor, limiter, maximizer, multiband
    tone,     // EQ, filter
    drive,    // saturator
    space,    // delay, reverb
    motion,   // phaser, flanger
    stereo,   // stereo expander
    voice     // vocoder
};

inline std::optional<juce::Colour> getFamilyColour (DeviceFamily family, ThemeName theme)
{
    const auto pick = [theme] (juce::uint32 dark, juce::uint32 light, juce::uint32 highContrast)
    {
        return juce::Colour (theme == ThemeName::light ? light : theme == ThemeName::highContrast ? highContrast : dark);
    };

    // clang-format off
    switch (family)
    {   //                                    dark        light       high contrast
        case DeviceFamily::dynamics: return pick (0xffff9a5c, 0xffb4501c, 0xffffa05a);
        case DeviceFamily::tone:     return pick (0xff3ecbe0, 0xff00708a, 0xff2fe6ff);
        case DeviceFamily::drive:    return pick (0xffff5f73, 0xffc42540, 0xffff6b80);
        case DeviceFamily::space:    return pick (0xff6fa8ff, 0xff2f5fd0, 0xff7fb2ff);
        case DeviceFamily::motion:   return pick (0xff5fd38d, 0xff3b7d12, 0xff4dff9a);
        case DeviceFamily::stereo:   return pick (0xff26d4a8, 0xff0b7d64, 0xff2bffc8);
        case DeviceFamily::voice:    return pick (0xffff7fd0, 0xffb3317d, 0xffff7fb8);
        case DeviceFamily::instrument:
        default:                     return std::nullopt;
    }
    // clang-format on
}

// The Source Control view (ui/versions/VersionsView.h): its rows, and the graph, a line for each branch in its color with a
// dot for each of its commits.
namespace History
{
    inline constexpr int changeRowHeight = ListRow::height, versionRowHeight = 30, noteHeight = 52;
    inline constexpr int laneWidth = 12; // a branch's line's column
    inline constexpr float dotSize = 8.0f, lineWidth = 2.0f;
    inline constexpr int buttonWidth = 72;                     // Done, Merge and Cancel, at their widest
    inline constexpr float tickSize = 12.0f, tickInset = 3.0f; // a merge's line's box, filled while the line is taken
    inline constexpr int playWidth = 20;                       // a version's play mark's column, while any has a bounce
    inline constexpr float playSize = 8.0f;

    // As VS Code's Source Control view lays out its parts (SideBar): the message, at most so many lines tall as it grows;
    // Commit across the view with its menu's part at its right end; a section's count's badge; and a change's row, one level
    // into its group: its icon where a chevron would be, its name and what changed after it, its actions under the pointer and
    // its letter (U, A, M, D, C) at its end, so big.
    inline constexpr int messageLines = 6;
    inline constexpr int saveMenuWidth = 28;
    inline constexpr float rowIconSize = 14.0f;
    inline constexpr int rowIcon = SideBar::rowChevron + SideBar::indent, rowText = rowIcon + SideBar::chevronSize + SideBar::gap;
    inline constexpr int actionWidth = SideBar::rowActionWidth, letterWidth = 16;
    inline constexpr float letterSize = 11.0f;
    inline constexpr float branchBadgeAlpha = 0.25f;      // a branch's color behind its name on its last commit, in the graph
    inline constexpr float strikethroughThickness = 1.0f; // through the name of what was taken away
} // namespace History

// The changes since a commit (ui/versions/VersionsView.h), as VS Code colors what git finds changed: what was added green,
// what changed amber, what was taken away red, its letter beside each, so that color is never the only sign. VS Code's 2026
// Dark and 2026 Light colors (gitDecoration.*), and High Contrast's.
enum class ChangeKind
{
    added,
    changed,
    removed
};

inline juce::Colour getChangeColour (ChangeKind kind, ThemeName theme)
{
    const auto pick = [theme] (juce::uint32 dark, juce::uint32 light, juce::uint32 highContrast)
    {
        return juce::Colour (theme == ThemeName::light ? light : theme == ThemeName::highContrast ? highContrast : dark);
    };

    // clang-format off
    switch (kind)
    {   //                                 dark        light       high contrast
        case ChangeKind::added:   return pick (0xff73c991, 0xff587c0c, 0xffa1e3ad);
        case ChangeKind::removed: return pick (0xfff48771, 0xffad0707, 0xffff6b6b);
        case ChangeKind::changed:
        default:                  return pick (0xffe5ba7d, 0xff667309, 0xffe2c08d);
    }
    // clang-format on
}

// What pops up over the window, a menu, a tooltip or a list such as the command search, on frosted glass as VS Code's are
// (ui/theme/Popups.h): what is behind it blurred this much (CSS's blur(12px), a standard deviation), under the popup's own
// colour at this opacity (workbench.modernUIFrostedGlassOpacity's default), the blur worked out at this fraction of the
// screen's resolution, which it hides; solid where it isn't frosted. Rounded (cardCornerRadius), outlined in the border
// colour, over a soft shadow this wide and dark. A menu as VS Code's: at least so wide, its items so tall, each inset from
// its sides with its own corners (itemCornerRadius), the one highlighted in a wash of the focus colour outlined in it, its
// text and its shortcut, fainter, this far from its ends; its separators' rows so tall. And a picker's list
// (ui/versions/BranchPicker.h): its width, its rows and how many show at once, and its icons.
namespace Popup
{
    inline constexpr float frostBlur = 12.0f, frostTint = 0.8f, frostResolution = 0.25f;
    inline constexpr int shadowRadius = 12;
    inline constexpr float shadowAlpha = 0.36f;
    inline constexpr int menuMinimumWidth = 160, itemHeight = 24, itemInset = 4, itemTextInset = 12, separatorHeight = 9;
    inline constexpr float selectionWash = 0.15f, shortcutAlpha = 0.7f;
    inline constexpr int pickerWidth = 520, pickerRowHeight = 30, pickerRows = 10;
    inline constexpr float pickerIconSize = 16.0f;

    // A tooltip, as VS Code's hovers: its text this far from its edges, its lines at most so long; and so far from the pointer.
    inline constexpr int tooltipPaddingX = 8, tooltipPaddingY = 4, tooltipMaximumWidth = 400, tooltipOffset = 12;
} // namespace Popup

// A button as VS Code's: filled in the accent (the main one, or one switched on), else clear with a border in the control
// border colour, the hover wash over it under the pointer; outlined in the focus colour while it has the keyboard focus, its
// fill this far inside the outline, as VS Code's focus outline is drawn outside the button. A tick box this big, rounded.
namespace ButtonTint
{
    inline constexpr float focusGap = 2.0f;
    inline constexpr float tickBoxSize = 18.0f, tickBoxCorner = 3.0f;
} // namespace ButtonTint

// The knobs of devices' editors, square: a device's main controls large, the rest compact.
inline constexpr int knobSize = 64;
inline constexpr int compactKnobSize = 40;

// Colours that tell apart the parts of a device's editor, such as the EQ's bands. Each part also shows its number,
// with an outline in the theme's text colour, so that colour is never the only sign.
inline constexpr juce::uint32 partColours[] = { 0xffe5534b, 0xfff0883e, 0xffd4a72c, 0xff57ab5a,
                                                0xff39c5cf, 0xff539bf5, 0xff986ee2, 0xffe275ad };

// A device editor's graph, such as the EQ's: the analyzer's fill and line, the grid, the line at 0 dB and a selected
// part's own curve (alphas), then the curve's width, the dots' radius and the graph's height.
namespace GraphTint
{
    inline constexpr float spectrumFill = 0.14f;
    inline constexpr float spectrumLine = 0.35f;
    inline constexpr float grid = 0.1f;
    inline constexpr float zeroLine = 0.25f;
    inline constexpr float partCurve = 0.25f;
    inline constexpr float partOff = 0.5f; // a part that is off, such as an EQ band's button
    inline constexpr float marker = 0.5f;  // a line that marks a setting, such as a compressor's threshold
    inline constexpr float curveWidth = 2.0f;
    inline constexpr float dotRadius = 8.0f;
    inline constexpr int height = 280;
    inline constexpr int minimumHeight = 120;     // the least a graph shrinks to, in a Devices view shorter than its editor
    inline constexpr int minimumWidth = 200;      // and narrows to, in one narrower, before its editor shows its parts in pages
    inline constexpr float levelDotRadius = 5.0f; // a dot that rides a curve at a level, such as a synth's filter at its cutoff
    inline constexpr float curveWash = 0.15f;     // under a dynamics curve, as Maximus shades it
    inline constexpr float heardWash = 0.55f;     // and up to the level its compressor hears, which moves with the sound
    inline constexpr int meterWidth = 12;         // a meter beside a graph, such as how far a compressor turns down
    inline constexpr float padLit = 0.85f;        // how far a drum pad's colour fills it as it plays (partCurve when it doesn't)

    // A point of a picture that a drag moves, such as an envelope's corner, and how near the pointer must come to take one
    // (pixels); and how far a drag with Shift moves it, as on knobs.
    inline constexpr float handleRadius = 4.5f;
    inline constexpr float grabDistance = 12.0f;
    inline constexpr float fineDragFactor = 0.2f;
    inline constexpr float handleOutline = 1.5f; // a hollow handle's, such as a compressor's threshold ring
} // namespace GraphTint

// How far a button that needs attention, such as one asking for the instrument a track's notes need, is mixed with the
// warning color (as dark as VS Code's warning fields are, so that its text still reads at 4.5:1).
inline constexpr float attentionTint = 0.2f;

// The accent color's alpha behind something selected: a device's slot in the mixer, or a browser row while the browser
// hasn't the keyboard focus.
inline constexpr float selectionWash = 0.3f;

// The playhead (ui/timeline/TimelineEditor.h), in the focus colour, VS Code's light blue, so that it is easy to find: a marker
// at the foot of the ruler, square at the top and coming to a point (its body this tall, its top corners this round), and a
// line this wide from the point down over the time line; and its alpha while it shows the notes being edited playing in a
// linked clip elsewhere in the song, rather than the song's own position.
namespace PlayheadTint
{
    inline constexpr int markerWidth = 12, markerHeight = 12;
    inline constexpr float body = 6.0f, corner = 2.0f, lineWidth = 2.0f;
} // namespace PlayheadTint

inline constexpr float linkedPlayheadAlpha = 0.5f;

// Track colors are saved in projects, so their palette is in the model: phazite::getTrackColours().

// How strongly a track's color tints its header: how far the header's surface is mixed with it. The track's clips
// are in its color; its row in the playlist isn't (the owner's choice, 2026-10-16): a selected track's row is only a
// shade lighter, the alpha of the text color over the background.
namespace TrackTint
{
    inline constexpr float selectedRow = 0.06f;
    inline constexpr float header = 0.3f;
    inline constexpr float selectedHeader = 0.5f;
} // namespace TrackTint

// A track's header's left edge in the playlist (ui/playlist/TrackHeader.h), and its automation lanes': the strip in its
// color, and how far each folder it is in moves it right, which the folder's rail, as wide as a strip, runs down (pixels).
namespace TrackStrip
{
    inline constexpr int width = 8;
    inline constexpr int indent = 12;
} // namespace TrackStrip

// A track's peak bar along its header's right edge (ui/components/PeakBar.h), as FL Studio's playlist shows that a track
// sounds: so wide, as tall as the row; filled in the track's colour, this much brighter, over a trough of it this opaque; a
// clip lit at its top this tall, for so many seconds.
namespace PeakBarTint
{
    inline constexpr int width = 4, clipHeight = 3;
    inline constexpr float brighter = 0.35f, trough = 0.3f, clipSeconds = 2.0f;
} // namespace PeakBarTint

// Tracks dragged in the playlist: the headers of those that move, this opaque while they move (an alpha); the accent line
// where they would go, this thick, from a dot this wide at the depth in folders they would go to; and a folder they would
// go into, outlined in the accent this thick, over a wash of it (an alpha).
namespace TrackDropTint
{
    inline constexpr float moving = 0.45f;
    inline constexpr float line = 2.0f;
    inline constexpr float dot = 7.0f;
    inline constexpr float outline = 2.0f;
    inline constexpr float wash = 0.12f;
} // namespace TrackDropTint

// How a folder's row in the playlist shows its tracks' clips (model/arrangement/PlaylistModel.h: getFolderSummary()): a thin
// lane for each track that holds clips, at most this tall (pixels), between this much room above and below; each clip a bar
// in its track's color (alphas, a muted one fainter), a pixel apart from the next lane's.
namespace FolderTint
{
    inline constexpr float tallestLane = 5.0f;
    inline constexpr int inset = 3;
    inline constexpr float clip = 0.8f, mutedClip = 0.3f;
} // namespace FolderTint

// How a frozen track shows (docs/design.md, section 18; roadmap step 2.11): its clips under a wash of the palette's frozen
// color, with stripes of it across them this far apart (pixels), like frost (alphas); and its frozen devices' controls
// faded to this alpha.
namespace FrozenTint
{
    inline constexpr float frost = 0.28f;
    inline constexpr float stripes = 0.35f;
    inline constexpr float stripeSpacing = 8.0f;
    inline constexpr float stripeWidth = 1.5f;
    inline constexpr float frozenDevice = 0.45f;
} // namespace FrozenTint

// How an automation lane shows under its track (docs/design.md, section 10): its header's surface mixed with the track's
// color, less than the track's header; its background, the panels' surface this opaque over the playlist's, so that
// the grid shows through; the wash under its line, in the track's color; the dashed line where a parameter without
// automation sits (alphas); and its points' radius, a selected point's (filled with the text color and ringed in the
// accent, so that it stands out on a track of any color), its ring's thickness, the radius of the hollow handle that
// bends the line under the mouse, and how near the pointer must come to take a point, a handle or the line (pixels).
namespace LaneTint
{
    inline constexpr float header = 0.15f;
    inline constexpr float background = 0.6f;
    inline constexpr float wash = 0.12f;
    inline constexpr float idleLine = 0.5f;
    inline constexpr float pointRadius = 4.0f;
    inline constexpr float selectedPointRadius = 5.5f;
    inline constexpr float selectedRing = 2.0f;
    inline constexpr float handleRadius = 4.0f;
    inline constexpr float grabDistance = 8.0f;
} // namespace LaneTint

// The ring around a modulated knob (ui::Knob), in the modulation color: how thick its arc is, how far outside the knob's
// edge, the size of the dot where the value is now, and how far inside the edge a drag takes the ring rather than the
// knob (pixels); and how faint it is while its modulators are switched off, as is a modulator's meter (an alpha). While
// a modulator is dragged: how faint the ring is round each knob it could modulate, and how thick it is round the one under
// the pointer, over a wash of the modulation color (alpha).
namespace ModulationRing
{
    inline constexpr float width = 1.5f;
    inline constexpr float offset = 1.0f;
    inline constexpr float dotSize = 3.5f;
    inline constexpr float grabWidth = 3.0f;
    inline constexpr float offAlpha = 0.35f;
    inline constexpr float targetAlpha = 0.5f;
    inline constexpr float dropWidth = 2.5f;
    inline constexpr float dropWash = 0.2f;
} // namespace ModulationRing

// A modulator being dragged by its grip (ui/mixer/StripParts.h): its grip lit in a wash of the modulation colour under
// the pointer and while dragging (alpha); the chip with its name that follows the pointer, its height, and how far right
// of and below the pointer it is, so that the knob under the pointer stays in view (pixels), and how opaque it is; and
// the outline of the slot it is dragged from.
namespace ModulatorDragTint
{
    inline constexpr float gripWash = 0.25f;
    inline constexpr int chipHeight = 20;
    inline constexpr int chipOffsetX = 14, chipOffsetY = 12;
    inline constexpr float chipAlpha = 0.92f;
    inline constexpr float sourceOutline = 1.5f;
} // namespace ModulatorDragTint

// How the song's sections show behind the playlist's tracks, as ghosts: a faint wash of each section's color
// (stronger while it is selected) and a line in it where the section starts (alphas).
namespace SectionTint
{
    inline constexpr float wash = 0.04f;
    inline constexpr float selectedWash = 0.1f;
    inline constexpr float edge = 0.6f;
} // namespace SectionTint

// The piano roll's velocity lane (ui/pianoroll/PianoRollParts.h): each note's stem, this wide, centred on where the note
// starts, with a ball this wide on top, centred on the stem (pixels).
namespace VelocityTint
{
    inline constexpr float stem = 2.0f;
    inline constexpr float ball = 6.0f;
} // namespace VelocityTint

// The piano roll's marks on slide and portamento notes (model/notes/NoteValues.h), in the note's ink at its start: a slide
// note's triangle, pointing on as FL Studio's slide icon does, and a portamento note's rising stroke, each at most this big,
// this far inside the note, the stroke this thick (pixels).
namespace NoteMarkTint
{
    inline constexpr float size = 8.0f;
    inline constexpr float inset = 3.0f;
    inline constexpr float stroke = 1.5f;
} // namespace NoteMarkTint

// How the chord lane shows the song's chords (ui/playlist/ChordLane.h): each a block in a wash of the accent color,
// stronger under the pointer, with a line where it starts, and a line in the warning color where the key changes
// (alphas); the piano roll's wash over the rows of the notes of the chord there (an alpha of the accent color); the
// lane's height, and how near a chord's right edge a drag takes the edge (pixels).
namespace ChordTint
{
    inline constexpr float block = 0.16f;
    inline constexpr float hoveredBlock = 0.28f;
    inline constexpr float edge = 0.7f;
    inline constexpr float keyChange = 0.9f;
    inline constexpr float pianoRollRows = 0.1f;
    inline constexpr int laneHeight = 24;
    inline constexpr int grabDistance = 6;
} // namespace ChordTint

// How an audio clip's fades show, in the playlist and the audio clip editor: what a fade takes away is shaded black above
// its curve from silence to full volume (alphas), and the curve's thickness.
namespace FadeTint
{
    inline constexpr float shade = 0.3f;
    inline constexpr float curve = 0.6f;
    inline constexpr float curveWidth = 1.5f;
} // namespace FadeTint

// How the audio clip editor shows its clip (ui/audioclip/AudioClipEditor.h): over the panels' background, the clip in a
// wash of its track's color with its audio in that color, and the audio its edges trim off in the muted text color
// (alphas); each edge a bracket, its line (thicker under the pointer) with ticks this long into the clip along its top and
// bottom, and a grip in its middle, which a drag trims; the round handles at its top, which a drag fades, larger under the
// pointer, ringed in the text color; the hollow handle in the middle of each fade's curve, which a drag up or down bends, as
// an automation lane's line bends, filled under the pointer (radii), shown on a fade at least so wide; how far a drag of
// half the clip's height bends a fade, from straight to all the way (a bend of 1); and how near the pointer must come to
// an edge or a handle to take it (pixels).
namespace AudioClipTint
{
    inline constexpr float clipWash = 0.16f;
    inline constexpr float trimmedAudio = 0.35f;
    inline constexpr float edgeWidth = 2.0f, hoveredEdgeWidth = 3.0f;
    inline constexpr float bracketLength = 10.0f;
    inline constexpr int gripWidth = 8;
    inline constexpr int gripHeight = 36;
    inline constexpr float handleRadius = 6.0f, hoveredHandleRadius = 7.5f, handleRing = 1.5f;
    inline constexpr float bendRadius = 5.0f, hoveredBendRadius = 6.5f;
    inline constexpr float minimumBendWidth = 24.0f;
    inline constexpr float bendPerHalfHeight = 1.0f;
    inline constexpr int grabDistance = 8;
} // namespace AudioClipTint

// A track's arm switch (ui/components/ArmButton.h): its dot, as a part of the button's height, and the dot's outline while
// it is off (pixels); lit, the dot is the danger colour, as record buttons are.
namespace ArmTint
{
    inline constexpr float dot = 0.55f;
    inline constexpr float outline = 1.5f;
} // namespace ArmTint

// The lesson card (ui/shell/LessonCard.h): as wide as this, with a shadow this deep around it (pixels) and so dark (an
// alpha), its corners, its outline in the accent colour and the dots of how far the lesson has come; and the ring around
// the control a step is about: its width and how far outside the control (pixels); as a step starts, so many rings that
// spread out this far and fade, over so many seconds, drawn so many times a second; and after, how many times a second
// the card looks whether the control moved and whether the song shows the step done.
namespace LessonTint
{
    inline constexpr int width = 380;
    inline constexpr int shadow = 12;
    inline constexpr float shadowAlpha = 0.4f;
    inline constexpr float corner = 8.0f;
    inline constexpr float outline = 1.5f;
    inline constexpr float dot = 6.0f;
    inline constexpr float ringWidth = 2.5f, ringGap = 3.0f;
    inline constexpr int pulses = 3;
    inline constexpr float pulseSpread = 10.0f;
    inline constexpr double pulseSeconds = 1.8;
    inline constexpr int pulseFramesPerSecond = 30, checksPerSecond = 4;
} // namespace LessonTint

// A view's tab dragged over one of the two areas (ui/shell/Panels.h): the accent colour's alpha over the area, and the width of
// the line where its tab would go (pixels).
namespace ViewDropTint
{
    inline constexpr float wash = 0.12f;
    inline constexpr float line = 2.0f;
} // namespace ViewDropTint

// A track's mute and solo switch (ui/components/TrackSwitch.h), and every on/off switch, drawn as it is (PowerSwitch.h):
// its light, as a part of the switch's size, with an edge in the background colour that keeps it apart from a track's
// tinted header, and the ring of a muted one's light (its outline, an off switch's too) and of a soloed one's, this far
// outside it (pixels); and how faint the light is while other tracks' solos silence the track, or a switch is disabled
// (an alpha). A switch is this large to click, its light smaller.
namespace SwitchTint
{
    inline constexpr float light = 0.5f;
    inline constexpr float edge = 1.0f;
    inline constexpr float ring = 1.5f, ringGap = 2.0f;
    inline constexpr float silenced = 0.3f;
    inline constexpr int size = 20;
    inline constexpr float partOff = 0.45f; // a device's part while its switch is off (PowerSwitch.h), faint
} // namespace SwitchTint

// An audio clip in the playlist while its render is being made (roadmap step 2.18), silent until then: its waveform faint
// (alpha), and a bar along its bottom edge of how far the render has come, over a track of the same ink (alpha, height).
namespace RenderingTint
{
    inline constexpr float waveform = 0.3f;
    inline constexpr float barTrack = 0.25f;
    inline constexpr float barHeight = 3.0f;
} // namespace RenderingTint

// How the loop shows in a ruler: its lane along the top, which a drag loops (a shade of the text color), a bar of
// the loop color in that lane over the part that loops, and a wash of it below (alphas); the lane's height, and how
// near the loop's edge a press takes the edge (pixels).
namespace LoopTint
{
    inline constexpr float lane = 0.06f;
    inline constexpr float bar = 0.9f;
    inline constexpr float wash = 0.14f;
    inline constexpr int laneHeight = 8;
    inline constexpr int edgeGrab = 4;
} // namespace LoopTint

// How the wavetable synth's editor draws an oscillator's table (ui/devices/WavetableEditor.h): its frames one behind
// another in the muted text color, the further back the higher and further right (by these parts of the drawing's height
// and width at the back), the frame it plays in the accent color, and where a sound dropped on it would go in a wash of
// the accent color (alphas); at most so many frames, and the drawing's height, at most and at least (pixels).
namespace WavetableTint
{
    inline constexpr float frames = 0.22f;
    inline constexpr float rise = 0.35f, shift = 0.2f;
    inline constexpr float dropWash = 0.2f;
    inline constexpr float lineWidth = 1.0f, playedWidth = 2.0f;
    inline constexpr int framesDrawn = 24;
    inline constexpr int height = 120, minimumHeight = 44;
} // namespace WavetableTint

// WCAG 2.2's contrast ratio between two colours, from 1 (none) to 21 (black on white). Text needs at
// least 4.5, and the parts of a control that identify it at least 3.
inline double getContrastRatio (juce::Colour a, juce::Colour b)
{
    const auto luminance = [] (juce::Colour c)
    {
        const auto channel = [] (juce::uint8 value)
        {
            const auto v = value / 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4);
        };

        return 0.2126 * channel (c.getRed()) + 0.7152 * channel (c.getGreen()) + 0.0722 * channel (c.getBlue());
    };

    const auto la = luminance (a), lb = luminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}

// Black or white, whichever reads better on a color: for text on track, clip and section colors.
inline juce::Colour getInkFor (juce::Colour background)
{
    return getContrastRatio (juce::Colours::black, background) >= getContrastRatio (juce::Colours::white, background)
               ? juce::Colours::black
               : juce::Colours::white;
}
} // namespace phazite::ui
