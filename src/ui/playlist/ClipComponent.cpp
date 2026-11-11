// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipComponent.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/timeline/TimelineEditor.h"

#include <cmath>
#include <utility>

namespace phazite::ui
{
namespace
{
    constexpr float corner = 3.0f;   // rounded corners
    constexpr int minNoteRange = 12; // semitones: a note preview spans at least an octave

    // A mark's drawing, made once, in the ink it is shown in.
    juce::Drawable* getMark (std::unique_ptr<juce::Drawable>& drawing, juce::Colour& drawnIn, Icon icon, juce::Colour ink)
    {
        if (drawing == nullptr)
            drawing = createIcon (icon);

        if (drawing != nullptr && ink != drawnIn)
        {
            drawing->replaceColour (drawnIn, ink);
            drawnIn = ink;
        }

        return drawing.get();
    }
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
    setTitle (info.name); // what screen readers call it, and what its marks show
    describe();

    // A recording's notes are given to it as they are played (showNotes()).
    if (info.kind == ClipType::midi && ! isRecording())
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

void ClipComponent::showRecording (int take)
{
    if (std::exchange (recordingTake, std::max (1, take)) != recordingTake)
    {
        describe();
        repaint();
    }
}

void ClipComponent::showNotes (std::vector<NoteShape> shown)
{
    notes = std::move (shown);
    repaint();
}

void ClipComponent::showRecordingEnd (std::optional<float> end)
{
    if (end != recordingEnd)
    {
        recordingEnd = end;
        repaint();
    }
}

void ClipComponent::showLook (juce::Colour newColour, bool isSelected, bool isFrozen, bool isTrackSilent)
{
    if (colour != newColour || selected != isSelected || frozen != isFrozen || trackSilent != isTrackSilent)
    {
        colour = newColour;
        selected = isSelected;
        frozen = isFrozen;

        if (std::exchange (trackSilent, isTrackSilent) != isTrackSilent)
            describe();

        repaint();
    }
}

void ClipComponent::describe()
{
    if (isRecording())
    {
        setDescription ("Being recorded: it is kept as you see it when recording stops");
        return;
    }

    juce::StringArray about;

    if (rendering.has_value())
        about.add ("Being stretched again: silent until it is ready");
    else if (info.followsTempo)
        about.add ("Stretches with the song's tempo");

    if (info.linked)
        about.add ("An alias: it plays what its copies play");

    if (info.muted)
        about.add ("Muted: it doesn't play");
    else if (trackSilent)
        about.add ("Its track isn't heard now: it doesn't play");

    setDescription (about.joinIntoString (". "));
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

juce::Rectangle<int> ClipComponent::getShape() const
{
    // A pixel short of its end, where what is behind shows as a line between it and a clip right after it: their two edges
    // side by side looked like one thick one (the owner, 2026-11-07). A clip two pixels wide keeps both.
    return getLocalBounds().withTrimmedRight (getWidth() > 2 ? 1 : 0);
}

juce::Colour ClipComponent::getInk() const
{
    // For what shows: the fill over the playlist's background, fainter where the clip is faint.
    const auto alpha = ClipTint::body * (isFaint() ? ClipTint::muted : 1.0f);
    return getInkFor (getPalette (*this).background.overlaidWith (getFill().withAlpha (alpha)));
}

//==============================================================================
void ClipComponent::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto shape = getShape();
    auto body = shape.toFloat().reduced (0.5f);
    const auto fill = getFill();
    const auto ink = getInk();

    // A recording growing with the playhead ends at its line (showRecordingEnd()), as a clip ends a pixel and a half short of
    // its end's column.
    if (recordingEnd.has_value())
        body.setRight (juce::jlimit (body.getX() + 1.0f, std::max (body.getX() + 1.0f, body.getRight()), *recordingEnd - 1.5f));

    // A clip that doesn't play, muted or on a track that isn't heard, all of it faint, as a folder's row and the overview draw a
    // muted one; a selected one's outline stays as it is.
    const auto faint = isFaint();

    if (faint)
        g.beginTransparencyLayer (ClipTint::muted);

    // See-through, so that the grid shows through it.
    g.setColour (fill.withAlpha (ClipTint::body));
    g.fillRoundedRectangle (body, corner);

    // What it holds fills it, without a strip for its name, which its track's header shows (docs/design.md, section 6).
    const auto content = shape.reduced (2);

    if (info.kind == ClipType::audio)
    {
        // The part of it in view, which stays the same while the playlist does (ClipWaveform.h).
        if (const auto* parent = getParentComponent(); waveform != nullptr && parent != nullptr)
        {
            g.setColour (ink.withAlpha (rendering.has_value() ? RenderingTint::waveform : 0.7f));
            waveform->paint (g, content.getIntersection (getLocalArea (parent, parent->getLocalBounds())), getWidth());
        }

        paintFades (g, shape);

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

    // A frozen track's clips are frosted: its file plays, not them. The stripes inside its outline: clipped to its rectangle,
    // which the renderer rounds out to whole pixels while the clip is a part of one off them (scrolling continuously), they
    // reached into the line between it and a clip right after it, more at one frame than the next. Only those across what is
    // being painted: a clip too long to keep as a picture is painted again at each frame.
    if (frozen)
    {
        juce::Path outline;
        outline.addRoundedRectangle (body, corner);
        const juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (outline);
        g.setColour (p.frozen.withAlpha (FrozenTint::frost));
        g.fillPath (outline);
        g.setColour (p.frozen.withAlpha (FrozenTint::stripes));

        const auto painted = g.getClipBounds().toFloat();
        const auto height = body.getHeight();

        for (auto stripe = std::max (0, (int) std::floor (painted.getX() / FrozenTint::stripeSpacing));; ++stripe)
        {
            const auto x = -height + (float) stripe * FrozenTint::stripeSpacing;

            if (x >= body.getWidth() || x > painted.getRight())
                break;

            g.drawLine (x, body.getBottom(), x + height, body.getY(), FrozenTint::stripeWidth);
        }
    }

    paintTags (g, shape);

    if (faint)
        g.endTransparencyLayer();

    // Its edge in its colour, opaque (faint where it is); a selected clip's in the text color, which shows on any clip color; a
    // recording's in the danger colour, as record buttons are.
    if (isRecording())
    {
        g.setColour (p.danger);
        g.drawRoundedRectangle (body.reduced (0.5f), corner, RecordingTint::edge);
        return;
    }

    g.setColour (selected ? p.text : colour.withMultipliedAlpha (faint ? ClipTint::muted : 1.0f));
    g.drawRoundedRectangle (body, corner, selected ? 2.0f : 1.0f);
}

juce::String ClipComponent::getLabel() const
{
    if (isRecording())
        return recordingTake > 1 ? "Recording take " + juce::String (recordingTake) : juce::String ("Recording");

    auto label = info.namedLikeTrack ? juce::String() : info.name;

    if (info.takes > 1)
        label << (label.isEmpty() ? "" : "  ") << "Take " << info.take << "/" << info.takes;

    return label;
}

void ClipComponent::paintTags (juce::Graphics& g, juce::Rectangle<int> shape)
{
    if (shape.getHeight() < ClipLabelTint::minimumClipHeight)
        return;

    // In the part of it in view, so that they stay in view as it scrolls past the playlist's edges, as FL Studio keeps a clip's
    // label on screen: across, the time line's window, on the screen's whole pixels, which scrolling continuously moves the clip
    // under by parts of one (getShownTimeline()), so that the tags stay put there rather than going back and forth by up to a
    // pixel at every frame; down, its layer, under the pinned tracks' band.
    auto area = shape.toFloat().getIntersection (getShownTimeline (*this));

    if (const auto* parent = getParentComponent())
        area = area.getIntersection (getLocalArea (parent, parent->getLocalBounds()).toFloat());

    area = area.reduced ((float) ClipLabelTint::margin).withHeight ((float) ClipLabelTint::height);

    if (area.getWidth() < (float) ClipLabelTint::height)
        return;

    // Each tag in the clip's colour, opaque, so that what it holds doesn't show through what the tag says.
    const auto tag = getFill();
    const auto ink = getInkFor (tag);
    const auto fillTag = [&g, tag] (juce::Rectangle<float> box)
    {
        g.setColour (tag);
        g.fillRoundedRectangle (box, ClipLabelTint::corner);
    };

    // At the right, its marks: an alias's link, and the metronome of a sample that stretches with the tempo.
    std::vector<juce::Drawable*> marks;

    if (info.linked)
        marks.push_back (getMark (aliasMark, aliasMarkColour, Icon::alias, ink));

    if (info.followsTempo)
        marks.push_back (getMark (tempoMark, tempoMarkColour, Icon::metronome, ink));

    std::erase (marks, nullptr);
    const auto iconSize = (float) ClipLabelTint::iconSize, iconGap = (float) ClipLabelTint::iconGap;
    const auto marksWidth = (float) marks.size() * (iconSize + iconGap) + iconGap;

    if (! marks.empty() && marksWidth <= area.getWidth())
    {
        auto box = area.removeFromRight (marksWidth);
        fillTag (box);
        box.removeFromLeft (iconGap);

        for (auto* mark : marks)
        {
            const auto slot = box.removeFromLeft (iconSize);
            mark->drawWithin (g, slot.withSizeKeepingCentre (iconSize, iconSize), juce::RectanglePlacement::centred, 1.0f);
            box.removeFromLeft (iconGap);
        }

        area.removeFromRight (iconGap);
    }

    // At the left, its name, as wide as it is or as there is room for, where there is room for a few letters.
    if (const auto label = getLabel(); label.isNotEmpty())
    {
        const juce::Font font { juce::FontOptions (TextSize::small) };
        const auto inset = (float) ClipLabelTint::inset;
        const auto width = std::ceil (juce::GlyphArrangement::getStringWidth (font, label)) + 2.0f * inset;
        const auto box = area.withWidth (std::min (area.getWidth(), width));

        if (box.getWidth() >= 2.0f * inset + (float) ClipLabelTint::height)
        {
            fillTag (box);
            g.setColour (ink);
            g.setFont (font);
            g.drawText (label, box.reduced (inset, 0.0f), juce::Justification::centredLeft, true);
        }
    }
}

void ClipComponent::paintFades (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto width = (float) area.getWidth(), height = (float) area.getHeight();
    const auto fadeIn = (float) info.fadeIn * width;
    const auto fadeOut = (float) info.fadeOut * width;

    if (fadeIn > 0.0f)
        paintFade (g, 0.0f, fadeIn, 0.0f, height, info.fadeInCurve);

    if (fadeOut > 0.0f)
        paintFade (g, width, width - fadeOut, 0.0f, height, info.fadeOutCurve);
}

void paintFade (juce::Graphics& g, float x0, float x1, float top, float bottom, FadeCurve shape)
{
    // The curve, from x0 (silence) to x1 (full volume), and the part of the clip above it.
    juce::Path curve;
    constexpr int steps = 32;

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
    const auto end = (float) getShape().getRight(); // short of the line after it

    // Only those where it is being painted: a clip too long to keep as a picture is painted again at each frame the song
    // scrolls, and most of it is out of view (docs/performance-audit.md, section 16).
    const auto painted = g.getClipBounds().toFloat();
    g.setColour (ink);

    for (const auto& note : notes)
    {
        const auto left = std::max (0.0f, beatToX (note.start));
        const auto right = std::min (end, beatToX (note.end));

        if (right <= left)
            continue; // outside the clip

        const auto top = (float) area.getBottom() - (float) (note.pitch - bottomNote + 1.0) * noteHeight;

        if (const juce::Rectangle<float> box (left, top, std::max (1.0f, right - left), std::max (1.0f, noteHeight - 1.0f));
            box.intersects (painted))
            g.fillRect (box);
    }
}
} // namespace phazite::ui
