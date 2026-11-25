# The versions format

*How Phazite keeps a project's history, for anyone writing a tool that reads or writes it: a project's `Versions` folder,
its packs of pieces, its versions and branches, and how two songs are compared and merged. Written 2026-11-09, from the
[format audit](archive/format-audit.md). The code is `src/model/versions/`; where this file and the code disagree, the code is
right and this file has a bug.*

This text is under the AGPL, as all of Phazite is. Implementing the format it describes, in software of any license, open
or closed, needs no permission: copyright covers the text, not the format.

Phazite is pre-alpha, and the format has no version yet: an update may change it, until the owner says compatibility
starts ([design.md, section 18](design.md#18-projects-saving-and-export)). Then a file naming the format and its version
comes first ([format audit, P5](archive/format-audit.md#34-portability-as-a-concept)).

## 1. The idea

A history is a store of **pieces**, each named by the SHA-256 of what it holds, written once and never changed, as git
keeps its objects. A song is a tree of elements (XML); its bigger parts (tracks, clips, devices, curves) are pieces of
their own, so a version that changed one note stores that note's run, its clip's piece, its track's and the song's: a few
kilobytes. A **version** is a piece too, naming its song's piece and the versions it follows. A **branch** names its last
version, in a small file each computer writes for itself.

What a song means (which children are ordered, what an element's key is, which properties hold IDs, what isn't a
change) is data, in [`tests/golden/format/song-rules.json`](../tests/golden/format/song-rules.json), so that a tool can
compare and merge songs as Phazite does without Phazite's code. Another kind of document could use the same store with
rules of its own. Phazite writes a song as a project file holds it ([project-format.md](project-format.md), section 3):
the song's element's children in the one order its description gives them, and no empty `MIDILINKS` or `SECTIONS` (a
song without links or sections holds none; an empty one a version holds is read as none).

## 2. The folder

```
Versions/
  pieces/<name>.pack                 pieces, in packs (section 3)
  branches/<branch>/<computer>.xml   a branch as one computer last had it (section 6); only that computer writes it
  messages/<computer>.xml            versions' messages as one computer edited them (section 6); only it writes it
  current                            the id of the branch the project file is on, where its song doesn't say (one line, no end of line needed)
  local/<computer>/files.xml         this computer's cache of its audio files' hashes (section 8)
  local/<computer>/working.xml       the songs this computer left on branches when it switched away (section 6)
  local/<computer>/logs/<branch>     each move of a branch's last version on this computer (section 6)
  local/<computer>/history           this computer's Local History: the song at times, without versions (section 6)
  local/<computer>/swept             when this computer last swept its packs, and what of it is still to delete (section 7)
  local/<computer>/stamped           the newest time this computer stamped anything with (section 7)
  bounces/<version>.flac             a version's bounce, if it has one: its song rendered, FLAC, 16 bits
```

`<branch>` is the branch's id and `<computer>` the computer's: each 32 lowercase hexadecimal digits (a UUID without its
dashes). A computer's id is made once, at random, and never names the user or the machine.

**Who writes what.** Pieces and bounces are written once and never changed. Each computer writes only its own files: its
record of each branch, its file of edited messages, everything under `local/<computer>/`, and its own packs (the pack's
writer, section 3), which only it deletes, as a sweep joins them (section 7). So a folder that Dropbox, OneDrive or a NAS
syncs between computers never has two writers on one file, but `current`, which goes with the project file, as the
project file does.

**Writing safely.** Every file is written whole to a temporary name, flushed to the disk, then renamed into place; a log
is appended to and flushed. A reader ignores what it doesn't know: files, elements and properties.

## 3. Packs

A pack holds pieces and an index of them, and is named by the SHA-256 of its index followed by its writer's 16 bytes
(below), in lowercase hexadecimal: `<SHA-256 of index and writer>.pack`. Numbers are little-endian.

| Part | Bytes | |
|---|---|---|
| mark | 4 | `PZPK` |
| pieces | ... | each compressed by zlib (RFC 1950: zlib's header and checksum, not gzip's), one after another |
| index | 56 per piece | its name (32 bytes, the SHA-256 itself), where it starts in the file (8), its size compressed (4) and not (4), and when it was written into the store (8, milliseconds since 1970) |
| count | 4 | how many pieces the index lists |
| index start | 8 | where the index starts in the file |
| writer | 16 | who wrote the pack: the first 16 bytes of the SHA-256 of the computer's id (its UTF-8) |
| mark | 4 | `PZPK` |

A reader:
- leaves out a pack whose marks aren't both there, whose index doesn't fit exactly between its start and the count, or
  whose index's and writer's SHA-256 isn't the pack's name;
- refuses a piece whose uncompressed size is more than zlib can make of its compressed bytes (1,032 times them, and a
  little), or that doesn't inflate to that size;
- checks each piece's SHA-256 against its name, and treats one that doesn't match as missing;
- takes a piece from the first pack that lists it, when several do, and from the next one where that copy is damaged.

A reader reads a piece from any pack, whoever wrote it. A writer writes the pieces its own packs don't hold yet into a new
pack: a piece only another computer's pack holds is written again. Phazite forgets a copy it found damaged, so that its
next write that holds the piece writes it again. A pack of one writer holding the same pieces, written at the same
time, has the same name: one there already that doesn't read whole is written again in its place; two writers' packs
never share a name. Joining its own packs (section 7), a writer gives each piece the time of its newest own copy, reading it from any copy that checks out; a pack of its own that holds a damaged
copy goes once every piece it lists is kept in another pack, and stays while one isn't.

[`tools/read-versions.pl`](../tools/read-versions.pl) is a reader of packs in under 100 lines of Perl, written from this
section alone.

## 4. Pieces

A piece is one element of a song as UTF-8 XML, written in one way only (the same song always makes the same bytes, so
the same name):
- no XML declaration, no spaces between elements, no line ends;
- the element's type, then its properties, in the order of their names (byte order of their UTF-8), each
  `name="value"`;
- `/>` for an element with no children, else `>`, its children in their order, and `</type>`.

**Values.** Every value is text, written by these rules
([`src/model/base/internal/SongText.h`](../src/model/base/internal/SongText.h)):
- a whole number below 10^15 in its digits, without `.0`: `4`, `-12`;
- a number that is a 32-bit float's value in the fewest digits that read back as that float: `0.3`, not
  `0.300000011920929`;
- any other number in 15 significant digits, as C's `printf ("%.15g")` writes them: `0.333333333333333`, `1e-07`;
- true and false as `1` and `0`;
- text as it is, without the characters XML 1.0 can't hold (the control characters below 32 but tab, line feed and
  carriage return).

**Escaping.** `&` `<` `>` `"` as `&amp;` `&lt;` `&gt;` `&quot;`; tab, line feed and carriage return as `&#9;` `&#10;`
`&#13;`, so that they come back; everything else as it is, in UTF-8.

**The store's own elements** start with `_`, which no song's name does. A reader leaves out one it doesn't know.

| Element | Where | What |
|---|---|---|
| `<_piece _hash="H" _type="T" k="v".../>` | in the place of an element that is a piece of its own | the piece H holds that element, of type T; the properties its key is made of (section 5) are copied beside, so what is missing is known without reading it |
| `<_piece _hash="H" _type="_run"/>` | in the place of a run of an element's children | the piece H holds `<_run>` and the children, one after another, which go where the reference is |
| `<_bytes name="P" hash="H"/>` | first among an element's children | the element's property P is bytes, held by the piece H in them; its value is H's bytes in standard base 64 (RFC 4648, with `=`) |
| `<_parameters hash="H"/>` | after any `_bytes`, on a plug-in's `DEVICE` | the piece H holds `<_parameters>` with a `<_parameter id= name= value=/>` for each of the plug-in's parameters as the plug-in told them when the version was made, by the plug-in's own IDs, values from 0 to 1; not part of the song |

**How a writer splits a song** is its own choice; a reader only follows the references. Phazite makes pieces of its
tracks, the master, clips, devices, containers and automation curves (`piecesOfTheirOwn` in the rules); puts an
element's notes or points (`inRuns`) in runs once it has 128 of them, a run ending after an element whose text's FNV-1a
hash (64 bits) is a multiple of 64, so that a note changed or added changes its own run alone; and keeps a plug-in's
state (`bytes`) out of line when its base 64 reads back as the same text.

**Reading.** A song is joined from its own piece down, each reference replaced by what its piece holds. A reader that
can't read a piece either fails the whole song, or, when the user asks for as much as can be read (Phazite's Restore),
puts in its place a stand-in: an element of the reference's type with its key's properties and `_missing="1"`, which
making the song like the version leaves as it is now. A lost run or `_bytes` makes the element it is in the stand-in.

## 5. What a song means

[`tests/golden/format/song-rules.json`](../tests/golden/format/song-rules.json), written by Phazite's tests from the
tables its code reads (`src/model/base/Schema.cpp`), says:
- `items`: the elements with an ID of their own, the property that holds it (`id`), and `MASTER`, which holds one too;
- `keys`: for the other elements, the properties their key is made of, joined with `/` (a curve by its device and its
  parameter, a tempo by its beat, written with 6 decimals); `unkeyed`: elements kept as a list in their parent (a curve's
  points); any other element is keyed by its type, the one of its kind in its parent;
- `orders`: the elements whose order means something, and which order (a track's devices are its `chain`);
- `otherIDHolders` and `idNamers`: properties that hold an ID of the song's, and those that name one, which renumbering
  changes together;
- `uncountedProperties` and `uncountedElements`: view state and how the song plays, which are no change;
- `piecesOfTheirOwn`, `inRuns`, `bytes`: how Phazite splits a song (section 4).

IDs are 64-bit numbers a song counts up (`nextID` on `PHAZITE`), never given twice. Two lines of history count on from the
same place, so a merge can find one ID on two different things (section 9).

`idNumbering` on `PHAZITE` says which numbering a song's IDs are in: none, or a stamp of 16 lowercase hexadecimal digits
that Phazite makes as it gives a song's IDs anew as it opens (a file whose IDs leave too little room to count on:
[project-format.md](project-format.md), section 7). It is part of the song and goes into a version's song with it. The
versions saved before a song's IDs were given anew hold them as they were, without that stamp: Phazite matches two songs'
items by ID only where their stamps are the same (none and none, or one stamp and the same), and restores no line from a
version, nor merges one, whose song is stamped otherwise than the song, or whose parting with it is (section 9). What
makes the song another whole (a version opened in its place, a branch switched to, Restore This Version, Restore from
Local History) matches nothing by ID that way and takes such a song with its stamp.

[`tests/golden/format/devices.json`](../tests/golden/format/devices.json) lists Phazite's own devices' parameters, their
ranges and defaults, and the name each choice is saved by. The song's elements and properties are described one by one
in [`src/model/base/internal/Schema.h`](../src/model/base/internal/Schema.h); the project file's format is
[project-format.md](project-format.md).

## 6. Versions and branches

**A version** is a piece holding a `VERSION` element: its song's piece (`song`), the versions it follows (`parents`, ids
separated by spaces: none for the first, two for a merge, the second the one merged in), its `name`, `note` and
`summary`, when it was made (`saved`, milliseconds since 1970), the id of the branch it was made on (`branch`) and the
Phazite that made it (`phazite`). Its children:
- a `FILE` for each audio file its song plays: its `path` (as the song holds it, relative to the project's folder with
  `/`), its `size` and its `sha256`. Audio files are named, not copied: files in `Audio/` and `Generated/` are written once
  and never changed;
- for a merge that gave new IDs to what the other line added, a `RENUMBERED` for each: the ID as the other line has it
  (`from`), as the merge gave it (`to`), and for a note its clip's group (`clip`: `link <alias group>` or `clip <id>`).

A version's id is its piece's name. Its record never changes.

**A branch** is each computer's record of it, `branches/<branch>/<computer>.xml`, a `BRANCH` element with its `id`, its
`name`, its `colour` (8 hexadecimal digits, ARGB), when the name or color were last set or the branch was deleted
(`changed`, milliseconds since 1970), its last version there (`head`), and `deleted="1"` once that computer deleted it. A
reader keeps the properties it doesn't know and writes them back. The computers' records together make the branch:
- its name and color are the record's whose `changed` is newest;
- it is gone when that newest record is deleted and its head follows every other record's head; versions another computer
  made on the branch since, or a name it gave since, bring it back;
- its last version is the head that follows every other record's head (a version follows another if the other is it or
  among its parents' line), when one does;
- otherwise the lines went their own ways: this computer's head (else the newest record's) is the branch's, and the
  others are lines to merge.

A deleted branch's versions that no other branch holds leave the graph and stay in the packs, as versions no branch holds.

**A version's message, edited** (Phazite's Rename Version...): a version's record never changes, so each computer keeps
the messages it edited in `messages/<computer>.xml`, a `MESSAGES` element with a `MESSAGE` for each version: its id
(`commit`), its `name` and `note` as they are now (either empty: a version without a name is named by its `summary`), and
when they were set (`changed`). Of every computer's, the newest names the version, in place of its record's name and note.

The song says which branch it is on and which version it grew from (`versionsBranch` and `versionsBase` on `PHAZITE`:
[project-format.md](project-format.md), section 3), which go with it into every file it is saved into; `current` names the
branch only where the song doesn't (a song without them, or naming a branch the folder hasn't). Where `current` names no
branch either, the branch made first is the current one: the branch whose oldest version (by `saved`) is the oldest. The
song's changes are worked out against the version it grew from: where the branch has newer versions, they are versions
the song doesn't hold yet, never changes taken out of it.

**This computer's own.** `local/<computer>/working.xml` is a `WORKING` element with a `LEFT` for each branch this
computer switched away from with a song of its own on it: the `branch` and the `song` (a song piece, view state
included). `local/<computer>/logs/<branch>` is a line for each move of the branch's last version on this computer, as
git's reflog: when (milliseconds since 1970), the version it was at (`-` for none), the one it went to (`-` for none),
why (`commit` for a version saved, `merge`, `branch`, `recover`, `undo` for Undo Last Version, `delete` for a branch
deleted, `reset` for a branch whose last version couldn't be read taken back to the newest version of its log that
reads, `cancel` for a branch a command made and took away again as it didn't finish), the song left on the branch (a
song piece: a deleted branch's, kept with its versions; a recovered branch's, which it came back with; `-` for none),
and the branch's name then, which may hold spaces and is the rest of the line; separated by single spaces. A log is read
as bytes: a line of another shape (one a crash cut off, without its line end) and zero bytes (which a power loss can
leave at a file's end) are left out, and the writer writes the log again whole, of its lines alone, at its next move.
Every computer's logs, read together, say when and why a version left every branch, from which Recover Lost Versions
offers it 30 days (section 7).

`local/<computer>/history` is this computer's Local History, a line for each snapshot of the song, oldest first: when it
was taken (milliseconds since 1970), the id of the branch the song was on, the song's piece (written as a branch's song
left on it is, view state included), and why (`open`, `auto`, `restore`, `discard`, `merge`, `abort` (before Cancel
Merge), `history`), separated by spaces. Phazite writes the file whole, thinned: every snapshot of the last two hours,
the newest of each hour for a day, the newest of each day for 30 days; and reads it thinned as of now, so that a snapshot
the file still lists past that is none. It is read as a log is: a line of another shape, and zero bytes, are left out.

**Versions no branch holds** are still in the packs (section 7): a reader finds them by the `<VERSION ` their pieces
start with, and when and why they left by the logs. Phazite's Recover Lost Versions lists those that left within 30
days, and makes a branch of a line of them, with the song left on it where a branch was deleted.

## 7. Keeping and sweeping

**Phazite deletes nothing from the store yet**: no piece, bounce or edited message. What follows is how it joins its
packs, losing nothing, and what a writer that lets go of pieces must keep.

**What names a piece.** Every branch as the computers' records make it (its last version and the lines that went their
own way), the songs left on branches (`working.xml`), Local History's snapshots, the songs before the merges waiting in
the open song and in any of those (`MERGING`'s `before`, `version` and `head`), and, by every computer's logs, the
versions a move of the last 30 days went from or to and the song a branch deleted within 30 days left; where a
computer's record of a branch can't be found, the branch's last place by that computer's log, in the log's order, unless
the log deleted it. A version names its song and the versions it follows; a song's piece names those it references
(section 4). Edited messages and audio files name no piece; a plug-in's state and its parameters' values name nothing.
A writer that lets go of pieces must leave out only pieces of its own that nothing names and that it wrote more than 30
days before the keeping time; and, where its own records name a version or a song that only another computer's packs
hold, write its own copy of it first, as that computer may let go of its own.

**The sweep.** A writer joins its own packs as of the keeping time: now, but no more than a week past its last sweep
(`local/<computer>/swept`: its first line when, a time after now taken as now; then a line for each version that went
whose bounce and edited message are still to delete, none while nothing goes), by which Local History is thinned and
the logs' 30 days are counted too; a writer's first sweep (none noted) notes the time. Every piece written within those
30 days counts as named, with what it names. Where a file that names something doesn't read whole (a branch's record,
`working.xml`, a recent line of a history or a log), or a sync program left its copy of one (a "conflicted copy", or
another name no writer gives), or a named piece can't be read (damaged, or not arrived), the walk of what is named is
unsure. A log's or a history's line of another shape, or zero bytes, count as a file that doesn't read whole only while
they are recent: a line whose first word is a time within the last 30 days, or zero bytes beside such lines; older ones,
and lines without a time, are passed over. Files a system puts in folders (`desktop.ini`, `Thumbs.db`, `.DS_Store`,
`.directory`, and every name starting with a dot) are no writer's and are passed over. A writer never deletes another's
pack.

**Times.** Every time a writer stamps (a version's `saved`, a branch record's `changed`, an edited message's, a log's
line, a snapshot, a piece's entry) is later than the stamp it gave before, and later than a floor set as the history
opens: the newest time it ever stamped, kept in `local/<computer>/stamped` (one line, milliseconds since 1970) and taken
again from its logs, its history and its own packs' entries (a time later than ten years past the clock passed over, as
a damaged file's), never lowered and apart from the sweep's time. So a clock
set back, even at every start, stamps nothing older than what came before it, and no two stamps of a writer are the
same; a clock that was ahead keeps what it stamped meanwhile longer. A noted sweep time after now is written again as
now, so that a clock put right lets weekly sweeps go on.

Phazite sweeps as a history opens, once a week. It joins its packs by size, as git's geometric repacking does: those
under 1 MiB, those of a size class (each four times the one before, from 1 MiB) once four of a class gather, and those
that hold the same pieces twice (left by a crash), keeping every piece; a pack with none of its class to join is left
as it is. The joined pack is written whole beside the others (under a name starting `joining-`, which a writer deletes
once it is a day old), its bytes checksummed as they are written and read back against it before the packs it replaces
are deleted (each one's writer read again first); a join with nothing to write writes no pack. A pack that can't be
deleted (another program holds it) stays for the next sweep.

## 8. Audio files

`local/<computer>/files.xml` is a cache: a `FILES` element with a `FILE` for each file this computer hashed, its `path`,
`size`, `modified` (milliseconds since 1970) and `sha256`, so that a file is read again only when its size or time
change. A tool can delete it.

## 9. Comparing and merging

Two songs are compared element by element, matched by their keys (section 5); two values are the same when they write
the same text (section 4).

A merge of another line into this one is three-way, from where the lines parted: of the versions both lines follow, one
that no other of them follows (as `git merge-base`, by the graph, not by when versions were made; where merges crossed
and there are several, the newest, then the greater id). For each element by its key, and each property:
- changed on one side only: that side's;
- changed on both sides to different values: a conflict, this side's kept until the user picks, with all three values;
- added on one side: added; taken away on one side and unchanged on the other: taken away; taken away on one side and
  changed on the other: kept.

Then each alias group of the merged song (the clips with one `link`) holds together: its keeper is its first clip, in the
song's order, that the parting and both sides hold with that `link`; failing that, the first both sides hold with it; the
first both sides hold at all; or its first. Every other clip of the group takes the keeper's `NOTE` and `CONTROL` elements,
copies in their order, and the conflicts found in those clips' notes are dropped, so that a note's conflict is listed (and
counted in `chosen`) once, in the keeper; a value chosen for it is set in every clip of the group. Of audio clips, every
other clip of the group takes the keeper's `file`, `tempo`, `pitch` and `reversed` (or their absence) and copies of its
`WARP` elements in their order (so an alias one line added, which kept the path the group played there, plays the file the
other line located anew), and the conflicts found in those four properties of the other clips are dropped, so that such a
conflict is listed once, in the keeper; a value chosen for it is set in every clip of the group. A group left with one
clip loses its `link`.

**IDs given anew.** What the other line added gets a new ID where this side has something else with that ID, and what
names it is changed to match; the merge's version records each (`RENUMBERED`). Before a later merge of the same line,
the other side's song and where they parted are translated through the records of this line's merge versions since the
parting whose merged-in version the other line follows, all at once, so that the same thing is matched by the same ID:
- only the IDs that the song where they parted holds (for a note, in that clip) are translated, the later record's word
  over an earlier's;
- any other ID of either song that one of them is translated to (an item the other line made after a merge gave its ID
  to one of theirs, on a computer whose count never reached this line's) gets a new ID of this side's too, in both songs,
  and the merge's version records it with the others (`RENUMBERED`), so that a later merge translates it alike.

Where the other line's own merges gave anew what this line holds too (a merge back: main merged idea-2 and gave an item of
it a new ID, and idea-2 merges main; or two computers each merged the same version and gave an item of it different IDs),
the other side's song is translated first, before the records above, into the parting's numbering, through the records of
the other line's merges since the parting whose merged-in version this line follows (the latest for each `from`, of the
IDs its song holds), all at once; the records above then take it into this side's. One numbering a stage, so that nothing
is translated twice:
- each ID that line gave such an item is made the parting's, its `from`, where one of the records above translates that
  `from` (an item the parting holds; a note in its clip, as the parting numbers the clip); else this line's straight away:
  the ID this line's latest merge gave the same `from` (for a note, in its clip as this line numbers it), else the `from`
  itself; to itself (`to>to`) where it is that ID already;
- an item of the other line's own in the way first gets a new ID of this side's, one neither song nor any record holds,
  so that it stays that line's addition: one under an ID the first crosses to, unless it crosses away itself or is the
  parting's own item there (one the parting holds that none of that line's records gave another ID); and one under an ID
  one of its merges gave anew (a `from` of theirs) whose merged-in version the parting follows, where the parting holds
  that ID too (that version's item), whether or not this line has a record for it; a note alike, in its clip, which the
  parting is asked for by its own ID for that clip;
- a note is translated by its clip as these translate it: by its new ID, for a clip given one;
- the targets of the first never count as IDs of either song that one of the records above is translated to.
The merge's version records these with the others (`RENUMBERED`), each as the merged song holds it, after both stages
(`from>to`, `to` the ID the records above took it on to; a note's by its clip as the merged song numbers it), so that a later
merge translates by it at once. Where they aren't one to one, the crossings are left out and the new IDs kept, as an ID no
item has can never make two items one.

Where both songs, translated, would still hold an ID twice (an item's anywhere, a note's in one clip: records another tool
wrote, or a history these translations don't cover), nothing is merged, and the merge says why: two items under one ID
would be made one. Nor where the other side's song, or the parting's, is stamped with another numbering than this side's
(`idNumbering`, section 5): its IDs from 2^61 up were given anew on one side and not the other, so an ID can't be relied on
to name the same item on both. Nor, once the list shows, where the song has been made another numbering's since (Restore
This Version, Restore This Automatic Save, Discard All Changes, or Undo of one): Merge keeps the stamp the song had as its
list was worked out, and takes nothing into a song stamped otherwise.

A record that isn't one to one (an ID 0, or one ID given to two: a damaged one, or one another history wrote) is left out.

**A merge waiting for its version** (git's `MERGE_HEAD`) is part of the song, so that every file the song is written into
(the project file, its autosave, a song left on a branch, Local History) holds the merge state that song is in: a
`MERGING` element of `PHAZITE`, all its values text. `version` is the version merged in and `head` the branch's last
version as the merge was taken, each a version's id; `before` is the song as it was before the merge, a song piece (view
state included) that Cancel Merge brings back. A merge takes every line the other line changed. `chosen` has a digit
for each conflict, in the merge's order of them (`0` the parting's value, `1` this branch's, `2` the other line's);
`renumbered` are the IDs it gave anew, as its version will record them, `translated` those the two songs were
translated through before it, and `crossed` those the other line's song was translated through before that (above, as the
first stage gives them, not as its version records them), each
`from>to`, or for a note `<its clip's group>/from>to`, separated by commas. Nothing is written into it after it is taken.

It waits until its own version finishes it: a version of the branch's line whose second parent is `version`, whose first
parent follows `head`, and whose `RENUMBERED` holds every one of `renumbered`. Its version follows the branch's last
version (which may have moved on since `head`) and the one merged in. Where the branch's line holds `version` otherwise:
- through another computer's merge of it (a version with `version` as its second parent) that recorded other IDs, it still
  waits, and its version records `renumbered` and, for each `from` that merge recorded and this one didn't, `from>from`
  (the item as this song holds it); a later merge of that line translates through both records, the later one by the
  graph winning for each ID;
- without any such merge, it waits no more, and Cancel Merge takes the element out.

A version's song never holds `MERGING`, whichever song it was made from, and a reader of one leaves it out. A reader takes
the element as untrusted: one whose ids aren't ids, whose version merged in the history hasn't (a project opened without
its Versions folder), or whose IDs aren't one to one merges nothing, and Cancel Merge takes it out of the song, alone. The
song before it is read on its own: where it reads as a whole song, Cancel Merge makes the song it, even where the
parting's or the other line's song can't be read (the merge then waits without its conflicts); where it doesn't (a
`before` naming a track's piece), Cancel Merge makes the song the branch's last version. Its conflicts are found again by
merging the same three songs.

## 10. Test vectors

[`tests/golden/format/song.pack`](../tests/golden/format/song.pack) is a pack Phazite wrote of a small song and its
version; [`tests/golden/format/song.txt`](../tests/golden/format/song.txt) lists its pieces, a line each in the order of
their names, as `tools/read-versions.pl` prints them: name, size, and the piece's text or `base64:` and its bytes. A
reader that lists the pack as `song.txt` does reads Phazite's pieces as Phazite does. CI checks the Perl reader so on
every push; Phazite's golden tests (`tests/GoldenFormatTests.cpp`) write both files again when the format changes on
purpose.
