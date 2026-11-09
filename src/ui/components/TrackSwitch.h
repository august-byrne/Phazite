// SPDX-License-Identifier: AGPL-3.0-or-later
// A track's one switch for mute and solo (docs/design.md, section 8), in its playlist header, its mixer strip and the
// inspector: a small light, as FL Studio's mute switch is (the owner, 2026-11-01: separate M and S buttons took room and
// said the same twice). Lit, the track plays: a click mutes it, the light going out to a hollow ring, and another click
// plays it again. A right-click solos the track alone, as in FL Studio, and again unsolos it; Ctrl+right-click solos it
// along with the tracks soloed already. A soloed track's light is ringed, in the solo colour. Soloing never changes the
// mute switches (FL Studio's solo does, a long-standing complaint): a track that other tracks' solos silence shows its
// light faint. Neither is an undo step, as in many DAWs. Shape tells the states apart as well as colour: filled, hollow,
// ringed, faint.
#pragma once

#include "model/base/IDs.h"
#include "model/mixer/MixerModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class TrackSwitch : public juce::Button
{
public:
    explicit TrackSwitch (ProjectContext&);

    void setTrack (TrackID); // an empty ID for none
    void update();           // shows the track's mute and solo as they are now

    // What it shows, in words, for the inspector beside it and for screen readers: "Playing", "Muted", "Soloed", "Muted
    // and soloed", or "Silent while other tracks are soloed"; nothing without a track.
    juce::String getStateText() const;

    void clicked (const juce::ModifierKeys&) override;
    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

private:
    ProjectContext& context;
    TrackID track;
    std::optional<MuteSolo> shown;

    using juce::Button::clicked;
};

// The light of a switch for mute and solo, a track's or a container's chain's (ui/mixer/ContainerEditor.h), as it shows
// a state: lit, hollow while muted, ringed while soloed, faint while others' solos silence it; a wash under the pointer.
void paintMuteSoloLight (juce::Graphics&, const juce::Component&, const MuteSolo&, bool isHighlighted, bool isDown);
} // namespace phazite::ui
