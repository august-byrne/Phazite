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
  Backups/         earlier saves of the project file, kept by age, and the autosave
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
its tags), but on a `CLIP`, a `WARP`, a `NOTE` or a `CONTROL`: a song's clips, their warp markers and a clip's notes and
controllers' values are values in Phazite (each with the properties this page names), so a property or an element it
doesn't know on one of them isn't kept, nor a property of the other kind of clip's (a `length` on an audio clip, a
`file` on a MIDI clip). Phazite writes a clip's properties only while they are set, so one another program wrote at its
default (`offset="0"`, `name=""`) is gone at the next save. A `CURVE` holds its `POINT`s alone: an element of another
type in one goes as the song opens. An element it does know, where `Schema.h` doesn't put it, it leaves
out as it opens the file (section 7).

## 3. The song

`PHAZITE` holds, among others (Schema.h names every one):
- `TEMPOMAP`: the tempos (`TEMPO`: `beat`, `bpm`, and `curve`, written only while it isn't 1, a step) and time
  signatures (`TIMESIGNATURE`: `beat`, `numerator`, `denominator`), each from its beat. Read in any order, each kind
  by beat, one of a kind for a beat (a tempo's or a time signature's key is its beat: of two at one beat the first
  stays, as "References" below says; the open song can come to hold two at one beat, known bug 240 in
  [known-bugs.md](known-bugs.md)); a number that is absent or none reads as its description says (a
  `bpm` of 120, a `numerator` and a `denominator` of 4, a `beat` of 0, a `curve` of 1), a kind with none holds one
  from the song's start, and a property or an element Phazite doesn't know on the map or an entry isn't kept. Phazite
  writes the tempos first, then the time signatures, each kind by beat;
- `TRACKS`: the tracks top to bottom, a folder's tracks inside it, each with its clips (first among its elements, in
  the track's order of them), its devices, its sends and its channel; and `MASTER`, the master's channel and devices;
- `AUTOMATION`: the curves, each a parameter's by its device's ID and the parameter's ID, with its points in beats,
  each at a position along the parameter's control's travel (`value`, 0 to 1, as its knob or fader moves along its
  scale: for a frequency, logarithmic, minimum × (maximum / minimum)^position, so 0.5 on a 20 Hz to 20 kHz cutoff is
  632 Hz; for another skewed parameter, skewed so that its midpoint is at 0.5), so that a straight line between two
  points is an even turn of the knob; a plug-in's parameters are from 0 to 1 already, and a pan's centre is 0.5; one
  curve of a parameter, the first kept as the song opens, and a property or an element Phazite doesn't know on
  `AUTOMATION`, a curve or a point isn't kept. Written in the order the song made the curves (a curve made goes after
  the others), each point's `beat` and `value`, and its `curve` only while it isn't 0; the element is written only while
  the song has a curve (a song whose last curve went is written without one);
- `MIDILINKS`: the MIDI links (MIDI learn), each a `MIDILINK` with a hardware controller, by its `channel` (1 to 16)
  and `number` (0 to 119; a link with another goes as the song opens), and the parameter it turns, by its `device`'s ID
  and its `parameter`'s ID as a curve names them (a modulator's parameter by its name; a macro by its ID as `device` and
  the word `value` as `parameter`, so that the link names the macro's ID once); one link of a controller and one of a
  parameter, the first kept as the song opens; a `channel` or a `number` written as text reads as the loader reads a
  number, made whole, and a property or an element Phazite doesn't know on `MIDILINKS` or a link isn't kept. Written
  in the song's order (a link made goes after the others), each link's four properties, its `device`'s ID as text; the
  element is written once the first link is made, and stays, empty, once the last goes;
- `SECTIONS`: the song's parts (`SECTION`: `id`, `start`, `length`, `name` and `energy`, always written, and `role`, `tip`
  and `colour` each written only where the section has one, a `colour` written empty kept so). Read in any order (the
  sections' readers sort them by their starts, two at one start in the file's order), each ID once (a second section of
  one ID, and one without an ID the song can count on from, is kept and given a new ID as the song opens, as a track or
  a clip would be: section 7, "IDs"); a number that is absent or none reads as its description says (a `start` of 0, a
  `length` of 4, an `energy` of 0.5), a number written as text reads as the loader reads a number, and a property or an
  element Phazite doesn't know on `SECTIONS` or a section isn't kept. Written in the song's order (a section made goes
  after the others); the element is written once the first section is made, and stays, empty, once the last goes;
- `CHORDLANE`, once the song has a chord lane (without one, its keys are the key set last, `LASTKEY`): its chords
  (`CHORD`: `start`, `length`, `degree`, `type`, and `accidental` and `inversion`, each written only while it isn't 0)
  and the keys they are in (`CHORDKEY`: `start`, `keyRoot`, `keyScale`). Read in any order (the lane's readers sort each
  kind by its start), one of a kind for a start (a chord's or a key's key is its start); a number that is absent or none
  reads as its description says (a `start` of 0, a `length` of 4, a `degree` of 1, an `accidental` and an `inversion` of
  0; a key without a `keyRoot` is none), and a property or an element Phazite doesn't know on the lane or an entry isn't
  kept. An empty `CHORDLANE` is a lane still. Phazite writes the chords first, then the keys, each kind in the lane's
  order;
- `MERGING`, while a merge taken into the song waits for its version: what it merged, where, and the song before it
  (a piece in the project's Versions folder), as [versions-format.md](versions-format.md) says (section 8). It goes in
  and out of the song with the merge's undo step, is no part of a version's song, and is read as untrusted: one that
  doesn't hold together waits for nothing.

**Where the song stands in its versions.** `versionsBranch` on `PHAZITE` is the id of the branch of the project's
versions the song is on (32 lowercase hexadecimal digits), and `versionsBase` the id of the version it grew from (64
lowercase hexadecimal digits), which its changes are worked out against; neither before the first version. They go with
the song into its file, its autosave, its backups and the song left on a branch, so that whichever of them opens says
where it stands; a version's own song holds neither. They are no change to the song (an uncounted property): Phazite moves
them without an undo step as a version is saved, a branch made or switched to, Undo Last Version or Reset Branch to Last
Readable Version run, and writes them into the project file at once while the song has no other unsaved change (else with
the next save and autosave); only Take the Newer Versions moves them in an undo step. Text of another form is left out as
the file opens. Where they name a branch or a version the Versions folder hasn't, or a version that can't be read, the song
takes the branch the folder's `current` file names and its last version ([versions-format.md](versions-format.md)).

**Time** is in beats from the song's start, as doubles, a beat being a quarter note whatever the time signature, as
MIDI files and DAWproject count it, so that the song keeps its notes on their beats when its tempo changes. A tempo
(`bpm` on `TEMPO`) counts quarter notes a minute, and a time signature (`TIMESIGNATURE`) only makes its bars
`numerator` x 4 / `denominator` beats long (3 in 6/8, 3.5 in 7/8). An audio clip's part of its file is in seconds of
the file.

**MIDI channels.** A MIDI clip's `NOTE` and `CONTROL` (a controller event: a controller, the pitch wheel, pressure or a
program change) each play on a MIDI channel, `channel`, from 1 to 16 as musicians count them; one without it is on
channel 1, which Phazite never writes. A `CONTROL` of kind `programChange` plays: its `value` is the program, 0 to 127.
Phazite writes a clip's `NOTE` elements, then its `CONTROL` elements, after its warp markers, each kind in
its order (a note's place among the notes means nothing; the controllers' values keep theirs, which is the order they
play in at one beat); a reader takes them in any order.

**IDs.** Every track, clip, note (within its clip), device, container, chain, send, modulator, macro and section has an
ID, a positive 64-bit number the song gives (`nextID` on `PHAZITE` is the next), never twice. What one element says of
another names it by its ID. Copies get new IDs; the rules of which properties hold or name an ID are data, in
[`tests/golden/format/song-rules.json`](../tests/golden/format/song-rules.json). `idNumbering` on `PHAZITE` says which
numbering the IDs are in: none until Phazite gives a song's IDs anew as it opens (section 7, "IDs"), then a stamp of 16
lowercase hexadecimal digits that Phazite makes each time it does, the first 16 of the SHA-256 of the song's project text
as it was before. It is part of the song, so a version's song holds it too. Two songs with different stamps (none being
one of them) are in two numberings: their IDs below 2^61 are as they were, but those from 2^61 up were given anew in one
of them (or differently in each), so an ID can't be relied on to name the same item in both, and Phazite matches the items
of two songs by ID only where their stamps are the same ([versions-format.md](versions-format.md), section 5).

**View state and how the song plays** (track heights, the grid, where a device's window was; the loop, the metronome,
where playback starts), and where it stands in its versions, are saved, but are no change to the song: the rules list them
as `uncountedProperties` and `uncountedElements`.

**The description.** Every element and property a song holds is described once in Phazite's code
(`src/model/base/internal/Schema.h`), and
[`tests/golden/format/song-schema.json`](../tests/golden/format/song-schema.json) is that description as data, the test
vector for the song's names, types and ranges: `elements`, each with its `name`, the elements it goes `in` (none for
`PHAZITE`), its `kind` (`song`, `view` or `state`) and whether it is `counted` as a change with everything in it; and
`properties`, each with its `name`, the elements it is `on`, its `type` (`bool`, `int`, `int64`, `float`, `double`, `text`
or `bytes`, in base 64), its `form` where it is more than its type (`ownID`, an item's own ID; `songID`, another ID of the
song's it holds; `idName`, the ID of another item it names, an `int64` or, as `text`, its decimal digits (a lane's, a
curve's, a modulation's and a MIDI link's `device`: `"1042"`); `words`; `hexID`, so many lowercase hexadecimal `digits`;
`colour`; `numbers`, separated by spaces), its `range` (`low`, `high`, `whole`, what a value that is no number becomes,
whether one outside goes rather than being brought in, and `powersOfTwo`), its `words`, its `default` where it has one,
its `kind`, and whether it is `counted` (or the elements it is `uncountedOn`). A family of names made in a loop (a drum
pad's sounds) is one entry, named by its pattern (`pad<note>Sound<layer>`), with the count of its `names`. A file whose
values keep to it opens as it is; one outside it opens kept to it (section 7). Elements sorted by name, properties by name
and elements.

## 4. Devices

A track's devices are `DEVICE` elements in the order the sound goes through them, an instrument first, with containers
(`CONTAINER`, each with its `CHAIN`s) among them.

**Phazite's own devices** have a `type` ("phaziteSynth"), and their values set by hand in a `VALUES` child, a property
for each parameter by its ID; a parameter at its default isn't written. A number is its value; **a choice is written by
its name** ("Low Pass", "1/8D"), so that what it means doesn't hang on the choices' order.
[`tests/golden/format/devices.json`](../tests/golden/format/devices.json) lists every device's parameters, their ranges,
their knobs' scales (`scale` "logarithmic", or a `midpoint` halfway along), defaults and choices' names. A device's
sounds and files are in a `SOUNDS` child.

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
  Phazite hasn't, is left out with all it holds (a clip anywhere but on a track: a folder or a return holds none); an element of a type Phazite doesn't know stays where it is, untouched, but in a `CURVE`, which holds its points alone; a property that should be one of a set of words
  (a track's kind, a clip's) and isn't is left out, a track without a kind becoming a track or a folder; text between
  tags is dropped.
- **Numbers:** each number `Schema.h` gives a range is brought within it, made whole where it is an integer, and one
  that is no number at all takes its default (a tempo of 0 opens at the slowest tempo, one that is no number at 120
  BPM). A channel's volume and a send's level are held to -100 to +6 dB, places to a million beats, every value set by
  hand to its parameter's range and every automation point to 0..1 (one that is none or no number is left out,
  and a curve that had points and is left without goes), and a value or a curve of a parameter its device hasn't is left out; a step
  sequencer's steps are held to -1..1 (one that is no finite number becomes 0) and a sampler's slices to 0..1 (one that
  is none is left out), each in place, the others as they are written; `versionsBranch`, `versionsBase` and
  `idNumbering` that aren't ids of their form are left out. First of all, before any number is read, a
  number whose exponent has ten digits or more ("1e12345678901", which JUCE's reader of numbers can't count) is taken as
  JUCE's reader takes it, past every number for an exponent above 0 ("0e12345678901" too, which that reader reads as
  infinity) and 0 for one below, and so as no finite number; a name that reads as one stays as it is.
- **Switches:** a switch written another way than Phazite writes it ("true", "false", "yes") is written as Phazite
  writes it, as it plays: "1", or "0" or none where Phazite leaves one that is off out; so that the versions, which
  compare text, see no change where there is none.
- **IDs:** an item without an ID, or with one another item holds, gets a new one (a track that does is no longer frozen,
  as its frozen file is found by its ID); IDs stay below 2^62, and `nextID` is set past every ID the song holds (an ID
  past 2^61 inside an element Phazite doesn't know, which stays as it is, isn't counted). A song whose IDs or `nextID`
  reach 3 * 2^60 gets, once, a new ID for every one at 2^61 or past it (an item's, a fader's, a VCA's, an alias group's,
  a note's, the same in each clip of its alias group), counted on from past every ID it holds or names below 2^61, and
  what named each names its new one (a name of none of them goes); a merge waiting in it goes; and the song is stamped
  with its new numbering (`idNumbering`, made from the song as it was, so that the same file gets the same IDs and the
  same stamp at every opening). Any other file's IDs and `nextID` stay as they are, so a song can never run out of IDs. A
  file with IDs just below 3 * 2^60 (another program's, or Phazite's after a session on one) opens as it is, and what that
  session made gets its new IDs at the next opening, once (`renumberMark` in `src/model/base/internal/Schema.h` says what
  that costs); the versions saved before then keep the IDs as they were, without the stamp, and Phazite restores no line
  from them and merges none of them. An ID written another way ("12.0", "012") is written as its whole number, as it is read (one that
  reads as no ID, such as "+12", which reads as 0, gets a new one, and what names it is dropped).
- **References:** a lane, a curve or a modulation of a device the song hasn't, and a send to a return it hasn't, is left
  out, and a sidechain or a pad's
  track that names a device the song hasn't is cleared, and so is a sidechain the model would refuse: a device that
  would hear its own track's sound round a loop (on a drum sampler's track, listening to one of its own pads' tracks, say).
  Of two elements of one parent with one key (two curves of one
  parameter, two tempos on one beat), the first stays.
- **Alias groups:** the clips of a group (`link`) hold the same notes and controllers' values, and its audio clips play
  one file alike: they share their `file` (the path as written, compared as text, whether the file is there or not),
  `tempo`, `pitch`, `reversed` and `WARP` markers. Each keeps its own place, the part of the file it plays (`fileStart`,
  `fileEnd`), `gain`, fades, `mute` and `take`. Renaming a clip writes the same `name` into every clip of its group,
  but a clip on a track of that name holds none, as a clip without a `name` is named after its track; the loader
  doesn't compare names. A clip of another kind than the group's first clip in the song's order, or whose `NOTE` and
  `CONTROL` elements differ from that clip's (in a property, in number or in order), or an audio clip whose `file`,
  `tempo`, `pitch` or `reversed` (each as read, its default where it has none: no tempo, 0, forwards) or `WARP` elements
  (in a property, in number or in order) differ from that clip's, is made a clip of its own: its `link` is left out and
  what it holds stays. A group left with one clip, its only clip loses its `link`.
- **Sound files:** a sound file named on a network path, or by a path that climbs out of the project's folder with
  `..`, is named `Audio/<its name>` instead, its name unchanged, so it is never looked for there; the Project view lists
  it as missing until it is located. A full path on this computer stays as it is. An audio clip that names no file is
  left out.

A preset is untrusted too: its volumes are held to the same range, and a sound it names on a network path, or one that
isn't an audio file, is refused without being opened. A DAWproject's `project.xml` and `metadata.xml` are read only up
to 256 MB each, and what it unpacks only up to what an archive of its size can hold; and a DAWproject Phazite writes is
refused before it is written, and before its bounces are rendered, where it would hold 4 GB or more, or more than 65,534
files, which the zip writer can't hold (and 65,535 a reader takes for ZIP64's marker). The rules are stated a second
time, apart from the loader, in [`tests/SongCheck.h`](../tests/SongCheck.h), and the readers are fuzzed in every test
run ([`tests/FuzzTests.cpp`](../tests/FuzzTests.cpp)).
