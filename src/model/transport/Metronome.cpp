// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Metronome.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"

namespace phazite
{
bool isMetronomeOn (const Song& song)
{
    return song.getState().getProperty (schema::metronome, false);
}

void setMetronomeOn (Song& song, bool on)
{
    // Written only while it is on, without an undo manager.
    if (on)
        song.getState().setProperty (schema::metronome, true, nullptr);
    else
        song.getState().removeProperty (schema::metronome, nullptr);
}

void toggleMetronome (Song& song)
{
    setMetronomeOn (song, ! isMetronomeOn (song));
}

bool isCountInOn (const AudioIO& audio)
{
    return audio.isCountInOn();
}

void setCountInOn (AudioIO& audio, bool on)
{
    audio.setCountInOn (on);
}

void toggleCountIn (AudioIO& audio)
{
    setCountInOn (audio, ! isCountInOn (audio));
}

void addMetronomeCommands (CommandRegistry& registry, Selection& selection, AudioIO& audio)
{
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };

    registry.add ({ .id = "transport.metronome",
                    .name = "Metronome",
                    .category = "Transport",
                    .description = "Clicks on every beat while the song plays, louder on each bar's first.",
                    .synonyms = { "click", "click track", "beat", "tick", "tempo" },
                    .perform = withSong (selection, toggleMetronome),
                    .isEnabled = hasSong,
                    .isTicked = ifSong (selection, isMetronomeOn) });

    registry.add ({ .id = "transport.countIn",
                    .name = "Count-In",
                    .category = "Transport",
                    .description = "Plays a bar of clicks before recording starts from a stop, even with the metronome off.",
                    .synonyms = { "precount", "pre-roll", "count in", "lead-in" },
                    .perform = [&audio] { toggleCountIn (audio); },
                    .isEnabled = hasSong,
                    .isTicked = [&audio] { return isCountInOn (audio); } });
}
} // namespace phazite
