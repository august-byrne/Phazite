// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipComponent.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int nameHeight = 16;   // the strip with the clip's name, in clips tall enough for one
    constexpr float corner = 3.0f;   // rounded corners
    constexpr int minNoteRange = 12; // semitones: a note preview spans at least an octave
} // namespace

ClipComponent::ClipComponent (ProjectContext& projectContext, ClipID clip)
    : context (projectContext),
      id (clip)
{
    setInterceptsMouseClicks (false, false); // the playlist handles the mouse
}

ClipComponent::~ClipComponent() = default;

void ClipComponent::showClip (const ClipInfo& shown)
{
    info = shown;
    setTitle (info.name); // what screen readers call it

    if (info.kind == ClipType::midi)
        notes = getNoteShapes (context, id);
    else if (waveform == nullptr)
        waveform = makeClipWaveform (context, id, *this);

    repaint();
}

void ClipComponent::showLook (juce::Colour newColour, bool isSelected)
{
    if (colour != newColour || selected != isSelected)
    {
        colour = newColour;
        selected = isSelected;
        repaint();
    }
}

//==============================================================================
void ClipComponent::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto body = getLocalBounds().toFloat().reduced (0.5f);
    const auto fill = selected ? colour.brighter (0.3f) : colour;
    const auto ink = getInkFor (fill);

    g.setColour (fill);
    g.fillRoundedRectangle (body, corner);

    // The name, in a strip along the top. An alias (a linked copy) has a star after its name.
    auto content = getLocalBounds().reduced (2);

    if (getHeight() >= nameHeight * 2)
    {
        g.setColour (ink);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (info.name + (info.linked ? "*" : ""), content.removeFromTop (nameHeight).reduced (2, 0),
                    juce::Justification::centredLeft, true);
    }

    if (info.kind == ClipType::audio)
    {
        if (waveform != nullptr)
        {
            g.setColour (ink.withAlpha (0.7f));
            waveform->paint (g, content, getWidth());
        }

        paintFades (g, getLocalBounds());
    }
    else
    {
        paintNotes (g, content, ink.withAlpha (0.8f));
    }

    // A selected clip has an outline in the text color, which shows on any clip color.
    g.setColour (selected ? p.text : colour.darker (0.5f));
    g.drawRoundedRectangle (body, corner, selected ? 2.0f : 1.0f);
}

void ClipComponent::paintFades (juce::Graphics& g, juce::Rectangle<int> area)
{
    // What a fade takes away is shaded, above its curve from silence to full volume.
    const auto width = (float) area.getWidth(), height = (float) area.getHeight();
    const auto fadeIn = (float) info.fadeIn * width;
    const auto fadeOut = (float) info.fadeOut * width;

    // The curve, from x0 (silence) to x1 (full volume), and the part of the clip above it.
    const auto drawFade = [&g, height] (float x0, float x1, FadeShape shape)
    {
        juce::Path curve;
        constexpr int steps = 16;

        for (int i = 0; i <= steps; ++i)
        {
            const auto alpha = (float) i / (float) steps;
            const auto point = juce::Point<float> (x0 + (x1 - x0) * alpha, height * (1.0f - getFadeGain (shape, alpha)));

            if (i == 0)
                curve.startNewSubPath (point);
            else
                curve.lineTo (point);
        }

        auto shade = curve;
        shade.lineTo (x1, 0.0f);
        shade.lineTo (x0, 0.0f);
        shade.closeSubPath();

        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillPath (shade);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));
    };

    if (fadeIn > 0.0f)
        drawFade (0.0f, fadeIn, info.fadeInShape);

    if (fadeOut > 0.0f)
        drawFade (width, width - fadeOut, info.fadeOutShape);
}

void ClipComponent::paintNotes (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour ink)
{
    if (notes.empty() || area.getHeight() < 4)
        return;

    // The notes' pitches fill the height, over at least an octave so that a few notes don't fill it all.
    auto lowest = 127, highest = 0;

    for (const auto& note : notes)
    {
        lowest = std::min (lowest, note.pitch);
        highest = std::max (highest, note.pitch);
    }

    const auto range = std::max (minNoteRange, highest - lowest + 1);
    const auto bottomNote = (lowest + highest + 1 - range) / 2.0;
    const auto noteHeight = (float) area.getHeight() / (float) range;

    const auto clipLength = std::max (1.0e-6, info.range.getLength().inBeats());
    const auto beatToX = [&] (double beat) { return (float) (beat / clipLength * getWidth()); };

    g.setColour (ink);

    for (const auto& note : notes)
    {
        const auto left = std::max (0.0f, beatToX (note.start));
        const auto right = std::min ((float) getWidth(), beatToX (note.end));

        if (right <= left)
            continue; // outside the clip

        const auto top = (float) area.getBottom() - (float) (note.pitch - bottomNote + 1.0) * noteHeight;
        g.fillRect (juce::Rectangle<float> (left, top, std::max (1.0f, right - left), std::max (1.0f, noteHeight - 1.0f)));
    }
}
} // namespace phazite::ui
