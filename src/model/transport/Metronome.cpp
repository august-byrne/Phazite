// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Metronome.h"
#include "model/commands/CommandRegistry.h"

namespace phazite
{
bool isMetronomeOn (const te::Edit& edit)
{
    return edit.clickTrackEnabled.get();
}

void setMetronomeOn (te::Edit& edit, bool on)
{
    // Tracktion writes these without an undo manager. The bars' first beats are louder, as in most DAWs.
    if (on)
        edit.clickTrackEmphasiseBars = true;

    edit.clickTrackEnabled = on;
}

void toggleMetronome (te::Edit& edit)
{
    setMetronomeOn (edit, ! isMetronomeOn (edit));
}

bool isCountInOn (const te::Edit& edit)
{
    return edit.getCountInMode() != te::Edit::CountIn::none;
}

void setCountInOn (te::Edit& edit, bool on)
{
    edit.setCountInMode (on ? te::Edit::CountIn::oneBar : te::Edit::CountIn::none);
}

void toggleCountIn (te::Edit& edit)
{
    setCountInOn (edit, ! isCountInOn (edit));
}

void addMetronomeCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };

    registry.add ({ .id = "transport.metronome",
                    .name = "Metronome",
                    .category = "Transport",
                    .description = "Clicks on every beat while the song plays, louder on each bar's first.",
                    .synonyms = { "click", "click track", "beat", "tick", "tempo" },
                    .perform = withEdit (selection, toggleMetronome),
                    .isEnabled = hasEdit,
                    .isTicked = ifEdit (selection, isMetronomeOn) });

    registry.add ({ .id = "transport.countIn",
                    .name = "Count-In",
                    .category = "Transport",
                    .description = "Plays a bar of clicks before recording starts from a stop, even with the metronome off.",
                    .synonyms = { "precount", "pre-roll", "count in", "lead-in" },
                    .perform = withEdit (selection, toggleCountIn),
                    .isEnabled = hasEdit,
                    .isTicked = ifEdit (selection, isCountInOn) });
}
} // namespace phazite
