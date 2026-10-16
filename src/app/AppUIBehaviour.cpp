// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppUIBehaviour.h"
#include "PluginWindow.h"

namespace phazite
{
std::unique_ptr<juce::Component> AppUIBehaviour::createPluginWindow (te::PluginWindowState& state)
{
    return phazite::createPluginWindow (state); // the free function, not this
}

void AppUIBehaviour::recreatePluginWindowContentAsync (te::Plugin& plugin)
{
    // Once the plug-in has settled, as Tracktion's own default waits.
    juce::Component::SafePointer<juce::Component> window (plugin.windowState != nullptr ? plugin.windowState->pluginWindow.get() : nullptr);

    if (window == nullptr)
    {
        te::UIBehaviour::recreatePluginWindowContentAsync (plugin);
        return;
    }

    juce::MessageManager::callAsync (
        [window, safePlugin = te::Plugin::Ptr (&plugin)]
        {
            if (window != nullptr)
                phazite::recreatePluginWindowContent (*safePlugin);
        });
}
} // namespace phazite
