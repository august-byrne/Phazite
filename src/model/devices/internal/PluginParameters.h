// SPDX-License-Identifier: AGPL-3.0-or-later
// A plug-in's parameters through its updates. Tracktion knows a plug-in's parameters (a VST3's) by their places in the
// plug-in's list, "0", "1" and on, and a project names them so wherever it refers to one: its automation curve, what
// modulates it, its value set by hand under automation, and its automation lane in the playlist. An update of the plug-in
// that adds, removes or reorders parameters would move all of those to other parameters. So the project also saves the
// plug-in's own ID for each parameter it refers to (schema::phaziteParameterIDs), which stays the same through updates,
// and a plug-in that opens finds each of them again where its update put it.
#pragma once

#include "model/base/internal/Tracktion.h"

namespace phazite
{
// Stores, without an undo step, the plug-in's own IDs of the parameters the project refers to now (storeDeviceState()
// calls it). Leaves what is stored as it is while the plug-in isn't open.
void storeParameterIDs (te::ExternalPlugin&);

// Moves what refers to the plug-in's parameters to where its instance, just made, has them, if they moved since the
// project stored their IDs, and lets go of what refers to a parameter it no longer has, saying so in the log. Changes
// nothing if it knows none of them: then it isn't the plug-in that stored them. Before Tracktion makes the plug-in's
// parameters, which find their automation by those names (EngineBehaviour::doAdditionalInitialisation()).
void findMovedParameters (te::ExternalPlugin&);
} // namespace phazite
