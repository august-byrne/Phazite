// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineEditor.h"
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "model/Project.h"

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
    setDescription ("Click or drag to move the playhead. It snaps to the grid; hold Alt to place it freely. "
                    "Roll the mouse wheel to zoom.");
}

void TimelineRuler::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surfaceRaised);
    g.setColour (p.border);
    g.fillRect (0, getHeight() - 1, getWidth(), 1);

    auto* edit = editor.getEdit();

    if (edit == nullptr)
        return;

    const auto& tempo = edit->tempoSequence;
    const auto& view = editor.getView();
    const auto clip = g.getClipBounds();
    const te::BeatRange visible (view.xToBeat (clip.getX() - 1), view.xToBeat (clip.getRight() + 1));

    // Ticks: tall for bars, shorter for beats, shortest between them.
    const auto tickType = getAdaptiveSnapType (tempo, visible.getStart(), view.pixelsPerBeat, false, minLineSpacing);

    forEachGridLine (tempo, tickType, false, visible,
                     [&] (te::BeatPosition beat, GridLineType type)
                     {
                         const auto isBar = type == GridLineType::bar;
                         const auto tick = isBar ? getHeight() / 2 : (type == GridLineType::beat ? getHeight() / 4 : getHeight() / 8);
                         g.setColour (isBar ? p.textMuted : p.textMuted.withAlpha (0.4f));
                         g.fillRect (juce::roundToInt (view.beatToX (beat)), getHeight() - 1 - tick, 1, tick);
                     });

    // Bar numbers, as far apart as they need to be to read: every bar, or every 2nd, 4th, ...
    const auto barWidth = view.pixelsPerBeat * tempo.getTimeSigAt (visible.getStart()).numerator.get();
    auto every = 1;

    while (every * barWidth < 32.0 && every < (1 << 20))
        every *= 2;

    // From a little before the area being drawn, so a number its edge cuts is drawn whole.
    auto bar = std::max (0, tempo.toBarsAndBeats (tempo.toTime (view.xToBeat (clip.getX() - 48))).bars);
    bar -= bar % every;

    g.setColour (p.textMuted);
    g.setFont (LookAndFeel::getNumberFont (TextSize::small));

    for (;; bar += every)
    {
        const auto x = (float) view.beatToX (tempo.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 }));

        if (x > (float) clip.getRight())
            break;

        g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 4.0f, 2.0f, 48.0f, (float) getHeight() / 2.0f),
                    juce::Justification::centredLeft, false);
    }
}

void TimelineRuler::mouseDown (const juce::MouseEvent& e)
{
    movePlayhead (e);
}

void TimelineRuler::mouseDrag (const juce::MouseEvent& e)
{
    movePlayhead (e);
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
        setInterceptsMouseClicks (false, false);
        setAlwaysOnTop (true);
    }

    void paint (juce::Graphics& g) override
    {
        const auto centre = (float) getWidth() / 2.0f;
        g.setColour (getPalette (*this).text);
        g.fillRect (juce::Rectangle<float> (centre - 0.75f, 0.0f, 1.5f, (float) getHeight()));

        juce::Path marker; // a triangle pointing down, at the bottom of the ruler
        marker.addTriangle (0.0f, (float) rulerHeight - 9.0f, (float) getWidth(), (float) rulerHeight - 9.0f, centre,
                            (float) rulerHeight - 1.0f);
        g.fillPath (marker);
    }
};

//==============================================================================
TimelineEditor::TimelineEditor (CommandRegistry& registry, const juce::Identifier& stateType)
    : commands (registry),
      gridStateType (stateType),
      playhead (std::make_unique<Playhead>())
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
        button->onClick = [this, toolToShow] { commands.invoke (getToolCommandID (toolToShow)); };
        addChildComponent (button);
    }

    for (auto* scrollBar : { &horizontalScrollBar, &verticalScrollBar })
    {
        scrollBar->addListener (this);
        scrollBar->setAutoHide (false);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &snapButton, &gridButton, &ruler, &horizontalScrollBar, &verticalScrollBar,
                                                             playhead.get() })
        addAndMakeVisible (c);

    snapButton.setEnabled (false); // until a project is open
    gridButton.setEnabled (false);
    updateGridControls();
    addMouseListener (this, true); // mouseDown() hears about clicks on the children too
}

TimelineEditor::~TimelineEditor()
{
    removeMouseListener (this);
    horizontalScrollBar.removeListener (this);
    verticalScrollBar.removeListener (this);
}

void TimelineEditor::mouseDown (const juce::MouseEvent&)
{
    if (onUsed != nullptr)
        onUsed();
}

void TimelineEditor::setTimelineEdit (te::Edit* newEdit)
{
    edit = newEdit;
    view = {};
    grid = newEdit != nullptr ? GridSettings::load (getGridState (false)) : GridSettings();
    snapButton.setEnabled (newEdit != nullptr);
    gridButton.setEnabled (newEdit != nullptr);

    updateGridControls();
    setViewStart ({});

    if (newEdit != nullptr)
        startTimerHz (5);
    else
        stopTimer();
}

juce::ValueTree TimelineEditor::getGridState (bool createIfMissing) const
{
    auto* e = edit.get();

    if (e == nullptr)
        return {};

    // Reading leaves the project as it is: a project that doesn't have the state yet reads the defaults.
    auto state = createIfMissing ? getPhaziteState (*e) : findPhaziteState (*e);

    if (! gridStateType.isValid())
        return state;

    return createIfMissing ? state.getOrCreateChildWithName (gridStateType, nullptr) : state.getChildWithName (gridStateType);
}

//==============================================================================
void TimelineEditor::setViewStart (te::BeatPosition start)
{
    view.start = te::BeatPosition::fromBeats (std::max (0.0, start.inBeats()));
    ruler.repaint();
    timelineChanged();
    updatePlayhead();
    updateScrollBar();
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

void TimelineEditor::zoomTo (te::BeatRange range)
{
    if (range.getLength() <= te::BeatDuration())
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

    if (content.getLength() <= te::BeatDuration())
        return;

    const auto width = std::max (1, getTimelineArea().getWidth() - Space::l);
    view.pixelsPerBeat =
        juce::jlimit (TimelineView::minPixelsPerBeat, TimelineView::maxPixelsPerBeat, width / content.getLength().inBeats());
    setViewStart (content.getStart());
    updateGridControls();
}

void TimelineEditor::centreOnPlayhead()
{
    setViewStart (getPlayheadBeat() - te::BeatDuration::fromBeats (getTimelineArea().getWidth() / 2.0 / view.pixelsPerBeat));
}

void TimelineEditor::movePlayheadTo (te::BeatPosition beat)
{
    if (auto* e = edit.get())
        e->getTransport().setPosition (e->tempoSequence.toTime (beat));

    updatePlayhead();
}

te::BeatPosition TimelineEditor::getPlayheadBeat() const
{
    if (auto* e = edit.get())
        return e->tempoSequence.toBeats (e->getTransport().getPosition());

    return {};
}

void TimelineEditor::updatePlayhead()
{
    if (edit.get() == nullptr)
    {
        playhead->setVisible (false);
        return;
    }

    const auto area = getTimelineArea();
    const auto x = view.beatToX (getPlayheadBeat());
    const auto isInView = x >= -1.0 && x <= area.getWidth() + 1.0;
    playhead->setVisible (isInView);

    if (isInView)
        playhead->setBounds (area.getX() + juce::roundToInt (x) - playheadWidth / 2, ruler.getY(), playheadWidth,
                             area.getBottom() - ruler.getY());
}

void TimelineEditor::updateScrollBar()
{
    // The content plus some room after it.
    const auto visibleBeats = std::max (1, getTimelineArea().getWidth()) / view.pixelsPerBeat;
    contentEnd = getContentRange().getEnd().inBeats();
    const auto total = std::max ({ contentEnd + 64.0, view.start.inBeats() + visibleBeats, 256.0 });
    horizontalScrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
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

//==============================================================================
te::TimecodeSnapType TimelineEditor::getSnapType() const
{
    if (auto* e = edit.get())
        return getGridSnapType (grid, e->tempoSequence, view.start, view.pixelsPerBeat);

    return te::TimecodeSnapType::get1BeatSnapType();
}

double TimelineEditor::getGridStepInBeats() const
{
    if (auto* e = edit.get())
        return phazite::getGridStepInBeats (getSnapType(), e->tempoSequence, view.start, grid.triplets);

    return 1.0;
}

te::BeatPosition TimelineEditor::snap (te::BeatPosition beat, juce::ModifierKeys mods) const
{
    auto* e = edit.get();

    if (e == nullptr || ! grid.snap || mods.isAltDown())
        return beat;

    return snapToGrid (beat, getSnapType(), e->tempoSequence, grid.triplets);
}

te::BeatPosition TimelineEditor::snapDown (te::BeatPosition beat, juce::ModifierKeys mods) const
{
    auto* e = edit.get();

    if (e == nullptr || ! grid.snap || mods.isAltDown())
        return beat;

    const auto& tempo = e->tempoSequence;
    return tempo.toBeats (getSnapType().roundTimeDown (tempo.toTime (beat), tempo, grid.triplets));
}

void TimelineEditor::setGrid (const GridSettings& newGrid)
{
    grid = newGrid;

    if (auto state = getGridState (true); state.isValid())
        grid.save (state);

    updateGridControls();
    gridChanged();
}

void TimelineEditor::updateGridControls()
{
    snapButton.setToggleState (grid.snap, juce::dontSendNotification);

    juce::String text ("Grid: ");

    if (auto* e = edit.get())
        text << getGridDescription (getSnapType(), e->tempoSequence, grid.triplets);

    gridButton.setButtonText (grid.adaptive ? text + " (auto)" : text);
}

void TimelineEditor::showGridMenu()
{
    auto* e = edit.get();

    if (e == nullptr)
        return;

    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "grid.snap" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "grid.adaptive", "grid.finer", "grid.coarser", "grid.triplets" });
    menu.addSectionHeader ("Fixed size");

    // Item IDs are the levels plus one, as an ID can't be 0; command items have much larger IDs.
    for (auto level = GridSettings::minFixedLevel; level <= GridSettings::maxFixedLevel; ++level)
        menu.addItem (level + 1, getGridDescription ({ te::TimecodeType::barsBeats, level }, e->tempoSequence, grid.triplets), true,
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

    tool = newTool;

    for (size_t i = 0; i < std::size (allTools); ++i)
        toolButtons[(int) i]->setToggleState (allTools[i] == tool, juce::dontSendNotification);

    toolChanged();
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

//==============================================================================
void TimelineEditor::scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar)
        setViewStart (te::BeatPosition::fromBeats (newRangeStart));
    else
        verticalScrollBarMoved (newRangeStart);
}

void TimelineEditor::timerCallback()
{
    auto* e = edit.get();

    if (e == nullptr)
        return;

    // Smooth while playing; otherwise only often enough to notice moves made elsewhere.
    const auto playing = e->getTransport().isPlaying();

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
            setViewStart (beat - te::BeatDuration::fromBeats (width * 0.05 / view.pixelsPerBeat));
    }

    if (std::abs (getContentRange().getEnd().inBeats() - contentEnd) > 1.0e-6)
        updateScrollBar();

    updatePlayhead();
}

//==============================================================================
void paintGridLines (juce::Graphics& g, const TimelineEditor& editor, juce::Rectangle<int> area)
{
    auto* edit = editor.getEdit();

    if (edit == nullptr)
        return;

    const auto& p = getPalette (editor);
    const auto& tempo = edit->tempoSequence;
    const auto& view = editor.getView();
    const auto triplets = editor.getGrid().triplets;
    const te::BeatRange visible (view.xToBeat (area.getX() - 1), view.xToBeat (area.getRight() + 1));
    auto type = editor.getSnapType();

    if (getGridStepInBeats (type, tempo, visible.getStart(), triplets) * view.pixelsPerBeat < minLineSpacing)
        type = getAdaptiveSnapType (tempo, visible.getStart(), view.pixelsPerBeat, triplets, minLineSpacing);

    const auto barColour = p.border.interpolatedWith (p.textMuted, 0.3f);
    const auto subdivisionColour = p.border.withMultipliedAlpha (0.5f);

    forEachGridLine (tempo, type, triplets, visible,
                     [&] (te::BeatPosition beat, GridLineType lineType)
                     {
                         g.setColour (lineType == GridLineType::bar ? barColour
                                                                    : (lineType == GridLineType::beat ? p.border : subdivisionColour));
                         g.fillRect (juce::roundToInt (view.beatToX (beat)), area.getY(), 1, area.getHeight());
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
        return editor != nullptr && editor->getEdit() != nullptr;
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
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.zoomOut",
                    .name = "Zoom Out",
                    .category = "View",
                    .description = "Shows more of the song, in less detail.",
                    .synonyms = { "further", "horizontal zoom" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.zoom (1.0 / zoomStep); }),
                    .isEnabled = hasEdit });

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
                                grid.fixedLevel = juce::jlimit (GridSettings::minFixedLevel, GridSettings::maxFixedLevel,
                                                                editor.getSnapType().getLevel());

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
                    .isEnabled = hasEdit });

    registry.add ({ .id = "grid.coarser",
                    .name = "Coarser Grid",
                    .category = "Grid",
                    .description = "Makes the grid's steps larger.",
                    .synonyms = { "widen grid", "larger grid", "fewer lines" },
                    .perform = changeGrid ([] (GridSettings& grid) { grid.makeCoarser(); }),
                    .isEnabled = hasEdit });

    registry.add ({ .id = "grid.triplets",
                    .name = "Triplet Grid",
                    .category = "Grid",
                    .description = "Divides beats into three instead of two.",
                    .synonyms = { "triplets", "swing", "thirds" },
                    .perform = changeGrid ([] (GridSettings& grid) { grid.triplets = ! grid.triplets; }),
                    .isEnabled = hasEdit,
                    .isTicked = ticked ([] (const TimelineEditor& editor) { return editor.getGrid().triplets; }) });

    // The tools, with FL Studio's letters in the keymaps (Esc for the Smart Tool).
    for (const auto toolToAdd : allTools)
    {
        registry.add ({ .id = getToolCommandID (toolToAdd),
                        .name = toolToAdd == Tool::smart ? getToolName (toolToAdd) : getToolName (toolToAdd) + " Tool",
                        .category = "Tool",
                        .description = describeTool (toolToAdd),
                        .synonyms = getToolSynonyms (toolToAdd),
                        .perform = withEditor ([toolToAdd] (TimelineEditor& editor) { editor.setTool (toolToAdd); }),
                        .isEnabled =
                            [getActiveEditor, toolToAdd]
                        {
                            auto* editor = getActiveEditor();
                            return editor != nullptr && editor->canUseTool (toolToAdd);
                        },
                        .isTicked = ticked ([toolToAdd] (const TimelineEditor& editor) { return editor.getTool() == toolToAdd; }) });
    }

    // Nudging and quantizing act on what is selected in the editor: clips or a time range in the playlist,
    // notes in the piano roll.
    const auto getUndoTarget = [getActiveEditor]() -> te::Edit*
    {
        auto* editor = getActiveEditor();
        return editor != nullptr ? editor->getEdit() : nullptr;
    };

    const auto canNudge = ticked ([] (const TimelineEditor& editor) { return editor.canNudgeSelection(); });

    registry.add ({ .id = "edit.nudgeLeft",
                    .name = "Nudge Left",
                    .category = "Edit",
                    .description = "Moves the selected clips, notes or time range a step of the grid earlier.",
                    .synonyms = { "move left", "earlier", "shift" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.nudgeSelection (-1); }),
                    .isEnabled = canNudge,
                    .undoTarget = getUndoTarget });

    registry.add ({ .id = "edit.nudgeRight",
                    .name = "Nudge Right",
                    .category = "Edit",
                    .description = "Moves the selected clips, notes or time range a step of the grid later.",
                    .synonyms = { "move right", "later", "shift" },
                    .perform = withEditor ([] (TimelineEditor& editor) { editor.nudgeSelection (1); }),
                    .isEnabled = canNudge,
                    .undoTarget = getUndoTarget });

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
