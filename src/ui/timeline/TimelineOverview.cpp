// SPDX-License-Identifier: AGPL-3.0-or-later
// A time line editor's overview of all it shows, along the top of its time bar (TimelineEditor.h: getOverview()).
#include "TimelineEditor.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr float edgeGrab = 5.0f;   // how near the thumb's end a press grabs it, to zoom
    constexpr double leastBeats = 1.0; // the least the thumb shrinks to, zooming
} // namespace

struct TimelineEditor::Overview : public juce::Component
{
    explicit Overview (TimelineEditor& e)
        : editor (e)
    {
        setTitle ("Overview");
        setDescription ("All of it in miniature, with the part in view over it: drag that part to scroll, either of its ends to "
                        "zoom, or press elsewhere to go there");
        setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    }

    enum class Drag
    {
        none,
        move,  // the thumb, to scroll
        start, // its left end, to zoom
        end    // its right end
    };

    juce::Range<double> getRange() const { return editor.horizontalScrollBar.getRangeLimit(); } // the time line's, as its scroll bar has it
    juce::Range<double> getViewRange() const { return editor.horizontalScrollBar.getCurrentRange(); }

    float xFor (double beat) const
    {
        const auto whole = getRange();
        return whole.getLength() > 0.0 ? (float) ((beat - whole.getStart()) / whole.getLength() * getWidth()) : 0.0f;
    }

    // The beat at an x over a range: the time line's now, or the one a drag began over.
    double beatAt (float x, juce::Range<double> over) const
    {
        return over.getStart() + (double) x / std::max (1, getWidth()) * over.getLength();
    }

    juce::Rectangle<float> getThumbArea() const
    {
        const auto shown = getViewRange();
        const auto left = xFor (shown.getStart()), right = std::max (left + 4.0f, xFor (shown.getEnd()));
        return juce::Rectangle<float>::leftTopRightBottom (left, 1.0f, right, (float) getHeight() - 1.0f);
    }

    Drag getDragAt (juce::Point<float> position) const
    {
        const auto thumb = getThumbArea();

        if (std::abs (position.x - thumb.getX()) <= edgeGrab)
            return Drag::start;

        if (std::abs (position.x - thumb.getRight()) <= edgeGrab)
            return Drag::end;

        return thumb.contains (position) ? Drag::move : Drag::none;
    }

    void contentChanged()
    {
        marks = editor.getOverviewMarks();
        picture = {};
        repaint();
    }

    void drawPicture (float scale)
    {
        // The marks, each a sliver at least a pixel wide, faint while muted: drawn once for a range and a size, so that
        // scrolling only moves the thumb over them.
        const auto wide = juce::roundToInt ((float) getWidth() * scale), high = juce::roundToInt ((float) getHeight() * scale);
        picture = juce::Image (juce::Image::ARGB, std::max (1, wide), std::max (1, high), true);
        drawnRange = getRange();
        drawnScale = scale;

        if (marks.empty() || drawnRange.getLength() <= 0.0)
            return;

        juce::Graphics g (picture);
        const auto top = 2.0f * scale, tall = (float) high - 4.0f * scale;

        for (const auto& mark : marks)
        {
            const auto left = (float) ((mark.start - drawnRange.getStart()) / drawnRange.getLength() * wide);
            const auto right = std::max (left + 1.0f, (float) ((mark.end - drawnRange.getStart()) / drawnRange.getLength() * wide));
            const auto y = top + mark.top * tall, bottom = std::max (y + 1.0f, top + mark.bottom * tall);
            g.setColour (mark.colour.withAlpha (mark.faint ? OverviewTint::muted : OverviewTint::clip));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (left, y, right, bottom));
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        g.fillAll (p.surfaceRaised); // the time bar's, as the ruler under it is

        const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (picture.isNull() || drawnRange != getRange() || ! juce::approximatelyEqual (drawnScale, scale)
            || picture.getWidth() != juce::roundToInt ((float) getWidth() * scale))
            drawPicture (scale);

        g.drawImageTransformed (picture, juce::AffineTransform::scale (1.0f / scale));

        // The part in view: a see-through thumb, brighter under the pointer and while it is dragged, as FL Studio's.
        const auto thumb = getThumbArea();
        const auto lit = drag != Drag::none || hovered != Drag::none;
        g.setColour (p.text.withAlpha (lit ? OverviewTint::thumbLit : OverviewTint::thumb));
        g.fillRoundedRectangle (thumb, cornerRadius);
        g.setColour (p.text.withAlpha (OverviewTint::thumbEdge));
        g.drawRoundedRectangle (thumb.reduced (0.5f), cornerRadius, 1.0f);
    }

    void resized() override { picture = {}; }
    void lookAndFeelChanged() override { contentChanged(); } // its marks in the new theme's colours

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (const auto at = getDragAt (e.position); at != hovered)
        {
            hovered = at;
            setMouseCursor (at == Drag::start || at == Drag::end ? juce::MouseCursor::LeftRightResizeCursor
                                                                 : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hovered = Drag::none;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! editor.timelineContext.hasProject())
            return;

        // Away from the thumb, that part comes into view, centred where the time line has room, and the drag goes on from
        // there.
        drag = getDragAt (e.position);

        if (drag == Drag::none)
        {
            const auto whole = getRange();
            const auto length = getViewRange().getLength();
            const auto latest = std::max (whole.getStart(), whole.getEnd() - length);
            editor.pauseFollowing();
            editor.setViewStart (
                BeatPosition::fromBeats (juce::jlimit (whole.getStart(), latest, beatAt (e.position.x, whole) - length / 2.0)));
            drag = Drag::move;
        }

        rangeAtDragStart = getRange();
        viewAtDragStart = getViewRange();
        beatAtDragStart = beatAt (e.position.x, rangeAtDragStart);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag == Drag::none)
            return;

        // Over the time line as the drag began, and within it. The view grows the time line as it moves past its end, so a
        // pointer read over the time line as it is now went further with each move: carried far past the strip's end, it
        // took the view out to beats too far to draw, and Phazite froze (the owner, 2026-11-06).
        const auto whole = rangeAtDragStart;
        const auto moved = beatAt (e.position.x, whole) - beatAtDragStart;
        editor.pauseFollowing();

        if (drag == Drag::move)
        {
            const auto latest = std::max (whole.getStart(), whole.getEnd() - viewAtDragStart.getLength());
            editor.setViewStart (BeatPosition::fromBeats (juce::jlimit (whole.getStart(), latest, viewAtDragStart.getStart() + moved)));
            return;
        }

        // An end dragged zooms, the other end staying where it was, as far as the time line's ends.
        auto start = viewAtDragStart.getStart(), end = viewAtDragStart.getEnd();

        if (drag == Drag::start)
            start = juce::jlimit (whole.getStart(), std::max (whole.getStart(), end - leastBeats), start + moved);
        else
            end = juce::jlimit (start + leastBeats, std::max (start + leastBeats, whole.getEnd()), end + moved);

        editor.zoomTo ({ BeatPosition::fromBeats (start), BeatPosition::fromBeats (end) });
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag = Drag::none;
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        editor.scrollByWheel (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY);
    }

    TimelineEditor& editor;
    std::vector<OverviewMark> marks;
    juce::Image picture;            // the marks, drawn over the range they were drawn for, at the display's scale
    juce::Range<double> drawnRange; // that range
    float drawnScale = 0.0f;
    Drag drag = Drag::none, hovered = Drag::none;
    juce::Range<double> rangeAtDragStart, viewAtDragStart; // the time line's and the part in view, as the drag began
    double beatAtDragStart = 0.0;
};

//==============================================================================
void TimelineEditor::overviewContentChanged()
{
    if (auto* shown = dynamic_cast<Overview*> (overview.get()))
        shown->contentChanged();
}

juce::Component& TimelineEditor::getOverview() noexcept
{
    if (overview == nullptr)
        overview = std::make_unique<Overview> (*this);

    return *overview;
}

void TimelineEditor::makeSeeThrough (juce::ScrollBar& bar)
{
    const auto& p = getPalette (bar);
    bar.setColour (juce::ScrollBar::thumbColourId, p.scrollThumb.withMultipliedAlpha (ScrollTint::overlayThumb));
    bar.setColour (juce::ScrollBar::trackColourId, p.text.withAlpha (ScrollTint::overlayTrack));
}
} // namespace phazite::ui
