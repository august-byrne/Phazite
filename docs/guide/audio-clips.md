---
title: Audio clips
description: The audio clip editor - trimming, fades, gain, pitch, reverse, following the tempo, warp markers, Quantize Audio - and consolidating clips.
sidebar:
  order: 5
---

An audio clip plays a part of an audio file. You can trim, fade and copy it in the [playlist](playlist.md), and edit
the rest in the audio clip editor: the clip editor's tab in the lower panel, called **Audio clip** while it shows one.
Select an audio clip, or double-click it, and press **F7** if the tab isn't showing. It edits the first audio clip
selected.

## In the playlist

- **Trim:** drag a clip's left or right edge.
- **Fade:** drag from a clip's top-left or top-right corner.
- **Crossfade:** drag where two audio clips touch; each plays over the other while one fades out and the other in.
- **Fade shapes:** **Linear Fades**, **Convex Fades**, **Concave Fades** and **S-Curve Fades**, in the clip's
  right-click menu under **Fade Shape**, set the shape of the selected clips' fades.
- **Gain** is in the inspector and in the audio clip editor.

## The audio clip editor

The editor shows the whole file, as wide as the editor: it never scrolls or zooms, so the clip can't get lost in it.
The part the clip plays is in its track's colour, and what its edges trim off is shaded, so you can drag them back out.
The ruler counts the song's bars where the audio plays, so trims and fades line up with the song's grid. There are no
tools to choose: everything is a handle on the audio, and **Alt** places freely, as everywhere.

| On the audio | Does |
|---|---|
| Drag an edge (a bracket with a grip) | Trims the clip |
| Drag a round handle at a top corner | Fades the clip in or out |
| Double-click a fade's corner handle | Takes the fade away |
| Drag the hollow handle half way along a fade | Bends its curve |
| Double-click that handle | Straightens the fade |
| Right-click that handle | Switches the fade between **Curve** and **S-Curve** |
| Right-click elsewhere on the clip | The four fade shapes |
| Drag the clip's part | Plays another part of the audio: the clip stays where it is in the song |

## Gain, pitch and reverse

Down the editor's left, as fields to drag, scroll or type in:

| Control | Does |
|---|---|
| Gain | From -60 to +24 dB. The button beside it, **Normalize**, sets the gain so that the loudest peak of the part the clip plays is at -1 dB. It changes no file |
| Pitch | Up to 48 semitones up or down. The clip keeps its length |
| Fine | Up to 50 cents up or down |
| Reverse | Plays the same part of the audio backwards. The waveform is drawn backwards too |

**Reverse** and **Normalize** are commands too, for every selected audio clip, in a clip's right-click menu in the
playlist.

## Following the tempo

With **Follow tempo** on, the clip stretches with the song's tempo and keeps its pitch. **Tempo**, beside it, is the
tempo the audio was played at: it comes from the file when the file says, or else is the song's tempo where the clip
starts, so nothing changes until either tempo does. Set it to the audio's real tempo to put the audio in time with the
song.

**Follow Tempo** is also in a clip's right-click menu in the playlist, for every selected clip. **Samples Follow
Tempo**, under the transport bar's tempo field, switches it for every sample in the song at once
([The window](window.md#the-transport-bar)). A clip that follows the tempo shows a metronome mark at its top right.

Stretched, pitched and reversed audio is prepared in the background. Meanwhile the clip is silent, the editor says
"Preparing the audio. The clip is silent until it is ready." and the status bar shows **Stretching**.

## Warp markers

A warp marker pins a moment of the audio to a beat of the song. Between two markers the audio stretches to fit; before
the first and after the last it plays straight.

Along the top of the audio, the hits Phazite finds in it (drum hits, notes starting) show as small ticks, and the
markers as flags with a line down the audio.

| Along the top | Does |
|---|---|
| Double-click | Pins the audio there with a marker; on a hit, pins the hit |
| Drag a marker or a hit | Moves it, on the grid unless **Alt** is held. A hit dragged becomes a marker |
| Double-click a marker | Removes it |
| Right-click | **Remove Warp Marker** (over a marker), **Quantize Audio** and **Remove Warp Markers** |

A clip with markers follows the tempo; turning **Follow tempo** off takes its markers away.

## Quantize Audio

**Quantize Audio** moves each hit the clip plays to the nearest grid line, with a marker of its own, so that a loose
drum loop plays on the beat. The clip's start stays where it is. In the audio clip editor it is **Quantize**
(**Ctrl+U**), to the editor's grid; in the playlist, **Quantize Audio** in a clip's right-click menu works on every
selected audio clip, to the playlist's grid.

## Consolidating

**Consolidate** (**Ctrl+J**) joins the selected clips on each track, or what lies in the selected range of time, into
one clip. Audio clips become one new audio file of what they play, before the track's effects; MIDI clips become one
MIDI clip of their notes.

## Not yet

- A choice of stretch modes (one for drum loops, say), and formant control: planned.
- Warping from a tempo detected in the audio, and a sensitivity for the hits: planned.
- Gain changes inside a clip: planned.
