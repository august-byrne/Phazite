// SPDX-License-Identifier: AGPL-3.0-or-later
// Tracktion's UI callbacks as the app answers them: phazite::UIBehaviour's, plus devices' windows, which the UI makes
// (PluginWindow.h). Main passes one to createEngine().
#pragma once

#include "model/engine/internal/EngineSetup.h"

namespace phazite
{
class AppUIBehaviour final : public UIBehaviour
{
public:
    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState&) override;
    void recreatePluginWindowContentAsync (te::Plugin&) override;
};
} // namespace phazite
