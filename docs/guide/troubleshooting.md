---
title: Troubleshooting
description: What to do when there's no sound, notes come out late, a plug-in is missing or crashed, or Phazite closed unexpectedly.
sidebar:
  order: 2
---

## No sound

- Open **Audio Settings**, in the menu of the gear at the bottom left, choose your output and play its test sound.
- The status bar, along the bottom of the window, names the output and says how late its sound comes out, as its driver reports it ("48 kHz · 10 ms").

## Notes come out late

- **Bluetooth** headphones and speakers add about 150 to 250 ms between a key and its sound, which no driver reports: fine for listening, too slow for playing. Phazite says so in Audio Settings and in the status bar. Plug in wired headphones or an audio interface to play in time.
- With an audio interface, choose its ASIO® driver in Audio Settings: it comes out sooner than Windows' own.
- **Low Latency**, on the transport bar, takes away what adds a delay while you play: devices that look ahead are bypassed, delay compensation is off and the buffer shrinks. Turn it off again to mix.

## A plug-in is missing

Run **Scan for Plug-ins**, in the menu of the gear at the bottom left, after installing a plug-in. Phazite plays VST®3 plug-ins: a plug-in that comes only as VST2 doesn't show up.

## A plug-in crashed

Each plug-in plays in a process of its own. One that crashes or hangs says so in its slot, with **Reload**, and the song plays on. The Project view, down the left, lists the project's plug-ins, with a crashed one to reload.

## Part of a recording was lost

Phazite writes what you record as it comes in, with 8 seconds to spare. If the drive stalls for longer, the sound in between is lost: the take has silence there, so that everything after it stays in time, and Phazite says which track, where and how long. Record that part again with Punch In, and keep projects on a fast internal drive.

## Phazite closed unexpectedly

When Phazite starts after it didn't close properly, it says which project was open and asks what to do:

- **Open** it as it was.
- **Open with Plug-ins Off**, if a plug-in in it may have crashed Phazite: they stay as they were, silent, until the project opens again.
- **Start a New Project**.

If Auto Save had kept changes you hadn't saved yourself, Phazite asks whether to restore them.

## Reporting a bug

[Open an issue on GitHub](https://github.com/august-byrne/Phazite/issues) with what you did, what you expected, and what happened instead. **Help > About Phazite** has **Copy Version**, which copies Phazite's version and your system's name for the report.

VST is a registered trademark of Steinberg Media Technologies GmbH. ASIO is a registered trademark of Steinberg Media Technologies GmbH.
