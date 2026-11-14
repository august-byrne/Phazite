# The project format

*How a Phazite project is kept on disk, for anyone writing a tool that reads or writes one: its folder, its `.phazite`
file and what the song in it holds. Written 2026-11-09, from the [format audit](archive/format-audit.md). The song's elements and
properties are described one by one, with their types and ranges, in
[`src/model/base/internal/Schema.h`](../src/model/base/internal/Schema.h), the one place Phazite names them; the
history beside the project is [versions-format.md](versions-format.md). Where this file and the code disagree, the code is
right and this file has a bug.*

This text is under the AGPL, as all of Phazite is. Implementing the format it describes, in software of any license, open
or closed, needs no permission: copyright covers the text, not the format.

Phazite is pre-alpha, and the format has no version yet: an update may change it, until the owner says compatibility
starts ([design.md, section 18](design.md#18-projects-saving-and-export)).

## 1. The folder

```
<name>/
  <name>.phazite   the song (section 2)
  Audio/           recordings and imported audio, copied in as they come: written once, never changed
  Generated/       sounds Phazite made
  Renders/         exports
  Backups/         earlier saves of the project file (the newest ten), and the autosave
  Cache/           what Phazite can make again: frozen tracks, stretched clips' renders; safe to delete
  Versions/        the project's history (versions-format.md)
```

The song names its files by paths relative to the project's folder, with `/` between their parts. A file the song
couldn't copy in keeps its full path. A project is portable as its folder: copy or zip the folder.

## 2. The file

The `.phazite` file is the song alone, as UTF-8 XML, with lines ending in a line feed:
- `<?xml version="1.0" encoding="UTF-8"?>`, an empty line, then the song's element, `PHAZITE`;
- each element on a line of its own, indented two spaces a level;
- each property on a line of its own, in the order of their names (byte order of their UTF-8), the first beside the
  element's type and the others under it, so that a tool that compares text (git, say) shows a change as the lines it
  changed;
- `/>` for an element with no children.

Values are written as the versions' pieces write them ([versions-format.md, section 4](versions-format.md#4-pieces)):
whole numbers in their digits without `.0`, a float's value in the fewest digits that read back as that float, other
numbers in 15 significant digits, true and false as `1` and `0`, text without the characters XML 1.0 can't hold, and `&`
`<` `>` `"` tab, line feed and carriage return escaped. Every other character is written as it is, in UTF-8.

A reader must take any XML 1.0 (other writers may indent differently) and must keep what it doesn't know: Phazite keeps
an element or property it doesn't know through a load and a save (an element with all it holds, but not text between
its tags). An element it does know, where `Schema.h` doesn't put it, it leaves out as it opens the file (section 7).

## 3. The song

`PHAZITE` holds, among others (Schema.h names every one):
- `TEMPOMAP`: the tempos and time signatures, each from its beat;
- `TRACKS`: the tracks top to bottom, a folder's tracks inside it, each with its clips, its devices, its sends and its
  channel; and `MASTER`, the master's channel and devices;
- `AUTOMATION`: the curves, each a parameter's by its device's ID and the parameter's ID;
- `SECTIONS`, `CHORDLANE`: the song's parts and its chords.

**Time** is in beats from the song's start, as doubles, a beat being a quarter note whatever the time signature, as
MIDI files and DAWproject count it, so that the song keeps its notes on their beats when its tempo changes. A tempo
(`bpm` on `TEMPO`) counts quarter notes a minute, and a time signature (`TIMESIGNATURE`) only makes its bars
`numerator` x 4 / `denominator` beats long (3 in 6/8, 3.5 in 7/8). An audio clip's part of its file is in seconds of
the file.

**IDs.** Every track, clip, note (within its clip), device, container, chain, send, modulator, macro and section has an
ID, a positive 64-bit number the song gives (`nextID` on `PHAZITE` is the next), never twice. What one element says of
another names it by its ID. Copies get new IDs; the rules of which properties hold or name an ID are data, in
[`tests/golden/format/song-rules.json`](../tests/golden/format/song-rules.json).

**View state and how the song plays** (track heights, the grid, where a device's window was; the loop, the metronome,
where playback starts) are saved, but are no change to the song: the rules list them as `uncountedProperties` and
`uncountedElements`.

## 4. Devices

A track's devices are `DEVICE` elements in the order the sound goes through them, an instrument first, with containers
(`CONTAINER`, each with its `CHAIN`s) among them.

**Phazite's own devices** have a `type` ("phaziteSynth"), and their values set by hand in a `VALUES` child, a property
for each parameter by its ID; a parameter at its default isn't written. A number is its value; **a choice is written by
its name** ("Low Pass", "1/8D"), so that what it means doesn't hang on the choices' order.
[`tests/golden/format/devices.json`](../tests/golden/format/devices.json) lists every device's parameters, their ranges,
defaults and choices' names. A device's sounds and files are in a `SOUNDS` child.

**A plug-in** is a `DEVICE` of `type` `plugin`, naming the plug-in by its `format` ("VST3"), `name`, `manufacturer`,
`identifier` (its file on the computer it was added on) and `uid` (JUCE's number for it, which for a VST3 is made from
its class ID); `formatID`, its ID in its format (a VST3's class ID, 32 hexadecimal digits); and `version`, the version of
the plug-in that made its state. Phazite finds the plug-in by its format, `uid` and maker first, wherever this computer
keeps it, then by its file and name, then by its name, maker and format. Its `state` is the plug-in's own, in standard
base 64 (RFC 4648): for a VST3, **its preset file**, a `.vstpreset` as the VST3 SDK writes one ("VST3", the version, the
class ID, the processor's and the controller's chunks and their list), which any VST3 host reads and which DAWproject
holds too. Its `VALUE` children are the values set by hand of the parameters Phazite moves, by the plug-in's own
parameter IDs.

## 5. Presets

A preset (`.phazitepreset`) is JSON of its own, made to outlive changes inside the song: a device by its type, every
parameter by its ID (a choice by its name), a plug-in by its identity with its state as above, and containers, modulators
and macros as Phazite names them. Phazite makes new IDs as it loads one.

## 6. Other DAWs

DAWproject is how a project goes to and from other DAWs ([versions.md](archive/versions-plan.md#dawproject-with-versions)): what they
can't hold (Phazite's own devices, containers, modulators, the chord lane) goes as bounces beside the notes. A VST3's
state goes into a DAWproject as it is, since both hold its preset file.

## 7. A file that breaks the rules

Phazite opens projects that others made, so it takes every project file as untrusted, its own included. A file that
isn't XML, or whose element isn't `PHAZITE`, is refused with a reason. Any other is made consistent as it opens
(`makeConsistent()`, in [`src/model/project/Project.cpp`](../src/model/project/Project.cpp)), with no undo step; a
file Phazite wrote is consistent already and comes through unchanged, and so does what Phazite doesn't know:
- **Places and kinds:** an element Phazite knows, where `Schema.h` doesn't put it, or a device or modulator of a type
  Phazite hasn't, is left out with all it holds (a clip anywhere but on a track: a folder or a return holds none); an element of a type Phazite doesn't know stays where it is, untouched; a property that should be one of a set of words
  (a track's kind, a clip's) and isn't is left out, a track without a kind becoming a track or a folder; text between
  tags is dropped.
- **Numbers:** each number `Schema.h` gives a range is brought within it, made whole where it is an integer, and one
  that is no number at all takes its default (a tempo of 0 opens at the slowest tempo, one that is no number at 120
  BPM). A channel's volume and a send's level are held to -100 to +6 dB, places to a million beats, every value set by
  hand and every automation point to its parameter's range, and a parameter its device hasn't is left out.
- **IDs:** an item without an ID, or with one another item holds, gets a new one; IDs stay below 2^62, and `nextID`
  is set past every ID the song holds.
- **References:** a lane, a curve or a modulation of a device the song hasn't, and a send to a return it hasn't, is left
  out, and a sidechain or a pad's
  track that names a device the song hasn't is cleared. Of two elements of one parent with one key (two curves of one
  parameter, two tempos on one beat), the first stays.
- **Sound files:** a sound file named on a network path, or by a path that climbs out of the project's folder with
  `..`, is named `Audio/<its name>` instead, its name unchanged, so it is never looked for there; the Project view lists
  it as missing until it is located. A full path on this computer stays as it is. An audio clip that names no file is
  left out.

A preset is untrusted too: its volumes are held to the same range, and a sound it names on a network path, or one that
isn't an audio file, is refused without being opened. A DAWproject's `project.xml` and `metadata.xml` are read only up
to 256 MB each, and what it unpacks only up to what an archive of its size can hold. The rules are stated a second
time, apart from the loader, in [`tests/SongCheck.h`](../tests/SongCheck.h), and the readers are fuzzed in every test
run ([`tests/FuzzTests.cpp`](../tests/FuzzTests.cpp)).
