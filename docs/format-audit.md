# Format audit

*2026-11-09, the owner's request: "is source control able to handle changes in external plugins? i also want you to do an
audit of the source controls format in order to check it for robustness and extensibility. this format should not be
fragile and should be portable as a concept if anyone else wants to implement a tool with it, like how git is used in all
sorts of projects. do the same thing for the phazite project format, if that is seperate. and is there a reason we should
use our own .phazite instead of a DAWproject format (i know we are able to import/export as that/ just currious as to the
pros or cons of being our own .phazite format)." Nothing was changed for the audit itself, whose plan is
[section 6](#6-the-plan). The owner then asked for it to be built ("Do all of your recommendations now"), the same day;
[section 7](#7-what-was-built) says what was, and what waits.*

The audit covers the formats that hold a Phazite song: the versions' store in a project's `Versions` folder
([versions.md](versions.md)), the project file (`.phazite`) and, in passing, presets (`.phazitepreset`). The store and the
project file are separate formats for the same song. Both hold the elements and properties that
[Schema.h](../src/model/base/internal/Schema.h) names: the project file as one XML file, the song as it is now; the store
as pieces in packs, the song as it was at each commit.

How it was done:
- **The code** of both formats, and the JUCE 9.0.5 code they call to write files, numbers, binary data and plug-in states.
  What the audit says about behavior is read from that code: nothing was built or run in Phazite for it.
- **A real history, read from outside.** A reader of 46 lines of Perl, written from the comment in
  [PieceStore.h](../src/model/versions/internal/PieceStore.h) and nothing else of Phazite's, read the two packs of a
  project on the owner's computer, a copy of the Lo-fi Beat example with two commits
  ([section 3.2](#32-what-is-sound-and-should-stay)).
- **Git** as the measure of "portable as a concept", and **DAWproject 1.0**'s README and schema, as they stood on
  2026-11-09, for the last question.

## 1. In short

- **Plug-ins: yes, as far as anything outside a plug-in can know it.** A commit keeps each plug-in's whole state, stored once
  for all the commits that have the same bytes, and the value of every one of its parameters as text. A change made in a
  plug-in's own window is committed. The Changes list names the knobs that moved ("Serum: Cutoff 40% → 55%"). Restore and
  Discard put the commit's state back into the running plug-in as one undo step, and a checkout puts back its branch's. A
  merge takes the state of the side that changed it, or offers a conflict with its three values where both did. What it
  can't do: merge two edits inside one plug-in, keep the files a plug-in loads by itself, or tell which version of the
  plug-in made a state. And one part of the design isn't built, so a plug-in whose bytes differ at every save shows as
  changed when nothing changed ([section 2](#2-plug-ins-and-versions)).
- **The store is sound at its core.** Pieces are named by the SHA-256 of what they hold, written once, flushed to the disk
  and renamed into place, and checked at every read, and a reader needn't know how the writer split the song. The outside
  reader checked all 108 pieces and both packs. Ten robustness findings follow, most of them small fixes. The two that
  matter most only bite once two computers share a history ([section 3.3](#33-robustness)).
- **It isn't yet portable as a concept** ([section 3.4](#34-portability-as-a-concept)). What lets many tools share git's
  format is that its objects are few, simple and written down, and that a repository says which format it is. Phazite's
  store has the first and lacks the rest: its only specification is C++, a plug-in's state is wrapped in three of JUCE's
  private encodings, the same song can make different bytes depending on how its values were set in memory, and nothing
  names the format or its version.
- **The project file** ([section 4](#4-the-project-file)) is saved safely and reads well as text, one property per line. It
  shares the plug-in states' encodings, saves choices as numbers (`filterType="8.0"`), names a plug-in by its file's path,
  and offers no backup when it won't open.
- **Keep `.phazite`** ([section 5](#5-why-phazite-and-not-dawproject)). DAWproject's README lists "being the native
  file-format for a DAW" among its non-goals. It has no place for most of what Phazite saves and no way to extend it, its ids
  don't last from one save to the next (versions need IDs that do), and the DAWs that made it keep formats of their own and
  exchange through it, as Phazite does. Its one real advantage, an open format that other tools read, is one `.phazite` can
  have too.

## 2. Plug-ins and versions

### 2.1 What a commit keeps of a plug-in

| What | In the song | In a commit |
|---|---|---|
| Which plug-in it is: its format, name, maker, file, and JUCE's number for it | the `DEVICE`'s properties | as the song has them |
| Its state: everything it saves of itself | `state`, as text | a piece of its own, in its bytes; equal bytes are stored once for all commits |
| Every parameter's value, by the plug-in's own ID, with its name | no | a piece beside the state (`parametersPiece`), read from the plug-in as the commit is made |
| The values set by hand of the parameters Phazite moves (its controls, automation, modulation) | `VALUE` children | as the song has them |

A commit stores every plug-in's state into the song first, as a save does (`Song::storeDeviceStates()`), and Commit works
even while the Changes list is empty. So a change that moves no parameter, such as a sample loaded into a sampler plug-in,
is still committed.

### 2.2 What works

- **A change in a plug-in's own window** counts as soon as the plug-in reports it: a parameter moved, or a VST3's
  `setDirty()`. The Changes list names the knobs whose values moved since the last commit; a change that moved none shows as
  the plug-in's name alone.
- **Restore and Discard** give the running plug-in the commit's state (`PluginInstances::takeFromSong()`) in one undo step,
  and leave alone a plug-in whose state is the same (`tests/VersionsGoingBackTests.cpp`). A checkout does the same, outside
  the undo history.
- **A merge** takes the state of the side that changed it. Where both sides did, it is a conflict with the three states,
  settled by Accept Current, Incoming or Base, and heard at once while the song plays.
- **A plug-in that isn't installed** keeps its state, byte for byte, through commits, merges and checkouts, and plays again
  once it is installed.

### 2.3 What it can't do, and what isn't built

1. **Two edits inside one plug-in can't be merged.** A state is in the plug-in's own format, which only the plug-in reads,
   and no DAW merges them. Where both sides only moved parameters, a conflict could later offer a fourth choice: one side's
   state, with the knobs the other side moved set on top. That guesses how the plug-in works, so it should be offered,
   never done by itself.
2. **The files a plug-in loads by itself aren't kept.** A commit records the audio files Phazite's own clips and devices
   play, by path, size and SHA-256 (`VersionStore::hashFiles()`). A plug-in's samples, wavetables, impulse responses and
   libraries are its own business: replace a sample a plug-in reads, and no commit brings back the old one.
3. **The plug-in's version isn't recorded.** After an update, an old commit gives its bytes to the new plug-in. Plug-ins
   read their older states as a rule, but the sound can differ, and the commit's bounce is then the only reference. With the
   version kept beside each state, the Changes list could say "Serum updated: 1.36 → 1.37", and a conflict could warn.
4. **Bytes that differ when nothing changed show as a change. Designed, not built.** [versions.md](versions.md#differences)
   says a plug-in's settings count as changed only when the plug-in reported a change. The code counts any difference in the
   stored bytes: `pluginState` isn't among what goes uncounted ([Schema.cpp:115](../src/model/base/Schema.cpp#L115)), and
   `holdTheSame()` compares it. Every save, Auto Save's included (a second after each edit), stores every plug-in's state
   anew. So a plug-in that writes a time or a random number into its state marks its track M after any save. The fix:
   compare a plug-in's state only if it reported a change since the last commit, or if one of its knobs moved. Phazite
   counts reported changes already (`Song::markChangedOutsideHistory()`), for the whole song; it would count them for each
   device.
5. **A plug-in's conflict says "settings"**, though versions.md promises the knobs each side moved. The three commits' knob
   values are there to name them.
6. **A plug-in that isn't playing as a commit is made** (it crashed, or plug-ins are off) gets no knob values in that
   commit, so the next comparison can't name its knobs. Its state is kept.

## 3. The versions store

### 3.1 What it is

| In `Versions/` | What it holds | Written |
|---|---|---|
| `pieces/<SHA-256 of its index>.pack` | pieces, each compressed by zlib, then their index: one pack per commit, and per branch left | once, never changed |
| `branches/<uuid>.xml` | a branch: its name, color, last commit, and the song as it was left there | replaced whole |
| `current` | the branch the project file is | replaced whole |
| `files.xml` | each audio file's SHA-256, by its path, size and time, so that each is read once | replaced whole; a cache |
| `bounces/<commit id>.flac` | a commit's bounce | once |

A **piece** is one element of the song as canonical text: XML without spaces, its properties in the order of their names.
A part of it that is a piece of its own (a track, the master, a clip, a device, a container, an automation curve) is
written in its place as `<PIECE piece="<SHA-256>"/>`. A long list of notes or points goes in runs (`RUN`), each a piece,
whose ends depend on what they hold. A plug-in's state is a piece of its own, in its bytes (`statePiece`). A **commit** is a
piece too: a `VERSION` record that names its song's piece, its parents, its name, note and summary, when it was saved, its
branch, the Phazite that saved it, and its audio files.

In git's words: pieces are blobs and trees at once, a `VERSION` is a commit, a branch's file is a ref, `current` is HEAD,
and the staged song is the index, kept in memory only.

### 3.2 What is sound, and should stay

- **Named by content.** A piece's name is the SHA-256 of its bytes before compression, so the compression can change
  without renaming anything. Git names its objects the same way.
- **Written once, safely.** A pack goes to a temporary file, is flushed to the disk (JUCE calls `FlushFileBuffers` on
  Windows and `fsync` elsewhere), then renamed into place (`ReplaceFile`, `rename`). A crash leaves a temporary file, never
  half a pack under a pack's name. A pack cut off is left out as the store opens: it needs its marks at both ends and an
  index that fits.
- **Checked at every read.** A piece's bytes are hashed and compared with its name, so damage is never read as other music.
- **Readers don't depend on the writer's choices.** Which elements become pieces, and where runs end, is the writer's
  business; a reader follows `PIECE` and `RUN` wherever they are. So the split can change at any time without breaking a
  reader: the property that has kept git's objects the same for twenty years.
- **Commits and branches.** A commit is named by its record's hash, with its parents in order (the first is its branch's).
  A branch is named by a UUID, its name kept apart, so two computers, or a disk that ignores case, never confuse two
  branches.
- **Audio named, not copied,** by path, size and SHA-256. **Bounces** hang off a commit by its ID, outside its record, so
  making or deleting one changes no history.
- **Readable from outside, today.** The Perl reader, written from `PieceStore.h`'s comment alone, read both packs of the real
  project and checked their marks and indexes, every piece's SHA-256 (108 of 108) and each pack's name (the SHA-256 of its
  index), and printed the `VERSION` records with their parents, songs and files.

### 3.3 Robustness

**R1. A pack's index isn't checked.** A pack is named by the SHA-256 of its index, but the store doesn't compare them as it
reads the index ([PieceStore.cpp:65](../src/model/versions/PieceStore.cpp#L65)). One hash of a few kilobytes per pack, as
the store opens, would catch a damaged index before any of its entries is used. *Small.*

**R2. A damaged index can crash Phazite.** Each entry gives its piece's size before compression, and the store allocates
that much before reading anything ([PieceStore.cpp:36](../src/model/versions/PieceStore.cpp#L36)): up to 4 GB. If the
allocation fails, JUCE throws `std::bad_alloc`, nothing catches it, and Phazite closes. Fix: refuse a size larger than zlib
can make of the compressed bytes (1,032 times them), or inflate as a stream while hashing. *Small.*

**R3. One damaged piece costs every commit that shares it.** A song is read whole or not at all (`joinSong()`), and pieces
are shared: a plug-in state that 200 commits have is one piece, so damage to it makes 200 commits unreadable. Fix: read
what can be read, with a marked stand-in where a piece is missing ("Bass: a clip couldn't be read"), so that Restore can
still take the rest; and a Verify History command, as git's `fsck`, that reads every piece and says what is damaged.
*Medium.*

**R4. Commits that nothing names are lost to the window.** A branch's file damaged or deleted takes its line away: its
commits are still in the packs, but nothing lists them. And a branch moved by mistake can't be moved back. Fix: find
commits nothing names by looking for `VERSION` pieces in the packs (git's `fsck --lost-found`), and keep an append-only log
of each branch's commits (git's reflog). *Small to medium.*

**R5. Packs are never joined.** Each commit, and each branch left, writes a pack, and the store reads every pack's index as
it opens. Packs were chosen to avoid thousands of tiny files, but a few thousand commits make a few thousand packs. Fix:
join them in the background into one pack once there are more than, say, 50, as git's `repack` does, writing the new pack
before deleting the old ones. *Medium.*

**R6. Rewriting a record drops what this build doesn't know.** `writeBranch()` writes only the fields `BranchRecord` has
([VersionStore.cpp:169](../src/model/versions/VersionStore.cpp#L169)), so a field that a later Phazite adds to a branch is
lost when an earlier one renames the branch. Fix: keep the attributes it doesn't know when it rewrites a record. *Small.*

**R7. The merge base is chosen by the clock.** It is the common commit saved last (`findCommonVersion()`,
[VersionHistory.cpp:1080](../src/model/versions/VersionHistory.cpp#L1080)). On one computer that is right. With two
computers whose clocks differ, or after merges that cross each other, it can pick an older base, and changes both lines
already share then show as conflicts. Fix: choose by the graph, a common ancestor that isn't an ancestor of another one, as
`git merge-base` does. *Small.*

**R8. Two computers aren't modeled, though versions.md says they are.** Its "Decided against" says the store "only adds
files, and each computer writes only its own branches' files", so that a folder Dropbox or OneDrive syncs stays safe. In
the code, every computer writes the same branch file, `current` and `files.xml`, and the last one written wins, or the sync
tool makes "conflicted copies". Packs are safe to sync. Fix, before backup to a synced folder or the cloud: keep each
computer's own files (`current`, `files.xml`, branches left with unsaved work) apart from the shared ones, and give each
computer its own record of where a shared branch is (git's remote-tracking branches), which Phazite then merges. *Medium,
when the cloud comes.*

**R9. IDs that a merge renumbers aren't remembered.** A song counts its IDs, and a merge gives new IDs to what the other
line added where they collide ([SongMerge.cpp:125](../src/model/versions/SongMerge.cpp#L125)), without recording old and
new. On one computer collisions don't happen: a checkout carries the higher count over, and a DAWproject imported as a
branch counts past the song. Between two computers they will. Then the next merge of the same line matches that item by
its old ID, which on this line belongs to something else, so the other line's later edits to it land on the wrong item or
clash with it. This is read from the merge's rules; it wasn't run. Fix, before collaboration: record the renumbering in the
merge's commit and translate through it, or make IDs unique per computer (the computer's number in an ID's top bits).
*Medium, when the cloud comes.*

**R10. Small ones.** Where `current` names no branch, the store falls back to "the first one made", but takes the first
file the folder lists ([VersionHistory.cpp:174](../src/model/versions/VersionHistory.cpp#L174)). And an audio file whose
copy into the project failed keeps its full path, which can carry the user's name into every commit; so can a plug-in's
file in a per-user folder ([section 4.2](#42-findings), F2).

### 3.4 Portability as a concept

Git's format serves so many tools because it is small, written down and implemented many times over (libgit2, JGit,
go-git, Dulwich and others), and because a repository says which format it is: `core.repositoryFormatVersion`, with
extensions a reader must understand, as repositories with SHA-256 names use. What another tool needs to read Phazite's
history today:

| To read | It needs | Written down in |
|---|---|---|
| a pack | its layout | `PieceStore.h`'s comment, which is enough |
| a piece | XML, with `PIECE`, `RUN`, `statePiece` and `parametersPiece` | the code of `SongPieces.cpp` |
| a commit or a branch | their properties | `Schema.h` (`schema::versions`) |
| what the song means | every element and property, with its type and range | `Schema.h`'s comments |
| what a device's values mean | each device's parameters, ranges and choices | each device's description in C++ (`src/devices/`) |
| a plug-in's state | JUCE's base 64, JUCE's binary XML block, JUCE's VST3 wrapper | JUCE's source code only |

**P1. No specification outside the C++.** The pack is described well; the rest is spread over `SongPieces.cpp`,
`VersionStore.cpp` and `Schema.h`. Fix: `docs/versions-format.md`, with the packs, the pieces' text, the records and the
branches, and test vectors (a small song, its pieces' bytes and names) that another implementation checks itself
against; the Perl reader becomes a tool that checks them. *Medium.*

**P2. A plug-in's state is wrapped in three of JUCE's encodings.** For a VST3, JUCE's `getStateInformation()` takes the
plug-in's two state streams, its processor's and its controller's, writes each into an XML element in JUCE's own base 64,
and puts that XML into JUCE's binary block, a magic number and a length before it (`copyXmlToBinary()`). The project file
then writes the block with `MemoryBlock::toBase64Encoding()`: JUCE's own scheme (the size, a dot, then an alphabet of its
own), not the base 64 of RFC 4648, though `Schema.h` calls it base 64. So:
- an outside tool must rebuild three of JUCE's formats to reach the bytes a VST3 knows;
- Phazite must read JUCE's wrapper for as long as it reads its old projects, even if it someday hosts plug-ins without
  JUCE (CLAP);
- the state is in base 64 twice in the project file: about 1.8 times its size, where standard base 64 of the plug-in's
  bytes is 1.33.

Fix: keep each plug-in's state in its format's own documented container. A VST3's is a `.vstpreset`, the VST3 SDK's
`PresetFile`, which Phazite's helper already writes for the DAWproject export. DAWproject asks for the same: a device's
state "in its native format", and "the VST3 plug-in state should be stored in .vstpreset format". An Audio Unit's is its
ClassInfo property list, and a CLAP's its state stream. In the project file, use standard base 64 (`juce::Base64`). A
commit, the project file and a DAWproject export then hold the same bytes for a plug-in. One care:
after JUCE's `setPreset()`, the helper must read the parameters' values again, as `setStateInformation()` does. *Medium.*

**P3. The same song doesn't always make the same bytes.** A value is written as JUCE's `var::toString()` gives it, which
depends on how it was set. A number set as an integer writes "4", as a double "4.0", and as a float widened to a double
"0.300000011920929"; a value read from a file is its text. The real project's pieces show it: `curve="-0.300000011920929"`,
`resonance="0.1000000014901161"`, `length="8.000000000000004"`. The differences aren't affected, since they compare
values. But storage is, versions.md's "the same song always makes the same pieces" holds only for the same types in memory,
and an outside writer can't make Phazite's bytes. Fix: one rule for a number's text, the shortest that reads back to the
same value (C++'s `std::to_chars`; the canonical JSON of RFC 8785 uses the same rule), integers without ".0", and parameters
that are floats written as floats ("0.3"). *Small to medium.*

**P4. Characters XML doesn't allow.** Both writers write a character below 32, other than a tab and the line ends, as
`&#1;` and the like, which XML 1.0 forbids even as a reference. A strict parser (libxml2's, .NET's, Java's) then refuses the
piece or the project file. Such characters come only from pasted text (a track's name, a note), and the fix is to drop
them where text comes in. *Small.*

**P5. Nothing names the format or its version.** That is the owner's rule while Phazite is pre-alpha, and it is right for
the song's schema. The store is different: a history can't be migrated without rewriting every hash, as git's move from
SHA-1 to SHA-256 shows. So when compatibility starts, the store's own format (the packs, the pieces' text, the records)
should be frozen and written down first, with a version of its own. The song's schema inside the pieces can keep changing,
with upgrades applied as an old commit is read: every `VERSION` already records the Phazite that saved it. A file such as
`Versions/format` would then name the format, its hash and the extensions a reader must understand, and a Phazite that
doesn't understand one would refuse to write to the store, as git refuses a repository whose extensions it doesn't know.
Nothing to add until the owner says. *Small, then.*

**P6. The hash isn't named.** Pieces are named by bare SHA-256. That is safe for decades; naming the hash in the format's
file (P5) is all it needs.

**P7. The song's rules are code, not data.** What an element's key is, which children have no order, which properties
hold or name an ID, and what isn't counted are C++ functions in `Schema.cpp`. For the store to be a concept others reuse,
as git is, they would be a small file shipped with the format, as `.gitattributes` tells git how to merge a kind of file.
Then the store and its three-way merge by IDs could serve any document made of elements with IDs (a score, a game's sound
banks, a lighting show), each with its own schema. Two smaller changes go with it. A binary value would go out of line by
a rule the schema states (any property it marks as bytes), not by `pluginState`'s name, so that a new kind (a sample
embedded later) needs no new reader. And the store's own names (`PIECE`, `RUN`, `piece`, `statePiece`, `parametersPiece`)
would take a prefix that no schema's names can have. Others built this already: Dolt for tables, and Irmin, a git-like
store of any data type with merge functions of its own. *Medium, later.*

**P8. A pack's entries have no room to grow.** Each is 48 bytes, and zlib is fixed for all of them. Since names hash the
bytes before compression, a byte naming the compression (zstd later, or deltas) can come with the format's version (P5).

**The specification's license is the owner's call.** The code is under the AGPL; a format that others implement, closed
tools included, spreads under a permissive license, as DAWproject's MIT one. Recommended: the format's documents under
CC BY 4.0 or MIT, the code as it is. *The owner's decision (2026-11-09): everything stays under the AGPL, and each
specification says that implementing its format needs no permission, in software of any license: copyright covers the
text, not the format.*

### 3.5 Where versions.md and the code disagree

- "The store only adds files, and each computer writes only its own branches' files": not so (R8).
- "A plug-in's own settings count as changed only when the plug-in reported a change": not built
  ([section 2.3](#23-what-it-cant-do-and-what-isnt-built), item 4).
- "Pieces are compared by hash, from the top down, so only what changed is read": the differences compare whole songs in
  memory, a track at a time as each changes. How that does on a big song is for the benchmark versions.md lists as not
  built yet.
- "Its clash names the parameters each side moved": it says "settings" (section 2.3, item 5).
- "The same song always makes the same pieces": only for the same types in memory (P3).

## 4. The project file

### 4.1 What is sound

- **A folder, one text file, and files that don't change.** The `.phazite` file is the song alone, as XML. Audio is copied
  into `Audio/` as it is imported, and named from the song by paths relative to the project's folder, with `/` between
  their parts. Positions are in beats.
- **Saved safely.** The same temporary file, flush and rename as the packs; ten backups; Auto Save written from a copy on a
  background thread; and the autosave for a crash. One Phazite runs at a time, so two never write one project.
- **Reads well as text.** One property per line, in the order of their names: git, or any tool that compares text, shows a
  change as the lines it changed.
- **One schema.** Every name is in `Schema.h`, with its type, range and default, and whether it is the song's, view state
  or state.
- **Tolerant.** Elements and properties Phazite doesn't know survive a load and a save (JUCE's `ValueTree` keeps them),
  and JUCE 9's XML reader is the hardened one.
- **A second copy.** With versions, the last commit holds the song too.

### 4.2 Findings

**F1. Plug-in states in JUCE's encodings,** as P2: the project file is where an outside tool meets them first.

**F2. A plug-in is named by its file and JUCE's number.** The song keeps a plug-in's format, name, maker, the path of its
file, and `uid`, a number JUCE makes for it. The song finds it by path and name, then by name, maker and format
([Mixer.cpp:846](../src/model/mixer/Mixer.cpp#L846)). The path is this computer's (a per-user VST3 folder holds the user's
name), and the number is JUCE's. Fix: keep the format's own ID, a VST3's class ID, which the DAWproject export already
reads from the preset's header, and find the plug-in by it first. *Small.*

**F3. Choices are saved as numbers.** A choice parameter of Phazite's own devices saves the number of its choice:
`filterType="8.0"`. Another tool can't tell what 8 is without the device's description in C++, and once compatibility
starts, a filter type added in the middle of the list would change old songs. Fix: save a choice by a name that never
changes (`filterType="lowpass"`, say), keeping the number in memory. *Small to medium.*

**F4. Numbers and characters,** as P3 and P4. And JUCE's writer writes every character outside ASCII as a number: a track
called "Café" is `Caf&#233;` in the file, though `é` in the pieces. That is valid XML, but harder to read in git. Writing the
project file with the pieces' writer (UTF-8, one rule for numbers) would make the two agree. *Small.*

**F5. Nothing checks a song as it opens.** A file edited by hand, or made by another tool, with a clip of negative length
or a volume of 1,000 dB, opens as it is. Fix: check each value against its range in `Schema.h` as the song opens, put right
what is out of range, and say what was. *Medium, with the ranges as data (P7).*

**F6. A project file that won't open is dropped.** Phazite takes it out of Open Recent and shows an error
([ProjectSession.cpp:601](../src/app/ProjectSession.cpp#L601)), though `Backups/` and the last commit both hold the song.
Fix: offer the newest backup that opens, or the last commit. *Small.*

**F7. No format name or version,** by the owner's rule until compatibility starts. Then: a format name and version on
`PHAZITE`, and `docs/project-format.md`, made from `Schema.h`'s comments and the devices' descriptions.

**F8. Presets** are JSON in a schema of their own, built to outlive changes inside the song: the right idea. Their plug-in
states carry JUCE's encodings too (F1).

## 5. Why .phazite and not DAWproject

### 5.1 DAWproject today

- Version 1.0, "stable", under the MIT license: a zip holding `project.xml` and `metadata.xml` (UTF-8), the audio embedded
  or referenced, and each plug-in's state as a file of its own, in the plug-in format's own form (a VST3's as a
  `.vstpreset`).
- Supported, by its README, by Bitwig Studio (from 5.0.9), Studio One (6.5), Cubase (14), Cubasis (3.7.1), VST Live (2.2)
  and n-Track Studio (10.2.2); REAPER reaches it through ProjectConverter, which the community made, and others through
  DawVert.
- Its non-goals, in its README: "Being the native file-format for a DAW", "Optimal performance (like a binary format could
  provide)" and "Storing non-session data (view settings, preferences)".
- Its devices: plug-ins (VST2, VST3, CLAP, AU) and four generic ones, an EQ, a compressor, a gate and a limiter. Its schema
  has no extension point (no `xs:any`): a DAW's own device goes in only as a `Device` with a state file that DAW alone reads.
- It is at version 1.0, and its last changes, in 2025, were to its README.

### 5.2 What Phazite would gain from it as its own format

- Every project would open in Bitwig Studio, Studio One and Cubase as it is, with no export.
- A published specification, a schema to check files against, and a library (in Java).
- The trust an open format earns.

### 5.3 What it would cost

- **Most of a Phazite song has no place in it.** Phazite's own devices (each would be a `Device` with a private state
  file), containers with their chains, splits and macros, modulators, the chord lane, sections with their roles, outlines,
  drum pads' sounds, sampler slices, frozen tracks, takes and view state. All of it would go into private files inside the
  zip, which other DAWs drop: a private format in a public wrapper, with two ways of saying the same things to keep in step.
- **Versions need IDs that last.** Phazite's differences and merges work because every track, clip, device and note keeps
  one ID from save to save. DAWproject's ids are the file's own (`id25`), any DAW may number them anew, and notes have none.
- **A zip is rewritten whole.** Auto Save writes a second after each change, and a zip of the project with its plug-in
  states would be rewritten each time. Git and text tools can't look inside one, and the store couldn't split it.
- **Its makers don't use it that way.** Bitwig Studio and Studio One keep `.bwproject` and `.song` files, and exchange
  through DAWproject, as Phazite does. It is the usual split elsewhere too: MuseScore's own files, with MusicXML to
  exchange; Blender's `.blend`, with glTF.
- **Pace.** Phazite's schema changes every week while it is pre-alpha; DAWproject's has stayed at 1.0. Phazite would wait
  for it, or extend it privately.

### 5.4 The answer

Keep `.phazite` as Phazite's own format and DAWproject as the bridge to other DAWs, as
[versions.md](versions.md#dawproject-with-versions) decided. The advantage DAWproject would bring, an open format another
tool can read, `.phazite` can have through the plan's items 3, 4, 5 and 8: standard encodings, the same plug-in bytes as a
DAWproject export, choices by name, and a written specification. DAWproject then carries what other DAWs understand, and
`.phazite` all of the song, documented.

## 6. The plan

Now, while Phazite is pre-alpha (none needs the owner's word on compatibility):

1. **The store's small fixes** (S): the index checked (R1), the size bounded (R2), unknown fields kept (R6), the
   fallback branch (R10), and the merge base chosen by the graph (R7).
2. **Plug-in changes as designed** (S-M): a state counts as changed only after a change the plug-in reported or a knob
   moved; a conflict names the knobs; each plug-in's version is kept beside its state (section 2.3, items 3 to 5).
3. **Plug-in states in their formats' own containers** (M): `.vstpreset` for a VST3, standard base 64, and a plug-in
   found by its class ID (P2, F1, F2). This changes what is saved, and the golden files, as the rule of no backward
   compatibility allows; the commit says why.
4. **One canonical text for values** (S-M): numbers, the characters XML forbids, and the project file written by the
   pieces' writer (P3, P4, F4).
5. **Choices saved by name** (S-M, F3).
6. **Recovery** (M): Verify History, a commit with a damaged piece restored in part, commits nothing names found again, a
   log of each branch's commits, and a backup offered when a project file won't open (R3, R4, F6).
7. **Packs joined in the background** (M, R5).
8. **The specifications** (M): `docs/versions-format.md` and `docs/project-format.md` with test vectors and the outside
   reader as their check (P1, F7), and versions.md's five sentences put right (section 3.5).

Before two computers share a history (backup to a synced folder, or the cloud): R8 and R9.

When compatibility starts, on the owner's word: freeze and version the store's format first (P5, P6, P8), then the project
file's (F7), with the license of the specifications decided.

Later, to make the store a concept others reuse: the song's rules as data (P7).

## 7. What was built

The same day, on the owner's word, with the owner's answers: compatibility doesn't start yet, so nothing names the
formats or their versions; and the specifications stay under the AGPL, each saying that implementing its format needs no
permission. Everything else in the plan was built, R8 and R9 and the rules as data (P7) with it, and the tests and golden
files changed with it.

| Finding | Now |
|---|---|
| R1 | A pack whose index doesn't hash to its name is left out (`PieceStore::readIndex()`). |
| R2 | An entry's size is checked against what zlib could make of its bytes, and an allocation that fails is a damaged piece, not a crash. |
| R3 | A version with damaged pieces is brought back in part: what can't be read stays as the song has it, and the window says what was left out (`joinSong()`'s stand-ins, `ApplySong`). |
| R4 | Each computer keeps a log of each branch's moves (`local/<computer>/logs/`, as git's reflog). **Verify History** reads every piece and every version's song, in the background, and says what is damaged and which versions in the packs no branch's line holds; **Recover Lost Commits** makes a branch of each line of those. |
| R5 | Packs are joined into one in the background as the history opens, once there are more than 50, the joined pack written whole before the others go, and a pack with a damaged piece kept. |
| R6 | A branch's record keeps the fields this build doesn't know, and writes them back. |
| R7 | The merge base is the common version nearest both lines, by the graph (git's `merge-base`), not by the clock. |
| R8 | Each computer writes its own record of each branch (`branches/<branch>/<computer>.xml`), and a computer is known by an ID made at random, never its name. The records are taken together as the history opens: a branch's last version is the one that follows every computer's; where two computers committed on one branch, it is this computer's, and the Versions view names the other line's last version until a merge joins them. |
| R9 | The IDs a merge gives anew are kept with its version (`RENUMBERED`), and a later merge from the same line reads the other side through them, so that what it brought in already is the same items, not new ones. |
| R10 | Where `current` names no branch, the branch made first (by its oldest version) is taken, and its last version's song is what the changes are listed against. |
| P1, F7 | [versions-format.md](versions-format.md) and [project-format.md](project-format.md), with test vectors (`tests/golden/format/`: a pack and what it holds, the song's rules and every device's parameters) and an outside reader of under 100 lines of Perl (`tools/read-versions.pl`) that CI runs on them. |
| P2, F1, F8 | A VST3's state is its preset file, a `.vstpreset` as the VST3 SDK writes it (`src/sandbox/core/Vst3State.h`), in standard base 64, in the song, its versions and its presets alike. Other formats keep JUCE's bytes, in standard base 64. |
| P3, P4, F4 | One text for every value, the same in the project file and the pieces (`SongText.h`), whatever type holds it; no characters XML can't hold; the project file in UTF-8. |
| P7 | The song's rules are data: Schema.h's tables, written out as `tests/golden/format/song-rules.json`. |
| F2 | A plug-in keeps its format's ID (a VST3's class ID) and its version beside its state, and is found by its format, number and maker before its file's path. |
| F3 | Choices are saved by their names, in songs and presets, and a device's description says each choice's name. |
| F6 | A project file that won't open offers the newest backup that reads, or the last commit's song, opened in its place as an unsaved change, which a save writes into the project's file. |
| Section 2.3, items 3 to 5 | A plug-in's bytes count as changed only after a change it reported, or a knob moved (each device's reports counted); its version is kept, so the Changes list says "updated 1.36 → 1.37"; and its clash names the knobs whose values differ, and its versions. |

What waits:
- **On the owner's word that compatibility starts:** a file naming the store's format and version, the hash named in it
  (P5, P6), room for a pack's entries to grow (P8), and a format name and version on `PHAZITE` (F7).
- **Not built:** checking a song's values against their ranges as it opens (F5, medium); merging two edits inside one
  plug-in (section 2.3, item 1, offered, never done by itself); keeping the files a plug-in loads by itself (item 2,
  which nothing outside the plug-in can know); the knob values of a plug-in that wasn't playing as a version was saved
  (item 6); and the full paths that can carry the user's name into a project and its versions (R10, F2): an audio file
  whose copy into the project failed, and a plug-in's file in a per-user folder, which the song still keeps to find the
  plug-in on the computer it was added on, though it is found by its format's ID first now.

## 8. Sources

- DAWproject: [README](https://github.com/bitwig/dawproject/blob/main/README.md),
  [Project.xsd](https://github.com/bitwig/dawproject/blob/main/Project.xsd), and the state of a
  [device](https://github.com/bitwig/dawproject/blob/main/src/main/java/com/bitwig/dawproject/device/Device.java) and of a
  [VST3 plug-in](https://github.com/bitwig/dawproject/blob/main/src/main/java/com/bitwig/dawproject/device/Vst3Plugin.java),
  read on 2026-11-09.
- JUCE 9.0.5 (`external/JUCE/modules/`): a VST3's state, `getStateInformation()` and `getPreset()` in
  `juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h`; `copyXmlToBinary()` in
  `juce_audio_processors_headless/processors/juce_AudioProcessor.cpp`; `MemoryBlock::toBase64Encoding()` in
  `juce_core/memory/juce_MemoryBlock.cpp`; numbers as text, `serialiseDouble()`, in `juce_core/text/juce_String.cpp`; XML's
  escaping, `escapeIllegalXmlChars()`, in `juce_core/xml/juce_XmlElement.cpp`; files replaced and flushed, `replaceFileIn()`
  and `flushInternal()`, in `juce_core/files/juce_File.cpp`, `juce_core/native/juce_Files_windows.cpp` and
  `juce_core/native/juce_SharedCode_posix.h`; the VST3 SDK's preset file in
  `juce_audio_processors_headless/format_types/VST3_SDK/public.sdk/source/vst/vstpresetfile.h`.
- Git: [merge-base](https://git-scm.com/docs/git-merge-base), [reflog](https://git-scm.com/docs/git-reflog),
  [fsck](https://git-scm.com/docs/git-fsck), [config](https://git-scm.com/docs/git-config) (`core.repositoryFormatVersion`,
  `extensions.objectFormat`), [the move to SHA-256](https://git-scm.com/docs/hash-function-transition).
- [XML 1.0, its characters](https://www.w3.org/TR/xml/#charsets); [RFC 4648](https://www.rfc-editor.org/rfc/rfc4648), base 64;
  [RFC 8785](https://www.rfc-editor.org/rfc/rfc8785), canonical JSON and its numbers.
- [Irmin](https://github.com/mirage/irmin); Dolt, as cited in [versions.md](versions.md#what-others-tried).
