---
title: Getting started
description: Get Phazite, set up your audio, play an example song and make a first beat.
sidebar:
  order: 1
---

Phazite is pre-alpha: it works, but it changes from one build to the next, and projects saved now may not open in later builds. Try it, and [say what tripped you up](https://github.com/august-byrne/Phazite/issues).

## Get Phazite

Phazite runs on Windows 11 x64. The [Download page](https://august-byrne.github.io/phazite-site/download/) has the installer once there is a release; until then, you build it yourself: the README's [Try it](../../README.md#try-it) has the steps, and [Building Phazite](../building.md) explains each one, with fixes for when something goes wrong.

## The first start

The Welcome window opens the first time Phazite starts. Everything in it is optional, a page at a time:

1. **Coming from...?** FL Studio's or Ableton Live's shortcuts, if you know one of them, or Phazite's own. **Keymap**, in the menu of the gear at the bottom left, changes it later.
2. **Your audio output:** where Phazite plays, with a test sound to check that you hear it.
3. **Phazite Sounds:** 85 free drums, loops and effects, in the public domain (CC0), downloaded only if you ask.
4. **Where to start:** an example song or a lesson.

**Help > Welcome** brings it back at any time.

## Play an example song

**Help > Example Songs** opens a finished song as a new project, to play, take apart and save as your own. Press **Space** to play and stop.

- **House Groove:** house at 124 BPM with a breakdown and two drops, a bass and chords that pump under the kick, risers, and sends to a reverb and a delay.
- **Lo-fi Beat:** a lazy, swung kit at 82 BPM with vinyl and rain, ninth chords on a wobbling e-piano, a walking bass and a dub delay.
- **Exodus:** dark synthwave at 114 BPM, Exodus by XCALIBUR (CC BY 4.0) on presets made after its original sounds: a rolling bass over a sub, an arpeggio in four layers, two buses that pump under the kick, and its tempo speeding up and slowing down in the tempo lane.

## Take a lesson

**Help > Lessons** walks you through a task one step at a time, with a ring around the control each step is about. Each starts from an empty song:

- **Make a Beat:** a drum sampler, drum hits clicked into the piano roll, played and repeated.
- **Record Audio:** a voice or an instrument through your audio interface, with the metronome, takes and punching in.
- **Arrange a Song:** a song laid out in sections, one repeated with everything in it, and the song exported.
- **Add Effects:** an effect on a track in the mixer, and how one of its knobs changes over the song.
- **Write Chords:** a chord progression on the song's chord lane, and a part that plays it.

## Keys to know

| Key | What it does |
|---|---|
| **Space** | Plays and stops. |
| **M** (**Ctrl+T** with FL Studio's shortcuts) | Turns the computer keyboard into a piano: A S D F G H J K L ; for the white keys, W E T Y U O P for the black ones. A MIDI keyboard plays as soon as it is plugged in. |
| **Ctrl+K** | Finds any command by name, and shows its keys. |
| **Ctrl+S** | Saves. The first time, Phazite asks for a name and suggests `Documents\Phazite\Projects`; from then on, Auto Save keeps the project saved. |
| **Ctrl+Alt+S** (**Ctrl+N** with FL Studio's shortcuts) | Commits the song in Source Control, the branch icon down the left, with a message. |

The bar along the bottom of the window says what the control under the pointer does. **Keyboard Shortcuts**, in the settings, changes the keys of any command.

## Plug-ins and audio

**Scan for Plug-ins**, in the menu of the gear at the bottom left, finds your VST®3 plug-ins. The scan runs in a helper process, so a plug-in that crashes or hangs can't take Phazite down, and each plug-in then plays in a process of its own.

If you hear nothing, choose your output in **Audio Settings**, in the same menu. [Troubleshooting](troubleshooting.md) has more.

## Finish a song

**File > Export Song** renders the song to a WAV or FLAC file, or each of its tracks to a file of its own (stems), the song normalized to a loudness if you like.

VST is a registered trademark of Steinberg Media Technologies GmbH.
