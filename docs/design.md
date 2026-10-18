# Phazite design

*Status: draft for review, October 2026.* This document describes the product Phazite is aiming for, and why. The plan for building it is in [roadmap.md](roadmap.md). **Decided** marks choices the project owner has made; **Open** marks questions that still need a decision ([section 25](#25-open-questions)). The research behind the design is summarized in [appendix A](#appendix-a-research-summary), with sources in [appendix B](#appendix-b-sources). How the code is structured is in [section 24](#24-code-structure).

## Contents

1. [Vision and scope](#1-vision-and-scope)
2. [Who Phazite is for](#2-who-phazite-is-for)
3. [Design principles](#3-design-principles)
4. [The window](#4-the-window)
5. [Building blocks](#5-building-blocks)
6. [The playlist](#6-the-playlist)
7. [The piano roll](#7-the-piano-roll)
8. [The mixer](#8-the-mixer)
9. [Devices, containers and presets](#9-devices-containers-and-presets)
10. [Automation and modulation](#10-automation-and-modulation)
11. [The browser](#11-the-browser)
12. [Commands, keys and help](#12-commands-keys-and-help)
13. [Getting started and song outlines](#13-getting-started-and-song-outlines)
14. [Chords](#14-chords)
15. [Sound generator](#15-sound-generator)
16. [AI and generative features](#16-ai-and-generative-features)
17. [Recording and Capture](#17-recording-and-capture)
18. [Projects, saving and export](#18-projects-saving-and-export)
19. [Plug-in hosting and stability](#19-plug-in-hosting-and-stability)
20. [Performance](#20-performance)
21. [Accessibility](#21-accessibility)
22. [Privacy, licences and trademarks](#22-privacy-licences-and-trademarks)
23. [Tracktion Engine: what it gives and what we build](#23-tracktion-engine-what-it-gives-and-what-we-build)
24. [Code structure](#24-code-structure)
25. [Open questions](#25-open-questions)
26. [Decision log](#26-decision-log)

## 1. Vision and scope

Phazite is an open-source (AGPL-3.0-or-later) digital audio workstation, built on Tracktion Engine and JUCE, for Windows first and then Linux and macOS. It aims to be:

- **Quick to start:** sound within seconds of opening it. Song outlines and chord progressions show newcomers what to do next.
- **Deep when needed:** professional routing, automation, modulation and mixing are one click away, never hidden behind a mode.
- **Fast and lean:** low CPU and memory use, in both the audio engine and the UI.
- **Smart in context:** AI and generative features that are useful *because* they run inside a project. They know its key, chords, sections and sounds.
- **Local and open:** no account, no cloud requirement, and projects that stay on your machine.

In one line: **FL Studio's speed without FL Studio's split.** Phazite keeps the playlist, piano roll and mixer workflow that electronic producers love, and drops the channel rack and unlinked mixer that they complain about most ([appendix A](#appendix-a-research-summary)).

**v1 scope (decided):**
- the playlist, piano roll and mixer;
- built-in instruments and effects for electronic music;
- automation and modulation;
- song outlines and chord progressions;
- chord suggestions and a drum sound generator;
- VST®3 hosting on Windows.

[Section 25](#25-open-questions) and the [roadmap](roadmap.md#after-v1) list what waits until after v1.

## 2. Who Phazite is for

| | Who | What they need |
|---|---|---|
| **Primary** | Electronic and beat producers coming from FL Studio or Ableton Live, mostly on Windows | Fast clip editing by hotkey, a great piano roll, free routing, automation on everything, many third-party plug-ins (such as Serum and OTT) |
| **Secondary** | Newcomers making their first tracks | Sound immediately, guidance on song structure and chords, plain language, nothing hidden |
| **Later** | Recording musicians and mix engineers | Audio recording, takes and comping, deeper metering, project interchange |

Why this audience:
- **Professionals:** surveys of professionals put Pro Tools first, with about 37% of music and 64% of post-production picks in Production Expert's 2025 survey. FL Studio gets about 1% there, but that audience is mostly engineers ([B1](#appendix-b-sources), [B2](#appendix-b-sources)).
- **Electronic producers:** an academic survey found that Live and FL Studio lead among amateur electronic producers, and Live and Pro Tools among professionals ([B3](#appendix-b-sources)). FL Studio users are the youngest and least formally trained, and 83% use Windows.

"Professional" for Phazite's audience therefore means routing, automation, mixing and plug-in depth, not film post-production.

## 3. Design principles

1. **One track, one channel (decided).** Every track is a playlist row, a device chain and a mixer channel, created together with the same name and color. There is no channel rack. Free routing stays available through sends, buses and sidechains. *Why:* FL Studio's split between channel rack, patterns, playlist and mixer is its most repeated complaint, and Image-Line itself is moving to a 1:1 "Track Mode" ([B4](#appendix-b-sources), [B5](#appendix-b-sources)).
2. **One window.** Panels dock around the playlist, any panel can move to a second monitor, and layouts are saved. No pile of floating windows.
3. **No skill-level modes.** There is no beginner/pro switch. Depth appears in place: in the inspector, in context menus, in the command palette. *Why:* Apple removed Logic's "Complete Features" switch in Logic 12 after years of confusion ([B6](#appendix-b-sources), [B7](#appendix-b-sources)).
4. **Every action is a command.** Each action has a name, a description and a default shortcut. It can be searched, rebound, chained into macros, and later called by scripts or an assistant.
5. **Opinionated defaults.** New projects make sound immediately and look finished. Customizing is optional, never required. *Why:* REAPER's blank first screen and Waveform's "eccentric" one are the most common complaints about each ([B8](#appendix-b-sources), [B9](#appendix-b-sources)).
6. **Help where you are.** A hint bar says what the mouse and modifier keys will do under the pointer, and hovering any control explains it.
7. **Nothing invisible.** Routing is drawn, linked clips are marked, automation and modulation show on the controls they move, and each track shows its CPU use.
8. **Plain names.** Track, clip, folder, section, device, send. No private vocabulary.
9. **Stable by default.** Autosave and crash recovery from the first release; a crashing plug-in should never cost the user their work ([section 19](#19-plug-in-hosting-and-stability)).
10. **Fast by default.** Strict real-time rules on the audio thread, a UI that does nothing when nothing changes, quick startup ([section 20](#20-performance)).
11. **Local, private, licence-clean.** No account, no personal data in project files, and only AGPL-compatible code, models and data ([section 22](#22-privacy-licences-and-trademarks)).
12. **Accessible.** Everything works from the keyboard, screen readers work on Windows and macOS, and color is never the only signal ([section 21](#21-accessibility)).

## 4. The window

```
+-----------------------------------------------------------------------+
| Play  Rec   140 BPM   4/4   CPU 12%        Ctrl+K: search any command |
+----------+--------------------------------------------+---------------+
| Browser  | [Playlist] [Mixer]      (the whole song)   | Inspector     |
| (Ctrl+F) | + Track  Intro    Build    Drop    (bars)  | (properties   |
| folders  | v Drums  (folder = bus)                    |  of what is   |
| tags     |     Kick    [] [] [] [] [] [] [] []        |  selected)    |
| plug-ins | v Synths                                   |               |
| samples  |     Lead    [riff ] [riff*] [riff*] *alias |               |
| presets  |       `- Cutoff lane   /\/\/\_/\           |               |
|          +--------------------------------------------+               |
|          | [Piano roll] [Devices]    (the selection)  |               |
+----------+--------------------------------------------+---------------+
| Hint bar: what the mouse and modifier keys do under the pointer       |
+-----------------------------------------------------------------------+
```

**The whole song in the centre, the selection below (the owner's decision, 2026-10-17).** The owner found that the editor dock mixed one clip's editor with every track's mixer, that the inspector showed only a copy of a mixer channel, and that effects needed a place to be edited in line, as notes are in the piano roll. Seven DAWs' layouts (Appendix A) showed that the bottom of the window is for the selection in all of them; four also let the mixer take that place (Bitwig, Studio Pro, Logic Pro, Cubase), while Ableton Live keeps it for the selection and gives the mixer a main view of its own, as Bitwig's Mix view does with its Device panel below; an in-line view of the selected track's devices is the clearest trend (Ableton's Device View and Bitwig's Device panel for years, Studio Pro 8's Channel Overview and Reason 14's Track Panel since 2025-26); and side inspectors show properties of the selection.
- **Top bar:** play, loop and record, tempo, time signature, CPU and disk meters, the master level, the computer keyboard and the command search (Ctrl+K). A key is a track's, so the piano roll shows it, not the top bar (2026-10-13).
- **Browser (left):** instruments, effects, plug-ins, presets, samples, generated sounds and projects ([section 11](#11-the-browser)).
- **Centre: the whole song,** in one of two views, chosen by the tabs at its top, so that no key needs remembering (the owner asked for as few "magic key presses" as possible; F5 and F9 do the same, as in FL Studio):
  - **Playlist:** the time bar (the song's sections over the bar numbers, with the button that adds a track beside it), then global lanes (chords, tempo, markers), then the tracks, with the sections drawn faintly behind them ([section 6](#6-the-playlist));
  - **Mixer:** every channel side by side ([section 8](#8-the-mixer)).
- **Editor dock (bottom): the selection, in detail,** in tabs:
  - **The selected clip's editor** (F7), its tab named for what it shows: the **piano roll** for MIDI clips, and the **audio clip editor** for audio clips (2026-10-17, [section 7](#the-audio-clip-editor)). It follows the selection, as Studio Pro's Edit view opens the Audio Editor for audio events and the Note Editor for instrument parts ([B81](#appendix-b-sources)): MIDI clips selected show in the piano roll, audio clips in the audio clip editor, and a selected track in the editor for what it holds; double-clicking a clip shows it here;
  - **Devices** (F6): the selected track's devices in a row, each shown in line ([section 9](#9-devices-containers-and-presets)).
  Automation has no tab: it is edited in its lanes, under its tracks in the playlist (2026-10-17, [section 10](#10-automation-and-modulation)).
- **Inspector (right): the properties of what is selected,** as Bitwig's, Studio Pro's, Logic Pro's and Cubase's inspectors show them, laid out to use its width: a track's name, color, volume, pan, where its sound goes and its sends; a clip's name, place, length, mute, fades and whether it is an alias; a section's name, color, place, length, energy and tip; and, with nothing selected, the song's tempo, time signature and length. A track's also shows its instrument, its effects and its automation mode, each a click from the Devices view or its lanes. It isn't a copy of a mixer channel, which the mixer already shows.
- **Hint bar (bottom edge):** what clicking, dragging and the modifier keys do under the pointer.

**Panels and layouts:**
- Panels can be resized, collapsed and detached to another monitor.
- A layout saves the arrangement of panels. Three ship by default, and they are layouts, not modes:
  - *Beat:* playlist and piano roll;
  - *Mix:* the mixer, the selected track's devices and the inspector;
  - *Full.*
- Function keys follow FL Studio where that makes sense: F5 playlist, F6 devices, F7 piano roll, F8 plug-in picker, F9 mixer. The browser shows and hides with Ctrl+Alt+B (Ableton Live's key; Alt+F8 in FL Studio's keymap).

### Design language

Phazite's look should be as clean and consistent as its workflow. Its design language has five parts. Typography is decided; [roadmap](roadmap.md) step 0.6 defines the rest as reusable design tokens (named colors, sizes and spacings that every component uses).

**Typography (decided): Inter**, an open-source typeface designed for screens ([B35](#appendix-b-sources)).
- **Why bundle a font:** text looks the same and takes the same space on Windows, Linux and macOS, so labels fit everywhere. The DAWs that could be checked do this:
  - Ableton ships its own "AbletonSans" fonts with Live ([B36](#appendix-b-sources));
  - REAPER's default theme switched to Inter "as default font on all platforms" in version 7.70 ([B37](#appendix-b-sources)).
- **Licence:** Inter uses the SIL Open Font License 1.1.
  - The OFL explicitly allows bundling with any software, and the software doesn't have to adopt the font's licence ([B38](#appendix-b-sources)).
  - The GNU project lists it as a free license for fonts ([B39](#appendix-b-sources)).
  - Phazite ships the font files next to the app rather than compiling them in, together with their licence text and copyright notice.
- **Numbers:** tempo, time, levels and meter readouts use Inter's fixed-width (tabular) digits, so values don't jitter as they change. JUCE 8 can switch this OpenType feature on (`FontOptions::withFeatureEnabled`).
- **Scale:** a small set of sizes and three weights (regular, medium and semibold), scaled with the UI.
- **Fallback:** if the font files are missing, Phazite uses the system font.

**The rest of the design language:**
- **Color:**
  - semantic tokens (background, surface, text, accent, warning) instead of hard-coded colors;
  - a dark theme first, then light and high-contrast themes;
  - a track-color palette that stays distinguishable for color-blind users.
- **Spacing and sizing:** a 4-pixel grid, standard control heights and consistent padding.
- **Icons (decided in step 0.6): [Lucide](https://lucide.dev)** ([B42](#appendix-b-sources)), line icons under the ISC licence (those taken from Feather: MIT), drawn in the theme's colors, plus custom icons drawn to match. Phazite copies in only the icons it uses (`resources/icons/lucide/`).
- **Motion:** short, purposeful animation (around 100–150 ms). None in places that redraw constantly, and the system's reduced-motion setting is respected.
- **Tokens a designer can edit (decided 2026-10-14):** the tokens move out of C++ into a file in the Design Tokens Community Group's format (DTCG 2025.10, [B56](#appendix-b-sources)), which a designer edits directly or exports from Figma, Penpot or Tokens Studio, and which the build turns into C++. A gallery app shows every control and view in all three themes, so a change can be seen without opening a project. A declarative layout tool (JIVE or foleys_gui_magic) comes only if dialogs become a bottleneck; the playlist, piano roll and meters stay painted in code. This is phase A4 of the [roadmap's architecture track](roadmap.md#architecture-track).

## 5. Building blocks

| Concept | What it is | Tracktion Engine |
|---|---|---|
| Project | A folder with the project file, its audio and its generated sounds | `Edit`, plus folder-based `Project` |
| Track | Playlist row, device chain and mixer channel in one | `AudioTrack`, one type that holds audio, MIDI and step clips |
| Folder | A collapsible, colored group of tracks, and a bus that mixes them (its switch can make it plain) | `FolderTrack`, submix or plain |
| Clip | Audio, MIDI or automation on a track | `WaveAudioClip`, `MidiClip`, clip automation curves |
| Alias | A linked copy of a clip: edit one and all change; "Make unique" breaks the link | Clips sharing a `linkID` |
| Section | A named, colored part of the song (Intro, Drop) that spans all tracks | `ArrangerTrack` and `ArrangerClip` (labels only; Phazite implements the section edits) |
| Chord lane | The song's chord progression | `ChordTrack` and `ChordClip` (diatonic only, see [section 14](#14-chords)) |
| Key | The project key and scale, which can change over time | `PitchSequence` |
| Device | An instrument or effect, built-in or plug-in | `Plugin`, `ExternalPlugin` |
| Container | A group of devices with macro knobs, saved as one preset | `RackType` with `MacroParameter`s |
| Send and return | A level-controlled feed to a return track | `AuxSendPlugin`, `AuxReturnPlugin` |
| Modulator | An LFO, envelope follower, step sequence, random source, breakpoint envelope or note follower driving any parameter | `Modifier` types |
| Macro | One knob driving several parameters | `MacroParameter` |

**Rules:**
- **Dropping creates tracks.** Drop an instrument, a sample or a MIDI file on empty playlist space and a new track appears. It is named after what was dropped and colored by type: for example, drums in one color family and synths in another.
- **Audio and notes don't share a track in v1.** In Tracktion, an instrument with no audio input silences its track's audio clips. So v1 puts dropped audio on a new track next to the instrument's track. Mixing both on one row, like Bitwig's hybrid tracks, is a later option.
- **Drums without a channel rack.** Two ways are first-class, and neither uses step buttons:
  - one-shots placed straight onto their own tracks (a kick track, a snare track);
  - a drum-sampler track whose clips are edited in the piano roll's drum mode.
- **Reuse without patterns.** Aliases do the job of FL Studio's pattern reuse. "Duplicate section as aliases" repeats a whole part of the song.
- **Clip effects.** Up to five devices can sit on a single clip, which is handy for one-shots without a track of their own (like Ardour's Region FX; Tracktion supports clip plug-ins).

## 6. The playlist

**The Smart Tool is the default pointer (decided).** Over a clip, where the pointer is decides what a drag does, as with Pro Tools' Smart Tool ([B10](#appendix-b-sources)):

| Where on a clip | Dragging does |
|---|---|
| Left or right edge | Trims the clip |
| Top-left or top-right corner of an audio clip | Draws a fade-in or fade-out |
| Upper half | Selects a time range (a clip on a short track has no upper half: all of it moves) |
| Lower half | Moves the clip; with a modifier key it copies or makes an alias (the keymap sets which key) |
| Bottom of the boundary between two touching clips | Draws a crossfade |

**On empty space:**
- dragging selects a time range across tracks, like REAPER's razor edits;
- double-clicking creates an empty MIDI clip on an instrument track, and under the last track adds a track;
- with the FL Studio keymap, a single click places the last-used clip, as FL's draw tool does.

The final mapping will be settled in the usability tests ([section 12](#12-commands-keys-and-help)).

**Single-key tools** use FL Studio's letters:

| Key | Tool |
|---|---|
| P | Draw: a click places the last-used clip |
| B | Paint: drag to repeat it |
| C | Slice |
| S | Slip: moves the content inside a clip |
| T | Mute |
| D | Delete |
| E | Select |
| Z | Zoom |

Esc returns to the Smart Tool. The current tool and its key show on the toolbar and in the hint bar.

- **Digits too (2026-10-13):** in Phazite's keymap, 1 to 9 also pick the tools, in the toolbar's order. They never play notes, so they work while the computer keyboard does.
- **Tool keys spring back (2026-10-13),** as in Bitwig and Ableton Live: a tap of a tool's key switches to the tool, but holding the key while using the mouse keeps the tool only until the key is let go (or until a drag still going on then ends).
- **A held key repeats only the commands that take a step** (nudging, zooming, moving through tracks, undo). Choosing the tool in use again changes nothing, so holding a tool's key never ends a drag.

**Right-click** opens a context menu by default. With the FL Studio keymap it deletes clips, as FL users expect, and a click on empty space places the last clip used, as FL's Draw tool does; Ctrl+drag selects a time range there instead (2026-10-13). Both are keymap settings (`rightClickDeletes`, `clickPlacesClips`). In Phazite's keymap a click on empty space selects, so a click never adds a clip by accident; the piano roll adds a note with a click and deletes it with a right-click in every keymap.

**Editing commands:**
- split at the playhead or pointer;
- duplicate, make alias, make unique. Duplicate with a time range selected copies what lies in it on its tracks right after it, cut at the range's edges as Split cuts (the clips in the range aren't cut there); each piece replaces what it lands on, as REAPER's razor edits trim what is behind them, and the range moves to the copy, so Duplicate again repeats it; Undo puts the range back, as REAPER's undo puts razor edit areas back (2026-10-14, [open question 19](#25-open-questions));
- delete, with or without closing the gap (ripple);
- insert or remove time across all tracks;
- nudge, and quantize clip positions;
- reverse, normalize, clip gain;
- fades and crossfades with curve shapes;
- bounce clips to audio.

**Tracks:**
- **add one** with the "+ Track" button over the track headers (below the selected track), the row after the last track (at the end; right-click it for a folder or a return track), a double-click under the last track, or the Add menu, as Logic, Cubase and Studio One place it (2026-10-13);
- color and name (automatic from the content, editable), height. The track's header and every clip on it are in its color, strongly enough to tell tracks apart at a glance (2026-10-13); the row behind the clips isn't tinted, so the clips stand out, and a clip can't show another color than its track's (the owner, 2026-10-16);
- **move** a track by the handle at the left of its header (2026-10-13). The rest of the header selects, renames and resizes, so a click never moves a track by accident;
- **collapse** a track to a thin row that shows its name: the arrow in its header, U, or dragging its bottom edge up past the smallest height (2026-10-13). A collapsed folder hides its tracks instead;
- **pin** tracks so they stay visible while scrolling, like Pro Tools' Track Pin;
- a **focus mode** that collapses all other tracks;
- mute, solo and record-arm;
- a CPU readout.

**Sections** (redesigned 2026-10-13) don't take a row of their own:
- they are a thin band in the time bar, above the bar numbers: each section's name over its time, with the energy curve faintly behind;
- behind the tracks they show as ghosts: a faint wash of each one's color and a line where it starts, so the song's shape stays in view;
- drag a section to move it, with everything in it, across all tracks (where it would go shows across the tracks too); drag its right edge to resize it; double-click to add or rename one;
- duplicate, duplicate as aliases, delete, rename, recolor;
- right-click the band or the bar numbers to add a section there or apply a song outline ([section 13](#13-getting-started-and-song-outlines)). The Add menu has both too; there is no Song menu.

**The loop (2026-10-13):** playback loops the song, as FL Studio's song mode does: from its start to the end of the bar where its last clip ends, so the loop grows as clips go later. Sections don't count, since an outline can run longer than what is in it so far. That is also where the song ends for Go to End (End); a song without clips ends where it starts, though it loops its first bar (2026-10-14). Loop Selection (Ctrl+L, as in Ableton Live) loops the time range selected in the playlist instead, or the whole song when none is selected; Loop Section loops the selected sections, and Loop the Song goes back to following the song. With FL Studio's keymap, as in FL Studio, a selected time range loops while it is selected, and the song again after. The Loop button in the top bar (L) turns looping on or off, and the rulers show the loop as a band. Playback that doesn't loop stops at the song's end and goes back to where it started, rather than playing on into silence, as Logic stops at its end marker. Where the loop is isn't an undo step, as in other DAWs. **The rulers' loop strip (2026-10-16):** the top strip of the playlist's and the piano roll's rulers is for the loop, as the top of Logic Pro's ruler is its cycle area and Cubase's ruler sets its loop: dragging along it loops that part of the song (Ctrl+drag starts a new loop anywhere), dragging the loop's ends or middle changes or moves it, a click turns looping off and on, and a double-click loops the whole song; lower down, the ruler moves the playhead.

**Grid, zoom and scrolling:**
- The grid adapts to the zoom level, with snap on by default; hold a modifier to bypass it. Triplets are available.
- Scrolling is smooth in both directions, never jumping a whole track at a time (an Ardour complaint).
- Dragging anything (clips, edges, ranges, rectangles, tracks, sections, notes, the playhead) near or past an edge scrolls the view along, faster the further out the pointer is (2026-10-13).
- Zoom to selection.

**Clip picker:** a searchable list of every clip in the project, like FL Studio's Picker panel. Drag a clip into the playlist, or pick it as the clip the draw tool places.

## 7. The piano roll

This is the editor the target users spend most time in, and FL Studio's is its most-praised feature: "designed… like a paint program" ([B11](#appendix-b-sources)). Gaps here are what sent FL Studio users away from Bitwig and Reason ([appendix A](#appendix-a-research-summary)).

- **Time counts from the clip (2026-10-13),** as in FL Studio and Live: bar 1 is where the clip's content starts, and a clip opens there. The grid and snapping count from it too; the header says where the clip is in the song, and the playhead shows when the clip plays. With several clips it counts from the first. Later, a switch to song time like Bitwig 6's could edit a whole track.
- **The playhead follows the notes, not only the clip (2026-10-16):** while an alias of a clip shown (a linked copy, which shares its notes) plays elsewhere in the song, the playhead shows, fainter, which of the notes are playing, as FL Studio's piano roll follows its pattern wherever the playlist plays it. Edits change every alias, so the playhead shows every time they play. The view doesn't turn pages while the song plays, so the notes being edited stay in view; View > Follow Playback turns that on.
- **What it shows:** the MIDI clips selected in the playlist, or, when a track is selected, its clip at the playhead or the nearest one. A track without clips shows from the bar the playhead is in, and the first note drawn starts a clip there.
- **Clips grow with their notes, into empty space only (2026-10-14):** a note drawn, moved, lengthened or duplicated past its clip's end makes the clip end at the end of that bar, as FL Studio's patterns grow, but never past where the next clip on the track starts; a note that doesn't fit stays in the clip without playing, as outside a trimmed clip, until the clip's end is dragged out. A looped clip doesn't grow.
- **Mouse model from FL Studio:**
  - left-click adds a note and right-click deletes it;
  - new notes reuse the last length and velocity;
  - dragging an edge resizes a note and dragging its body moves it;
  - the mouse wheel scrolls and zooms.

  A menu button and Shift+right-click open the context menu.
- **Tools:** draw, paint (drag to lay notes along the grid, like Ardour's note brushing), slice, select, mute and delete, on the same keys as in the playlist.
- **Ghost notes:** notes from other tracks, chosen per track, show faintly behind the clip being edited. Click one to jump to its track.
- **Key and chords:**
  - **a key is a track's (2026-10-13),** chosen in the piano roll's toolbar, and off until chosen: then the rows are a piano's, black keys darker. With a key, the rows outside it are shaded and notes can snap to it. A new track gets the key chosen last in the song, and one command gives every track the same key. (Tracktion's own key is one for the whole song, so this is Phazite's.) The top bar shows no key;
  - the tones of the current chord on the chord lane are highlighted;
  - transposing in steps of the key needs one; without, it is in semitones.

  Out-of-scale shading should be subtle and explained, because an Ardour user found its version confusing.
- **Chord stamps:** pick a chord type and click to place it. When the song has a chord lane, there is also "stamp the current chord".
- **Transforms with live preview:** quantize (with strength and swing), legato, chop, strum, arpeggiate, flam, humanize, transpose (diatonic or chromatic), velocity scaling, reverse and flip.
  - Each shows a preview before it is applied.
  - Its settings stay editable until you commit, which avoids Live 12's gotcha where arpeggiated notes become permanent once you select another clip.
  - Each has a hotkey.
- **Several clips at once:** edit the notes of several selected clips together, each in its track's color.
- **Lanes under the notes:** velocity is always shown. CC, pitch-bend and per-note expression lanes appear on demand.
- **Drum mode:** for drum-sampler tracks, rows show pad names instead of piano keys and notes are drawn as hits without length. Rows can be reordered or hidden. It comes on by itself with such tracks, and the grid's menu and the command palette turn it on or off; it has no button (2026-10-13).
- **Slides:** pitch glides between notes are sent as MPE note expression to plug-ins that support it. FL Studio's slide notes only work with its own instruments.
- **Capture:** press Capture after playing and the last phrase becomes a clip ([section 17](#17-recording-and-capture)).

### The audio clip editor

**Decided by Claude for the owner to review (2026-10-17, roadmap step 2.6).** The editor dock's first tab shows it in place of the piano roll while the selection is an audio clip, or a track that holds audio, and the tab is then called "Audio clip". It edits one clip at a time: the first audio clip selected. What the reference DAWs show of an audio clip ([B76](#appendix-b-sources) to [B80](#appendix-b-sources), Appendix A): Ableton Live's Clip View has a gain slider in decibels, a pitch control in semitones with a second field in cents, Reverse (which makes a reversed copy of the sample) and Warp with the sample's tempo, over the sample with its start and end markers; Logic Pro's region parameters are gain, transpose and fine tune, Reverse, and Follow Tempo; Studio Pro's audio events have a file tempo that they stretch from to follow the song's; Bitwig's have a stretch mode, their original tempo, fades, and gain and pitch; FL Studio's sampler has pitch, time and stretch modes, reverse and fades.
- **The song's bars, unlike the piano roll:** the ruler counts the song's bars, as the playlist's does, and the clip shows where it is in the song, so that trims and fades line up with the song's grid: a recording seldom starts on a bar of its own, as a pattern does. Its grid is the editor dock's, which the piano roll shares (the project keeps it); Studio Pro's editor also snaps apart from its arrangement ([B81](#appendix-b-sources)).
- **The audio, large:** all of the file, where it plays in the song: the clip's part in its track's color, and the audio its edges trim off shaded, so that the clip can be trimmed back out to it. Gain shows in the waveform's height, as in the playlist.
- **One way to edit, with visible handles and no keys:** the clip's edges, each with a grip, trim it; square handles at its top corners fade it in and out, and a double-click on one takes its fade away; a drag on the audio moves it inside the clip, as the Slip tool does in the playlist. Alt places freely, as everywhere, and there are no tools to choose. The fades' shape is on a right-click, as on the clip in the playlist, and in the inspector.
- **How the clip plays its audio, down the left,** as Ableton's Clip View and Logic's and Bitwig's inspectors list it: fields to drag, scroll or type in, as the top bar's tempo is, each change one undo step and each drag one:
  - **Gain,** from -60 to +24 dB;
  - **Pitch,** up to 48 semitones up or down, and **Fine,** 50 cents either way, as Ableton's and Logic's pitch is in semitones and cents; the clip keeps its length;
  - **Reverse:** the same part of the audio plays, backwards, where the clip is;
  - **Follow tempo,** with the clip's **Tempo** (Ableton's Warp and its sample's tempo, Studio Pro's file tempo, Bitwig's tempo): following, the clip stretches with the song's tempo, keeping its pitch, from the tempo its file gives, or else the song's where the clip starts, so that nothing changes until either tempo does. Setting the tempo a vocal was sung at puts it in time: the clip keeps its start and the audio in it, and gets shorter or longer.
- **Reverse and Follow Tempo are commands too,** for the selected audio clips: on a clip's right-click menu in the playlist, and in the command palette.
- **Stretched, pitched and reversed audio is prepared in the background:** Tracktion makes a copy of the file to play, so the clip is silent for a moment after such a change (well under a second for a loop, longer for a long recording), and the editor says so meanwhile.
- **Not yet:** a choice of stretch algorithms (Phazite uses one, SoundTouch for now, [section 23](#23-tracktion-engine-what-it-gives-and-what-we-build)), warp markers, normalizing, gain changes inside a clip, and formants.

## 8. The mixer

**Redesigned 2026-10-13.** The owner gave a track with notes an effect instead of an instrument, heard nothing, and found "+ Device", returns and routing confusing. Research into the mixers people praise (summed up in [Appendix A](#appendix-a-research-summary)) showed the cause: Phazite's single device list followed Waveform's model, which Tracktion's own DAW uses and where instruments and effects share one list. The DAWs that don't trip people up (Logic Pro, Reason, Live, Studio One) give the instrument a place of its own above the effects.

- **Where it is (2026-10-17):** the centre's second view, beside the playlist ([section 4](#4-the-window)), with the editor dock below it, as Bitwig's Mix view has its Device panel and Ableton's Session view its Device View. A click on a device's name in a strip shows the device in the dock's Devices view; a plug-in's own window opens too, as FL Studio's mixer opens it.
- **Channels:** one per track, in playlist order and colors. Folders appear as their own channels, with their tracks after them. The return tracks come last, together next to the master as in Ableton Live, set apart by a gap.
- **A strip reads top to bottom as the sound goes,** as in Logic Pro and Reason's Track Panel:
  - **the name**, and what the channel is and what feeds it: "Bus: mixes its tracks", or "Return, from Lead, Pad";
  - **the instrument** (tracks only): the device that turns the track's notes into sound, in a slot of its own above the effects. A track with notes but no instrument asks for one ("Choose instrument", tinted), and so does the piano roll's toolbar. Its menu lists instruments only;
  - **effects**, which change the sound, each with an on/off switch and two key controls, so plug-in windows rarely need opening (like Logic's Smart Controls and Studio Pro's Channel Overview). "+ Effect" lists effects only; on a track whose notes have no instrument, it offers the instrument first;
  - **sends:** a knob for every return the track can send to, off until first turned, which makes the send after the fader in the same undo step (question 13, built in step 2.9), with a Pre/Post switch once it is there. "+ Send" makes a new return, with an effect or empty;
  - **where the sound goes:** "Out: Master", or the bus it is in; a click offers the master, the buses (choosing one moves the track into that folder, as the playlist then shows it) and a new bus from the selected tracks (question 12, built in step 2.9);
  - pan, mute and solo, fader and meter, and along the bottom a thin bar of how much of each block's time its devices take, as Tracktion measures each device, amber above half (step 2.9; record-arm later).
- **Returns** hold effects that several tracks share. A new return with an effect comes in one step, as in Studio One: the return, named after the effect, and a send to it from the track. A built-in reverb on it gives only its wet sound. Returns are solo-safe, as Studio One's effect channels are, so soloing a track keeps its reverb audible. Tracktion has no pre/post switch for sends, so Phazite places the send before or after the fader in the device chain.
- **Folders are buses (2026-10-16, [question 11](#25-open-questions)):** every new folder mixes its tracks through its own effects, fader and meter, as group tracks do in Ableton Live and Bitwig and folders in REAPER. Its Bus switch can make it plain, so that its fader only scales its tracks' volumes (a VCA); a plain folder becomes a bus again when it gets an effect, and turns plain when its last one goes. Folders in projects made before keep what they were.
- **Routing shows on selection:** selecting a channel outlines the channels it feeds (the returns it sends to, the bus it is in) and the ones that feed it. This shows routing without cables, whose views (REAPER's, FL Studio's) get mixed reviews.
- **Mute and solo (2026-10-13):** separate M and S buttons, and a solo never changes the mute switches. FL Studio's solo turns the others' mute switches off and back on, which loses earlier mutes: a complaint on its forum since 2007. As FL users expect, right-clicking M solos the track alone, and does so again to unsolo it; Ctrl+click on S does the same. A track that other tracks' solos silence shows its M half lit, as Logic flashes it. Soloing a folder solos its tracks, and soloing a track in a bus keeps the bus audible.
- **Sidechain:** any device with a sidechain input, Phazite's compressor or a plug-in, has a Sidechain submenu in its menu (the mixer's slots and the Devices view) that lists the tracks it can listen to, and the compressor's editor the same choice (built in step 2.9). The source must be a track, not a folder bus (an engine limit).
- **Routing view (built in step 2.9, decided by Claude for the owner to review):** the mixer's **Routing** button shows it in place of the strips: every channel as a box, in the mixer's order with the master last, with jacks along its bottom (Out; In for a bus, a return or the master; SC for each device with a sidechain), and the wires of the selected or hovered channel as cables under the boxes, like Reason's back panel: where its sound goes in the text color, sends in the accent color, sidechains in the modulation color. Dragging from an Out jack onto a bus or the master routes the channel there, onto a return sends to it, and onto an SC jack makes that device listen to it; right-clicking a box offers to take each of its wires away. Wires show only for the selected or hovered channel, so the view never turns to spaghetti.
- **Master:** its effects, fader and meter, ending with a loudness meter (LUFS and true peak), built on Tracktion's real-time-safe EBU R128 meter.
- **One output per track:** FL Studio's "route one insert to several inserts" is done with sends.
- **Open:** see [section 25](#25-open-questions), questions 11 to 15.

## 9. Devices, containers and presets

**Device chain:**
- each track has an instrument first, then its effects;
- drag devices to reorder them; bypass, dry/wet and A/B comparison for each.

**The Devices view (the owner's decision, 2026-10-17):** the editor dock's second tab (F6) shows the selected track's devices in a row, left to right as the sound goes through them, as Ableton Live's Device View, Bitwig's Device panel, Studio Pro 8's Channel Overview and Reason 14's Track Panel do ([section 4](#4-the-window)):
- **first, the track's modulators and macros,** where their editors have room next to the knobs they are dragged onto. The mixer's strips no longer show them;
- **then its instrument and its effects,** each a panel as tall as the dock, with its name, an on/off switch and its mix in a header, and "+ Effect" after them;
- **Phazite's own devices show their whole editors there** (the EQ's curve, the compressor's graph, the drum sampler's pads), and a container shows its macros and its devices, nested. They have no windows of their own any more: the dock moves to a second monitor instead (the owner chose one place);
- **a plug-in, or one of Tracktion's built-in devices, shows a knob for each of its key parameters and a button that opens its own window,** since none of the DAWs researched shows a plug-in's own interface in line: Studio Pro shows eight chosen parameters, Ableton a set chosen with Configure, Bitwig a list of them all;
- **which track:** the one selected, or, with none, the one live MIDI plays, as the inspector chose its channel before; a click on a device's name in the mixer shows that device's channel, the master's too.

**Mix (2026-10-13):** each effect has a Mix knob, first among its knobs in the strip: how much of what it puts out is its effect, from 0 (only the sound coming in) to 100 % (only the effect). The owner asked for it, from FL Studio. Research found it isn't standard: FL Studio's mixer slots and REAPER's effect windows have one for every plug-in, and Tracktion has one in its engine; Bitwig and Ableton Live put one on their own devices and blend a plug-in through a container or rack; Logic Pro, Cubase, Pro Tools and Studio One leave it to the plug-in's own control or to sends. It crossfades the dry and wet levels Tracktion gives every plug-in, linearly (Ableton's "equal-loudness" option could come later for reverbs and delays). Known limits, until Phazite's own mix replaces it (latency-compensated and ramped, as Tracktion's rack wet/dry is): the dry sound isn't delayed to match an effect's latency, so the knob's tooltip warns on effects with latency, as FL Studio's manual does; and a change applies per audio block, so a fast move can click. Keep effects on returns at 100 %. Phazite's own effects (step 1.6) have that better mix already: a `mix` parameter of their own, which their plug-in applies with the dry sound delayed to match the effect and the change ramped across each block.

**Phazite's own devices (step 1.6, 2026-10-15):** each is DSP without Tracktion (`src/devices/`) in a thin Tracktion plug-in (`src/model/devices/internal/DevicePlugin.h`), with an editor that uses only the model's handles (`src/ui/devices/`), which the Devices view shows (in its own window until 2026-10-17); until a device has an editor of its own, it shows a knob per parameter, each in its unit. Their parameters move smoothly, so turning a knob doesn't click; each passes a guarded real-time audit scenario. Projects that hold one are format version 2.
- **EQ:** eight bands, each a low cut, low shelf, bell, notch, high shelf or high cut (cuts 12 to 48 dB per octave, their Q a resonant bump), then an output and the mix. A new EQ changes nothing: its cuts are off and the other bands flat (defaults, proposed: a low cut at 30 Hz, a low shelf at 100 Hz, bells at 250, 800, 2,500 and 6,000 Hz, a high shelf at 10 kHz and a high cut at 18 kHz). Its editor draws its curve over an analyzer of what it plays: drag a band's dot to move its frequency and gain as one undo step, the wheel for its Q, a double-click to turn it on or off, a right-click for its shape; below, the selected band's controls, which the keyboard and screen readers reach. In a mixer slot its key controls are the low and high shelves' gains, a quick tilt.
- **Compressor:** threshold, ratio, attack, release, a soft knee and makeup gain, a low cut on what it listens to (so bass notes don't pump it), and the mix for parallel compression (defaults, proposed: -18 dB, 4:1, 10 ms, 120 ms, a 6 dB knee, no makeup, the low cut at 20 Hz, 100 %). It turns both channels down alike, by the louder one's peak, so the stereo image holds, and times its gain in decibels (Giannoulis, Massberg and Reiss, 2012), so attack and release are time constants: after the attack time it has gone 63 % of the way. Its editor draws its curve (level in against level out, with the makeup) with a dot riding it at the level the compressor hears, a meter of how far it turns the sound down, a knob for each control and the sidechain's source; dragging across the curve moves the threshold, as one undo step. In a mixer slot its key controls are the threshold and ratio.
- **Limiter:** the master's last device: a gain that raises the sound, a ceiling it never goes over, a release, and the mix (defaults, proposed: no gain, a ceiling of -1 dB, which streaming services' loudness rules ask for, and 100 ms). It looks 2 ms ahead, so it turns the sound down in a smooth ramp before a peak arrives rather than clipping it, and finds the peaks between samples (true peaks) with a 4x interpolation, so that what a converter or a lossy encoder makes of it stays under the ceiling too; its samples never go over it. The look-ahead delays the sound by 99 samples at 48 kHz, which Tracktion makes up for on the other tracks. Its editor has meters of the loudest sound coming in and going out, with the ceiling marked, and of how far it turns the sound down. In a mixer slot its key controls are the gain and ceiling.
- **Reverb:** a room or hall around the sound: size, decay (the time the bass takes to fade by 60 dB), pre-delay, damping (past which the treble fades sooner), a low cut on the tail, width, and the mix (defaults, proposed: 50 % size, 2 s, 10 ms, 6 kHz, 100 Hz, full width, and 25 % mix on a track; a return's reverb is all effect). It is a feedback delay network: after the pre-delay and four allpass filters that smear the attack (from Dattorro's plate, 1997), eight delay lines feed each other through an energy-keeping Householder matrix. Each line's length moves slowly, at a rate of its own, so the tail doesn't ring like metal, from a fixed seed, so a song renders the same every time. Its editor draws how the bass and the treble fade after the pre-delay. In a mixer slot its key controls are the decay and size.
- **Synth:** subtractive, with 16 voices. Each mixes two band-limited oscillators (sine, triangle, saw or square with a pulse width; the second tuned in semitones and detuned in cents), a sine an octave down and noise, through a resonant low-, band- or high-pass filter with an envelope of its own and key tracking, then an amp envelope; one LFO moves the pitch, the cutoff or the level, and the mod wheel adds to it. Mono mode plays one voice legato, gliding between the keys held and back to the one still held when a key lets go, as a bassline does; glide works in poly mode too, from the last note. Velocity counts squared (proposed: 60 % of the level depends on it), pitch bend has a range (2 semitones), and the sustain pedal holds notes. When it runs out of voices it takes the one ringing out longest, then the oldest. **Unison (step 2.10):** each oscillator plays up to seven times at once, detuned against each other by up to a semitone (25 cents at first) and spread across the stereo field (80 % at first), at a level that keeps the voice as loud, for the wide, thick sound of a "supersaw"; one voice, the default, plays as the synth did before, so older projects sound the same. Its noise and its unison's starting phases come from fixed seeds, so it renders the same every time. Its editor is a panel of its controls in the order the sound goes through them (`src/ui/devices/DevicePanel.h`), and in a mixer slot its key controls are the cutoff and resonance.
- **Drum Sampler:** 16 pads on the notes from 36 up, General MIDI's drums, laid out as FL Studio's FPC and an MPC lay them: the first at the bottom left. Each plays its sound once, whole, however short the note (a one-shot), at its level, pan and tuning, with an attack that softens its start and a length that cuts it short with a 10 ms fade (at its longest, 10 s, the whole sound). Pads in the same choke group stop each other; the closed, pedal and open hats start in group 1. A new sampler plays a kit that Phazite synthesizes from the same code as the synthesized drums in Phazite Sounds (`src/devices/drums/DrumSynthesis.h`), so it needs no files; a sound dropped on a pad from the browser or Explorer, or picked with Load..., replaces the pad's, and the project plays its own copy, in its Audio folder (`src/model/devices/internal/DrumKit.h`); Reset puts the kit's back. The sampler swaps a sound in while it plays without the audio thread waiting or freeing memory: a voice of the old sound stops at the next block, and the old sound is freed two blocks later. Clicking a pad, or Space or Enter on it, plays it and shows its controls; pads light up as they play. The piano roll's drum rows are its pads, named after what they play. In a mixer slot its key controls are the kick's and snare's levels.
- **The devices of step 2.10 (2026-10-18, decided by Claude for the owner to review):** each a panel of its controls in the order the sound goes through them, with switches for what is on or off, until it gets an editor with a graph of its own; each passes DSP tests and a guarded real-time audit scenario, and waits for A/B listening against reference tools. Projects that hold any of them, or the synth's unison, are format version 8.
  - **Filter:** a resonant low-pass, high-pass, band-pass or notch at 12 or 24 dB per octave (the two sections of a fourth-order Butterworth, the second carrying the resonance), with a drive that saturates what goes in. Its cutoff glides in octaves over about 10 ms, so lanes, modulators and knobs sweep it smoothly. A new filter changes little: a low-pass open all the way (20 kHz), at 24 dB and 10 % resonance. In a mixer slot: cutoff and resonance.
  - **Multiband:** in the style of Xfer's OTT: three bands split by Linkwitz-Riley crossovers (90 Hz and 2.5 kHz at first) that sum back flat, each turned down hard above -30 dB (10:1, a 6 dB knee) and brought up 3:1 below -40 dB (by 18 dB at most, fading out toward silence so noise isn't raised), both channels alike. Depth (50 %) sets how much of that happens, time (50 %) how fast (a tenth to ten times each band's attack and release, the low band slowest), upward and downward how much of each (100 %), and each band has a gain, with input and output gains. In a mixer slot: depth and time.
  - **Maximizer:** in the style of FL Studio's Maximus: a gain pushes the sound into three bands (120 Hz and 5 kHz at first), each squeezed by a compressor of its own (above -12 dB, from 1:1 to 10:1, 30 % at first) and set to a level, then Phazite's limiter keeps every sample and the peaks between them under the ceiling (-1 dB) with its release (100 ms). With no gain or squeeze it is the limiter alone. In a mixer slot: gain and ceiling.
  - **Saturator:** drives the sound (6 dB at first) into one of five curves: soft (like tanh), tape (gentler), tube (lopsided, adding even harmonics, its DC filtered out), hard (clipping) or fold (peaks folded back); auto gain (on at first) takes the drive back off so quiet parts keep their level; a tone tilts it ±6 dB around 1 kHz; then an output gain. It runs at twice the sample rate through half-band filters, so the harmonics it adds don't alias. In a mixer slot: drive and tone.
  - **Delay:** echoes on a note value at the song's tempo (a dotted eighth at first) or in milliseconds, with feedback (35 %) that saturates softly so it never runs away, each echo darker and thinner through a low cut (100 Hz) and a high cut (8 kHz), and ping-pong, which bounces them between the sides. A change of time glides over about 50 ms, as a tape's would. It puts out the echoes alone; its mix starts at 25 % (a return's is all effect). In a mixer slot: time and feedback.
  - **Utility:** gain (±36 dB), balance, width (mono to twice as wide, through mid and side), bass in mono below a frequency (120 Hz at first), and each side's polarity. It changes nothing until turned, and has no mix. In a mixer slot: gain and width.
- **Sidechain:** a device that has one, such as the compressor, can listen to another track instead of the sound passing through it, to duck a bass to the kick (`src/model/mixer/internal/Sidechain.h`). A muted source still feeds it, so a "ghost" kick can duck without being heard. Only tracks whose sound doesn't come from the device's own track are offered, since a bus, a send or another sidechain in the way would make a loop that Tracktion's player can't play; deleting the source turns the sidechain off in the same undo step. A plug-in with sidechain inputs uses the same routing, chosen from its menu or the routing view (step 2.9).

**Plug-ins that aren't Phazite's own (the owner, 2026-10-16, [question 6](#25-open-questions)):** none is built into Phazite, however good or freely licensed, such as Airwindows' effects (MIT) or Surge XT (GPL-3.0). Phazite may offer some as optional downloads, as it offers Phazite Sounds, for people who want them; its own devices then keep one design language, which the owner can make stricter later.

**Containers:**
- Select some devices and choose "Group into container". The container shows its devices in a row, with up to eight macro knobs that can drive any parameter inside, and saves as one preset. This is like Reason's Combinator and Ableton's racks.
- Parallel chains inside a container come later; Tracktion racks can be wired freely.
- Engine note: Tracktion runs one shared processing graph for every instance of a rack type, so each Phazite container gets its own rack type.

**Presets:**
- device presets, for built-ins and for plug-in state;
- track presets: drag a track into the browser to save its device chain and settings, as in Studio One;
- container presets.

**Containers as built (step 2.4, 2026-10-16; Claude's choices for the owner to review, each what most DAWs do):**
- **Grouping, as in Ableton Live and Bitwig:** Ctrl-click (or Shift-click) devices' names in a channel strip to select them (a plain click still opens a device's window), then Group into Container: Ctrl+G, which groups tracks into a folder while tracks are selected, or right-click a device. The selected effects go into a container in the first one's place, in their order on the channel, keeping their settings, automation and what the track's modulators do to them; a device right-clicked without being selected is grouped alone. Ungroup Container (Ctrl+Shift+G, or the container's right-click) puts them back. Each is one undo step. Devices are selected on their own: selecting one deselects tracks and clips, and the other way round.
- **Effects only for now:** an instrument can't go in a container (instrument containers, Ableton's Instrument Rack, come later), nor can a container (Tracktion's racks don't nest). A device in a container hears the sound passing through it: grouping one whose sidechain listened to another track turns that off (later: sidechains in containers).
- **Its slot** in the strip shows its name, its mix (a dry/wet blend that Tracktion delays to match the devices' latency) and its first macros as knobs; a click shows it in the Devices view (it had a window of its own until 2026-10-17). **There** it shows its name and eight macros on the left, as Ableton's racks show theirs, and its devices in a row on the right, with "+ Effect" after them. Double-click a name to rename it.
- **Eight macros, always there**, as Ableton's racks and Bitwig's remote controls show eight: "Macro 1" to "Macro 8", each at 0 when made, so that a parameter it modulates doesn't move until the macro is turned (a track's macros, from step 2.3, now start at 0 too). Drag a macro's grip onto a knob of one of the container's devices, or right-click such a knob > Modulate, where the container's macros come before the track's modulators; a macro moves only its container's devices. The track's modulators reach the devices in its containers too.
- **A container's devices are devices:** they turn on and off, move (Move Left and Right), save presets, are automated in lanes under the track, and are modulated, as any device is.
- **Each container plays a rack type of its own:** a duplicated track's containers get copies, which the copy's own modulators and macros modulate. Engine note: Tracktion's plug-in cache brings back the same container object when its removal is undone, still playing the rack type object it was made with, while a rack type removed and added again by undo and redo would be another object; so rack types stay in a project for the session (Tracktion's rack list keeps unused ones too), and those no container plays are dropped when the project opens.

**Presets as built (step 2.4, 2026-10-16; Claude's choices for the owner to review):**
- **One kind of file, `.phazitepreset`,** in `Documents/Phazite/Presets`: a folder for each device's presets ("EQ", or a plug-in's name), Containers and Tracks, as Ableton's User Library keeps presets by device. The browser shows that folder as a Presets place after Effects, each preset with what it holds ("EQ", "Container", "Track"), and search finds them. A preset holds what the project's file would, in the format version of the Phazite that saved it, which a newer one brings up to date; one from a newer Phazite isn't offered.
- **Saving:** a device's or container's right-click > Save Device Preset..., or a track's right-click > Save Track Preset..., opens a save dialog in its folder, named after it, as FL Studio's "Save preset as" does. Dragging a track by its handle out of the playlist into the browser saves it at once in Tracks, named after it, as in Studio One, and the browser shows the new preset.
- **What a preset holds:** a device's settings and mix (a plug-in's whole state), not its automation, what modulates it from outside the preset, its sidechain's source or whether it is on (a preset loads on). A container's devices and macros with what they modulate in it, its mix and name. A track's name, color, volume and pan, instrument, effects and containers, and its modulators and macros with what they modulate on it; not its clips, automation, sends or routing (Studio One's and Cubase's track presets leave clips out too). A drum sampler's own sounds go by their full paths, and a project that takes the preset copies them into its Audio folder.
- **Loading:** drag a preset onto a track or a mixer channel, or press Enter (or double-click) for the selected track, as a device: a device's or container's goes where that device would (after the effects; an instrument in place of the track's, or on a new track), a container named after its preset; below the tracks, an effect's goes on the master. A track's preset makes a new track after the one it is dropped on, or at the end. Each is one undo step, "Add" and the preset's name, with new IDs throughout, so a container's macros modulate the devices that came with them. Preset files dropped from Explorer load the same way. **Later:** a preset dropped on a device of its kind replaces its settings (FL Studio loads presets into the plug-in), browsing a device's presets from its window, and renaming presets in the browser.

**Plug-in windows:** each plug-in's own interface (and the knobs of one of Tracktion's built-in devices) opens in its own window, kept above the main window, and its position is remembered. Phazite's own devices and containers have none since 2026-10-17: they show in the Devices view.

**Keys over plug-in windows (2026-10-13):** play and stop (and pause), and the computer keyboard while it plays notes, work while a plug-in's window has the keyboard focus, so a sound can be played while it is tweaked (`src/ui/input/PluginKeys.h`; commands opt in with `Command::overPlugins`). Every other key goes to the plug-in. DAWs differ here: Bitwig gives a focused plug-in every key, REAPER and FL Studio have a per-plug-in switch, and Ableton Live keeps Space on Windows. On Windows, a plug-in's native window gets keys before JUCE sees them, so a message hook on the message thread catches these keys on their way to it. The hook is installed at startup, before any plug-in window: Windows calls the hook installed last first, and a plug-in built with JUCE installs its own hook, which offers each key to its focused control (a preset name being typed) and passes on the keys it doesn't use. A key typed into a native text field goes to the plug-in too. **Decided (2026-10-16):** a "Send All Keys to the Plug-in" switch in each plug-in window's menu, as REAPER and FL Studio have, for plug-ins whose own text fields Phazite can't see; off at first, and remembered for that plug-in in every project (step 2.12, [question 16](#25-open-questions)).

**Track limit:** Tracktion's default of 16 devices per track is raised. FL Studio's 10 effect slots per mixer track is a known complaint ([B12](#appendix-b-sources)).

## 10. Automation and modulation

Producers in Phazite's audience automate "all sorts of sliders", so this has to be effortless.

**Right-click any control,** built-in or a plug-in parameter, and choose:
- **Automate:** an automation lane appears under the track, ready to draw.
- **Modulate:** adds an LFO, envelope follower, step sequence, random source, breakpoint envelope or note follower. It shows as a ring on the control; drag the ring to set the amount.
- **MIDI learn.**

A hotkey automates the last-touched control (FL Studio's "last tweaked", Cubase 15).

**Lanes:**
- draw with the pencil, or use line and curve shapes (sine, ramp, steps);
- bend segments; select and move points;
- edit points from the keyboard;
- write modes per track: read, touch, latch and write.

**Lanes as built (step 2.1, 2026-10-16; Claude's choices for the owner to review, each what most DAWs do):**
- A lane shows under its track in the playlist, as in Ableton Live, Bitwig, Logic, Cubase and Studio One: a header with a chevron, the parameter (in a lane without automation, click it for another of the track's: its volume, pan and sends, then its devices') and the track's write mode; and the curve, drawn as the engine plays it, in the track's color. Lanes are view state, saved with each track ([section 18](#18-projects-saving-and-export)): showing or collapsing one is no undo step. The master's lanes show under a Master row below the tracks while it has some, as Live shows its main track.
- **Automation never hides (the owner's choice, 2026-10-17):** every parameter with automation has a lane, even one written while no lane showed (by recording, or in an older project), so nothing plays from an unseen place, as Bitwig deletes a lane's automation when its lane goes. To make room, a lane collapses to a thin row that still draws its line, with its chevron (the icon that collapses tracks), by dragging its bottom edge up, or with its track: a collapsed track's lanes are thin rows too, and the master row's chevron does the same for the master's lanes. A double-click on a thin lane, or its chevron, expands it (and its track, if the track kept it thin). A lane goes only with its automation: its menu's Delete Automation (Remove Lane, for a lane without automation) is one undo step, and an undo brings the lane back with the automation. Focus mode is the one view that leaves lanes out, those of the tracks out of focus.
- **Two clicks:** right-click any of Phazite's knobs or faders (the mixer's, the inspector's, a device's in the Devices view) → Automate; its lane shows, its track opens, and the playlist scrolls to it. A plug-in's own window is the plug-in's, so its parameters are automated from its slot in the mixer (right-click → Automate → the parameter), from a lane's parameter menu, or by moving the knob in the plug-in's window and pressing Ctrl+Shift+A, Automate Last Touched Parameter (FL Studio's "last tweaked", Cubase's last-touched control), which works over plug-in windows.
- **The tools in a lane (made simpler at the owner's request, 2026-10-17: the Smart Tool needs no keys):** a click adds a point, on the grid; pressed on the line, the new point sits on the line, so nothing sounds different until a drag pulls it, as Studio One's arrow tool does. Dragging a point moves it with the other selected points; selected points show larger, in the text color with an accent ring, on a track of any color. The line under the mouse shows a hollow handle in its middle, as Studio One's and FL Studio's do: dragging it up or down bends the line, no further than where it still runs from point to point, so the points stay put. A double-click deletes a point, or straightens a line at its handle, and a drag on empty space selects the points in a rectangle. Keys only refine, with the meanings they have everywhere in Phazite: Alt places off the grid, Shift moves in fine steps (as on knobs and faders), and Ctrl adds to the selection or takes out of it. Draw draws a step for each grid cell it passes, as Live's draw mode does, or a free line with Alt. Paint draws a shape between where its drag starts and ends, a cycle a grid step long, as high and low as it drags (sine, or from the lane's menu: triangle, square, saw up, saw down or a line), as Studio One's paint tool does. Delete deletes the points it passes; Select selects. With FL Studio's keymap a right-click deletes points, as it deletes clips; otherwise it shows the lane's menu.
- **Keys:** the arrow keys raise and lower the selected points by a hundredth of the range (with Shift a tenth) while points are selected, as they move selected notes; nudging moves them a grid step; Delete and Duplicate act on them. A expands the selected tracks' lanes, or collapses them all while one shows expanded, as Live and Logic Pro show and hide automation with A (a track without lanes gets one for its volume). Automate Last Touched is Ctrl+Shift+A, not Shift+A, because Shift and a letter must run the letter's own shortcut while the computer keyboard plays notes.
- **Write modes** are each track's, as in Logic Pro, Pro Tools, Cubase and Studio One (Live has one arm button instead), and Read by default; changing one is no undo step, as mute and solo aren't, and a mode that writes shows in red in the lane's header. They behave as Tracktion records them: Read plays the lanes, and a control moved by hand goes back to its lane at the lane's next change; Touch writes while a control is held, and the lane takes over again when it is let go; Latch writes from a control's first move until playback stops; Write overwrites every automated parameter of the track from the moment playback starts until its control is let go, and then the track is in Latch, as Pro Tools can switch after a write pass. What a pass records is one undo step, "Record Automation", or the step of the control being dragged.
- Tracktion keeps track automation in seconds; Phazite keeps it on its beats when the tempo or the time signature changes, as clips are kept, in the same undo step. The values Tracktion writes into a device as its automation plays aren't unsaved changes.

**No automation clips (the owner's decision, 2026-10-17):** lanes are the one way to automate. Step 2.2 had built FL Studio's automation clips; trying them, the owner found that they lay over the track's own clips, hiding them and catching the mouse, and that they broke the rule that everything in a track's row belongs to its instrument, since a clip moved anywhere still set what it was made for. Asked whether any DAW has both, the research found two philosophies:
- **Lanes:** Ableton Live, Bitwig, Logic Pro, Cubase, Studio One, Pro Tools and REAPER keep automation with its parameter, in lanes under the parameter's track.
- **Free clips:** FL Studio (and LMMS), whose playlist rows aren't tied to an instrument, which is what lets a clip go anywhere. Phazite's rows are tracks with instruments, so the model didn't fit.
- **Where both exist, the second kind is tied down:** Ableton's clip envelopes, Bitwig's clip automation, Logic's region automation and Studio One's part automation live inside the instrument's own clips; REAPER's automation items live inside a parameter's own lane, and so do the automation clips Bitwig 6 added in 2026, each linked to the note or audio clip it belongs to, which it follows when that clip is copied or moved (Bitwig's release notes for 6.0; Sound On Sound's review). None lets a clip on one track set another's parameter.
- **FL Studio's own users ask for lanes:** "collapsible automation lanes attached directly to tracks" (Image-Line forum, March 2026), and that new clips stop landing "on any random track" (February 2026).
- **What clips do well** is go with the music when parts are rearranged (KVR, "The automation problem and which DAWs have a solution?", 2022). Lane DAWs do that by moving automation with its clips: Ableton Live ("Live normally moves all automation with the clip"), Bitwig (Automation Follow, on by default) and Studio One (Automation Follows Events, on by default); Cubase, Logic and Pro Tools have it as a setting.

So Phazite keeps lanes, which are also Tracktion's own path for automation and the lightest for the engine, and gives up linked automation (aliases sharing a curve), which no lane DAW but REAPER has. Projects saved with automation clips (format version 4) open with each clip's curve as its parameter's own automation over where the clip was, jumping to the curve where the clip started and back where it ended, as it played; the clips go, and so does the track named Automation that Phazite made for the master's and folders' clips, once empty (format version 5: `src/model/base/Migrations.h`).

**Automation goes with the music (2026-10-17):** moving or copying clips along their track (dragging, Ctrl+drag, nudging, Duplicate) takes the automation of the track's parameters under them along, in the same undo step, in place of what is where it lands, as Ableton Live, Bitwig and Studio One do by default; so do deleting clips with Close Gap, which takes their time out of the track, and deleting or duplicating a time range selected across tracks (and their folders). Each stretch keeps its shape at its edges wherever it goes, and where a moved stretch was, the line runs straight across. A clip moved to another track leaves its automation, since that track's devices are others, and a clip deleted without closing its gap leaves it too. A drag moves the automation once, when the drag ends. Automation on a folder or the master doesn't follow the clips of the tracks inside (the shortcoming users find in Cubase's Automation Follows Events): a selected time range across the folder carries it.

**Modulators and macros as built (step 2.3, 2026-10-16; Claude's choices for the owner to review):**
- **Each track's, as Cubase 15's modulators are:** a track, a folder and the master each have modulators of their own (an LFO, an envelope follower of how loud the track is, a step sequencer, a random source, a breakpoint envelope, and a note follower of the pitch or velocity of the notes it plays) and macros, knobs that move several parameters at once (on tracks and returns only: Tracktion keeps macros on tracks that hold clips). They modulate the parameters of the track's own devices and its volume, pan and sends. They are Tracktion's modifiers and macro parameters (`src/model/modulation/ModulationModel.h`).
- **Two ways to assign one:** right-click a knob → Modulate → one of its track's modulators (ticked while it modulates the knob; choosing it again stops that), or New LFO … New Macro, which adds one already modulating the knob, as one undo step. Or drag a modulator by its grip, from the Devices view's first column (a channel strip's Modulators section until 2026-10-17), onto a knob of one of its track's devices, as Bitwig drags modulators. A modulation starts at a quarter of the parameter's range, and moves the parameter around where its knob is set: both ways for a bipolar modulator (LFOs, random sources and step sequencers start bipolar; Tracktion's start one way, and its step sequencers and envelope followers at no depth), the other way for a negative amount.
- **The ring:** a modulated knob shows a ring in the modulation color: the range its modulations can move it over, and a dot where it is now, while the knob itself shows its own value (or its automation's), as in Bitwig. Dragging the ring (the knob's outer edge) up or down changes its first modulation's amount, with Shift in fine steps, as one undo step; the rest of the knob turns it as before.
- **Editors:** a click on a modulator's name opens its editor in a bubble, as it opens when the modulator is added: its output over the last two seconds, a knob for each of its parameters (rate, wave, depth, polarity, sync...), and a step sequencer's steps, drawn with the mouse, one undo step per drag. A macro's slot is its knob. Right-click a modulator's name to stop it modulating a parameter, or to remove it, with what it modulates.
- What modulation plays is no unsaved change, and steady playback of every kind allocates nothing on the audio thread (a guarded scenario of the [real-time audit](realtime-audit.md)). Modulators' own parameters can't be automated or modulated yet.

**How the two layers combine:**
- the lane sets the base value, and modulators move around it;
- each control shows both: its automated value and the modulation range.

**Engine notes:**
- modulators update once per audio block, which is fine for most uses but coarse for very fast modulation;
- parameters inside containers can't be recorded live yet.

## 11. The browser

**Built in step 1.5 (2026-10-13)** (`src/ui/browser/Browser.h`, `src/services/Library.h`, `src/model/engine/Preview.h`, `src/ui/browser/PluginPicker.h`):

- **One browser, two ways in:**
  - **a tree of places**, as FL Studio's browser has it: Favorites, Recent, Sounds by Type, Phazite Sounds (the free sounds, once downloaded), Instruments, Effects, then your own folders as they are on disk (FL Studio users ask other DAWs for this), and **Add Folder...**. One click opens a folder, as in FL Studio. The folders open when Phazite closes are open again at the next start;
  - **search** across your sounds and the devices, as you type, best matches first (names that start with what you typed, then names with a word that does). Ctrl+F (Phazite's keymap) puts the cursor in the search field; Down or Enter moves to the results.
- **Tags** come from the words in a sound's name and folders: what it is (Kick, Snare, Clap, Hat, Cymbal, Tom, Percussion, Bass, FX, Foley, Vocal, Synth), Loop or One-shot, and its tempo ("120 BPM"). Search finds them, so "kick loop" finds a kick loop in a folder called Loops. A background thread indexes the sounds (up to 50,000).
- **Preview:**
  - click a sound, or move to it with the arrow keys, to hear it; Esc stops it. The headphones switch under the tree turns previews off, and a fader sets how loud they play;
  - loops play in time with the project's tempo, stretched by SoundTouch; while the song plays, a loop starts on one of its bars. Tracktion knows a loop by its loop data, or by a tempo in its name ("Break 120 BPM") when it lasts whole bars at that tempo. Effects made for some bars ("Riser 4 Bars 128 BPM") play in time but once. One-shots play at their own pitch. MIDI files play on the 4OSC synth;
  - loops keep their own pitch for now. Tracktion's pitch matching would pull every loop toward the song's pitch, which is C in a song without a key, and it ignores major and minor (an A minor loop would go up to C in C major). **Later:** in key, once songs have a key: the selected track's, treating relative major and minor as the same key, as Logic Pro does;
  - **later:** previews through the selected track's devices, and tempo detection for loops whose names don't say (Tracktion includes a tempo detector).
- **Putting things into the song:**
  - **drag and drop:** onto empty space below the tracks, a new track; onto a track, a clip at the grid line before the pointer, or a device; onto a mixer channel, a device; onto the master, an effect. An instrument dropped on a track that can't hold one (a folder, a return, a track with audio clips) gets a new track next to it, named and colored after it. **Later:** onto a device slot replaces it; onto a drum pad loads the sound (step 1.6);
  - **Enter or a double-click** puts a sound on the selected track at the play start (where Space plays from), or on a new track; a device goes onto the selected track.
- **Plug-in picker (F8, as in FL Studio):** type part of an instrument's or effect's name or maker, then press Enter (or click it) to add it to the selected track, or to a new one. Instruments come before effects. Phazite's keymap moves the browser's show-and-hide key to Ctrl+Alt+B, Ableton Live's; FL Studio's keymap keeps Alt+F8.
- **Favorites and recent items:** the star at the end of a row (on the selected row, and filled on favorites) or the right-click menu makes an item a favorite. Recent lists the last 50 items put into the song. **Later:** collections (named sets, as Ableton Live's colored ones).
- **Sounds by Type** lists every sound, from Phazite's library and the user's folders, by its type tag (Kicks, Snares, ... Loops), as Ableton Live's labels and Bitwig's "Drum Hits" do.
- **Phazite Sounds (2026-10-13):** 85 free sounds for a new user to start with (9.3 MB of FLAC): acoustic and electronic drums, 808s, loops at 90 to 174 BPM, risers, impacts and other effects, and foley.
  - **An optional download, not part of Phazite.** They live in their own public repository, [phazite-sounds](https://github.com/august-byrne/phazite-sounds), whose releases hold the pack as a zip, so Phazite's repository and installer stay small and contributing a sound never touches Phazite's code. Phazite offers them once, in the welcome window ([section 13](#13-getting-started-and-song-outlines)), and the browser's Phazite Sounds place has a **Download** row until they are installed; Help > Phazite Sounds offers them again. This follows Bitwig's and Studio One's sound packages, which install after the program, rather than FL Studio's bundled library; experienced users skip them.
  - **Download (`src/services/SoundPack.h`):** only when the user asks, over HTTPS from GitHub, on a thread of its own, with progress in the browser and the welcome window. Phazite pins the release it installs by address, size and SHA-256, so a changed or damaged file is refused, and a released zip is never replaced; a new release means a new pin in Phazite. The zip unpacks into `Documents/Phazite/Packs/Phazite Sounds` (entries that would land outside are refused), whose `sources.json` names the pack and version. A zip downloaded by hand installs the same way (**Install from a File...**), for computers without a connection. Linux builds have no TLS without curl, so there the button opens the release page; **later:** curl on Linux, and updates when a new version is pinned.
  - **They are CC0 only**, so songs made with them carry no conditions; CC BY and CC BY-SA would make every released song owe credits, and "royalty-free" licences forbid redistribution. Third-party sounds come from libraries whose licence files say CC0 and whose descriptions show no one else's material: VCSL, Virtuosity Drums, Big Rusty Drums, body_percussion, FreePats, screened Sonic Pi samples, EatMe's breakbeat and Kenney's foley. Phazite synthesizes the rest (808s, analog-style drums, risers, sweeps and impacts) and plays its loops from these sounds to its own patterns: `phazite_soundkit` (`tools/soundkit/`) renders the pack from the recipes in the sounds repository, the same bytes every time, and zips it the same way every time. The pack's `sources.json` records each sound's source, licence evidence and hash, and the sounds repository's CI checks that every sound is listed there as CC0. Drum machines' own recordings (the 909's cymbals), sampled records (the Amen break) and the makers' names are kept out.
- **Later:** automatic tags for your own samples, from analysis (one-shot or loop, drum type, brightness, key, tempo), and "find similar".

## 12. Commands, keys and help

**Command registry:**
- every action has an ID, a name, a description, search synonyms (searching "cut" finds Split), a category and a default key;
- the registry itself has no UI code (2026-10-14): it lists the commands, runs each inside its named undo step, says whether one is enabled or ticked, and searches them. The UI connects it to keys and menus through JUCE's `ApplicationCommandManager`, which Tracktion's `UIBehaviour` expects ([section 24](#24-code-structure));
- later, commands take parameters and return results, so that scripts and the assistant can run them, as MuseScore's newest commands do ([B46](#appendix-b-sources)).

**Command palette (Ctrl+K):** a fuzzy search over every command, showing each one's shortcut. Enter runs the selected command.

**Menu bar (2026-10-13): File, Edit, Add, View, Options and Help.** Like FL Studio's, Live's and Bitwig's, it has no Track, Song or Transport menu:
- **Add:** a track, a folder or a return track, a section, and the song outlines;
- **Edit** also holds the selected tracks' edits (color, mute, solo, folders, order) and inserting and removing time;
- **View** also holds going to the start or end, focus mode, pinning, collapsing and track heights;
- **Help:** the command search, the keyboard shortcuts and hover help;
- play and stop are on the top bar and the keys. Every command stays in a menu as well as in right-click menus, as Apple's and Microsoft's guidelines ask.

**Keymaps (decided: whatever users find most intuitive; FL Studio style is fine):**
- **First launch asks** "Coming from FL Studio, Ableton Live, Logic, Pro Tools, or new to music software?" and loads a matching keymap. Digital Performer 12 has switchable keysets that copy other DAWs' shortcuts ([B13](#appendix-b-sources)). v1 ships the Phazite default and FL Studio keymaps, with Ableton Live next.
- **The Phazite default** follows conventions that most DAWs and the operating system share:
  - Space plays, and stops back at the play start: where playback last started (the last click in a time ruler, or the song's start in a project just opened), so a sound can be replayed quickly while it is tweaked. FL Studio, Ableton Live, REAPER and Waveform do this; Logic Pro, Studio One and Bitwig pause instead, with an option to go back. Shift+Space pauses and plays on from there, as in Ableton Live (Ctrl+Space in FL Studio's keymap, as in FL Studio);
  - Ctrl+Z and Ctrl+Shift+Z undo and redo;
  - Ctrl+S saves;
  - Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste.

  It adds FL Studio's single-key tools, which reviewers praise.
- **Any command can be rebound.** Ctrl+Alt+click on a menu item assigns a shortcut directly, as Logic's Control-click does. Changed keys are saved by the command's ID (`edit.split`): JUCE's own format saves numbered commands, and the numbers change when commands are added (step 1.2).
- **Keys that depend on what is selected:** several commands can share a key, and the enabled one with the highest priority runs. With notes selected, the arrow keys move the notes; otherwise they select tracks or nudge clips (step 1.3).
- **The computer keyboard plays notes**, on by default so a new project makes sound right away (step 0.7):
  - the layout of Ableton Live and Logic Pro: the middle row of letters plays the white keys from middle C, the row above the black keys, Z and X change the octave and C and V the velocity in steps of 20 ([B40](#appendix-b-sources));
  - it takes only keys pressed without Shift, Ctrl, Alt or Cmd, and never keys typed into a text field;
  - a top-bar button, M in Phazite's keymap (as in Ableton Live) and the Options menu turn it off. In the FL Studio keymap Ctrl+T does, and F10 opens the MIDI settings, as in FL Studio ([B41](#appendix-b-sources)). FL's manual doesn't document which keys play which notes, so that keymap uses the same layout;
  - **single-key shortcuts (decided, open question 9):** as in Ableton Live 12 ([B40](#appendix-b-sources)), while the computer keyboard plays notes, Shift and a letter run that letter's shortcut (FL Studio's tools, for example), unless Shift and that letter have a shortcut of their own. With the computer keyboard off, the letters alone run them. So a keymap shouldn't give Shift and a letter a different command than the letter alone.
- **How "most intuitive" gets settled:**
  - watch three to five people, both FL Studio users and complete beginners, do the same tasks with a prototype: place a kick on every beat, trim, fade, duplicate a section, write a chord;
  - count hesitations and errors, and adjust the defaults from that.

**Macros:** chain commands into one, like REAPER's custom actions, and bind the result to a key.

**Named undo:**
- every command is one named undo step ("Split clip", "Apply outline"), and an undo history panel lists them;
- Tracktion groups changes on a timer and doesn't name them, so Phazite starts a named transaction for each command, and for each edit the UI asks the model for. From architecture phase A2, the UI never starts one itself, and a drag is one step, opened when it starts and kept or rolled back when it ends ([section 24](#24-code-structure)).

**Help:**
- the hint bar is always there;
- hover descriptions are on by default for new users and can be turned off with one click;
- later, a manual page for the hovered control.

## 13. Getting started and song outlines

**First launch** needs no account and no sign-in:
1. "Coming from…?" chooses a keymap.
2. Audio setup: pick an output, or an ASIO® driver when an audio interface is present.
3. A starter project opens with a drum track loaded with a kit and a synth track. The computer keyboard plays notes, and the hint bar suggests a next step ("Press Space to play", "Drag a kick from the browser").

**The welcome window (2026-10-13)** is where first launch asks these things, a page at a time (`src/ui/shell/Welcome.h`). It opens once, over the starter project, the first time Phazite starts, and **Help > Welcome** opens it again. Every page is optional. Its last page offers Phazite Sounds ([section 11](#11-the-browser)): **Download**, which shows its progress, or **Install from a File...**, and **Not Now**.

**First launch as built (step 1.9, 2026-10-15, proposed):**
- The welcome window has three pages, with **Back**, **Next** and **Not Now**: "Coming from another music app?" (FL Studio's keymap for FL Studio; Phazite's own for Ableton Live, Logic Pro, another app or none), the sound page, and Phazite Sounds. A choice of app takes effect at once.
- The sound page has JUCE's device chooser, with outputs only and the sample rate and buffer size behind a button, and **Play a Test Sound**, a one-second beep through the output chosen; on Windows it explains ASIO. Ableton Live opens its Learn View on first launch and points newcomers to its "Setting up Audio I/O" lesson ([B63](#appendix-b-sources)), and newcomers' "no sound" threads in DAW forums often come down to the wrong output, or an ASIO driver another app holds.
- The starter project selects its Synth track, so the computer keyboard plays the synth at once: the drum sampler's pads are two octaves below the keyboard's first octave.
- **The guide:** while the pointer rests on nothing with a tip of its own, the hint bar suggests the next step: play the song, play notes on the computer keyboard, drag a sound from the browser, record (or capture) what is played, and export the song, with the keys of the keymap in use (`src/ui/shell/Guide.h`). A step goes once it is done, in any way, and stays done in later sessions; recording counts as playing notes. **Help > Getting Started Tips** turns the guide off and on.

**New project (step 1.9, 2026-10-15, proposed; the name moved to the first save on 2026-10-16):** a window with the tempo, time signature, key (none, or a root with major or minor) and an optional song outline, whose sections it lists, as GarageBand's project chooser offers tempo, key and time signature before a project starts ([B62](#appendix-b-sources)). The project is untitled until its first save asks what to call it and where it goes ([section 18](#18-projects-saving-and-export)). A new project starts with a Drums track on the drum sampler and a Synth track on the synth, without clips, unless that box is unticked, and the computer keyboard plays the synth; the key goes on those tracks and on tracks added later. What it starts with is part of its first save, not undo steps (`src/model/project/NewProject.h`).

**Song outlines (decided)** are templates that lay out a song's sections, so newcomers see how songs are usually shaped. Applying one:
- creates colored sections with typical lengths;
- draws an **energy curve**, faintly, behind the sections' names in the time bar;
- gives each section a one-line tip, such as for a build-up: "Raise the energy: risers, a snare roll, open a filter".

Sections stay fully editable, and later features use them; chord suggestions, for example, can tell a chorus from a verse.

v1 outlines (decided), with typical lengths in bars at the project tempo, all editable:

| Style | Sections (bars) |
|---|---|
| Pop | Intro 4, Verse 8, Pre-chorus 4, Chorus 8, Verse 8, Pre-chorus 4, Chorus 8, Bridge 8, Chorus 16, Outro 4 |
| Rock | Intro 8, Verse 16, Chorus 8, Verse 16, Chorus 8, Solo 8, Bridge 8, Chorus 16, Outro 8 |
| Electronic (house, EDM) | Intro 16, Build-up 8, Drop 16, Breakdown 16, Build-up 8, Drop 16, Outro 16 |
| Hip-hop and trap | Intro 4, Hook 8, Verse 16, Hook 8, Verse 16, Hook 8, Outro 4 |
| Lo-fi beat (2026-10-13) | Intro 4, Loop A 8, Loop B 8, Loop A 8, Break 4, Loop B 8, Outro 8 |
| R&B (2026-10-13) | Intro 4, Verse 16, Pre-chorus 4, Chorus 8, Verse 16, Pre-chorus 4, Chorus 8, Bridge 8, Chorus 8, Outro 4 |
| Drum and bass (2026-10-13) | Intro 32, Build-up 8, Drop 32, Breakdown 24, Build-up 8, Drop 32, Outro 24 |
| Techno (2026-10-13) | Intro 32, Groove 32, Build-up 16, Breakdown 16, Peak 32, Outro 32 |

- **Later:** dubstep, trance and future bass, DJ-friendly variants with 32-bar intros and outros, and outlines saved from a song's own sections.
- **Outlines are data files**, so adding a style needs no code changes.
- Each outline can also suggest a chord progression per section, linking to [section 14](#14-chords).

## 14. Chords

**The chord lane** is a global lane under the sections that shows the song's chords. It drives:
- piano-roll highlighting of chord tones and the scale;
- generated parts: "make a chord part, bass line or arpeggio from the chord lane", with voicing presets (triads, sevenths, spread) and rhythm presets (sustained, pulses, offbeat stabs, arpeggio);
- chord suggestions ([section 16](#16-ai-and-generative-features)).

**Chord progression outlines (decided)** are a library of common progressions to try. You can:
- browse them by mood and genre; each entry shows its numerals and a name;
- audition one in the project's key and tempo with the selected instrument;
- apply it to a section, a selection or the whole song. Change the key and the progression follows.

Starting set (numerals relative to the key; lowercase means a minor chord):

| Progression | Feel, and where it's common |
|---|---|
| I–V–vi–IV | Bright pop anthem |
| vi–IV–I–V | The same chords, more wistful; pop and EDM |
| I–vi–IV–V | 1950s doo-wop and retro pop |
| ii–V–I (with sevenths) | Jazz, house, lo-fi |
| i–VI–III–VII | Epic minor; trance, EDM, film |
| i–VII–VI–VII | Driving minor; rock and EDM |
| i–iv–v (or i–iv–V) | Dark minor; hip-hop and trap |
| I–IV–V | Rock, blues, country |
| I–♭VII–IV | Mixolydian rock |
| IV–V–iii–vi | "Royal road"; J-pop and anime |
| i–VII–VI–V | Andalusian cadence; flamenco and rock |
| 12-bar blues | Blues and rock and roll |

v1 should have about 40, tagged by mood and genre, in a data file that is easy to extend.

**Engine note:** Tracktion's chord progressions accept Roman numerals only for the seven diatonic chords. They are stored lowercase as a degree plus 6, 7 or 9, and take their quality from the scale (`tracktion_Musicality.cpp`). Borrowed chords such as ♭VII and secondary dominants such as V/V can't be written. Phazite therefore keeps progressions in its own format: degree, accidental, quality, extensions, inversion and length. Since Phazite makes its parts itself (below), nothing in Tracktion reads the chords, and they aren't written into Tracktion's chord track: a second copy would have to be kept in step for nothing (decided at step 2.7).

**As built (step 2.7, 2026-10-18; decided by Claude for the owner to review):**
- **Where it is.** The chord lane is a thin row under the playlist's bar numbers, above the tracks, always there; an empty lane says what to do with it. Its header, over the track headers, shows the song's key and a **...** menu with the progressions and the parts. Studio Pro's chord track is a global track that can be hidden ([B82](#appendix-b-sources)); Phazite's stays, being thin.
- **Chords.** A click on empty space offers the key's seven chords, then those from outside it (borrowed from the parallel key, and the dominants of the scale's chords), and adds the one chosen there, a bar long, up to the next chord, as new chords in Studio Pro default to a bar ([B82](#appendix-b-sources)). A click on a chord offers the same chords in its place, its kind (major, minor, sevenths, sus, sixths, ninths, power), its inversion, and Delete; Studio Pro's right-click changes a chord's root, type and bass the same way. Dragging a chord's right edge makes it longer or shorter, on the grid, over the chord after it. Each change is one undo step. Names spell each note by its step of the scale (the leading note of D harmonic minor is C♯, not D♭).
- **Keys.** Chords are saved as numerals in the key where they are, so a change of key moves them all: the header's key button changes the song's key, and **Change Key from Here** in the lane's menu adds a key change at a chord or a bar. Until a key is chosen, the lane is in the new project's key, or C major.
- **Progressions.** The Chord Progressions window (**Add > Chord Progressions**, the lane's menus, the command palette) lists 40, narrowed by mood and genre, each with its name, its chords in the song's key, its numerals and its feel. **Play** hears the selected one in the song's key and tempo, held chords over their roots on Phazite's synth, looping in time with the song. **Put on the Chord Lane** lays it over the selected sections, the selected time range, or the whole song (once from the start in an empty song), repeated to fill it, as one undo step. A progression written for minor plays, in a major key, in the minor key with the song's notes (A minor's i–VI–III–VII in C major is Am–F–C–G), and one for major in a minor key in its relative major; in a mode, they play in the major or minor key with its notes.
- **Parts.** **Make a Part from the Chords** (the lane's menus; **Add > Make Chord Part**, **Make Bass Line** and **Make Arpeggio**, and the palette, with the first style of each) adds a track at the end with Phazite's synth set up for the part (a slow pad, a mono bass with a sub, a pluck), and a clip on it over the selected time range, the selected sections, or all of the chords. Styles: held triads, held sevenths, spread pads, a chord on every beat and offbeat stabs; held roots, roots in eighth notes, root and fifth (the chord's own fifth) and octaves; arpeggios up, down, and up and down. Chords are voiced around middle C, each moving as little as it can from the one before; bass lines go from C2 up an octave, playing an inversion's bass note; arpeggios start at middle C.
- **Following.** A part follows the chords while its notes are the ones the lane made for it: a change of a chord or the key makes them again, in the same undo step. Editing its notes stops it, and the inspector's **Follows the chords** says so; turned on, the part's notes are made from the chords again, and turned off it is a clip like any other. This is the way of Logic Pro's Session Players, whose regions follow the chord track ([B83](#appendix-b-sources)), rather than Cubase's and Studio Pro's, which move notes played by hand onto the chords by modes ([B82](#appendix-b-sources), [B84](#appendix-b-sources)): Phazite made the notes, so it can make them again exactly, and nothing changes notes the user wrote.
- **Time.** Chords go with the song's time as sections do: inserting or removing time, and moving or copying a section or a range, move them; parts, being clips, go with their tracks.
- **The piano roll** tints the rows of each chord's notes while the chord lasts, in any key.
- **Not yet:** chords detected in MIDI or audio (Studio Pro's Extract to Chord Track), notes played by hand following the chords (Cubase's and Studio Pro's follow modes), outlines that suggest progressions for their sections, auditioning with the selected track's instrument, and progressions saved as presets.

## 15. Sound generator

**Decided:** generate new kicks, snares and other drum sounds as a set, then keep or favorite the ones you like each time.

**How it works for the user:**
- **Open it** from the browser's Generate tab, from a drum pad, or by right-clicking a drum track.
- **Pick a type:** kick, 808, snare, clap, closed hat, open hat, tom, rim or percussion. Optionally add character words: punchy, boomy, tight, dirty, dark, bright, short, long, lo-fi.
- **Generate** fills a grid with 12 sounds. Click one to hear it. **Audition in context** swaps it into the playing loop temporarily.
- **For each sound you can:**
  - **keep** it, which adds it to the project;
  - **favorite** it, which adds it to your library;
  - **lock** it, so it stays while the rest regenerate;
  - ask for **more like this**: the next batch varies around it, and a similarity slider sets how far;
  - drag it onto a track or a pad.
- **It uses the project:**
  - kicks, 808s and toms are tuned to the project's key;
  - decay lengths fit the tempo;
  - loudness matches the rest of the kit;
  - later, it steers away from frequencies the existing drums already fill.
- **History** steps back and forth through earlier batches.
- **Every sound keeps its recipe** (engine version, seed and parameters), so it can be reopened and tweaked in the drum synth or re-rendered at any sample rate.

**v1 engine: a drum synthesizer plus evolution.**
- **Each drum type is a synthesis patch.** A kick, for example, is a pitched oscillator with a pitch envelope, a click transient and saturation. A snare is a tuned body plus filtered noise. Hats are metallic oscillators or FM through filters.
- **Generate** samples the patch's parameters from musically tuned ranges for each character word. It then measures the results (loudness, length, brightness) and discards weak ones.
- **More like this** mutates the chosen sound's parameters. Later, "breed" combines two favorites, as Synplant does.
- **The drum synth is also a built-in instrument,** so a generated kick can stay live and editable on a track.
- **Why not a neural model in v1:** no suitably licensed model exists (see the licence table in [section 16](#16-ai-and-generative-features)). Synthesized drums are a staple of electronic music anyway. Realistic acoustic drums are where a trained model would add the most, later.

**Prior art** ([B14](#appendix-b-sources), [B15](#appendix-b-sources), [B16](#appendix-b-sources)):
- **Audialab Emergent Drums 2:** generates drums from scratch, and variations of a sample with a similarity slider.
- **Steinberg Backbone with Sony's DrumGAN:** smart parameters such as "kick-ness", and a variation slider.
- **Sonic Charge Synplant 2:** a seed with branches; plant a branch to evolve the sound further.

## 16. AI and generative features

**Principles:**
1. **Useful because it's in a project.** Each feature reads the project's context: key, tempo, chords, melody, sections and outline style, the sounds already used, and your past choices.
2. **Assistive.** You get suggestions you audition and accept. You own the result and can edit it.
3. **Local-first and opt-in.** Everything runs on your machine. Nothing leaves it unless you later add a cloud model yourself.
4. **Licence-clean.** Only models and data whose licences fit the AGPL.
5. **Transparent.**
   - Suggestions say why they were made.
   - The project records which generative features produced what (generator, version, seed). That makes results reproducible and lets users label their work accurately, for example with the music industry's "AI-Assisted" labels ([B17](#appendix-b-sources)).
6. **Out of scope:**
   - text-to-song generation; 82% of producers oppose it in Tracklib's 2025 survey ([B18](#appendix-b-sources));
   - stem separation, because many tools already do it (decided).

**v1 features (decided):** chord suggestions and sound generation ([section 15](#15-sound-generator)). The assistant comes later.

**v1 engines (decided):** algorithmic. The chord engine is built on music theory, and the sound generator on synthesis and evolution. Trained models can come later, behind the same interface.

### Chord suggestions

**What it suggests:**
- **Next chord:** after any chord on the chord lane, four to six suggestions grouped as safe, colorful or surprising.
- **Fill a section:** three to five complete progressions for the selected section, suited to its role (verse, chorus, drop) and the outline style.
- **Replace:** alternatives for a selected chord: substitutions, borrowed chords, and inversions for a smoother bass line.
- **Fit my melody:** chords that contain the melody's notes on strong beats, for a selected MIDI clip.

**How it presents them:**
- A "safe ↔ adventurous" slider sets how far suggestions stray.
- Every suggestion says why it was made: "contains your melody's C and E on beats 1 and 3", "borrowed from the parallel minor".
- You audition a suggestion in place, apply it with one click, and can undo it.

**Engine, v1:** a music-theory engine that scores candidate chords by:
- how well they fit the melody;
- smooth voice leading;
- harmonic function and cadences;
- the section's role;
- style weights for each outline (pop, rock, electronic, hip-hop).

It is instant, explains itself, works offline and needs no training data. It also learns your taste locally from the suggestions you accept.

**Engine, later:** a small trained model, if training data with a suitable licence exists. Today:
- Chordonomicon, the largest chord dataset with section and genre labels (about 680,000 songs), is non-commercial ([B19](#appendix-b-sources)).
- Hookpad's data is proprietary.
- The McGill Billboard annotations are public domain (CC0) and include verse and chorus labels, but cover only 890 chart slots ([B20](#appendix-b-sources)). That is enough to calibrate pop and rock weights, not electronic music.

**Prior art** ([B21](#appendix-b-sources), [B22](#appendix-b-sources), [B23](#appendix-b-sources)):
- **Cubase's Chord Assistant:** suggests the next chord by cadence, shared notes or the circle of fifths, and fills gaps between two chords.
- **Hookpad:** next-chord suggestions drawn from 75,000+ songs, chords that contain a given melody note, and a paid AI add-on.
- **Fender Studio Pro 8:** chord suggestions and progression presets.
- **FL Studio 2024:** a chord progression tool.

### Model licence check (October 2026)

| Candidate | Licence | Fit for Phazite |
|---|---|---|
| Stable Audio Open Small (Stability AI): 0.5B parameters, 44.1 kHz stereo, up to 11 s, trained on Creative Commons audio from Freesound and the Free Music Archive ([B24](#appendix-b-sources)) | Stability AI Community License: free below US$1M annual revenue; revocable; not sublicensable; requires "Powered by Stability AI"; outputs may not be used to train other foundation models ([B25](#appendix-b-sources)) | Can't be bundled with an AGPL app. At most an optional download that each user accepts separately |
| Meta AudioCraft (MusicGen, AudioGen) | Code MIT; weights CC-BY-NC 4.0 ([B26](#appendix-b-sources)) | No: non-commercial |
| RAVE (IRCAM) | CC BY-NC 4.0 ([B27](#appendix-b-sources)) | No: non-commercial |
| stable-audio-tools (training code) | MIT ([B28](#appendix-b-sources)) | Yes. It could train our own drum model on public-domain audio later, which needs GPU time and careful data curation |
| Chordonomicon (chord dataset) | CC-BY-NC 4.0 | No: non-commercial |
| McGill Billboard chord annotations | CC0 | Yes |

No permissively licensed drum-generation model was found. This table summarizes licence texts and is not legal advice.

**After v1:**
- **The assistant.** It acts through the command system: every action is shown before it runs and can be undone. It runs on a local model or your own API key, as Waveform 14's and Ardour 9.5's do ([B9](#appendix-b-sources), [B29](#appendix-b-sources)).
- Audio-to-MIDI.
- Automatic tagging and "find similar" for your samples.
- Neural sound generation with our own model.
- Analysis of a reference track's structure, shown against your song outline.

## 17. Recording and Capture

**v1:**
- **Live MIDI:** every MIDI keyboard, and the computer keyboard, plays one track, without arming it: the selected track with an instrument (step 1.1), or the project's first track with one.
- **MIDI recording** from controllers and the computer keyboard onto that track, with overdub or replace, and optional input quantize (step 1.7). It starts from where playback starts, after the count-in, and each take is one undo step. The song's own loop doesn't loop a recording, which runs on past the song's end; a range looped on purpose does.
- **Capture:** Phazite keeps recent MIDI input (Tracktion keeps 30 seconds by default), and one key turns it into a clip (step 1.7). This is Live's Capture MIDI and Logic's Flashback Capture ([B30](#appendix-b-sources)). While the song plays, the notes stay where they were played; while it is stopped, the clip goes at the play start's bar, or after the track's clips. If the project is empty, Capture also detects the tempo.
- **Audio recording:** record-arm, input monitoring, count-in, punch-in over the loop range, and basic takes.
- **Metronome** with count-in (step 1.7): Tracktion's click, louder on each bar's first beat, and a bar of clicks before recording starts from a stop.

**After v1:** take lanes and comping, and audio Capture. Tracktion's audio Capture needs its project mode.

## 18. Projects, saving and export

**A project is a folder** containing:

| Item | Contents |
|---|---|
| Project file | The project itself |
| `Audio/` | Recordings, and imported audio copied in on import |
| `Generated/` | Generated sounds, with their recipes |
| `Renders/` | Bounces and exports |
| `Backups/` | Earlier versions of the project file |

**Saving:**
- **A new project is untitled until its first save (the owner, 2026-10-16),** as in Ableton Live and FL Studio: the first save (Save, Save As, or Save in the question when closing) asks, with the system's save dialog, what to call it and where it goes, starting in `Documents/Phazite/Projects`, and Phazite makes the project's folder of that name there. Later saves don't ask. Until then the project lives in a folder of its own in the user's application data (`%LOCALAPPDATA%\Phazite\Unsaved Projects` on Windows), so that recordings, imported audio and the autosave have a place from the start; the first save copies its folders across and goes on there with its undo history, and the untitled folder is deleted when the project closes, saved or not. An untitled project isn't in Open Recent, and Export Song offers `Documents/Phazite/Exports` for it. If Phazite didn't close properly while an untitled project had changes saved automatically, the next start offers to restore it before anything else (**Restore** or **Discard them**); untitled folders with nothing to restore are deleted then. A lock keeps a second Phazite running at the same time away from the folders the first one uses (`src/model/project/internal/UnsavedProjects.h`).
- Saves use the engine's safe save, which writes a temporary file and then renames it, and keep a rolling set of backups.
- **Autosave** writes a separate file every few minutes. If Phazite crashed, the next launch offers to restore it.
- **Unsaved changes** (the `*` in the title bar, the question when closing, and what autosave writes) are what the project holds that it didn't hold when it was opened or last saved. Notifications that change nothing don't count, so opening or making a project never shows a `*`, and what a new project starts with (the starter song, an outline's sections) is part of its first save, not an undo step (2026-10-14).
- Project files contain no personal data, and media paths are relative to the project folder ([section 22](#22-privacy-licences-and-trademarks)).
- Phazite's own data is stored in the project file alongside the engine's: sections and the outline style, track keys, and later the chord lane's source and the window layout. One schema file lists all of it with a schema version; a project saved by an older Phazite is converted when it opens, and Phazite saves its own values, never Tracktion's enum numbers (2026-10-14, [section 24](#24-code-structure)). Version 0 is every project saved up to 2026-10-14; version 1 saves the grid's fixed size and key scales by name; version 2 can hold Phazite's own devices, with the sound files on the drum sampler's pads (step 1.6). A converted project has no unsaved changes when it opens, and its first save keeps the old file in `Backups/`. Phazite doesn't open a project saved by a newer Phazite, keeps it in Open Recent, and at startup opens the next recent project instead ([question 17](#25-open-questions), decided 2026-10-16).
- **View state** (track heights, collapsed and pinned tracks, drum mode and drum rows, the grids' settings) is saved with the project, so it reopens as it was left. Changing it isn't an undo step and doesn't count as an unsaved change: no `*` in the title bar and no question when closing, as with the playhead's position (2026-10-14).
- **How the song plays (2026-10-16, [question 18](#25-open-questions)):** whether the loop is on, whether it loops the song or a range, and the metronome's switch are saved with the project too, and, like where the loop is and where playback starts, switching them isn't an unsaved change: they are about playing the song, not the song.

**Export:**
- WAV and FLAC; no MP3 (the owner, 2026-10-16, [question 5](#25-open-questions)).
- Choice of sample rate and bit depth.
- Normalize to a loudness target (LUFS) with a true-peak ceiling.
- Effect tails, and export of a chosen range or sections.
- Stems: one file per track or bus.

**Export Song (step 1.8, 2026-10-15, proposed):** File > Export Song renders the song from its start to its end (the end of the bar its last clip ends in), then its tails until they fall silent, 10 seconds at most; muted tracks stay silent. It writes WAV or FLAC at 44.1, 48, 88.2 or 96 kHz, in 16 bits (dithered), 24 bits, or 32-bit float for WAV. **Normalize** turns the whole song up or down to an integrated loudness (EBU R 128), never taking its true peak over a ceiling; it doesn't limit, so a mix with high peaks can end up under the target, and the report then says by how much and suggests the limiter. The file goes into the project's `Renders` folder, named after the project; it is written next to that name and replaces a file there only once it is complete, so a cancelled or failed export leaves the old one. The export runs in the background with a progress bar and Cancel; playback stops for it. A range, sections and stems come later.

**Bounce and freeze:** bounce in place and per-track freeze; the engine supports both.

**After v1:** DAWproject import and export (the engine has partial support), and archives that include the media.

## 19. Plug-in hosting and stability

**Formats:**
- v1: VST3 on Windows and Linux.
- Later: Audio Units on macOS.
- After v1: CLAP. Neither JUCE nor Tracktion hosts CLAP, so it needs a custom plug-in format built on the MIT-licensed CLAP SDK.
- Later: LV2 on Linux.

**Scanning:**
- Plug-ins are scanned in a separate process, which the smoke test already does.
- A timeout is added, so a hanging plug-in can't stall the scan.
- Plug-ins that fail to scan are listed.

**v1 crash protection:**
- autosave and crash recovery ([section 18](#18-projects-saving-and-export));
- a plug-in that crashed while loading is disabled on the next start and reported.

**Before v1 (the owner, 2026-10-16, [question 7](#25-open-questions)): sandboxed hosting, on by default,** unless measurements show that it costs a lot of performance; then it is a choice per plug-in.
- Plug-ins run in helper processes. If one crashes, its slot shows "Plug-in crashed — Reload" instead of Phazite closing.
- Grouping options trade memory for isolation, as in Bitwig: all together, by vendor, or one per plug-in ([B31](#appendix-b-sources)).
- *Why on by default:* Waveform's sandboxing is off by default, and a MusicTech reviewer lost the whole app to one plug-in ([B9](#appendix-b-sources)).

**Stable parameter identity:** Tracktion identifies hosted plug-in parameters by their index, so saved automation can break when a plug-in update reorders them. Phazite should store each VST3 parameter's ID as well.

## 20. Performance

**Targets** (to be measured; see the [roadmap](roadmap.md)):

| Area | Target |
|---|---|
| Audio | No dropouts at a 128-sample buffer (about 3 ms) with a typical electronic project: 20–40 tracks and a dozen third-party synths, on a mid-range laptop |
| UI | 60 fps scrolling and zooming in the playlist and piano roll with 100+ tracks. Near-zero CPU when idle: only what changed gets repainted |
| Startup | Under 3 seconds to a playable starter project on an SSD, with plug-in scanning in the background |
| Memory | Long audio streams from disk rather than loading into memory |

**Real-time rules:** the audio-thread rules in `CLAUDE.md` apply to all Phazite code.
- **Engine violations:** some Tracktion Engine code breaks those rules:
  - a mutex in every track's volume plug-in;
  - a lock around hosted plug-in processing;
  - automation posting messages to the UI;
  - the recording path.
- **The plan:**
  - measure first, with the sanitizer builds;
  - then, for each problem, report it upstream, work around it, or keep a fork.

  Tracktion takes bug reports on the JUCE forum but doesn't accept third-party pull requests ([B32](#appendix-b-sources)).

**Heavy tracks:** freeze and bounce in place relieve them, and a per-track CPU meter shows where the time goes. After v1, tracks that aren't being edited can be pre-rendered in the background, like Digital Performer's PreGen.

## 21. Accessibility

- **Keyboard:** every action can be reached from the keyboard: the command palette, moving focus between panels, and arrow-key editing of notes and automation points.
- **Screen readers:**
  - Every custom control gets a JUCE accessibility handler with a name, role and value.
  - JUCE supports Windows UI Automation and macOS; it has no Linux back end yet ([B33](#appendix-b-sources)).
- **Vision:**
  - themes meet WCAG 2.2 contrast: 4.5:1 for text and 3:1 for controls ([B34](#appendix-b-sources));
  - color is never the only signal: aliases, for example, show an icon as well as a tint;
  - UI scaling and HiDPI apply everywhere, including plug-in windows.
- **Benchmarks:** REAPER with OSARA, and Live 12.

## 22. Privacy, licences and trademarks

**Privacy:**
- **No account and no telemetry by default.** Any future crash reporting is opt-in.
- **No personal data in project files.** Tracktion writes the operating system user's full name into every saved project (`modifiedBy` in `tracktion_Edit.cpp`). Phazite overrides it before it saves any project.
- **API keys** for optional cloud AI (later) are stored in the operating system's credential store, never in projects or settings files.
- **Downloads (2026-10-13):** Phazite connects to the internet only when the user asks it to download something, so far only Phazite Sounds (from GitHub, over HTTPS). It checks each download against the SHA-256 it was built with.

**Licences:**
- **Compatibility:** only AGPL-compatible code, models and data; the list is in `CLAUDE.md`.
- **Notices:** third-party notices ship with the app, for example SoundTouch (LGPL-2.1-or-later), Signalsmith Stretch (MIT) and the Inter font (SIL OFL 1.1).
- **Fonts:** fonts may use the SIL Open Font License 1.1. They ship as separate files with their licence text.
- **Sounds:** Phazite Sounds are CC0 1.0 and a separate download from their own repository; the program never includes sounds.
- **Proprietary options:** Tracktion's proprietary options stay off: Elastique, REX and ReWire.

**Trademarks:**
- VST is a registered trademark of Steinberg Media Technologies GmbH.
- ASIO is a registered trademark of Steinberg Media Technologies GmbH.
- Other product names in this document are trademarks of their respective owners. They are used only to describe and compare.

## 23. Tracktion Engine: what it gives and what we build

**What the engine gives** (it ships without UI):
- Tracks, folders and buses; audio, MIDI and step clips; linked clips.
- Automation with read, touch, latch and write modes; clip automation; six modulator types and macros.
- Sends, sidechains, freely wired racks, and delay compensation across the whole graph, on a multithreaded audio graph.
- Recording with takes, comping and a retrospective buffer.
- Freeze, bounce in place, rendering with loudness normalization, and stems.
- Time-stretching and pitch-shifting: SoundTouch, which Phazite uses, and, behind one compile flag, the MIT-licensed Signalsmith Stretch, which pitch-shifts more cleanly but can't be used yet (watch-outs, below).
- Partial DAWproject support and clip-launcher machinery.
- Scales, chords, key detection from MIDI notes, and a chord, bass and arpeggio generator.

**What Phazite builds:**
- All of the UI. This is by far the largest piece of work: Waveform, on the same engine, shows that the interface is where DAWs differ ([B9](#appendix-b-sources)).
- The model between the UI and the engine: an API for each area of the project, with Phazite's own IDs and values, and the schema of what a project saves ([section 24](#24-code-structure)).
- Commands and keymaps, selection and copy/paste handlers, and named undo.
- Section editing, the chord format, the song outlines and the progression library.
- Built-in devices with editors, the drum synth and the sound generator.
- Autosave, crash recovery, presets and templates.
- A scan timeout, and plug-in sandboxing before v1 (step 3.3). After v1: CLAP.

**Watch-outs, from reading the engine source:**
- **Personal data:** saved projects contain the operating system user's full name (see [section 22](#22-privacy-licences-and-trademarks)). Handled in step 0.2: `phazite::PropertyStorage` gives no user name.
- **UI hooks:** MIDI learn, app commands, plug-in windows and freeze need a `UIBehaviour`. Step 0.2 added `phazite::UIBehaviour`.
- **Edit loading:** `EditLoader` builds projects on a background thread. That breaks the `CLAUDE.md` rule that Tracktion objects are created on the message thread, so Phazite loads them with `phazite::loadEdit()` (step 0.2).
- **MIDI device scan:** `DeviceManager::initialise()` asks for a MIDI scan in 5 ms, then restarts the same timer with the 4-second hot-plug check, so MIDI inputs would appear 4 seconds after startup. `phazite::createEngine()` asks for the scan again (step 0.7).
- **State written while playing:** Tracktion writes the playhead position into the project, and adds input-device entries when it prepares playback. `phazite::Project` doesn't count those as unsaved changes (step 0.7).
- **Notifications that change nothing:** after every tempo update (one follows loading an Edit) each MIDI clip notifies its mute switch without changing it, and linked clips copy their notes to each other again; JUCE also notifies a value written again as another type. `phazite::Project` keeps a copy of the project as opened or last saved, and counts an unsaved change only when what counts differs from it (2026-10-14).
- **Clips added in code:** Tracktion puts a track's clips in order only asynchronously after one is added, as an undo step that marks the Edit changed; it does so while it loads a project. So `phazite::Project::create()` saves what a new project starts with and opens the project from that save (2026-10-14).
- **Project IDs:** `te::loadEditFromFile` replaces the ID saved in a project with the one it is given, but the Edit names its temporary folder (freeze files, comp renders) after that ID. `phazite::Project` reads the saved ID first, so a project keeps it, and gives a Save As copy a new one (step 0.5).
- **Stretched clips' timing (measured 2026-10-17):** a clip that follows the song's tempo or is pitched plays from a copy Tracktion renders in the background (a proxy), and that render takes no account of the stretcher's delay. With SoundTouch, what Phazite uses, such clips play early: 13 ms when a loop follows the tempo at its own tempo, about 20 ms when pitched, and 20 to 28 ms when stretched (from 120 to 100 BPM). With Signalsmith Stretch they would play late: 100 to 134 ms. Measured by where the energy of clicks on each beat lands, rendering offline as export does. Signalsmith sounds cleaner (pitch-shifted a voice and a chord, it leaves 9 and 15 dB less energy outside the notes than SoundTouch), but it takes about three times as long to render (3 to 4 seconds for a 3-minute recording, against 1.3), and stretching as it plays, as the browser's preview does, it costs ten times as much and spends 45% of an audio block catching up with its delay each time a preview loops. Phazite keeps SoundTouch until the timing is fixed in Tracktion: report it upstream, or fix it in a fork (the owner's call, question 4); then Signalsmith can render clips, with SoundTouch for previews. The early clips are known bug 1 ([known-bugs.md](known-bugs.md)), to fix later (the owner, 2026-10-18).
- **Undoing a parameter change:** undo puts back a parameter's stored value (a track's volume, say) but not the value the engine plays with. `phazite::undo()` and `redo()` set every parameter that isn't automated to its stored value afterwards. Tracktion only stores a value once it first changes, and undoing that first change removes it again, so Phazite's controls store the value, without an undo step, before changing it; otherwise the fix would cost the redo history (step 0.6). Worth reporting upstream.
- **Mute and solo** aren't part of the undo history.
- **Track insert points:** `TrackInsertPoint (track, false)` means "after this track", except for the first top-level track, where it means the start of the list. `phazite::getInsertPointAfter()` names the track explicitly (step 1.1).
- **Aliases:** changes to linked clips are mirrored without undo, so Phazite's alias commands must edit every linked clip inside one undo step.
- **Folder buses:** a submix folder has no output of its own; it uses its first child track's output. The bus routing UI has to account for this. Adding a plug-in to a plain folder with `PluginList::insertPlugin()` shows Tracktion's "Converted to submix track" message, so `phazite::makeBus()` adds the bus's volume and meter to the folder's state itself (step 1.4).
- **Sections and time edits:** Tracktion's `insertSpaceIntoEdit()` and `deleteRegionOfTracks()` split and trim arranger clips like any other clip, so inserting time inside a section would cut it in two. Phazite's own time edits (`src/model/arrangement/internal/Arrangement.h`) make a section longer or shorter instead (step 1.2).
- **Note selections:** Tracktion's `SelectedMidiEvents` keeps plain pointers to the clips whose notes it selects, so the piano roll lets go of it before a clip it shows is deleted (step 1.3).
- **Guide notes:** a track plays guide notes (the piano roll's audition) only if `EngineBehaviour::shouldPlayMidiGuideNotes()` is true, which it isn't by default, so the piano roll forces them (step 1.3).
- **Built-in devices:** most of Tracktion's effects lock, allocate or read `juce::CachedValue`s on the audio thread ([real-time audit](realtime-audit.md), finding 16); Phazite offers its own devices instead (step 1.6) and none of Tracktion's; projects that hold Tracktion's 4OSC synth or reverb, offered until Phazite's own came, still open and play.
- **Plug-in parameters:** hosted plug-in parameters are identified by index ([section 19](#19-plug-in-hosting-and-stability)).
- **Real-time rules:** several engine paths break them ([section 20](#20-performance)).

**How Phazite depends on it (decided 2026-10-14): an engine of Phazite's own is a hedge, not a goal.** Phazite stays on Tracktion Engine.
- *Why:* Tracktion is about 244,000 lines and Phazite about 25,000. A rewrite leaves features behind for a long time (Audacity 4.0 shipped without MIDI tracks or the mixer, [B49](#appendix-b-sources)), and Tracktion's lead developer explains the cost of writing an engine in "Why you shouldn't write a DAW" ([B55](#appendix-b-sources)).
- The code is structured so that a part could be replaced: after architecture phase A3, only the model uses Tracktion, behind Phazite's own functions, IDs and values ([section 24](#24-code-structure)).
- A part is replaced only where Phazite gains something, one capability at a time (the tempo model, the playback graph or plug-in hosting, say), each after a decision dated in the [decision log](#26-decision-log). An interface goes in front of that part, the replacement is built behind it, and the old code is deleted after the switch ([B54](#appendix-b-sources)). The golden tests, which compare saved projects and short renders with committed files, are the safety net.
- Phazite's own devices are engine-neutral from step 1.6: DSP with a thin Tracktion plug-in around it.
- Engine problems are still fixed with workarounds first, and a fork if those get awkward ([question 4](#25-open-questions)).

## 24. Code structure

*Decided 2026-10-14*, after a review of Phazite's code and of how other open-source DAWs keep their layers apart ([appendix A](#appendix-a-research-summary)). The structure aims for three things:
- the UI can be redesigned without touching the rules of editing;
- every edit can be tested without a window, and later run by scripts and the assistant, through commands;
- a part of the engine could be replaced without rewriting the UI ([section 23](#23-tracktion-engine-what-it-gives-and-what-we-build)).

The roadmap's [architecture track](roadmap.md#architecture-track) gets there in phases A0 to A5. After each step the app builds, behaves the same and passes its tests.

**Layers.** Each row uses only the rows below it:

| Folder | What it holds | Built into |
|---|---|---|
| `src/app/` | Creates everything and connects it: startup, the main window with its menus and dialogs, and the open project. It owns the services and hands them to what needs them | `phazite_app`, a library the tests link, and `Main.cpp`, the program |
| `src/ui/` | What the user sees and touches: `theme/` (design tokens, look and feel, icons), `components/` (the base controls), `input/` (keys, keymaps, the computer keyboard, keys over plug-in windows, drag and drop), `devices/` (device menus and windows), and a folder for each view: the time line, the playlist, the piano roll, the mixer, the browser and the window's shell | `phazite_ui` |
| `src/services/` | What the app keeps outside any project: settings (every key in one list), recent projects, the Phazite Sounds download and the browser's library index | `phazite_core` |
| `src/model/` | The project model: a folder for each area (`arrangement/` for tracks, clips, sections and the grid, `notes/`, `mixer/`, `transport/`, `project/`, `engine/`), each with the functions the UI calls; `commands/`, the command registry without UI code, with undo; and `base/`, which every area builds on: the one header that brings in Tracktion, and the schema of what a project saves, with its migrations | `phazite_core` |
| `src/devices/` | From step 1.6: Phazite's own instruments and effects, as DSP that doesn't depend on Tracktion, each in a thin Tracktion plug-in | `phazite_core` |
| Tracktion Engine and JUCE | The project store (Tracktion's Edit) and the audio engine; JUCE for windows, audio devices and plug-in hosting | `phazite_core` |

**Rules.** From phase A0, `tools/check-architecture.sh` counts what breaks rules 1 to 4 and 7 in CI and before every commit. Where code still breaks a rule, the count has a limit that can only go down, so nothing new breaks it while the old cases are fixed; a commit that raises a limit on purpose says why in its message, and CI checks that it does.

1. **Dependencies point down only:** the app uses the UI, the UI uses the services and the model, and the services may use the model; never the other way round.
2. **No window code in the model:** the model and the services include none of JUCE's window modules (`juce_gui_basics`, `juce_gui_extra`) and name no window class; key and mouse handling, pixel sizes and how values are shown belong in the UI. For now the model may still use `juce_graphics`, for color values. *None has since phase A1: the model's hook for devices' windows moved to the app, and the architecture check counts no window module or window class in the model or the services.*
3. **The UI decides nothing about the project:** it never changes the project or starts an undo step; it runs a command or calls a model function. *None since phase A2 (2026-10-15): the UI began 46 undo steps and changed the project directly in about 20 places when A2 began, and the architecture check counts no undo step begun in the UI.*
4. **Phazite's own types across boundaries:** objects pass between layers as IDs and plain values, never as Tracktion pointers. The UI doesn't use Tracktion; in the model, only the code behind each area's functions does. *At the end of phase A1 the UI names Tracktion in 546 places and includes it in 7 files (the budgets in `tools/architecture-budget.txt`); 279 and 2 once A3 had done the mixer, the top bar and the time line, and none once it had done the playlist, the piano roll and the browser (2026-10-16); none after phase A3, which ended on 2026-10-16 with the UI library compiled without Tracktion's include path. In the model, only `base/internal/Tracktion.h` includes it, and every header that names a Tracktion type is in its area's `internal/` folder, which the UI doesn't include.*
5. **Tokens and strings:** every color, size, corner radius and font in the UI comes from a design token, and every text the user reads from a strings table (phase A4).
6. **Fast data is polled:** meters, the playhead and other values that change many times a second are read from small read-only sources on a timer, never stored in the project or sent as change messages (phase A3).
7. **One named undo step per change:** every change to the project happens inside one named undo step, owned by a command or by the model function the UI called; from phase A2, a drag is a gesture session that opens one step and keeps or rolls it back when the drag ends. Changes that are deliberately not undo steps (mute and solo, the loop, view state) go through model functions too.

**What stays the same, on purpose:**
- Tracktion's Edit stays the single store of project data, as Tracktion's developers advise ([B53](#appendix-b-sources)) and as Audacity 4 kept its old storage ([B48](#appendix-b-sources)).
- One library, `phazite_core`, compiles JUCE and Tracktion once for every program.
- Commands stay the public API: menus, shortcuts, the palette, macros and named undo use them, and later scripts and the assistant will ([section 12](#12-commands-keys-and-help)).
- The app passes each object the services it needs; at Phazite's size that is simpler than a dependency-injection framework.
- The busiest views (the playlist, the piano roll, the meters) stay painted in C++; a designer works through the tokens and the gallery app ([design language](#design-language)).
- The real-time rules in `CLAUDE.md` don't change.

**Project data (decided 2026-10-14):**
- Tracktion's Edit stays the project store: a `.phazite` file is the Edit saved as XML, with Phazite's own properties on Tracktion's nodes and in a `PHAZITE` branch of its own.
- One schema file names every property Phazite saves, marks which are view state ([section 18](#18-projects-saving-and-export)), and carries a schema version. A project saved by an older version is converted when it opens, by migrations kept next to the schema, each with a test. Phazite doesn't open a project saved by a newer Phazite ([question 17](#25-open-questions)), so nothing it holds is lost.
- Phazite saves its own values, never Tracktion's enum numbers, so a Tracktion update can't change what a saved project means. The two it saved that way before (the grid's fixed size, and the scale of a track's key and of the key chosen last) are converted.
- Notes have stable IDs saved in the project (phase A3; project format version 3, 2026-10-16), so that selections, undo and scripts can refer to one note reliably: each note has one that no other note of its clip has, and a copy of a clip, an alias too, keeps them (`schema::phaziteNoteID`).

## 25. Open questions

1. ~~AI engines in v1~~ **Decided:** algorithmic in v1, trained models later ([section 16](#16-ai-and-generative-features)).
2. ~~Outline styles first~~ **Decided:** pop, rock, electronic, and hip-hop/trap ([section 13](#13-getting-started-and-song-outlines)).
3. ~~UI font~~ **Decided:** Inter, bundled, under the SIL Open Font License; `CLAUDE.md` now allows the OFL for fonts ([design language](#design-language)).
4. ~~Engine fixes~~ **Decided:** try simple workarounds first. If they make Phazite's code too awkward, or upstream issues block us, keep a fork of Tracktion Engine. Problems are reported upstream either way ([roadmap](roadmap.md), step 0.3). Replacing a part of the engine outright is a separate choice, made one capability at a time after a dated decision ([section 23](#23-tracktion-engine-what-it-gives-and-what-we-build), 2026-10-14).
5. ~~MP3 export~~ **Decided (2026-10-16, the owner):** no MP3 export. Phazite exports WAV and FLAC.
6. ~~Airwindows~~ **Decided (2026-10-16, the owner):** plug-ins that aren't Phazite's own, such as Airwindows' effects and Surge XT, are never built into Phazite. Phazite may offer them as optional downloads, as it offers Phazite Sounds, and its own devices keep one design language ([section 9](#9-devices-containers-and-presets)).
7. ~~Plug-in sandboxing~~ **Decided (2026-10-16, the owner):** before v1, on by default, unless it costs a lot of performance: other people's plug-ins mustn't be able to crash Phazite ([section 19](#19-plug-in-hosting-and-stability)).
8. ~~Ableton Live keymap~~ **Decided (2026-10-16, the owner):** in v1.
9. ~~Single-key tools and the computer keyboard~~ **Decided:** Ableton Live's way. While the computer keyboard plays notes, Shift and a letter run that letter's single-key shortcut ([section 12](#12-commands-keys-and-help)).
10. ~~The computer keyboard at startup~~ **Decided (2026-10-16):** it starts off, so that the tools' letters (P, S, T, D, E, C, Z) work; M or the top bar's button turns it on, and Phazite remembers the choice. The digits 1 to 9 pick the tools either way.

Raised by the mixer research (2026-10-13), each with what the research recommends:

11. ~~Folders as buses~~ **Decided (2026-10-16):** every new folder is a bus, as group tracks are in Ableton Live and Bitwig and folders in REAPER; the Bus switch can still make it plain ([section 8](#8-the-mixer)).
12. ~~Choosing where a track's sound goes~~ **Decided (2026-10-16):** the Out line becomes a menu: Master, the folder buses, and a new bus from the selected tracks. A folder is the bus of the tracks in it, so choosing a bus moves the track into that folder, and Master moves it out of its buses, as REAPER's folders and Bitwig's groups route what is in them: the playlist and the mixer then never disagree about where a sound goes. (Tracktion can't feed a folder's bus from a track outside it.) Sending a track's output straight to another track, as FL Studio's "route to this track only" does, can come later. Built in step 2.9.
13. ~~Sends~~ **Decided (2026-10-16):** a send knob for every return on every strip, off until first touched, as in Ableton Live and Bitwig: one sentence explains it to beginners, and nothing has to be added first. "+ Send" stays, to make a new return. Built in step 2.9.
14. ~~New tracks~~ **Decided (2026-10-16):** "+ Track" makes an empty track, as in Ableton Live, FL Studio, REAPER and Bitwig; the browser, the plug-in picker and **Choose instrument** give it an instrument.
15. ~~Starter project~~ **Decided (2026-10-16):** no reverb return at first. Most DAWs' new projects have none (Ableton Live's default set is the exception), it would use CPU while nothing is sent to it, and "+ Send" makes a return with a reverb in one step.

Raised by the third round of feedback (2026-10-13):

16. ~~Keys over plug-in windows~~ **Decided (2026-10-16):** as proposed, a "Send All Keys to the Plug-in" switch in each plug-in window's menu, as REAPER and FL Studio have, for plug-ins that draw their own text fields without JUCE, where Phazite can't tell that a name is being typed. It is off at first, and remembered for that plug-in in every project, since such a plug-in needs it everywhere ([section 9](#9-devices-containers-and-presets)). Built in step 2.12.

Raised by the architecture review (2026-10-14):

17. ~~Projects from a newer Phazite~~ **Decided (2026-10-16):** as proposed: Phazite doesn't open a project a newer version saved, whose data it might misread or drop when saving; it says that a newer Phazite saved it, keeps it in Open Recent, and at startup opens the next recent project instead. An untitled project a newer Phazite left to restore waits for that version too. (The alternatives were opening it read-only, or after a warning.)
18. ~~Choosing what loops~~ **Decided (2026-10-16):** turning looping on or off, and switching between looping the song and a range, are treated like the loop's position: saved, but not unsaved changes. So is the metronome's switch, which step 1.7 had counted as the loop's switch was ([section 18](#18-projects-saving-and-export)).

Raised by the golden tests (2026-10-14):

19. ~~What a duplicated time range does to what is after it~~ **Decided (2026-10-16):** each piece of the copy replaces what it lands on, as REAPER's razor edits do by default and as pasting in Ableton Live overwrites, so nothing plays twice. (The alternatives were clearing the whole stretch after the range on its tracks, which also deletes clips on tracks where the range was empty, or laying the copy over what is there, as FL Studio's playlist does.)

## 26. Decision log

| Date | Decision |
|---|---|
| 2026-10-10 | Licence AGPL-3.0-or-later; Tracktion Engine and JUCE; VST3 first; Windows first; ASIO on Windows |
| 2026-10-12 | The structure in this document is accepted in outline |
| 2026-10-12 | Each track is its own mixer channel; no channel rack |
| 2026-10-12 | The clip launcher waits until after v1 |
| 2026-10-12 | Keys and mouse: whatever users find most intuitive. FL Studio-style keys and a Pro Tools-style Smart Tool, plus a first-launch keymap picker, validated with users |
| 2026-10-12 | AI in v1: chord suggestions and sound generation, useful through project context. No stem separation; the assistant comes later |
| 2026-10-12 | Song outlines and chord progression outlines for newcomers |
| 2026-10-12 | The v1 AI engines are algorithmic (music theory, synthesis and evolution); trained models later |
| 2026-10-12 | First outline styles: pop, rock, electronic, and hip-hop/trap |
| 2026-10-12 | Typography: bundle Inter (SIL OFL 1.1) as separate files, as part of a design language with color, spacing, icon and motion tokens |
| 2026-10-12 | Engine problems: simple workarounds first; fork Tracktion Engine if workarounds get awkward or upstream issues block us |
| 2026-10-12 | Step 0.7 defaults: the computer keyboard plays notes, laid out as in Ableton Live and Logic Pro; live MIDI plays the first track with an instrument until tracks can be selected |
| 2026-10-12 | Icons: Lucide (ISC), step 0.6 |
| 2026-10-12 | Single-key shortcuts and the computer keyboard: Ableton Live's way, Shift and a letter for the shortcut while the keyboard plays notes |
| 2026-10-12 | Step 1.1 defaults: the playlist's time axis is in beats, as in FL Studio and Ableton Live, so tempo changes don't move the grid; the grid adapts to the zoom unless a fixed size is picked; live MIDI follows the selected track (like FL Studio's selected channel); Delete deletes the selection and selects the next track, as in Ableton Live; track heights are saved in the project but aren't undo steps |
| 2026-10-12 | Smart Tool defaults until the milestone 1 usability test settles them: Ctrl+drag copies a clip and Ctrl+Shift+drag makes an alias, in both keymaps (FL Studio's Shift+drag clone isn't mapped yet); Alt places freely; clips show their track's color unless given their own; Duplicate puts copies of clips right after them, as FL Studio's Ctrl+B does |
| 2026-10-12 | Step 1.1 tools: FL Studio's letters in both keymaps, Esc for the Smart Tool; Draw places an alias of the last MIDI clip used (FL Studio's pattern reuse) or a copy of the last audio clip; with FL Studio's keymap right-click deletes clips, otherwise it opens a menu (a keymap setting, `rightClickDeletes`). Phazite's keymap takes Ableton Live's editing keys (Ctrl+E split, Ctrl+I insert time, Ctrl+Shift+Delete remove time, arrows nudge, 0 mutes), FL Studio's keymap its own (Shift+arrows nudge, Alt+M mutes) |
| 2026-10-12 | Step 1.1 folders and files: a new folder is plain (its volume works as a VCA) and becomes a bus when it holds an effect; Ableton Live's keys group (Ctrl+G), ungroup (Ctrl+Shift+G) and collapse (U); collapsed and pinned states are saved but aren't undo steps. Dropped files are copied into the project's `Audio` folder (paths relative to the project); the first goes on the track it is dropped on if that suits it, and the others on new tracks named after them and colored by the words in their names |
| 2026-10-12 | Step 1.2 defaults: a dragged section goes in at the section boundary nearest the pointer, and the sections between make room, as with Studio One's arranger track; Ctrl copies it and Ctrl+Shift copies it as aliases. Dragging a section's right edge inserts or removes time at its end, so the rest of the song moves along; Shift resizes the section alone. Clips are split at a section's edges so that what is inside goes along, automation moves with them, and tempo and time-signature changes stay where they are when a section moves. Each outline section has an energy level (drawn as the curve) and a tip (shown in the hint bar), in the outline's data file. Section colors follow their role, from the track palette |
| 2026-10-12 | Step 1.3 defaults: the piano roll follows the MIDI clips selected in the playlist and edits several at once. As in FL Studio, new notes take the length and velocity of the note clicked last, a drag that follows the click moves the new note, Ctrl+drag selects in a rectangle and Shift+drag copies. Ghost notes show every other track's notes until they can be chosen per track, and a double-right-click on one edits its clip (as in FL Studio: a click adds a note). With notes selected, the note keys come before the playlist's: Ableton Live's in Phazite's keymap (the arrows transpose and move, Shift+Up and Shift+Down by octaves, Shift+Left and Shift+Right change the length), FL Studio's in its keymap. Drum mode is on for tracks whose instrument is a sampler, and the rows' order and hidden rows are kept in the track without undo steps. Quantize's swing makes every other grid line up to a third of a step late (a triplet feel at 100 %), and each transform keeps the settings used last |
| 2026-10-12 | Step 1.4 defaults: devices go before a channel's sends and fader, instruments first, and instruments only on tracks. A folder becomes a bus when it gets a device, its fader starting where the plain folder's volume was, and turns plain again when its last device goes. A new send starts at −6 dB, after the fader. The loudness meter measures the master's devices, before the master fader. Until step 1.6 the device list offers Tracktion's reverb and 4OSC synth only, besides scanned plug-ins, because Tracktion's other devices break the real-time rules. Device windows stay above the main window while Phazite is in front |
| 2026-10-13 | After trying steps 1.1 to 1.4: a track's color runs through its whole row; dragging past an edge scrolls; sections become a band in the time bar and ghosts behind the tracks, with no row or Song menu of their own, and four more outlines (lo-fi, R&B, drum and bass, techno); the piano roll counts bars from the clip's start and opens there, shows a selected track's clip at or nearest the playhead, and on a track without clips the first note starts one; a key is a track's, off until chosen, and the top bar shows none; drum mode has no button; the menu bar is File, Edit, Add, View, Options and Help; "+ Track" sits over the track headers, with a row after the last track that adds one; the Smart Tool's fades are for audio clips, and short clips move from anywhere but their edges |
| 2026-10-13 | Keys: a held key repeats only the commands that take a step; tool keys spring back after a hold with the mouse, as in Bitwig and Ableton Live; in Phazite's keymap the digits 1 to 9 pick the tools and M turns the computer keyboard on and off |
| 2026-10-13 | Second round: playback loops the whole song and the loop grows with it, as in FL Studio's song mode, unless a selection or sections are looped on purpose; a Loop button and L turn looping on or off. A track moves only by the handle in its header. Tracks collapse to a thin row (the arrow in the header, U, or dragging the bottom edge past the smallest height). Track colors are stronger in headers and rows, with black or white names, whichever reads better. With FL Studio's keymap a click on empty space places the last clip used |
| 2026-10-13 | Mixer redesign, after research into the mixers people praise: a strip reads top to bottom as the sound goes, with the instrument in a slot of its own above the effects, and "+ Effect" and "Choose instrument" menus that list only what they add; a track with notes but no instrument asks for one, in the mixer, the inspector and the piano roll. "+ Send" can make a return with an effect in one step, named after it; returns are solo-safe and sit together next to the master; folders get a Bus switch; selecting a channel outlines what it feeds and what feeds it. Mute and solo stay separate buttons, and a solo never changes the mute switches (FL Studio's destructive solo is a long-standing complaint); right-click M solos a track alone, as FL users expect. Ctrl+L loops the selection, or the song with none; with FL Studio's keymap a selected time range loops while it is selected; playback that doesn't loop stops at the song's end |
| 2026-10-13 | Third round, after the owner tried the mixer: Space stops back at the play start, which a click in a time ruler sets and which is the song's start in a project just opened, as in FL Studio, Ableton Live, REAPER and Waveform; Shift+Space pauses (Ctrl+Space in FL Studio's keymap). Play, stop and the computer keyboard work while a plug-in window has the keyboard focus, through a message hook installed before any plug-in's. Each effect has a Mix knob, as FL Studio's mixer slots have, with its limits documented (no latency compensation or ramping yet). The inspector's channel strip fills its height |
| 2026-10-13 | Step 1.5 defaults, after research into the browsers people praise: one tree of places, as in FL Studio, where one click opens a folder; clicking a sound or moving to it with the arrow keys previews it (on by default), Esc stops it, and Shift+Enter or Right plays it again; loops preview at the song's tempo and start on its bar while it plays, and keep their own pitch until Phazite has a song key to match; Enter or a double-click puts the item on the selected track at the play start; a dropped loop follows the song's tempo, as it previewed; F8 is the plug-in picker in both keymaps, Phazite's keymap moves the browser to Ctrl+Alt+B and adds Ctrl+F for its search; favorites by a star; Recent keeps the last 50 items put into the song; "Sounds by Type" lists every library's sounds by tag. Phazite's own sounds are CC0 only: CC0 libraries plus Phazite's own synthesis, each recorded with its source |
| 2026-10-13 | Phazite Sounds become an optional download, kept out of Phazite's repository and installer: the releases of their own public repository (phazite-sounds), pinned in Phazite by address, size and SHA-256. Phazite asks once, in a welcome window that will grow into first-run setup with more pages; the browser offers them until they are installed, and a zip downloaded by hand installs too |
| 2026-10-14 | Code structure, after a review of Phazite's code and of how Ardour, MuseScore, Audacity, Zrythm, ossia score and LMMS keep their layers apart: the layers, folders and seven rules of [section 24](#24-code-structure). The app uses the UI, the UI the services and the model; the model has no window code; the UI runs commands and calls model functions rather than changing the project or starting undo steps, and in time sees only Phazite's own types. `tools/check-architecture.sh` checks the rules in CI and before every commit, with limits that can only go down. The roadmap's architecture track (phases A0 to A5) does the work |
| 2026-10-14 | An engine of Phazite's own is a hedge, not a goal: Phazite stays on Tracktion Engine and replaces a part only where it gains something, one capability at a time, each after a decision dated in this log, behind an interface and with the golden tests as the safety net. Phazite's own devices are engine-neutral from step 1.6 either way. Engine problems are still fixed with workarounds first and a fork if those get awkward (2026-10-12) |
| 2026-10-14 | Projects: Tracktion's Edit stays the project store. Phazite adds a schema version, keeps the list of what it saves and the migrations of older projects in one place, and saves its own values rather than Tracktion's enum numbers; the two saved that way so far (the grid's fixed size and the scale of a key) are converted when an older project opens |
| 2026-10-14 | Notes get stable IDs saved in the project, so that selections, undo and scripts can refer to a note reliably (architecture phase A3) |
| 2026-10-14 | View state (track heights, collapsed and pinned tracks, drum mode and drum rows, grid settings) stays saved in the project, but changing it no longer counts as an unsaved change, as with the playhead's position |
| 2026-10-14 | Architecture phases A0 and A1 (golden tests and the architecture check, then untangling the model without changing what the app does) come now, before step 1.6, so that the new devices start on the new structure |
| 2026-10-14 | Designer workflow: the design tokens move into a file in the DTCG format, which a designer edits or exports from Figma, Penpot or Tokens Studio, and a gallery app shows every control and view in all three themes (architecture phase A4). A declarative layout tool (JIVE or foleys_gui_magic) only if dialogs become a bottleneck |
| 2026-10-14 | Go to End goes to the song's end, where the loop ends: the end of the bar its last clip ends in, as FL Studio ends a song for export. Sections don't count, even in a song without clips |
| 2026-10-14 | A note edit makes its clip longer only into the empty space after it, never over the next clip on its track, as Bitwig extends clips and as Studio One can keep events from overlapping; a note that doesn't fit stays in the clip without playing. A looped clip doesn't grow |
| 2026-10-14 | Duplicate works on a selected time range, as in REAPER and Ableton Live and as FL Studio's Ctrl+B repeats its time selection: what lies in it on its tracks goes again right after it, each piece replacing what it lands on (open question 19), and the range moves to the copy so that Duplicate again repeats it, and back on Undo, as REAPER's undo puts razor edit areas back; the clips in the range aren't cut, and Duplicate as Alias stays for clips and sections |
| 2026-10-15 | Step 1.6 begins while the owner is away, with these as proposals until the owner decides: Phazite's devices have plain names (EQ, Compressor, Limiter, Reverb, Synth, Drum Sampler); each of Phazite's effects has a Mix of its own, delayed to match the effect and ramped; the EQ has eight switchable bands with the defaults in [section 9](#9-devices-containers-and-presets), its editor's dots move a band's frequency and gain as one undo step, and its slot's key controls are the shelves' gains; projects that hold Phazite's devices are format version 2, which an older Phazite won't open |
| 2026-10-15 | Proposals for the compressor, until the owner decides: the defaults in [section 9](#9-devices-containers-and-presets), stereo-linked peak detection with attack and release as time constants, a low cut on what it listens to, and a sidechain that can listen to any track that wouldn't make a loop, muted or not, with "Off" meaning its own sound |
| 2026-10-15 | Proposals for the limiter, until the owner decides: the defaults in [section 9](#9-devices-containers-and-presets), a fixed 2 ms look-ahead (no knob), true-peak detection always on, and a mix like every other effect's, though parallel limiting lets peaks through |
| 2026-10-15 | Proposals for the reverb, until the owner decides: the controls and defaults in [section 9](#9-devices-containers-and-presets), with the decay measured on the bass and a 25 % mix on an insert, and it replaces Tracktion's reverb in the device list (projects that hold Tracktion's still open and play) |
| 2026-10-15 | Proposals for the synth, until the owner decides: the controls in [section 9](#9-devices-containers-and-presets) with their defaults (two detuned saws through a 3 kHz low-pass that its envelope opens two octaves, a 2 ms attack, 80 % sustain and a 200 ms release, at -6 dB), 16 voices, the mod wheel on the LFO's depth, and no unison, chorus or wavetables in this first version |
| 2026-10-15 | Proposals for the drum sampler, until the owner decides: 16 pads in General MIDI's layout from note 36, one-shots only (no gated or looping pads yet), a synthesized default kit, the hats in one choke group, linear velocity, files copied into the project's Audio folder, sounds up to 30 s, and a length knob whose longest setting plays the whole sound |
| 2026-10-15 | Step 1.7 begins while the owner is away, with these as proposals until the owner decides. The metronome is Tracktion's click, louder on each bar's first beat; the project keeps whether it is on and counts switching it as a change to save, as it does the loop's switch, but it isn't an undo step. The count-in is a bar of clicks before recording starts from a stop, heard with the metronome off; it is the app's setting for every project, off at first. Keys: O in Phazite's keymap (Ableton Live's), and Ctrl+M and Ctrl+P (FL Studio's metronome and precount) in FL Studio's. A right-click on the metronome button offers the count-in |
| 2026-10-15 | Proposals for the tempo and time signature, until the owner decides: the top bar's tempo is a field to drag up or down (a beat per minute every 2 pixels), scroll, step with the arrow keys, or click and type (to a hundredth), as in Ableton Live; each drag, typed tempo, notch or key press is one undo step, "Change Tempo", and clips keep their beats. A click on the time signature offers 2/4, 3/4, 4/4, 5/4, 6/8, 7/8, 9/8 and 12/8. A song has one tempo and one time signature, from its start, until a tempo lane comes |
| 2026-10-15 | Proposals for MIDI recording, until the owner decides: Record (R, in both keymaps: FL Studio's key, in its manual; Logic Pro's in Apple's guide for the iPad and in third-party lists, though Apple's guides for the Mac that were found don't confirm it) records the computer keyboard and MIDI keyboards onto the track live MIDI plays (the selected track with an instrument), with nothing to arm, from where playback starts after the count-in, or from where the song is if it plays already. Overdub is on at first: the notes join the clips already there; off, they take those clips' place. Quantize Recordings (off at first) moves notes to the nearest sixteenth as they go in. Both are the app's settings. The song's own loop doesn't loop a recording, which runs on past the song's end, and the loop is back around the song afterwards; a range looped on purpose loops the recording, if it is at least 2 seconds long (Tracktion's limit). Each take is one undo step, "Record", and stopping goes back to where recording started. Live MIDI's track stays armed, invisibly, so that Record starts at once |
| 2026-10-15 | Proposals for Capture, until the owner decides: Ctrl+Shift+C in both keymaps and a button in the top bar. (Checked later the same day: Ableton Live 12's manual gives Capture MIDI no key, and uses Ctrl+Shift+C for Copy Time in the Arrangement, which Phazite doesn't have, [B60](#appendix-b-sources); the third-party lists that called it Capture's key were wrong.) Capture takes the last 30 seconds Tracktion keeps of what was played, recording or not. While the song plays, the notes stay where they were played; while it is stopped, they go at the bar the play start is in, or after the track's clips, the first note on the bar's first beat. In a song without clips, Capture first sets the tempo from the notes: the whole BPM between 80 and 160 at which they fall best on sixteenths, with more of them on beats, nearer 120 when tempos fit as well, and none when no tempo fits. The clip runs whole bars, its notes quantized when recordings are. It is one undo step, "Capture", the new tempo with it |
| 2026-10-15 | Step 1.8 begins while the owner is away, with these as proposals until the owner decides: Export Song as described in [section 18](#18-projects-saving-and-export). Its window starts with a 24-bit WAV at 44.1 kHz, as the song plays (no normalizing); Normalize offers -14 LUFS at first, the level Spotify's guide asks masters for ([B58](#appendix-b-sources)), under a -1 dBTP ceiling (Spotify's, and EBU R 128's maximum, [B59](#appendix-b-sources)), and any target from -30 to -5 LUFS and ceiling from -6 to 0 dBTP; the report counts a file within 0.5 LU of its target as meeting it (R 128's tolerance). The window keeps its choices for the next export, in any project. A file of that name is replaced without asking only when this window exported it or the file chooser asked already; otherwise the window asks. Keys: Ctrl+Shift+R in Phazite's keymap (Ableton Live's Export Audio/Video, [B60](#appendix-b-sources)) and Ctrl+R in FL Studio's (its Export wave file, [B61](#appendix-b-sources)). MP3 stays out until question 5 is decided |
| 2026-10-15 | Step 1.9 begins while the owner is away, with these as proposals until the owner decides: first launch and new projects as described in [section 13](#13-getting-started-and-song-outlines). The welcome window now opens on every first launch, at "Coming from...?", even with Phazite Sounds installed. Nothing is chosen there until a click, unless FL Studio's keymap is in use, since Phazite's own is what two of its three choices give. The test sound is JUCE's: a 440 Hz beep for a second at -6 dBFS. The guide has five steps and Help > Getting Started Tips (on at first). New projects start with the two instrument tracks, ticked at first, at 120 BPM in 4/4 without a key or an outline; the key offers major and minor only, and the piano roll's key menu has the other scales |
| 2026-10-16 | After the owner tried phase 1: a new project is untitled until its first save asks what to call it and where it goes ([section 18](#18-projects-saving-and-export)); the computer keyboard starts off (question 10); playlist rows aren't tinted, and every clip shows its track's color; the piano roll doesn't turn pages while the song plays, and its playhead shows, fainter, where an alias of the clip shown plays its notes, as FL Studio's piano roll follows its pattern ([section 7](#7-the-piano-roll)); dragging along the top strip of a ruler loops that part of the song, as Logic Pro's cycle area and Cubase's ruler do, and a click there turns looping off and on ([section 6](#6-the-playlist)); the editors' scroll bars show only when there is something to scroll; a panel in its own window fills it at once. The owner decided questions 5 to 8: no MP3, plug-ins that aren't Phazite's own never built in (optional downloads at most), plug-in sandboxing before v1 unless it costs a lot of performance, and an Ableton Live keymap in v1. The owner left questions 11 to 19 to Claude's judgement, favoring what most DAWs do, for review later; [section 25](#25-open-questions) gives each decision and why |
| 2026-10-16 | Step 2.1, automation lanes, decided by Claude for the owner to review ([section 10](#10-automation-and-modulation)): lanes under the tracks, as view state, and the master's under a Master row while it has some; Automate on every knob's and fader's right-click, and on a device's slot for plug-ins, whose own windows Phazite can't add to, with Automate Last Touched (Ctrl+Shift+A) over plug-in windows; Live's ways in a lane (click adds, Alt+drag bends, Draw draws steps), Studio One's shapes for Paint, write modes per track as most DAWs have them (Read by default), A to show automation as in Live and Logic, and the arrow keys for selected points |
| 2026-10-16 | Step 2.2, automation clips, decided by Claude for the owner to review ([section 10](#10-automation-and-modulation)): made from a control's right-click, a device slot's menu or a command for the parameter changed last, on the parameter's track (the master's and folders' on an Automation track), four bars from the playhead's bar; while one plays its curve sets the parameter over its lane, and before and after it the lane or the parameter's own value holds; as in FL Studio, a clip sets the parameter it was made for on whichever track it is moved or copied to; its curve is edited on the clip with the lanes' tools, a double-click adding a point; aliases share their curve |
| 2026-10-16 | Step 2.3, modulators and macros, decided by Claude for the owner to review ([section 10](#10-automation-and-modulation)): each track's, as in Cubase 15; assigned from a knob's right-click (Modulate) or by dragging a modulator's grip from a channel strip onto a knob; a ring around a modulated knob shows the range and where the value is, and dragging the ring changes the amount; new LFOs, random sources and step sequencers move both ways; modulators' editors open in a bubble |
| 2026-10-16 | Step 2.4, containers and presets, decided by Claude for the owner to review ([section 9](#9-devices-containers-and-presets)): devices selected by Ctrl-click and grouped with Ctrl+G, as in Ableton Live and Bitwig; effects only, in a row; eight macros always there, as in Ableton's racks, starting at 0 (a track's macros too); a container's window with its macros on the left and its devices in a row; presets in one file type, in a folder per device, Containers and Tracks under Documents/Phazite/Presets, shown in the browser; saved from right-click menus with a save dialog, or by dragging a track into the browser; a track's preset leaves out its clips, automation and sends, and makes a new track |
| 2026-10-17 | The owner chose lanes as the one way to automate, after research showed two philosophies (lanes in Ableton Live, Bitwig, Logic Pro, Cubase, Studio One, Pro Tools and REAPER; free automation clips in FL Studio, whose rows aren't tied to instruments): automation clips (step 2.2) are gone, and projects that hold them open with their curves as lane automation (format version 5); automation goes with the clips above it, moved or copied along their tracks, and with time ranges deleted or duplicated across tracks, as in Ableton Live, Bitwig and Studio One ([section 10](#10-automation-and-modulation)) |
| 2026-10-17 | The owner: automation never hides, and every playlist row collapses to a thin row with a chevron rather than closing. Every parameter with automation has a lane; a lane collapses with its chevron, its bottom edge or its collapsed track (the master row's chevron collapses the master's lanes), and goes only with its automation, as one undo step (format version 6) ([section 10](#10-automation-and-modulation)) |
| 2026-10-17 | The owner: a lane's points and lines took too many key and button combinations, and the editor dock's Automation tab was an unused leftover. The Smart Tool now needs no keys in a lane: a click adds a point, a press on the line pulls a new point from it (Studio One), a hollow handle in the middle of the line under the mouse bends it (Studio One, FL Studio) in place of Alt+drag, and a double-click deletes a point or straightens a line at its handle; Alt, Shift and Ctrl keep their meanings from the rest of Phazite (off the grid, fine steps, add to the selection), so Shift no longer selects points. Dragging a line up or down went: select its two points and drag one. The Automation tab went, since automation is edited in its lanes ([section 10](#10-automation-and-modulation)) |
| 2026-10-17 | The owner: the editor dock mixed one clip's editor with every track's mixer, the inspector showed only a copy of a mixer channel, and effects needed a place in line, as the piano roll is for notes. After research into seven DAWs' panels ([section 4](#4-the-window), Appendix A), the owner chose: the centre shows the whole song, as Playlist or Mixer, chosen by tabs at its top (F5 and F9 too), as Ableton Live and Bitwig keep the mixer apart from the selection, with as few keys to remember as possible; the editor dock shows the selection, as Piano roll and a new Devices view (F6) of the selected track's devices in line, its modulators and macros first, where Phazite's own devices and containers open (they have no windows of their own any more), and plug-ins show their key knobs and a button for their windows; and the inspector stays, showing properties of what is selected, as Bitwig's, Studio Pro's, Logic Pro's and Cubase's do, not a copy of a mixer channel. The Audio placeholder tab goes; an audio clip editor gets a step of its own ([roadmap](roadmap.md) steps 2.5 and 2.6) |
| 2026-10-17 | Step 2.6, the audio clip editor, decided by Claude for the owner to review ([section 7](#the-audio-clip-editor)): it takes the editor dock's first tab while an audio clip, or a track that holds audio, is selected, and the tab is then called "Audio clip"; it counts the song's bars, unlike the piano roll, so that trims and fades meet the song's grid, and shares the piano roll's grid; all of the file shows, the audio the clip's edges trim off shaded; its edges trim, square handles at its top corners fade (a double-click takes a fade away) and a drag on its audio moves it inside the clip, with no tools or keys to learn; Gain, Pitch and Fine, Reverse, and Follow tempo with the tempo the audio was played at, as fields and switches down its left, each change one undo step; Reverse and Follow Tempo are also commands for the selected clips; and an audio clip's end now stops where its audio does when its start was trimmed |
| 2026-10-17 | The owner asked for Signalsmith Stretch to be measured against SoundTouch, and switched to if it was worth it. It pitch-shifts more cleanly, but renders three times slower, costs ten times as much when previews stretch as they play, and in Tracktion's background renders, which clips play, it would make every pitched or tempo-following clip 0.1 s late; SoundTouch's clips turned out to play 13 to 28 ms early. Phazite keeps SoundTouch until Tracktion's timing is fixed ([section 23](#23-tracktion-engine-what-it-gives-and-what-we-build)) |
| 2026-10-18 | The owner: the early clips are a bug to fix at a later date, logged in a new list of known bugs ([known-bugs.md](known-bugs.md)); the sound generator's first part (step 2.8) moves to phase 3, into step 3.2 with its second; and the rest of phase 2 (steps 2.7 and 2.9 to 2.12) comes next |
| 2026-10-18 | Step 2.7, the chord lane, decided by Claude for the owner to review ([section 14](#14-chords)): a thin lane always under the playlist's bar numbers; chords added and changed by clicking (the key's seven chords, then borrowed ones and secondary dominants; kind and inversion), resized by their right edge; chords kept as numerals in the key where they are, with key changes along the lane, so a change of key moves them; 40 progressions in a data file, played in the song's key on Phazite's synth and laid over the selected sections, range or whole song, a minor progression playing in a major key's relative minor; parts (chords, bass, arpeggio, twelve styles) on new tracks with Phazite's synth, which follow the chords, as Logic Pro's Session Players do, until their notes are edited, shown and turned back on in the inspector; chords go with the song's time as sections do; the piano roll tints chord notes. The chords aren't written into Tracktion's chord track, since nothing reads them there. Projects with chords are format version 7 |
| 2026-10-18 | Step 2.9, mixer depth, decided by Claude for the owner to review ([section 8](#8-the-mixer)): every strip and the inspector show a send knob for each return the track can send to, which its first turn makes (a press without a drag makes nothing), after the fader, in that turn's undo step; "+ Send" only makes new returns; the Out line is a menu of the master, the buses and a new bus from the selected tracks; a thin CPU bar along each strip's bottom, amber above half; a Sidechain submenu on every device with a sidechain input, plug-ins included; and the routing view behind the mixer's Routing button, with boxes and jacks, wires only for the selected or hovered channel, cables dragged from Out jacks, and wires taken away from a box's right-click menu |
| 2026-10-18 | Step 2.10, more devices, decided by Claude for the owner to review ([section 9](#9-devices-containers-and-presets)): a filter, a multiband dynamics device in OTT's style, a maximizer in Maximus's (three squeezed bands into Phazite's limiter), a saturator with five curves at twice the sample rate, a tempo-synced delay with ping-pong, and a utility, each with the defaults in section 9 and a panel of its controls (switches for what is on or off); and unison for the synth, up to seven voices detuned and spread, one at first so older projects sound the same. Projects with them are format version 8. Each still needs A/B listening against reference tools |

## Appendix A. Research summary

In October 2026, research agents reviewed about 20 DAWs, and the Tracktion Engine source was audited against what a DAW needs.
- **Sources of opinion:** reviews (Sound On Sound, MusicTech, Attack, Tape Op, MusicRadar), KVR and vendor forums.
- **Limits:** Reddit and Gearspace couldn't be fetched, so treat opinions as a sample, not a census.

| DAW | Loved | Borrowed for Phazite | Avoided |
|---|---|---|---|
| FL Studio 2026 | The piano roll ("like a paint program"); fast pattern painting; lifetime free updates | The piano-roll mouse model and transforms, single-key tools, playlist colors and folders, F-keys | The channel rack / pattern / playlist / mixer split; invisible routing; floating windows |
| Ableton Live 12 | Session View for sketching; MIDI Tools; the tag browser; Capture MIDI; Bounce in Place; Info View | Capture, transforms with preview, hover help, a per-track CPU meter | Shortcuts that can't be remapped; clips that silently override the arrangement |
| Bitwig Studio 6 | Modulators on anything; clip automation and aliases; tools on number keys; a footer showing mouse actions; plug-in sandboxing | Drag-to-modulate, aliases, the hint bar, sandboxing | Depth before basics; odd default shortcuts; a messy browser |
| Fender Studio Pro 8 (Studio One) | Drag-and-drop everything; Arranger track; chord track; Channel Overview | Drop to create tracks, track presets, sections, progression presets | An assistant that can only talk; setup before the keyboard plays |
| Reason 14 | Players (chord/arp generators); the Combinator; auto-cabling | Containers with macros, the routing view | A sequencer that lags behind the instruments |
| Logic Pro 12 | Live Loops tied to tracks; track stacks (folder or bus); Smart Controls; Quick Help; Flashback Capture | Folders that become buses, key controls on channel strips, hover help | The old beginner/pro switch (removed in Logic 12); CPU-heavy presets |
| Cubase 15 | Modulators; last-touched automation; the docked lower zone; Chord Assistant | Last-touched automation, the editor dock, chord suggestions | "Not a beginner DAW"; inconsistent UI; half-finished features |
| GarageBand | Hover help on by default; templates that set key and tempo first | First-run help, the new-project dialog | Hidden mixer and buses |
| Pro Tools 2026 | Editing speed; the Smart Tool; single-key command mode; Track Pin | The Smart Tool, track pinning | Tiered track limits; iLok; "fiddly" MIDI |
| REAPER 7 | Price and size; "everything is just a track"; a searchable action list; scripting; crash protection | The command palette, macros, range edits across tracks | "Nothing there when you first open it" |
| Ardour 9 | Open source, pay what you want; Region FX; note brushing | Clip effects, paint tool | "If in doubt, right-click somewhere"; track-snapped scrolling |
| Waveform 14 (same engine) | One-screen layout; one track type; racks and drag-on modifiers | The engine's track model and modulators | An "eccentric" first screen; sandboxing off by default |
| Digital Performer, Sonar, LUNA, Renoise, MPC, BandLab | Several arrangements per project; workspace presets; automatic track naming and coloring; keyboard-first editing; perform then commit; sound in seconds | Keymap import, auto-naming, Capture | Private vocabulary; account and activation hurdles |

**Mixers (researched 2026-10-13, for the [mixer redesign](#8-the-mixer)):** Logic Pro, Pro Tools, Live, Bitwig, Studio One, Cubase, FL Studio, REAPER, Reason, Ardour, Mixbus and Waveform, from their manuals, reviews and forums.
- **The instrument has a place of its own** above the effects in the DAWs that don't trip people up (Logic, Reason's Track Panel, Live's enforced chain order). Where instruments share the effect slots (Pro Tools, REAPER, Waveform), order mistakes cause silence.
- **Sends:** a send for every return on every track (Live, Bitwig, Mixbus) is explained to beginners in one sentence, and Studio One's "drop an effect on the sends to make a return" is the most praised setup. Logic's hidden auxes and FL Studio's unnamed mixer inserts confuse newcomers. The words aux, bus, send and return mean different things from one DAW to the next.
- **Folders that are buses** are where the field converges (Live, Pro Tools routing folders, Cubase 15, Logic's summing stacks).
- **Routing diagrams** get mixed reviews; showing routing on the selected strip (Logic, REAPER's route button) works without clutter.
- **Solo:** every other DAW keeps solo apart from the mute switches; FL Studio's solo changes them, a complaint on its forum since 2007.

**Panels around the timeline (researched 2026-10-17, for [section 4](#4-the-window)):** what seven DAWs put at the bottom of the window and at its side, from their manuals and Sound On Sound ([B64](#appendix-b-sources) to [B75](#appendix-b-sources)).

| DAW | Bottom of the window | The selected track's devices in line | Side panel |
|---|---|---|---|
| Ableton Live 12 | Clip View or Device View, or both stacked: the selection only | The Device View; a plug-in shows the parameters chosen with Configure, and a button opens its window | None; the mixer is in Session View, and on each track's row in Arrangement View |
| Bitwig Studio 6 | One at a time: Detail Editor, Automation Editor, Device panel or Mixer; the Mix view has the mixer in the centre and the Device panel below | The Device panel; a plug-in shows a list of its parameters, and a button opens its window | Inspector (left): properties of the selection |
| Fender Studio Pro 8 | One at a time: Editor, Console or Channel Overview (new in 8) | The Channel Overview: the channel's controls, then compact panels (Studio Pro's own effects with their graphs and knobs, a plug-in's first eight parameters or those chosen) | Inspector (left) |
| Logic Pro | Editors, Mixer or Smart Controls | Smart Controls: a few knobs for the track's sound; plug-ins open windows | Inspector (left): region and track settings, and two channel strips (the track's and where it goes) |
| Cubase 15 | Lower Zone tabs: MixConsole, Editor, Sampler Control, Chord Pads, Modulators | None; plug-ins open windows | Inspector (left): sections, a copy of the channel's fader among them |
| Reason 14 | The Track Panel (new in 2026): the selected track's devices, signal chain, sends and pan, in the sequencer | Its rack devices | None |
| FL Studio 2025 | No dock: floating windows (F7 piano roll, F9 mixer) | None: the mixer's side panel lists the selected track's effect slots, and each opens a window | The mixer's side panel |

**Audio clip editors (researched 2026-10-17, for [the audio clip editor](#the-audio-clip-editor)):** what five DAWs show of an audio clip, from their manuals ([B76](#appendix-b-sources) to [B81](#appendix-b-sources)). Logic Pro's is from its guide for the iPad; the cells marked "not checked" weren't in the pages read.

| DAW | Where | Gain | Pitch | Reverse | Tempo |
|---|---|---|---|---|---|
| Ableton Live 12 | The Clip View, under the session or arrangement: its sample with start and end markers | A slider, in dB | Semitones, and cents in a second field | Makes a reversed copy of the sample | Warp, from the sample's tempo |
| Logic Pro | The region inspector | In dB | Transpose (semitones) and Fine Tune (cents) | Yes, though not while Flex & Follow is on | Follow Tempo (Apple Loops), or Flex & Follow |
| Fender Studio Pro 8 | The inspector, and the Edit view (F2), which opens the Audio Editor for audio events | Not checked | Not checked | Not checked | The event's File Tempo; each track Timestretches, Follows or Doesn't Follow |
| Bitwig Studio | The inspector, and the Detail Editor | A gain expression, in dB | A pitch expression, in semitones | Not in the inspector | A stretch mode, and the event's original tempo |
| FL Studio | The sampler's channel settings | Not checked | Pitch, keeping the length | Reverse | Time (to the project's tempo), and a stretch mode |

**Code structure (researched 2026-10-13, for [section 24](#24-code-structure)):** Ardour, MuseScore and its shared framework, Audacity 3 and 4, LMMS, Zrythm 2, ossia score, the JUCE hosts Element and Helio, and apps built on Tracktion, from their developer documentation and source.
- **Boundaries hold only where a build rule or a check enforces them.** Ardour's core library builds without its UI toolkit ([B43](#appendix-b-sources)), and MuseScore makes each module a CMake project of its own ([B44](#appendix-b-sources)). LMMS, with folders only, let its core grow UI code: its core track class declares how to make its own view ([B52](#appendix-b-sources)).
- **In the newer designs, the UI never changes the project directly.** It sends a command or calls an operation that owns the undo step: MuseScore's view models send commands to controllers ([B45](#appendix-b-sources)), Zrythm writes down "zero direct model mutation" ([B50](#appendix-b-sources)), and ossia's presenters submit commands, a drag sending updates and then one commit or rollback ([B51](#appendix-b-sources)).
- **Commands are becoming the public API.** MuseScore's 2026 decision records turn them into typed calls with parameters and results, and say which ones shortcuts, scripts or an assistant may use ([B46](#appendix-b-sources), [B47](#appendix-b-sources)).
- **Objects cross boundaries as stable IDs, never pointers:** Tracktion's `EditItemID`, Ardour's `PBD::ID`, ossia's paths and Zrythm's UUIDs. Tracktion's developers advise apps to look items up by ID and to keep the Edit as the one store of project data ([B53](#appendix-b-sources)).
- **Engines were wrapped only when something was really being replaced, one piece at a time.** Audacity 4 rebuilt its UI over its old engine through adapters ([B48](#appendix-b-sources)) and shipped 4.0 without MIDI tracks or the mixer ([B49](#appendix-b-sources)). Replacing one capability behind an interface ("branch by abstraction", [B54](#appendix-b-sources)) avoids a rewrite, whose cost Tracktion's lead developer describes ([B55](#appendix-b-sources)).
- **Designers work through named theme values, and the busiest views stay painted in code.** Design tokens have a shared format, DTCG 2025.10 ([B56](#appendix-b-sources)); the Melatonin Inspector shows a running JUCE app's sizes, colors and fonts ([B57](#appendix-b-sources)).
- **Fast-changing data stays out of the project:** meters and the playhead are read from small sources, not stored in the project or sent as change messages.

**Where the field is converging:**
- Each track is a row, a chain and a channel.
- Two automation layers: lanes under each track, and modulators. Where a DAW also has clip-like automation, it lives inside an instrument's clips or a parameter's own lane (research of 2026-10-17, [section 10](#10-automation-and-modulation)).
- Linked clips with "make unique".
- Capture instead of record.
- One window with layouts.
- Every action is a command.
- No beginner/pro switch.
- Help in place.
- Sandboxing on by default.
- AI that assists inside the work.
- Project portability through DAWproject.

## Appendix B. Sources

- **B1** Production Expert, 2025 DAW survey: <https://www.production-expert.com/production-expert-1/2025-daw-survey-the-results>
- **B2** Bobby Owsinski's reading of the 2025 figures: <https://bobbyowsinskiblog.com/?p=9579>
- **B3** Avanzini et al., SMC 2025 DAW survey: <https://avanzini.di.unimi.it/downloads/publications/avanzini_smc25.pdf>
- **B4** FL Studio manual, Playlist (Track Mode): <https://www.image-line.com/fl-studio-learning-content/fl-studio-online-manual/html/playlist.htm>
- **B5** Image-Line forum, feature request on the channel rack/playlist split: <https://forum.image-line.com/viewtopic.php?p=2069597>
- **B6** Apple, What's new in Logic Pro: <https://support.apple.com/guide/logicpro/whats-new-in-logic-pro-lgcp4a62a494/mac>
- **B7** MusicRadar, 8 things about Logic Pro: <https://www.musicradar.com/news/8-things-logic-pro>
- **B8** MusicRadar, best DAWs (REAPER entry): <https://www.musicradar.com/news/the-best-daws-the-best-music-production-software-for-pc-and-mac>
- **B9** MusicTech, Waveform 14 Pro review: <https://musictech.com/reviews/digital-audio-workstations/tracktion-waveform-14-pro-review/>
- **B10** Avid, Pro Tools: <https://www.avid.com/pro-tools>
- **B11** KVR forum, FL Studio piano roll: <https://www.kvraudio.com/forum/viewtopic.php?p=8634884>
- **B12** Image-Line forum, effect slots per mixer track: <https://forum.image-line.com/viewtopic.php?p=2063103>
- **B13** Sonicstate, Digital Performer 12: <https://sonicstate.com/amped/2026/09/17/digital-performer-12-ships/>
- **B14** Audialab, Emergent Drums 2: <https://audialab.com/products/emergent-drums-2/>
- **B15** Sonicstate, Steinberg Backbone with DrumGAN: <https://www.sonicstate.com/news/2022/06/24/steinberg-incorporates-ai-into-backbone>
- **B16** MusicRadar, Synplant 2 review: <https://www.musicradar.com/reviews/sonic-charge-synplant-2-review>
- **B17** RIAA, labeling program for generative AI: <https://www.riaa.com/news/music-community-introduces-new-labeling-program-to-distinguish-generative-ai-in-sound-recordings/>
- **B18** MusicTech, Tracklib 2025 survey: <https://musictech.com/news/industry/music-producers-are-rejecting-ai-study-reveals-over-80-of-producers-are-against-ai-generated-songs>
- **B19** Chordonomicon dataset: <https://huggingface.co/datasets/ailsntua/Chordonomicon>
- **B20** mirdata, McGill Billboard loader (licence and size): <https://mirdata.readthedocs.io/en/0.3.6/_modules/mirdata/datasets/billboard.html>
- **B21** Steinberg, Chord Assistant: <https://steinberg.help/cubase_le/v13/en/cubase_nuendo/topics/chord_pads/chord_pads_chord_assistant_c.html>
- **B22** Hooktheory, Hookpad: <https://www.hooktheory.com/hookpad>
- **B23** Audiofanzine, Fender Studio Pro 8 review: <https://en.audiofanzine.com/general-sequencer/fender/studio-pro-8/editorial/reviews/test-fender-studio-pro-8.html>
- **B24** Stable Audio Open Small model card: <https://huggingface.co/stabilityai/stable-audio-open-small>
- **B25** Stability AI Community License: <https://stability.ai/community-license-agreement>
- **B26** Meta AudioCraft: <https://github.com/facebookresearch/audiocraft>
- **B27** RAVE licence: <https://github.com/acids-ircam/RAVE/blob/master/LICENSE>
- **B28** stable-audio-tools: <https://github.com/Stability-AI/stable-audio-tools>
- **B29** Ardour 9.5 release notes: <https://ardour.org/news/9.5.html>
- **B30** Ableton Live manual, Recording New Clips (Capture MIDI): <https://www.ableton.com/en/live-manual/12/recording-new-clips/>
- **B31** Bitwig user guide, plug-in handling: <https://www.bitwig.com/userguide/latest/vst_plug-in_handling_and_options>
- **B32** Tracktion Engine README (contributions): <https://github.com/Tracktion/tracktion_engine>
- **B33** JUCE forum, Linux accessibility request: <https://forum.juce.com/t/linux-accessibility-support/69234>
- **B34** W3C, WCAG 2.2: <https://www.w3.org/TR/WCAG22/>
- **B35** Inter typeface (licence: SIL OFL 1.1): <https://github.com/rsms/inter>
- **B36** Ableton forum, Live 10's bundled AbletonSans fonts: <https://forum.ableton.com/viewtopic.php?p=1735289>
- **B37** REAPER changelog, 7.70 ("use Inter as default font on all platforms"): <https://www.reaper.fm/whatsnew.txt>
- **B38** SIL Open Font License FAQ (bundling with other software): <https://openfontlicense.org/ofl-faq/>
- **B39** GNU project, licence list (SIL OFL entry): <https://www.gnu.org/licenses/license-list.html#SILOFL>
- **B40** Ableton Live 12 manual, MIDI and Key Remote Control (playing MIDI with the computer keyboard): <https://www.ableton.com/en/manual/midi-and-key-remote-control/>
- **B41** FL Studio manual, keyboard and mouse shortcuts (Ctrl+T typing keyboard to piano, F10 MIDI settings): <https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/basics_shortcuts.htm>
- **B42** Lucide icons and licence (ISC; MIT for the icons from Feather): <https://github.com/lucide-icons/lucide/blob/main/LICENSE>
- **B43** Ardour developer documentation: <https://ardour.org/files/doxygen/>
- **B44** MuseScore framework, decision record 00001 (modules): <https://raw.githubusercontent.com/musescore/muse_framework/main/docs/adr/00001-CoreArch-Modularity.md>
- **B45** MuseScore framework, decision record 00005 (interaction workflow): <https://raw.githubusercontent.com/musescore/muse_framework/main/docs/adr/00005-CoreArch-Interact-workflow.md>
- **B46** MuseScore framework, decision record 00101 (commands replace actions): <https://raw.githubusercontent.com/musescore/muse_framework/main/docs/adr/00101-Commands-Replacing-actions-with-commands.md>
- **B47** MuseScore framework, decision record 00107 (where commands may be used): <https://raw.githubusercontent.com/musescore/muse_framework/main/docs/adr/00107-Commands-Availability.md>
- **B48** Audacity 4, track-editing interfaces and their Audacity 3 adapters: <https://github.com/audacity/audacity/tree/master/src/trackedit>
- **B49** Audacity 4.0.0 release notes: <https://www.audacityteam.org/changelog/4.0.0/>
- **B50** Zrythm, undo system: <https://raw.githubusercontent.com/zrythm/zrythm/master/doc/dev/undo_system.md>
- **B51** ossia score, commands: <https://ossia.io/score-api-docs/_commands.html>
- **B52** LMMS, Track.h: <https://raw.githubusercontent.com/LMMS/lmms/master/include/Track.h>
- **B53** JUCE forum, structuring an app on Tracktion's Edit (2020): <https://forum.juce.com/t/questions-about-app-structure-state-valuetree-using-tracktion-engine/38483>
- **B54** Martin Fowler, Branch by Abstraction: <https://martinfowler.com/bliki/BranchByAbstraction.html>
- **B55** Dave Rowland, "Why you shouldn't write a DAW" (ADC 2023): <https://youtu.be/GMlnh6_9aTc>
- **B56** Design Tokens Community Group, format 2025.10: <https://www.designtokens.org/TR/2025.10/>
- **B57** Melatonin Inspector (MIT): <https://github.com/sudara/melatonin_inspector>
- **B58** Spotify for Artists, Loudness normalization: <https://support.spotify.com/artists/article/loudness-normalization/>
- **B59** EBU R 128, Loudness normalisation and permitted maximum level of audio signals (2014): <https://tech.ebu.ch/docs/r/r128-2014.pdf>
- **B60** Ableton Live 12 manual, Live Keyboard Shortcuts: <https://www.ableton.com/en/live-manual/12/live-keyboard-shortcuts/>
- **B61** FL Studio manual, File menu: <https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/menu_file.htm>
- **B62** Apple, GarageBand for Mac User Guide, Create, save, and delete projects: <https://support.apple.com/guide/garageband/gbndd0893c9b/mac>
- **B63** Ableton Live 12 manual, First Steps: <https://www.ableton.com/en/live-manual/12/first-steps/>
- **B64** Ableton Live 12 manual, Working with Instruments and Effects (the Device View, stacked with the Clip View): <https://www.ableton.com/en/live-manual/12/working-with-instruments-and-effects/>
- **B65** Ableton Live 12 manual, Mixing (the mixer in Session and Arrangement View): <https://www.ableton.com/en/live-manual/12/mixing/>
- **B66** Ableton Live 12 manual, Using Plug-Ins (Configure, the plug-in window): <https://www.ableton.com/en/live-manual/12/using-plug-ins/>
- **B67** Bitwig Studio user guide, The Window Footer (one panel at a time in each place): <https://www.bitwig.com/userguide/latest/the_window_footer>
- **B68** Bitwig Studio user guide, The Mix View: <https://bitwig.com/userguide/bws44-504/the_mix_view>
- **B69** Bitwig Studio user guide, Working on a Tablet Computer (the Device panel above the mixer instead of below it): <https://www.bitwig.com/userguide/latest/working_on_a_tablet_computer>
- **B70** Bitwig Studio user guide, VST plug-ins (a plug-in in the Device panel): <https://bitwig.com/userguide/latest/vst_plug-ins>
- **B71** Fender Studio Pro manual, Channel Overview and Editor: <https://fenderstudiopromanual.fender.com/en/Content/Mixing_Topics/Channel%20Overview%20and%20Editor.htm>
- **B72** Sound On Sound, Studio Pro: The Channel Overview: <https://www.soundonsound.com/techniques/studio-pro-channel-overview>
- **B73** Apple, Logic Pro manual, the arrange channel strips in the Inspector: <https://help.apple.com/logicpro/mac/9.1.6/en/logicpro/usermanual/chapter_10_section_1.html>
- **B74** Steinberg, setting up the Lower Zone, and Track Inspector sections: <https://archive.steinberg.help/nuendo/v11/en/cubase_nuendo/topics/project_window/project_window_lower_zone_setting_up_t.html>, <https://steinberg.help/cubase_artist/v13/en/cubase_nuendo/topics/tracks_about/tracks_about_inspector_sections_r.html>
- **B75** MusicTech, Reason 14's Track Panel: <https://musictech.com/news/music/reason-14/>
- **B76** Ableton Live 12 manual, Clip View (the Audio Utilities panel: Warp, Reverse, Gain and Pitch; the start and end markers; the sample's details): <https://www.ableton.com/en/live-manual/12/clip-view/>
- **B77** Bitwig Studio user guide, Inspecting Audio Clips (stretch Mode, Tempo, Fades; the Gain and Pitch expressions): <https://www.bitwig.com/userguide/latest/inspecting_audio_clips>
- **B78** Fender Studio Pro manual, Timestretching (File Tempo; Don't Follow, Follow and Timestretch): <https://fenderstudiopromanual.fender.com/en/Content/Editing_Topics/Timestretching.htm>
- **B79** Apple, Logic Pro for iPad User Guide, audio region parameters (Gain, Transpose, Fine Tune, Reverse, Flex & Follow, Follow Tempo): <https://support.apple.com/guide/logicpro-ipad/lpip1ae16a9f/ipados>
- **B80** FL Studio manual, Sampler Channel Settings (Pitch, Time, the stretch Mode, Reverse, fades): <https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/chansettings_sampler.htm>
- **B81** Fender Studio Pro manual, Edit View Event Editing (F2 opens the Audio Editor for audio events and the Note Editor for instrument parts; the editor's snapping is its own): <https://fenderstudiopromanual.fender.com/en/Content/Editing_Topics/Edit_View_Event_Editing.htm>
- **B82** Fender Studio Pro manual, Chord Track (a global track; new chords a bar long; a chord's end dragged; the Chord Selector and the right-click menu for root, type and bass; tracks' Follow Chords modes; progressions saved as presets): <https://fenderstudiopromanual.fender.com/en/Content/Arranging_Topics/Chord%20Track.htm>
- **B83** Apple, Logic Pro for Mac User Guide, Chords and Session Players (which chords a Session Player region follows; converting Session Player regions into MIDI regions): <https://support.apple.com/guide/logicpro/lgcp70dd5af3/mac>
- **B84** Steinberg, Cubase Pro 12 Operation Manual, Follow Chord Track Options (Chords, Scales, Root Notes for bass, Voicings, Single Voice): <https://archive.steinberg.help/cubase_pro/v12/en/cubase_nuendo/topics/chord_functions/chord_functions_follow_chord_track_r.html>
