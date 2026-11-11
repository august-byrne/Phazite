// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SidechainMenus.h"
#include "model/mixer/MixerModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr float dotSize = 8.0f;
    constexpr float arrowSize = 8.0f; // the chevron, as a list's (LookAndFeel::drawComboBox())

    // A dot in a track's colour, for its menu item.
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

    juce::String describeNone (const juce::String& meaning)
    {
        return meaning.isEmpty() ? juce::String ("None") : "None (" + meaning + ")";
    }
} // namespace

juce::PopupMenu makeSidechainMenu (ProjectContext& context, DeviceID device, const std::function<void()>& changed,
                                   const juce::String& noneMeaning)
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

    menu.addItem (juce::PopupMenu::Item (describeNone (noneMeaning)).setTicked (! source.isValid()).setAction (choose ({})));
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
    refresh();
}

void SidechainButton::show (const SidechainChoice& newSource)
{
    source = newSource;
    const auto listens = source.track.isValid();
    const auto none = describeNone (noneMeaning);

    // A device in a container hears what passes through it: nothing to choose there.
    const auto canChoose = hasSidechain (context, device);
    setEnabled (canChoose);
    setButtonText (listens ? source.name : juce::String ("None"));
    setDescription (listens ? "Listens to " + source.name : none);
    setTooltip (! canChoose ? juce::String ("In a container it hears the sound passing through it: take it out of the container for it to "
                                            "listen to another track")
                : listens   ? "Listens to " + source.name + " through its sidechain. Click to choose another track, or None"
                            : none + ". Click to choose a track for it to listen to through its sidechain instead");
    repaint();
}

void SidechainButton::refresh()
{
    const auto track = getSidechainSource (context, device);
    SidechainChoice shown;

    for (const auto& choice : getSidechainChoices (context, device))
        if (choice.track == track)
            shown = choice;

    show (shown);
}

void SidechainButton::setNoneMeaning (const juce::String& meaning)
{
    noneMeaning = meaning;
    show (source);
}

juce::PopupMenu SidechainButton::getMenu()
{
    const juce::Component::SafePointer<SidechainButton> safeThis (this);
    return makeSidechainMenu (
        context, device,
        [safeThis]
        {
            if (safeThis == nullptr)
                return;

            safeThis->refresh();

            if (safeThis->onChange)
                safeThis->onChange();
        },
        noneMeaning);
}

void SidechainButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    // As a list to pick from (LookAndFeel::drawComboBox()): on the surface, outlined (in the focus colour while it has the
    // keys), its arrow at its right; lit under the pointer, as a button is.
    const auto& p = getPalette (*this);
    const auto bounds = getLocalBounds().toFloat();
    const auto alpha = isEnabled() ? 1.0f : 0.5f;
    g.setColour (p.surface);
    g.fillRoundedRectangle (bounds, cornerRadius);

    if (isEnabled() && (isHighlighted || isDown))
    {
        g.setColour (isDown ? p.selected : p.hover);
        g.fillRoundedRectangle (bounds, cornerRadius);
    }

    g.setColour ((hasKeyboardFocus (false) ? p.focus : p.controlBorder).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds.reduced (0.5f), cornerRadius, 1.0f);

    const juce::Point<float> arrow { bounds.getRight() - (float) Space::m, bounds.getCentreY() };
    juce::Path chevron;
    chevron.startNewSubPath (arrow.x - arrowSize / 2.0f, arrow.y - arrowSize / 4.0f);
    chevron.lineTo (arrow.x, arrow.y + arrowSize / 4.0f);
    chevron.lineTo (arrow.x + arrowSize / 2.0f, arrow.y - arrowSize / 4.0f);
    g.setColour (p.text.withMultipliedAlpha (alpha));
    g.strokePath (chevron, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The track's dot, then its name; or a hollow dot and None while it listens to no other track.
    auto area = getLocalBounds().withTrimmedRight (Space::xl).reduced (Space::s, 0);
    const auto dot = area.removeFromLeft ((int) dotSize).toFloat().withSizeKeepingCentre (dotSize, dotSize);
    area.removeFromLeft (Space::s);

    if (source.track.isValid())
    {
        g.setColour (source.colour.withMultipliedAlpha (alpha));
        g.fillEllipse (dot);
    }
    else
    {
        g.setColour (p.textMuted.withMultipliedAlpha (alpha));
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
    }

    g.setColour ((source.track.isValid() ? p.text : p.textMuted).withMultipliedAlpha (alpha));
    g.setFont (juce::FontOptions (std::min (TextSize::body, (float) getHeight() * 0.85f)));
    g.drawFittedText (getButtonText(), area, juce::Justification::centredLeft, 1);
}

void SidechainButton::clicked()
{
    // Under it, at least as wide, as a list's menu opens.
    getMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMinimumWidth (getWidth()));
}
} // namespace phazite::ui
