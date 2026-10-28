# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread that Tracktion's code causes are in
[realtime-audit.md](realtime-audit.md) instead, and Tracktion's other quirks in [design.md](design.md), section 23.

## 3. Two limiters in a song silence what is played live

*Found 2026-10-21, by a test of the Low Latency switch (roadmap step 2.16).*

- **How it shows:** with two of Phazite's look-ahead dynamics devices anywhere in the song (two Limiters, or a Limiter
  and a Maximizer, which holds one: on the master, on a track, or one of each), notes played on the computer keyboard
  or a MIDI keyboard make no sound, stopped or playing. The song itself plays as it should. One of them alone is
  fine, and so is Low Latency, which bypasses them.
- **Why:** not found yet. The note never reaches the instrument: Phazite's test instrument, processed every block,
  receives no event. It isn't delay compensation (turning it off changes nothing), the amount of look-ahead (two test
  devices with the same 99 samples of latency, or with tails, are fine), or a graph rebuilt over and over (the
  instrument is prepared once). Something in two Limiter DSPs together breaks Tracktion's live MIDI path
  (`MidiInputDeviceNode`, whose messages wait for the node that last processed); memory written out of place would fit.
  The order matters: the live-note benchmark (`tests/Benchmarks.cpp`) adds its limiters after playback was prepared, and
  there the note sounds, 200 samples late as delay compensation says; the failing case adds them before
  `ensureContextAllocated()`.
- **How to fix:** reproduce with the steps below in the `linux-asan` build to catch a bad write, or under the debugger
  with a breakpoint in `MidiInputDeviceNode::handleIncomingMidiMessage`, and see where the note goes.
- **How to test it:** a single-track Edit with a second track, an instrument on the first, routed as live MIDI
  (`routeLiveMidiInput()`), a Limiter and a Maximizer on the master, played through `tests/HostedDevice.h`: a note sent
  to All MIDI Ins (`playIntoAllMidiInputs()`) should sound within a block plus the look-ahead.
