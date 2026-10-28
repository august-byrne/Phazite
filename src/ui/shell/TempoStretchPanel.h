// SPDX-License-Identifier: AGPL-3.0-or-later
// What a change of the tempo stretches, under the top bar's tempo field while the tempo changes (docs/design.md,
// section 4; roadmap step 2.18): how many of the song's samples follow the tempo and how many keep their speed, and
// whether its patterns follow it, with a switch for each, which run the commands "Samples Follow Tempo" and "Patterns
// Follow Tempo". DAWs keep this in settings rather than ask at each change (Studio One's Follow Tempo, Reaper's
// timebase); Phazite shows them where the tempo changes, so that what will stretch is never a surprise. It never takes
// the keyboard's focus, so a tempo being typed goes on.
#pragma once

#include "model/transport/TempoValues.h"
#include "ui/input/CommandBridge.h"

#include <optional>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class TempoStretchPanel : public juce::Component
{
public:
    TempoStretchPanel (CommandBridge&, ProjectContext&);

    // Shows what a tempo change stretches now.
    void update();

    // How tall it is, at its width (Shell::tempoStretchWidth).
    int getIdealHeight() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CommandBridge& commands;
    ProjectContext& context;
    juce::ToggleButton samples { "Samples" }, patterns { "Patterns" };
    std::optional<TempoStretch> shown; // what it says
};
} // namespace phazite::ui
