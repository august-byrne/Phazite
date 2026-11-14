# Chords

*Design section 14.*

The chord lane holds the song's chords along its time, as a guide to what is written over them: the playlist shows
them, and the piano roll tints the rows of each chord's notes. A library of chord progressions puts chords on the
lane. The lane plays nothing.

## Status

| Feature | State | Note |
| --- | --- | --- |
| The chord lane in the playlist, there while the song has chords | Built | |
| Adding, changing, resizing and deleting chords | Built | |
| Chords kept as numerals in a key; key changes along the song | Built | |
| Chords from outside the key, kinds and inversions | Built | |
| The Chord Progressions window: 40 progressions by mood and genre | Built | |
| Hearing a progression in the song's key and tempo | Built | On Phazite's synth, not the selected track's instrument |
| Putting a progression on sections, a time range or the whole song | Built | |
| The piano roll tints each chord's notes | Built | [piano-roll.md](piano-roll.md) |
| Chords move with the song's time | Built | |
| Parts made from the chords (chord part, bass line, arpeggio) | Declined | A weak second way to what the piano roll's tools write |
| Chord suggestions | Planned | After v1 ([ai.md](ai.md)) |
| Chords detected in MIDI or audio | Planned | No roadmap step |
| Notes played by hand following the chords | Planned | No roadmap step |
| Outlines that suggest progressions for their sections | Planned | With chord suggestions |
| Progressions saved by the user | Planned | No roadmap step |

## The chord lane

The chord lane is a row 18 pixels tall, as the sections' band is, under the playlist's bar numbers and above the
tracks ([playlist.md](playlist.md)). It shows only while the song has chords. Each chord shows its name and its
numeral in the key there, and a line marks where the key changes.

A song's first chord comes from the Chord Progressions window, or from **Add Chord Here** in the time bar's
right-click menu, beside Add Section Here. When the last chord is deleted, the lane goes.

Its header, over the track headers, has its name and a **...** menu with **Chord Progressions** and **Delete All
Chords**. Delete All Chords is one undo step and takes the key changes after the song's start too; the song's key
stays.

The sections stay in a band of their own over the bar numbers: sections span time and move what is in them, and the
bar numbers already take three drags (where playback starts, the playhead and the loop).

## Editing chords

| Gesture | What it does |
| --- | --- |
| Click empty space in the lane | A menu headed "Add a chord in bar N, in (the key)": the key's seven chords, then **Outside the Key** (chords borrowed from the parallel key, and the dominants of the scale's chords). The one chosen is added there, a bar long, or up to the next chord |
| Click a chord | A menu headed by the chord: the same chords to put in its place (the current one ticked), **Kind**, **Inversion**, and **Delete Chord** |
| Drag a chord's right edge | Makes it longer or shorter, on the grid, over the chord after it if it grows |

Both menus end with **Change Key from Here** (and **Remove Key Change Here** at a key change) and **Chord
Progressions**.

- **Kind** changes the chord on the same root: major, minor, sevenths (dominant, major, minor, half-diminished,
  diminished), sus2 and sus4, diminished, augmented, sixths, ninths, and the power chord.
- **Inversion** offers Root Position and the First, Second and Third Inversion, as many as the chord has notes.
- Each change is one undo step ("Add Chord", "Change Chord", "Change Chord Length", "Delete Chord").
- Names spell each note by its step of the scale: the leading note of D harmonic minor is C♯, not D♭.

## Keys

Chords are saved as numerals in the key where they are, so a change of key moves them all.

- The inspector's **Key**, on the song's page that shows with nothing selected, changes the song's key.
- **Change Key from Here**, in the lane's menus, adds a key change at a chord or a bar; the chords after it keep
  their numerals and move with the key. **Remove Key Change Here** takes one away, and the key before it goes on.
  The key at the song's start always stays.
- Until a key is chosen, the lane is in the new project's key, or else C major.

A numeral holds the step of the key's scale (1 to 7), an accidental (flat, none or sharp), the chord's kind and its
inversion. So chords from outside the key can be written: ♭VII in a major key, or a major chord on the second step.

## Chord progressions

The Chord Progressions window (**Add > Chord Progressions**, the lane's menus, the command search) lists the 40
progressions that ship with Phazite. Two menus narrow them by mood and by genre. Each shows its name, its chords in
the song's key, its numerals and its feel.

- **Play** hears the selected one in the song's key and tempo: held chords over their roots on Phazite's synth,
  looping in time with the song.
- **Put on the Chord Lane** lays it over what the **Put on** box beside it says: the selected sections, the selected
  time range (each offered while there is one) or the whole song (once from the start in an empty song). The
  progression repeats to fill the range, in place of the chords there, as one undo step ("Apply Chord
  Progression").

A progression is written for major or for minor. In a song whose key has the other third it plays in the major or
minor key with the song's notes: A minor's i–VI–III–VII in C major is Am–F–C–G, and a progression for major in a
minor key plays in its relative major. In a mode, it plays in the major or minor key with the mode's notes.

Some of the 40:

| Progression | Numerals | Written for |
| --- | --- | --- |
| Pop anthem | I–V–vi–IV | Major |
| Wistful pop | vi–IV–I–V | Major |
| Doo-wop | I–vi–IV–V | Major |
| Three-chord rock | I–IV–V, the V for two bars | Major |
| Mixolydian rock | I–♭VII–IV–I | Major |
| 12-bar blues | I7, IV7, I7, V7, IV7, I7, V7 over twelve bars | Major |
| Royal road | IVmaj7–V7–iii7–vi | Major |
| The ii-V-I | ii7–V7–Imaj7 | Major |
| Lo-fi ninths | ii9–V9–Imaj9–vi9 | Major |
| Epic minor | i–VI–III–VII | Minor |
| Driving minor | i–VII–VI–VII | Minor |
| Dark minor | i–iv–v | Minor |
| Andalusian cadence | i–VII–VI–V | Minor |

The progressions are a data file (`resources/progressions/progressions.json`). Each entry has an id, a name, the
scale it is written for, its feel, its moods and genres, and its chords: numerals separated by spaces, each a bar
long unless `*` and a number says otherwise ("I*2 IV V*0.5 V7*0.5"). A numeral is written with an optional "b" or
"#", the step in Roman numerals (upper case for a major third, lower case for a minor third), its kind ("7",
"maj7", "m7b5", "sus4", "9"...), and an inversion ("/1" to "/3").

## Chords and the rest of the song

- **Time:** chords go with the song's time as sections do: inserting or removing time, and moving or copying a
  section or a time range, move them.
- **The piano roll** tints the rows of each chord's notes while the chord lasts, in any key.
- **Nothing plays from the lane.** The progressions' preview and the example songs' parts write chords as notes
  (held triads, spread chords, offbeat stabs and roots), voiced so that each chord moves as little as it can from
  the one before; the lane itself makes no clips.
- The song keeps its chords once, in its chord lane (`CHORDLANE` in the project file).

## Decisions

- **The lane shows only while the song has chords,** so an empty lane takes no row of the playlist.
- **The chord lane is a guide that plays nothing,** so it can go away without anything playing from somewhere
  unseen.
- **No parts generated from the chords.** Patterns made by rule sound static, and the piano roll's chord stamp and
  Arpeggiate transform write the same notes by hand. Making parts could come back as a piano-roll tool that writes
  notes once, or as players worth hearing ([ai.md](ai.md)).
- **Chords are numerals in a key,** not note names, so that a key change moves them and a progression plays in any
  key.
- **Sections have a band of their own,** not the bar numbers' row.
- **A new chord is a bar long.**
- **The song's key is in the inspector,** not in the lane's header.

## Not built

- Parts made from the chords, following them until edited: Declined.
- Chord suggestions (next chord, fill a section, replace, fit my melody): Planned, after v1.
- Chords detected in MIDI or audio: Planned.
- Notes played by hand following the chords: Planned.
- Outlines that suggest progressions for their sections: Planned.
- Hearing a progression with the selected track's instrument: Planned.
- Progressions saved by the user: Planned.

## In the code

- `src/model/chords/ChordValues.h`: numerals, chord names, the key a progression plays in, and chords written as
  notes (`makePartNotes()`).
- `src/model/chords/Progressions.h`, `resources/progressions/progressions.json`: the progressions.
- `src/model/chords/ChordModel.h`: the lane as the window reaches it: its chords, keys, edits and the progressions
  laid on it; `internal/Chords.h`: the song's side, with the time edits that move chords.
- `src/ui/playlist/ChordLane.h`: the lane and its header; `src/ui/shell/ProgressionsPanel.h`: the Chord Progressions
  window.
- `src/model/base/internal/Schema.h`: `CHORDLANE` and its children.
- Tests: `tests/ChordValueTests.cpp`, `ChordModelTests.cpp`, `ChordLaneTests.cpp`.
