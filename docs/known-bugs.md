# Known bugs

Bugs that were found and aren't fixed yet: how each shows, why it happens, and how to fix it. Fix them before v1 unless
the owner says otherwise. Add a bug here when it can't be fixed in the change that found it, and take it out in the
commit that fixes it. Races and allocations on the audio thread that Tracktion's code causes are in
[realtime-audit.md](realtime-audit.md) instead, and Tracktion's other quirks in [design.md](design.md), section 23.

None at the moment. The last, bug 3 ("two limiters in a song silence what is played live"), was fixed on 2026-10-28: the
limiters had nothing to do with it ([realtime-audit.md](realtime-audit.md), finding 23).
