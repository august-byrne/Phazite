---
title: Recording
description: Arming tracks, inputs and hearing them, the count-in and metronome, takes, punching in, and Capture.
sidebar:
  order: 8
---

Recording puts what you play into the song: the notes you play on a MIDI keyboard or the computer keyboard, and the
sound that comes into your audio interface's inputs. **Capture** keeps the notes you just played even when you didn't
press Record. **Help > Lessons > Record Audio** walks you through a first recording.

## Before you start

- **Audio Settings** (in the menu of the gear at the bottom left, or a click on the audio output in the status bar)
  chooses the audio device, its sample rate and its buffer size. On Windows, an audio interface usually plays with
  the least delay through its ASIO® driver.
- **MIDI Settings** turns MIDI inputs on or off. A MIDI keyboard plays as soon as it is plugged in.
- **Low latency**, on the transport bar, takes away the delay you hear while you play: devices that look ahead are
  bypassed, delay compensation is off and the buffer shrinks. Turn it off again to mix.

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

## Arming tracks

Every track that records has an arm switch, beside its mute and solo switch, in its playlist header and its mixer
strip. Folders and returns have none.

**A track with an instrument records MIDI.** What you play on MIDI keyboards and the computer keyboard plays one track:
the selected track with an instrument, or else the first track with one. Its arm switch lights by itself, so you never
need to arm it first. A click on another instrument track's switch sends live MIDI there.

**A track without an instrument records an audio input.** A click on its arm switch arms or disarms it. On a track
with no input yet, the click opens the input list first. A right-click offers:

- **Input:** each of the audio device's inputs alone ("In 1"), each pair ("In 1+2"), and **No input**;
- **Hear the input:** **While armed** (the default), **Always**, or **Never (the audio interface plays it to you)**,
  for an interface that sends its input to your headphones itself;
- **Arm Only This Track**, which disarms every other track.

While a track is armed, what it records shows under its name: "In 1", "In 1+2" or "MIDI", and "Recording In 1" while it
records. Arming is not saved: a project always opens with nothing armed, so a microphone can't feed back as it opens.
**Arm Track** arms the selected tracks.

## Record

**Record** (**R**) records what you play on MIDI onto the track live MIDI plays, and the inputs of the armed tracks.
It starts from where playback starts, after the count-in if that is on. Its tooltip says what it records onto. The
whole recording is one undo step.

The arrow beside Record (or a right-click on it) opens its options:

| Option | Does |
|---|---|
| Count-In | Plays a bar of clicks before recording starts from a stop, even with the metronome off |
| Punch In | Keeps only what is played in the looped range ([below](#punching-in)) |
| Overdub | Adds the notes you record to the clip already there; off, they take its place. On at first |
| Quantize Recordings | Moves the notes you record to the nearest sixteenth as they go in |
| Record an Input on a New Track | Lists the inputs; choosing one adds an armed track called "Audio" with that input |

With nothing armed to record onto, Record's menu says "Nothing records yet: arm a track, or record an input on a new
track" and offers **Record an Input on a New Track**.

While it records, a clip grows on each track that records, with the input's waveform or the notes drawn as they come
in. Audio is written as a WAV file into the project's Audio folder, named after the track ("Vocals Take 1.wav"), and it
lands in the song where you played it. If the audio device's driver reports its delays wrongly, **Correction**, under
**Recording** in the audio settings, moves recordings earlier or later by a number of milliseconds.

If you move the playhead while recording, recording goes on from the new place, and what was recorded so far is kept.

## The metronome and the count-in

**Metronome** (**O**) clicks on each of the time signature's beats while the song plays, louder on each bar's first.
The arrow beside it (or a right-click) has **Count-In** too. The song remembers whether its metronome is on.

## Takes

Loop a range of time ([The playlist](playlist.md#the-loop-and-the-playhead)) and record round it: you get one clip over
the range with a take for each time round.

- The last whole take plays. A take cut short by Stop is kept too.
- What you played before the range, if you started earlier, becomes a clip of its own.
- The clip's tag says which take it plays: "Take 2/3".
- A right-click on the clip has **Take**, which lists the takes, with **Previous Take** and **Next Take**. Choosing one
  is one undo step.

## Punching in

With **Punch In** on, Record keeps only what you play inside the looped range, in place of what was there, and
crossfades its edges. The loop shows red in the rulers while Punch In is on. While the loop covers the whole song
there is nothing to punch into, so loop a range first.

## Capture

Phazite keeps the last 30 seconds of what you played on MIDI keyboards and the computer keyboard, recording or not.
**Capture** (**Ctrl+Shift+C**, or its button on the transport bar) turns them into a clip on the track live MIDI plays,
and selects it.

- While the song plays, the notes stay where you played them in it.
- While it is stopped, the clip goes at the bar where playback starts, with the first note on that bar's first beat.
- In a song without clips, Capture also sets the song's tempo from what you played, when it can tell.
- With **Quantize Recordings** on, the captured notes are quantized to sixteenths.

## Not yet

- Take lanes and comping, to pick the best parts of several takes: planned.
- Capture of audio: planned.
- An input meter in the input list: planned.
- Several MIDI tracks armed at once: planned.
- Hearing the input only inside the punch range: planned.
- A metronome grouped in dotted quarters for 6/8 and other compound meters: planned.
