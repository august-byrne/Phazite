# The mixer

*Design section 8.*

The mixer shows every channel of the song side by side: a strip for each track, folder and return, and the master.
It is where levels are set, devices are listed and added, and sound is routed, through sends, buses and sidechains.

## Status

| Feature | State | Note |
| --- | --- | --- |
| A strip for every track, folder and return, and the master | Built | |
| Instrument slot apart from the effects | Built | |
| Effect slots, moved by dragging | Built | Within a channel; not from one channel to another |
| Send knobs, pre or post fader | Built | |
| Returns, made with their send in one step | Built | |
| Output to the master or a bus | Built | |
| Output to any track, or to several | Declined | One output per track; sends do the rest |
| Folders as buses, or plain | Built | |
| One switch for mute and solo | Built | |
| Fader in decibels with a scale; meter on the same scale | Built | |
| CPU bar on each strip | Built | |
| Routing outlined from the selection | Built | |
| Routing view with cables | Declined | Routing is set on the strips |
| Sidechain picker inside the device; Duck Under | Built | |
| Constant-power pan | Built | |
| Loudness meter on the master (LUFS, true peak) | Built | |

## Where it is

The mixer is a view of the upper panel, beside the playlist (F9), with the lower panel under it
([window.md](window.md)). A click on a device's name in a strip shows the device in the lower panel's Devices view; a
plug-in's own window opens too.

## Channels

- One strip per track, in the playlist's order and colours.
- A folder is a strip of its own, with its tracks after it.
- The returns come last, together, set apart by a gap, with a slim column after them whose "+" adds a return.
- The master is on the right.
- The mixer has no toolbar or row of its own.
- The strips scroll sideways. Only the channels in view have strips, and one more at either end, which the Tab key
  goes on to and scrolls into view, as a list shows a window of its rows: a song of hundreds of tracks costs the
  mixer what a dozen do. A strip stays while it is in use (a fader dragged, a value typed) as the mixer scrolls; a
  jump makes what part of a frame can bear, and the rest over the next turns of the message loop. A
  screen reader hears the strips in view, and from the channels' view how many channels there are and which show.

## A strip, top to bottom

A strip reads as the sound goes.

1. **The name,** and under it what the channel is and what feeds it: "Bus: mixes its tracks", or "Return, from Lead,
   Pad". At the name's right, the track's switch for mute and solo, and its arm switch while it records audio. A
   folder has its **Bus** switch here.
2. **The instrument** (tracks only): the device that turns the track's notes into sound, in a slot of its own above
   the effects. A track with notes and no instrument asks for one ("Choose instrument", tinted). Its menu lists
   instruments only. It has no on/off switch: the track's switch mutes it, and Freeze takes its CPU away while keeping
   its sound.
3. **Effects,** each a slot of one line: its on/off switch and its name. The slots list the chain; the Devices view
   has the controls.
4. **Sends:** a knob for every return the track can send to, off until first turned. Turning it makes the send, after
   the fader, in the same undo step; once it is there it has a Pre/Post switch.
5. **Where the sound goes:** "Out: Master", or the bus the track is in.
6. **Pan, fader and meter.** The fader is as tall in every strip, however many devices and sends are above it; those
   scroll in the room that is left.
7. **A CPU bar** along the bottom: how much of each block's time the strip's devices take, in the warning colour above
   half. It shows a load while the song plays and is empty while it is stopped.

| Gesture | Does |
| --- | --- |
| Click a strip | Selects its channel |
| Click a device's name | Shows the device in the Devices view |
| Ctrl+click, Shift+click a device's name | Selects the device |
| Right-click a device's name | Opens its menu |
| Drag an effect's name up or down | Moves it in the chain; a line shows where it goes |
| + Effect | Lists effects only; on a track whose notes have no instrument, it offers the instrument first |
| + Send | Makes a new return with a send to it, with an effect or empty |
| Click Out | Offers the master, the buses, and New Bus from the Selected Tracks |
| Drop a device or a preset from the browser | Adds it to the channel |

Choosing a bus in the Out menu moves the track into that folder, as the playlist then shows it.

### The fader, its scale and the meter

- Every volume fader shows and takes decibels. Its travel follows one curve: a position from 0 to 1 is
  20 ln(position) + 6 dB, which puts 0 dB at about 74 % of the travel and +6 dB at the top; the bottom is silence.
- Left of the fader, a scale marks +6, 0, -6, -12, -18, -24 and -36 dB where the fader puts them, 0 dB in the text
  colour.
- The meter right of the fader reads on the same scale: it shows each level where the fader's thumb would be at that
  level, so a sound at -12 dB lights it up to the scale's -12, wherever the fader is. The fader sets a gain and the
  meter shows the level after it.
- Volume automation lanes draw the fader's position.
- A value can be typed into a fader or a knob, and a double-click resets it ([devices.md](devices.md)).

### Pan

Constant power, with the sound at its level in the middle: panned hard to a side, that side is 3 dB louder and the
other silent, so a sound keeps its loudness wherever it is panned. A stereo track's pan works the same way on its two
sides, as a balance.

## Mute and solo

One small switch, a light, in a track's playlist header, its mixer strip and the inspector.

| Gesture | Does |
| --- | --- |
| Click | Mutes the track (the light goes out to a hollow ring); again plays it |
| Right-click | Solos the track alone; again ends the solo |
| Ctrl+right-click | Solos it along with the tracks soloed already |

- Lit green, the track plays. A soloed track's light is ringed, in the solo colour. A track that other tracks' solos
  silence shows its light faint. Shape tells the states apart as well as colour: filled, hollow, ringed, faint.
- Mute silences the track's output only, fading over 5 ms: its sends before the fader go on, its sends after the
  fader are silent with it, and its instrument and effects play on underneath, as in Ableton Live and most DAWs, so
  that a note it holds, a reverb's tail and a delay's echoes are heard at once when it is heard again. A muted track
  still feeds a sidechain that listens to it ([devices.md](devices.md)).
- A track that another track's solo silences sends nothing, before the fader or after, but to a return that is
  soloed; it plays on underneath too.
- Muting a bus silences its output only: its tracks go on playing into it while something still listens to it (a
  sidechain keyed from it, or a send of it that goes on), and what else they send, after their faders, is silent with
  the bus. (A bus has no sends of its own in Phazite: Add Send refuses one, so in practice that is its key.)
- A muted track still costs its CPU live: Freeze is what frees it (Bounce in Place freezes the original it mutes).
  A render (an export, a stem, a freeze, a bounce) leaves out what it can't hear, so a muted track that feeds nothing
  heard costs it nothing.
- A solo never changes the mute switches.
- Soloing a folder solos its tracks, and soloing a track in a bus keeps the bus audible.
- Returns are solo-safe: soloing a track keeps its reverb audible.
- Neither mute nor solo is an undo step.
- Mute Track and Solo Track are commands too, in the track's right-click menu and the Edit menu, for the keyboard and
  screen readers. Ableton Live's keymap gives Solo Track the S key.

## Returns and sends

- A return holds effects that several tracks share.
- A new return with an effect comes in one step: the return, named after the effect, and a send to it from the track.
  One of Phazite's reverbs on a return gives only its wet sound.
- **A send gives the return a copy.** The track still plays through its own Out, and the return's effect plays
  alongside it. To run several tracks' whole sound through one effect, they go into a bus instead. The tooltips of the
  send knobs, + Send, the returns' "+" and the Out button say so.
- A send knob reads and takes decibels on the fader's curve, and is automated and modulated as a fader is: its lane and
  its track's modulators and macros move it along that travel while the song plays, and the knob shows where they put
  it ([automation-and-modulation.md](automation-and-modulation.md)).
- A strip has a send knob for each return its sound can go to without coming back to it; a send that changes where
  sound goes takes away or brings back the other strips' knobs. A send whose sound would come back through sends and
  buses is never made (a loop, which the engine can't play); one that comes back only through a sidechain is made, and
  the sidechain goes (below).

## Folders are buses

- Every new folder mixes its tracks through its own effects, fader and meter.
- Its Bus switch can make it plain: a plain folder has no sound of its own, and its fader only scales its tracks'
  volumes.
- A plain folder becomes a bus again when it gets an effect, and a folder whose switch is off turns plain again when
  its last effect goes. A bus that holds an effect cannot be made plain by its switch.
- New Bus from the Selected Tracks, in the Out menu, puts the selected tracks into a new folder.

## Routing shows on selection

Selecting a channel outlines the channels it feeds (the returns it sends to, the bus it is in), the ones that feed it,
and those its sidechains link it to, either way round: a kick outlines the bass that ducks under it, and the bass the
kick. There is no routing view and there are no cables.

## Sidechain

A device with a sidechain input (Phazite's compressor, its multiband, its vocoder, or a plug-in) has one place to
choose what it listens to, inside the device:

- one of Phazite's own devices has a drop-down in its editor, labelled **Sidechain** where a label stands beside it
  ([devices.md](devices.md));
- a plug-in has it at the right end of its window's header, by its Plug-in menu, and above its knobs on its panel in
  the Devices view.

The picker shows the source's colour and name, or a hollow dot and None while the device listens to the sound passing
through it.

- Its menu lists None and every other track in the song's order, each with its colour. The source can be a track, a
  return or a bus.
- A source that would make a loop (one whose sound comes from the device's own track) is greyed, with why. A plain
  folder is greyed too, with why.
- Choosing a source turns the sidechain on, in one undo step. There is no switch of its own and no routing to set up.
- A change that would make a sidechain a loop (a track moved into the bus it listens to, a send to the return it
  listens to), deleting the source, or making a source folder plain turns that sidechain off in the same undo step.
- In the mixer, a slot whose device listens to a track has a stripe of that track's colour down its left, and its
  tooltip names the track.

**Duck Under,** in a track's right-click menu, does "kick ducks bass" in one step ("Duck Bass Under Kick"): it adds
Phazite's compressor after the track's effects, in its Duck mode, listening to the other track.

Choosing a source for Phazite's compressor still as it was made (but for its mode) puts it in its Duck mode in the same
undo step, set up to pump under a kick: from -14 dB, 14 dB down, in 1 ms, held 120 ms, and back over 80 ms. When the
source plays a drum sampler, the compressor hears only the kick's body, from 60 to 150 Hz, so that the snare, clap and
hats on the same track do not move it. A compressor set its own way keeps its mode: compressing, it is then a keyed
compressor, its curve worked by the source's level. While it shapes transients (its Transient mode), it hears its own
sound, and the mixer does not link the two channels.

## The master

Its effects, fader and meter, and a loudness meter: momentary, short-term and integrated loudness in LUFS and the true
peak, with a button that starts the integrated loudness and the true peak again. The engine measures it by EBU R128 on
the audio thread, the true peak by ITU-R BS.1770's filter. It measures the master's effects, before the master volume.

## Decisions

- The instrument has a place of its own above the effects, so that a track with notes is never given an effect where
  it needs an instrument without the strip saying so.
- The instrument has no on/off switch: it would be a second way to mute the track, and Freeze already frees its CPU.
- Effect slots are one line without knobs: the strip lists the chain, and the Devices view controls it.
- A folder is a bus by default, as group tracks are in Ableton Live and Bitwig.
- One switch for mute and solo, click and right-click, in place of separate M and S buttons; a solo never changes the
  mute switches, so that earlier mutes are not lost.
- Routing is set on the strips and shown from the selection; the routing view with cables was removed, because every
  DAW compared puts everyday routing on the strip.
- A sidechain is chosen inside the device, in one step, under the word Sidechain; "Key" reads as the musical key.
- One output per track; sending one channel to several is done with sends.
- Faders work in decibels on one curve, with 0 dB at about three quarters of the travel, and the meter reads on the
  fader's scale.
- Pan is constant power with 0 dB in the middle, as Ableton Live's and Logic Pro's default.

## Not built

- A routing view with cables: Declined.
- An output to any track, or to several at once: Declined; sends do it.

## In the code

- `src/ui/mixer/MixerView.h`, `ChannelStrip.h`, `MasterStrip.h`, `DeviceSlots.h`, `StripParts.h`.
- `src/ui/mixer/DevicesView.h`: the Devices view the slots open.
- `src/ui/components/Controls.h` (`ui::Fader`, `ui::FaderScale`, `ui::LevelMeter`, `ui::Knob`), `TrackSwitch.h`,
  `ArmButton.h`.
- `src/ui/devices/SidechainMenus.h`: the sidechain picker.
- `src/model/mixer/MixerModel.h`: the mixer by IDs (channels, volume and pan handles, sends, outputs, sidechains,
  `duckUnder()`, `getCpuLoad()`), `LevelSource.h`, `SidechainChoice.h`.
- `src/model/mixer/internal/Mixer.h`, `Buses.h`, `Sidechain.h`: the song's side.
- `src/engine/nodes/Channel.h` (fader curve, pan, mute, meter), `Strip.h`, `LoudnessMeter.h`.
