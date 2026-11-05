# Phazite's look, from VS Code's

Phazite's dark and light themes are VS Code's **2026 Dark** and **2026 Light**, and its window is laid out as VS Code's **Modern UI** lays out its own (the owner's decision, 2026-11-05: [design.md](design.md#design-language)). This page says what was taken, where from, and where Phazite does otherwise. The values themselves live in one place, `src/ui/theme/DesignTokens.h`, and JUCE's controls are drawn with them in `src/ui/theme/LookAndFeel.cpp`.

## May Phazite copy it?

Yes, as values, with Microsoft's notice. VS Code's source ([microsoft/vscode](https://github.com/microsoft/vscode), "Code - OSS"), its themes' files and its CSS included, is under the MIT licence, which lets anyone copy and change it as long as the copyright notice and the licence go with every copy. Phazite takes colours and sizes, not code, and ships the notice next to the app (`licenses/VSCode.txt`, from `resources/licenses/`; [third-party.md](third-party.md)). Colour values and sizes on their own aren't protected by copyright; a whole look can be protected as trade dress only where it tells people whose product it is, and Phazite doesn't present itself as VS Code.

What Phazite doesn't take:

- **The name and the logo.** "Visual Studio Code" and "VS Code" are Microsoft's trademarks: Phazite's themes are called Dark and Light, and nothing in the app says VS Code. The documentation names it only to say where the values come from.
- **Codicons**, VS Code's icons, which are under CC BY 4.0 (a different licence, asking for credit wherever they show): Phazite keeps Lucide's (ISC), at VS Code's sizes where it has them (24 px in the activity bar).
- **Segoe UI**, Windows' font, which may not be shipped with an app: Phazite keeps Inter, at VS Code's sizes and weights.

## Where the values come from

Read from VS Code's repository on 2026-11-05 (the files as they are on its `main` branch then):

- The themes: `extensions/theme-defaults/themes/2026-dark.json` and `2026-light.json` (each over `dark_modern.json` / `light_modern.json`).
- The Modern UI: `src/vs/workbench/contrib/modernUI/` (its `README.md` and `browser/media/*.css`: activity bar, command center, rounded corners, padding, tabs, pane headers, status bar, frosted glass, font ramp) and `src/vs/workbench/browser/media/floatingPanels.css`, with the sizes in `src/vs/workbench/browser/parts/activitybar/activitybarPart.ts`, `statusbar/statusbarPart.ts`, `src/vs/workbench/services/layout/browser/layoutService.ts` (`FLOATING_PANEL_MARGIN`) and `src/vs/platform/window/common/window.ts` (the title bar's height).
- The registries of sizes and colours: `src/vs/platform/theme/common/sizes/baseSizes.ts` (text sizes, weights, corner radii, spacing) and `colors/*.ts` (what a colour falls back to where a theme doesn't set it: the Modern UI's tabs and activity bar items).
- The controls: `src/vs/base/browser/ui/` (`menu/menu.ts`, `button/button.css`, `inputbox/inputBox.css`, `toggle/toggle.css`, `countBadge/countBadge.css`, `keybindingLabel/keybindingLabel.css`, `scrollbar/`, `list/`) and `src/vs/platform/quickinput/browser/` (the command search).

## Colours

Each palette entry (`ui::Palette`) is the VS Code colour named beside it, in 2026 Dark / 2026 Light. The high contrast theme is Phazite's own (black, white edges, a yellow accent), as before.

| Phazite | VS Code | Dark | Light |
|---|---|---|---|
| `background`: the song's views (playlist, piano roll, mixer, audio clip editor) | `editor.background` | `#121314` | `#FFFFFF` |
| `surface`: the window's frame (title, transport and status bars, the gaps between cards) and the side panels | `sideBar.background`, `titleBar.activeBackground`, `statusBar.background` | `#191A1B` | `#FAFAFD` |
| `surfaceRaised`: menus, call-outs, tooltips, rulers | `menu.background`, `editorWidget.background` | `#202122` | `#F0F0F3` (VS Code's `editorStickyScrollHover`; its menus are `#FAFAFD`, as the panels) |
| `border`: the cards' edges, lines between parts | `surface.border`, `widget.border`, `menu.border` | `#2A2B2C` | `#E4E5E6` |
| `controlBorder`: fields, lists to pick from, clear buttons | `input.border`, `dropdown.border`, `button.secondaryBorder` | `#333536` | `#D8D8D8` |
| `text` | `foreground` | `#BFBFBF` | `#202020` |
| `textMuted` | `descriptionForeground`, `statusBar.foreground` | `#8C8C8C` | `#606060` |
| `textBright`: on a selection | `list.activeSelectionForeground` | `#EDEDED` | `#202020` |
| `accent`: the main button, what is on, the command search's row | `button.background`, `quickInputList.focusBackground` | `#297AA0` | `#0069CC` |
| `textOnAccent` | `button.foreground` | `#FFFFFF` | `#FFFFFF` |
| `focus`: the keyboard focus, a menu's highlighted item | `focusBorder` (70 % opaque in VS Code), `menu.selectionBorder` | `#3994BC` | `#0069CC` |
| `hover`: under the pointer | `list.hoverBackground` | `#FFFFFF14` | `#00000014` |
| `selected` | `list.activeSelectionBackground` | `#FFFFFF22` | `#00000025` |
| `selectedInactive`: the tab that shows, the activity bar's view | `list.inactiveSelectionBackground` (`modernTab.activeBackground`) | `#2C2D2E` | `#DADADA99`, made opaque: `#E7E7E8` |
| `scrollThumb` | `scrollbarSlider.background` (`hoverBackground`, `activeBackground` brighter) | `#A8A9AA85` (`90`, `9C`) | `#646464C0` (`D0`, `E0`) |
| `warning` | `list.warningForeground` | `#E5BA7D` | `#895503` (VS Code's `problemsWarningIcon`, which reads as amber) |
| `danger` | `errorForeground` | `#F48771` | `#AD0707` |
| The changes' letters and names (`getChangeColour()`) | `gitDecoration.added/modified/deletedResourceForeground` | `#73C991`, `#E5BA7D`, `#F48771` | `#587C0C`, `#667309`, `#AD0707` |
| A menu's highlighted item | `menu.selectionBackground` | `#3994BC26` | `#0069CC1A` |
| A shortcut's keys | `keybindingLabel.*` | muted text at 17 %, the control border | the same |

Phazite's own colours (the loop, modulation, frost, a level brought up, a track's switches, the devices' families, the parts' colours) stay as they were: VS Code has nothing like them. Every pair still meets WCAG 2.2's contrast (`tests/DesignTokenTests.cpp`): text 4.5:1 on everything it is drawn on, controls' parts 3:1. For that, a button that needs attention is mixed with the warning colour 20 % now, not 35 % (`attentionTint`), and text that was in the accent (the top bar's "hearing", the lesson card's kind) is in the focus colour.

## Layout

As the Modern UI's floating panels (`DesignTokens.h`, `Shell`):

- **Cards.** Each panel is a card with corners of 8 px (`cornerRadius.large`) and a 1 px edge in the border colour, 4 px apart and 4 px from the window's sides and from the status bar (`FLOATING_PANEL_MARGIN`), on the frame's colour, which is the title bar's. The activity bar and the left panel are one card, the line between them the activity bar's own, as VS Code joins its activity bar to its side bar. Each area of views (the Playlist and Mixer tabs, the clip editor's) is a card; so is the inspector. The dividers are the 4 px gaps; under the pointer, a line in the focus colour, as VS Code's sashes. (`ui::PanelCards`, over the panels, under the lesson card.)
- **Title bar**: 35 px (`DEFAULT_CUSTOM_TITLEBAR_HEIGHT`), on the frame, without a line under it. The command search as VS Code's command center: 22 px tall, corners of 6 px, clear in a faint outline, washed and outlined in the control border under the pointer. The window's buttons wash under the pointer, Close red (`#E81123`, 90 %).
- **Transport bar**: Phazite's own (VS Code has none), on the frame under the title bar, as one with it.
- **Activity bar**: 44 px wide (36 px and its 8 px lane), its items 36 px tall and 8 px apart, icons 24 px; the view that shows on a 32 px box with corners of 4 px in the inactive selection's colour, its icon bright; the one under the pointer on the hover wash; the rest muted. No marker along its edge. The badge: 16 px tall, fully rounded, in the accent, its count 10 px semibold in white.
- **Status bar**: 28 px (22 px and the Modern UI's 6 px), on the frame, without a line above it, its text 12 px; each item a rounded 22 px pill on the hover wash under the pointer.
- **Tabs** (the view areas'): VS Code's panel tabs in the Modern UI, pills 24 px tall in the 32 px bar, corners of 4 px, 2 px apart, names 13 px with 10 px either side; the one that shows on the inactive selection's colour in bright text, the one under the pointer on the hover wash, the rest muted. No line under them.

## Controls

- **Buttons** (`button.css`): corners of 4 px. The main one, or one switched on, filled in the accent, a little lighter under the pointer; the others clear, in a 1 px edge of the control border, on the hover wash under the pointer. The keyboard focus: a 1 px outline in the focus colour with the fill 2 px inside it, as VS Code draws its outline 2 px outside the button (JUCE can't draw outside a component). Where two join (Commit and its menu), a faint line between them, as VS Code's split buttons. Text 13 px.
- **Fields** (`inputBox.css`) and **lists to pick from**: corners of 4 px, on the panels' surface, a 1 px edge in the control border, the focus colour while they have the keyboard; a list to pick from has a chevron at its right. **Tick boxes** (`toggle.css`): 18 px, corners of 3 px, on the raised surface in a muted edge, the tick in the text colour.
- **Scroll bars** (`scrollbars.css`, `roundedCorners.css`): 10 px wide, a thumb with corners of 4 px, no track and no buttons.
- **Lists**: rows 22 px, the hover and selection washes inset 4 px from the list's sides with corners of 4 px (`padding.css`, `roundedCorners.css`); a selected row's text bright; the browser's selection outlined in the focus colour while it has the keyboard, in the inactive selection's colour when not.
- **Menus** (`menu.ts`): corners of 8 px, a 1 px edge in the border colour, at least 160 px wide, 4 px inside their edge; items 24 px tall with corners of 6 px, text 12 px, 12 px from their ends; the highlighted item on a wash of the focus colour outlined in it; a tick in the left margin; the shortcut at the right at 70 %; separators a 1 px line; group titles semibold. A little translucent (92 %, `modernUIFrostedGlassOpacity`'s default), over a soft shadow.
- **Command search and pickers** (`quickInput.css`): no arrow, as VS Code's quick input, which hangs from the top of the window; rows 22 px (the command search's), the one picked filled in the accent with white text, each shortcut's keys in little boxes (`keybindingLabel.css`: corners of 3 px, 11 px text).
- **Tooltips**: as VS Code's hovers, corners of 8 px on the raised surface in the border colour, text 13 px.
- **Text**: Inter at VS Code's ramp (`baseSizes.ts`, `fontRamp.css`): 13 px for body text, 12 px for labels, section titles, menus and the status bar, 10 px for badges; two weights, regular and semibold; section titles semibold in Title Case, as the Modern UI writes them (not VS Code's older capitals).

## Where Phazite differs, and why

- **No blur behind what pops up.** VS Code blurs what is behind its menus (`backdrop-filter`); JUCE can't, as each menu is a window of its own. Phazite's are as translucent as VS Code's, without the blur.
- **Shadows** are JUCE's drop shadow, 12 px soft at 36 %, around every menu, call-out and tooltip; VS Code's widgets' are softer still (`--vscode-shadow-xl`: 20 px at 15 %), which don't show against a dark window.
- **Focus outlines** are drawn inside a control, its fill shrinking 2 px, where VS Code's sit 2 px outside it.
- **The high contrast theme** stays Phazite's own; VS Code's High Contrast Dark is a different theme that the owner didn't ask for.
- **Fonts and icons**: Inter and Lucide, above.
