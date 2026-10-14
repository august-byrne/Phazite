// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion Engine as Phazite's model includes it: the engine's one module header and the te alias. Model code
// includes this, not <tracktion_engine/tracktion_engine.h>, so the model's use of the engine starts in one place.
#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace phazite
{
namespace te = tracktion;
} // namespace phazite
