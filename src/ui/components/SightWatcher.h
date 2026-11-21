// SPDX-License-Identifier: AGPL-3.0-or-later
// Whether a view can be seen, and what tells it as that may have changed, for a view that builds nothing while it can't
// (the inspector, the Devices view, the versions): as it, or anything it is in, is shown or hidden (its panel, its tab, the
// area that holds its tab), or it goes into something else (a panel into a window of its own). JUCE tells a component of its
// own visibility alone (visibilityChanged()): this listens to it and to everything it is in.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>
#include <vector>

namespace phazite::ui
{
// Whether a component and everything it is in are visible, in a window: a minimized one too, which tells no component as it
// is restored, so that a view there keeps up as it did.
inline bool isInSight (const juce::Component& component)
{
    for (auto* each = &component; each != nullptr; each = each->getParentComponent())
        if (! each->isVisible())
            return false;

    return component.getTopLevelComponent()->isOnDesktop();
}

// Calls changed() as a component, or anything it is in, is shown or hidden, or goes into something else: for it to see
// whether it is in sight now (isInSight()). Message thread.
class SightWatcher final : private juce::ComponentListener
{
public:
    SightWatcher (juce::Component& toWatch, std::function<void()> onChange)
        : watched (toWatch),
          changed (std::move (onChange))
    {
        listen();
    }

    ~SightWatcher() override { stopListening(); }

    SightWatcher (const SightWatcher&) = delete;
    SightWatcher& operator= (const SightWatcher&) = delete;

private:
    juce::Component& watched;
    std::function<void()> changed;
    std::vector<juce::Component::SafePointer<juce::Component>> heard; // it, and what it is in

    void listen()
    {
        for (auto* each = &watched; each != nullptr; each = each->getParentComponent())
        {
            each->addComponentListener (this);
            heard.emplace_back (each);
        }
    }

    void stopListening()
    {
        for (auto& each : heard)
            if (auto* component = each.getComponent())
                component->removeComponentListener (this);

        heard.clear();
    }

    void tell()
    {
        if (changed != nullptr)
            changed();
    }

    void componentVisibilityChanged (juce::Component&) override { tell(); }

    // It, or something it is in, went into something else: what it is in now heard instead.
    void componentParentHierarchyChanged (juce::Component&) override
    {
        stopListening();
        listen();
        tell();
    }
};
} // namespace phazite::ui
