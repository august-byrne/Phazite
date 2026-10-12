// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Playlist.h"
#include "LookAndFeel.h"
#include "model/Project.h"
#include "model/Tracks.h"

namespace phazite::ui
{
namespace
{
    constexpr int toolbarHeight = 36;
    constexpr int rulerHeight = 28;
    constexpr int headerWidth = 200;
    constexpr int scrollBarSize = 12;
    constexpr int playheadWidth = 11; // the marker in the ruler is this wide; the line runs down its middle

    constexpr double wheelNotch = 60.0 / 256.0; // one notch of a mouse wheel, as JUCE reports it on Windows
    constexpr double zoomPerNotch = 1.25;
    constexpr double scrollPerNotch = 48.0; // pixels
    constexpr double zoomStep = 1.5;        // for the zoom commands
    constexpr double minLineSpacing = 6.0;  // grid lines closer than this many pixels aren't drawn

    bool isTrackState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TRACK) || state.hasType (te::IDs::FOLDERTRACK);
    }

    bool isTempoState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TEMPO) || state.hasType (te::IDs::TIMESIG);
    }

    // Where a track goes to be the playlist's first: after Tracktion's global tracks (tempo, markers...).
    te::TrackInsertPoint getFirstInsertPoint (te::Edit& edit)
    {
        te::Track* lastGlobalTrack = nullptr;

        edit.getTrackList().visitAllTopLevel (
            [&lastGlobalTrack] (te::Track& track)
            {
                if (track.isAudioTrack() || track.isFolderTrack())
                    return false;

                lastGlobalTrack = &track;
                return true;
            });

        return { nullptr, lastGlobalTrack };
    }

    std::unique_ptr<juce::Drawable> createSwatch (juce::Colour colour)
    {
        auto swatch = std::make_unique<juce::DrawableRectangle>();
        swatch->setRectangle (juce::Parallelogram<float> (juce::Rectangle<float> (16.0f, 16.0f)));
        swatch->setCornerSize ({ 3.0f, 3.0f });
        swatch->setFill (colour);
        return swatch;
    }

    // A tooltip with the command's first shortcut, if it has one.
    juce::String withShortcut (const CommandRegistry& commands, const juce::String& text, const juce::String& commandID)
    {
        const auto keys = commands.getShortcutDescriptions (commandID);
        return keys.isEmpty() ? text : text + " (" + keys[0] + ")";
    }

    void addCommandItems (juce::PopupMenu& menu, CommandRegistry& commands, std::initializer_list<const char*> ids)
    {
        for (const auto* id : ids)
            if (const auto commandID = commands.getCommandID (id); commandID != 0)
                menu.addCommandItem (&commands.getCommandManager(), commandID);
    }
} // namespace

//==============================================================================
// The bar numbers above the tracks. Clicking or dragging there moves the playhead.
struct Playlist::Ruler : public juce::Component
{
    explicit Ruler (Playlist& p)
        : playlist (p)
    {
        setTitle ("Timeline");
        setDescription ("Click or drag to move the playhead. It snaps to the grid; hold Alt to place it freely. "
                        "Roll the mouse wheel to zoom.");
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        g.fillAll (p.surfaceRaised);
        g.setColour (p.border);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        auto* shownEdit = playlist.edit.get();

        if (shownEdit == nullptr)
            return;

        const auto& tempo = shownEdit->tempoSequence;
        const auto& timeline = playlist.view;
        const auto clip = g.getClipBounds();
        const te::BeatRange range (timeline.xToBeat (clip.getX() - 1), timeline.xToBeat (clip.getRight() + 1));

        // Ticks: tall for bars, shorter for beats, shortest between them.
        const auto tickType = getAdaptiveSnapType (tempo, range.getStart(), timeline.pixelsPerBeat, false, minLineSpacing);

        forEachGridLine (tempo, tickType, false, range,
                         [&] (te::BeatPosition beat, GridLineType type)
                         {
                             const auto isBar = type == GridLineType::bar;
                             const auto tick = isBar ? getHeight() / 2 : (type == GridLineType::beat ? getHeight() / 4 : getHeight() / 8);
                             g.setColour (isBar ? p.textMuted : p.textMuted.withAlpha (0.4f));
                             g.fillRect (juce::roundToInt (timeline.beatToX (beat)), getHeight() - 1 - tick, 1, tick);
                         });

        // Bar numbers, as far apart as they need to be to read: every bar, or every 2nd, 4th, ...
        const auto barWidth = timeline.pixelsPerBeat * tempo.getTimeSigAt (range.getStart()).numerator.get();
        auto every = 1;

        while (every * barWidth < 32.0 && every < (1 << 20))
            every *= 2;

        // From a little before the area being drawn, so a number its edge cuts is drawn whole.
        auto bar = std::max (0, tempo.toBarsAndBeats (tempo.toTime (timeline.xToBeat (clip.getX() - 48))).bars);
        bar -= bar % every;

        g.setColour (p.textMuted);
        g.setFont (LookAndFeel::getNumberFont (TextSize::small));

        for (;; bar += every)
        {
            const auto x = (float) timeline.beatToX (tempo.toBeats (te::tempo::BarsAndBeats { bar, {}, 0 }));

            if (x > (float) clip.getRight())
                break;

            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 4.0f, 2.0f, 48.0f, (float) getHeight() / 2.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override { movePlayhead (e); }
    void mouseDrag (const juce::MouseEvent& e) override { movePlayhead (e); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        playlist.zoomAround (e.position.x, std::pow (zoomPerNotch, wheel.deltaY / wheelNotch));
    }

    void movePlayhead (const juce::MouseEvent& e)
    {
        playlist.movePlayheadTo (playlist.snap (playlist.view.xToBeat (std::max (0.0f, e.position.x)), e.mods));
    }

    Playlist& playlist;
};

//==============================================================================
// The tracks' rows, with the grid. Clips come in the next part of roadmap step 1.1.
struct Playlist::Lanes : public juce::Component
{
    explicit Lanes (Playlist& p)
        : playlist (p)
    {
        setTitle ("Tracks");
        setDescription ("Click a track to select it. Mouse wheel: scroll; Shift+wheel: scroll sideways; Ctrl+wheel: zoom; "
                        "Alt+wheel: track height.");
        setWantsKeyboardFocus (true); // a click here takes the keyboard back from text fields and sliders
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto clip = g.getClipBounds();
        g.fillAll (p.background);

        auto* shownEdit = playlist.edit.get();

        if (shownEdit == nullptr)
            return;

        for (const auto& row : playlist.rows)
        {
            const auto top = row.top - playlist.scrollY;

            if (top >= clip.getBottom())
                break;

            if (auto* track = row.track.get(); track != nullptr && top + row.height > clip.getY() && playlist.selection.isSelected (track))
            {
                g.setColour (p.accent.withAlpha (0.08f));
                g.fillRect (clip.getX(), top, clip.getWidth(), row.height);
            }
        }

        // The grid, as fine as the snapping, unless its lines would be too close together to see.
        const auto& tempo = shownEdit->tempoSequence;
        const auto& timeline = playlist.view;
        const auto triplets = playlist.grid.triplets;
        const te::BeatRange range (timeline.xToBeat (clip.getX() - 1), timeline.xToBeat (clip.getRight() + 1));
        auto type = playlist.getSnapType();

        if (getGridStepInBeats (type, tempo, range.getStart(), triplets) * timeline.pixelsPerBeat < minLineSpacing)
            type = getAdaptiveSnapType (tempo, range.getStart(), timeline.pixelsPerBeat, triplets, minLineSpacing);

        const auto barColour = p.border.interpolatedWith (p.textMuted, 0.3f);
        const auto subdivisionColour = p.border.withMultipliedAlpha (0.5f);

        forEachGridLine (tempo, type, triplets, range,
                         [&] (te::BeatPosition beat, GridLineType lineType)
                         {
                             g.setColour (lineType == GridLineType::bar ? barColour
                                                                        : (lineType == GridLineType::beat ? p.border : subdivisionColour));
                             g.fillRect (juce::roundToInt (timeline.beatToX (beat)), clip.getY(), 1, clip.getHeight());
                         });

        // A line under each track.
        g.setColour (p.border);

        for (const auto& row : playlist.rows)
            if (const auto bottom = row.top + row.height - 1 - playlist.scrollY; bottom >= clip.getY() && bottom < clip.getBottom())
                g.fillRect (clip.getX(), bottom, clip.getWidth(), 1);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto row = playlist.getRowAt (e.y);
        auto* track = row >= 0 ? playlist.rows[(size_t) row].track.get() : nullptr;

        if (track == nullptr)
        {
            playlist.selection.deselectAll();
            return;
        }

        if (! e.mods.isPopupMenu())
        {
            playlist.trackClicked (*track, e.mods);
            return;
        }

        if (! playlist.selection.isSelected (track))
            playlist.trackClicked (*track, {});

        playlist.showTrackMenu (*track);
    }

    Playlist& playlist;
};

//==============================================================================
// The playhead: a line over the tracks, with a marker in the ruler. Clicks go through it.
struct Playlist::Playhead : public juce::Component
{
    Playhead() { setInterceptsMouseClicks (false, false); }

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
Playlist::Playlist (CommandRegistry& registry, te::SelectionManager& selectionManager)
    : commands (registry),
      selection (selectionManager),
      ruler (std::make_unique<Ruler> (*this)),
      lanes (std::make_unique<Lanes> (*this)),
      playhead (std::make_unique<Playhead>())
{
    setTitle ("Playlist");

    addTrackButton.setButtonText ("Track");
    addTrackButton.onClick = [this] { commands.invoke ("track.add"); };
    snapButton.onClick = [this] { commands.invoke ("grid.snap"); };
    gridButton.setTitle ("Grid");
    gridButton.onClick = [this] { showGridMenu(); };

    headerColumn.setTitle ("Track headers");
    headerColumn.addAndMakeVisible (headerContent);

    for (auto* scrollBar : { &verticalScrollBar, &horizontalScrollBar })
    {
        scrollBar->addListener (this);
        scrollBar->setAutoHide (false);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &snapButton, &gridButton, ruler.get(), &headerColumn,
                                                             lanes.get(), &verticalScrollBar, &horizontalScrollBar, playhead.get() })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &snapButton, &gridButton })
        c->setEnabled (false); // until a project is open

    selection.addChangeListener (this);
    updateGridControls();
}

Playlist::~Playlist()
{
    selection.removeChangeListener (this);
    editState.removeListener (this);
    headers.clear(); // before headerContent, which holds them
}

void Playlist::setEdit (te::Edit* newEdit)
{
    // The same project again (after a save, say) keeps the view as it is.
    if (newEdit == edit.get() && editState.isValid() == (newEdit != nullptr))
        return;

    editState.removeListener (this);
    edit = newEdit;
    editState = newEdit != nullptr ? newEdit->state : juce::ValueTree();
    editState.addListener (this);

    view = {};
    scrollY = 0;
    dropRow = -1;
    selectionAnchor = nullptr;
    grid = newEdit != nullptr ? GridSettings::load (findPhaziteState (*newEdit)) : GridSettings();
    headers.clear();
    rows.clear();
    cancelPendingUpdate();

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &snapButton, &gridButton })
        c->setEnabled (newEdit != nullptr);

    rebuild();
    updateGridControls();
    setViewStart ({});

    if (newEdit != nullptr)
        startTimerHz (5);
    else
        stopTimer();
}

//==============================================================================
void Playlist::rebuild()
{
    auto* e = edit.get();
    const auto tracks = e != nullptr ? getPlaylistTracks (*e) : juce::Array<te::Track*>();

    // The headers of tracks that are still there are kept, in the tracks' new order.
    juce::OwnedArray<TrackHeader> newHeaders;

    for (auto* track : tracks)
    {
        auto* header = getHeaderFor (track);

        if (header != nullptr)
        {
            headers.removeObject (header, false);
        }
        else
        {
            header = new TrackHeader (*this, *track);
            headerContent.addAndMakeVisible (header);
        }

        newHeaders.add (header);
    }

    headers.swapWith (newHeaders); // newHeaders now holds the deleted tracks' headers, and deletes them

    rows.clear();

    for (int i = 0; i < tracks.size(); ++i)
        rows.push_back ({ tracks[i], 0, 0, getTrackColour (*tracks[i], i) });

    layOutRows();
    updateHeaders();
    ruler->repaint();
}

void Playlist::layOutRows()
{
    auto top = 0;

    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto& row = rows[i];
        row.top = top;

        if (auto* track = row.track.get())
            row.height = getTrackHeight (*track);

        headers[(int) i]->setBounds (0, top, headerWidth, row.height);
        top += row.height;
    }

    contentHeight = top;
    headerContent.setSize (headerWidth, std::max (contentHeight, 1));
    setScrollY (scrollY); // keeps it in range, and moves the headers with it
}

void Playlist::updateHeaders()
{
    for (int i = 0; i < headers.size(); ++i)
        if (auto* track = rows[(size_t) i].track.get())
            headers[i]->update (rows[(size_t) i].colour, selection.isSelected (track));
}

int Playlist::getRowAt (int y) const
{
    const auto contentY = y + scrollY;

    for (size_t i = 0; i < rows.size(); ++i)
        if (contentY >= rows[i].top && contentY < rows[i].top + rows[i].height)
            return (int) i;

    return -1;
}

int Playlist::getRowOf (const te::Track* track) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (track != nullptr && rows[i].track.get() == track)
            return (int) i;

    return -1;
}

TrackHeader* Playlist::getHeaderFor (const te::Track* track) const
{
    for (auto* header : headers)
        if (track != nullptr && header->getTrack() == track)
            return header;

    return nullptr;
}

void Playlist::setScrollY (int newScrollY)
{
    // There is room for one more track under the last, to scroll to.
    const auto maxScroll = std::max (0, contentHeight + TrackHeight::standard - lanes->getHeight());
    scrollY = juce::jlimit (0, maxScroll, newScrollY);
    headerContent.setTopLeftPosition (0, -scrollY);
    lanes->repaint();
    updateScrollBars();
}

void Playlist::scrollToShow (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;

    const auto& r = rows[(size_t) row];

    if (r.top < scrollY)
        setScrollY (r.top);
    else if (r.top + r.height > scrollY + lanes->getHeight())
        setScrollY (r.top + r.height - lanes->getHeight());
}

void Playlist::updateScrollBars()
{
    // Sideways, the song plus some room after it; up and down, the tracks.
    const auto visibleBeats = std::max (1, lanes->getWidth()) / view.pixelsPerBeat;
    songEnd = getSongEndInBeats();
    const auto total = std::max ({ songEnd + 64.0, view.start.inBeats() + visibleBeats, 256.0 });
    horizontalScrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    horizontalScrollBar.setCurrentRange (view.start.inBeats(), visibleBeats, juce::dontSendNotification);
    horizontalScrollBar.setSingleStepSize (scrollPerNotch / view.pixelsPerBeat);

    const auto visibleHeight = std::max (1, lanes->getHeight());
    verticalScrollBar.setRangeLimits (0.0, std::max (contentHeight + TrackHeight::standard, visibleHeight), juce::dontSendNotification);
    verticalScrollBar.setCurrentRange (scrollY, visibleHeight, juce::dontSendNotification);
    verticalScrollBar.setSingleStepSize (scrollPerNotch);
}

//==============================================================================
te::TimecodeSnapType Playlist::getSnapType() const
{
    if (auto* e = edit.get())
        return getGridSnapType (grid, e->tempoSequence, view.start, view.pixelsPerBeat);

    return te::TimecodeSnapType::get1BeatSnapType();
}

te::BeatPosition Playlist::snap (te::BeatPosition beat, juce::ModifierKeys mods) const
{
    auto* e = edit.get();

    if (e == nullptr || ! grid.snap || mods.isAltDown())
        return beat;

    return snapToGrid (beat, getSnapType(), e->tempoSequence, grid.triplets);
}

double Playlist::getSongEndInBeats() const
{
    if (auto* e = edit.get())
        return e->tempoSequence.toBeats (te::toPosition (e->getLength())).inBeats();

    return 0.0;
}

void Playlist::setViewStart (te::BeatPosition start)
{
    view.start = te::BeatPosition::fromBeats (std::max (0.0, start.inBeats()));
    ruler->repaint();
    lanes->repaint();
    updatePlayhead();
    updateScrollBars();
}

void Playlist::zoomAround (double x, double factor)
{
    view.zoomAround (x, factor);
    setViewStart (view.start);
    updateGridControls(); // an adaptive grid's size changes with the zoom
}

void Playlist::zoom (double factor)
{
    auto x = lanes->getWidth() / 2.0;

    if (auto* e = edit.get())
        if (const auto playheadX = view.beatToX (e->tempoSequence.toBeats (e->getTransport().getPosition()));
            playheadX >= 0.0 && playheadX <= lanes->getWidth())
            x = playheadX;

    zoomAround (x, factor);
}

void Playlist::zoomToFit()
{
    const auto beats = std::max (getSongEndInBeats(), 16.0);
    view.pixelsPerBeat =
        juce::jlimit (TimelineView::minPixelsPerBeat, TimelineView::maxPixelsPerBeat, std::max (1, lanes->getWidth() - Space::l) / beats);
    setViewStart ({});
    updateGridControls();
}

void Playlist::centreOnPlayhead()
{
    if (auto* e = edit.get())
        setViewStart (e->tempoSequence.toBeats (e->getTransport().getPosition())
                      - te::BeatDuration::fromBeats (lanes->getWidth() / 2.0 / view.pixelsPerBeat));
}

void Playlist::movePlayheadTo (te::BeatPosition beat)
{
    if (auto* e = edit.get())
        e->getTransport().setPosition (e->tempoSequence.toTime (beat));

    updatePlayhead();
}

void Playlist::updatePlayhead()
{
    auto* e = edit.get();

    if (e == nullptr)
    {
        playhead->setVisible (false);
        return;
    }

    const auto x = view.beatToX (e->tempoSequence.toBeats (e->getTransport().getPosition()));
    const auto isInView = x >= -1.0 && x <= lanes->getWidth() + 1.0;
    playhead->setVisible (isInView);

    if (isInView)
        playhead->setBounds (lanes->getX() + juce::roundToInt (x) - playheadWidth / 2, ruler->getY(), playheadWidth,
                             ruler->getHeight() + lanes->getHeight());
}

//==============================================================================
void Playlist::setGrid (const GridSettings& newGrid)
{
    grid = newGrid;

    if (auto* e = edit.get())
    {
        auto state = getPhaziteState (*e);
        grid.save (state);
    }

    updateGridControls();
    lanes->repaint();
}

void Playlist::updateGridControls()
{
    snapButton.setToggleState (grid.snap, juce::dontSendNotification);

    juce::String text ("Grid: ");

    if (auto* e = edit.get())
        text << getGridDescription (getSnapType(), e->tempoSequence, grid.triplets);

    gridButton.setButtonText (grid.adaptive ? text + " (auto)" : text);
}

void Playlist::showGridMenu()
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
                        [safeThis = juce::Component::SafePointer<Playlist> (this)] (int result)
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

void Playlist::setColourOfSelectedTracks (juce::Colour colour)
{
    const auto tracks = getSelectedTracks (selection);

    if (tracks.isEmpty())
        return;

    tracks.getFirst()->edit.getUndoManager().beginNewTransaction (tracks.size() > 1 ? "Change Track Colors" : "Change Track Color");

    for (auto* track : tracks)
        track->setColour (colour);
}

//==============================================================================
void Playlist::trackClicked (te::Track& track, const juce::ModifierKeys& mods)
{
    // The arrow keys then step through the tracks, rather than move a slider that had the focus.
    lanes->grabKeyboardFocus();

    // Shift selects the tracks from the last one clicked to this one.
    if (const auto anchorRow = getRowOf (selectionAnchor.get()); mods.isShiftDown() && anchorRow >= 0)
    {
        const auto row = getRowOf (&track);
        selection.deselectAll();

        for (auto i = std::min (anchorRow, row); i <= std::max (anchorRow, row); ++i)
            if (auto* t = rows[(size_t) i].track.get())
                selection.addToSelection (t);

        return;
    }

    if (! mods.isCommandDown())
        selection.selectOnly (track);
    else if (selection.isSelected (track))
        selection.deselect (&track);
    else
        selection.addToSelection (track);

    selectionAnchor = &track;
}

void Playlist::showTrackMenu (te::Track&)
{
    juce::PopupMenu menu;
    addCommandItems (menu, commands, { "edit.rename", "track.color" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "track.mute", "track.solo" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "track.moveUp", "track.moveDown" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "edit.duplicate", "edit.delete" });
    menu.addSeparator();
    addCommandItems (menu, commands, { "track.add" });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void Playlist::showColourMenu (te::Track& track, juce::Component& target)
{
    const auto current = getTrackColour (track, getRowOf (&track));
    const auto& colours = getTrackColours();
    juce::PopupMenu menu;

    for (size_t i = 0; i < colours.size(); ++i)
    {
        juce::PopupMenu::Item item (colours[i].name);
        item.itemID = (int) i + 1;
        item.isTicked = colours[i].colour == current;
        item.image = createSwatch (colours[i].colour);
        menu.addItem (std::move (item));
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target),
                        [safeThis = juce::Component::SafePointer<Playlist> (this)] (int result)
                        {
                            if (safeThis != nullptr && juce::isPositiveAndNotGreaterThan (result, (int) getTrackColours().size()))
                                safeThis->setColourOfSelectedTracks (getTrackColours()[(size_t) result - 1].colour);
                        });
}

void Playlist::trackDragged (TrackHeader&, const juce::MouseEvent& e)
{
    // Scroll when dragging past the top or bottom.
    const auto y = e.getEventRelativeTo (lanes.get()).getPosition().y;

    if (y < 0)
        setScrollY (scrollY + y / 4);
    else if (y > lanes->getHeight())
        setScrollY (scrollY + (y - lanes->getHeight()) / 4);

    // The track would go to the nearest boundary between rows.
    const auto contentY = y + scrollY;
    const auto boundary = [this] (size_t i) { return i < rows.size() ? rows[i].top : contentHeight; };
    size_t nearest = 0;

    for (size_t i = 1; i <= rows.size(); ++i)
        if (std::abs (contentY - boundary (i)) < std::abs (contentY - boundary (nearest)))
            nearest = i;

    dropRow = (int) nearest;
    repaint();
}

void Playlist::trackDragEnded (TrackHeader&, const juce::MouseEvent&)
{
    const auto target = dropRow;
    dropRow = -1;
    repaint();

    auto* e = edit.get();
    const auto moving = getSelectedTracks (selection);

    if (e == nullptr || target < 0 || moving.isEmpty())
        return;

    // The tracks go, in order, after the nearest track above the drop point that isn't moving.
    te::Track* previous = nullptr;

    for (auto i = target - 1; i >= 0 && previous == nullptr; --i)
        if (auto* t = rows[(size_t) i].track.get(); t != nullptr && ! selection.isSelected (t))
            previous = t;

    e->getUndoManager().beginNewTransaction (moving.size() > 1 ? "Move Tracks" : "Move Track");

    for (auto* track : moving)
    {
        e->moveTrack (track, previous != nullptr ? getInsertPointAfter (*previous) : getFirstInsertPoint (*e));
        previous = track;
    }
}

//==============================================================================
void Playlist::addCommands (CommandRegistry& registry)
{
    const auto hasEdit = [this] { return edit.get() != nullptr; };
    const auto hasSelectedTracks = [this] { return ! getSelectedTracks (selection).isEmpty(); };

    registry.add ({ .id = "edit.rename",
                    .name = "Rename",
                    .category = "Edit",
                    .description = "Renames the selected track.",
                    .synonyms = { "name", "label", "title" },
                    .perform =
                        [this]
                    {
                        handleUpdateNowIfNeeded(); // a track added a moment ago has its header now

                        if (auto* track = getSelectedTracks (selection).getFirst())
                        {
                            scrollToShow (getRowOf (track));

                            if (auto* header = getHeaderFor (track))
                                header->startRenaming();
                        }
                    },
                    .isEnabled = hasSelectedTracks,
                    .currentName = [hasSelectedTracks] { return hasSelectedTracks() ? juce::String ("Rename Track") : juce::String(); } });

    registry.add ({ .id = "track.color",
                    .name = "Track Color",
                    .category = "Track",
                    .description = "Changes the color of the selected tracks.",
                    .synonyms = { "colour", "recolor", "paint" },
                    .perform =
                        [this]
                    {
                        handleUpdateNowIfNeeded();

                        if (auto* track = getSelectedTracks (selection).getFirst())
                        {
                            scrollToShow (getRowOf (track));

                            if (auto* header = getHeaderFor (track))
                                showColourMenu (*track, *header);
                        }
                    },
                    .isEnabled = hasSelectedTracks });

    registry.add ({ .id = "view.zoomIn",
                    .name = "Zoom In",
                    .category = "View",
                    .description = "Shows less of the song, in more detail.",
                    .synonyms = { "magnify", "closer", "horizontal zoom" },
                    .perform = [this] { zoom (zoomStep); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.zoomOut",
                    .name = "Zoom Out",
                    .category = "View",
                    .description = "Shows more of the song, in less detail.",
                    .synonyms = { "further", "horizontal zoom" },
                    .perform = [this] { zoom (1.0 / zoomStep); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.zoomToFit",
                    .name = "Zoom to Fit",
                    .category = "View",
                    .description = "Shows the whole song.",
                    .synonyms = { "show all", "overview", "whole song" },
                    .perform = [this] { zoomToFit(); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.centreOnPlayhead",
                    .name = "Center on Playhead",
                    .category = "View",
                    .description = "Scrolls the playlist so that the playhead is in the middle.",
                    .synonyms = { "find playhead", "scroll to playhead", "where am i" },
                    .perform = [this] { centreOnPlayhead(); },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "view.followPlayback",
                    .name = "Follow Playback",
                    .category = "View",
                    .description = "While playing, turns the page when the playhead reaches the right edge.",
                    .synonyms = { "auto scroll", "scroll", "page" },
                    .perform = [this] { followPlayback = ! followPlayback; },
                    .isTicked = [this] { return followPlayback; } });

    registry.add ({ .id = "grid.snap",
                    .name = "Snap to Grid",
                    .category = "Grid",
                    .description = "Lines up what you move or place with the grid. Hold Alt to place things freely.",
                    .synonyms = { "magnet", "quantize", "align" },
                    .perform =
                        [this]
                    {
                        auto newGrid = grid;
                        newGrid.snap = ! newGrid.snap;
                        setGrid (newGrid);
                    },
                    .isEnabled = hasEdit,
                    .isTicked = [this] { return grid.snap; } });

    registry.add ({ .id = "grid.adaptive",
                    .name = "Adaptive Grid",
                    .category = "Grid",
                    .description = "Makes the grid finer as you zoom in and coarser as you zoom out, or keeps it one size.",
                    .synonyms = { "zoom adaptive", "fixed grid", "auto" },
                    .perform =
                        [this]
                    {
                        auto newGrid = grid;

                        // A fixed grid starts at the size the adaptive one had.
                        if (newGrid.adaptive)
                            newGrid.fixedLevel =
                                juce::jlimit (GridSettings::minFixedLevel, GridSettings::maxFixedLevel, getSnapType().getLevel());

                        newGrid.adaptive = ! newGrid.adaptive;
                        setGrid (newGrid);
                    },
                    .isEnabled = hasEdit,
                    .isTicked = [this] { return grid.adaptive; } });

    registry.add ({ .id = "grid.finer",
                    .name = "Finer Grid",
                    .category = "Grid",
                    .description = "Makes the grid's steps smaller.",
                    .synonyms = { "narrow grid", "smaller grid", "more lines" },
                    .perform =
                        [this]
                    {
                        auto newGrid = grid;
                        newGrid.makeFiner();
                        setGrid (newGrid);
                    },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "grid.coarser",
                    .name = "Coarser Grid",
                    .category = "Grid",
                    .description = "Makes the grid's steps larger.",
                    .synonyms = { "widen grid", "larger grid", "fewer lines" },
                    .perform =
                        [this]
                    {
                        auto newGrid = grid;
                        newGrid.makeCoarser();
                        setGrid (newGrid);
                    },
                    .isEnabled = hasEdit });

    registry.add ({ .id = "grid.triplets",
                    .name = "Triplet Grid",
                    .category = "Grid",
                    .description = "Divides beats into three instead of two.",
                    .synonyms = { "triplets", "swing", "thirds" },
                    .perform =
                        [this]
                    {
                        auto newGrid = grid;
                        newGrid.triplets = ! newGrid.triplets;
                        setGrid (newGrid);
                    },
                    .isEnabled = hasEdit,
                    .isTicked = [this] { return grid.triplets; } });

    refreshShortcutTexts();
}

void Playlist::refreshShortcutTexts()
{
    addTrackButton.setTooltip (withShortcut (commands, "Adds an empty track below the selected one", "track.add"));
    snapButton.setTooltip (
        withShortcut (commands, "Snap to grid: what you move or place lines up with the grid. Hold Alt to place freely", "grid.snap"));
    gridButton.setTooltip ("The grid's size. An adaptive grid gets finer as you zoom in");
}

//==============================================================================
void Playlist::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (0, toolbarHeight - 1, getWidth(), 1);
}

void Playlist::paintOverChildren (juce::Graphics& g)
{
    if (dropRow < 0)
        return;

    // A line where the dragged track would go.
    const auto contentY = dropRow < (int) rows.size() ? rows[(size_t) dropRow].top : contentHeight;
    g.setColour (getPalette (*this).accent);
    g.fillRect (headerColumn.getX(), lanes->getY() + contentY - scrollY - 1, lanes->getRight() - headerColumn.getX(), 3);
}

void Playlist::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs);
    addTrackButton.setBounds (toolbar.removeFromLeft (96));
    toolbar.removeFromLeft (Space::s);
    snapButton.setBounds (toolbar.removeFromLeft (controlHeight + Space::s));
    toolbar.removeFromLeft (Space::xs);
    gridButton.setBounds (toolbar.removeFromLeft (180));

    horizontalScrollBar.setBounds (area.removeFromBottom (scrollBarSize).withTrimmedLeft (headerWidth).withTrimmedRight (scrollBarSize));
    auto rulerArea = area.removeFromTop (rulerHeight);
    verticalScrollBar.setBounds (area.removeFromRight (scrollBarSize));
    headerColumn.setBounds (area.removeFromLeft (headerWidth));
    ruler->setBounds (rulerArea.withTrimmedLeft (headerWidth).withTrimmedRight (scrollBarSize));
    lanes->setBounds (area);

    layOutRows();
    setViewStart (view.start);
}

void Playlist::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto notches = [] (float delta) { return delta / wheelNotch; };

    // Ctrl+wheel zooms around the pointer, as in FL Studio.
    if (e.mods.isCommandDown())
    {
        zoomAround (e.getEventRelativeTo (lanes.get()).position.x, std::pow (zoomPerNotch, notches (wheel.deltaY)));
        return;
    }

    // Alt+wheel makes every track taller or shorter.
    if (e.mods.isAltDown())
    {
        if (auto* ed = edit.get())
        {
            const auto change = juce::roundToInt (notches (wheel.deltaY) * TrackHeight::step / 2);

            for (auto* track : getPlaylistTracks (*ed))
                setTrackHeight (*track, getTrackHeight (*track) + change);

            layOutRows();
        }

        return;
    }

    const auto sideways = e.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX;
    const auto upDown = e.mods.isShiftDown() ? 0.0f : wheel.deltaY;

    if (sideways != 0.0f)
        setViewStart (view.xToBeat (-notches (sideways) * scrollPerNotch));

    if (upDown != 0.0f)
        setScrollY (scrollY - juce::roundToInt (notches (upDown) * scrollPerNotch));
}

//==============================================================================
void Playlist::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateHeaders();
    lanes->repaint();

    if (auto* track = selection.getFirstItemOfType<te::Track>())
        scrollToShow (getRowOf (track));
}

void Playlist::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (isTrackState (tree))
    {
        triggerAsyncUpdate(); // a name, color, height, mute or solo
    }
    else if (isTempoState (tree))
    {
        ruler->repaint();
        lanes->repaint();
    }
}

void Playlist::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (isTrackState (child))
        triggerAsyncUpdate();
}

void Playlist::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (isTrackState (child))
        triggerAsyncUpdate();
}

void Playlist::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == editState || isTrackState (parent))
        triggerAsyncUpdate();
}

void Playlist::scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &horizontalScrollBar)
        setViewStart (te::BeatPosition::fromBeats (newRangeStart));
    else
        setScrollY (juce::roundToInt (newRangeStart));
}

void Playlist::timerCallback()
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
        const auto beat = e->tempoSequence.toBeats (e->getTransport().getPosition());

        if (const auto x = view.beatToX (beat); x < 0.0 || x > lanes->getWidth() - 4.0)
            setViewStart (beat - te::BeatDuration::fromBeats (lanes->getWidth() * 0.05 / view.pixelsPerBeat));
    }

    if (std::abs (getSongEndInBeats() - songEnd) > 1.0e-6)
        updateScrollBars();

    updatePlayhead();
}
} // namespace phazite::ui
