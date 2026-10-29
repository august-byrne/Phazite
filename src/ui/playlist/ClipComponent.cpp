// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipComponent.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

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
    setDescription (rendering.has_value() ? "Being stretched again: silent until it is ready"
                    : info.followsTempo   ? "Stretches with the song's tempo"
                                          : juce::String());

    if (info.kind == ClipType::midi)
        notes = getNoteShapes (context, id);

    repaint();
}

void ClipComponent::showWaveform (ClipWaveform* newWaveform)
{
    if (waveform != newWaveform)
    {
        waveform = newWaveform;
        repaint();
    }
}

void ClipComponent::showLook (juce::Colour newColour, bool isSelected, bool isFrozen)
{
    if (colour != newColour || selected != isSelected || frozen != isFrozen)
    {
        colour = newColour;
        selected = isSelected;
        frozen = isFrozen;
        repaint();
    }
}

void ClipComponent::showRendering (std::optional<double> progress)
{
    if (progress.has_value())
        progress = std::round (*progress * 100.0) / 100.0;

    if (progress == rendering)
        return;

    const auto changedState = progress.has_value() != rendering.has_value();
    rendering = progress;

    if (changedState)
        showClip (info); // its description
    else
        repaint (getLocalBounds().removeFromBottom (juce::roundToInt (RenderingTint::barHeight) + 2));
}

juce::Colour ClipComponent::getFill() const
{
    return selected ? colour.brighter (0.3f) : colour;
}

juce::Colour ClipComponent::getInk() const
{
    return getInkFor (getFill());
}

//==============================================================================
void ClipComponent::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto body = getLocalBounds().toFloat().reduced (0.5f);
    const auto fill = getFill();
    const auto ink = getInk();

    g.setColour (fill);
    g.fillRoundedRectangle (body, corner);

    // The name, in a strip along the top. An alias (a linked copy) has a star after its name. A sample that follows the
    // song's tempo, and so stretches when it changes, shows the metronome at the strip's end (docs/design.md, section 4).
    auto content = getLocalBounds().reduced (2);

    if (getHeight() >= nameHeight * 2)
    {
        auto strip = content.removeFromTop (nameHeight);

        if (info.followsTempo && strip.getWidth() > 3 * nameHeight)
        {
            if (tempoMark == nullptr)
                tempoMark = createIcon (Icon::metronome);

            if (tempoMark != nullptr)
            {
                if (ink != tempoMarkColour)
                {
                    tempoMark->replaceColour (tempoMarkColour, ink);
                    tempoMarkColour = ink;
                }

                tempoMark->drawWithin (g, strip.removeFromRight (nameHeight).reduced (2).toFloat(), juce::RectanglePlacement::centred,
                                       1.0f);
            }
        }

        g.setColour (ink);
        g.setFont (juce::FontOptions (TextSize::small));

        // A clip recorded round a looped range: the take it plays, of how many, at the strip's end (its menu chooses).
        if (info.takes > 1)
        {
            const auto takeText = "Take " + juce::String (info.take) + "/" + juce::String (info.takes);
            const auto takeWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), takeText)) + 4;

            if (strip.getWidth() > takeWidth + 3 * nameHeight)
                g.drawText (takeText, strip.removeFromRight (takeWidth), juce::Justification::centredRight, false);
        }

        g.drawText (info.name + (info.linked ? "*" : ""), strip.reduced (2, 0), juce::Justification::centredLeft, true);
    }

    if (info.kind == ClipType::audio)
    {
        // The part of it in view, which stays the same while the playlist does (ClipWaveform.h).
        if (const auto* parent = getParentComponent(); waveform != nullptr && parent != nullptr)
        {
            g.setColour (ink.withAlpha (rendering.has_value() ? RenderingTint::waveform : 0.7f));
            waveform->paint (g, content.getIntersection (getLocalArea (parent, parent->getLocalBounds())), getWidth());
        }

        paintFades (g, getLocalBounds());

        // Silent while its render is being made: how far that has come, along its bottom edge.
        if (rendering.has_value())
        {
            const auto bar = body.reduced (2.0f, 0.0f).removeFromBottom (RenderingTint::barHeight + 1.0f).withTrimmedBottom (1.0f);
            g.setColour (ink.withAlpha (RenderingTint::barTrack));
            g.fillRect (bar);
            g.setColour (ink);
            g.fillRect (bar.withWidth (bar.getWidth() * (float) *rendering));
        }
    }
    else
    {
        paintNotes (g, content, ink.withAlpha (0.8f));
    }

    // A frozen track's clips are frosted: its file plays, not them.
    if (frozen)
    {
        g.saveState();
        g.reduceClipRegion (getLocalBounds());
        g.setColour (p.frozen.withAlpha (FrozenTint::frost));
        g.fillRoundedRectangle (body, corner);
        g.setColour (p.frozen.withAlpha (FrozenTint::stripes));

        for (auto x = -body.getHeight(); x < body.getWidth(); x += FrozenTint::stripeSpacing)
            g.drawLine (x, body.getBottom(), x + body.getHeight(), body.getY(), FrozenTint::stripeWidth);

        g.restoreState();
    }

    // A selected clip has an outline in the text color, which shows on any clip color.
    g.setColour (selected ? p.text : colour.darker (0.5f));
    g.drawRoundedRectangle (body, corner, selected ? 2.0f : 1.0f);
}

void ClipComponent::paintFades (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto width = (float) area.getWidth(), height = (float) area.getHeight();
    const auto fadeIn = (float) info.fadeIn * width;
    const auto fadeOut = (float) info.fadeOut * width;

    if (fadeIn > 0.0f)
        paintFade (g, 0.0f, fadeIn, 0.0f, height, info.fadeInShape);

    if (fadeOut > 0.0f)
        paintFade (g, width, width - fadeOut, 0.0f, height, info.fadeOutShape);
}

void paintFade (juce::Graphics& g, float x0, float x1, float top, float bottom, FadeShape shape)
{
    // The curve, from x0 (silence) to x1 (full volume), and the part of the clip above it.
    juce::Path curve;
    constexpr int steps = 16;

    for (int i = 0; i <= steps; ++i)
    {
        const auto alpha = (float) i / (float) steps;
        const auto point = juce::Point<float> (x0 + (x1 - x0) * alpha, bottom - (bottom - top) * getFadeGain (shape, alpha));

        if (i == 0)
            curve.startNewSubPath (point);
        else
            curve.lineTo (point);
    }

    auto shade = curve;
    shade.lineTo (x1, top);
    shade.lineTo (x0, top);
    shade.closeSubPath();

    g.setColour (juce::Colours::black.withAlpha (FadeTint::shade));
    g.fillPath (shade);
    g.setColour (juce::Colours::black.withAlpha (FadeTint::curve));
    g.strokePath (curve, juce::PathStrokeType (FadeTint::curveWidth));
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
