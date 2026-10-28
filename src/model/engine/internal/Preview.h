// SPDX-License-Identifier: AGPL-3.0-or-later
// The previewer's Tracktion side (model/engine/Preview.h), for the app and tests: one made for an Engine, and the Edit a
// sound previews in. Not for the UI.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <memory>

namespace phazite
{
class Previewer;

// Its preview Edit goes when it does, which must be before the Engine.
std::unique_ptr<Previewer> createPreviewer (te::Engine&);

// The Edit a sound previews in: Tracktion's preview Edit (te::Edit::createEditForPreviewingFile()), made again from the
// one before if given, with a MIDI file on Phazite's synth; inTime says whether it is a loop in time with the project
// (which internal/PreviewLoop.h then stretches). Tracktion keeps what it read of a file by its path, so it reads the file
// again if it changed since: a loop written again at the same path would otherwise play at its old length.
std::unique_ptr<te::Edit> createPreviewEdit (te::Engine&, const juce::File&, const te::Edit* project, bool& inTime,
                                             std::unique_ptr<te::Edit> previous = {});
} // namespace phazite
