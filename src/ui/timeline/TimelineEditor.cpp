// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineEditor.h"
#include "TimelineGesture.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/TransportModel.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
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

    // The fewest of an editor's pixels that make a whole number of the screen's at a scale (TimelineEditor::getViewStep()):
    // 1 at 100 % and 200 %, 2 at 150 %, 4 at 125 % and 175 %; 1 at a scale that no few pixels fit.
    int getWholeStep (double scale)
    {
        for (int step = 1; step <= 8; ++step)
            if (const auto screenPixels = scale * step; std::abs (screenPixels - std::round (screenPixels)) < 1.0e-3)
                return step;

        return 1;
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
TimelinePartWindow::TimelinePartWindow (juce::Component& shown)
    : part (&shown)
{
    // The part takes the clicks, through its own transform; the window only clips it.
    setInterceptsMouseClicks (false, true);
    addAndMakeVisible (shown);
    shown.addComponentListener (this);
    componentVisibilityChanged (shown);
}

TimelinePartWindow::~TimelinePartWindow()
{
    if (part != nullptr)
        part->removeComponentListener (this);
}

void TimelinePartWindow::setOverhang (int pixels)
{
    if (pixels != overhang)
    {
        overhang = pixels;
        resized();
    }
}

void TimelinePartWindow::resized()
{
    if (part != nullptr)
        part->setBounds (0, 0, getWidth() + overhang, getHeight());
}

void TimelinePartWindow::componentVisibilityChanged (juce::Component& shown)
{
    setOpaque (shown.isVisible() && shown.isOpaque());
}

void TimelinePartWindow::componentBeingDeleted (juce::Component&)
{
    setOpaque (false);
}

juce::Rectangle<float> getShownTimeline (const juce::Component& component)
{
    for (auto* parent = component.getParentComponent(); parent != nullptr; parent = parent->getParentComponent())
        if (auto* window = dynamic_cast<TimelinePartWindow*> (parent))
            return component.getLocalArea (window, window->getLocalBounds().toFloat());

    return component.getLocalBounds().toFloat();
}

//==============================================================================
TimelineRuler::TimelineRuler (TimelineEditor& e)
    : editor (e)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
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
    const auto columnOf = [&view, origin] (BeatPosition beat) { return view.beatToColumn (beat + origin); };

    // The loop's lane along the top, washed under the pointer, and the loop in it, over a wash of the part that loops, under
    // the ticks: song beats, not counted from the origin.
    if (laneHovered)
    {
        g.setColour (p.hover);
        g.fillRect (getLoopLane());
    }

    if (const auto loop = getLoop (context); loop.has_value() && ! loop->isEmpty())
    {
        // Its edges in the columns where the grid draws their beats (TimelineView::beatToColumn()).
        const auto loopStart = (float) view.beatToColumn (loop->getStart()), loopEnd = (float) view.beatToColumn (loop->getEnd());
        const juce::Rectangle<float> area (loopStart, 0.0f, loopEnd - loopStart, (float) getHeight() - 1.0f);
        // Red while Record punches into it, as Logic Pro's autopunch range is.
        const auto colour = isPunchOn (context) && canPunch (context) ? p.danger : p.loop;
        g.setColour (colour.withAlpha (LoopTint::wash));
        g.fillRect (area);
        g.setColour (colour.withAlpha (LoopTint::bar));
        g.fillRect (area.withHeight ((float) LoopTint::laneHeight));
    }

    // Bar numbers, as far apart as they need to be to read: every bar, or every 2nd, 4th, ... As the ticks', chosen where the
    // view starts, whatever part of the ruler is being painted (paintGridLines() says why).
    const auto viewStart = view.start - origin;
    const auto barWidth = view.pixelsPerBeat * getBarLength (context, viewStart).inBeats();
    auto every = 1;

    while (every * barWidth < RulerTint::numberSpacing && every < (1 << 20))
        every *= 2;

    // Short ticks up from the foot for the other bars and the beats, where they are far enough apart, and nothing finer: the
    // grid under the ruler shows that. A numbered bar's tick comes with its number.
    const auto foot = getHeight() - 1;
    const auto tickStep = getAdaptiveGridStep (context, viewStart, view.pixelsPerBeat, false, RulerTint::tickSpacing);

    forEachGridLine (context, tickStep, visible,
                     [&] (BeatPosition beat, GridLineType type)
                     {
                         const auto isBar = type == GridLineType::bar;

                         if (type == GridLineType::subdivision || (isBar && getBarAt (context, beat) % every == 0))
                             return;

                         const auto tick = isBar ? RulerTint::barTick : RulerTint::beatTick;
                         g.setColour (p.textMuted.withAlpha (isBar ? RulerTint::bar : RulerTint::beat));
                         g.fillRect (columnOf (beat), foot - tick, 1, tick);
                     });

    // The numbered bars, each a tick from the loop's lane down to the foot with its number beside it: from a little before
    // the area being drawn, so a number its edge cuts is drawn whole, to the bar its end is in, so that the loop ends however
    // far along the view is.
    constexpr int numberWidth = 48;
    const auto top = LoopTint::laneHeight;
    auto bar = std::max (0, getBarAt (context, view.xToBeat (clip.getX() - numberWidth) - origin));
    bar -= bar % every;
    const auto lastBar = getBarAt (context, view.xToBeat (clip.getRight() + 1) - origin);
    g.setFont (LookAndFeel::getNumberFont (TextSize::small));

    for (; bar <= lastBar; bar += every)
    {
        const auto x = columnOf (getBarStart (context, bar));
        g.setColour (p.textMuted.withAlpha (RulerTint::numberTick));
        g.fillRect (x, top, 1, foot - top);
        g.setColour (p.textMuted);
        g.drawText (juce::String (bar + 1), juce::Rectangle<int> (x + RulerTint::numberGap, top, numberWidth, foot - top),
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
    // The loop's lane washed under the pointer, where a drag loops.
    if (const auto inLane = editor.hasProject() && getLoopLane().contains (e.getPosition()); inLane != laneHovered)
    {
        laneHovered = inLane;
        repaint (getLoopLane());
    }

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

void TimelineRuler::mouseExit (const juce::MouseEvent&)
{
    if (laneHovered)
    {
        laneHovered = false;
        repaint (getLoopLane());
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

    // A press below the loop's lane takes the playhead there, and a drag takes it along, snapping to the grid (Alt places it
    // freely). While it is held the song is silent, paused where it was if it played, and it plays on from where the
    // playhead is let go.
    if (loopDrag == LoopDrag::none)
    {
        draggingPlayhead = true;
        pausedForDrag = pausePlayback (editor.getContext());
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

    // Past an edge of what shows of the ruler (its window, a pixel narrower), the time line scrolls along.
    const auto shown = getShownTimeline (*this).toNearestInt();

    if (draggingPlayhead)
    {
        editor.autoScroll (e, shown, TimelineEditor::ScrollDirections::sideways);
        movePlayhead (e);
        return;
    }

    if (loopDrag == LoopDrag::none)
        return;

    editor.autoScroll (e, shown, TimelineEditor::ScrollDirections::sideways);

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
    // The playhead let go: the song plays on from there if the press paused it.
    if (std::exchange (draggingPlayhead, false) && std::exchange (pausedForDrag, false))
        resumePlayback (editor.getContext());

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
// The playhead, in the focus colour so that it is easy to find: a marker at the foot of the ruler pointing down at where
// playback is, and a line from its point down over the time line (nothing above the marker). Clicks go through it to the
// ruler, which a drag along moves it. It sits on whole pixels, a pixel wider than its marker, and draws itself the part of a
// pixel further right that the playhead is (setOffset()), so that it glides rather than steps (docs/performance-audit.md,
// section 16).
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

    // How far right of its whole pixel it is drawn, from 0 to 1.
    void setOffset (float newOffset)
    {
        if (! juce::exactlyEqual (offset, newOffset))
        {
            offset = newOffset;
            repaint();
        }
    }

    bool linked = false;
    float offset = 0.0f;
    FrameLog* log = nullptr; // told when it is painted, as the frames' log times the paints

    void paint (juce::Graphics& g) override
    {
        const auto left = offset, width = (float) PlayheadTint::markerWidth, centre = left + width / 2.0f;
        const auto tip = (float) PlayheadTint::markerHeight, body = PlayheadTint::body, corner = PlayheadTint::corner;
        g.setColour (getPalette (*this).focus.withMultipliedAlpha (linked ? linkedPlayheadAlpha : 1.0f));

        // The marker: square at the top, its corners rounded, its sides turning in to the point.
        juce::Path marker;
        marker.startNewSubPath (left, body);
        marker.lineTo (left, corner);
        marker.quadraticTo (left, 0.0f, left + corner, 0.0f);
        marker.lineTo (left + width - corner, 0.0f);
        marker.quadraticTo (left + width, 0.0f, left + width, corner);
        marker.lineTo (left + width, body);
        marker.lineTo (centre, tip);
        marker.closeSubPath();
        g.fillPath (marker);

        // The line, from the point down.
        g.fillRect (juce::Rectangle<float> (centre - PlayheadTint::lineWidth / 2.0f, tip - 1.0f, PlayheadTint::lineWidth,
                                            (float) getHeight() - tip + 1.0f));

        if (log != nullptr)
            log->painted (juce::Time::getMillisecondCounterHiRes());
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

    void mouseDown (const juce::MouseEvent& e) override
    {
        editor.mouseIsDown = true;
        editor.holdsTimeline = editor.isInTimelinePart (e.eventComponent);
        editor.usedWhileKeyDown = editor.toolBeforeKey.has_value();

        if (editor.onUsed != nullptr)
            editor.onUsed();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        editor.mouseIsDown = false;
        editor.holdsTimeline = false;

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
    // The editor itself takes the keys it is given (a view's key, or a panel hiding with the keys in it), rather than
    // passing them to its toolbar's first button, which would show its focus ring.
    setWantsKeyboardFocus (true);

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
        button->setComponentID (getToolCommandID (toolToShow)); // what lessons point at (Lessons::Step::target)
        addChildComponent (button);
    }

    // The vertical scroll bar hides while everything fits; the horizontal one always shows (the time line goes on).
    for (auto* scrollBar : { &horizontalScrollBar, &verticalScrollBar })
    {
        scrollBar->addListener (this);
        scrollBar->setAutoHide (scrollBar == &verticalScrollBar);
    }

    addAndMakeVisible (snapButton);
    addAndMakeVisible (gridButton);
    addTimelinePart (ruler);

    for (auto* c : std::initializer_list<juce::Component*> { &horizontalScrollBar, &verticalScrollBar, playhead.get() })
        addAndMakeVisible (c);

    verticalScrollBar.addComponentListener (this);

    snapButton.setEnabled (false); // until a project is open
    gridButton.setEnabled (false);
    updateGridControls();
    addMouseListener (useListener.get(), true);
    playhead->log = &frameLog;
}

TimelineEditor::~TimelineEditor()
{
    removeMouseListener (useListener.get());
    verticalScrollBar.removeComponentListener (this);
    horizontalScrollBar.removeListener (this);
    verticalScrollBar.removeListener (this);
}

void TimelineEditor::addTimelinePart (juce::Component& part)
{
    jassert (findPartWindow (part) == nullptr);
    auto& window = *partWindows.emplace_back (std::make_unique<TimelinePartWindow> (part));
    addAndMakeVisible (window);
}

void TimelineEditor::placeTimelinePart (juce::Component& part, juce::Rectangle<int> area)
{
    if (auto* window = findPartWindow (part))
        window->setBounds (area);
}

juce::Rectangle<int> TimelineEditor::getTimelinePartArea (const juce::Component& part) const
{
    const auto* window = findPartWindow (part);
    return window != nullptr ? window->getBounds() : juce::Rectangle<int>();
}

TimelinePartWindow* TimelineEditor::findPartWindow (const juce::Component& part) const
{
    for (const auto& window : partWindows)
        if (window->getPart() == &part)
            return window.get();

    return nullptr;
}

void TimelineEditor::updateViewStep()
{
    // The screen's pixels for each of the editor's: the display's scale (on Windows, its monitor's), Phazite's own over it,
    // and any transform the editor is in.
    const auto* peer = getPeer();
    const auto scale = (peer != nullptr ? peer->getPlatformScaleFactor() : 1.0)
                       * (double) juce::Component::getApproximateScaleFactorForComponent (this)
                       * (double) juce::Desktop::getInstance().getGlobalScaleFactor();

    if (const auto step = getWholeStep (scale); step != viewStep)
    {
        viewStep = step;

        for (const auto& window : partWindows)
            window->setOverhang (step);
    }
}

bool TimelineEditor::isInTimelinePart (const juce::Component* component) const
{
    for (const auto& window : partWindows)
        if (component != nullptr && (component == window.get() || window->isParentOf (component)))
            return true;

    return false;
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

    frameBeat.reset();
    holdsPlayhead = false;
    frameLog = FrameLog();
    updateGridControls();
    setViewStart ({});

    // At each frame of the display from now on (showFrame()), so that the playhead moves from the first frame after Play;
    // a frame does nothing while the song is stopped, and the window's own painting wakes the message thread at each one
    // anyway. A few times a second, the timer notices what changed elsewhere.
    playheadFrames = open ? juce::VBlankAttachment (this, [this] (double seconds) { showFrame (seconds); }) : juce::VBlankAttachment();

    if (open)
        startTimerHz (5);
    else
        stopTimer();
}

//==============================================================================
void TimelineEditor::setViewStart (BeatPosition start)
{
    // From the time origin, and no later than a view goes, however it is scrolled; on a whole step of the song's pixels
    // (getViewStep()), which only following the playhead continuously moves the time line's parts by a part of, after this
    // (followPlayhead()).
    updateViewStep();
    setSubPixel (0.0f);
    const auto earliest = getEarliestStart();
    const auto latest = std::max (earliest, BeatPosition::fromBeats (TimelineView::latestStart));
    const auto steps = std::round (std::clamp (start, earliest, latest).inBeats() * view.pixelsPerBeat / viewStep);
    view.start = std::clamp (BeatPosition::fromBeats (steps * viewStep / view.pixelsPerBeat), earliest, latest);
    ruler.repaint();
    timelineChanged();
    updatePlayhead();
    updateScrollBar();
}

BeatPosition TimelineEditor::getEarliestStart() const
{
    // The whole step nearest the origin, which puts the origin's line in the view's first column (TimelineView::beatToColumn())
    // at 100 %, or within half a step of it.
    const auto steps = std::round (timeOrigin.inBeats() * view.pixelsPerBeat / viewStep);
    return BeatPosition::fromBeats (steps * viewStep / view.pixelsPerBeat);
}

void TimelineEditor::setTimeOrigin (BeatPosition newOrigin)
{
    timeOrigin = std::max (BeatPosition(), newOrigin);
    updateGridControls(); // the grid's size can depend on where bars start
    setViewStart (view.start);
}

void TimelineEditor::zoomAround (double x, double factor)
{
    if (isViewFixed())
        return;

    view.zoomAround (x, factor);
    setViewStart (view.start);
    updateGridControls(); // an adaptive grid's size changes with the zoom
}

void TimelineEditor::zoom (double factor)
{
    const auto width = getTimelineArea().getWidth();
    auto x = width / 2.0;

    if (const auto shownX = view.beatToX (getShownPlayheadBeat()); shownX >= 0.0 && shownX <= width)
        x = shownX;

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

void TimelineEditor::setFollowingPlayback (bool shouldFollow) noexcept
{
    followPlayback = shouldFollow;
    followPaused = false;
}

void TimelineEditor::pauseFollowing()
{
    if (isPlaying (timelineContext))
        followPaused = true;
}

void TimelineEditor::centreOnPlayhead()
{
    if (isViewFixed())
        return;

    followPaused = false;
    setViewStart (getShownPlayheadBeat() - BeatDuration::fromBeats (getTimelineArea().getWidth() / 2.0 / view.pixelsPerBeat));
}

void TimelineEditor::movePlayheadTo (BeatPosition beat)
{
    // Playback starts there from now on, and Space comes back there when it stops; the view follows it again. It shows
    // there at once, not where the last frame heard it.
    phazite::movePlayhead (timelineContext, beat);
    followPaused = false;
    frameBeat.reset();
    updatePlayhead();
}

BeatPosition TimelineEditor::getPlayheadBeat() const
{
    return phazite::getPlayheadBeat (timelineContext);
}

BeatPosition TimelineEditor::getShownPlayheadBeat() const
{
    return frameBeat.value_or (getPlayheadBeat());
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

    // At the song's position (while it plays, the beat heard at the frame shown), or where a linked clip's playing shows in
    // this editor's notes, where the time line's parts draw it: the part of a pixel they are moved left by, left of the view's
    // place for it. Between pixels, so that it glides at any zoom, but on the whole pixel the view scrolls under it, while it
    // does, which takes the song there.
    const auto area = getTimelineArea();
    const auto songBeat = getShownPlayheadBeat().inBeats();
    const auto linkedBeat = getLinkedPlayheadBeat (songBeat);
    const auto x = (linkedBeat.value_or (songBeat) - view.start.inBeats()) * view.pixelsPerBeat - subPixel;
    playheadX = holdsPlayhead ? std::round (x) : x;
    const auto isInView = playheadX >= -1.0 && playheadX <= area.getWidth() + 1.0;
    playhead->setLinked (linkedBeat.has_value());
    playhead->setVisible (isInView);

    // The marker's point at the foot of the ruler, the line under it.
    if (isInView)
    {
        const auto left = std::floor (playheadX);
        const auto top = getTimelinePartArea (ruler).getBottom() - PlayheadTint::markerHeight;
        playhead->setBounds (area.getX() + (int) left - PlayheadTint::markerWidth / 2, top, PlayheadTint::markerWidth + 1,
                             area.getBottom() - top);
        playhead->setOffset ((float) (playheadX - left));
    }

    playheadShown();
}

void TimelineEditor::updateScrollBar()
{
    // From the time origin, the content plus some room after it, and as far as the view goes, rounded up to 16 bars of 4/4: a
    // view followed past the content's end grows the time line now and then, not at every frame, which redrew the overview's
    // picture at every frame, its marks shrinking and creeping as they went.
    constexpr double growth = 64.0;
    const auto visibleBeats = std::max (1, getTimelineArea().getWidth()) / view.pixelsPerBeat;
    contentEnd = getContentRange().getEnd().inBeats();
    const auto total =
        std::max ({ contentEnd + 64.0, std::ceil ((view.start.inBeats() + visibleBeats) / growth) * growth, timeOrigin.inBeats() + 256.0 });
    horizontalScrollBar.setRangeLimits (timeOrigin.inBeats(), total, juce::dontSendNotification);
    horizontalScrollBar.setCurrentRange (view.start.inBeats(), visibleBeats, juce::dontSendNotification);
    horizontalScrollBar.setSingleStepSize (scrollPerNotch / view.pixelsPerBeat);

    if (overview != nullptr)
        overview->repaint(); // its thumb, where the part in view is now
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
    if (isViewFixed())
        return;

    // By whole steps, as the view starts on one (setViewStart(): a pixel at 100 %), the rest kept for the next move of the
    // wheel: a trackpad's moves can be parts of a pixel.
    pauseFollowing();
    const auto pixels = -getWheelNotches (wheelDelta) * scrollPerNotch + wheelRest;
    const auto whole = std::trunc (pixels / viewStep) * viewStep;
    wheelRest = pixels - whole;
    setViewStart (view.xToBeat (whole));
}

bool TimelineEditor::autoScroll (const juce::MouseEvent& e, juce::Rectangle<int> area, ScrollDirections directions)
{
    // By the time since the drag before, by the events' own times, so that the speed doesn't depend on how often the mouse
    // reports: the drags' auto-repeat comes 60 times a second while the mouse is still, a moving mouse up to a thousand.
    // Counted as at least a frame each, as before, they scrolled two to sixteen times as fast while the hand moved. The first
    // drag after a pause counts as a frame.
    constexpr double frame = 1.0 / 60.0, longest = 1.0 / 20.0;
    const auto elapsed = (e.eventTime - lastAutoScrollTime).inSeconds();
    const auto seconds = elapsed >= 0.0 && elapsed <= longest ? elapsed : frame;
    lastAutoScrollTime = e.eventTime;
    const auto position = e.getPosition();

    // By whole steps, as the view starts on one (setViewStart(): a pixel at 100 %), and the rows by whole pixels, the rest kept
    // for the next drag: a pointer just past the margin scrolls a quarter of a pixel a frame.
    const auto takeWholeSteps = [seconds] (double speed, int step, double& rest)
    {
        const auto pixels = speed * seconds + rest;
        const auto whole = std::trunc (pixels / step) * step;
        rest = juce::exactlyEqual (speed, 0.0) ? 0.0 : pixels - whole;
        return juce::exactlyEqual (speed, 0.0) ? 0.0 : whole;
    };

    auto scrolled = false;

    if (directions != ScrollDirections::upAndDown && ! isViewFixed())
    {
        if (const auto pixels = takeWholeSteps (getAutoScrollSpeed (position.x - area.getX(), area.getWidth()), viewStep, autoScrollRest);
            ! juce::exactlyEqual (pixels, 0.0))
        {
            const auto before = view.start.inBeats();
            setViewStart (view.xToBeat (pixels));
            scrolled = ! juce::exactlyEqual (view.start.inBeats(), before);

            if (scrolled)
                pauseFollowing();
        }
    }

    if (directions != ScrollDirections::sideways)
        if (const auto pixels = takeWholeSteps (getAutoScrollSpeed (position.y - area.getY(), area.getHeight()), 1, autoScrollRestY);
            ! juce::exactlyEqual (pixels, 0.0))
            scrolled = scrollVerticallyBy ((int) pixels) || scrolled;

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
        text << describeGridStep (getGridStep());

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
        menu.addItem (level + 1, describeGridStep ({ level, grid.triplets }), true, ! grid.adaptive && grid.fixedLevel == level);

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
    if (! canUseTool (newTool) || usesSmartToolFor (newTool))
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
        button->setVisible (canUseTool (allTools[i]) && ! usesSmartToolFor (allTools[i]));

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
    // Moved by the user (the editor moves it without telling itself).
    if (scrollBar == &horizontalScrollBar)
    {
        pauseFollowing();
        setViewStart (BeatPosition::fromBeats (newRangeStart));
    }
    else
    {
        verticalScrollBarMoved (newRangeStart);
    }
}

void TimelineEditor::timerCallback()
{
    if (! timelineContext.hasProject())
        return;

    // While playing where it shows, the playhead moves at each frame of the display (showFrame()); this notices moves made
    // elsewhere a few times a second. An editor that doesn't show draws nothing (docs/performance-audit.md, U5 and section 8).
    if (! isShowing())
        return;

    if (std::abs (getContentRange().getEnd().inBeats() - contentEnd) > 1.0e-6)
        updateScrollBar();

    if (const auto loop = getLoop (timelineContext); ! isNear (loop, shownLoop))
    {
        shownLoop = loop;
        ruler.repaint();
    }

    if (const auto punch = isPunchOn (timelineContext) && canPunch (timelineContext); punch != shownPunch)
    {
        shownPunch = punch;
        ruler.repaint();
    }

    followPlayhead();
}

void TimelineEditor::followPlayhead()
{
    // Playback started: the view follows again, if a scroll paused it.
    const auto playing = isPlaying (timelineContext);

    if (playing && ! playedWhenFollowed)
        followPaused = false;

    playedWhenFollowed = playing;
    holdsPlayhead = false;
    updateViewStep();  // the window may have gone to a display of another scale
    auto shift = 0.0f; // the part of a step what draws the song moves by, scrolling continuously

    // Turn the page when the playhead passes the right edge, or jumps back out of view (at a loop's end), or scroll it to
    // the middle, unless the user scrolled away while it played. While the mouse holds the time line down (dragging what is
    // in view; a press anywhere else in the editor, a track's mute, a piano key, doesn't), the view stays where it is, the
    // part of a pixel too, rather than stepping to a whole pixel and back as the press comes and goes. The playhead followed
    // is the one shown: the piano roll's, where a clip linked to its notes plays.
    const auto follows = followPlayback && ! followPaused && ! isViewFixed() && playing;

    if (follows && (horizontalScrollBar.isMouseButtonDown() || holdsTimeline))
    {
        shift = subPixel;
    }
    else if (follows)
    {
        const auto songBeat = getShownPlayheadBeat();
        const auto linkedBeat = getLinkedPlayheadBeat (songBeat.inBeats());
        const auto beat = linkedBeat.has_value() ? BeatPosition::fromBeats (*linkedBeat) : songBeat;
        const auto width = getTimelineArea().getWidth();

        if (scrollContinuously)
        {
            // The view by whole steps (getViewStep(), a pixel at 100 %), which what it draws lines up with, and the rest as a
            // shift of what draws the song (setSubPixel()), so that the song glides under the playhead: by whole pixels alone, it
            // stood still one frame in four at the owner's zoom (docs/performance-audit.md, section 16). The view starts at the
            // last step the song has passed with the playhead in the middle, and the shift, less than a step, is always left,
            // which the time line's parts are a step wider than their windows for (addTimelinePart()). The playhead stays on one
            // column, the middle's, a whole pixel: a middle half a pixel in (a time line an odd number of pixels wide) put the
            // playhead, rounded, on one side of it or the other as the song moved on by parts of a pixel, so that it jittered
            // (the owner, 2026-11-07). Near the song's start the playhead crosses to the middle first.
            const auto middle = width / 2;
            const auto exact = beat.inBeats() * view.pixelsPerBeat;
            const auto startPixels = std::floor ((exact - middle) / viewStep) * viewStep;
            const auto earliest = getEarliestStart();
            const auto start = std::max (earliest, BeatPosition::fromBeats (startPixels / view.pixelsPerBeat));
            holdsPlayhead = start > earliest; // the view moves under it, rather than waiting at the song's start

            if (holdsPlayhead)
                shift = (float) (exact - middle - startPixels);

            if (start != view.start)
                setViewStart (start);
        }
        else if (const auto x = view.beatToX (beat); x < 0.0 || x > width - 4.0)
        {
            setViewStart (beat - BeatDuration::fromBeats (width * 0.05 / view.pixelsPerBeat));
        }
    }
    else if (subPixel >= 0.5f * (float) viewStep)
    {
        // Stopped, or no longer following: on whole steps again, the nearer of the two, so that what shows moves by half a step
        // at most.
        setViewStart (view.xToBeat ((double) viewStep));
    }

    setSubPixel (shift);
    updatePlayhead();
}

void TimelineEditor::setSubPixel (float shift)
{
    if (juce::exactlyEqual (shift, subPixel))
        return;

    // A translation JUCE draws, the parts' mouse positions and hit-testing included, and their children's pictures moved
    // with them without being drawn again; their windows keep them to whole pixels.
    subPixel = shift;
    const auto transform = juce::exactlyEqual (shift, 0.0f) ? juce::AffineTransform() : juce::AffineTransform::translation (-shift, 0.0f);

    for (const auto& window : partWindows)
        if (auto* part = window->getPart())
            part->setTransform (transform);
}

void TimelineEditor::showFrame (double seconds)
{
    if (timelineContext.hasProject() && isShowing())
        frameComing();

    const auto playing = timelineContext.hasProject() && isShowing() && isPlaying (timelineContext);

    // Stopped, or hidden: the playhead where playback went back to, and the frames since it started into the log.
    if (! playing)
    {
        if (frameBeat.has_value())
        {
            frameBeat.reset();
            followPlayhead();
            writeFrameLog();
        }

        return;
    }

    // The beat heard at the frame's own time, not at the moment the message thread gets to it, which changes with what else
    // it does (docs/performance-audit.md, section 16).
    const auto taken = juce::Time::getMillisecondCounterHiRes();
    const auto milliseconds = seconds * 1000.0;
    const auto startBefore = view.start;
    const auto xBefore = playheadX;
    const auto shiftBefore = subPixel;
    frameBeat = getHeardPlayheadBeat (timelineContext, milliseconds);
    followPlayhead();

    // Where the song shows under the playhead's line: the view's start and the line's place, and the part of a pixel the song
    // is shifted by while it scrolls continuously.
    const auto moved =
        playhead->isVisible()
        && (view.start != startBefore || ! juce::exactlyEqual (playheadX, xBefore) || ! juce::exactlyEqual (subPixel, shiftBefore));
    frameLog.frame (milliseconds, taken, frameBeat->inBeats(), view.start.inBeats() * view.pixelsPerBeat + playheadX + subPixel, moved);
}

void TimelineEditor::writeFrameLog()
{
    // A second of frames or more; less tells nothing.
    if (frameLog.getFrames() >= 60)
        juce::Logger::writeToLog (frameLog.takeSummary (getTitle(), ! followPlayback     ? "not following the playhead"
                                                                    : scrollContinuously ? "scrolling continuously"
                                                                                         : "turning pages"));

    frameLog = FrameLog();
}

//==============================================================================
void paintGridLines (juce::Graphics& g, const TimelineEditor& editor, juce::Rectangle<int> area)
{
    if (! editor.hasProject())
        return;

    const auto& p = getPalette (editor);
    const auto& context = editor.getContext();
    const auto& view = editor.getView();

    // The grid counts from the time origin: the beats here are from there. Its step is the view's, chosen where the view
    // starts, whatever part of it is being painted: chosen where the painting starts, a part repainted alone (under the
    // playhead, at each frame) could take another step than the rest where the time signature changes.
    const auto origin = fromSongStart (editor.getTimeOrigin());
    const BeatRange visible (view.xToBeat (area.getX() - 1) - origin, view.xToBeat (area.getRight() + 1) - origin);
    const auto viewStart = view.start - origin;
    auto step = editor.getGridStep();

    if (getStepInBeats (context, step, viewStart) * view.pixelsPerBeat < minLineSpacing)
        step = getAdaptiveGridStep (context, viewStart, view.pixelsPerBeat, step.triplets, minLineSpacing);

    const auto barColour = p.border.interpolatedWith (p.textMuted, 0.3f);
    const auto subdivisionColour = p.border.withMultipliedAlpha (0.5f);

    forEachGridLine (context, step, visible,
                     [&] (BeatPosition beat, GridLineType lineType)
                     {
                         g.setColour (lineType == GridLineType::bar ? barColour
                                                                    : (lineType == GridLineType::beat ? p.border : subdivisionColour));
                         g.fillRect (view.beatToColumn (beat + origin), area.getY(), 1, area.getHeight());
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
                    .undoTarget = getUndoTarget,
                    .currentName = [getActiveEditor]
                    {
                        auto* editor = getActiveEditor();
                        return editor != nullptr ? editor->getQuantizeName() : juce::String();
                    } });
}
} // namespace phazite::ui
