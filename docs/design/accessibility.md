# Accessibility

*Design section 21.*

Phazite is meant to be usable without a mouse, with a screen reader, and by people who need more contrast, larger
text or larger targets. Accessibility is part of every control, not a mode.

## Status

| Feature | State | Note |
| --- | --- | --- |
| Every action a command, reachable from the keyboard | Built | |
| Search Commands (the command palette) | Built | |
| Keys that show a view give it the keyboard focus | Built | |
| Arrow keys edit selected notes and automation points | Built | |
| Values typed into knobs, faders and points | Built | |
| A name, role and value on every control, for screen readers | Built | Checked by a test; not yet tried by a person with a screen reader |
| Screen readers on Windows | Built | Through JUCE's UI Automation support; not yet tried by a person |
| Screen readers on macOS | Planned | Comes with macOS (after v1, item 11) |
| Screen readers on Linux | Planned | JUCE has no Linux back end (after v1, item 12) |
| Themes that meet WCAG 2.2 contrast | Built | Checked by a test in every theme |
| A high-contrast theme | Built | |
| Colour never the only signal | Built | |
| Interface size, 75 % to 200 % | Built | |
| Larger click targets (WCAG 2.2, 2.5.8) | Partly built | The smallest targets only: track switches, colour strips, dividers, the loop's edges |
| A setting for reduced motion | Planned | No roadmap step |

## Keyboard

- **Every user action is a command** in one registry, with a name, a description and search words. Menus, keys, the
  shortcuts editor and the command search all use it, so nothing can be done only with the mouse
  ([commands-keys-help.md](commands-keys-help.md)).
- **Search Commands** (`view.commandPalette`, Ctrl+K) finds and runs any command by name.
- **A view its key shows gets the keys.** F5, F6, Shift+F6, F7 and F9 move the keyboard focus into the view they
  show: the playlist, the instrument, the effects, the piano roll or the mixer. A screen reader then says the view's
  name, and Tab goes on into its controls. The left panel's views (Browser, Project, Source Control) take the focus
  the same way.
- The mixer and the Devices view take the focus themselves, as the playlist does, so that the arrow keys still run
  commands until Tab reaches a control.
- **Arrow keys edit what is selected.** They move and transpose selected notes, raise and lower selected automation
  points, and otherwise move through tracks. A held key repeats the commands that take a step.
- **Values can be typed.** A knob, a fader, an automation point or a tempo point takes a value typed into a field
  over it, so that an exact value never needs a drag.
- Controls are built from Phazite's base controls (`ui::Knob`, `ui::Fader` and the others in
  `src/ui/components/Controls.h`), which give keyboard use.

## Screen readers

- Every custom control has a JUCE accessibility handler with a name, a role and a value. Every control has a title.
- **Checked across the window:** every visible control, and every component that takes the keyboard, has a name and
  a role. That includes the scrolling areas, each named for what it scrolls ("Channels", "Properties"). A tree takes
  the keys itself, without a stop of its own for its scrolling.
- Where a state is shown by look alone, it is also said: a crashed plug-in's slot is named "*name*, crashed", an
  activity bar's icon is a switch that is on or off, and a line of the changes is read with its status ("Bass,
  Modified: volume").
- JUCE supports Windows UI Automation and macOS's accessibility. It has no Linux back end.

## Vision

- **Contrast:** every theme meets WCAG 2.2: 4.5:1 for text and 3:1 for controls. A test checks every theme, and that
  track names stay readable on their headers in every track colour.
- **Themes:** Dark, Light and High Contrast (white and yellow on black), and **Theme: Follow System**
  ([window.md](window.md)).
- **Colour is never the only signal.** A linked clip shows a link mark as well as its colour; a line of the changes
  shows a letter as well as its colour; a frozen track shows a mark in its header; meters over 0 dB light their top.
  Track colours come from a palette chosen to be told apart with colour-blindness.
- **Interface size:** Larger Interface and Smaller Interface (Ctrl+= and Ctrl+-) scale everything, from 75 % to
  200 %.
- Popups are solid, not frosted, in the high-contrast theme and where the system is set to reduce transparency.

## Motor

Toward WCAG 2.2's 2.5.8 Target Size (Minimum), 24 by 24 pixels, what is drawn small on purpose takes clicks round
it:

- a track's mute and solo switch takes clicks 24 pixels square where its row has room;
- a track's colour strip takes clicks 16 pixels into the header;
- the panels' dividers take drags 2 pixels into the cards beside them, while only the gap lights;
- the loop's lane and its edges in the rulers take clicks a little beyond what is drawn.

A drag is never the only way to set a value: knobs, faders and points take the arrow keys or a typed value.

## Benchmarks

The apps to measure against are REAPER with OSARA, and Ableton Live 12.

## Decisions

- Accessibility is built into the base controls, so that a new control gets a name, a role, a value and keyboard use
  without extra work, and a test fails when one lacks them.
- Showing a view moves the keyboard focus into it; otherwise a key would show a view that the keys can't reach.
- Views take the focus themselves rather than their first control, so that single-key shortcuts keep working until
  Tab is pressed.
- Contrast is a rule of the themes, checked by a test, not a matter of taste for each colour.
- Small things stay drawn small and take clicks beyond their edges, rather than every control growing to 24 pixels.

## Not built

- A session with a screen reader (Narrator or NVDA) by a person who relies on one: not yet done.
- Screen readers on Linux: Planned, after v1; it needs an AT-SPI back end that JUCE doesn't have.
- Screen readers on macOS: Planned, with the macOS port.
- A setting that reduces motion (the playhead's scrolling, fades): Planned.
- A 24-pixel target for every control: Planned; only the smallest targets were enlarged.

## In the code

- `src/model/commands/CommandRegistry.h`: every action as a command.
- `src/ui/input/KeyDispatcher.h`, `CommandBridge.h`, `Keymap.h`: the keys that run commands.
- `src/ui/shell/CommandPalette.h`: Search Commands.
- `src/ui/components/Controls.h`, `ParameterControls` in `src/ui/devices/`, `TypedField.h`: the base controls.
- `src/ui/theme/DesignTokens.h`: the themes' colours, and sizes such as `SwitchTint::trackHitSize`.
- `src/ui/shell/PanelLayout.h`: `PanelDivider::place()`.
- Tests: `tests/AppShellTests.cpp` ("A view its key shows gets the keyboard...", "Every control in the window has a
  name and a role for screen readers..."), `tests/DesignTokenTests.cpp` ("Every theme meets WCAG 2.2's contrast for
  text and controls").
