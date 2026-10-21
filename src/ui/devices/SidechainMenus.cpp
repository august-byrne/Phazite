// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SidechainMenus.h"
#include "model/mixer/MixerModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr float dotSize = 8.0f;

    // A dot in a track's colour, for its menu item and the button.
    std::unique_ptr<juce::Drawable> makeDot (juce::Colour colour)
    {
        auto dot = std::make_unique<juce::DrawablePath>();
        juce::Path path;
        path.addEllipse (0.0f, 0.0f, dotSize, dotSize);
        dot->setPath (path);
        dot->setFill (colour);
        return dot;
    }

    // A track to listen to, with its colour; one that can't be picked, greyed, with why.
    juce::PopupMenu::Item makeTrackItem (const SidechainChoice& choice, bool ticked, std::function<void()> action)
    {
        juce::PopupMenu::Item item (choice.whyNot.isEmpty() ? choice.name : choice.name + " (" + choice.whyNot + ")");
        item.setImage (makeDot (choice.colour));
        item.setEnabled (choice.whyNot.isEmpty());
        item.setTicked (ticked);
        item.setAction (std::move (action));
        return item;
    }
} // namespace

juce::PopupMenu makeSidechainMenu (ProjectContext& context, DeviceID device, const std::function<void()>& changed)
{
    juce::PopupMenu menu;
    const auto source = getSidechainSource (context, device);
    const auto choose = [&context, device, changed] (TrackID track)
    {
        return [&context, device, changed, track]
        {
            setSidechainSource (context, device, track);

            if (changed)
                changed();
        };
    };

    menu.addItem (juce::PopupMenu::Item ("Off: the sound passing through it").setTicked (! source.isValid()).setAction (choose ({})));
    menu.addSeparator();

    for (const auto& choice : getSidechainChoices (context, device))
        menu.addItem (makeTrackItem (choice, choice.track == source, choose (choice.track)));

    return menu;
}

juce::PopupMenu makeDuckUnderMenu (ProjectContext& context, TrackID ducked)
{
    juce::PopupMenu menu;

    for (const auto& choice : getDuckUnderChoices (context, ducked))
        menu.addItem (makeTrackItem (choice, false, [&context, ducked, key = choice.track] { duckUnder (context, ducked, key); }));

    return menu;
}

//==============================================================================
SidechainButton::SidechainButton (ProjectContext& c, DeviceID id)
    : juce::Button ("Sidechain"),
      context (c),
      device (id)
{
    setTitle ("Sidechain");
    show ({});
}

void SidechainButton::show (const SidechainChoice& newSource)
{
    source = newSource;
    const auto listens = source.track.isValid();
    setButtonText (listens ? source.name : juce::String ("Sidechain"));
    setDescription (listens ? "Listens to " + source.name : juce::String ("Listens to the sound passing through it"));
    setTooltip (listens ? "Listens to " + source.name + ", rather than the sound passing through it: click to choose another, or Off"
                        : "Click to make it listen to another track, as a compressor ducking under a kick does");
    repaint();
}

void SidechainButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    const auto& p = getPalette (*this);
    getLookAndFeel().drawButtonBackground (g, *this, p.surfaceRaised, isHighlighted, isDown);

    // The source's dot, then its name; or a hollow dot while it listens to the sound passing through it.
    auto area = getLocalBounds().reduced (Space::s, 0);
    const auto dot = area.removeFromLeft ((int) dotSize).toFloat().withSizeKeepingCentre (dotSize, dotSize);
    area.removeFromLeft (Space::xs);

    if (source.track.isValid())
    {
        g.setColour (source.colour);
        g.fillEllipse (dot);
    }
    else
    {
        g.setColour (p.textMuted);
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
    }

    g.setColour (source.track.isValid() ? p.text : p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawFittedText (getButtonText(), area, juce::Justification::centredLeft, 1);
}

void SidechainButton::clicked()
{
    const juce::Component::SafePointer<SidechainButton> safeThis (this);
    makeSidechainMenu (context, device,
                       [safeThis]
                       {
                           if (safeThis != nullptr && safeThis->onChange)
                               safeThis->onChange();
                       })
        .showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}
} // namespace phazite::ui
