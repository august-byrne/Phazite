// SPDX-License-Identifier: AGPL-3.0-or-later
// The title along the top of a view in a side panel, as VS Code's side bar names the view it shows ("Explorer", "Source
// Control"): the view's name at the left, and its actions at the right, each a quiet icon button, the view's menu (More
// Actions) last. It takes no clicks of its own. The inspector's title is one too, naming what it shows.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace phazite::ui
{
class ViewTitle : public juce::Component
{
public:
    explicit ViewTitle (const juce::String& name);

    // Laid out from the left in the order added, the last at the right edge. The buttons are the view's.
    void addAction (juce::Button&);

    const juce::String& getName() const noexcept { return name; }
    void setText (const juce::String&); // the name, for a title that names what its view shows

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String name;
    std::vector<juce::Component::SafePointer<juce::Button>> actions; // which may go before it does
};
} // namespace phazite::ui
