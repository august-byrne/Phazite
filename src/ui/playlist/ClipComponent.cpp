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

ClipComponent::ClipComponent (te::Clip& c)
    : clip (c)
{
    setInterceptsMouseClicks (false, false); // the playlist handles the mouse
    setTitle (c.getName());
}

ClipComponent::~ClipComponent() = default;

void ClipComponent::update (juce::Colour newColour, bool isSelected)
{
    auto* c = clip.get();

    if (c == nullptr)
        return;

    setTitle (c->getName()); // what screen readers call it

    if (auto* audio = dynamic_cast<te::AudioClipBase*> (c))
        updateThumbnail (*audio);

    colour = newColour;
    selected = isSelected;
    repaint();
}

void ClipComponent::updateThumbnail (te::AudioClipBase& audio)
{
    const auto file = audio.getPlaybackFile();

    if (thumbnail != nullptr && file.getFile() == thumbnailFile)
        return;

    thumbnailFile = file.getFile();

    if (thumbnail == nullptr)
        thumbnail = std::make_unique<te::SmartThumbnail> (audio.edit.engine, file, *this, &audio.edit); // repaints this as it loads
    else
        thumbnail->setNewFile (file);
}

double ClipComponent::xToTime (double x) const
{
    auto* c = clip.get();

    if (c == nullptr)
        return 0.0;

    // The clip's width spans its beats; the tempo turns a beat into a time.
    const auto beat = c->getStartBeat() + te::BeatDuration::fromBeats (c->getLengthInBeats().inBeats() * x / std::max (1, getWidth()));
    return c->edit.tempoSequence.toTime (beat).inSeconds();
}

//==============================================================================
void ClipComponent::paint (juce::Graphics& g)
{
    auto* c = clip.get();

    if (c == nullptr)
        return;

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
        g.drawText (c->getName() + (c->isLinked() ? "*" : ""), content.removeFromTop (nameHeight).reduced (2, 0),
                    juce::Justification::centredLeft, true);
    }

    if (auto* audio = dynamic_cast<te::AudioClipBase*> (c))
    {
        paintWaveform (g, *audio, content, ink.withAlpha (0.7f));
        paintFades (g, *audio, getLocalBounds());
    }
    else if (auto* midi = dynamic_cast<te::MidiClip*> (c))
    {
        paintNotes (g, *midi, content, ink.withAlpha (0.8f));
    }

    // A selected clip has an outline in the text color, which shows on any clip color.
    g.setColour (selected ? p.text : colour.darker (0.5f));
    g.drawRoundedRectangle (body, corner, selected ? 2.0f : 1.0f);
}

void ClipComponent::paintWaveform (juce::Graphics& g, te::AudioClipBase& audio, juce::Rectangle<int> area, juce::Colour ink)
{
    const auto visible = area.getIntersection (g.getClipBounds());

    if (thumbnail == nullptr || visible.isEmpty())
        return;

    // The time in the audio file at an x: from the clip's offset, at its speed. A time-stretched clip
    // plays a proxy file that is already at the Edit's tempo.
    const auto position = audio.getPosition();
    const auto speed = audio.usesTimeStretchedProxy() ? 1.0 : audio.getSpeedRatio();
    const auto sourceTime = [&] (int x)
    { return te::TimePosition::fromSeconds ((xToTime (x) - position.getStart().inSeconds() + position.getOffset().inSeconds()) * speed); };

    g.setColour (ink);
    thumbnail->drawChannels (g, visible, { sourceTime (visible.getX()), sourceTime (visible.getRight()) }, audio.getGain());
}

void ClipComponent::paintFades (juce::Graphics& g, te::AudioClipBase& audio, juce::Rectangle<int> area)
{
    const auto length = audio.getPosition().getLength().inSeconds();

    if (length <= 0.0)
        return;

    // What a fade takes away is shaded, above its curve from silence to full volume.
    const auto width = (float) area.getWidth(), height = (float) area.getHeight();
    const auto fadeIn = (float) (audio.getFadeIn().inSeconds() / length) * width;
    const auto fadeOut = (float) (audio.getFadeOut().inSeconds() / length) * width;

    // The curve, from x0 (silence) to x1 (full volume), and the part of the clip above it.
    const auto drawFade = [&g, height] (float x0, float x1, te::AudioFadeCurve::Type type)
    {
        juce::Path curve;
        constexpr int steps = 16;

        for (int i = 0; i <= steps; ++i)
        {
            const auto alpha = (float) i / (float) steps;
            const auto point =
                juce::Point<float> (x0 + (x1 - x0) * alpha, height * (1.0f - te::AudioFadeCurve::alphaToGainForType (type, alpha)));

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
        drawFade (0.0f, fadeIn, audio.getFadeInType());

    if (fadeOut > 0.0f)
        drawFade (width, width - fadeOut, audio.getFadeOutType());
}

void ClipComponent::paintNotes (juce::Graphics& g, te::MidiClip& midi, juce::Rectangle<int> area, juce::Colour ink)
{
    const auto& notes = midi.getSequence().getNotes();

    if (notes.isEmpty() || area.getHeight() < 4)
        return;

    // The notes' pitches fill the height, over at least an octave so that a few notes don't fill it all.
    auto lowest = 127, highest = 0;

    for (auto* note : notes)
    {
        lowest = std::min (lowest, note->getNoteNumber());
        highest = std::max (highest, note->getNoteNumber());
    }

    const auto range = std::max (minNoteRange, highest - lowest + 1);
    const auto bottomNote = (lowest + highest + 1 - range) / 2.0;
    const auto noteHeight = (float) area.getHeight() / (float) range;

    const auto clipStart = midi.getStartBeat();
    const auto clipLength = std::max (1.0e-6, midi.getLengthInBeats().inBeats());
    const auto contentStart = midi.getContentStartBeat();
    const auto beatToX = [&] (te::BeatPosition beat) { return (float) ((beat - clipStart).inBeats() / clipLength * getWidth()); };

    g.setColour (ink);

    for (auto* note : notes)
    {
        const auto start = contentStart + te::toDuration (note->getStartBeat());
        const auto left = std::max (0.0f, beatToX (start));
        const auto right = std::min ((float) getWidth(), beatToX (start + note->getLengthBeats()));

        if (right <= left)
            continue; // outside the clip

        const auto top = (float) area.getBottom() - (float) (note->getNoteNumber() - bottomNote + 1.0) * noteHeight;
        g.fillRect (juce::Rectangle<float> (left, top, std::max (1.0f, right - left), std::max (1.0f, noteHeight - 1.0f)));
    }
}
} // namespace phazite::ui
