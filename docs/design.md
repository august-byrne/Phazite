# Phazite design

*Status: draft for review, October 2026.* This document describes the product Phazite is aiming for, and why. The plan for building it is in [roadmap.md](roadmap.md). **Decided** marks choices the project owner has made; **Open** marks questions that still need a decision ([section 24](#24-open-questions)). The research behind the design is summarized in [appendix A](#appendix-a-research-summary), with sources in [appendix B](#appendix-b-sources).

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
24. [Open questions](#24-open-questions)
25. [Decision log](#25-decision-log)

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

[Section 24](#24-open-questions) and the [roadmap](roadmap.md#after-v1) list what waits until after v1.

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
| Play  Rec   140 BPM   F minor   CPU 12%    Ctrl+K: search any command |
+----------+--------------------------------------------+---------------+
| Browser  | Sections [ Intro   ][ Build  ][ Drop    ]  | Inspector     |
|  (F8)    | v Drums  (folder = bus)                    | (selection)   |
| folders  |     Kick    [] [] [] [] [] [] [] []        | devices       |
| tags     |     Snare      []    []    []    []        | sends         |
| plug-ins | v Synths                                   | macros        |
| samples  |     Lead    [riff ] [riff*] [riff*] *alias | modulators    |
| presets  |       `- Cutoff lane   /\/\/\_/\           |               |
|          | > FX  (3 tracks, collapsed)                |               |
+----------+--------------------------------------------+---------------+
| Editor dock: piano roll (F7) | audio | automation | mixer (F9)        |
+-----------------------------------------------------------------------+
| Hint bar: what the mouse and modifier keys do under the pointer       |
+-----------------------------------------------------------------------+
```

- **Top bar:** transport, tempo, key, time signature, CPU and disk meters, and the command search (Ctrl+K).
- **Browser (left):** instruments, effects, plug-ins, presets, samples, generated sounds and projects ([section 11](#11-the-browser)).
- **Playlist (center):** the sections strip, then global lanes (chords, tempo, markers), then the tracks ([section 6](#6-the-playlist)).
- **Inspector (right):** the properties of whatever is selected: a track's devices and sends, a clip's gain and fades, a device's macros and modulators.
- **Editor dock (bottom):** tabs for the piano roll, audio editor, automation editor and mixer. It follows the selection, so selecting a MIDI clip shows it in the piano roll.
- **Hint bar (bottom edge):** what clicking, dragging and the modifier keys do under the pointer.

**Panels and layouts:**
- Panels can be resized, collapsed and detached to another monitor.
- A layout saves the arrangement of panels. Three ship by default, and they are layouts, not modes:
  - *Beat:* playlist and piano roll;
  - *Mix:* a large mixer;
  - *Full.*
- Function keys follow FL Studio where that makes sense: F5 playlist, F7 piano roll, F8 plug-in picker and browser, F9 mixer.

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

## 5. Building blocks

| Concept | What it is | Tracktion Engine |
|---|---|---|
| Project | A folder with the project file, its audio and its generated sounds | `Edit`, plus folder-based `Project` |
| Track | Playlist row, device chain and mixer channel in one | `AudioTrack`, one type that holds audio, MIDI and step clips |
| Folder | A collapsible, colored group of tracks; becomes a bus when it holds an effect | `FolderTrack`, plain or submix |
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
| Top-left or top-right corner | Draws a fade-in or fade-out |
| Upper half | Selects a time range |
| Lower half | Moves the clip; with a modifier key it copies or makes an alias (the keymap sets which key) |
| Bottom of the boundary between two touching clips | Draws a crossfade |

**On empty space:**
- dragging selects a time range across tracks, like REAPER's razor edits;
- double-clicking creates an empty MIDI clip on an instrument track;
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

**Right-click** opens a context menu by default. With the FL Studio keymap it deletes, as FL users expect. Both are a setting.

**Editing commands:**
- split at the playhead or pointer;
- duplicate, make alias, make unique;
- delete, with or without closing the gap (ripple);
- insert or remove time across all tracks;
- nudge, and quantize clip positions;
- reverse, normalize, clip gain;
- fades and crossfades with curve shapes;
- bounce clips to audio.

**Tracks:**
- color and name (automatic from the content, editable), height;
- collapsible folders;
- **pin** tracks so they stay visible while scrolling, like Pro Tools' Track Pin;
- a **focus mode** that collapses all other tracks;
- mute, solo and record-arm;
- a CPU readout.

**Sections strip:**
- drag a section to move it, with everything in it, across all tracks;
- duplicate, duplicate as aliases, delete, rename, recolor;
- "Apply outline" ([section 13](#13-getting-started-and-song-outlines)).

**Grid, zoom and scrolling:**
- The grid adapts to the zoom level, with snap on by default; hold a modifier to bypass it. Triplets are available.
- Scrolling is smooth in both directions, never jumping a whole track at a time (an Ardour complaint).
- Zoom to selection.

**Clip picker:** a searchable list of every clip in the project, like FL Studio's Picker panel. Drag a clip into the playlist, or pick it as the clip the draw tool places.

## 7. The piano roll

This is the editor the target users spend most time in, and FL Studio's is its most-praised feature: "designed… like a paint program" ([B11](#appendix-b-sources)). Gaps here are what sent FL Studio users away from Bitwig and Reason ([appendix A](#appendix-a-research-summary)).

- **Mouse model from FL Studio:**
  - left-click adds a note and right-click deletes it;
  - new notes reuse the last length and velocity;
  - dragging an edge resizes a note and dragging its body moves it;
  - the mouse wheel scrolls and zooms.

  A menu button and Shift+right-click open the context menu.
- **Tools:** draw, paint (drag to lay notes along the grid, like Ardour's note brushing), slice, select, mute and delete, on the same keys as in the playlist.
- **Ghost notes:** notes from other tracks, chosen per track, show faintly behind the clip being edited. Click one to jump to its track.
- **Key and chords:**
  - rows outside the project scale are shaded;
  - the tones of the current chord on the chord lane are highlighted;
  - snapping to the scale is optional.

  Out-of-scale shading should be subtle and explained, because an Ardour user found its version confusing.
- **Chord stamps:** pick a chord type and click to place it. When the song has a chord lane, there is also "stamp the current chord".
- **Transforms with live preview:** quantize (with strength and swing), legato, chop, strum, arpeggiate, flam, humanize, transpose (diatonic or chromatic), velocity scaling, reverse and flip.
  - Each shows a preview before it is applied.
  - Its settings stay editable until you commit, which avoids Live 12's gotcha where arpeggiated notes become permanent once you select another clip.
  - Each has a hotkey.
- **Several clips at once:** edit the notes of several selected clips together, each in its own color.
- **Lanes under the notes:** velocity is always shown. CC, pitch-bend and per-note expression lanes appear on demand.
- **Drum mode:** for drum-sampler tracks, rows show pad names instead of piano keys and notes are drawn as hits without length. Rows can be reordered or hidden.
- **Slides:** pitch glides between notes are sent as MPE note expression to plug-ins that support it. FL Studio's slide notes only work with its own instruments.
- **Capture:** press Capture after playing and the last phrase becomes a clip ([section 17](#17-recording-and-capture)).

## 8. The mixer

- **Channels:** one per track, in playlist order and colors. Folders appear as bus channels with their tracks grouped beside them.
- **Each channel strip shows:**
  - input and output;
  - device slots, each with a few key controls, so plug-in windows rarely need opening (like Logic's Smart Controls and Studio Pro's Channel Overview);
  - sends, pan, fader, meter;
  - mute, solo and record-arm;
  - CPU.
- **Returns:** add a return track (for a shared reverb, say) and send to it from any track, pre- or post-fader per send. Tracktion has no pre/post switch, so Phazite places the send before or after the fader in the device chain.
- **Sidechain:** any device with a sidechain input gets a source picker that lists the tracks. The source must be a track, not a folder bus (an engine limit).
- **Routing view:** a diagram of every track, bus, send and sidechain drawn as cables, like Reason's back panel. Drag a cable to reroute it.
- **Master:** ends with a loudness meter (LUFS and true peak), built on Tracktion's real-time-safe EBU R128 meter.
- **One output per track:** FL Studio's "route one insert to several inserts" is done with sends.
- **Later:** VCA faders on folders.

## 9. Devices, containers and presets

**Device chain:**
- each track has an instrument first, then its effects;
- drag devices to reorder them; bypass, dry/wet and A/B comparison for each.

**Containers:**
- Select some devices and choose "Group into container". The container shows its devices in a row, with up to eight macro knobs that can drive any parameter inside, and saves as one preset. This is like Reason's Combinator and Ableton's racks.
- Parallel chains inside a container come later; Tracktion racks can be wired freely.
- Engine note: Tracktion runs one shared processing graph for every instance of a rack type, so each Phazite container gets its own rack type.

**Presets:**
- device presets, for built-ins and for plug-in state;
- track presets: drag a track into the browser to save its device chain and settings, as in Studio One;
- container presets.

**Plug-in windows:** each opens in its own window, kept above the main window, and its position is remembered.

**Track limit:** Tracktion's default of 16 devices per track is raised. FL Studio's 10 effect slots per mixer track is a known complaint ([B12](#appendix-b-sources)).

## 10. Automation and modulation

Producers in Phazite's audience automate "all sorts of sliders", so this has to be effortless.

**Right-click any control,** built-in or a plug-in parameter, and choose:
- **Automate:** an automation lane appears under the track, ready to draw.
- **Create automation clip:** an FL Studio-style clip on the track, which can be reused and aliased.
- **Modulate:** adds an LFO, envelope follower, step sequence, random source, breakpoint envelope or note follower. It shows as a ring on the control; drag the ring to set the amount.
- **MIDI learn.**

A hotkey automates the last-touched control (FL Studio's "last tweaked", Cubase 15).

**Lanes:**
- draw with the pencil, or use line and curve shapes (sine, ramp, steps);
- bend segments; select and move points;
- edit points from the keyboard;
- write modes per track: read, touch, latch and write.

**Automation clips:**
- In Tracktion, clip automation curves can be absolute, relative or scaling, can loop independently of their clip, and can target any parameter in the project. That last point was checked in the engine source: the target is looked up project-wide.
- Phazite shows them as a clip type of their own.

**How the three layers combine:**
- the lane sets the base value, automation clips adjust it, and modulators move around it;
- each control shows all three: its base value, the automated value and the modulation range.

**Engine notes:**
- modulators update once per audio block, which is fine for most uses but coarse for very fast modulation;
- parameters inside containers can't be recorded live yet.

## 11. The browser

- **One browser, two ways in:**
  - **folders**, showing your sample folders as they are on disk (FL Studio users ask other DAWs for this);
  - **tags and search** across everything: instruments, effects, plug-ins, presets, samples, generated sounds and projects.
- **Preview:**
  - click to hear;
  - loops play in time with the project's tempo and key, one-shots at their own pitch;
  - optionally, previews play through the selected track's devices.

  Loops are detected and their tempo measured on import (Tracktion includes a tempo detector).
- **Drag and drop:**
  - onto empty space: a new track;
  - onto a track: adds a clip or a device;
  - onto a device slot: replaces it;
  - onto a drum pad: loads the sound.
- **Plug-in picker (F8):** type to filter, then press Enter to add the plug-in to the selected track or a new one.
- **Favorites, recent items and collections.**
- **Later:** automatic tags for your own samples, from analysis (one-shot or loop, drum type, brightness, key, tempo), and "find similar".

## 12. Commands, keys and help

**Command registry:**
- every action has an ID, a name, a description, search synonyms (searching "cut" finds Split), a category and a default key;
- it is built on JUCE's `ApplicationCommandManager`, which Tracktion's `UIBehaviour` already expects.

**Command palette (Ctrl+K):** a fuzzy search over every command, showing each one's shortcut. Enter runs the selected command.

**Keymaps (decided: whatever users find most intuitive; FL Studio style is fine):**
- **First launch asks** "Coming from FL Studio, Ableton Live, Logic, Pro Tools, or new to music software?" and loads a matching keymap. Digital Performer 12 has switchable keysets that copy other DAWs' shortcuts ([B13](#appendix-b-sources)). v1 ships the Phazite default and FL Studio keymaps, with Ableton Live next.
- **The Phazite default** follows conventions that most DAWs and the operating system share:
  - Space plays and stops;
  - Ctrl+Z and Ctrl+Shift+Z undo and redo;
  - Ctrl+S saves;
  - Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste.

  It adds FL Studio's single-key tools, which reviewers praise.
- **Any command can be rebound.** Ctrl+Alt+click on a menu item assigns a shortcut directly, as Logic's Control-click does.
- **The computer keyboard plays notes**, on by default so a new project makes sound right away (step 0.7):
  - the layout of Ableton Live and Logic Pro: the middle row of letters plays the white keys from middle C, the row above the black keys, Z and X change the octave and C and V the velocity in steps of 20 ([B40](#appendix-b-sources));
  - it takes only keys pressed without Shift, Ctrl, Alt or Cmd, and never keys typed into a text field;
  - a top-bar button and the Options menu turn it off. In the FL Studio keymap Ctrl+T does too, and F10 opens the MIDI settings, as in FL Studio ([B41](#appendix-b-sources)). FL's manual doesn't document which keys play which notes, so that keymap uses the same layout;
  - **single-key shortcuts (decided, open question 9):** as in Ableton Live 12 ([B40](#appendix-b-sources)), while the computer keyboard plays notes, Shift and a letter run that letter's shortcut (FL Studio's tools, for example), unless Shift and that letter have a shortcut of their own. With the computer keyboard off, the letters alone run them. So a keymap shouldn't give Shift and a letter a different command than the letter alone.
- **How "most intuitive" gets settled:**
  - watch three to five people, both FL Studio users and complete beginners, do the same tasks with a prototype: place a kick on every beat, trim, fade, duplicate a section, write a chord;
  - count hesitations and errors, and adjust the defaults from that.

**Macros:** chain commands into one, like REAPER's custom actions, and bind the result to a key.

**Named undo:**
- every command is one named undo step ("Split clip", "Apply outline"), and an undo history panel lists them;
- Tracktion groups changes on a timer and doesn't name them, so Phazite starts a named transaction for each command.

**Help:**
- the hint bar is always there;
- hover descriptions are on by default for new users and can be turned off with one click;
- later, a manual page for the hovered control.

## 13. Getting started and song outlines

**First launch** needs no account and no sign-in:
1. "Coming from…?" chooses a keymap.
2. Audio setup: pick an output, or an ASIO® driver when an audio interface is present.
3. A starter project opens with a drum track loaded with a kit and a synth track. The computer keyboard plays notes, and the hint bar suggests a next step ("Press Space to play", "Drag a kick from the browser").

**New project:** a dialog with tempo, key and an optional song outline.

**Song outlines (decided)** are templates that lay out a song's sections, so newcomers see how songs are usually shaped. Applying one:
- creates colored sections with typical lengths;
- draws an **energy curve** above them;
- gives each section a one-line tip, such as for a build-up: "Raise the energy: risers, a snare roll, open a filter".

Sections stay fully editable, and later features use them; chord suggestions, for example, can tell a chorus from a verse.

v1 outlines (decided), with typical lengths in bars at the project tempo, all editable:

| Style | Sections (bars) |
|---|---|
| Pop | Intro 4, Verse 8, Pre-chorus 4, Chorus 8, Verse 8, Pre-chorus 4, Chorus 8, Bridge 8, Chorus 16, Outro 4 |
| Rock | Intro 8, Verse 16, Chorus 8, Verse 16, Chorus 8, Solo 8, Bridge 8, Chorus 16, Outro 8 |
| Electronic (house, EDM) | Intro 16, Build-up 8, Drop 16, Breakdown 16, Build-up 8, Drop 16, Outro 16 |
| Hip-hop and trap | Intro 4, Hook 8, Verse 16, Hook 8, Verse 16, Hook 8, Outro 4 |

- **Later:** drum and bass, techno, dubstep, lo-fi, trance and future bass, plus DJ-friendly variants with 32-bar intros and outros.
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

**Engine note:** Tracktion's chord progressions accept Roman numerals only for the seven diatonic chords. They are stored lowercase as a degree plus 6, 7 or 9, and take their quality from the scale (`tracktion_Musicality.cpp`). Borrowed chords such as ♭VII and secondary dominants such as V/V can't be written. Phazite therefore:
- keeps progressions in its own format: degree, accidental, quality, extensions, inversion and length;
- writes concrete chord names, such as "Bb" or "Gm7", into the engine's chord clips, which the engine also accepts.

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
- **Live MIDI:** every MIDI keyboard, and the computer keyboard, plays one track, without arming it. For now that is the project's first track with an instrument; once tracks can be selected (step 1.1), it is the selected track.
- **MIDI recording** from controllers and the computer keyboard, with overdub or replace, and optional input quantize.
- **Capture:** Phazite keeps recent MIDI input (Tracktion keeps 30 seconds by default), and one key turns it into a clip. This is Live's Capture MIDI and Logic's Flashback Capture ([B30](#appendix-b-sources)). If the project is empty, Capture also detects the tempo.
- **Audio recording:** record-arm, input monitoring, count-in, punch-in over the loop range, and basic takes.
- **Metronome** with count-in.

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
- Saves use the engine's safe save, which writes a temporary file and then renames it, and keep a rolling set of backups.
- **Autosave** writes a separate file every few minutes. If Phazite crashed, the next launch offers to restore it.
- Project files contain no personal data, and media paths are relative to the project folder ([section 22](#22-privacy-licences-and-trademarks)).
- Phazite-specific data is stored in the project file alongside the engine's: sections, the chord lane's source, the outline style and the window layout.

**Export:**
- WAV and FLAC. MP3 needs an encoder (**Open**).
- Choice of sample rate and bit depth.
- Normalize to a loudness target (LUFS) with a true-peak ceiling.
- Effect tails, and export of a chosen range or sections.
- Stems: one file per track or bus.

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

**After v1: sandboxed hosting, on by default.**
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

**Licences:**
- **Compatibility:** only AGPL-compatible code, models and data; the list is in `CLAUDE.md`.
- **Notices:** third-party notices ship with the app, for example SoundTouch (LGPL-2.1-or-later), Signalsmith Stretch (MIT) and the Inter font (SIL OFL 1.1).
- **Fonts:** fonts may use the SIL Open Font License 1.1. They ship as separate files with their licence text.
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
- Time-stretching. One compile flag enables the MIT-licensed Signalsmith Stretch, which is better than the SoundTouch mode Phazite uses today.
- Partial DAWproject support and clip-launcher machinery.
- Scales, chords, key detection from MIDI notes, and a chord, bass and arpeggio generator.

**What Phazite builds:**
- All of the UI. This is by far the largest piece of work: Waveform, on the same engine, shows that the interface is where DAWs differ ([B9](#appendix-b-sources)).
- Commands and keymaps, selection and copy/paste handlers, and named undo.
- Section editing, the chord format, the song outlines and the progression library.
- Built-in devices with editors, the drum synth and the sound generator.
- Autosave, crash recovery, presets and templates.
- A scan timeout. After v1: plug-in sandboxing and CLAP.

**Watch-outs, from reading the engine source:**
- **Personal data:** saved projects contain the operating system user's full name (see [section 22](#22-privacy-licences-and-trademarks)). Handled in step 0.2: `phazite::PropertyStorage` gives no user name.
- **UI hooks:** MIDI learn, app commands, plug-in windows and freeze need a `UIBehaviour`. Step 0.2 added `phazite::UIBehaviour`.
- **Edit loading:** `EditLoader` builds projects on a background thread. That breaks the `CLAUDE.md` rule that Tracktion objects are created on the message thread, so Phazite loads them with `phazite::loadEdit()` (step 0.2).
- **MIDI device scan:** `DeviceManager::initialise()` asks for a MIDI scan in 5 ms, then restarts the same timer with the 4-second hot-plug check, so MIDI inputs would appear 4 seconds after startup. `phazite::createEngine()` asks for the scan again (step 0.7).
- **State written while playing:** Tracktion writes the playhead position into the project, and adds input-device entries when it prepares playback. `phazite::Project` doesn't count those as unsaved changes (step 0.7).
- **Project IDs:** `te::loadEditFromFile` replaces the ID saved in a project with the one it is given, but the Edit names its temporary folder (freeze files, comp renders) after that ID. `phazite::Project` reads the saved ID first, so a project keeps it, and gives a Save As copy a new one (step 0.5).
- **Undoing a parameter change:** undo puts back a parameter's stored value (a track's volume, say) but not the value the engine plays with. `phazite::undo()` and `redo()` set every parameter that isn't automated to its stored value afterwards. Tracktion only stores a value once it first changes, and undoing that first change removes it again, so Phazite's controls store the value, without an undo step, before changing it; otherwise the fix would cost the redo history (step 0.6). Worth reporting upstream.
- **Mute and solo** aren't part of the undo history.
- **Track insert points:** `TrackInsertPoint (track, false)` means "after this track", except for the first top-level track, where it means the start of the list. `phazite::getInsertPointAfter()` names the track explicitly (step 1.1).
- **Aliases:** changes to linked clips are mirrored without undo, so Phazite's alias commands must edit every linked clip inside one undo step.
- **Folder buses:** a submix folder has no output of its own; it uses its first child track's output. The bus routing UI has to account for this.
- **Plug-in parameters:** hosted plug-in parameters are identified by index ([section 19](#19-plug-in-hosting-and-stability)).
- **Real-time rules:** several engine paths break them ([section 20](#20-performance)).

## 24. Open questions

1. ~~AI engines in v1~~ **Decided:** algorithmic in v1, trained models later ([section 16](#16-ai-and-generative-features)).
2. ~~Outline styles first~~ **Decided:** pop, rock, electronic, and hip-hop/trap ([section 13](#13-getting-started-and-song-outlines)).
3. ~~UI font~~ **Decided:** Inter, bundled, under the SIL Open Font License; `CLAUDE.md` now allows the OFL for fonts ([design language](#design-language)).
4. ~~Engine fixes~~ **Decided:** try simple workarounds first. If they make Phazite's code too awkward, or upstream issues block us, keep a fork of Tracktion Engine. Problems are reported upstream either way ([roadmap](roadmap.md), step 0.3).
5. **MP3 export:** it needs an encoder such as LAME (LGPL). Include it or not?
6. **Airwindows:** ship its 186 MIT-licensed effects as extra devices? They also lock on the audio thread today.
7. **Plug-in sandboxing:** first update after v1 (proposed), or before v1?
8. **Ableton Live keymap:** in v1, or soon after?
9. ~~Single-key tools and the computer keyboard~~ **Decided:** Ableton Live's way. The computer keyboard plays notes by default, and Shift and a letter run that letter's single-key shortcut ([section 12](#12-commands-keys-and-help)).

## 25. Decision log

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

## Appendix A. Research summary

In October 2026, research agents reviewed about 20 DAWs, and the Tracktion Engine source was audited against what a DAW needs.
- **Sources of opinion:** reviews (Sound On Sound, MusicTech, Attack, Tape Op, MusicRadar), KVR and vendor forums.
- **Limits:** Reddit and Gearspace couldn't be fetched, so treat opinions as a sample, not a census.

| DAW | Loved | Borrowed for Phazite | Avoided |
|---|---|---|---|
| FL Studio 2026 | The piano roll ("like a paint program"); fast pattern painting; lifetime free updates | The piano-roll mouse model and transforms, single-key tools, playlist colors and folders, F-keys | The channel rack / pattern / playlist / mixer split; invisible routing; floating windows |
| Ableton Live 12 | Session View for sketching; MIDI Tools; the tag browser; Capture MIDI; Bounce in Place; Info View | Capture, transforms with preview, hover help, a per-track CPU meter | Shortcuts that can't be remapped; clips that silently override the arrangement |
| Bitwig Studio 6 | Modulators on anything; automation clips and aliases; tools on number keys; a footer showing mouse actions; plug-in sandboxing | Drag-to-modulate, aliases, the hint bar, sandboxing | Depth before basics; odd default shortcuts; a messy browser |
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

**Where the field is converging:**
- Each track is a row, a chain and a channel.
- Three automation layers: lanes, automation clips and modulators.
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
