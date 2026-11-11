# Recording audit

*2026-11-08, the owner's request: "do we currently have a "bounce in place"/"consolidate" feature. if not, we should add
it. also i see people praising ableton's audio clip recording and warping and quantizing capabilities. I want us to also
have these capabilities. i also notice that recording is a little nebulous in phazite on where it is recording ... phazite
also doesn't show the recording populating the playlist live like ableton does. I want an audit of what ableton gets right
for audio recording, and i want it implemented in phazite." Section 5 says what was built from it, and section 6 what is
left.*

The audit compares Phazite with Ableton Live 12.4.16 (30 October 2026, its newest release) on recording, takes and
comping, consolidating and bouncing, warping and quantizing audio. Ableton's behaviour comes from its Live 12 reference
manual and release notes; what users praise and complain about comes from forums and reviews (section 7). FL Studio,
Logic Pro, Studio One, Bitwig and REAPER were checked for one question only: how each shows where a recording goes.

## 1. In short

- **Where a recording goes is shown on every track in Live.** Every track that can record has an arm button, audio and MIDI
  alike; its input is chosen on the track; arming is exclusive at first (Ctrl adds more). Record (F9) records onto every
  armed track and nothing else. Phazite showed an arm button on audio tracks only: what was played on MIDI recorded onto the
  selected track with an instrument, which only the status bar named, and nothing said what Record would record onto.
- **A recording grows in the arrangement as it records.** Live draws the clip in its track's colour from where recording
  started, and draws MIDI notes as they are played (a user's report; the manual says nothing of a live waveform). Phazite
  showed nothing until recording stopped.
- **Consolidate (Ctrl+J)** joins the clips selected, or a time range, into one clip on each track: one MIDI clip of the notes,
  or one new audio file rendered from the clips before the track's effects, with their gain, warping, pitch and fades.
  Phazite had no Consolidate. It had **Bounce in Place**, which renders a whole track through its devices onto a new audio
  track under it, as Live 12.2's Bounce to New Track does for a whole track, and Logic Pro's Bounce in Place and Cubase's
  Render in Place do.
- **Warping is what users praise most.** Warp markers pin a time in a sample to a beat; the audio between two markers
  stretches to fit. Live finds the hits (transients) when a sample loads, shows them as small marks, and turns one into a
  marker as it is dragged. A recording comes in warped at the song's tempo. Phazite could stretch a whole clip to the song's
  tempo from one tempo of its own (Follow tempo), with no markers.
- **Quantizing audio (Ctrl+U)** moves the hit nearest each grid line onto it, by an Amount, through warp markers, and works on
  several clips at once. Phazite quantized notes and clips' starts, not the audio inside a clip.
- **Comping** (Live 11) keeps each pass of a loop recording on a take lane under the track; a range selected on a lane, with
  Enter, goes into the main lane. Phazite keeps the passes as takes of one clip and plays one of them whole.

## 2. What Ableton Live does

### 2.1 Arming and inputs

- Clicking a track's Arm button arms it and disarms every other track (Exclusive Arm, on unless turned off); Ctrl+click
  arms more than one, and C arms the selected tracks [rec], [mix], [keys].
- A MIDI track with an instrument is armed by itself as an instrument goes into it. Arming that follows the selection comes
  from control surfaces (Push), and shows as a pink or light red arm button; a track armed by hand is red [mix], [lightred],
  [pink].
- Each track's In/Out section chooses its input: Audio From and the input channel, or MIDI From (all inputs at first). Its
  monitoring is In (always), Auto (while armed, and not while its clips play) or Off [io].
- Arrangement Record (F9) records onto every armed track. With none armed it records automation and launched clips only; no
  track is made for a microphone [rec], [sess], [auto].
- Punch in and out are the loop brace's ends. The count-in waits before recording starts and shows in blue counting up to
  1.1.1 [rec].
- Recorded files go into the project's Samples/Recorded folder, named after their track ("Guitar 0001.aif"), at the File
  Type and Bit Depth of the settings [rec], [naming].
- What comes in is placed where it was played: by the latency the driver reports, a Driver Error Compensation for drivers
  that report it wrongly (left at 0 unless measured), and with Keep Monitoring Latency in Recorded Audio on for a monitored
  track, by what was heard through Live [dec], [mix].

### 2.2 What shows while recording

- In the Session View each slot that records shows a clip with a red launch button [rec].
- In the Arrangement a recording clip shows solid, in its track's colour, its MIDI notes drawn as they are played
  ([midilive], a user, 2024). The manual doesn't say whether an audio clip's waveform is drawn as it comes in; that is
  unconfirmed.
- Users complain that the piano roll shows nothing while the Arrangement records [pianoroll].

### 2.3 Takes and comping

- Each pass recorded over clips, or round a loop, adds a take lane; the last one is copied to the main lane [comp].
- Ctrl+Alt+U shows the take lanes. A range selected on a lane and Enter copies it into the main lane, joined by 4 ms
  crossfades; Ctrl+Up and Down put the next or previous take there; T auditions a lane. What the main lane plays is lit in the
  track's colour, the rest faded [comp], [keys].
- Reviewers found it "works exceptionally well", with "convenient keyboard shortcuts" [mt11], [sos11]. Users complain that
  every pass is kept and lanes pile up ("a gigantic mess"), and that it is less refined than Logic Pro's take folders and
  Pro Tools' playlists [disable], [lwmr].

### 2.4 Consolidate and bounce

- **Consolidate (Ctrl+J)** "combines selected material from several adjacent clips into one new clip". For audio, "a new
  sample is created for every track in the selection", recorded from the warping engine's output, before the track's effects
  and mixer: clip gain, warping, pitch and clip envelopes are in it, effects aren't. For MIDI it is one MIDI clip. Files go into
  Samples/Processed/Consolidate, dated [arr], [consname].
- **Bounce to New Track (Ctrl+B, 12.2)** renders the selected clips or time range through the track's devices onto a new track
  under it, called "(Bounce)" in the track's colour, and mutes the source clips [bounce].
- **Bounce Track in Place (12.2)** replaces a track with an audio track of its sound, its devices rendered and gone, its
  volume, pan and sends kept; it replaced Freeze and Flatten [bounce].
- **Paste Bounced Audio (Ctrl+Alt+V, 12.3)** pastes the render of what was copied [bounce].

### 2.5 Warping

- Warp markers: a double-click in the upper half of the sample editor adds one, a drag moves it, a double-click or Backspace
  deletes it; Ctrl selects several [warp].
- Live finds a sample's transients as it loads and shows them as small grey marks. Hovering over one shows a pseudo warp
  marker, which becomes a marker when it is dragged or double-clicked [warp].
- Auto-Warp Long Samples is on at first; the default warp mode is Beats. A recording comes in warped, at the song's tempo,
  so that it keeps to the beats when the tempo changes ([warp2009], Ableton staff; [rec]).
- The BPM field sets the clip's tempo, with x2 and /2 beside it; Warp From Here, Set 1.1.1 Here and Warp Selection As... set
  markers from a point; six warp modes (Beats, Tones, Texture, Re-Pitch, Complex, Complex Pro) suit drums, voices, pads,
  turntable speed and whole songs [warp].
- Steinberg's forum, in December 2025: "there is still no comparison to the speed of doing it in Live"; "Live blows Cubase out
  of the water" on warping loops [stein].

### 2.6 Quantizing audio

- Ctrl+U moves "the nearest transient to the closest grid line", to the grid or a chosen division, by an Amount that
  "shifts the Warp Markers by a percentage" [warp]. Since 12.0 several audio clips quantize at once, and within a time
  selection [rn].
- Users complain that every transient found is used, with no sensitivity to leave quiet ones alone [multi2013].

### 2.7 How other DAWs show where a recording goes

- FL Studio arms a mixer track by choosing its input, and its record button can ask what to record (audio into Edison or the
  playlist, notes, automation) [flrec], [flboris].
- Logic Pro arms the selected track by itself when Record is pressed with none armed [logic]; Studio One's Audio Input
  Follows Selection arms and monitors the selected audio track [s1]; REAPER records nothing unarmed, with an option to arm the
  selected track [reaper]; Bitwig's arming can follow the selection [bitwig].
- None makes a new audio track by itself for a microphone; Logic's Create Track makes one for a recording that overlaps a
  region [logicset].

## 3. Phazite before this (2026-11-08)

- **Arming** (step 3.4): an arm button on tracks without an instrument, with its input list (each channel, then each pair)
  and its monitoring; a click arms that track alone, as in FL Studio, Logic Pro, Studio Pro and REAPER. Tracks with an
  instrument had none: MIDI went to the selected track with an instrument, as FL Studio's selected channel, which only the
  status bar showed ("MIDI → Bass").
- **Record** recorded what was played on MIDI onto that track and every armed track's input, as one undo step; the count-in,
  Punch In over the looped range, Overdub and Quantize Recordings were in its menu. Record was greyed out while nothing would
  record, with no hint of what to do.
- **Takes**: each time round a looped range is a take of one clip, "Take 2/3", chosen from its menu or with Next and Previous
  Take. No take lanes or comping.
- **Placement** to the sample: the device's reported delays, the delay compensation and a Recording correction in the audio
  settings, as Ableton's Driver Error Compensation.
- **Freeze** and **Bounce in Place** (step 2.11): a whole track rendered to a file it plays, or onto a new audio track under it
  with the original muted and collapsed. No Consolidate.
- **Follow tempo** (step 2.18, 4.8): a clip stretches with the song's tempo from one tempo of its own, along the song's tempo
  changes, by Rubber Band's R3 engine in the background. No warp markers, transients or audio quantize; recordings came in
  not following the tempo.

## 4. The plan

What Phazite takes, and why, by the owner's principles: one way to do each thing, nothing hidden, research before taste.

| Ableton Live | Phazite before | Phazite now |
|---|---|---|
| An arm button on every track that records, audio and MIDI | Audio tracks only | Every track that records: an audio track's records its input; an instrument track's is lit on the track live MIDI plays, as Live's auto-arm, a click on another's sends MIDI there, and on the lit one stops it recording there |
| The input shown on the track | "In 1" under an armed track's name | The same, and "MIDI" under the track MIDI records onto; red, "Recording In 1", while it records |
| Record records onto the armed tracks | Unnamed; greyed out with nothing to record | Its tooltip names what it records onto ("Lead (MIDI) and Vocals (In 1)"); with nothing to record, a click opens its menu, which offers Record an Input on a New Track |
| A recording clip grows as it records | Nothing until recording stopped | Each track that records shows a clip growing with the playhead, its edge red, an input's waveform drawn as it comes in and the notes as they are played; round a loop, the take it is on |
| Consolidate (Ctrl+J) | None | Consolidate: the selected clips, or the time range, become one clip on each track, a MIDI clip of the notes or an audio file rendered before the track's effects |
| Bounce to New Track on a selection | Whole tracks only | Kept as Bounce in Place, which other DAWs mean by the name |
| Recordings come in warped | Not following the tempo | Recordings follow the song's tempo from the tempo they were played at |
| Warp markers, transients, pseudo markers | None | Warp markers in the audio clip editor, the transients found in the background, a mark under the pointer that becomes a marker as it is dragged |
| Quantize audio by an Amount | None | Quantize Audio: the hit nearest each grid line moves to it, by the quantize strength, through warp markers |
| Take lanes and comping | Takes of one clip | Section 6 |

## 5. What was built (2026-11-08)

### 5.1 Where a recording goes

- **An arm switch on every track that records,** beside its mute and solo in the playlist header and the mixer strip
  (`src/ui/components/ArmButton.h`).
  - A track without an instrument arms its input, as before.
  - A track with an instrument has one too. It is lit on the track live MIDI plays (the selected track with an instrument), as
    Live's auto-arm lights the selected MIDI track.
  - A click on another instrument track's switch sends MIDI there, armed. A click on the lit one stops that track recording
    MIDI; it still plays. It records again when clicked, or when MIDI moves to another track (`setMidiArmed()` in
    `src/model/transport/RecordingModel.h`). This setting belongs to the session and isn't saved.
- **What a track records shows under its name:** "In 1", "In 1+2" or "MIDI". While Record records onto it, the text turns red
  and reads "Recording In 1". The mixer strip says "Armed: MIDI".
- **Record names what it records onto.** Its tooltip reads "Record onto Lead (MIDI) and Vocals (In 1)..."
  (`describeRecordTargets()`).
- **Record is no longer greyed out when nothing would record.** A click then opens its menu, headed "Nothing records yet".
  - The menu always has **Record an Input on a New Track**, listing the device's inputs, the one chosen last first.
  - Choosing an input adds a track called "Audio" ("Audio 2", and so on) after the selected track. The new track records that
    input, is armed and selected, all as one undo step, "Add Audio Track" (`addTrackForInput()`).
  - This follows FL Studio's record button, which asks what to record. No DAW checked makes a track for a microphone by itself.
- **Kept as they were, on the research:**
  - Arming stays per track rather than exclusive (step 3.4's choice, with FL Studio, Logic Pro, Studio Pro and REAPER).
  - MIDI still follows the selection (FL Studio, and Live's auto-arm).
  - A new audio track still starts without an input (Bitwig).

### 5.2 Recordings as they come in

- **A clip grows on each track that records** (`src/ui/playlist/PlaylistRecording.cpp`, from `getRecordingsInProgress()`).
  - It is drawn in its track's colour with a red edge, tagged "Recording", or "Recording take 2" round a loop.
  - It starts where recording started. Round a loop it starts where this time round began; punched in, at the punch's start.
  - Round a loop that recording started before, once round, the part before the loop shows beside it, whole
    (2026-11-10).
  - Its end follows the playhead's line at each frame of the display.
- **An input's waveform draws as it comes in.**
  - The take's writing thread keeps the loudest sample of every 256 frames beside the file it writes.
  - The writing thread and the message thread share those levels under a lock. The audio thread never touches them, so it
    takes no lock and allocates nothing.
- **MIDI notes draw as they are played,** a held note growing. Round a loop they fold in as they will be kept.
  - The clip shows from the first note played. An instrument track that only follows the selection while a vocal records
    therefore shows nothing.
- **When recording stops,** the take's clips take its place.
- **Live is only documented to draw MIDI live.** No source confirms it draws an input's waveform as it records (section 2.2),
  so Phazite does more here.

### 5.3 Recordings follow the tempo

- **A recording comes in following the song's tempo,** from the tempo it was played at, as Live warps a recording at the
  song's tempo. It then keeps to its beats when the tempo changes later.
- **A recording made where the tempo changes keeps its own speed,** since it was played along those changes.
- **Nothing is rendered until the tempo changes.**

### 5.4 Consolidate

- **Consolidate** (`clip.consolidate`: Ctrl+J in Phazite's and Live's keymaps, the clip menu and the Edit menu) joins clips into
  one clip on each track (`src/model/arrangement/internal/Consolidate.h`, `src/model/mixer/FreezeModel.h`). It takes either
  the selected clips on each track, from the first one's start to the last one's end, or what lies in the time range selected
  across tracks.
  - **MIDI** becomes one MIDI clip at once, without a render. It holds the notes as they play, cut at their clips' and the
    range's edges, and their controller events.
  - **Audio** is rendered by Phazite's engine from the track's audio clips there, before its devices. The clips' gain, fades,
    pitch, stretch and warp markers are in the render.
    - It renders in the background with a progress window, into "*track* (Consolidated).wav" in the project's Audio folder.
    - A render job can now start partway into the song (`OfflineRender::Job::startSeconds`).
    - The new clip follows the tempo if every clip it replaces did and the tempo holds steady over it.
  - It is one undo step, "Consolidate". The new clips are selected, or the time range stays over them.
  - A frozen track plays live again first, since its frozen file would hold its devices.
- **Bounce in Place stays as it was.** It does what Logic Pro's Bounce in Place and Cubase's Render in Place do, and what
  Live 12.2's Bounce to New Track does for a whole track. Its search words no longer include "consolidate".

### 5.5 Warping

- **Warp markers on audio clips** (`schema::WARP`).
  - Each marker is a time of the file, saved with where that time plays at the clip's tempo. A marker therefore keeps its beat
    when the clip's tempo changes.
  - An alias shares its clip's markers.
- **How the audio plays with markers.**
  - Between two markers the audio stretches to fit. Before the first marker and after the last, it plays straight.
  - The clip's start keeps its audio.
  - Rubber Band R3 renders it along a time map, by the same path that follows the song's tempo changes
    (`src/model/arrangement/internal/AudioClips.h`).
- **Warp markers and Follow tempo.** A clip with markers follows the tempo. Turning Follow tempo off removes them.
- **The hits in a file are found once,** in the background, by the sampler's onset detection
  (`src/model/arrangement/internal/Transients.h`).
- **The audio clip editor has a warp strip along its top.**
  - The hits show as small ticks.
  - Markers show as flags, in the palette's warning colour (Live's markers are yellow), each with a line down the audio.
  - A double-click pins the audio there with a marker.
  - A marker drags to the grid (with Alt, freely), and the audio either side stretches to fit. The view holds still until the
    drag ends.
  - A hit under the pointer shows the marker a drag would make of it.
  - A double-click on a marker removes it.
  - A right-click offers Remove Warp Marker, Quantize Audio and Remove Warp Markers.
- **The playlist draws a warped clip's waveform** one stretch at a time (`src/app/ClipWaveforms.cpp`).

### 5.6 Quantizing audio

- **Quantize Audio** moves each hit a clip plays to the grid line nearest it, through a warp marker of its own.
  - In the playlist it is `clip.quantizeAudio`, on the clip menu, and uses the playlist's grid.
  - In the audio clip editor it is Quantize (edit.quantize, Ctrl+U), and uses the editor's grid, as Live's Ctrl+U does in its
    sample editor.
- **What stays put:**
  - The clip's start holds its audio.
  - Markers already there that no hit replaces stay.
  - A hit that would cross the one before it stays where it is.
- **It is one undo step.** `quantizeAudio()` takes a strength; the commands use all of it, as Live's Amount at 100 %.

### 5.7 Tests

- `tests/AudioRecordingTests.cpp`:
  - An input's growing range, the take it is on round a loop and the punch.
  - Takes from before a looped range (stopped before it, in it, round once and twice, a moment before it) and from
    within it (stopped in the first, second and third time round: the partial first one a take after silence, and the
    take shown while recording the finished clip's last).
  - A writing thread that stalls (caught up, still stalled at Stop, round a loop, before a range then gone round), and
    its message.
  - Its waveform's levels: the metronome's clicks where they were played.
  - The new track for an input.
- `tests/RecordingTests.cpp`: notes shown as they are played, held and let go, folded round a loop; notes from before a
  looped range, with and without Overdub, and what shows meanwhile.
- `tests/PlaylistTests.cpp`:
  - The recording clip on its row, from the start to the playhead, growing, and replaced by the take's clip.
  - The part before a looped range beside this time round.
  - The instrument track's arm switch and its "MIDI" label.
- `tests/ConsolidateTests.cpp`:
  - MIDI clips joined, their notes cut as they played.
  - Audio clips rendered without their track's devices, with their gain.
  - A time range across tracks.
  - Nothing selected.
- `tests/WarpTests.cpp`:
  - A marker added and moved, its neighbours where they should be.
  - Each hit rendered where it now plays, within 2.5 ms.
  - The clip's tempo and Follow tempo with markers.
  - Hits found in the background.
  - Quantize Audio at full and half strength, with the hits rendered on their beats.
- `tests/AudioClipEditorTests.cpp`: a marker added by a double-click, dragged and removed, each one undo step.
- `tests/EngineTests.cpp`: an input's gaps read as silence in place.
- `tests/RealtimeAuditTests.cpp`: noting gaps allocates nothing on the audio thread.

### 5.8 Since: every time round a take, and a drive that stalls (2026-11-10)

- **A take round a looped range keeps everything played** (known bug 10).
  - Started before the range, the part before it is a clip of its own, as Bitwig and Live keep it, and the range's
    clip has a take for each time round from its start.
  - Started within the range, the first, partial time round is a take too, as Logic Pro and Bitwig keep a partial
    first pass: its file starts with silence from the range's start, so every take lies where the range does.
  - The last whole take plays. Stopped before the range's end, it is one clip of what was played. Notes alike.
- **Sound lost while the drive didn't keep up** (past 8 seconds of buffer) is silence in the take, and what follows
  stays in time, as Live leaves a gap and Audacity fills one (known bug 12).
  - One error names each track, where and how long, wherever a clip keeps the sound.
  - Stop waits for the writing thread however long the drive takes, never ending it in the middle of a write.

## 6. What is left

- **Take lanes and comping** (the roadmap's item 4 after v1) are the largest of Live's recording features that Phazite lacks.
  - Phazite already keeps a loop's passes as takes of one file and one clip, so comping would build on that:
    - a lane for each take under its track, shown as automation lanes are;
    - a range selected on a lane playing that take in the main lane, joined by short crossfades (Live's are 4 ms);
    - Next and Previous Take on Ctrl+Up and Down, which the Live keymap has already.
  - What users dislike in Live to avoid: a lane kept for every pass forever, and lanes that leave automation out.
- **An input meter** in the track while it is armed, and a clip mark after 0 dBFS (design section 17).
- **Warp modes:** Phazite has one stretcher, R3. Still to come are a Beats mode that slices drums at their hits, and Re-Pitch.
- **Live's warp commands and a hit sensitivity.**
  - Warp From Here, Set 1.1.1 Here and Warp as N-Bar Loop need tempo detection, which comes after v1.
  - A sensitivity for hits: users complain that Live has none.
- **More bouncing:** Paste Bounced Audio, and Bounce to New Track for a selection of clips rather than a whole track.
- **Several MIDI tracks armed at once,** as Live arms more with Ctrl.
- **Warp markers in DAWproject files,** in its Warps element.
- **Recording latency while monitoring:** recordings are placed by the latencies the device reports, as before. A setting like
  Live's Keep Monitoring Latency in Recorded Audio could follow if a monitored chain needs it.

## 7. Sources

- [rec] Ableton, Live 12 manual, Recording New Clips: <https://www.ableton.com/en/live-manual/12/recording-new-clips/>
- [comp] Ableton, Live 12 manual, Comping: <https://www.ableton.com/en/live-manual/12/comping/>
- [arr] Ableton, Live 12 manual, Arrangement View (Consolidate, fades, Link Tracks): <https://www.ableton.com/en/live-manual/12/arrangement-view/>
- [warp] Ableton, Live 12 manual, Audio Clips, Tempo, and Warping: <https://www.ableton.com/en/live-manual/12/audio-clips-tempo-and-warping/>
- [io] Ableton, Live 12 manual, Routing and I/O: <https://www.ableton.com/en/live-manual/12/routing-and-i-o/>
- [mix] Ableton, Live 12 manual, Mixing (Exclusive Arm, Keep Monitoring Latency): <https://www.ableton.com/en/live-manual/12/mixing/>
- [bounce] Ableton, Live 12 manual, Bounce to Audio: <https://www.ableton.com/en/live-manual/12/bounce-to-audio/>
- [keys] Ableton, Live 12 manual, Keyboard Shortcuts: <https://www.ableton.com/en/live-manual/12/live-keyboard-shortcuts/>
- [sess] Ableton, Live 12 manual, Session View: <https://www.ableton.com/en/live-manual/12/session-view/>
- [auto] Ableton, Live 12 manual, Automation and Editing Envelopes: <https://www.ableton.com/en/live-manual/12/automation-and-editing-envelopes/>
- [rn] Ableton, Live 12 release notes: <https://www.ableton.com/en/release-notes/live-12/>
- [dec] Ableton Help Center, Driver Error Compensation FAQ: <https://help.ableton.com/hc/en-us/articles/115000234830-Driver-Error-Compensation-FAQ>
- [lightred] Ableton forum, auto-armed tracks shown light red: <https://forum.ableton.com/viewtopic.php?p=1751381>
- [pink] Ableton forum, Push arming the selected track (2024): <https://forum.ableton.com/viewtopic.php?p=1823265>
- [midilive] Ableton forum, MIDI notes shown while recording (2024): <https://forum.ableton.com/viewtopic.php?p=1821240>
- [pianoroll] Ableton forum, no piano roll while recording: <https://forum.ableton.com/viewtopic.php?p=1790573>
- [naming] Arapahoe Libraries, Ableton Live recording tutorial (file names): <https://arapahoelibraries.org/blogs/post/ableton-live-recording-tutorial/>
- [consname] Ableton forum, consolidated files' names: <https://forum.ableton.com/viewtopic.php?p=1737377>
- [warp2009] Ableton forum, why recordings are warped (Ableton staff, 2009): <https://forum.ableton.com/viewtopic.php?p=827897>
- [multi2013] Ableton forum, quantizing every transient (2013): <https://forum.ableton.com/viewtopic.php?p=1559035>
- [disable] Ableton forum, take lanes always on (2022 to 2026): <https://forum.ableton.com/viewtopic.php?p=1823270>
- [sos11] Sound On Sound, review of Ableton Live 11: <https://www.soundonsound.com/reviews/ableton-live-11>
- [mt11] MusicTech, review of Ableton Live 11: <https://musictech.com/reviews/review-ableton-live-11/>
- [lwmr] LWMR wiki, recording and tracking workflows: <https://tuttopassastudios.github.io/LWMR-wiki/Topics/Recording/Recording-and-Tracking-Workflows>
- [stein] Steinberg forum, Ableton warping against Cubase's AudioWarp (2025): <https://forums.steinberg.net/t/ableton-warping-vs-cubase-nuendo-audio-warp-ease-of-use/1018570>
- [flrec] Image-Line, FL Studio manual, Recording Audio: <https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/recording_audio.htm>
- [flboris] Boris FX, how to record audio in FL Studio: <https://borisfx.com/blog/how-to-record-audio-in-fl-studio/>
- [logic] Apple Support Communities, Logic arming the selected track: <https://discussions.apple.com/thread/8645079>
- [logicset] Apple, Logic Pro recording settings (Create Track): <https://support.apple.com/en-gu/guide/logicpro/lgcp411dd5c8/mac>
- [s1] PreSonus, Studio One 3 advanced options (Audio Input Follows Selection): <https://support.presonus.com/hc/en-us/articles/210050343-Studio-One-3-Advanced-Options-Overview>
- [reaper] ProMix Academy, record-arming a track in REAPER: <https://promixacademy.com/blog/how-to-record-arm-a-track-in-reaper/>
- [bitwig] KVR forum, Bitwig's arming following the selection: <https://www.kvraudio.com/forum/viewtopic.php?p=7201738>
