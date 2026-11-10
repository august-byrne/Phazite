# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread in code Phazite can't change (JUCE's) are in
[realtime-audit.md](realtime-audit.md) instead.

## 10. A take recorded from before a looped range lands over the range, playing what was played before it

- **How it shows:** with bars 5 to 9 looped and the playhead at bar 1, Record, then Stop at bar 7 before going round:
  one clip over bars 5 to 9, "Take 1/2", that plays what was played at bars 1 to 5, and nothing at bars 1 to 4. Stopped
  at bar 3, the clip lands at bars 5 to 7. MIDI notes played before the range all land on its first beat.
- **Why:** `placeTake()` (`src/model/transport/AudioRecorder.cpp`) counts the times round from the range's start, so
  what came before it is time round -1; but the engine plays on from bar 1 to the range's end before it first goes
  round (`Engine::process()`), as `Take::fold()` knows. For MIDI, `Take::collectNotes()` starts what was played at the
  range's start, and `addNote()` moves earlier notes up to the clip's start.
- **How to fix:** count the first time round from where the take starts to the range's end, as `fold()` does, and give
  what came before the range a clip of its own (audio and MIDI; `played.from` the earlier of the take's start and the
  range's). Ask the owner what such a take shows: Ableton Live keeps the part before the loop as a clip of its own.

## 11. Audio clips that are warped, or follow tempo changes, play at the wrong speed where a beat isn't a quarter note

- **How it shows:** in 6/8 (or 3/8, 2/2), a loop that follows the tempo plays at the right speed until it gets a warp
  marker, Quantize Audio or a tempo change under it; then it plays twice as fast in x/8 (half as fast in x/2), and
  lasts half as long.
- **Why:** `ClipTime::beatAt()`, `timeAt()` and `getStraightFor()` (`src/model/arrangement/AudioClips.cpp`) count the
  file's tempo in quarter notes (`tempo / 60.0` beats a second), but a song's beat is its time signature's note
  (`TempoMap`: `240 / denominator`); the plain path, the ratio of the file's tempo to the song's, counts no beats and is
  right. MIDI files' quarter notes are taken as song beats too (`src/model/arrangement/Import.cpp`), which may have the
  same cause.
- **How to fix:** turn the file's quarter notes into song beats by the time signatures under the clip (denominator / 4
  in each) in those three functions; decide with the owner whether a beat stays the time signature's note, as the tempo
  map has it, and make MIDI import, the grid and the tempo lane agree.

## 12. A take recorded while its writing thread stalls plays early after the stall

- **How it shows:** recording to a drive that stalls for more than the take's 8 s buffer (`bufferSeconds`,
  `src/model/transport/Take.cpp`), what was recorded after the stall plays early by as long as was lost, without a
  word.
- **Why:** `Recording::Input::write()` (`src/engine/core/Recording.cpp`) drops a block when its buffer is full and sets
  `overflowed`, which nothing reads, while the take's length counts the block; `Take::finish()` lays the frames it got
  end to end from the take's start.
- **How to fix:** have the audio thread note where frames were dropped and how many (a small lock-free queue), and the
  writing thread write silence there; and tell the user that the take has a gap.
