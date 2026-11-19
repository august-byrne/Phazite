# The browser

*Design section 11.*

The browser is the view of the left panel that finds sounds, presets, instruments and effects, plays a sound before
it goes into the song, and puts things into the song. The plug-in picker (F8) is its quick way to add a device by
name.

## Status

| Feature | State | Note |
| --- | --- | --- |
| A tree of places: favorites, recent items, collections, sounds, devices with their presets, track presets, the user's folders | Built | |
| Search as you type, best matches first | Built | |
| Tags from names and folders: type, loop or one-shot, tempo, key | Built | |
| Keys and tempos from loop data in WAV and AIFF files | Built | |
| Filters: Type, Loop, Tempo, Key, with counts | Built | |
| Preview by click or arrow keys; loops in time with the song | Built | |
| The selected sound's waveform and facts under the tree | Built | |
| Loops previewed in the song's key | Planned | After v1, with the analysis below |
| Previews through the selected track's devices | Planned | No roadmap step; Swap does it for a pad or a device |
| A device's presets heard before they go into the song: an instrument's with a phrase, an effect's on a loop | Planned | Roadmap step S5.18 |
| Drag and drop into the playlist, the mixer and a drum pad | Built | |
| Enter or a double-click puts an item into the song | Built | |
| Plug-in picker (F8) | Built | |
| Favorites and recent items | Built | |
| Collections, kept by file | Built | |
| Swap: a pad's sound or a device's preset tried in place | Built | Not for plug-ins' presets, wavetables, or a pad's layers after the first |
| Phazite Sounds as an optional download | Built | No in-app download on Linux builds without curl |
| Updating Phazite Sounds when a new version is pinned | Planned | No roadmap step |
| A catalogue of free plug-ins and sound packs, each pinned by its size and SHA-256 | Planned | After v1 ([roadmap](../roadmap.md#after-10)) |
| The user's sounds known by analysis; Show Similar; a pad's next similar sound | Planned | After v1 ([roadmap](../roadmap.md#after-10)) |
| Tempo detection for loops whose names don't say | Planned | After v1, with the analysis |
| Sample maps, recorded previews of plug-ins' presets, ratings | Declined | Divisive, or covered by favorites and collections |

## The tree

The browser is laid out as a file explorer: a title, a search field, the filters, a tree, and under the tree the
selected sound and the preview's controls. The places at the top of the tree, in order:

| Place | Holds |
| --- | --- |
| Favorites | The items starred |
| Recent | The last 50 items put into the song |
| Collections | The user's named sets, each a place of its own, then **New Collection...** |
| Sounds | Phazite Sounds' folders (Drums, Loops...), once downloaded; until then a row that downloads them |
| Instruments | Phazite's instruments and the instrument plug-ins found by scanning, each with its presets |
| Effects | Phazite's effects and the effect plug-ins found by scanning, each with its presets |
| Track Presets | The user's track presets ([devices.md](devices.md)) |
| Other Presets | While it holds any: the user's presets of no device listed |
| The user's folders | Each folder added, as it is on disk |
| Add Folder... | Asks for a folder to add |

- One click opens a folder.
- The folders open when Phazite closes are open again at the next start.
- A folder's right-click menu has **Remove from the Browser**.
- Nothing at the top names Phazite or a pack: Phazite Sounds are sounds to start with, and the presets that come with
  Phazite are presets of its devices. The folders on disk stay where they are (`Documents/Phazite/Packs/Phazite Sounds`,
  the presets next to the app, the user's in `Documents/Phazite/Presets`); this is only what the browser shows.

**Presets under what they are for,** as Ableton Live's browser lists a device's presets under it:

| Preset | Shows under |
| --- | --- |
| A device's | Its row in Instruments or Effects, which opens to them by its chevron |
| An effect container's | **Container**, at the top of Effects, while there are some |
| An instrument container's | **Instrument Container**, at the top of Instruments, while there are some |
| A track's | **Track Presets** |
| One of the user's kept where no row above lists it | **Other Presets** |

- A device's row lists the presets of the folder of its name ("Synth") in Phazite's presets and in the user's as one
  list: their folders first, one of each name (Phazite's kinds of sound, Bass, Lead..., with the user's of that name
  inside), then the presets by name, with no heading or mark between Phazite's and the user's; of one name, the user's
  first. A folder there opens to the same folder of both. Folders are matched without regard to case, in each presets'
  folder as the index has them: a user's `synth` or `Synth/bass` is the Synth's, and its Bass's, on every system.
- A row lists its folder, not the presets' devices: a preset saved (through Save Preset's dialog) into another device's
  folder shows under that folder's device. A plug-in named as one of Phazite's devices ("Reverb") shares its folder,
  and so its presets (docs/known-bugs.md).
- A device's row shows a chevron only while it has presets, as the index counts them (no disk on the message thread):
  a device without any opens to nothing. While the index is first made the rows show none, and gain them once it is in.
  Where the index doesn't hold a presets' folder that is there whole (a library larger than it holds: 50,000 sounds,
  or as many folders), every device's row may open, reads its presets from the disk as it opens, and one without
  presets says so. A device without a folder name has none.
- An open device's row whose count changes (a preset saved from a plug-in's window, or one that came from outside) is
  listed again in place: what is open and selected in it stays.
- A click on the chevron (or Right) opens a device's row; a click on its name selects it. Enter, a double-click and a drag
  put the device in with its defaults; a preset's row puts the preset in.
- The containers' presets are in one folder, `Containers`: those of instrument containers at its top show under
  Instrument Container, the others at its top under Container; a folder in it shows under both.
- **Other Presets,** after Track Presets, shows only while it holds something, as the index has it (no listing to decide
  it): the presets at the top of the user's presets' folder, and each folder there named for no device listed, nor
  `Containers` nor `Tracks` (a plug-in not scanned yet, as after a fresh install or a settings reset, or one removed). It
  opens as a folder does. Once the plug-in is scanned, its folder leaves Other Presets and shows under the plug-in's
  row, with no restart (the scan's list changing tells the library, which tells the browser). Phazite's own presets are
  all in their devices' folders: the test that walks every one of them and the user's of each kind finds each in the
  tree.
- A preset saved (from a device's Presets menu, Save Device Preset or Save Track Preset, a plug-in's window, or a track
  dragged into the browser) is shown selected where it belongs, once the index has it: under its folder's device, a
  container's row, Track Presets or Other Presets.

## Search

Typing in the search field lists what matches across the sounds, the presets, the folders and the devices: every word
typed, in any order, in an item's name, its tags or the names of the folders it is in, as Ableton Live's, Bitwig's and
FL Studio's browsers match. Best matches first: names that start with what was typed, then names with a word that
starts with its first word, then names and tags that hold every word, then items that hold some of the words only in
their folders' names; shorter names first among equals. Folders that match come after everything else, as a way to
what they hold.

- The search runs in the background as each key is typed, so typing never waits for it: each key's search cancels the
  one before, and the list shows the rows it showed until the newest text's results are in; Enter and Down wait for them
  (they are milliseconds away) and act on them. What it looks through for each item (its name, words, tags and
  folders, in lowercase) is made once, as the index is built. Items that rank the same keep the index's order, from
  one key to the next.
- It lists the best 200; where there are more, the last line says how many ("The first 200 of 3,412: type more to
  find fewer").

- Ctrl+F (**Search the Browser**, in Phazite's and Ableton Live's keymaps) shows the browser and puts the cursor in
  the search field. Down or Enter moves from the field to the results.
- Showing the browser by its icon gives the keys to the browser itself, not to the search field, so that Space still
  plays the song while the browser is looked through.
- A background thread indexes the sounds, first by their names, then again with what their loop data say. It indexes
  up to 50,000 sounds; a larger library is searched as far as that, and the results say so. It follows a folder that
  links elsewhere, but not one that links back into a folder it is already in.

## Tags

A sound's tags come from the words in its name and in the names of its folders:

- **what it is:** Kick, Snare, Clap, Hat, Cymbal, Tom, Percussion, Bass, FX, Foley, Vocal, Synth;
- **Loop or One-shot;**
- **its tempo** ("120 BPM");
- **its key** ("F# minor", or a note alone for a one-shot tuned to it).

Search finds tags, so "kick loop" finds a kick loop in a folder called Loops.

**Keys from names.** A key is read as sample packs write it: a note with a sharp or a flat and "m", "min", "minor",
"maj" or "major" after it or as the next word ("Am", "C#min", "F# Minor", "Cmaj7"). A note alone counts only where
the name sets it apart: as its last word ("808 Long F"), in brackets ("Kick (F)"), with a sharp or a flat, or after
"key" ("Key of G"). A capital M is never a minor ("FM Bass"), nor is a word that merely starts with a note. A folder
named for a key with minor or major ("Loops/F# Minor") gives it to the sounds in it whose names give none. A note
alone on a drum's variant ("Kick A") can be taken for its key.

**Loop data.** A WAV file's acid chunk names a root note; an AIFF file's Apple loop data name a root with minor or
major. Loop data also make a sound a loop or a one-shot whatever its name says, and give a loop its tempo from its
beats and length. The index reads loop data on its own thread, after it has listed every sound by name, and keeps
what it read in `Library facts.txt` beside the settings, by each file's path, size and time, so that a later start
reads only what is new or changed.

**Opening a folder.** The window never waits for the disk as a place or a folder opens, and a folder shows what it holds
now, as every DAW's browser does. Phazite Sounds, the user's folders and the presets' folders are what the index walks:
once it has walked them to the end, their folders open from the index, at once, and are read from the disk all the
same, on a thread of their own. Where the folder differs (a file added, removed or renamed from Explorer, or bounced
into it by another program), its rows are brought up to date in place (what is selected, what is open and where the
list is scrolled stay; nothing is remade where nothing differs), and the index is patched for that folder by its own
rules, so that search and the filters find what is there. While the index is first made, or in a library larger than
it holds (50,000 sounds), a folder is read on a thread of its own alone: its row says "Reading this folder..." until its
rows are in. Down, Up or Enter pressed on that folder meanwhile act on its rows once they are in, if they come within a
moment and nothing else was done first (a click, another key, text typed); else the key is dropped, never run later on
what is selected then. Nothing else waits for it: the search, the filters, other folders and their keys go on.

A drive that doesn't answer (a network drive asleep) takes one read thread and no more: a folder's reads are joined
(opened again, looked at again, it waits for the read in flight), and each root (Phazite Sounds, each of the user's
folders, each presets' folder) has one read in flight at a time, until that read has waited on the system for three
seconds: then the root's next folder is read beside it (a link within the root to a drive asleep holds up only its own
folder; a file's header that hangs counts the same). While every read thread busy waits on the system that long (the
four at least), a spare thread takes the next read, up to one for each root of the reads out and one more; a folder
whose read has waited on the system, or to start, for those three seconds says "This drive isn't answering..." in place
of "Reading this folder...". An indexed folder keeps showing the index's rows meanwhile; closing it lets go of what it
would have read. A folder's rows come from what is known of its files (the index, the loop data read before, their
names) and its presets' headers (small files: a file that isn't a preset Phazite reads is left out); where the index
holds the folder and it changed, the new sounds' headers (their loop data) are read after the rows are shown, one at a
time, as the index is patched, and the rows change again only if those make a difference. The library's going waits for
none of them.
Folders that were open within a folder still being read (as the browser restores what was open at the start, after a
swap or a filter) open again once its rows are in, as does the row that was selected, unless the user selected another
meanwhile.

A list of presets (a device's row, a container's, Track Presets, or a folder in them) is two folders read as one: the
same folder in the user's presets and in Phazite's. Each is listed as a folder is, and the list shows what they hold
together once both are in: at once where the index knows both (holds it whole, or knows that it isn't there because
its presets' folder wasn't there as it walked, as on a fresh install), else as each read comes. A folder that isn't
there and one the index has with nothing in it are the same, so that a device without a folder of the user's isn't
listed again at each opening or look. Where one folder is late (a user's presets' folder on a drive that doesn't
answer), the list shows what the other gave, with "This drive isn't answering..." under it, and the rest when it comes.
Closing the row, or the browser going, lets go of both reads: nothing comes after.

A folder already open when what it holds changes is read again as the browser takes the keys again, or as Phazite comes
back to the front (from Explorer, or the program that wrote into the folder), no more than 16 at a time (those in view
first), and the next time it opens. Coming back to the front reads again those in view whatever their last read (the
user may have changed the folder seconds after looking at it); the keys taken again within Phazite, and folders out of
view, at most once in five seconds. The index takes a read's patch only where it listed the folder after the walk did,
so that a walk's older listing doesn't bring back a file deleted since, nor a patch's older listing the walk's.
No folder is watched: a change from outside comes from the user working outside Phazite, who looks at the browser
again by coming back to it, which is when it is read; and watching every folder of a large library, network drives
among them, costs a handle for each and isn't reliable on network shares. A folder that came from outside (a new folder
of samples) is walked by the index as a whole; a folder that leads back to one it is in (a link) is never patched
through. A root inside another (a user's folder within Phazite Sounds) is walked once, with the root it is in, whose
folders' words its sounds' tags take, read again or walked.

## Filters

Under the search field is a chip for each of **Type**, **Loop**, **Tempo** and **Key**.

- A chip opens a menu of the values that what the browser shows has, as the other filters and the search's words
  leave it, each with how many sounds have it, the one set ticked. So a value chosen never leaves nothing. The counts
  are made in the background, as the search is, when the chip is clicked; the menu opens once they are in (at once in
  a library of tens of thousands of sounds), never with the counts of what showed before: words typed or a filter set
  meanwhile cancel it.
- A chip that is set shows its value in the accent's wash; its x takes it away.
- **Clear**, there while a filter is set or the search has words, takes all of them away.
- Nothing keeps the filters from one start to the next.

| Filter | Offers |
| --- | --- |
| Type | What a sound is (Kick, Snare...), from every library |
| Loop | Loops, or one-shots |
| Tempo | The tempos the sounds' tags give, from the slowest |
| Key | The keys, from C: each root alone (a tuned one-shot), then major, then minor |

While any filter is set:

- each place shows how many of its sounds have the filters' values, and lists them as one list, its folders' sounds
  within it; places with none don't show. The places are listed in the background, as the search is, at each filter
  set and each change of the index; the tree shows the places it showed until the newest filters' are in, whole, and a
  key that moves or Enter pressed meanwhile acts on those once they are;
- a list shows 2,000 sounds at most, then says how many more there are;
- search results take the filters too;
- MIDI files take the filters as sounds do; folders, devices and presets have none of their values, so they don't
  show, nor do Instruments, Effects and Track Presets;
- the folders that were open before open again once the filters are taken away.

## Preview

| Gesture | What it does |
| --- | --- |
| Click a sound, or move to it with the arrow keys | Plays it |
| Click a loop that is playing | Stops it |
| Click a one-shot again | Plays it again from its start |
| Esc | Stops the preview |
| Start or stop the song | Stops the preview: the transport plays the song |

- The headphones switch under the tree turns previews off; the fader beside it sets how loud they play (a
  double-click sets -6 dB).
- **Loops play in time** with the project's tempo, stretched as their clips would be, by Rubber Band's R3 engine.
  While the song plays, a loop started then plays on the song's bars, from where the song is in it.
- **One-shots play as they are,** never stretched, at their own pitch.
- Phazite knows a loop by its loop data, or by a tempo in its name ("Break 120 BPM") when it lasts whole bars at that
  tempo. An effect made for some bars ("Riser 4 Bars 128 BPM") plays in time, but once.
- Loops keep their own pitch.
- MIDI files play on Phazite's synth, in time, looping over whole bars.

**The selected sound,** under the tree: its waveform, and a line of what it is: a loop, at what tempo and how many
bars, or a one-shot; how long it lasts; its sample rate, bits and channels.

## Putting things into the song

**Drag and drop:**

| Dropped on | What happens |
| --- | --- |
| Empty space below the tracks | A new track, with the sound as a clip or the device on it |
| A track | A sound becomes a clip at the grid line before the pointer; a device goes on the track |
| A mixer channel | A device goes on the channel |
| The master | An effect goes on the master |
| One of the drum sampler's pads, in its editor | The sound becomes the pad's sound |

An instrument dropped on a track that can't hold one (a folder, a return, a track with audio clips) gets a new track
next to it, named and coloured after it. A track dragged from the playlist into the browser is saved as a track
preset ([devices.md](devices.md)).

**Enter or a double-click** puts a sound on the selected track at the play start (where Space plays from), or on a
new track; a device or a preset goes onto the selected track.

## The plug-in picker

F8 (**Add Instrument or Effect**) opens a small search over the devices: Phazite's own and the plug-ins found by
scanning. Type part of an instrument's or effect's name or its maker's, use the arrow keys, and press Enter (or
click one) to add it to the selected track, or to a new one. Before anything is typed it lists instruments, then
effects, Phazite's own first in each; once something is typed, the best matches first. Esc or a click elsewhere
closes it.

F8 is the picker in every keymap. Showing and hiding the left panel is Ctrl+Alt+B in Phazite's and Ableton Live's
keymaps and Alt+F8 in FL Studio's.

## Favorites, recent items and collections

- **Favorites:** the star at the end of a row (shown on the selected row, and filled on favorites), or **Add to
  Favorites** in the right-click menu.
- **Recent** lists the last 50 items put into the song.
- **Collections** are named sets of sounds, presets, folders and devices. Right-click an item, **Add to
  Collection**: the collections are ticked where the item is in them (choosing one again takes it out), and **New
  Collection...** names a new one in a field over the tree. A collection's menu has **Rename...** and **Delete
  Collection**, which asks first; its sounds stay where they are on disk.
- **Kept by file:** a sound, MIDI file or preset is known by its size and the SHA-256 of its first 64 KiB besides its
  path, so one renamed or moved stays in its collections and among the favorites, found again in the folder it was
  in, or anywhere in the library. The index looks for them on its own thread as it is built; a listing that misses
  one builds the index again, which shows it once found. The sets are kept in the settings.

## Swap

Swap tries the browser's items in place in the song, heard in the beat and through the track's devices.

- **On a drum pad:** the **Swap** button beside Reset in the drum sampler's editor.
- **On one of Phazite's own devices:** **Swap: Try Them in Place**, the first item of its Presets menu.

While a swap is open:

- the browser shows, with a strip over its search that says what is swapped and what is in its place now, with
  **Keep** and **Put Back**, and the browser has the keys;
- each sound (for a pad) or preset of the device's type (for a device) the browser moves to, by the arrow keys or a
  click, plays in that place: in the beat while the song plays; while the song is stopped the pad plays once, or an
  instrument plays a short note;
- a pad's swap shows the sounds of its sound's type (a kick's: kicks), by the filters;
- a device's swap shows its presets alone, as its row lists them (Phazite's and the user's as one), every folder of
  them open, from the top, so that Down tries one after another; the filters are hidden;
- previews stay silent, and what can't be tried shows muted;
- the pad being swapped is outlined and its Swap button lit; a click on it keeps what is tried.

| Action | What it does |
| --- | --- |
| Enter, a double-click or Keep | Keeps what is tried as one undo step ("Put Kick 03 on Pad 36", "Load Preset Warm Pad"); a sound is copied into the project only then |
| Esc or Put Back | Puts the old one back, leaving nothing to redo |

Once the swap ends the browser is as it was before: its search, its filters, its open folders, the selected row and
how far it was scrolled.

**Anything else that changes the song meanwhile keeps what is tried, as a step of its own, and ends the swap:**
another edit, a drag, an undo (which then takes it back, as a step that can be redone), a save, another swap, or the
browser hidden. So the next change is never mixed into the swap's step, and what is heard is what stays. Auto Save
waits while a swap has tried something.

## Phazite Sounds

Phazite Sounds are 85 free sounds for a new user to start with: acoustic and electronic drums, 808s, loops from 90
to 174 BPM, risers, impacts and other effects, and foley.

- **An optional download, not part of Phazite.** They live in their own public repository,
  [phazite-sounds](https://github.com/august-byrne/phazite-sounds), whose releases hold the pack as a zip, so
  Phazite's repository and installer stay small. Phazite offers them once, in the welcome window
  ([getting-started.md](getting-started.md)); the browser's Sounds has a row that downloads them until they are
  installed, then lists their folders; **Help > Phazite Sounds** offers them again.
- **Download:** only when the user asks, over HTTPS, on a thread of its own, with progress in the browser and the
  welcome window. Phazite pins the release it installs by its address, size and SHA-256, so a changed or damaged file
  is refused. A released zip is never replaced: a new release means a new pin in Phazite.
- **Install:** the zip unpacks into `Documents/Phazite/Packs/Phazite Sounds`, whose `sources.json` names the pack and
  its version. A zip downloaded by hand installs the same way (**Install from a File...**), for computers without a
  connection.
- **Linux builds without curl** can't download over HTTPS: there the button opens the release's page in a browser.
- **CC0 only,** so songs made with them carry no conditions. The sounds repository's README has the rules for what
  may go in and how the pack is made; `phazite_soundkit` (`tools/soundkit/`) renders and zips it from the recipes
  there.

## Decisions

- **The tree of places is the backbone;** filters and search narrow it. Tags never replace folders, and the user's
  folders show as they are on disk.
- **Presets show under what they are for,** Phazite's and the user's as one list (the owner, after stage 6): a place
  for "Phazite's" or "yours" made the user look in two places for one device's sounds.
- **Each filter offers only what has results, with counts,** and shows its value on its chip, so a filter is never
  left on unseen. Filters aren't kept between starts for the same reason.
- **One way to find a type's sounds:** the Type filter lists them from every library. There is no place of its own
  for each type.
- **A drum pad has no file dialog:** Swap and drag and drop put sounds on pads, and a folder outside the browser is
  one Add Folder... away. A wavetable oscillator keeps its Load Wavetable File..., as Swap doesn't try tables.
- **The Presets menu is one button on every device,** with Swap as its first item, rather than a button that opens
  Swap for Phazite's devices and a menu for plug-ins.
- **A swap gives way to any other change,** keeping what was tried: what is heard is what stays.
- **Loops preview at their own pitch** until songs' keys can be used well: pulling every loop toward one pitch would
  ignore major and minor.
- **Phazite Sounds are a separate, optional download, CC0 only.** CC BY sounds would make every released song owe
  credits.
- **One-shots are never stretched** in previews; loops always are.

## Not built

- The user's own sounds known by analysis, in the background and paused at will (loop or one-shot, drum type, tempo,
  key, brightness), shown as guesses the user corrects: Planned, after v1.
- Show Similar, and a drum pad's previous and next similar sound with a lock: Planned, after v1.
- Loops previewed in the song's key (relative major and minor counted as one key): Planned, after v1.
- Tempo detection for loops whose names and loop data don't say: Planned, after v1.
- Previews through the selected track's devices: Planned.
- An item dropped onto a device slot replacing the device there: Planned.
- Swap for a plug-in's presets, for wavetables, and for a pad's layers after the first: Planned.
- Updates of Phazite Sounds when a new version is pinned, and downloads on Linux builds without curl: Planned.
- Sample maps, recorded previews of plug-ins' presets, and ratings: Declined.

## In the code

- `src/services/Library.h`: the sounds and presets, the index, tags, search, favorites, recent items and collections.
- `src/services/BrowserModel.h`: the places, what each holds, search results, the filters (`BrowserFilters`,
  `FilterOptions`) and the rows' texts, without the window.
- `src/services/SoundPack.h`: the Phazite Sounds download and its pinned release.
- `src/model/engine/AudioFiles.h`: what Phazite reads of an audio file, and a sound's key (`SoundKey`,
  `getKeyFromName()`).
- `src/model/engine/Preview.h`, `internal/PreviewLoop.h`: previews.
- `src/model/devices/SwapModel.h`: Swap; the undo convention's `Trial` in `src/model/commands/internal/Undo.h`.
- `src/ui/browser/Browser.h`, `FilterBar.h`, `SoundView.h`, `PluginPicker.h`: the view, the filter chips, the selected
  sound and the plug-in picker; `src/ui/input/LibraryDrag.h`: what a dragged item carries.
- `src/ui/playlist/PlaylistDrops.cpp`: what is dropped on the playlist.
- Tests: `tests/BrowserTests.cpp`, `BrowserModelTests.cpp`, `BrowserFilterTests.cpp`, `LibraryTests.cpp`,
  `SoundKeyTests.cpp`, `SwapTests.cpp`, `SoundPackTests.cpp`.
