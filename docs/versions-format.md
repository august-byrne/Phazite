# The versions format

*How Phazite keeps a project's history, for anyone writing a tool that reads or writes it: a project's `Versions` folder,
its packs of pieces, its commits and branches, and how two songs are compared and merged. Written 2026-11-09, from the
[format audit](format-audit.md). The code is `src/model/versions/`; where this file and the code disagree, the code is
right and this file has a bug.*

This text is under the AGPL, as all of Phazite is. Implementing the format it describes, in software of any license, open
or closed, needs no permission: copyright covers the text, not the format.

Phazite is pre-alpha, and the format has no version yet: an update may change it, until the owner says compatibility
starts ([design.md, section 18](design.md#18-projects-saving-and-export)). Then a file naming the format and its version
comes first ([format audit, P5](format-audit.md#34-portability-as-a-concept)).

## 1. The idea

A history is a store of **pieces**, each named by the SHA-256 of what it holds, written once and never changed, as git
keeps its objects. A song is a tree of elements (XML); its bigger parts (tracks, clips, devices, curves) are pieces of
their own, so a commit that changed one note stores that note's run, its clip's piece, its track's and the song's: a few
kilobytes. A **commit** is a piece too, naming its song's piece and the commits it follows. A **branch** names its last
commit, in a small file each computer writes for itself.

What a song means (which children are ordered, what an element's key is, which properties hold IDs, what isn't a
change) is data, in [`tests/golden/format/song-rules.json`](../tests/golden/format/song-rules.json), so that a tool can
compare and merge songs as Phazite does without Phazite's code. Another kind of document could use the same store with
rules of its own.

## 2. The folder

```
Versions/
  pieces/<name>.pack                 pieces, in packs (section 3)
  branches/<branch>/<computer>.xml   a branch as one computer last had it (section 6); only that computer writes it
  messages/<computer>.xml            commits' messages as one computer edited them (section 6); only it writes it
  current                            the id of the branch the project file is (one line, no end of line needed)
  local/<computer>/files.xml         this computer's cache of its audio files' hashes (section 7)
  local/<computer>/working.xml       the songs this computer left on branches when it switched away (section 6)
  local/<computer>/logs/<branch>     each move of a branch's last commit on this computer (section 6)
  local/<computer>/history           this computer's Local History: the song at times, without commits (section 6)
  bounces/<commit>.flac              a commit's bounce, if it has one: its song rendered, FLAC, 16 bits
```

`<branch>` is the branch's id and `<computer>` the computer's: each 32 lowercase hexadecimal digits (a UUID without its
dashes). A computer's id is made once, at random, and never names the user or the machine.

**Who writes what.** Pieces and bounces are written once and never changed. Each computer writes only its own files: its
record of each branch, its file of edited messages, and everything under `local/<computer>/`. So a folder that Dropbox, OneDrive or a NAS syncs
between computers never has two writers on one file, but `current`, which goes with the project file, as the project file
does.

**Writing safely.** Every file is written whole to a temporary name, flushed to the disk, then renamed into place; a log
is appended to and flushed. A reader ignores what it doesn't know: files, elements and properties.

## 3. Packs

A pack holds pieces and an index of them, and is named by the SHA-256 of its index, in lowercase hexadecimal:
`<SHA-256 of index>.pack`. Numbers are little-endian.

| Part | Bytes | |
|---|---|---|
| mark | 4 | `PZPK` |
| pieces | ... | each compressed by zlib (RFC 1950: zlib's header and checksum, not gzip's), one after another |
| index | 48 per piece | its name (32 bytes, the SHA-256 itself), where it starts in the file (8), its size compressed (4) and not (4) |
| count | 4 | how many pieces the index lists |
| index start | 8 | where the index starts in the file |
| mark | 4 | `PZPK` |

A reader:
- leaves out a pack whose marks aren't both there, whose index doesn't fit exactly between its start and the count, or
  whose index's SHA-256 isn't the pack's name;
- refuses a piece whose uncompressed size is more than zlib can make of its compressed bytes (1,032 times them, and a
  little), or that doesn't inflate to that size;
- checks each piece's SHA-256 against its name, and treats one that doesn't match as missing;
- takes a piece from the first pack that lists it, when several do.

A writer writes the pieces no pack has yet into a new pack. Phazite joins its packs into one once there are more than 50,
writing the joined pack before deleting the others, and keeps a pack that holds a damaged piece.

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
| `<_parameters hash="H"/>` | after any `_bytes`, on a plug-in's `DEVICE` | the piece H holds `<_parameters>` with a `<_parameter id= name= value=/>` for each of the plug-in's parameters as the plug-in told them when the commit was made, by the plug-in's own IDs, values from 0 to 1; not part of the song |

**How a writer splits a song** is its own choice; a reader only follows the references. Phazite makes pieces of its
tracks, the master, clips, devices, containers and automation curves (`piecesOfTheirOwn` in the rules); puts an
element's notes or points (`inRuns`) in runs once it has 128 of them, a run ending after an element whose text's FNV-1a
hash (64 bits) is a multiple of 64, so that a note changed or added changes its own run alone; and keeps a plug-in's
state (`bytes`) out of line when its base 64 reads back as the same text.

**Reading.** A song is joined from its own piece down, each reference replaced by what its piece holds. A reader that
can't read a piece either fails the whole song, or, when the user asks for as much as can be read (Phazite's Restore),
puts in its place a stand-in: an element of the reference's type with its key's properties and `_missing="1"`, which
making the song like the commit leaves as it is now. A lost run or `_bytes` makes the element it is in the stand-in.

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
same place, so a merge can find one ID on two different things (section 8).

[`tests/golden/format/devices.json`](../tests/golden/format/devices.json) lists Phazite's own devices' parameters, their
ranges and defaults, and the name each choice is saved by. The song's elements and properties are described one by one
in [`src/model/base/internal/Schema.h`](../src/model/base/internal/Schema.h); the project file's format is
[project-format.md](project-format.md).

## 6. Commits and branches

**A commit** is a piece holding a `VERSION` element: its song's piece (`song`), the commits it follows (`parents`, ids
separated by spaces: none for the first, two for a merge, the second the one merged in), its `name`, `note` and
`summary`, when it was made (`saved`, milliseconds since 1970), the id of the branch it was made on (`branch`) and the
Phazite that made it (`phazite`). Its children:
- a `FILE` for each audio file its song plays: its `path` (as the song holds it, relative to the project's folder with
  `/`), its `size` and its `sha256`. Audio files are named, not copied: files in `Audio/` and `Generated/` are written once
  and never changed;
- for a merge that gave new IDs to what the other line added, a `RENUMBERED` for each: the ID as the other line has it
  (`from`), as the merge gave it (`to`), and for a note its clip's group (`clip`: `link <alias group>` or `clip <id>`).

A commit's id is its piece's name. Its record never changes.

**A branch** is each computer's record of it, `branches/<branch>/<computer>.xml`, a `BRANCH` element with its `id`, its
`name`, its `colour` (8 hexadecimal digits, ARGB), when the name or color were last set or the branch was deleted
(`changed`, milliseconds since 1970), its last commit there (`head`), and `deleted="1"` once that computer deleted it. A
reader keeps the properties it doesn't know and writes them back. The computers' records together make the branch:
- its name and color are the record's whose `changed` is newest;
- it is gone when that newest record is deleted and its head follows every other record's head; commits another computer
  made on the branch since, or a name it gave since, bring it back;
- its last commit is the head that follows every other record's head (a commit follows another if the other is it or
  among its parents' line), when one does;
- otherwise the lines went their own ways: this computer's head (else the newest record's) is the branch's, and the
  others are lines to merge.

A deleted branch's commits that no other branch holds leave the graph and stay in the packs, as commits no branch holds.

**A commit checked out on its own** (Phazite's Checkout (Detached), git's detached `HEAD`) is a record of the same kind, of
no `name`, whose `detachedFrom` is the id of the branch the song came from, and whose `head` is the commit checked out;
`current` names it while the song is there. It is that computer's alone: a reader leaves out every other computer's
record that has `detachedFrom`. Phazite writes it `deleted="1"` once the song goes back to a branch.

**A commit's message, edited** (Phazite's Edit Commit Message...): a commit's record never changes, so each computer keeps
the messages it edited in `messages/<computer>.xml`, a `MESSAGES` element with a `MESSAGE` for each commit: its id
(`commit`), its `name` and `note` as they are now (either empty: a commit without a name is named by its `summary`), and
when they were set (`changed`). Of every computer's, the newest names the commit, in place of its record's name and note.

Where `current` names no branch, the branch made first is the current one: the branch whose oldest commit (by `saved`)
is the oldest.

**This computer's own.** `local/<computer>/working.xml` is a `WORKING` element with a `LEFT` for each branch this
computer switched away from with a song of its own on it: the `branch` and the `song` (a song piece, view state
included). `local/<computer>/logs/<branch>` is a line for each move of the branch's last commit on this computer, as
git's reflog: when (milliseconds since 1970), the commit it was at (`-` for none), the one it went to (`-` for none), and
why (`commit`, `merge`, `branch`, `recover`, `undo` for Undo Last Commit, `delete` for a branch deleted, `checkout` for a
commit checked out on its own), separated by spaces. A last line without its line end was cut off and is left out.

`local/<computer>/history` is this computer's Local History, a line for each snapshot of the song, oldest first: when it
was taken (milliseconds since 1970), the id of the branch the song was on, the song's piece (written as a branch's song
left on it is, view state included), and why (`open`, `auto`, `restore`, `discard`, `merge`, `history`, `checkout`), separated by
spaces. Phazite writes the file whole, thinned: every snapshot of the last two hours, the newest of each hour for a day,
the newest of each day for 30 days. A last line without its line end was cut off and is left out.

**Commits no branch holds** are still in the packs: a reader finds them by the `<VERSION ` their pieces start with.
Phazite's Verify History lists them, and Recover Lost Commits makes a branch of each line of them.

## 7. Audio files

`local/<computer>/files.xml` is a cache: a `FILES` element with a `FILE` for each file this computer hashed, its `path`,
`size`, `modified` (milliseconds since 1970) and `sha256`, so that a file is read again only when its size or time
change. A tool can delete it.

## 8. Comparing and merging

Two songs are compared element by element, matched by their keys (section 5); two values are the same when they write
the same text (section 4).

A merge of another line into this one is three-way, from where the lines parted: of the commits both lines follow, one
that no other of them follows (as `git merge-base`, by the graph, not by when commits were made; where merges crossed
and there are several, the newest, then the greater id). For each element by its key, and each property:
- changed on one side only: that side's;
- changed on both sides to different values: a conflict, this side's kept until the user picks, with all three values;
- added on one side: added; taken away on one side and unchanged on the other: taken away; taken away on one side and
  changed on the other: kept.

**IDs given anew.** What the other line added gets a new ID where this side has something else with that ID, and what
names it is changed to match; the merge's commit records each (`RENUMBERED`). Before a later merge of the same line,
the other side's song and where they parted are translated through the records of this line's merge commits since the
parting whose merged-in commit the other line follows, all at once, so that the same thing is matched by the same ID.

## 9. Test vectors

[`tests/golden/format/song.pack`](../tests/golden/format/song.pack) is a pack Phazite wrote of a small song and its
commit; [`tests/golden/format/song.txt`](../tests/golden/format/song.txt) lists its pieces, a line each in the order of
their names, as `tools/read-versions.pl` prints them: name, size, and the piece's text or `base64:` and its bytes. A
reader that lists the pack as `song.txt` does reads Phazite's pieces as Phazite does. CI checks the Perl reader so on
every push; Phazite's golden tests (`tests/GoldenFormatTests.cpp`) write both files again when the format changes on
purpose.
