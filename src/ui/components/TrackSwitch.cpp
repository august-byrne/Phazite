// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TrackSwitch.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    juce::String describeSwitch (const MuteSolo& state)
    {
        if (state.muted && state.soloed)
            return "Muted and soloed: silent until it is unmuted. Click to unmute it, or right-click to unsolo it";

        if (state.muted)
            return "Muted. Click to play it again, or right-click to solo it";

        if (state.soloed)
            return "Soloed: only the soloed tracks play. Right-click to unsolo it; Ctrl+right-click another track to solo it too";

        if (state.silencedBySolo)
            return "Silent while other tracks are soloed. Right-click to solo this track instead, Ctrl+right-click to solo it too, "
                   "or click to mute it";

        return "Playing. Click to mute this track, or right-click to solo it, as in FL Studio (Ctrl+right-click solos it along "
               "with others)";
    }
} // namespace

TrackSwitch::TrackSwitch (ProjectContext& projectContext)
    : juce::Button ("Mute and solo"),
      context (projectContext)
{
    setMouseClickGrabsKeyboardFocus (false);
    update();
}

void TrackSwitch::setTrack (TrackID id)
{
    track = id;
    update();
}

void TrackSwitch::update()
{
    const auto state = track.isValid() ? getMuteSolo (context, track) : std::nullopt;

    if (state == shown && getTitle().isNotEmpty())
        return;

    shown = state;
    setEnabled (shown.has_value());
    setTitle (shown.has_value() ? "Mute and solo: " + getStateText() : juce::String ("Mute and solo")); // what screen readers say
    setTooltip (shown.has_value() ? describeSwitch (*shown) : juce::String());
    repaint();
}

juce::String TrackSwitch::getStateText() const
{
    if (! shown.has_value())
        return {};

    if (shown->muted)
        return shown->soloed ? "Muted and soloed" : "Muted";

    if (shown->soloed)
        return "Soloed";

    return shown->silencedBySolo ? "Silent while other tracks are soloed" : "Playing";
}

void TrackSwitch::clicked (const juce::ModifierKeys& mods)
{
    if (! shown.has_value())
        return;

    // A click (or Space while it has the keyboard focus) mutes; a right-click solos, alone or, with Ctrl, along with others.
    if (! mods.isPopupMenu())
        toggleMute (context, track);
    else if (mods.isCommandDown())
        toggleSolo (context, track);
    else
        toggleSoloAlone (context, track);

    update();
}

void TrackSwitch::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    if (shown.has_value())
        paintMuteSoloLight (g, *this, *shown, isHighlighted, isDown);
}

void paintMuteSoloLight (juce::Graphics& g, const juce::Component& component, const MuteSolo& state, bool isHighlighted, bool isDown)
{
    const auto& p = getPalette (component);
    const auto bounds = component.getLocalBounds().toFloat();
    const auto size = std::min (bounds.getWidth(), bounds.getHeight()) * SwitchTint::light;
    const auto light = bounds.withSizeKeepingCentre (size, size);

    if (isHighlighted || isDown)
    {
        g.setColour (isDown ? p.background : p.surface);
        g.fillRoundedRectangle (bounds, cornerRadius);
    }

    // Muted: the light out, a hollow ring. Otherwise lit, on an edge that keeps it apart from a tinted header: faint while
    // others' solos silence it, in the solo colour while it is soloed.
    if (state.muted)
    {
        g.setColour (isHighlighted ? p.text : p.textMuted);
        g.drawEllipse (light.reduced (SwitchTint::ring / 2.0f), SwitchTint::ring);
    }
    else
    {
        g.setColour (p.background);
        g.fillEllipse (light.expanded (SwitchTint::edge));
        g.setColour (state.soloed ? p.soloed : p.playing.withMultipliedAlpha (state.silencedBySolo ? SwitchTint::silenced : 1.0f));
        g.fillEllipse (light);
    }

    // Soloed: a ring around it.
    if (state.soloed)
    {
        g.setColour (p.soloed);
        g.drawEllipse (light.expanded (SwitchTint::ringGap + SwitchTint::ring / 2.0f), SwitchTint::ring);
    }
}
} // namespace phazite::ui
