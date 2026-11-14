---
title: Exporting
description: Export the song or its stems as WAV or FLAC, normalized to a loudness if you like, and exchange songs with other DAWs as DAWproject files.
sidebar:
  order: 11
---

## Export Song

**File > Export Song** (**Ctrl+Shift+R**) opens the Export window. **Help > Lessons > Arrange a Song** ends with an
export, if you'd like a walk through it.

| Setting | Choices |
|---|---|
| Export | **The song**, or **Stems: a file for each channel** |
| Format | **WAV**, or **FLAC (lossless, smaller)** |
| Sample rate | 44.1, 48, 88.2 or 96 kHz |
| Bit depth | **16-bit, dithered**, **24-bit**, or **32-bit float** (WAV only) |
| Loudness | **Off: as it plays**, or **Normalize** to a **Target** and a **Ceiling** (the song only) |

Phazite remembers your choices for the next export.

**The song** is rendered through the master's effects, from its start to the end of the bar its last clip ends in,
and then on until its echoes and reverb tails have died away (10 seconds at most). Muted tracks stay silent.

**Normalize** turns the whole song up or down to an integrated loudness, from -30 to -5 LUFS, without taking its true
peak over the ceiling, from -6 to 0 dBTP. It is one change of level and never limits, so a mix with high peaks can end
up under the target; the window then says by how much.

**Where it goes.** The file goes into the project's `Renders` folder, named after the project; the window shows the
name, and a button beside it chooses another file. A file that is there already is replaced only after you say so,
and only once the new one is complete, so a cancelled or failed export leaves the old one as it was.

The export runs in the background, with a progress bar and **Cancel**, and playback stops for it. When it is done, a
button shows the file in its folder.

A plug-in that crashes while the song renders fails the export, which names the plug-in: reload it and export again.
Nothing is written without it.

## Stems

Stems are a file for each channel that reaches the master: each track outside a bus, each bus with the tracks in it,
and each return. Together they add up to the mix without the master's effects, ready for mixing elsewhere.

- Each stem goes through its channel's effects, fader and pan. The sends go into the returns' own stems.
- A channel that is muted, or that a solo leaves silent, has no stem.
- Every stem starts at the song's start, so they line up wherever you import them.
- Stems take the song's format, sample rate and bit depth, without Normalize, so they keep their levels.
- They go into a folder called "*project* Stems" in the `Renders` folder, each file named after its channel and
  numbered in the mixer's order ("01 Kick.wav"). Exporting again replaces the folder, after asking, and the old one
  goes to the recycle bin.

## DAWproject

DAWproject is an open format that other DAWs open, to take a song from one DAW to another.

**File > Export as DAWproject...** writes the song into the project's `Renders` folder as a `.dawproject` file. It
holds the tracks, folders, returns and sends, each channel's volume, pan, mute, solo and output, the MIDI clips'
notes, the audio clips' files with their fades, the tempo and time signature with their changes, the sections as
markers, the volume and pan automation, and VST®3 plug-ins with their settings and automation.

What another DAW can't play is bounced: Phazite's own devices and containers, modulation, an audio clip's gain,
stretch, pitch or reverse, and frozen tracks. Such a track's sound goes onto an audio track of its own beside it, and
the track itself is kept, muted, with its notes and clips, so the other DAW has both. The file's comment says what was
left out.

**File > Import DAWproject...** opens a DAWproject file as a new project. A file exported from a commit of the open
project comes back as a branch from that commit ([Source control](source-control.md)). A notice then says what it
couldn't bring in, track by track: plug-ins and devices by name, sends, automation and groups.

VST is a registered trademark of Steinberg Media Technologies GmbH.

## Not yet

- Exporting a chosen range or some sections only: planned.
- MP3: not planned. WAV and FLAC cover delivery.
- A DAWproject's plug-ins and sends taken into a new project: planned.
- Warp markers in DAWproject files: planned.
- An archive of a project with all its files, to send to someone: planned.
