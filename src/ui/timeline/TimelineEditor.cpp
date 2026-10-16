// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineEditor.h"
#include "TimelineGesture.h"
#include "model/transport/TransportModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int playheadWidth = 11;           // the marker in the ruler is this wide; the line runs down its middle
    constexpr double wheelNotch = 60.0 / 256.0; // one notch of a mouse wheel, as JUCE reports it on Windows
    constexpr double zoomPerNotch = 1.25;
    constexpr double scrollPerNotch = 48.0; // pixels
    constexpr double zoomStep = 1.5;        // for the zoom commands
    constexpr double minLineSpacing = 6.0;  // grid lines closer than this many pixels aren't drawn
    constexpr double minGridSpacing = 20.0; // adaptive grid lines at least this far apart, in pixels

    constexpr int autoScrollMargin = 16;             // pixels inside an edge where a drag starts to scroll
    constexpr double autoScrollSpeedPerPixel = 15.0; // pixels per second for each pixel nearer the edge, or past it
    constexpr double maxAutoScrollSpeed = 3000.0;    // pixels per second

    // How fast a drag at some position along an area scrolls it: toward the edge it is near or past, in pixels
    // per second (negative toward the start), or 0.
    double getAutoScrollSpeed (int position, int size)
    {
        const auto margin = std::min (autoScrollMargin, size / 4); // a small area keeps most of its room for dragging

        if (const auto pastStart = margin - position; pastStart > 0)
            return -std::min (maxAutoScrollSpeed, pastStart * autoScrollSpeedPerPixel);

        if (const auto pastEnd = position - (size - margin); pastEnd > 0)
            return std::min (maxAutoScrollSpeed, pastEnd * autoScrollSpeedPerPixel);

        return 0.0;
    }

    // How far a place is from the song's start, as a length: the time origin's, which the grid and the ruler count from.
    BeatDuration fromSongStart (BeatPosition position)
    {
        return position - BeatPosition();
    }

    bool isNear (std::optional<BeatRange> a, std::optional<BeatRange> b)
    {
        if (! a.has_value() || ! b.has_value())
            return a.has_value() == b.has_value();

        return std::abs ((a->getStart() - b->getStart()).inBeats()) < 1.0e-6 && std::abs ((a->getEnd() - b->getEnd()).inBeats()) < 1.0e-6;
    }

    juce::StringArray getToolSynonyms (Tool tool)
    {
        juce::StringArray synonyms { "tool", "pointer", "mode" };

        switch (tool)
        {
            case Tool::draw:
                synonyms.addArray ({ "pencil", "place", "stamp" });
                break;
            case Tool::paint:
                synonyms.addArray ({ "brush", "repeat", "fill" });
                break;
            case Tool::slice:
                synonyms.addArray ({ "cut", "knife", "split", "scissors" });
                break;
            case Tool::slip:
                synonyms.addArray ({ "slide", "content", "offset" });
                break;
            case Tool::mute:
                synonyms.addArray ({ "silence", "deactivate" });
                break;
            case Tool::erase:
                synonyms.addArray ({ "eraser", "remove", "delete" });
                break;
            case Tool::select:
                synonyms.addArray ({ "marquee", "box", "rectangle", "lasso" });
                break;
            case Tool::zoom:
                synonyms.addArray ({ "magnify", "magnifier" });
                break;
            case Tool::smart:
            default:
                synonyms.addArray ({ "arrow", "default", "normal" });
                break;
        }

        return synonyms;
    }

    Icon getToolIcon (Tool tool)
    {
        switch (tool)
        {
            case Tool::draw:
                return Icon::draw;
            case Tool::paint:
                return Icon::paint;
            case Tool::slice:
                return Icon::slice;
            case Tool::slip:
                return Icon::slip;
            case Tool::mute:
                return Icon::mute;
            case Tool::erase:
                return Icon::erase;
            case Tool::select:
                return Icon::select;
            case Tool::zoom:
                return Icon::zoom;
            case Tool::smart:
            default:
                return Icon::smartTool;
        }
    }
} // namespace

//==============================================================================
TimelineRuler::TimelineRuler (TimelineEditor& e)
    : editor (e)
{
    setTitle ("Timeline");
    setDescription ("Drag along the top to loop part of the song, or the loop's edges or middle to change it; click there to "
                    "turn looping on or off, double-click for the whole song. Lower down, click or drag to move the playhead. "
                    "Ctrl+drag makes a loop anywhere. It all snaps to the grid; hold Alt to place things freely. Roll the mouse "
                    "wheel to zoom.");
}

void TimelineRuler::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.border);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    if (! editor.hasProject())
        return;

    const auto& context = editor.getContext();
    const auto& view = editor.getView();
    const auto clip = g.getClipBounds();

    // Bars count from the time origin: the beats here are from there.
    const auto origin = fromSongStart (editor.getTimeOrigin());
    const BeatRange visible (view.xToBeat (clip.getX() - 1) - origin, view.xToBeat (clip.getRight() + 1) - origin);
    const auto xOf = [&view, origin] (BeatPosition beat) { return view.beatToX (beat + origin); };

    // The loop's lane along the top, and the loop in it, over a wash of the part that loops, under the ticks: song beats,
    // not counted from the origin.
    g.setColour (p.text.withAlpha (LoopTint::lane));
    g.fillRect (getLoopLane());

    if (const auto loop = getLoop (context); loop.has_value() && ! loop->isEmpty())
    {
        const auto loopStart = (float) view.beatToX (loop->getStart()), loopEnd = (float) view.beatToX (loop->getEnd());
        const juce::Rectangle<float> area (loopStart, 0.0f, loopEnd - loopStart, (float) getHeight() - 1.0f);
        g.setColour (p.loop.withAlpha (LoopTint::wash));
        g.fillRect (area);
        g.setColour (p.loop.withAlpha (LoopTint::bar));
        g.fillRect (area.withHeight ((float) LoopTint::laneHeight));
    }

    // Ticks: tall for bars, shorter for beats, shortest between them.
    const auto tickStep = getAdaptiveGridStep (context, visible.getStart(), view.pixelsPerBeat, false, minLineSpacing);

    forEachGridLine (context, tickStep, visible,
                     [&] (BeatPosition beat, GridLineType type)
                     {
                         const auto isBar = type == GridLineType::bar;
                         const auto tick = isBar ? getHeight() / 2 : (type == GridLineType::beat ? getHeight() / 4 : getHeight() / 8);
                         g.setColour (isBar ? p.textMuted : p.textMuted.withAlpha (0.4f));
                         g.fillRect (juce::roundToInt (xOf (beat)), getHeight() - 1 - tick, 1, tick);
                     });

    // Bar numbers, as far apart as they need to be to read: every bar, or every 2nd, 4th, ...
    const auto barWidth = view.pixelsPerBeat * getBeatsPerBar (context, visible.getStart());
    auto every = 1;

    while (every * barWidth < 32.0 && every < (1 << 20))
        every *= 2;

    // From a little before the area being drawn, so a number its edge cuts is drawn whole.
    auto bar = std::max (0, getBarAt (context, view.xToBeat (clip.getX() - 48) - origin));
    bar -= bar % every;

    g.setColour (p.textMuted);
    g.setFont (LookAndFeel::getNumberFont (TextSize::small));

    for (;; bar += every)
    {
        const auto x = (float) xOf (getBarStart (context, bar));

        if (x > (float) clip.getRight())
            break;

        g.drawText (
            juce::String (bar + 1),
            juce::Rectangle<float> (x + 4.0f, (float) LoopTint::laneHeight, 48.0f, (float) (getHeight() - LoopTint::laneHeight) / 2.0f),
            juce::Justification::centredLeft, false);
    }
}

juce::Rectangle<int> TimelineRuler::getLoopLane() const
{
    return getLocalBounds().removeFromTop (LoopTint::laneHeight);
}

double TimelineRuler::getBeatAt (const juce::MouseEvent& e) const
{
    return editor.snap (editor.getView().xToBeat (std::max (0.0f, e.position.x)), e.mods).inBeats();
}

TimelineRuler::LoopDrag TimelineRuler::getLoopDragAt (const juce::MouseEvent& e) const
{
    // Ctrl+drag makes a loop anywhere, as FL Studio's does; in the lane, a press on the loop changes it.
    const auto inLane = getLoopLane().contains (e.getPosition());

    if (! editor.hasProject() || ! (inLane || e.mods.isCommandDown()))
        return LoopDrag::none;

    const auto loop = getLoop (editor.getContext());

    if (! loop.has_value() || ! inLane || e.mods.isCommandDown())
        return LoopDrag::create;

    const auto& view = editor.getView();
    const auto startX = view.beatToX (loop->getStart()), endX = view.beatToX (loop->getEnd());
    const auto x = (double) e.position.x;

    if (std::abs (x - startX) <= LoopTint::edgeGrab)
        return LoopDrag::moveStart;

    if (std::abs (x - endX) <= LoopTint::edgeGrab)
        return LoopDrag::moveEnd;

    return x > startX && x < endX ? LoopDrag::move : LoopDrag::create;
}

void TimelineRuler::mouseMove (const juce::MouseEvent& e)
{
    switch (getLoopDragAt (e))
    {
        case LoopDrag::moveStart:
        case LoopDrag::moveEnd:
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            break;
        case LoopDrag::move:
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            break;
        case LoopDrag::create:
        case LoopDrag::none:
            setMouseCursor (juce::MouseCursor::NormalCursor);
            break;
    }
}

void TimelineRuler::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        editor.showRulerMenu (editor.getView().xToBeat (e.position.x));
        return;
    }

    beginDragAutoRepeat (TimelineEditor::autoRepeatInterval);
    loopDrag = getLoopDragAt (e);
    loopDragged = false;

    if (loopDrag == LoopDrag::none)
    {
        movePlayhead (e);
        return;
    }

    loopAnchor = getBeatAt (e);

    if (const auto loop = getLoop (editor.getContext()))
    {
        loopStartAtPress = loop->getStart().inBeats();
        loopEndAtPress = loop->getEnd().inBeats();
    }
}

void TimelineRuler::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;

    editor.autoScroll (e.getPosition(), getLocalBounds(), TimelineEditor::ScrollDirections::sideways);

    if (loopDrag == LoopDrag::none)
    {
        movePlayhead (e);
        return;
    }

    if (! loopDragged && ! TimelineGesture::isPastDragThreshold (e))
        return;

    loopDragged = true;
    dragLoop (e);
}

void TimelineRuler::dragLoop (const juce::MouseEvent& e)
{
    if (! editor.hasProject())
        return;

    const auto beat = getBeatAt (e);
    const auto loop = [this] (double start, double end)
    { loopRange (editor.getContext(), { BeatPosition::fromBeats (start), BeatPosition::fromBeats (end) }); };

    switch (loopDrag)
    {
        case LoopDrag::create:
            loop (std::min (loopAnchor, beat), std::max (loopAnchor, beat));
            break;
        case LoopDrag::moveStart:
            loop (std::min (beat, loopEndAtPress), loopEndAtPress);
            break;
        case LoopDrag::moveEnd:
            loop (loopStartAtPress, std::max (beat, loopStartAtPress));
            break;
        case LoopDrag::move:
        {
            // By whole steps of the grid from where it was, and not before the song's start.
            const auto distance = std::max (beat - loopAnchor, -loopStartAtPress);
            loop (loopStartAtPress + distance, loopEndAtPress + distance);
            break;
        }
        case LoopDrag::none:
            break;
    }

    repaint();
}

void TimelineRuler::mouseUp (const juce::MouseEvent& e)
{
    // A click in the lane, without a drag, turns looping on or off, as Logic Pro's does.
    if (loopDrag != LoopDrag::none && ! loopDragged && ! e.mods.isCommandDown() && ! e.mods.isPopupMenu())
        toggleLoop (editor.getContext());

    loopDrag = LoopDrag::none;
    repaint();
}

void TimelineRuler::mouseDoubleClick (const juce::MouseEvent& e)
{
    // The whole song again: the clicks before it turned looping off and on.
    if (getLoopLane().contains (e.getPosition()))
        loopSong (editor.getContext());

    repaint();
}

void TimelineRuler::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    editor.zoomAround (e.position.x, std::pow (zoomPerNotch, wheel.deltaY / wheelNotch));
}

void TimelineRuler::movePlayhead (const juce::MouseEvent& e)
{
    editor.movePlayheadTo (editor.snap (editor.getView().xToBeat (std::max (0.0f, e.position.x)), e.mods));
}

//==============================================================================
// The playhead: a line over the time line, with a marker in the ruler. Clicks go through it.
struct TimelineEditor::Playhead : public juce::Component
{
    Playhead()
    {
        setTitle ("Playhead");
        setInterceptsMouseClicks (false, false);
        setAlwaysOnTop (true);
    }

    // Fainter while it shows the notes being edited playing in a linked clip elsewhere, not the song's own position.
    void setLinked (bool shouldBeLinked)
    {
        if (linked != shouldBeLinked)
        {
            linked = shouldBeLinked;
            repaint();
        }
    }

    bool linked = false;

    void paint (juce::Graphics& g) override
    {
        const auto centre = (float) getWidth() / 2.0f;
        g.setColour (getPalette (*this).text.withMultipliedAlpha (linked ? linkedPlayheadAlpha : 1.0f));
        g.fillRect (juce::Rectangle<float> (centre - 0.75f, 0.0f, 1.5f, (float) getHeight()));

        juce::Path marker; // a triangle pointing down, at the bottom of the ruler
        marker.addTriangle (0.0f, (float) rulerHeight - 9.0f, (float) getWidth(), (float) rulerHeight - 9.0f, centre,
                            (float) rulerHeight - 1.0f);
        g.fillPath (marker);
    }
};

// Hears every mouse press and release in the editor and its children, for onUsed and the tools that spring
// back. A listener of its own: the editor listening to itself would also get every other mouse event twice,
// the mouse wheel's included.
struct TimelineEditor::UseListener : public juce::MouseListener
{
    explicit UseListener (TimelineEditor& e)
        : editor (e)
    {
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        editor.mouseIsDown = true;
        editor.usedWhileKeyDown = editor.toolBeforeKey.has_value();

        if (editor.onUsed != nullptr)
            editor.onUsed();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        editor.mouseIsDown = false;

        if (const auto back = editor.toolAfterDrag)
        {
            editor.toolAfterDrag.reset();
            editor.setTool (*back);
        }
    }

    TimelineEditor& editor;
};

//==============================================================================
TimelineEditor::TimelineEditor (CommandBridge& bridge, ProjectContext& context, GridOwner owner)
    : commands (bridge),
      gridOwner (owner),
      timelineContext (context),
      playhead (std::make_unique<Playhead>()),
      useListener (std::make_unique<UseListener> (*this))
{
    snapButton.onClick = [this] { commands.invoke ("grid.snap"); };
    gridButton.setTitle ("Grid");
    gridButton.onClick = [this] { showGridMenu(); };

    // The tools, one button each; the commands set them, so the keys and the buttons agree.
    for (const auto toolToShow : allTools)
    {
        auto* button = toolButtons.add (
            new IconButton (getToolName (toolToShow) + (toolToShow == Tool::smart ? "" : " tool"), getToolIcon (toolToShow)));
        button->setRadioGroupId (1);
        button->setClickingTogglesState (true);
        button->setToggleState (toolToShow == tool, juce::dontSendNotification);
        button->onClick = [this, toolToShow] { commands.invoke (getToolCommandID (toolToShow)); };
        addChildComponent (button);
    }

    // The vertical scroll bar hides while everything fits; the horizontal one always shows (the time line goes on).
    for (auto* scrollBar : { &horizontalScrollBar, &verticalScrollBar })
    {
        scrollBar->addListener (this);
        scrollBar->setAutoHide (scrollBar == &verticalScrollBar);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &snapButton, &gridButton, &ruler, &horizontalScrollBar, &verticalScrollBar,
                                                             playhead.get() })
        addAndMakeVisible (c);

    verticalScrollBar.addComponentListener (this);

    snapButton.setEnabled (false); // until a project is open
    gridButton.setEnabled (false);
    updateGridControls();
    addMouseListener (useListener.get(), true);
}

TimelineEditor::~TimelineEditor()
{
    removeMouseListener (useListener.get());
    verticalScrollBar.removeComponentListener (this);
    horizontalScrollBar.removeListener (this);
    verticalScrollBar.removeListener (this);
}

void TimelineEditor::componentVisibilityChanged (juce::Component& component)
{
    if (&component != &verticalScrollBar)
        return;

    // The time line takes the scroll bar's room, or gives it back. Laying out sets the bar's range again, which can
    // show or hide it once more (it showed while the rows had no room yet, say): then it lays out again. Whether the
    // bar is needed doesn't depend on its own room, so this settles at once.
    if (layingOutForScrollBar)
    {
        scrollBarChangedMeanwhile = true;
        return;
    }

    const juce::ScopedValueSetter<bool> layingOut (layingOutForScrollBar, true);

    for (int pass = 0; pass < 3; ++pass)
    {
        scrollBarChangedMeanwhile = false;
        resized();

        if (! scrollBarChangedMeanwhile)
            break;
    }
}

void TimelineEditor::showProject()
{
    const auto open = timelineContext.hasProject();
    view = {};
    timeOrigin = {};
    grid = loadGridSettings (timelineContext, gridOwner);
    snapButton.setEnabled (open);
    gridButton.setEnabled (open);

    updateGridControls();
    setViewStart ({});

    if (open)
        startTimerHz (5);
    else
        stopTimer();
}

//==============================================================================
void TimelineEditor::setViewStart (BeatPosition start)
{
    view.start = std::max (timeOrigin, start);
    ruler.repaint();
    timelineChanged();
    updatePlayhead();
    updateScrollBar();
}

void TimelineEditor::setTimeOrigin (BeatPosition newOrigin)
{
    timeOrigin = std::max (BeatPosition(), newOrigin);
    updateGridControls(); // the grid's size can depend on where bars start
    setViewStart (view.start);
}

void TimelineEditor::zoomAround (double x, double factor)
{
    view.zoomAround (x, factor);
    setViewStart (view.start);
    updateGridControls(); // an adaptive grid's size changes with the zoom
}

void TimelineEditor::zoom (double factor)
{
    const auto width = getTimelineArea().getWidth();
    auto x = width / 2.0;

    if (const auto playheadX = view.beatToX (getPlayheadBeat()); playheadX >= 0.0 && playheadX <= width)
        x = playheadX;

    zoomAround (x, factor);
}

void TimelineEditor::zoomTo (BeatRange range)
{
    if (range.getLength() <= BeatDuration())
        return;

    view.pixelsPerBeat = juce::jlimit (TimelineView::minPixelsPerBeat, TimelineView::maxPixelsPerBeat,
                                       std::max (1, getTimelineArea().getWidth()) / range.getLength().inBeats());
    setViewStart (range.getStart());
    updateGridControls();
}

void TimelineEditor::zoomToFit()
{
    // The content fills the width but for a margin at the right.
    const auto content = getContentRange();

    if (content.getLength() <= BeatDuration())
        return;

    const auto width = std::max (1, getTimelineArea().getWidth() - Space::l);
    view.pixelsPerBeat =
        juce::jlimit (TimelineView::minPixelsPerBeat, TimelineView::maxPixelsPerBeat, width / content.getLength().inBeats());
    setViewStart (content.getStart());
    updateGridControls();
}

void TimelineEditor::centreOnPlayhead()
{
    setViewStart (getPlayheadBeat() - BeatDuration::fromBeats (getTimelineArea().getWidth() / 2.0 / view.pixelsPerBeat));
}

void TimelineEditor::movePlayheadTo (BeatPosition beat)
{
    // Playback starts there from now on, and Space comes back there when it stops.
    phazite::movePlayhead (timelineContext, beat);
    updatePlayhead();
}

BeatPosition TimelineEditor::getPlayheadBeat() const
{
    return phazite::getPlayheadBeat (timelineContext);
}

bool TimelineEditor::isPlayheadShowingLinkedClip() const
{
    return playhead->linked;
}

void TimelineEditor::updatePlayhead()
{
    if (! timelineContext.hasProject())
    {
        playhead->setVisible (false);
        return;
    }

    // At the song's position, or where a linked clip's playing shows in this editor's notes.
    const auto area = getTimelineArea();
    const auto songBeat = getPlayheadBeat().inBeats();
    const auto linkedBeat = getLinkedPlayheadBeat (songBeat);
    const auto x = (linkedBeat.value_or (songBeat) - view.start.inBeats()) * view.pixelsPerBeat;
    const auto isInView = x >= -1.0 && x <= area.getWidth() + 1.0;
    playhead->setLinked (linkedBeat.has_value());
    playhead->setVisible (isInView);

    if (isInView)
        playhead->setBounds (area.getX() + juce::roundToInt (x) - playheadWidth / 2, ruler.getY(), playheadWidth,
                             area.getBottom() - ruler.getY());
}

void TimelineEditor::updateScrollBar()
{
    // From the time origin, the content plus some room after it.
    const auto visibleBeats = std::max (1, getTimelineArea().getWidth()) / view.pixelsPerBeat;
    contentEnd = getContentRange().getEnd().inBeats();
    const auto total = std::max ({ contentEnd + 64.0, view.start.inBeats() + visibleBeats, timeOrigin.inBeats() + 256.0 });
    horizontalScrollBar.setRangeLimits (timeOrigin.inBeats(), total, juce::dontSendNotification);
    horizontalScrollBar.setCurrentRange (view.start.inBeats(), visibleBeats, juce::dontSendNotification);
    horizontalScrollBar.setSingleStepSize (scrollPerNotch / view.pixelsPerBeat);
}

double TimelineEditor::getWheelNotches (float wheelDelta)
{
    return wheelDelta / wheelNotch;
}

void TimelineEditor::zoomByWheel (float x, float wheelDelta)
{
    zoomAround (x, std::pow (zoomPerNotch, getWheelNotches (wheelDelta)));
}

void TimelineEditor::scrollByWheel (float wheelDelta)
{
    setViewStart (view.xToBeat (-getWheelNotches (wheelDelta) * scrollPerNotch));
}

bool TimelineEditor::autoScroll (juce::Point<int> position, juce::Rectangle<int> area, ScrollDirections directions)
{
    // By the time since the last drag, so the speed doesn't depend on how often the mouse reports. The first
    // drag after a pause counts as a short one.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto seconds = juce::jlimit (1.0 / 60.0, 1.0 / 20.0, (now - lastAutoScrollTime) / 1000.0);
    lastAutoScrollTime = now;

    auto scrolled = false;

    if (directions != ScrollDirections::upAndDown)
    {
        if (const auto speed = getAutoScrollSpeed (position.x - area.getX(), area.getWidth()); ! juce::exactlyEqual (speed, 0.0))
        {
            const auto before = view.start.inBeats();
            setViewStart (view.xToBeat (speed * seconds));
            scrolled = ! juce::exactlyEqual (view.start.inBeats(), before);
        }
    }

    if (directions != ScrollDirections::sideways)
        if (const auto speed = getAutoScrollSpeed (position.y - area.getY(), area.getHeight()); ! juce::exactlyEqual (speed, 0.0))
            scrolled = scrollVerticallyBy (speed < 0.0 ? std::min (-1, juce::roundToInt (speed * seconds))
                                                       : std::max (1, juce::roundToInt (speed * seconds)))
                       || scrolled;

    return scrolled;
}

//==============================================================================
GridStep TimelineEditor::getGridStep() const
{
    return phazite::getGridStep (timelineContext, grid, view.start, view.pixelsPerBeat, minGridSpacing);
}

double TimelineEditor::getGridStepInBeats() const
{
    return getStepInBeats (timelineContext, getGridStep(), view.start);
}

BeatPosition TimelineEditor::snap (BeatPosition beat, juce::ModifierKeys mods) const
{
    if (! timelineContext.hasProject() || ! grid.snap || mods.isAltDown())
        return beat;

    // The grid counts from the time origin, as the ruler does.
    const auto origin = fromSongStart (timeOrigin);
    return snapToGrid (timelineContext, std::max (BeatPosition(), beat - origin), getGridStep()) + origin;
}

BeatPosition TimelineEditor::snapDown (BeatPosition beat, juce::ModifierKeys mods) const
{
    if (! timelineContext.hasProject() || ! grid.snap || mods.isAltDown())
        return beat;

    const auto origin = fromSongStart (timeOrigin);
    return snapDownToGrid (timelineContext, std::max (BeatPosition(), beat - origin), getGridStep()) + origin;
}

void TimelineEditor::setGrid (const GridSettings& newGrid)
{
    grid = newGrid;
    saveGridSettings (timelineContext, gridOwner, grid);
    updateGridControls();
    gridChanged();
}

void TimelineEditor::updateGridControls()
{
    snapButton.setToggleState (grid.snap, juce::dontSendNotification);

    juce::String text ("Grid: ");

    if (timelineContext.hasProject())
        text << describeGridStep (timelineContext, getGridStep());

    gridButton.setButtonText (grid.adaptive ? text + " (auto)" : text);
}

void TimelineEditor::showGridMenu()
{
    if (! timelineContext.hasProject())
        return;

    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "grid.snap" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "grid.adaptive", "grid.finer", "grid.coarser", "grid.triplets" });
    menu.addSectionHeader ("Fixed size");

    // Item IDs are the levels plus one, as an ID can't be 0; command items have much larger IDs.
    for (auto level = GridSettings::minFixedLevel; level <= GridSettings::maxFixedLevel; ++level)
        menu.addItem (level + 1, describeGridStep (timelineContext, { level, grid.triplets }), true,
                      ! grid.adaptive && grid.fixedLevel == level);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gridButton),
                        [safeThis = juce::Component::SafePointer<TimelineEditor> (this)] (int result)
                        {
                            const auto level = result - 1;

                            if (safeThis == nullptr || level < GridSettings::minFixedLevel || level > GridSettings::maxFixedLevel)
                                return;

                            auto newGrid = safeThis->grid;
                            newGrid.adaptive = false;
                            newGrid.fixedLevel = level;
                            safeThis->setGrid (newGrid);
                        });
}

//==============================================================================
void TimelineEditor::setTool (Tool newTool)
{
    if (! canUseTool (newTool))
        newTool = Tool::smart;

    // A held key repeats its command: choosing the tool in use again must not end what the mouse is doing.
    if (newTool == tool)
        return;

    tool = newTool;

    for (size_t i = 0; i < std::size (allTools); ++i)
        toolButtons[(int) i]->setToggleState (allTools[i] == tool, juce::dontSendNotification);

    toolChanged();
}

void TimelineEditor::pressTool (Tool newTool)
{
    toolBeforeKey = tool;
    usedWhileKeyDown = false;
    toolAfterDrag.reset();
    setTool (newTool);
}

void TimelineEditor::releaseTool()
{
    const auto back = std::exchange (toolBeforeKey, std::nullopt);

    // A tap keeps the tool; a hold while the mouse was used springs back.
    if (! back.has_value() || ! usedWhileKeyDown)
        return;

    if (mouseIsDown)
        toolAfterDrag = back;
    else
        setTool (*back);
}

juce::Rectangle<int> TimelineEditor::layOutToolbarControls (juce::Rectangle<int> toolbar)
{
    snapButton.setBounds (toolbar.removeFromLeft (controlHeight + Space::s));
    toolbar.removeFromLeft (Space::xs);
    gridButton.setBounds (toolbar.removeFromLeft (180));
    toolbar.removeFromLeft (Space::l);

    for (size_t i = 0; i < std::size (allTools); ++i)
    {
        auto* button = toolButtons[(int) i];
        button->setVisible (canUseTool (allTools[i]));

        if (button->isVisible())
            button->setBounds (toolbar.removeFromLeft (controlHeight + Space::s));
    }

    return toolbar;
}

void TimelineEditor::setActive (bool isNowActive)
{
    if (active != isNowActive)
    {
        active = isNowActive;
        repaint (getLocalBounds().removeFromTop (toolbarHeight));
    }
}

void TimelineEditor::paintEditorBackground (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (0, toolbarHeight - 1, getWidth(), 1);

    if (active)
    {
        g.setColour (p.accent);
        g.fillRect (0, 0, getWidth(), 2);
    }
}

void TimelineEditor::refreshShortcutTexts()
{
    snapButton.setTooltip (
        withShortcut (commands, "Snap to grid: what you move or place lines up with the grid. Hold Alt to place freely", "grid.snap"));
    gridButton.setTooltip ("The grid's size. An adaptive grid gets finer as you zoom in");

    for (size_t i = 0; i < std::size (allTools); ++i)
        toolButtons[(int) i]->setTooltip (withShortcut (commands, toolButtons[(int) i]->getTitle() + ": " + describeToolHere (allTools[i]),
                                                        getToolCommandID (allTools[i])));
}

void TimelineEditor::showRulerMenu (BeatPosition)
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "transport.loop", "transport.loopSong" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

//==============================================================================
void TimelineEditor::scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar)
        setViewStart (BeatPosition::fromBeats (newRangeStart));
    else
        verticalScrollBarMoved (newRangeStart);
}

void TimelineEditor::timerCallback()
{
    if (! timelineContext.hasProject())
        return;

    // Smooth while playing; otherwise only often enough to notice moves made elsewhere.
    const auto playing = isPlaying (timelineContext);

    if (playing != wasPlaying)
    {
        wasPlaying = playing;
        startTimerHz (playing ? 60 : 5);
    }

    // Turn the page when the playhead passes the right edge, or jumps back out of view (at a loop's end).
    if (playing && followPlayback && ! horizontalScrollBar.isMouseButtonDown())
    {
        const auto beat = getPlayheadBeat();
        const auto width = getTimelineArea().getWidth();

        if (const auto x = view.beatToX (beat); x < 0.0 || x > width - 4.0)
            setViewStart (beat - BeatDuration::fromBeats (width * 0.05 / view.pixelsPerBeat));
    }

    if (std::abs (getContentRange().getEnd().inBeats() - contentEnd) > 1.0e-6)
        updateScrollBar();

    if (const auto loop = getLoop (timelineContext); ! isNear (loop, shownLoop))
    {
        shownLoop = loop;
        ruler.repaint();
    }

    updatePlayhead();
}

//==============================================================================
void paintGridLines (juce::Graphics& g, const TimelineEditor& editor, juce::Rectangle<int> area)
{
    if (! editor.hasProject())
        return;

    const auto& p = getPalette (editor);
    const auto& context = editor.getContext();
    const auto& view = editor.getView();

    // The grid counts from the time origin: the beats here are from there.
    const auto origin = fromSongStart (editor.getTimeOrigin());
    const BeatRange visible (view.xToBeat (area.getX() - 1) - origin, view.xToBeat (area.getRight() + 1) - origin);
    auto step = editor.getGridStep();

    if (getStepInBeats (context, step, visible.getStart()) * view.pixelsPerBeat < minLineSpacing)
        step = getAdaptiveGridStep (context, visible.getStart(), view.pixelsPerBeat, step.triplets, minLineSpacing);

    const auto barColour = p.border.interpolatedWith (p.textMuted, 0.3f);
    const auto subdivisionColour = p.border.withMultipliedAlpha (0.5f);

    forEachGridLine (context, step, visible,
                     [&] (BeatPosition beat, GridLineType lineType)
                     {
                         g.setColour (lineType == GridLineType::bar ? barColour
                                                                    : (lineType == GridLineType::beat ? p.border : subdivisionColour));
                         g.fillRect (juce::roundToInt (view.beatToX (beat + origin)), area.getY(), 1, area.getHeight());
                     });
}

//==============================================================================
void addTimelineCommands (CommandRegistry& registry, std::function<TimelineEditor*()> getActiveEditor)
{
    // Each command acts on the editor used last, if there is one with a project.
    const auto withEditor = [getActiveEditor] (std::function<void (TimelineEditor&)> action)
    {
        return [getActiveEditor, action]
        {
            if (auto* editor = getActiveEditor())
                action (*editor);
        };
    };

    const auto hasEdit = [getActiveEditor]
    {
        auto* editor = getActiveEditor();
        return editor != nullptr && editor->hasProject();
    };

    const auto ticked = [getActiveEditor] (std::function<bool (const TimelineEditor&)> isOn)
    {
        return [getActiveEditor, isOn]
        {
            auto* editor = getActiveEditor();
            return editor != nullptr && isOn (*editor);
        };
    };

    const auto changeGrid = [withEditor] (std::function<void (GridSettings&)> change)
    {
        return withEditor (
            [change] (TimelineEditor& editor)
            {
                auto grid = editor.getGrid();
                change (grid);
                editor.setGrid (grid);
            });
    };

    registry.add ({ .id = "view.zoomIn",
                    .name = "Zoom In",
                    .category = "View",
                    .description = "Shows less of the song, in more detail.",
                    .synonyms = { "magnify", "closer", "horizontal zoom" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.zoom (zoomStep); }),
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "view.zoomOut",
                    .name = "Zoom Out",
                    .category = "View",
                    .description = "Shows more of the song, in less detail.",
                    .synonyms = { "further", "horizontal zoom" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.zoom (1.0 / zoomStep); }),
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "view.zoomToFit",
                    .name = "Zoom to Fit",
                    .category = "View",
                    .description = "Shows the whole song in the playlist, or all the notes in the piano roll.",
                    .synonyms = { "show all", "overview", "whole song" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.zoomToFit(); }),
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.centreOnPlayhead",
                    .name = "Center on Playhead",
                    .category = "View",
                    .description = "Scrolls so that the playhead is in the middle.",
                    .synonyms = { "find playhead", "scroll to playhead", "where am i" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.centreOnPlayhead(); }),
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.followPlayback",
                    .name = "Follow Playback",
                    .category = "View",
                    .description = "While playing, turns the page when the playhead reaches the right edge.",
                    .synonyms = { "auto scroll", "scroll", "page" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.setFollowingPlayback (! editor.isFollowingPlayback()); }),
                    .isTicked = ticked ([] (const TimelineEditor& editor) { return editor.isFollowingPlayback(); }) });

    registry.add ({ .id = "grid.snap",
                    .name = "Snap to Grid",
                    .category = "Grid",
                    .description = "Lines up what you move or place with the grid. Hold Alt to place things freely.",
                    .synonyms = { "magnet", "quantize", "align" },
                    .perform = changeGrid ([] (GridSettings& grid) { grid.snap = ! grid.snap; }),
                    .isEnabled = hasEdit,
                    .isTicked = ticked ([] (const TimelineEditor& editor) { return editor.getGrid().snap; }) });

    registry.add ({ .id = "grid.adaptive",
                    .name = "Adaptive Grid",
                    .category = "Grid",
                    .description = "Makes the grid finer as you zoom in and coarser as you zoom out, or keeps it one size.",
                    .synonyms = { "zoom adaptive", "fixed grid", "auto" },
                    .perform = withEditor (
                        [] (TimelineEditor& editor)
                        {
                            auto grid = editor.getGrid();

                            // A fixed grid starts at the size the adaptive one had.
                            if (grid.adaptive)
                                grid.fixedLevel =
                                    juce::jlimit (GridSettings::minFixedLevel, GridSettings::maxFixedLevel, editor.getGridStep().level);

                            grid.adaptive = ! grid.adaptive;
                            editor.setGrid (grid);
                        }),
                    .isEnabled = hasEdit,
                    .isTicked = ticked ([] (const TimelineEditor& editor) { return editor.getGrid().adaptive; }) });

    registry.add ({ .id = "grid.finer",
                    .name = "Finer Grid",
                    .category = "Grid",
                    .description = "Makes the grid's steps smaller.",
                    .synonyms = { "narrow grid", "smaller grid", "more lines" },
                    .perform = changeGrid ([] (GridSettings& grid) { grid.makeFiner(); }),
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "grid.coarser",
                    .name = "Coarser Grid",
                    .category = "Grid",
                    .description = "Makes the grid's steps larger.",
                    .synonyms = { "widen grid", "larger grid", "fewer lines" },
                    .perform = changeGrid ([] (GridSettings& grid) { grid.makeCoarser(); }),
                    .isEnabled = hasEdit,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "grid.triplets",
                    .name = "Triplet Grid",
                    .category = "Grid",
                    .description = "Divides beats into three instead of two.",
                    .synonyms = { "triplets", "swing", "thirds" },
                    .perform = changeGrid ([] (GridSettings& grid) { grid.triplets = ! grid.triplets; }),
                    .isEnabled = hasEdit,
                    .isTicked = ticked ([] (const TimelineEditor& editor) { return editor.getGrid().triplets; }) });

    // The tools, with FL Studio's letters in the keymaps (Esc for the Smart Tool), and in Phazite's also the
    // digits 1 to 9 in the toolbar's order, which never play notes. Their keys spring back (pressTool()).
    for (const auto toolToAdd : allTools)
    {
        registry.add ({ .id = getToolCommandID (toolToAdd),
                        .name = toolToAdd == Tool::smart ? getToolName (toolToAdd) : getToolName (toolToAdd) + " Tool",
                        .category = "Tool",
                        .description = describeTool (toolToAdd),
                        .synonyms = getToolSynonyms (toolToAdd),
                        .perform = withEditor ([toolToAdd] (TimelineEditor& editor) { editor.pressTool (toolToAdd); }),
                        .isEnabled =
                            [getActiveEditor, toolToAdd]
                        {
                            auto* editor = getActiveEditor();
                            return editor != nullptr && editor->canUseTool (toolToAdd);
                        },
                        .isTicked = ticked ([toolToAdd] (const TimelineEditor& editor) { return editor.getTool() == toolToAdd; }),
                        .keyReleased = withEditor ([] (TimelineEditor& editor) { editor.releaseTool(); }) });
    }

    // Nudging and quantizing act on what is selected in the editor: clips or a time range in the playlist,
    // notes in the piano roll.
    const auto getUndoTarget = [getActiveEditor]() -> juce::UndoManager*
    {
        auto* editor = getActiveEditor();
        return editor != nullptr ? editor->getContext().getUndoManager() : nullptr;
    };

    const auto canNudge = ticked ([] (const TimelineEditor& editor) { return editor.canNudgeSelection(); });

    registry.add ({ .id = "edit.nudgeLeft",
                    .name = "Nudge Left",
                    .category = "Edit",
                    .description = "Moves the selected clips, notes or time range a step of the grid earlier.",
                    .synonyms = { "move left", "earlier", "shift" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.nudgeSelection (-1); }),
                    .isEnabled = canNudge,
                    .undoTarget = getUndoTarget,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.nudgeRight",
                    .name = "Nudge Right",
                    .category = "Edit",
                    .description = "Moves the selected clips, notes or time range a step of the grid later.",
                    .synonyms = { "move right", "later", "shift" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.nudgeSelection (1); }),
                    .isEnabled = canNudge,
                    .undoTarget = getUndoTarget,
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.quantize",
                    .name = "Quantize",
                    .category = "Edit",
                    .description = "Moves the starts of the selected clips or notes to the nearest line of the grid.",
                    .synonyms = { "snap", "align", "grid", "timing" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.quantizeSelection(); }),
                    .isEnabled = ticked ([] (const TimelineEditor& editor) { return editor.canQuantizeSelection(); }),
                    .undoTarget = getUndoTarget });
}
} // namespace phazite::ui
