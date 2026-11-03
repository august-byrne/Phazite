// SPDX-License-Identifier: AGPL-3.0-or-later
#include "NewProject.h"
#include "internal/NewProject.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/mirror/internal/Bridge.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Tempo.h"

namespace phazite
{
bool isValid (const NewProjectSettings& settings)
{
    const auto tempoIsValid = settings.tempo >= minTempo && settings.tempo <= maxTempo;
    const auto keyIsValid =
        ! settings.keyRoot.has_value() || (juce::isPositiveAndBelow (*settings.keyRoot, 12) && findScale (settings.scale).has_value());
    const auto outlineIsValid = settings.outline.isEmpty() || findOutline (settings.outline) != nullptr;

    return tempoIsValid && isValid (settings.timeSignature) && keyIsValid && outlineIsValid;
}

void addNewProjectContent (te::Edit& edit, const NewProjectSettings& settings)
{
    // Not undo steps that matter: the project opens from its first save, without them. It starts with no tracks (the owner,
    // 2026-11-01), so the one Tracktion makes with the Edit goes.
    deleteTracks (getPlaylistTracks (edit), nullptr);
    setTempo (edit, settings.tempo);
    setTimeSignature (getSong (edit), settings.timeSignature);

    // The key goes on the tracks added later.
    if (settings.keyRoot.has_value())
        if (const auto scale = findScale (settings.scale))
            setLastKey (edit, { *settings.keyRoot, *scale });

    if (settings.outline.isNotEmpty())
        applyOutline (edit, settings.outline);
}
} // namespace phazite
