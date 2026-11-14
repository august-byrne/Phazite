---
title: Chords
description: The chord lane, the song's keys, and the Chord Progressions window.
sidebar:
  order: 10
---

The chord lane holds the song's chords along its time, as a guide for what you write over them: the playlist shows
them, and the [piano roll](piano-roll.md#keys-and-scales) tints the rows of each chord's notes while it lasts. The lane
itself plays nothing. **Help > Lessons > Write Chords** walks you through it.

## The chord lane

The chord lane is a thin row under the playlist's bar numbers, above the tracks. It shows only while the song has
chords: it appears with the first chord and goes with the last. Each chord shows its name and its numeral in the key
("Am, vi"), and a line marks where the key changes.

To put the first chord on the lane, right-click the bar numbers and choose **Add Chord Here**, or use the Chord
Progressions window (below).

| Gesture | Does |
|---|---|
| Click empty space in the lane | A menu of the key's seven chords, then **Outside the Key**. The chord you choose goes there, a bar long or up to the next chord |
| Click a chord | A menu of chords to put in its place, then **Kind**, **Inversion** and **Delete Chord** |
| Drag a chord's right edge | Makes it longer or shorter, on the grid |

- **Kind** changes the chord on the same root: major, minor, sevenths, sus2 and sus4, diminished, augmented, sixths,
  ninths, and the power chord.
- **Inversion** offers the root position and each inversion the chord has.
- **Outside the Key** has chords borrowed from the parallel key, and the dominants of the scale's chords.

Both menus end with **Change Key from Here** (or **Remove Key Change Here**, at a key change) and **Chord
Progressions**. The lane's header has a **...** menu with **Chord Progressions** and **Delete All Chords**.

Chords move with the song's time: inserting or removing time, and moving or copying a section or a range of time, take
them along.

## Keys

The lane keeps each chord as a numeral in the key where it is (I, IV, vi...), so a change of key moves every chord
with it.

- The song's key is **Key** in the inspector when nothing is selected.
- **Change Key from Here**, in the lane's menus, changes the key from that chord or bar on. The chords after it keep
  their numerals and move with the key. **Remove Key Change Here** takes it away again.

A track's own key, which shades the piano roll's rows, is set in the piano roll ([Keys and
scales](piano-roll.md#keys-and-scales)).

## Chord progressions

**Add > Chord Progressions** (also in the lane's menus) opens the Chord Progressions window: 40 common progressions,
each with its name, its chords in the song's key, its numerals and its feel. **Mood** and **Genre** narrow the list.

- **Play** plays the selected progression in the song's key and tempo, as held chords on Phazite's synth, looping;
  **Stop** stops it.
- **Put on the Chord Lane** lays it over what **Put on** says: **The selected sections**, **The selected time range**
  or **The whole song** (each offered when it applies). The progression repeats to fill the place, in place of the
  chords there, as one undo step.

A progression is written for major or for minor. In a song whose key has the other third, it plays in the major or
minor key with the song's notes, so A minor's i–VI–III–VII in C major is Am–F–C–G.

Some of the 40:

| Progression | Numerals |
|---|---|
| Pop anthem | I–V–vi–IV |
| Doo-wop | I–vi–IV–V |
| 12-bar blues | I7, IV7, I7, V7, IV7, I7, V7 over twelve bars |
| The ii-V-I | ii7–V7–Imaj7 |
| Lo-fi ninths | ii9–V9–Imaj9–vi9 |
| Epic minor | i–VI–III–VII |
| Andalusian cadence | i–VII–VI–V |

To play the chords, write a part over them in the piano roll: its chord stamps and the **Arpeggiate...** transform
help ([The piano roll](piano-roll.md#chord-stamps)).

## Not yet

- Chord suggestions (the next chord, a section filled): planned.
- Chords found in MIDI or audio: planned.
- Hearing a progression on the selected track's instrument: planned.
- Saving your own progressions: planned.
- Parts made from the chords automatically: not planned; the piano roll's chord stamps and Arpeggiate... write them.
